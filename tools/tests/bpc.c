/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/bpc.c - unit test of the program decompiler and compiler
 *               (src/bpc/, declared in inc/bgi/bpc.h); built as
 *               bin/test_bpc by `make test`
 *
 * The groups of tests, in the order they run:
 *
 *   shapes     - sources compile to the bytes the original compiler's
 *                shapes give (docs/bpc.md): the frame and the parameters,
 *                the argument orders, the two stores, the condition chains
 *                and the statements, the sign test, the wide literal, the
 *                string table and its padding
 *   round trip - the compiled programs decompile to source that compiles
 *                to the same bytes, and the source names the constructs it
 *                should (if / else / while / do / goto / __sign / __pop)
 *   areas      - the variables declared at an address of a memory area
 *                (global, heap, data, code, dynamic[n]): a name compiles
 *                to what the numeric form of the same access gives, the
 *                pointer tags follow the engine, name[index]
 *   records    - struct, typed locals, parameters and area variables: a.f,
 *                p->f, a[i].f, p[i], &a[i], sizeof, *p and a list assigned
 *                to an element compile to what the address arithmetic
 *                with casts gives; #encoding and the UTF-8 strings
 *   includes   - #include: the search order, a file read once, a message
 *                that names the included file
 *   defines    - #define / #undef: a name is replaced by its text as
 *                written (nothing is folded), also inside the text of
 *                another name and in a declared address, but not in
 *                strings, comments or after the dot of an instruction
 *   errors     - texts the compiler must reject
 *
 * The expected bytes come from listings assembled with Asm_Assemble, so
 * that labels and encodings need not be worked out by hand.
 */
#include "bgi/bpc.h"
#include "bgi/os.h"

#include <stdio.h>

static int gFails; // the number of failed checks so far
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

#define GEN GEN_1_553

// compile a source; NULL (with the messages on stdout) when it does not compile
static uint8_t* Compile(const char* src, uint32_t* size)
{
	BpcResult_t r;
	Bpc_Compile(src, strlen(src), "test", GEN, NULL, stdout, &r);
	BGI_Free(r.labels);
	if(r.errors)
		return NULL;
	*size = r.size;
	return r.file;
}

// assemble a listing; NULL when it does not assemble
static uint8_t* Assemble(const char* listing, uint32_t* size)
{
	AsmResult_t r;
	Asm_Assemble(listing, strlen(listing), GEN, stdout, &r);
	BGI_Free(r.labels);
	if(r.errors)
		return NULL;
	*size = r.size;
	return r.file;
}

// decompile a program into a malloc'ed string
static char* Decompile(const uint8_t* file, uint32_t size, int* blocks)
{
	BpcDecOptions_t opt;
	FILE* mem = tmpfile();
	long n;
	char* text;
	memset(&opt, 0, sizeof opt);
	opt.gen = GEN;
	opt.name = "test";
	*blocks = Bpc_Decompile(file, size, &opt, mem);
	n = ftell(mem);
	fseek(mem, 0, SEEK_SET);
	text = (char*)malloc((size_t)n + 1);
	if(fread(text, 1, (size_t)n, mem) != (size_t)n)
		n = 0;
	text[n] = 0;
	fclose(mem);
	return text;
}

static void Same(const char* what, const uint8_t* a, uint32_t an, const uint8_t* b, uint32_t bn)
{
	if(!a || !b)
	{
		printf("  FAIL %s: not produced\n", what);
		gFails++;
		return;
	}
	if(an != bn || memcmp(a, b, an) != 0)
	{
		uint32_t i;
		for(i = 0; i < an && i < bn && a[i] == b[i]; i++)
			;
		printf("  FAIL %s: %u / %u bytes, first difference at 0x%x\n", what, (unsigned)an, (unsigned)bn, (unsigned)i);
		gFails++;
	}
}

/* compile `src`, check it against the assembled `listing`, then round
 * trip it through the decompiler; returns the decompiled text (malloc'ed)
 * so that the caller can look for its constructs */
static char* Shape(const char* what, const char* src, const char* listing)
{
	uint32_t cn = 0, an = 0, rn = 0;
	uint8_t* c = Compile(src, &cn);
	uint8_t* a = Assemble(listing, &an);
	char* text = NULL;
	Same(what, c, cn, a, an);
	if(c)
	{
		int blocks = -1;
		uint8_t* r;
		text = Decompile(c, cn, &blocks);
		CHECK(blocks == 0);
		r = Compile(text, &rn);
		Same(what, c, cn, r, rn);
		BGI_Free(r);
	}
	BGI_Free(c);
	BGI_Free(a);
	return text;
}

// ---- shapes ----------------------------------------------------------------------------------------

