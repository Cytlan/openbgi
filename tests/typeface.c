/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/typeface.c - unit test of the fonts the engine draws with: its own
 *                    FreeType rasteriser (src/core/ftfont.c,
 *                    src/include/bgi/ftfont.h) and the typefaces over it or over a
 *                    host's renderer (src/gfx/typeface.c,
 *                    src/include/bgi/gfx/typeface.h); built as test_typeface by
 *                    `make test`
 *
 * The fonts are made by the test: small TrueType fonts whose glyphs are
 * rectangles on whole pixels at the sizes asked for, so that what FreeType
 * draws is known to the pixel.  The host's services are stand-ins that
 * hand out those fonts and note what they are asked.  The groups, in the
 * order they run (the first three need FreeType; a build without it says
 * so and leaves them out):
 *
 *   raster   - GDI's sizing: the cell, the baseline at the ascent, a
 *              full-width glyph twice the width asked for (the em stretched
 *              when the two disagree); the black-and-white and the 1-bit
 *              drawings alike; the advances; bold inks more; a character
 *              the face lacks drawn from the font the host finds for it, or
 *              as the face's box; no file, no font
 *   order    - a file the host forces wins over a font handed over as data,
 *              a font handed over wins over the host's match; family names
 *              in either case of ASCII letters
 *   engine   - the typefaces on the engine's rasteriser: a game's font as
 *              data, the host told of it; whether a face exists or is
 *              installed from the host's flags; the faces of a charset -
 *              the host's for the charset's language, in the text encoding,
 *              each once, the unspellable left out, then the game's font
 *              files ("B0 C2") that serve the charset's code page; a font
 *              file the host names by its path read through its files
 *   renderer - the typefaces on a host's renderer: every call goes to it
 *              with the engine's bytes, the fonts handed over as data are
 *              installed as well, a font file on disk goes by its path,
 *              a font of the engine's own by the renderer's call for
 *              those when it has one
 */
#include "bgi/ftfont.h"
#include "bgi/fontfile.h"
#include "bgi/gfx/typeface.h"
#include "bgi/core.h"
#include "host/os.h"
#include "host/os_common.h"

#include "testhost.h"

#include <stdio.h>

#include "check.h"

// ---- making fonts -------------------------------------------------------------------------

// a glyph: its character (0: .notdef), a rectangle in font units (none when x1 <= x0) and its advance
typedef struct TGlyph
{
	uint32_t cp;
	int x0, y0, x1, y1;
	int adv;
} TGlyph_t;

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

typedef struct Table
{
	const char* tag;
	uint8_t data[2048];
	size_t len;
} Table_t;

/* A TrueType font of 1000 units an em, ascent 800 and descent 200 (hhea
 * and OS/2 alike), with the glyphs given (sorted by character, .notdef
 * first) and the family `family`; the OS/2 table names the code pages
 * `codePages`.  The bytes written to `out`. */
