/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/fonts.c - unit test of the family names of font files
 *                 (src/base/fontfile.c, src/include/bgi/fontfile.h) and of the
 *                 faces opened in place of missing ones (FontNames_Resolve,
 *                 src/gfx/bmptext.c); built as test_fonts by `make test`
 *
 * The fonts are made by the test: a table directory with only a "name"
 * table, which is all the name lookups read.  The groups, in the order
 * they run:
 *
 *   names    - the families of a font: the English one first whatever the
 *              order of the records, a Japanese one, a name past the basic
 *              plane, the Macintosh record of the same name once, the
 *              other name IDs left out; a collection's faces apart; data
 *              that is no font, or cut short
 *   resolve  - (on the engine's own rasteriser: GDI takes no font without
 *              glyphs) a font handed over as data is installed under its
 *              names (gfx/typeface.c); a face
 *              that is not installed opens as the substitute of its class
 *              (FontNames_AddSubstitute, as the fonts built into the
 *              binary), of the other class when its own has none, as its
 *              fallback ("B0 C7") when that is installed
 */
#include "bgi/fontfile.h"
#include "bgi/core.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/typeface.h"
#include "host/os.h"

#include "testhost.h"

#include <stdio.h>

#include "check.h"

// ---- making fonts -------------------------------------------------------------------------

typedef struct NameRec
{
	uint16_t platform, encoding, lang, id;
	const char* text; // UTF-8; written as UTF-16BE, or as it is for the Macintosh platform
} NameRec_t;