static void TestFrame(void)
{
	char* t = Shape("frame",
		"#engine 1.553\n"
		"main(p1, p2) { int a; char buf[8]; int b = p1;\n"
		"    a = p1 + p2;\n"
		"    *(int*)1208 = a;\n"
		"    return a;\n"
		"}\n",
		".engine 1.553\n"
		"main:\n"
		"\tpush_fp\n\tpush_i8 24\n\tadd\n\tset_fp\n"
		"\tpush_local_addr 0x14\n\tstore 2\n"                                  // p1, the higher of the two parameter slots
		"\tpush_local_addr 0x18\n\tstore 2\n"                                  // p2 at the frame's bottom
		"\tpush_local_addr 0x14\n\tload 2\n\tpush_local_addr 0x4\n\tstore 2\n" // int b = p1: value, address, store
		"\tpush_local_addr 0x10\n\tpush_local_addr 0x14\n\tload 2\n\tpush_local_addr 0x18\n\tload 2\n\tadd\n\tstore_keep 2\n"
		"\tpush_i16 1208\n\tpush_local_addr 0x10\n\tload 2\n\tstore_keep 2\n"
		"\tpush_local_addr 0x10\n\tload 2\n\tpush_code_off EXIT\n\tjmp\n"
		"EXIT:\n\tpush_fp\n\tpush_i8 24\n\tsub\n\tset_fp\n\tret\n"
		"\t.align 16\n");
	if(t)
	{
		CHECK(strstr(t, "main(p1, p2)") != NULL);
		CHECK(strstr(t, "char l") != NULL); // the array nothing accesses
		CHECK(strstr(t, "return ") != NULL);
		free(t);
	}
}

static void TestConditions(void)
{
	char* t = Shape("conditions",
		"#engine 1.553\n"
		"main(p1, p2) {\n"
		"    if (p1 && p2) { sys.yield(); } else { sys.quit(); }\n"
		"    if (p1 || p2) return;\n"
		"    while (p1 < 10) { p1 = p1 + 1; if (p2) break; }\n"
		"    do { p2 = p2 - 1; } while (p2);\n"
		"    if (__sign(p1 - 4) < 0) { sys.yield(); }\n"
		"    if (!(p1 == 3)) {}\n"
		"}\n",
		".engine 1.553\n"
		"main:\n"
		"\tpush_fp\n\tpush_i8 8\n\tadd\n\tset_fp\n"
		"\tpush_local_addr 0x4\n\tstore 2\n\tpush_local_addr 0x8\n\tstore 2\n"
		// if (p1 && p2): both false jumps go to the else part
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_code_off ELSE\n\tjcc 1\n"
		"\tpush_local_addr 0x8\n\tload 2\n\tpush_code_off ELSE\n\tjcc 1\n"
		"\tsys.yield\n\tpush_code_off END\n\tjmp\n"
		"ELSE:\n\tsys.quit\n"
		"END:\n"
		// if (p1 || p2) return: the direct form, true jumps to the exit
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_code_off EXIT\n\tjcc 0\n"
		"\tpush_local_addr 0x8\n\tload 2\n\tpush_code_off EXIT\n\tjcc 0\n"
		// while
		"HEAD:\n\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 10\n\tlt\n\tpush_code_off WEND\n\tjcc 1\n"
		"\tpush_local_addr 0x4\n\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 1\n\tadd\n\tstore_keep 2\n"
		"\tpush_local_addr 0x8\n\tload 2\n\tpush_code_off WEND\n\tjcc 0\n"
		"\tpush_code_off HEAD\n\tjmp\n"
		"WEND:\n"
		// do .. while
		"DHEAD:\n\tpush_local_addr 0x8\n\tpush_local_addr 0x8\n\tload 2\n\tpush_i8 1\n\tsub\n\tstore_keep 2\n"
		"\tpush_local_addr 0x8\n\tload 2\n\tpush_code_off DHEAD\n\tjcc 0\n"
		// __sign(p1 - 4) < 0: the jump tests the sign (jcc 3: >= 0 goes past the then part)
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 4\n\tsub\n\tpush_code_off ELSE2\n\tjcc 3\n"
		"\tsys.yield\n"
		"ELSE2:\n"
		// an empty then part: a jump to the next instruction
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 3\n\teq\n\tpush_code_off ELSE3\n\tjcc 0\n"
		"ELSE3:\n"
		"EXIT:\n\tpush_fp\n\tpush_i8 8\n\tsub\n\tset_fp\n\tret\n"
		"\t.align 16\n");
	if(t)
	{
		CHECK(strstr(t, "if (p1 && p2) {") != NULL);
		CHECK(strstr(t, "} else {") != NULL);
		CHECK(strstr(t, "if (p1 || p2) return;") != NULL);
		CHECK(strstr(t, "while (p1 < 10) {") != NULL);
		CHECK(strstr(t, "if (p2) break;") != NULL);
		CHECK(strstr(t, "} while (p2);") != NULL);
		CHECK(strstr(t, "__sign(p1 - 4) < 0") != NULL);
		CHECK(strstr(t, "if (!(p1 == 3)) {}") != NULL);
		free(t);
	}
}