static size_t MakeTtf(uint8_t* out, const char* family, const TGlyph_t* g, int n, uint32_t codePages)
{
	static Table_t t[9];
	size_t off, len;
	int i, k, segs = 0;
	uint32_t glyf = 0;
	memset(t, 0, sizeof t);
	t[0].tag = "OS/2";
	t[1].tag = "cmap";
	t[2].tag = "glyf";
	t[3].tag = "head";
	t[4].tag = "hhea";
	t[5].tag = "hmtx";
	t[6].tag = "loca";
	t[7].tag = "maxp";
	t[8].tag = "name";
	// OS/2, version 1: the cell and the code pages
	Be16(t[0].data, 1);
	Be16(t[0].data + 4, 400);
	Be16(t[0].data + 6, 5);
	Be16(t[0].data + 68, 800);
	Be16(t[0].data + 70, (uint32_t)-200 & 0xffff);
	Be16(t[0].data + 74, 800);
	Be16(t[0].data + 76, 200);
	Be32(t[0].data + 78, codePages);
	t[0].len = 86;
	// glyf and loca (long offsets), hmtx
	for(i = 0; i < n; i++)
	{
		Be32(t[6].data + i * 4, glyf);
		Be16(t[5].data + i * 4, (uint32_t)g[i].adv);
		Be16(t[5].data + i * 4 + 2, (uint32_t)(g[i].x1 > g[i].x0 ? g[i].x0 : 0) & 0xffff);
		if(g[i].x1 > g[i].x0)
		{ // one contour of four points on the curve, clockwise; coordinates as 16-bit deltas
			uint8_t* p = t[2].data + glyf;
			int xs[4], ys[4], px = 0, py = 0;
			xs[0] = g[i].x0, ys[0] = g[i].y0;
			xs[1] = g[i].x0, ys[1] = g[i].y1;
			xs[2] = g[i].x1, ys[2] = g[i].y1;
			xs[3] = g[i].x1, ys[3] = g[i].y0;
			Be16(p, 1);
			Be16(p + 2, (uint32_t)g[i].x0 & 0xffff);
			Be16(p + 4, (uint32_t)g[i].y0 & 0xffff);
			Be16(p + 6, (uint32_t)g[i].x1 & 0xffff);
			Be16(p + 8, (uint32_t)g[i].y1 & 0xffff);
			Be16(p + 10, 3); // the last point of the contour
			Be16(p + 12, 0); // no instructions
			for(k = 0; k < 4; k++)
				p[14 + k] = 0x01;
			for(k = 0; k < 4; k++)
			{
				Be16(p + 18 + k * 2, (uint32_t)(xs[k] - px) & 0xffff);
				px = xs[k];
			}
			for(k = 0; k < 4; k++)
			{
				Be16(p + 26 + k * 2, (uint32_t)(ys[k] - py) & 0xffff);
				py = ys[k];
			}
			glyf += 36; // 34 bytes, padded
		}
	}
	Be32(t[6].data + n * 4, glyf);
	t[2].len = glyf;
	t[5].len = (size_t)n * 4;
	t[6].len = (size_t)(n + 1) * 4;
	// head
	Be32(t[3].data, 0x00010000);
	Be32(t[3].data + 12, 0x5f0f3cf5);
	Be16(t[3].data + 16, 0x000b);
	Be16(t[3].data + 18, 1000);
	Be16(t[3].data + 38, (uint32_t)-200 & 0xffff);
	Be16(t[3].data + 40, 1000);
	Be16(t[3].data + 42, 800);
	Be16(t[3].data + 46, 8);
	Be16(t[3].data + 48, 2);
	Be16(t[3].data + 50, 1); // long loca
	t[3].len = 54;
	// hhea
	Be32(t[4].data, 0x00010000);
	Be16(t[4].data + 4, 800);
	Be16(t[4].data + 6, (uint32_t)-200 & 0xffff);
	Be16(t[4].data + 10, 1000);
	Be16(t[4].data + 18, 1);
	Be16(t[4].data + 34, (uint32_t)n);
	t[4].len = 36;
	// maxp 1.0
	Be32(t[7].data, 0x00010000);
	Be16(t[7].data + 4, (uint32_t)n);
	Be16(t[7].data + 6, 4);
	Be16(t[7].data + 8, 1);
	Be16(t[7].data + 14, 2);
	t[7].len = 32;
	// cmap: format 4, a segment per character and the closing one
	{
		uint8_t* c = t[1].data;
		uint8_t* s = c + 12;
		int segCount, range = 1, sel = 0;
		for(i = 0; i < n; i++)
			if(g[i].cp && g[i].cp < 0xffff)
				segs++;
		segCount = segs + 1;
		while(range * 2 <= segCount)
			range *= 2, sel++;
		Be16(c, 0);
		Be16(c + 2, 1);
		Be16(c + 4, 3);
		Be16(c + 6, 1);
		Be32(c + 8, 12);
		Be16(s, 4);
		Be16(s + 2, (uint32_t)(16 + segCount * 8));
		Be16(s + 6, (uint32_t)segCount * 2);
		Be16(s + 8, (uint32_t)range * 2);
		Be16(s + 10, (uint32_t)sel);
		Be16(s + 12, (uint32_t)(segCount * 2 - range * 2));
		for(i = 0, k = 0; i < n; i++)
		{
			if(!g[i].cp || g[i].cp >= 0xffff)
				continue;
			Be16(s + 14 + k * 2, g[i].cp);                                         // endCode
			Be16(s + 16 + segCount * 2 + k * 2, g[i].cp);                          // startCode
			Be16(s + 16 + segCount * 4 + k * 2, ((uint32_t)i - g[i].cp) & 0xffff); // idDelta
			Be16(s + 16 + segCount * 6 + k * 2, 0);                                // idRangeOffset
			k++;
		}
		Be16(s + 14 + k * 2, 0xffff);
		Be16(s + 16 + segCount * 2 + k * 2, 0xffff);
		Be16(s + 16 + segCount * 4 + k * 2, 1);
		t[1].len = 12 + 16 + (size_t)segCount * 8;
	}
	// name: the family, English (US), on the Windows platform
	{
		uint8_t* p = t[8].data;
		size_t fl = strlen(family);
		Be16(p, 0);
		Be16(p + 2, 1);
		Be16(p + 4, 18);
		Be16(p + 6, 3);
		Be16(p + 8, 1);
		Be16(p + 10, 0x0409);
		Be16(p + 12, 1);
		Be16(p + 14, (uint32_t)fl * 2);
		Be16(p + 16, 0);
		for(i = 0; i < (int)fl; i++)
			Be16(p + 18 + i * 2, (uint8_t)family[i]);
		t[8].len = 18 + fl * 2;
	}
	// the table directory, then the tables, each on four bytes
	Be32(out, 0x00010000);
	Be16(out + 4, 9);
	Be16(out + 6, 128);
	Be16(out + 8, 3);
	Be16(out + 10, 9 * 16 - 128);
	off = 12 + 9 * 16;
	for(i = 0; i < 9; i++)
	{
		uint8_t* r = out + 12 + i * 16;
		memcpy(r, t[i].tag, 4);
		Be32(r + 4, 0);
		Be32(r + 8, (uint32_t)off);
		Be32(r + 12, (uint32_t)t[i].len);
		memcpy(out + off, t[i].data, t[i].len);
		len = (t[i].len + 3) & ~(size_t)3;
		memset(out + off + t[i].len, 0, len - t[i].len);
		off += len;
	}
	return off;
}