static void Be16(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void Be32(uint8_t* p, uint32_t v)
{
	Be16(p, v >> 16);
	Be16(p + 2, v & 0xffff);
}

// UTF-8 to UTF-16BE at out; the bytes written
static size_t ToUtf16Be(const char* s, uint8_t* out)
{
	const uint8_t* u = (const uint8_t*)s;
	size_t off = 0;
	while(*u)
	{
		uint32_t c;
		if(*u < 0x80)
			c = *u++;
		else if(*u < 0xe0)
			c = ((u[0] & 0x1fu) << 6) | (u[1] & 0x3fu), u += 2;
		else if(*u < 0xf0)
			c = ((u[0] & 0x0fu) << 12) | ((u[1] & 0x3fu) << 6) | (u[2] & 0x3fu), u += 3;
		else
			c = ((u[0] & 0x07u) << 18) | ((u[1] & 0x3fu) << 12) | ((u[2] & 0x3fu) << 6) | (u[3] & 0x3fu), u += 4;
		if(c >= 0x10000)
		{
			c -= 0x10000;
			Be16(out + off, 0xd800 + (c >> 10));
			Be16(out + off + 2, 0xdc00 + (c & 0x3ff));
			off += 4;
		}
		else
		{
			Be16(out + off, c);
			off += 2;
		}
	}
	return off;
}

/* a face at `at` of `buf`: the sfnt header, one table record, the name
 * table; the bytes it takes */
static size_t MakeFace(uint8_t* buf, size_t at, const NameRec_t* recs, int n)
{
	uint8_t* f = buf + at;
	uint8_t* name = f + 12 + 16;
	size_t strings = 6 + (size_t)n * 12, off = 0;
	int i;
	Be32(f, 0x00010000);
	Be16(f + 4, 1);
	memcpy(f + 12, "name", 4);
	Be32(f + 12 + 8, (uint32_t)(at + 12 + 16)); // offsets count from the start of the file
	Be16(name, 0);
	Be16(name + 2, (uint32_t)n);
	Be16(name + 4, (uint32_t)strings);
	for(i = 0; i < n; i++)
	{
		uint8_t* r = name + 6 + i * 12;
		size_t len;
		if(recs[i].platform == 1)
		{
			len = strlen(recs[i].text);
			memcpy(name + strings + off, recs[i].text, len);
		}
		else
			len = ToUtf16Be(recs[i].text, name + strings + off);
		Be16(r, recs[i].platform);
		Be16(r + 2, recs[i].encoding);
		Be16(r + 4, recs[i].lang);
		Be16(r + 6, recs[i].id);
		Be16(r + 8, (uint32_t)len);
		Be16(r + 10, (uint32_t)off);
		off += len;
	}
	Be32(f + 12 + 12, (uint32_t)(strings + off)); // the name table's length
	return 12 + 16 + strings + off;
}

static const NameRec_t kGothic[] = {
	{3, 1, 0x0411, 1, "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88 \xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf"}, // テスト ゴシック
	{3, 1, 0x0409, 2, "Regular"},                                                                               // a style, not a family
	{1, 0, 0, 1, "Test Gothic"},                                                                                // the Macintosh name, the same
	{3, 1, 0x0409, 1, "Test Gothic"},
	{0, 3, 0, 1, "Test \xf0\x9f\x80\x84"}, // a character past the basic plane (a surrogate pair)
};
static const NameRec_t kPGothic[] = {
	{3, 1, 0x0409, 1, "Test PGothic"},
};
static const NameRec_t kMincho[] = {
	{3, 1, 0x0409, 1, "Test Serif"},
};

static const NameRec_t kPropGothic[] = {
	{3, 1, 0x0409, 1, "Test Prop Sans"},
};
static const NameRec_t kPropMincho[] = {
	{3, 1, 0x0409, 1, "Test Prop Roman"},
};

static uint8_t gGothic[1024], gCollection[2048], gMincho[1024], gPropGothic[1024], gPropMincho[1024];
static size_t gGothicSize, gCollectionSize, gMinchoSize, gPropGothicSize, gPropMinchoSize;

static void MakeFonts(void)
{
	size_t a, b;
	gGothicSize = MakeFace(gGothic, 0, kGothic, (int)(sizeof kGothic / sizeof kGothic[0]));
	gMinchoSize = MakeFace(gMincho, 0, kMincho, 1);
	gPropGothicSize = MakeFace(gPropGothic, 0, kPropGothic, 1);
	gPropMinchoSize = MakeFace(gPropMincho, 0, kPropMincho, 1);
	// a collection of the two gothic faces
	memcpy(gCollection, "ttcf", 4);
	Be32(gCollection + 4, 0x00010000);
	Be32(gCollection + 8, 2);
	a = 20;
	b = a + MakeFace(gCollection, a, kGothic, (int)(sizeof kGothic / sizeof kGothic[0]));
	gCollectionSize = b + MakeFace(gCollection, b, kPGothic, 1);
	Be32(gCollection + 12, (uint32_t)a);
	Be32(gCollection + 16, (uint32_t)b);
}

// ---- the groups -----------------------------------------------------------------------------

static void TestNames(void)
{
	char names[8][FONTFILE_NAME_MAX];
	int n;
	CHECK(FontFile_Faces(gGothic, gGothicSize) == 1);
	n = FontFile_Families(gGothic, gGothicSize, 0, names, 8);
	CHECK(n == 3);
	CHECK(n >= 1 && strcmp(names[0], "Test Gothic") == 0); // English first
	CHECK(n >= 2 && strcmp(names[1], kGothic[0].text) == 0);
	CHECK(n >= 3 && strcmp(names[2], "Test \xf0\x9f\x80\x84") == 0);
	CHECK(FontFile_Families(gGothic, gGothicSize, 0, names, 1) == 3 && strcmp(names[0], "Test Gothic") == 0); // only one kept
	CHECK(FontFile_Families(gGothic, gGothicSize, 1, names, 8) == 0);                                         // no second face

	CHECK(FontFile_Faces(gCollection, gCollectionSize) == 2);
	n = FontFile_Families(gCollection, gCollectionSize, 1, names, 8);
	CHECK(n == 1 && strcmp(names[0], "Test PGothic") == 0);
	n = FontFile_Families(gCollection, gCollectionSize, 0, names, 8);
	CHECK(n == 3 && strcmp(names[0], "Test Gothic") == 0);

	CHECK(FontFile_Faces((const uint8_t*)"PK\3\4 not a font", 16) == 0);
	CHECK(FontFile_Families((const uint8_t*)"PK\3\4 not a font", 16, 0, names, 8) == 0);
	{ // cut short anywhere: no family past the end, no crash
		size_t cut;
		int bad = 0;
		for(cut = 0; cut < gGothicSize; cut++)
		{
			uint8_t* copy = (uint8_t*)malloc(cut ? cut : 1);
			memcpy(copy, gGothic, cut);
			n = FontFile_Families(copy, cut, 0, names, 8);
			if(n > 3)
				bad++;
			free(copy);
		}
		CHECK(bad == 0);
	}
}

static void TestResolve(OpenBGI_t* o)
{
	static const char kMsGothic[] = "\x82\x6c\x82\x72\x20\x83\x53\x83\x56\x83\x62\x83\x4e";          // ＭＳ ゴシック
	static const char kMsMincho[] = "\x82\x6c\x82\x72\x20\x96\xbe\x92\xa9";                          // ＭＳ 明朝
	static const char kMsPGothic[] = "\x82\x6c\x82\x72\x20\x82\x6f\x83\x53\x83\x56\x83\x62\x83\x4e"; // ＭＳ Ｐゴシック
	static const char kMsPMincho[] = "\x82\x6c\x82\x72\x20\x82\x6f\x96\xbe\x92\xa9";                 // ＭＳ Ｐ明朝
	CHECK(!Typefaces_Installed(o, "Test PGothic"));
	CHECK(Typefaces_AddMemory(o, gCollection, (uint32_t)gCollectionSize, 1));
	CHECK(Typefaces_Installed(o, "Test PGothic") && Typefaces_Installed(o, "test pgothic") && Typefaces_Exists(o, "Test PGothic"));
	CHECK(Typefaces_Installed(o, "\x83\x65\x83\x58\x83\x67 \x83\x53\x83\x56\x83\x62\x83\x4e")); // テスト ゴシック, the Japanese name
	CHECK(!Typefaces_Installed(o, "No Such Face 1234"));

	// without a substitute the faces stay what they are
	CHECK(strcmp(FontNames_Resolve(o, kMsGothic), kMsGothic) == 0);
	CHECK(FontNames_AddSubstitute(o, gGothic, (uint32_t)gGothicSize, 0));
	CHECK(!FontNames_AddSubstitute(o, (const uint8_t*)"not a font", 10, 1));
	if(!Typefaces_Installed(o, kMsGothic)) // (a system that has MS Gothic opens it)
		CHECK(strcmp(FontNames_Resolve(o, kMsGothic), "Test Gothic") == 0);
	if(!Typefaces_Installed(o, kMsMincho))
		CHECK(strcmp(FontNames_Resolve(o, kMsMincho), "Test Gothic") == 0);   // no Mincho substitute: the gothic one
	CHECK(strcmp(FontNames_Resolve(o, "Test PGothic"), "Test PGothic") == 0); // installed
	CHECK(FontNames_AddSubstitute(o, gMincho, (uint32_t)gMinchoSize, 1));
	CHECK(strcmp(FontNames_Resolve(o, "No Such Mincho"), "Test Serif") == 0);
	CHECK(strcmp(FontNames_Resolve(o, "No Such Serif"), "Test Serif") == 0);
	CHECK(strcmp(FontNames_Resolve(o, "No Such Face"), "Test Gothic") == 0);
	// --stand-in-font= says the design, whatever the name says
	o->engine.optStandIn = 2;
	CHECK(strcmp(FontNames_Resolve(o, "No Such Face"), "Test Serif") == 0 && strcmp(FontNames_Resolve(o, "No Such Sans Serif"), "Test Serif") == 0);
	o->engine.optStandIn = 1;
	CHECK(strcmp(FontNames_Resolve(o, "No Such Mincho"), "Test Gothic") == 0 && strcmp(FontNames_Resolve(o, "No Such Face"), "Test Gothic") == 0);
	CHECK(strcmp(FontNames_Resolve(o, "Test PGothic"), "Test PGothic") == 0); // (installed: itself)
	o->engine.optStandIn = 0;
	CHECK(strcmp(FontNames_Resolve(o, "No Such Mincho"), "Test Serif") == 0);
	// a fallback ("B0 C7") that is installed comes before the substitute, one that is not does not
	FontNames_SetFallback(o, "No Such Face", "Test PGothic");
	CHECK(strcmp(FontNames_Resolve(o, "No Such Face"), "Test PGothic") == 0);
	FontNames_SetFallback(o, "No Such Face", "Another Missing Face");
	CHECK(strcmp(FontNames_Resolve(o, "No Such Face"), "Test Gothic") == 0);

	// the proportional faces: without a substitute of their own the fixed one of the same design
	if(!Typefaces_Installed(o, kMsPGothic))
		CHECK(strcmp(FontNames_Resolve(o, kMsPGothic), "Test Gothic") == 0);
	if(!Typefaces_Installed(o, kMsPMincho))
		CHECK(strcmp(FontNames_Resolve(o, kMsPMincho), "Test Serif") == 0);
	CHECK(strcmp(FontNames_Resolve(o, "No Such PMincho"), "Test Serif") == 0);
	CHECK(!FontNames_AddSubstitute(o, gPropGothic, (uint32_t)gPropGothicSize, 4)); // no such class
	CHECK(!FontNames_AddSubstitute(o, gPropGothic, (uint32_t)gPropGothicSize, -1));
	CHECK(FontNames_AddSubstitute(o, gPropGothic, (uint32_t)gPropGothicSize, 2));
	if(!Typefaces_Installed(o, kMsPGothic))
		CHECK(strcmp(FontNames_Resolve(o, kMsPGothic), "Test Prop Sans") == 0);
	if(!Typefaces_Installed(o, "MS UI Gothic"))
		CHECK(strcmp(FontNames_Resolve(o, "MS UI Gothic"), "Test Prop Sans") == 0);
	CHECK(strcmp(FontNames_Resolve(o, "No Such PGothic"), "Test Prop Sans") == 0);
	CHECK(strcmp(FontNames_Resolve(o, "No Such P Gothic"), "Test Prop Sans") == 0);
	if(!Typefaces_Installed(o, kMsPMincho)) // (no proportional Mincho yet: the fixed Mincho before the proportional gothic)
		CHECK(strcmp(FontNames_Resolve(o, kMsPMincho), "Test Serif") == 0);
	CHECK(FontNames_AddSubstitute(o, gPropMincho, (uint32_t)gPropMinchoSize, 3));
	if(!Typefaces_Installed(o, kMsPMincho))
		CHECK(strcmp(FontNames_Resolve(o, kMsPMincho), "Test Prop Roman") == 0);
	CHECK(strcmp(FontNames_Resolve(o, "No Such PMincho"), "Test Prop Roman") == 0);
	// the fixed faces keep theirs, and a name that says neither is a fixed gothic
	if(!Typefaces_Installed(o, kMsGothic))
		CHECK(strcmp(FontNames_Resolve(o, kMsGothic), "Test Gothic") == 0);
	if(!Typefaces_Installed(o, kMsMincho))
		CHECK(strcmp(FontNames_Resolve(o, kMsMincho), "Test Serif") == 0);
	CHECK(strcmp(FontNames_Resolve(o, "No Such Sans Serif"), "Test Gothic") == 0);
	CHECK(strcmp(FontNames_Resolve(o, "Plain Unknown"), "Test Gothic") == 0);
}

int main(void)
{
	OpenBGI_t* o = OpenBGI_New(OS_Host()); // the engine whose code the test runs
	if(!o)
		return 1;
	OS_Init();
	TestHost_SetEnv("BGI_FONT", ""); // no file of the user's in place of the faces
	TestHost_SetEnv("BGI_FONT_GOTHIC", "");
	TestHost_SetEnv("BGI_FONT_MINCHO", "");
	MakeFonts();
	printf("names\n");
	TestNames();
	if(!OS_Host()->fontRenderer)
	{
		printf("resolve\n");
		TestResolve(o);
	}
	if(gFails)
	{
		printf("fonts: %d checks failed\n", gFails);
		return 1;
	}
	printf("fonts: all tests passed\n");
	return 0;
}