static void TestValues(void)
{
	char* t = Shape("values",
		"#engine 1.553\n"
		"main(p1) { int t[2]; int r;\n"
		"    *(int*)t = {1, 2};\n"
		"    r = sub(p1, 2);\n"
		"    *(char*)(p1 + 1) <- __pop;\n"
		"    sprintf(t, \"%d %d\", p1, r);\n"
		"    r = p1 ? -1 : 0x00000000;\n"
		"    r = (p1 > 0) && (r != 0);\n"
		"    (*(int*)(t + 4))(3);\n"
		"    gfx0.sprite_show(r, 1);\n"
		"    __inline(t, \"\\x01\\x02\");\n"
		"    debug_msg(\"%d %d\");\n"
		"    return r >>> 2;\n"
		"}\n"
		"sub(p1, p2) { return p1 * p2; }\n",
		".engine 1.553\n"
		"main:\n"
		"\tpush_fp\n\tpush_i8 16\n\tadd\n\tset_fp\n\tpush_local_addr 0x10\n\tstore 2\n"
		"\tpush_local_addr 0xc\n\tpush_i8 1\n\tpush_i8 2\n\tstore_multi 2, 2\n"                                // *(int*)t = {1, 2}
		"\tpush_i8 2\n\tpush_local_addr 0x10\n\tload 2\n\tpush_code_off sub\n\tcall\n"                         // the arguments right to left
		"\tpush_local_addr 0x4\n\tstore 2\n"                                                                   // a call's value: value, address, store
		"\tpush_local_addr 0x10\n\tload 2\n\tpush_i8 1\n\tadd\n\tstore 0\n"                                    // <- __pop: nothing pushed
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_local_addr 0x10\n\tload 2\n"                                  // sprintf: conversions right to left,
		"\tpush_local_addr 0xc\n\tpush_code_addr str_0\n\tsprintf\n"                                           // then the destination and the format
		"\tpush_local_addr 0x10\n\tload 2\n\tpush_i8 1\n\tnot\n\tpush_i8 1\n\tadd\n\tpush_i32 0x0\n\tselect\n" // -1, a wide 0
		"\tpush_local_addr 0x4\n\tstore 2\n"                                                                   // select stores like a call
		"\tpush_local_addr 0x4\n\tpush_local_addr 0x10\n\tload 2\n\tpush_i8 0\n\tgt\n"
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 0\n\tne\n\tland\n\tstore_keep 2\n" // parentheses make values
		"\tpush_i8 3\n\tpush_local_addr 0xc\n\tpush_i8 4\n\tadd\n\tload 2\n\tcall\n"   // a call through a value
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 1\n\tgfx0.sprite_show\n"           // an instruction: left to right
		"\tpush_local_addr 0xc\n\tstore_inline 0x01, 0x02\n"
		"\tpush_code_addr str_1\n\tdebug_msg\n" // the same literal again is another copy
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 2\n\tshr\n\tpush_code_off EXIT\n\tjmp\n"
		"EXIT:\n\tpush_fp\n\tpush_i8 16\n\tsub\n\tset_fp\n\tret\n"
		"sub:\n"
		"\tpush_fp\n\tpush_i8 8\n\tadd\n\tset_fp\n\tpush_local_addr 0x4\n\tstore 2\n\tpush_local_addr 0x8\n\tstore 2\n"
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_local_addr 0x8\n\tload 2\n\tmul\n\tpush_code_off EXIT2\n\tjmp\n"
		"EXIT2:\n\tpush_fp\n\tpush_i8 8\n\tsub\n\tset_fp\n\tret\n"
		"\t.align 16\n"
		"str_0:\n\t.str \"%d %d\"\n"
		"str_1:\n\t.str \"%d %d\"\n"
		"\t.align 16\n");
	if(t)
	{
		CHECK(strstr(t, "= sub_") != NULL || strstr(t, "= sub(") != NULL);
		CHECK(strstr(t, "<- __pop;") != NULL);
		CHECK(strstr(t, "sprintf(") != NULL);
		CHECK(strstr(t, "? -1 : 0x00000000") != NULL);
		CHECK(strstr(t, "__inline(") != NULL);
		CHECK(strstr(t, ">>> 2") != NULL);
		CHECK(strstr(t, "= {1, 2};") != NULL);
		free(t);
	}
}

static void TestJumps(void)
{
	char* t = Shape("jumps",
		"#engine 1.553\n"
		"main(p1) { int i;\n"
		"    i = 0;\n"
		"    for (;;) {\n"
		"        if (p1 == 1) goto done;\n"
		"        if (p1 == 2) goto done;\n"
		"        i = i + 1;\n"
		"        if (i > 3) continue;\n"
		"        sys.yield();\n"
		"    }\n"
		"done:\n"
		"    i;\n"
		"}\n",
		".engine 1.553\n"
		"main:\n"
		"\tpush_fp\n\tpush_i8 8\n\tadd\n\tset_fp\n\tpush_local_addr 0x8\n\tstore 2\n"
		"\tpush_local_addr 0x4\n\tpush_i8 0\n\tstore_keep 2\n"
		"HEAD:\n"
		"\tpush_local_addr 0x8\n\tload 2\n\tpush_i8 1\n\teq\n\tpush_code_off DONE\n\tjcc 0\n"
		"\tpush_local_addr 0x8\n\tload 2\n\tpush_i8 2\n\teq\n\tpush_code_off DONE\n\tjcc 0\n"
		"\tpush_local_addr 0x4\n\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 1\n\tadd\n\tstore_keep 2\n"
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 3\n\tgt\n\tpush_code_off HEAD\n\tjcc 0\n"
		"\tsys.yield\n"
		"\tpush_code_off HEAD\n\tjmp\n"
		"DONE:\n"
		"\tpush_local_addr 0x4\n\tload 2\n"
		"\tpush_fp\n\tpush_i8 8\n\tsub\n\tset_fp\n\tret\n"
		"\t.align 16\n");
	if(t)
	{
		CHECK(strstr(t, "while (!(p1 == 1) && !(p1 == 2)) {") != NULL); // the same bytes as the loop with the gotos
		CHECK(strstr(t, "continue;") != NULL);
		free(t);
	}
}