// the main test font: 'A', a space, a box and "あ" (full width)
static const TGlyph_t kSans[] = {
	{0, 0, 0, 0, 0, 500},
	{0x20, 0, 0, 0, 0, 500},
	{0x41, 100, 0, 400, 700, 500},
	{0x25a1, 50, 0, 450, 500, 500},
	{0x3042, 100, -100, 900, 700, 1000},
};
// the host's own sans: the same but for a narrower 'A'
static const TGlyph_t kSans2[] = {
	{0, 0, 0, 0, 0, 500},
	{0x20, 0, 0, 0, 0, 500},
	{0x41, 200, 0, 400, 700, 500},
	{0x25a1, 50, 0, 450, 500, 500},
	{0x3042, 100, -100, 900, 700, 1000},
};
// the font the host finds for Hebrew: an alef
static const TGlyph_t kHebrew[] = {
	{0, 0, 0, 0, 0, 500},
	{0x5d0, 0, 0, 500, 500, 600},
};

static uint8_t gSans[4096], gSans2[4096], gHebrew[4096];
static size_t gSansSize, gSans2Size, gHebrewSize;

static void MakeFonts(void)
{
	gSansSize = MakeTtf(gSans, "Test Sans", kSans, (int)BGI_COUNTOF(kSans), 1u << 17);                    // Japanese
	gSans2Size = MakeTtf(gSans2, "Other Sans", kSans2, (int)BGI_COUNTOF(kSans2), (1u << 0) | (1u << 18)); // Latin 1, simplified Chinese
	gHebrewSize = MakeTtf(gHebrew, "Test Hebrew", kHebrew, (int)BGI_COUNTOF(kHebrew), 1u << 5);
}

// ---- the host's stand-ins ----------------------------------------------------------------

static struct
{
	int forced; // find: hand out the second font, forced
	int found;  // find: hand out the second font as the match
	int findCalls, charCalls;
	uint32_t lastChar;    // what findChar was asked
	int familyFlags;      // what family answers
	char lastLang[16];    // what families was asked ("-" for NULL)
	const char* names[8]; // what families lists
	int added;            // fonts the engine handed over
	uint32_t addedSize;
	const char* path; // find: hand out this file by its path instead (with `found`)
} gHost;

static int FakeFind(void* user, const char* family, int serif, int bold, int italic, OpenBGI_FontFile_t* out)
{
	(void)user, (void)family, (void)serif, (void)bold, (void)italic;
	gHost.findCalls++;
	if(!gHost.forced && !gHost.found)
		return 0;
	if(gHost.path)
	{
		snprintf(out->path, sizeof out->path, "%s", gHost.path);
		out->data = NULL;
		out->size = 0;
		out->index = 0;
		return OPENBGI_FONT_FOUND;
	}
	out->path[0] = 0;
	out->data = gSans2;
	out->size = (uint32_t)gSans2Size;
	out->index = 0;
	return gHost.forced ? OPENBGI_FONT_FORCED : OPENBGI_FONT_FOUND;
}

static int FakeFindChar(void* user, uint32_t cp, int serif, int bold, OpenBGI_FontFile_t* out)
{
	(void)user, (void)serif, (void)bold;
	gHost.charCalls++;
	gHost.lastChar = cp;
	if(cp != 0x5d0)
		return 0;
	out->path[0] = 0;
	out->data = gHebrew;
	out->size = (uint32_t)gHebrewSize;
	out->index = 0;
	return OPENBGI_FONT_FOUND;
}

static int FakeFamily(void* user, const char* family)
{
	(void)user, (void)family;
	return gHost.familyFlags;
}

static void FakeFamilies(void* user, const char* lang, void (*each)(void* ctx, const char* family), void* ctx)
{
	int i;
	(void)user;
	snprintf(gHost.lastLang, sizeof gHost.lastLang, "%s", lang ? lang : "-");
	for(i = 0; i < 8 && gHost.names[i]; i++)
		each(ctx, gHost.names[i]);
}

static void FakeAdded(void* user, const void* data, uint32_t size)
{
	(void)user, (void)data;
	gHost.added++;
	gHost.addedSize = size;
}

static const OpenBGI_FontFiles_t kFiles = {sizeof(OpenBGI_FontFiles_t), FakeFind, FakeFindChar, FakeFamily, FakeFamilies, FakeAdded};

static uint32_t HostTicksMs(void* user)
{
	(void)user;
	return 0;
}

static int HostPerfNs(void* user, int64_t* out)
{
	(void)user;
	*out = 0;
	return 1;
}

static void HostLocalTime(void* user, OpenBGI_DateTime_t* out)
{
	(void)user;
	memset(out, 0, sizeof *out);
}

