/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/detect.c - unit test of the game detection (src/core/detect.c,
 *                  declared in src/include/bgi/detect.h); built as
 *                  test_detect by `make test`
 *
 * The programs are assembled from listings (asm.h), so every case is a
 * small game made up for the test.  The groups, in the order they run:
 *
 *   set      - collecting programs: only program files are taken, the
 *              boot program goes first, a DSC-compressed one is decoded
 *   product  - the literal "80 E8" is compared with: found in the boot
 *              program, deciding when the table carries it with one
 *              profile, narrowing the other steps when it carries it
 *              with several, kept as the product name when unknown
 *   hash     - the hash does not depend on the order of the programs or
 *              their compression, and an entry with it decides
 *   opcodes  - the closest build: the oldest that has every opcode the
 *              programs use, an argument only later builds accept, the
 *              1.667+ encodings
 */
#include "bgi/detect.h"
#include "bgi/core.h"
#include "bgi/asm.h"
#include "bgi/codec.h"
#include "host/os.h"

#include <stdio.h>

#include "check.h"

static char gPath[0x400]; // the scratch openbgi.json

// assemble a listing for `gen` and add the program to the set under `name`
static int AddListing(OpenBGI_t* o, DetectSet_t* s, const char* name, const char* text, EngineGen_t gen, int isIpl)
{
	AsmResult_t r;
	int ok;
	Asm_Assemble(o, text, strlen(text), gen, stderr, &r);
	if(!r.file)
		return 0;
	ok = DetectSet_Add(o, s, name, r.file, r.size, isIpl);
	BGI_Free(r.file);
	BGI_Free(r.labels);
	return ok;
}

// a boot program that checks for the product name `product` and quits otherwise
static void ProductIpl(char* out, size_t n, const char* product)
{
	snprintf(out, n,
		"\tpush_local_addr 0x10\n"
		"\tsys.get_product_name\n"
		"\tpush_local_addr 0x10\n"
		"\tpush_code_addr name\n"
		"\tstreq\n"
		"\tpush_code_off bad\n"
		"\tjcc 1\n"
		"\tret\n"
		"bad:\n"
		"\tsys.quit\n"
		"name:\n"
		"\t.str \"%s\"\n",
		product);
}

static void WriteTable(OpenBGI_t* o, const char* text)
{
	CHECK(OS_WriteNativeFile(gPath, text, (uint32_t)strlen(text)));
	Engine_ResetGames(o);
	CHECK(Engine_LoadGamesJson(o, gPath) > 0);
}

static void TestSet(OpenBGI_t* o)
{
	DetectSet_t s;
	static const uint8_t junk[] = "not a program at all";
	DetectSet_Init(&s);
	CHECK(!DetectSet_Add(o, &s, "readme.txt", junk, sizeof junk, 0));
	CHECK(AddListing(o, &s, "other._bp", "\tret\n", GEN_1_69_444, 0));
	CHECK(AddListing(o, &s, "ipl._bp", "\tret\n", GEN_1_69_444, 1));
	CHECK(s.count == 2 && s.haveIpl && strcmp(s.progs[0].name, "ipl._bp") == 0 && strcmp(s.progs[1].name, "other._bp") == 0);
	{ // a DSC-compressed program is decoded first: the same bytes as the plain one
		uint32_t n = 0;
		uint8_t* dsc = BGI_DscEncode(s.progs[1].data, s.progs[1].size, 0x1234, &n);
		CHECK(dsc != NULL && n > 0 && BGI_IsDsc(dsc));
		CHECK(dsc && DetectSet_Add(o, &s, "packed._bp", dsc, n, 0));
		CHECK(s.count == 3 && s.progs[2].size == s.progs[1].size && memcmp(s.progs[2].data, s.progs[1].data, s.progs[1].size) == 0);
		BGI_Free(dsc);
	}
	DetectSet_Free(&s);
	CHECK(s.count == 0 && s.progs == NULL);
}