// ---- memory areas ----------------------------------------------------------------------------------

// compile `src` (which names its engine) and compare with the assembled `listing`
static void Bytes(const char* what, const char* src, const char* listing)
{
	uint32_t cn = 0, an = 0;
	uint8_t* c = Compile(src, &cn);
	uint8_t* a = Assemble(listing, &an);
	Same(what, c, cn, a, an);
	BGI_Free(c);
	BGI_Free(a);
}

// two sources that must compile to the same file
static void SameSource(const char* what, const char* one, const char* other)
{
	uint32_t an = 0, bn = 0;
	uint8_t* a = Compile(one, &an);
	uint8_t* b = Compile(other, &bn);
	Same(what, a, an, b, bn);
	BGI_Free(a);
	BGI_Free(b);
}

static void TestAreas(void)
{
	static const char named[] =
		"#engine 1.553\n"
		"global int gFlag @ 1124;\n"
		"global char st[0x100] @ 0x1bb84;\n"
		"global int stIp @ st + 164;\n"
		"global short cfgVol[8] @ 32 + 16;\n"
		"global int hSprite[4 * 4] @ 2732;\n"
		"global int unused @ 0x00000004;\n" // a declaration compiles to nothing
		"main(p1) { char buf[8];\n"
		"    gFlag = 1;\n"
		"    stIp = stIp + 2;\n"
		"    cfgVol[p1] = hSprite[3];\n"
		"    buf[p1] = st[0];\n"
		"    p1 = &gFlag;\n"
		"    p1 = &hSprite[p1];\n"
		"    gFlag();\n"
		"}\n";
	char* t = Shape("global variables", named,
		".engine 1.553\n"
		"main:\n"
		"\tpush_fp\n\tpush_i8 12\n\tadd\n\tset_fp\n"
		"\tpush_local_addr 0xc\n\tstore 2\n"
		"\tpush_i16 1124\n\tpush_i8 1\n\tstore_keep 2\n" // a variable is its address and a load or store of its type
		"\tpush_i32 0x1bb84\n\tpush_i16 164\n\tadd\n"    // an address is compiled as it was written, a name in it too
		"\tpush_i32 0x1bb84\n\tpush_i16 164\n\tadd\n\tload 2\n\tpush_i8 2\n\tadd\n\tstore_keep 2\n"
		"\tpush_i8 32\n\tpush_i8 16\n\tadd\n\tpush_local_addr 0xc\n\tload 2\n\tpush_i8 2\n\tmul\n\tadd\n" // name[i]: address, i, the element size, mul, add
		"\tpush_i16 2732\n\tpush_i8 3\n\tpush_i8 4\n\tmul\n\tadd\n\tload 2\n"
		"\tstore_keep 1\n"
		"\tpush_local_addr 0x8\n\tpush_local_addr 0xc\n\tload 2\n\tadd\n" // a byte array: no multiplication; a local array indexes alike
		"\tpush_i32 0x1bb84\n\tpush_i8 0\n\tadd\n\tload 0\n"
		"\tstore_keep 0\n"
		"\tpush_local_addr 0xc\n\tpush_i16 1124\n\tstore_keep 2\n" // &name
		"\tpush_local_addr 0xc\n\tpush_i16 2732\n\tpush_local_addr 0xc\n\tload 2\n\tpush_i8 4\n\tmul\n\tadd\n\tstore_keep 2\n"
		"\tpush_i16 1124\n\tload 2\n\tcall\n" // a call through the variable's value
		"EXIT:\n\tpush_fp\n\tpush_i8 12\n\tsub\n\tset_fp\n\tret\n"
		"\t.align 16\n");
	free(t);
	// the names against the numbers they stand for
	SameSource("names and numbers", named,
		"#engine 1.553\n"
		"main(p1) { char buf[8];\n"
		"    *(int*)1124 = 1;\n"
		"    *(int*)(0x1bb84 + 164) = *(int*)(0x1bb84 + 164) + 2;\n"
		"    *(short*)(32 + 16 + p1 * 2) = *(int*)(2732 + 3 * 4);\n"
		"    *(char*)(buf + p1) = *(char*)(0x1bb84 + 0);\n"
		"    p1 = 1124;\n"
		"    p1 = 2732 + p1 * 4;\n"
		"    (*(int*)1124)();\n"
		"}\n");
	// the other areas: the first number of the address carries the tag of the engine's pointer layout
	Bytes("areas of 1.64",
		"#engine 1.64\n"
		"heap int hp @ 0x10;\n"
		"heap char blk[64] @ 0x20;\n"
		"heap int blkX @ blk + 4;\n"
		"dynamic[1] int dyn[4] @ 0x400;\n"
		"code char hdr[16] @ 0;\n"
		"data int tls @ 4;\n"
		"main() {\n"
		"    hp = 5;\n"
		"    blkX = hp;\n"
		"    dyn[2] = 1;\n"
		"    tls = hdr[1];\n"
		"}\n",
		".engine 1.64\n"
		"main:\n"
		"\tpush_fp\n\tpush_i8 0\n\tadd\n\tset_fp\n"
		"\tpush_i32 0x12000010\n\tpush_i8 5\n\tstore_keep 2\n"
		"\tpush_i32 0x12000020\n\tpush_i8 4\n\tadd\n\tpush_i32 0x12000010\n\tload 2\n\tstore_keep 2\n"
		"\tpush_i32 0x41000400\n\tpush_i8 2\n\tpush_i8 4\n\tmul\n\tadd\n\tpush_i8 1\n\tstore_keep 2\n"
		"\tpush_i32 0x10000004\n\tpush_i32 0x11000000\n\tpush_i8 1\n\tadd\n\tload 0\n\tstore_keep 2\n"
		"EXIT:\n\tpush_fp\n\tpush_i8 0\n\tsub\n\tset_fp\n\tret\n"
		"\t.align 16\n");
	Bytes("areas of 1.69/444",
		"#engine 1.69/444\nheap int hp @ 0x10;\ndynamic[0] int dyn @ 0x220400;\nmain() { hp = dyn; }\n",
		".engine 1.69/444\n"
		"main:\n"
		"\tpush_fp\n\tpush_i8 0\n\tadd\n\tset_fp\n"
		"\tpush_i32 0x14000010\n\tpush_i32 0x20220400\n\tload 2\n\tstore_keep 2\n"
		"EXIT:\n\tpush_fp\n\tpush_i8 0\n\tsub\n\tset_fp\n\tret\n"
		"\t.align 16\n");
	Bytes("areas of 1.553",
		"#engine 1.553\nheap int hp @ 0x10;\nmain() { hp = 1; }\n",
		".engine 1.553\n"
		"main:\n"
		"\tpush_fp\n\tpush_i8 0\n\tadd\n\tset_fp\n"
		"\tpush_i32 0x0c000010\n\tpush_i8 1\n\tstore_keep 2\n"
		"EXIT:\n\tpush_fp\n\tpush_i8 0\n\tsub\n\tset_fp\n\tret\n"
		"\t.align 16\n");
	// the words of the areas stay ordinary names where no declaration starts
	SameSource("area words as names",
		"#engine 1.553\nglobal(heap) { int data; int code; int dynamic[2]; data = heap; code = data; dynamic[1] = code; return dynamic[1]; }\n",
		"#engine 1.553\nmain(p1) { int a; int b; int c[2]; a = p1; b = a; c[1] = b; return c[1]; }\n");
}