// ---- looking at a drawing -----------------------------------------------------------------

// the rectangle of the ink in a w x h buffer (pitch bytes a row): 1 and its first and last columns and rows, 0 when blank
static int Ink(const uint8_t* buf, int pitch, int w, int h, int* x0, int* y0, int* x1, int* y1)
{
	int x, y, any = 0;
	*x0 = w, *y0 = h, *x1 = -1, *y1 = -1;
	for(y = 0; y < h; y++)
		for(x = 0; x < w; x++)
		{
			uint8_t v = buf[y * pitch + x];
			if(v != 0 && v != 0xff)
				return 0; // black and white only
			if(v)
			{
				any = 1;
				*x0 = x < *x0 ? x : *x0;
				*x1 = x > *x1 ? x : *x1;
				*y0 = y < *y0 ? y : *y0;
				*y1 = y > *y1 ? y : *y1;
			}
		}
	return any;
}

// the inked pixels of a w x h buffer
static int InkCount(const uint8_t* buf, int pitch, int w, int h)
{
	int x, y, n = 0;
	for(y = 0; y < h; y++)
		for(x = 0; x < w; x++)
			n += buf[y * pitch + x] != 0;
	return n;
}

// ---- the groups -----------------------------------------------------------------------------

static void TestRaster(void)
{
	FtFonts_t* l = FtFonts_New(NULL, &kFiles, NULL);
	FtFont_t* f;
	uint8_t buf[40 * 40], bits[4 * 40];
	int x0, y0, x1, y1, x, y, same;
	FtGlyph_t g;
	CHECK(l != NULL);
	if(!l)
		return;
	CHECK(FtFonts_AddMemory(l, gSans, (uint32_t)gSansSize, 0));
	CHECK(!FtFonts_AddMemory(l, "not a font", 10, 1));
	CHECK(FtFonts_HasFamily(l, "Test Sans") && FtFonts_HasFamily(l, "TEST SANS") && !FtFonts_HasFamily(l, "Test"));

	// 20 pixels a cell, 10 a half-width character: 50 units a pixel, the baseline on row 16
	f = FtFont_Open(l, "Test Sans", 20, 10, 0, 0);
	CHECK(f != NULL);
	if(!f)
	{
		FtFonts_Delete(l);
		return;
	}
	CHECK(FtFont_Height(f) == 20);
	memset(buf, 0x55, sizeof buf); // cleared by the drawing
	FtFont_Render(f, 'A', buf, 40, 20, 20);
	CHECK(Ink(buf, 40, 20, 20, &x0, &y0, &x1, &y1) && x0 == 2 && x1 == 7 && y0 == 2 && y1 == 15);
	FtFont_Render(f, 0x3042, buf, 40, 40, 20); // full width: 16 pixels from column 2, below the baseline too
	CHECK(Ink(buf, 40, 40, 20, &x0, &y0, &x1, &y1) && x0 == 2 && x1 == 17 && y0 == 2 && y1 == 17);
	memset(buf, 0x55, sizeof buf);
	FtFont_Render(f, 'A', buf, 40, 5, 4); // clipped to 5 x 4
	CHECK(Ink(buf, 40, 5, 4, &x0, &y0, &x1, &y1) && x0 == 2 && x1 == 4 && y0 == 2 && y1 == 3);
	CHECK(buf[5] == 0x55); // nothing written past the width
	// the 1-bit drawing is the same picture
	FtFont_Render(f, 'A', buf, 40, 20, 20);
	FtFont_RenderMono(f, 'A', bits, 4, 20, 20);
	same = 1;
	for(y = 0; y < 20; y++)
		for(x = 0; x < 20; x++)
			if(((bits[y * 4 + (x >> 3)] >> (7 - (x & 7))) & 1) != (buf[y * 40 + x] != 0))
				same = 0;
	CHECK(same);
	// the advances
	CHECK(FtFont_Advance(f, 'A') == 10 && FtFont_Advance(f, 0x3042) == 20);
	CHECK(FtFont_FaceAdvance(f, 'A') == 10 * 64);
	CHECK(FtFont_Glyph(f, 'A', 1, &g) && g.x == 2 && g.y == 2 && g.width == 6 && g.rows == 14 && g.advance == 640 && !g.mono);
	// the space draws nothing, a character the face lacks is the face's box (no font has it)
	FtFont_Render(f, ' ', buf, 40, 20, 20);
	CHECK(InkCount(buf, 40, 20, 20) == 0);
	FtFont_Render(f, 0x4e00, buf, 40, 20, 20);
	CHECK(Ink(buf, 40, 20, 20, &x0, &y0, &x1, &y1) && x0 == 1 && x1 == 8 && y0 == 6 && y1 == 15);
	CHECK(gHost.lastChar == 0x4e00);
	// Hebrew from the font the host finds, on the same baseline and scale
	FtFont_Render(f, 0x5d0, buf, 40, 20, 20);
	CHECK(Ink(buf, 40, 20, 20, &x0, &y0, &x1, &y1) && x0 == 0 && x1 == 9 && y0 == 6 && y1 == 15);
	CHECK(FtFont_Advance(f, 0x5d0) == 12);
	CHECK(FtFont_FaceAdvance(f, 0x5d0) == 500 * 64 / 50); // (the face's own .notdef: no stand-in here)
	FtFont_Close(f);

	// half as wide: the em stretched to a full width of 10 pixels
	f = FtFont_Open(l, "Test Sans", 20, 5, 0, 0);
	FtFont_Render(f, 'A', buf, 40, 20, 20);
	CHECK(Ink(buf, 40, 20, 20, &x0, &y0, &x1, &y1) && x0 == 1 && x1 == 3 && y0 == 2 && y1 == 15);
	CHECK(FtFont_Advance(f, 'A') == 5);
	FtFont_Close(f);

	// bold where the face is not: more ink
	f = FtFont_Open(l, "Test Sans", 20, 10, 0, 0);
	FtFont_Render(f, 'A', buf, 40, 20, 20);
	x = InkCount(buf, 40, 20, 20);
	FtFont_Close(f);
	f = FtFont_Open(l, "Test Sans", 20, 10, 1, 0);
	FtFont_Render(f, 'A', buf, 40, 20, 20);
	CHECK(InkCount(buf, 40, 20, 20) > x);
	FtFont_Close(f);

	CHECK(FtFont_Open(l, "Test Sans", 0, 10, 0, 0) == NULL);     // no height
	CHECK(FtFont_Open(l, "No Such Font", 20, 10, 0, 0) == NULL); // the host has nothing either
	f = FtFont_Open(l, "Test Sans", 20, 10, 0, 0);
	FtFonts_Delete(l); // with a font still open
}