static void TestProduct(OpenBGI_t* o)
{
	DetectSet_t s;
	DetectResult_t r;
	char text[0x400], found[0x100];
	Engine_ResetGames(o);
	// a product the built-in table carries with one profile decides at once
	DetectSet_Init(&s);
	ProductIpl(text, sizeof text, "SenNoHatouTsukisomeNoKouki");
	CHECK(AddListing(o, &s, "ipl._bp", text, GEN_1_636, 1));
	CHECK(Detect_ProductName(s.progs[0].data, s.progs[0].size, found, sizeof found) && strcmp(found, "SenNoHatouTsukisomeNoKouki") == 0);
	CHECK(Detect_Game(o, &s, NULL, "", &r) == DETECT_PRODUCT && strcmp(r.profile, "1.636") == 0 && r.gen == GEN_1_636 && r.entry >= 0);
	CHECK(strcmp(r.product, "SenNoHatouTsukisomeNoKouki") == 0 && strlen(r.hash) == 16);
	DetectSet_Free(&s);
	// a product the table does not know is kept; the opcodes choose the profile
	DetectSet_Init(&s);
	ProductIpl(text, sizeof text, "SomeOtherGame");
	CHECK(AddListing(o, &s, "ipl._bp", text, GEN_1_69_444, 1));
	CHECK(Detect_Game(o, &s, NULL, "", &r) == DETECT_OPCODES && strcmp(r.product, "SomeOtherGame") == 0 && r.entry == -1);
	CHECK(r.gen == GEN_1_64); // nothing but old instructions: the oldest build that has them all (1.58 lacks "80 E8")
	DetectSet_Free(&s);
	// a product two profiles carry (the trial and the full game): the opcodes choose between those two only
	DetectSet_Init(&s);
	ProductIpl(text, sizeof text, "Tayutama");
	CHECK(AddListing(o, &s, "ipl._bp", text, GEN_1_69_444, 1));
	CHECK(Detect_Game(o, &s, NULL, "", &r) == DETECT_OPCODES && strcmp(r.product, "Tayutama") == 0 && r.gen == GEN_1_69_444);
	CHECK(AddListing(o, &s, "vsync._bp", "\tpush_i8 1\n\tsys.set_vsync_wait\n\tret\n", GEN_1_69_451, 0));
	CHECK(Detect_Game(o, &s, NULL, "", &r) == DETECT_OPCODES && r.gen == GEN_1_69_451 && strcmp(r.profile, "1.69/451") == 0);
	DetectSet_Free(&s);
	// no "80 E8" comparison: no product name
	DetectSet_Init(&s);
	CHECK(AddListing(o, &s, "ipl._bp", "\tpush_local_addr 0x10\n\tsys.get_product_name\n\tret\n", GEN_1_69_444, 1));
	CHECK(!Detect_ProductName(s.progs[0].data, s.progs[0].size, found, sizeof found));
	CHECK(Detect_Game(o, &s, NULL, "", &r) == DETECT_OPCODES && r.product[0] == 0);
	DetectSet_Free(&s);
}

static void TestHash(OpenBGI_t* o)
{
	DetectSet_t a, b;
	DetectResult_t r;
	char ha[17], hb[17], table[0x400];
	// the same programs in another order hash the same; another program does not
	DetectSet_Init(&a);
	DetectSet_Init(&b);
	CHECK(AddListing(o, &a, "ipl._bp", "\tret\n", GEN_1_64, 1));
	CHECK(AddListing(o, &a, "Title._bp", "\tpush_i8 1\n\tret\n", GEN_1_64, 0));
	CHECK(AddListing(o, &a, "menu._bp", "\tpush_i8 2\n\tret\n", GEN_1_64, 0));
	CHECK(AddListing(o, &b, "menu._bp", "\tpush_i8 2\n\tret\n", GEN_1_64, 0));
	CHECK(AddListing(o, &b, "title._bp", "\tpush_i8 1\n\tret\n", GEN_1_64, 0));
	CHECK(AddListing(o, &b, "ipl._bp", "\tret\n", GEN_1_64, 1));
	Detect_Hash(&a, ha);
	Detect_Hash(&b, hb);
	CHECK(strlen(ha) == 16 && strcmp(ha, hb) == 0);
	CHECK(AddListing(o, &b, "extra._bp", "\tret\n", GEN_1_64, 0));
	Detect_Hash(&b, hb);
	CHECK(strcmp(ha, hb) != 0);
	// an entry that records the hash decides, with its profile and product
	snprintf(table, sizeof table, "{ \"games\": [ { \"product\": \"Hashed\", \"profile\": \"1.66\", \"hash\": \"%s\" } ] }\n", ha);
	WriteTable(o, table);
	CHECK(Detect_Game(o, &a, NULL, "", &r) == DETECT_HASH && strcmp(r.profile, "1.66") == 0 && r.entry == 0 && strcmp(r.hash, ha) == 0);
	CHECK(Detect_Game(o, &b, NULL, "", &r) == DETECT_OPCODES);
	DetectSet_Free(&a);
	DetectSet_Free(&b);
	{ // an empty set: nothing to go by
		DetectSet_t e;
		DetectSet_Init(&e);
		Detect_Hash(&e, ha);
		CHECK(ha[0] == 0 && Detect_Game(o, &e, NULL, "", &r) == DETECT_NONE);
	}
	Engine_ResetGames(o);
}