// ---- includes ----------------------------------------------------------------------------------------

typedef struct MemFile
{
	const char* path;
	const char* text;
} MemFile_t;

static int gReads; // the files ReadMem handed out

// the reader of PrepIncludes_t over a table of MemFile_t that ends with a NULL path
static uint8_t* ReadMem(void* ctx, const char* path, uint32_t* size)
{
	const MemFile_t* f;
	for(f = (const MemFile_t*)ctx; f->path; f++)
	{
		char native[256];
		size_t i;
		snprintf(native, sizeof native, "%s", f->path);
		for(i = 0; native[i]; i++)
			if(native[i] == '/')
				native[i] = OS_NATIVE_SEP;
		if(strcmp(native, path) == 0)
		{
			size_t n = strlen(f->text);
			uint8_t* copy = (uint8_t*)malloc(n + 1);
			memcpy(copy, f->text, n + 1);
			*size = (uint32_t)n;
			gReads++;
			return copy;
		}
	}
	return NULL;
}

static void TestIncludes(void)
{
	static const MemFile_t files[] = {
		{"src/globals.inc", "\xEF\xBB\xBF// the flags\nglobal int gFlag @ 1124;\n#include \"more.inc\"\n"},
		{"src/more.inc", "global int gOther @ 1128;\n#include \"globals.inc\"\n#include \"../lib/deep.inc\"\nhelper() { return gOther + gDeep; }\n"},
		{"src/../lib/deep.inc", "global int gDeep @ 1132;\n"}, // as more.inc names it
		{"lib/deep.inc", "global int gDeep @ 1132;\n"},
		{"lib/only.inc", "global int gOnly @ 1136;\n"},
		{"src/bad.inc", "global int gFine @ 4;\nglobal int gBad = 1;\n"},
		{NULL, NULL},
	};
	static const char* const dirs[] = {"nowhere", "lib"};
	static const char main1[] =
		"\xEF\xBB\xBF" // a byte order mark is skipped
		"#engine 1.553\n"
		"#include \"globals.inc\"\n"
		"#include \"./globals.inc\"\n" // the same file once more: not read again
		"#include \"only.inc\"\n"      // found through the directory list
		"main() { gFlag = helper() + gOnly; }\n";
	PrepIncludes_t inc;
	BpcResult_t r;
	uint32_t pn = 0;
	uint8_t* plain = Compile("#engine 1.553\nhelper() { return *(int*)1128 + *(int*)1132; }\nmain() { *(int*)1124 = helper() + *(int*)1136; }\n", &pn);
	memset(&inc, 0, sizeof inc);
	inc.dirs = dirs;
	inc.dirCount = 2;
	inc.read = ReadMem;
	inc.ctx = (void*)files;
	gReads = 0;
	Bpc_Compile(main1, strlen(main1), "src/main.bpc", GEN, &inc, stdout, &r);
	CHECK(r.errors == 0);
	Same("includes", r.file, r.size, plain, pn);
	CHECK(gReads == 4); // globals, more, deep, only - each once
	BGI_Free(r.file);
	BGI_Free(r.labels);
	BGI_Free(plain);
	{ // the directory of the source given apart from its name
		static const char main2[] = "#engine 1.553\n#include \"deep.inc\"\nmain() { gDeep = 1; }\n";
		PrepIncludes_t at = inc;
		at.baseDir = "lib";
		at.dirCount = 0;
		Bpc_Compile(main2, strlen(main2), "entry.bpc", GEN, &at, stdout, &r);
		CHECK(r.errors == 0 && r.file != NULL);
		BGI_Free(r.file);
		BGI_Free(r.labels);
	}
	{ // a message names the file and the line it is about
		static const char main3[] = "#engine 1.553\n#include \"bad.inc\"\nmain() { }\n";
		FILE* log = tmpfile();
		char text[512];
		size_t n;
		Bpc_Compile(main3, strlen(main3), "src/main.bpc", GEN, &inc, log ? log : stdout, &r);
		CHECK(r.errors == 1 && r.file == NULL);
		BGI_Free(r.labels);
		if(log)
		{
			fseek(log, 0, SEEK_SET);
			n = fread(text, 1, sizeof text - 1, log);
			text[n] = 0;
			fclose(log);
			CHECK(strstr(text, "bad.inc:2: error: gBad") != NULL);
		}
	}
}