static void TestOrder(void)
{
	FtFonts_t* l = FtFonts_New(NULL, &kFiles, NULL);
	FtFont_t* f;
	uint8_t buf[40 * 20];
	int x0, y0, x1, y1;
	if(!l)
		return;
	FtFonts_AddMemory(l, gSans, (uint32_t)gSansSize, 1);
	// the host's match ("Other Sans": its 'A' from column 4) does not beat the font handed over, but stands in for an unknown family
	gHost.found = 1;
	f = FtFont_Open(l, "test sans", 20, 10, 0, 0);
	FtFont_Render(f, 'A', buf, 40, 20, 20);
	CHECK(f && gHost.findCalls > 0 && Ink(buf, 40, 20, 20, &x0, &y0, &x1, &y1) && x0 == 2 && x1 == 7);
	FtFont_Close(f);
	f = FtFont_Open(l, "Unknown", 20, 10, 0, 0);
	FtFont_Render(f, 'A', buf, 40, 20, 20);
	CHECK(f && Ink(buf, 40, 20, 20, &x0, &y0, &x1, &y1) && x0 == 4 && x1 == 7);
	FtFont_Close(f);
	gHost.found = 0;
	// a forced file comes first
	gHost.forced = 1;
	f = FtFont_Open(l, "Test Sans", 20, 10, 0, 0);
	FtFont_Render(f, 'A', buf, 40, 20, 20);
	CHECK(f && Ink(buf, 40, 20, 20, &x0, &y0, &x1, &y1) && x0 == 4 && x1 == 7);
	FtFont_Close(f);
	gHost.forced = 0;
	FtFonts_Delete(l);
}

static OpenBGI_Host_t gEngineHost = {.size = sizeof(OpenBGI_Host_t), .api = OPENBGI_API_VERSION, .ticksMs = HostTicksMs, .perfNs = HostPerfNs, .localTime = HostLocalTime, .fontFiles = &kFiles, .caps = OPENBGI_CAP_DIALOGS};

static char gDir[0x400], gCwd[0x400]; // the scratch game directory, the directory the test began in

// the engine's host's files: the OS layer's, its native opens counted
static OpenBGI_Files_t gFiles;
static int gNativeOpens;

static void* CountedNativeOpen(void* user, const char* path, int mode)
{
	gNativeOpens++;
	return OsCommon_Files()->nativeOpen(user, path, mode);
}

// "font.ttf" (gSans, Japanese) and "zh.ttf" (gSans2, Latin and simplified Chinese) in a new game directory
static void MakeGameDir(void)
{
	char path[0x800];
	TestHost_ScratchName(gDir, sizeof gDir, "openbgi_typeface");
	TestHost_Mkdir(gDir);
	snprintf(path, sizeof path, "%s/font.ttf", gDir);
	OS_WriteNativeFile(path, gSans, (uint32_t)gSansSize);
	snprintf(path, sizeof path, "%s/zh.ttf", gDir);
	OS_WriteNativeFile(path, gSans2, (uint32_t)gSans2Size);
	OS_NativeAbsolute(".", gCwd, sizeof gCwd);
	OS_SetGameDir(gDir);
}

// the names a listing wrote, joined with '|'
static void Joined(const char* buf, int count, char* out, size_t n)
{
	int i;
	out[0] = 0;
	for(i = 0; i < count; i++)
	{
		snprintf(out + strlen(out), n - strlen(out), "%s%s", i ? "|" : "", buf);
		buf += strlen(buf) + 1;
	}
}