static void TestOpcodes(OpenBGI_t* o)
{
	DetectSet_t s;
	DetectResult_t r;
	EngineGen_t first, last, gen;
	int mismatches, allowed[GEN_COUNT];
	Engine_ResetGames(o);
	// "80 6E" came with 1.69 build 451: the oldest build that has it is the closest; every later one has it too
	DetectSet_Init(&s);
	CHECK(AddListing(o, &s, "ipl._bp", "\tpush_i8 1\n\tsys.set_vsync_wait\n\tret\n", GEN_1_69_451, 1));
	gen = Detect_Opcodes(&s, NULL, &first, &last, &mismatches);
	CHECK(gen == GEN_1_69_451 && first == GEN_1_69_451 && last == GEN_LAST && mismatches == 0);
	CHECK(Detect_Game(o, &s, NULL, "", &r) == DETECT_OPCODES && r.gen == GEN_1_69_451 && r.fitFirst == GEN_1_69_451);
	// the builds allowed decide what is closest
	memset(allowed, 0, sizeof allowed);
	allowed[GEN_1_58] = allowed[GEN_1_64] = 1;
	gen = Detect_Opcodes(&s, allowed, &first, &last, &mismatches);
	CHECK(gen == GEN_1_58 && mismatches == 1); // neither has "80 6E": the closer one has fewer opcodes the program does not use
	DetectSet_Free(&s);
	// an argument only later builds accept: text function 0x80000009 of "91 9A" needs 1.659
	DetectSet_Init(&s);
	CHECK(AddListing(o, &s, "ipl._bp", "\tpush_i32 0x80000009\n\tpush_i8 0\n\tgfx1.set_text_function\n\tret\n", GEN_1_659, 1));
	gen = Detect_Opcodes(&s, NULL, &first, &last, &mismatches);
	CHECK(gen == GEN_1_659 && mismatches == 0);
	DetectSet_Free(&s);
	// the same function number computed rather than pushed is no evidence: the instruction's own build (1.529) is closest
	DetectSet_Init(&s);
	CHECK(AddListing(o, &s, "ipl._bp", "\tpush_i32 0x80000008\n\tpush_i8 1\n\tadd\n\tpush_i8 0\n\tgfx1.set_text_function\n\tret\n", GEN_1_659, 1));
	gen = Detect_Opcodes(&s, NULL, &first, &last, &mismatches);
	CHECK(gen == GEN_1_529);
	DetectSet_Free(&s);
	// the 1.667+ encodings: an inline conditional jump reads as 1.667's
	DetectSet_Init(&s);
	CHECK(AddListing(o, &s, "ipl._bp", "\tpush_i8 1\n\tjcc 9, done\n\tpush_i8 2\ndone:\n\tret\n", GEN_1_667, 1));
	gen = Detect_Opcodes(&s, NULL, &first, &last, &mismatches);
	CHECK(gen == GEN_1_667 && mismatches == 0);
	DetectSet_Free(&s);
}

int main(void)
{
	const char* tmp = getenv("BGI_TEST_TMP");
	OpenBGI_t* o = OpenBGI_New(OS_Host()); // the engine whose code the test runs
	if(!o)
		return 1;
	if(!OS_Init())
		return 1;
	snprintf(gPath, sizeof gPath, "%s/openbgi_test_detect.json", tmp ? tmp : ".");
	printf("set\n");
	TestSet(o);
	printf("product\n");
	TestProduct(o);
	printf("hash\n");
	TestHash(o);
	printf("opcodes\n");
	TestOpcodes(o);
	remove(gPath);
	OS_Shutdown();
	if(gFails)
	{
		printf("%d check(s) failed\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