// ---- defines -----------------------------------------------------------------------------------------

static void TestDefines(void)
{
	SameSource("defined constants",
		"#engine 1.553\n"
		"#define ST_BASE 0x1bb84        // a comment is no part of the text\n"
		"#define ST_IP   ST_BASE + 164  /* nor is this one */\n"
		"#define TITLE   \"a // title /* with */ ST_BASE\"\n"
		"#define NOTHING\n"
		"#define NEG     -1\n"
		"#define yield   99\n"
		"#define COUNT   3\n"
		"global int stIp @ ST_IP;\n"
		"main(p1) { char buf[COUNT]; NOTHING\n"
		"    p1 = *(int*)(ST_IP) + stIp;\n"
		"    strcpy(buf, TITLE); strcpy(buf, TITLE); // TITLE: every use its own copy\n"
		"    p1 = \"COUNT\" + 'C' + 5 -NEG;\n"
		"    sys.yield(); /* COUNT\n"
		"    COUNT */ p1 = yield;\n"
		"#undef COUNT\n"
		"#define COUNT 4\n"
		"#define COUNT 4\n" // the same text again is no error
		"    p1 = COUNT;\n"
		"}\n",
		"#engine 1.553\n"
		"main(p1) { char buf[3];\n"
		"    p1 = *(int*)(0x1bb84 + 164) + *(int*)(0x1bb84 + 164);\n"
		"    strcpy(buf, \"a // title /* with */ ST_BASE\"); strcpy(buf, \"a // title /* with */ ST_BASE\");\n"
		"    p1 = \"COUNT\" + 'C' + 5 - -1;\n"
		"    sys.yield();\n"
		"    p1 = 99;\n"
		"    p1 = 4;\n"
		"}\n");
	{ // a message is about the line of the source, whatever was replaced on it
		static const char src[] = "#engine 1.553\n#define TWO 1 + 1\n\nmain() { int a;\n    a = TWO +;\n}\n";
		BpcResult_t r;
		FILE* log = tmpfile();
		char text[512];
		size_t n;
		Bpc_Compile(src, strlen(src), "lines.bpc", GEN, NULL, log ? log : stdout, &r);
		CHECK(r.errors > 0 && r.file == NULL);
		BGI_Free(r.labels);
		if(log)
		{
			fseek(log, 0, SEEK_SET);
			n = fread(text, 1, sizeof text - 1, log);
			text[n] = 0;
			fclose(log);
			CHECK(strstr(text, "lines.bpc:5: error:") != NULL);
		}
	}
}

// ---- records ---------------------------------------------------------------------------------------

/* Records are names for address arithmetic: every form is compared with
 * the same access written with numbers and casts, which the shapes above
 * pin to bytes. */