static void TestEngine(void)
{
	OpenBGI_t* o;
	Typeface_t* t;
	uint8_t buf[40 * 20];
	char names[0x200], list[0x200];
	int x0, y0, x1, y1, bytes, n;
	gFiles = *OsCommon_Files();
	gFiles.nativeOpen = CountedNativeOpen;
	gEngineHost.files = &gFiles;
	o = OpenBGI_New(&gEngineHost);
	if(!o)
		return;
	// a game's font as data: the host hears of it, its family is installed
	gHost.familyFlags = 0;
	CHECK(!Typefaces_Installed(o, "Test Sans") && !Typefaces_Exists(o, "Test Sans"));
	CHECK(Typefaces_AddMemory(o, gSans, (uint32_t)gSansSize, 0));
	CHECK(gHost.added == 1 && gHost.addedSize == gSansSize);
	CHECK(Typefaces_Installed(o, "Test Sans") && Typefaces_Exists(o, "TEST SANS"));
	CHECK(!Typefaces_AddMemory(o, "not a font", 10, 0) && gHost.added == 1);
	// drawn through the engine's rasteriser, the character in Shift-JIS
	t = Typeface_Open(o, "Test Sans", 20, 10, 0, 0);
	CHECK(t != NULL);
	Typeface_Render(t, "A", 1, buf, 40, 20, 20);
	CHECK(Ink(buf, 40, 20, 20, &x0, &y0, &x1, &y1) && x0 == 2 && x1 == 7 && y0 == 2 && y1 == 15);
	Typeface_Render(t, "\x82\xa0", 2, buf, 40, 40, 20); // あ
	CHECK(Ink(buf, 40, 40, 20, &x0, &y0, &x1, &y1) && x0 == 2 && x1 == 17);
	CHECK(Typeface_Advance(t, "A", 1) == 10 && Typeface_Advance(t, "\x82\xa0", 2) == 20);
	Typeface_Close(t);
	t = Typeface_OpenMono(o, "Test Sans", 20, 0); // size x size / 2
	CHECK(t != NULL && Typeface_Advance(t, "A", 1) == 10);
	// left open: OpenBGI_Delete closes it

	// what the host knows of a family
	gHost.familyFlags = OPENBGI_FONT_FOUND;
	CHECK(Typefaces_Exists(o, "MS Gothic") && Typefaces_Installed(o, "MS Gothic"));
	gHost.familyFlags = OPENBGI_FONT_FORCED; // a file of the user's in its place: installed, but not there
	CHECK(!Typefaces_Exists(o, "MS Gothic") && Typefaces_Installed(o, "MS Gothic"));
	gHost.familyFlags = OPENBGI_FONT_UNKNOWN; // it cannot tell: there for the scripts, not for the substitutes
	CHECK(Typefaces_Exists(o, "MS Gothic") && !Typefaces_Installed(o, "MS Gothic"));
	gHost.familyFlags = 0;
	CHECK(!Typefaces_Exists(o, "MS Gothic") && !Typefaces_Installed(o, "MS Gothic"));

	// the faces of a charset: the host's for the language, in Shift-JIS, each once, the unspellable left out
	gHost.names[0] = "Noto Sans CJK JP";
	gHost.names[1] = "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88"; // テスト
	gHost.names[2] = "Noto Sans CJK JP";                     // another style of the same family
	gHost.names[3] = "Fran\xc3\xa7ois";                      // code page 932 has no ç
	gHost.names[4] = NULL;
	n = Typefaces_Enum(o, NULL, 0x80, &bytes);
	CHECK(n == 2 && bytes == 17 + 7 && strcmp(gHost.lastLang, "ja") == 0);
	memset(names, 0x7f, sizeof names);
	CHECK(Typefaces_Enum(o, names, 0x80, &bytes) == 2 && bytes == 24);
	Joined(names, 2, list, sizeof list);
	CHECK(strcmp(list, "Noto Sans CJK JP|\x83\x65\x83\x58\x83\x67") == 0);
	Typefaces_Enum(o, NULL, 0x86, &bytes);
	CHECK(strcmp(gHost.lastLang, "zh-cn") == 0);
	Typefaces_Enum(o, NULL, 0x88, &bytes);
	CHECK(strcmp(gHost.lastLang, "zh-tw") == 0);
	Typefaces_Enum(o, NULL, 0x81, &bytes);
	CHECK(strcmp(gHost.lastLang, "ko") == 0);
	Typefaces_Enum(o, NULL, 0, &bytes);
	CHECK(strcmp(gHost.lastLang, "-") == 0);
	// in UTF-8 text mode the names as they are
	o->enc.textUtf8 = 1;
	n = Typefaces_Enum(o, names, 0x80, &bytes);
	Joined(names, n, list, sizeof list);
	CHECK(n == 3 && strcmp(list, "Noto Sans CJK JP|\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88|Fran\xc3\xa7ois") == 0);
	o->enc.textUtf8 = 0;

	// the game's font files ("B0 C2") are listed under the charsets of their code pages, the data ("B0 C3") not
	MakeGameDir();
	CHECK(Typefaces_AddFile(o, "font.ttf") && Typefaces_AddFile(o, "zh.ttf"));
	CHECK(!Typefaces_AddFile(o, "missing.ttf"));
	CHECK(Typefaces_Installed(o, "Other Sans") && gHost.added == 3);
	gHost.names[0] = NULL;
	n = Typefaces_Enum(o, names, 0x80, &bytes);
	Joined(names, n, list, sizeof list);
	CHECK(n == 1 && strcmp(list, "Test Sans") == 0);
	n = Typefaces_Enum(o, names, 0x86, &bytes);
	Joined(names, n, list, sizeof list);
	CHECK(n == 1 && strcmp(list, "Other Sans") == 0);
	n = Typefaces_Enum(o, names, 0, &bytes);
	Joined(names, n, list, sizeof list);
	CHECK(n == 1 && strcmp(list, "Other Sans") == 0);
	CHECK(Typefaces_Enum(o, names, 0x88, &bytes) == 0 && bytes == 0);

	// a font file the host names by its path is read through the host's files
	{
		char path[0x800];
		int opens = gNativeOpens;
		snprintf(path, sizeof path, "%s/zh.ttf", gDir);
		gHost.path = path;
		gHost.found = 1;
		t = Typeface_Open(o, "Some Face", 20, 10, 0, 0);
		CHECK(t != NULL && gNativeOpens == opens + 1);
		if(t)
		{
			Typeface_Render(t, "A", 1, buf, 40, 20, 20);
			CHECK(Ink(buf, 40, 20, 20, &x0, &y0, &x1, &y1));
			Typeface_Close(t);
		}
		gHost.path = NULL;
		gHost.found = 0;
	}
	OpenBGI_Delete(o);
}

// ---- a renderer of the host's own ------------------------------------------------------------

static struct
{
	int opens, monos, closes, renders, monoRenders, advances, adds, substitutes, files, exists, installed, enums;
	char face[32], ch[8], path[0x400];
	int height, width, bold, italic, len;
} gR;

static int gFont = 7; // the renderer's handle

static void* ROpen(void* user, const char* face, int height, int width, int bold, int italic)
{
	(void)user;
	gR.opens++;
	snprintf(gR.face, sizeof gR.face, "%s", face);
	gR.height = height, gR.width = width, gR.bold = bold, gR.italic = italic;
	return strcmp(face, "None") == 0 ? NULL : &gFont;
}

static void* ROpenMono(void* user, const char* face, int size, int bold)
{
	(void)user;
	gR.monos++;
	snprintf(gR.face, sizeof gR.face, "%s", face);
	gR.height = size, gR.bold = bold;
	return &gFont;
}

static void RClose(void* user, void* font)
{
	(void)user;
	gR.closes += font == &gFont;
}

static void RRender(void* user, void* font, const char* ch, int len, uint8_t* buf, int pitch, int w, int h)
{
	(void)user, (void)pitch, (void)w, (void)h;
	gR.renders += font == &gFont;
	memcpy(gR.ch, ch, (size_t)len);
	gR.len = len;
	buf[0] = 0xff;
}

static void RRenderMono(void* user, void* font, const char* ch, int len, uint8_t* bits, int pitch, int w, int h)
{
	(void)user, (void)ch, (void)len, (void)pitch, (void)w, (void)h;
	gR.monoRenders += font == &gFont;
	bits[0] = 0x80;
}

static int RAdvance(void* user, void* font, const char* ch, int len)
{
	(void)user, (void)ch;
	gR.advances += font == &gFont;
	return len * 11;
}

static int RAddFile(void* user, const char* path)
{
	(void)user;
	gR.files++;
	snprintf(gR.path, sizeof gR.path, "%s", path);
	return 1;
}

static int RAddMemory(void* user, const void* data, uint32_t size)
{
	(void)user, (void)data, (void)size;
	gR.adds++;
	return 1;
}

static int RAddSubstitute(void* user, const void* data, uint32_t size)
{
	(void)user, (void)data, (void)size;
	gR.substitutes++;
	return 1;
}

static int RFaceExists(void* user, const char* face)
{
	(void)user;
	gR.exists++;
	return strcmp(face, "Here") == 0;
}

static int RFaceInstalled(void* user, const char* face)
{
	(void)user;
	gR.installed++;
	return strcmp(face, "Here") == 0;
}

static int REnumFaces(void* user, char* buf, int charset, int* bytes)
{
	(void)user;
	gR.enums++;
	if(buf)
		memcpy(buf, "Here", 5);
	*bytes = charset == 0x80 ? 5 : 0;
	return charset == 0x80;
}

static const OpenBGI_FontRenderer_t kRenderer = {sizeof(OpenBGI_FontRenderer_t), ROpen, ROpenMono, RClose, RRender, RRenderMono, RAdvance, RAddFile,
	RAddMemory, RFaceExists, RFaceInstalled, REnumFaces, RAddSubstitute};