static void TestRecords(void)
{
	SameSource("records",
		"#engine 1.553\n"
		"struct Point { int x; int y; };\n"
		"struct Font { char kind; short size; short width; char bold; int colour @ 12; };\n" // packed; @ places a field
		"struct Sprite { int used; Point pos; char name[32]; Sprite* next; func free; int tail[2]; };\n"
		"struct Group { int count; Sprite* items; };\n"
		"global Font gFont @ 1000;\n"
		"global Sprite gSprite[16] @ 2000;\n"
		"global func gTable[8] @ 3000;\n"
		"global char gNames[4][32] @ 4000;\n"
		"global char* gText @ 5000;\n"
		"f(Sprite* p, i, Group* g, char** list, func* fns) { Sprite s; Point* q; char* c; short* w; func tbl[2];\n"
		"    gFont.colour = gFont.size + gFont.bold;\n" // fields of a variable: address, offset, add - also offset 0
		"    gFont.kind = 1;\n"
		"    gSprite[i].pos.y = p->pos.x;\n"         // an element, a record in a record, a pointer's field
		"    p->next->used = g->items[i].tail[1];\n" // pointers followed
		"    gTable[i] = f;\n"                       // a table of addresses is indexed with a shift
		"    gTable[i](1);\n"
		"    fns[i](2);\n"
		"    c = list[i];\n"
		"    i = *c + c[2] + *w + *gText + gText[i];\n" // *p reads what p points to
		"    *c = 3; *w = 4;\n"
		"    i = *p->name + *s.name + *gNames[i];\n" // ... and the first element of an array
		"    q = &gSprite[i].pos;\n"
		"    q->x = sizeof(Sprite) + sizeof(s) + sizeof(gSprite) + sizeof(Point);\n"
		"    memclr(s, sizeof(Sprite));\n" // a record's name is its address
		"    memcpy(&gSprite[i], s, sizeof(s));\n"
		"    strcpy(gNames[i], s.name);\n" // a row of a two-dimensional array, an array field
		"    tbl = {f, f};\n"
		"    gSprite[2].tail = {7, 8};\n" // a list fills an array ...
		"    gTable[3] = {f, f};\n"       // ... or the elements from one on
		"    p = &p[1];\n"
		"    s.free(p);\n"
		"}\n",
		"#engine 1.553\n"
		"f(p, i, g, list, fns) { int s[15]; int q; int c; int w; int tbl[2];\n"
		"    *(int*)(1000 + 12) = *(short*)(1000 + 1) + *(char*)(1000 + 5);\n"
		"    *(char*)(1000 + 0) = 1;\n"
		"    *(int*)(2000 + i * 60 + 4 + 4) = *(int*)(p + 4 + 0);\n"
		"    *(int*)(*(int*)(p + 44) + 0) = *(int*)(*(int*)(g + 4) + i * 60 + 52 + 1 * 4);\n"
		"    *(int*)(3000 + (i << 2)) = f;\n"
		"    (*(int*)(3000 + (i << 2)))(1);\n"
		"    (*(int*)(fns + (i << 2)))(2);\n"
		"    c = *(int*)(list + (i << 2));\n"
		"    i = *(char*)c + *(char*)(c + 2) + *(short*)w + *(char*)(*(int*)5000) + *(char*)(*(int*)5000 + i);\n"
		"    *(char*)c = 3; *(short*)w = 4;\n"
		"    i = *(char*)(p + 12) + *(char*)(s + 12) + *(char*)(4000 + i * 32);\n"
		"    q = 2000 + i * 60 + 4;\n"
		"    *(int*)(q + 0) = 60 + 60 + 960 + 8;\n"
		"    memclr(s, 60);\n"
		"    memcpy(2000 + i * 60, s, 60);\n"
		"    strcpy(4000 + i * 32, s + 12);\n"
		"    *(int*)tbl = {f, f};\n"
		"    *(int*)(2000 + 2 * 60 + 52) = {7, 8};\n"
		"    *(int*)(3000 + (3 << 2)) = {f, f};\n"
		"    p = p + 1 * 60;\n"
		"    (*(int*)(s + 48))(p);\n"
		"}\n");
	{                                                                                                  // the strings: Shift-JIS unless the file or the caller says UTF-8
		static const char sjis[] = "#engine 1.553\nmain() { sys.yield(\"\xc3\xa5\xe6\x97\xa5\"); }\n"; // "å日": no å in the code page
		static const char utf8[] = "#engine 1.553\n#encoding utf-8\nmain() { sys.yield(\"\xc3\xa5\xe6\x97\xa5\"); }\n";
		static const uint8_t wantSjis[] = {'?', 0x93, 0xfa, 0}, wantUtf8[] = {0xc3, 0xa5, 0xe6, 0x97, 0xa5, 0};
		uint32_t n = 0, i, found;
		FILE* null = tmpfile();
		uint8_t* a;
		BpcResult_t r;
		Bpc_Compile(sjis, strlen(sjis), "test", GEN, NULL, null ? null : stdout, &r); // (the "?" comes with a warning)
		BGI_Free(r.labels);
		a = r.file;
		n = r.size;
		for(found = 0, i = 0; a && i + sizeof wantSjis <= n; i++)
			found |= memcmp(a + i, wantSjis, sizeof wantSjis) == 0;
		CHECK(found);
		BGI_Free(a);
		a = Compile(utf8, &n);
		for(found = 0, i = 0; a && i + sizeof wantUtf8 <= n; i++)
			found |= memcmp(a + i, wantUtf8, sizeof wantUtf8) == 0;
		CHECK(found);
		BGI_Free(a);
		if(null)
			fclose(null);
	}
}

// ---- errors ----------------------------------------------------------------------------------------

static void TestErrors(void)
{
	static const char* const bad[] = {
		"main() { x = 1; }",                                         // an unknown variable
		"main() { break; }",                                         // break outside a loop
		"main() { if (p1) }",                                        // a missing statement
		"main() { int a; a = __sign(a) + 1; }",                      // __sign outside a condition
		"main() { sub(); }",                                         // an unknown function
		"main() { \"unterminated; }",                                // a bad string literal
		"global int g = 1; main() { }",                              // a value where a global is declared
		"global int g @ 4 = 1; main() { }",                          // ... after its address too
		"global int g; main() { }",                                  // no address
		"global float g @ 4; main() { }",                            // no such type
		"global int g @ 4; global int g @ 8; main() { }",            // declared twice
		"global int g @ 4; g() { } main() { }",                      // a variable and a function of one name
		"g() { } global int g @ 4; main() { }",                      // ... in the other order
		"global int g @ 4; main() { g[1] = 0; }",                    // an index on a variable that is no array
		"global int g[2] @ 4; main() { g = 0; }",                    // an assignment to an array
		"global int g[0] @ 4; main() { }",                           // an empty array
		"global int g @ p1; main() { }",                             // an address that names nothing declared
		"global int g @ 4; global int h @ g + 4; main() { }",        // a variable's value in an address (its address is &g)
		"global int g @ sys.yield(); main() { }",                    // an address that is no constant
		"global int g @ 8 / 0; main() { }",                          // a division by zero in an address
		"heap int h @ 4; global int g @ &h + 4; main() { }",         // an address built on another area's variable
		"heap char b[8] @ 4; heap int h @ 4 + b; main() { h = 1; }", // a name that is not the first term of a tagged address
		"heap int h @ -4; main() { h = 1; }",                        // a tagged address that starts with an operator
		"heap int h @ 0x4000000; main() { h = 1; }",                 // an offset beyond the 26 bits of this engine's pointers
		"dynamic[0] int d @ 4; main() { d = 1; }",                   // a numbered dynamic block where the blocks are pooled (1.535 on)
		"#include \"no/such/file.inc\"\nmain() { }",                 // a file that is not there
		"#include\nmain() { }",                                      // no file name
		"main() { int a[2]; a[1; }",                                 // an index without its bracket
		"main() { global int g @ 4; g = 1; }",                       // a declaration inside a function
		"#define A 1\n#define A 2\nmain() { }",                      // a name defined twice with different texts
		"#define F(x) x\nmain() { }",                                // a definition with parameters
		"#define\nmain() { }",                                       // no name
		"#define S \"open\nmain() { }",                              // a string that does not end in the definition
		"#define A A + 1\nmain(p1) { p1 = A; }",                     // a name in its own text stays a name, which nothing declares
		"#define A 1\n#undef A\nmain(p1) { p1 = A; }",               // used after its #undef
		"main() { dynamic[0] int d @ 4; }",                          // ... of a dynamic block's
		"struct A { A a; }; main() { }",                             // a record that contains itself
		"struct A { }; main() { }",                                  // a record without fields
		"struct A { int x; int x; }; main() { }",                    // a field declared twice
		"struct A { int x; }; struct A { int y; }; main() { }",      // a record declared twice
		"struct A { int x; }; main() { A a; a = 1; }",               // an assignment to a record
		"struct A { int x; }; main() { A a; a.y = 1; }",             // no such field
		"struct A { int x; }; main() { A a; a->x = 1; }",            // -> on a record that is no pointer
		"struct A { int x; }; main() { A* p; p.x = 1; }",            // . on a pointer
		"main() { int a; a.x = 1; }",                                // a field of something that is no record
		"main() { B* p; }",                                          // an unknown type
	};
	size_t i;
	FILE* null = tmpfile();
	for(i = 0; i < sizeof bad / sizeof bad[0]; i++)
	{
		BpcResult_t r;
		Bpc_Compile(bad[i], strlen(bad[i]), "bad", GEN, NULL, null ? null : stdout, &r);
		CHECK(r.errors > 0);
		CHECK(r.file == NULL);
		BGI_Free(r.labels);
	}
	if(null)
		fclose(null);
	{ // a file without a usable header
		uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
		BpcDecOptions_t opt;
		FILE* mem = tmpfile();
		memset(&opt, 0, sizeof opt);
		opt.gen = GEN;
		CHECK(Bpc_Decompile(junk, sizeof junk, &opt, mem ? mem : stdout) == -1);
		if(mem)
			fclose(mem);
	}
}

int main(void)
{
	OS_Init(); // the string conversions (the Shift-JIS strings of TestRecords)
	printf("bpc: shapes\n");
	TestFrame();
	TestConditions();
	TestValues();
	TestJumps();
	printf("bpc: areas\n");
	TestAreas();
	printf("bpc: records\n");
	TestRecords();
	printf("bpc: includes\n");
	TestIncludes();
	printf("bpc: defines\n");
	TestDefines();
	printf("bpc: errors\n");
	TestErrors();
	if(gFails)
	{
		printf("bpc: %d check(s) failed\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