// one that gives every font its own proportions: no call of its own for the engine's fonts
static const OpenBGI_FontRenderer_t kPlainRenderer = {sizeof(OpenBGI_FontRenderer_t), ROpen, ROpenMono, RClose, RRender, RRenderMono, RAdvance, RAddFile,
	RAddMemory, RFaceExists, RFaceInstalled, REnumFaces, NULL};

static OpenBGI_Host_t gRendererHost = {.size = sizeof(OpenBGI_Host_t), .api = OPENBGI_API_VERSION, .ticksMs = HostTicksMs, .perfNs = HostPerfNs, .localTime = HostLocalTime, .fontFiles = &kFiles, .fontRenderer = &kRenderer, .caps = OPENBGI_CAP_DIALOGS};

static void TestRenderer(void)
{
	OpenBGI_t* o;
	Typeface_t* t;
	uint8_t buf[16], bits[4];
	char names[16];
	int bytes, added = gHost.added;
	gRendererHost.files = OsCommon_Files();
	o = OpenBGI_New(&gRendererHost);
	if(!o)
		return;
	t = Typeface_Open(o, "\x82\x6c\x82\x72", 32, 16, 1, 1); // the engine's bytes go as they are
	CHECK(t != NULL && gR.opens == 1 && strcmp(gR.face, "\x82\x6c\x82\x72") == 0);
	CHECK(gR.height == 32 && gR.width == 16 && gR.bold && gR.italic);
	Typeface_Render(t, "\x82\xa0", 2, buf, 4, 4, 4);
	CHECK(gR.renders == 1 && gR.len == 2 && memcmp(gR.ch, "\x82\xa0", 2) == 0 && buf[0] == 0xff);
	Typeface_RenderMono(t, "A", 1, bits, 1, 8, 4);
	CHECK(gR.monoRenders == 1 && bits[0] == 0x80);
	CHECK(Typeface_Advance(t, "\x82\xa0", 2) == 22 && gR.advances == 1);
	Typeface_Close(t);
	CHECK(gR.closes == 1);
	CHECK(Typeface_Open(o, "None", 32, 16, 0, 0) == NULL);
	t = Typeface_OpenMono(o, "Mono", 12, 1);
	CHECK(t != NULL && gR.monos == 1 && gR.height == 12 && gR.bold);
	// the questions go to the renderer; a font handed over as data is installed besides
	CHECK(Typefaces_Exists(o, "Here") && !Typefaces_Exists(o, "Gone") && gR.exists == 2);
	CHECK(Typefaces_Installed(o, "Here") && !Typefaces_Installed(o, "Test Sans"));
	CHECK(Typefaces_AddMemory(o, gSans, (uint32_t)gSansSize, 1) && gR.adds == 1);
	CHECK(Typefaces_Installed(o, "Test Sans") && gHost.added == added); // (the host's font files are not told)
	// a font of the engine's own goes to the renderer as one, where the renderer tells them apart
	CHECK(Typefaces_AddSubstitute(o, gSans2, (uint32_t)gSans2Size) && gR.substitutes == 1 && gR.adds == 1);
	CHECK(Typefaces_Enum(o, names, 0x80, &bytes) == 1 && bytes == 5 && strcmp(names, "Here") == 0 && gR.enums == 1);
	// a font file on disk goes by its path, one only an archive holds from memory
	CHECK(Typefaces_AddFile(o, "font.ttf") && gR.files == 1 && strcmp(gR.path, "font.ttf") == 0);
	CHECK(!Typefaces_AddFile(o, "missing.ttf") && gR.files == 1 && gR.adds == 1);
	OpenBGI_Delete(o); // (closes the mono font)
	CHECK(gR.closes == 2);
	// and as any font from memory where it does not
	gRendererHost.fontRenderer = &kPlainRenderer;
	o = OpenBGI_New(&gRendererHost);
	gRendererHost.fontRenderer = &kRenderer;
	if(!o)
		return;
	CHECK(Typefaces_AddSubstitute(o, gSans2, (uint32_t)gSans2Size) && gR.substitutes == 1 && gR.adds == 2);
	CHECK(Typefaces_Installed(o, "Other Sans"));
	OpenBGI_Delete(o);
}

int main(void)
{
	FtFonts_t* probe;
	OS_Init();
	MakeFonts();
	probe = FtFonts_New(NULL, NULL, NULL);
	if(probe)
	{
		FtFonts_Delete(probe);
		printf("raster\n");
		TestRaster();
		printf("order\n");
		TestOrder();
		printf("engine\n");
		TestEngine();
	}
	else
	{
		printf("(no FreeType in this build: raster, order and engine left out)\n");
		MakeGameDir();
	}
	printf("renderer\n");
	TestRenderer();
	if(gDir[0])
	{
		TestHost_Chdir(gCwd);
		TestHost_RemoveTree(gDir);
	}
	if(gFails)
	{
		printf("typeface: %d checks failed\n", gFails);
		return 1;
	}
	printf("typeface: all tests passed\n");
	return 0;
}
