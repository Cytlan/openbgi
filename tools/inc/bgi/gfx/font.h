/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * font.h - the glyph rasteriser with its glyph cache, the cache of open
 * fonts, the glyph painting helpers of the bitmap manager, and the 1-bit
 * font of the "92 1E" text instruction (src/gfx/font.c, src/gfx/monofont.c).
 *
 * A FontRaster is one open font: it renders a character at an oversampled
 * size through the OS layer, box-filters it down to a (2*size) x size cell
 * of 8-bit coverage and keeps the most recently used glyphs in a small LRU
 * cache.  The FontCache owns the open rasterisers (one per
 * face/size/width/weight, identified by a small integer handle) together
 * with the "pre-build amount" requests of instruction "90 0E".
 *
 * The text engine proper (layout, ruby, message windows) is in
 * src/gfx/text/; this file is what it draws with.
 */
#ifndef BGI_GFX_FONT_H_
#define BGI_GFX_FONT_H_

#include "bgi/gfx/bitmap.h"
#include "bgi/os.h"

// ---- one cached glyph -----------------------------------------------------
typedef struct Glyph
{
	int32_t code;    // Shift-JIS code, 1 or 2 bytes
	int32_t dbcs;    // code >= 0x100
	uint8_t* pixels; // coverage cell, (2*size) x size bytes
	int32_t left;    // first inked column
	int32_t top;     // always 0
	int32_t right;   // last inked column
	int32_t bottom;  // size - 1
} Glyph_t;

// ---- antialiasing (instruction "90 0D") -----------------------------------
extern int gAaShift;             // sum of the oversampled block >> this
extern int gAaScale;             // oversampling factor per axis
extern int gAaRowShift;          // log2(gAaScale)
int Gfx_SetAntialias(int level); // 0..3; 1 when applied

// ---- the rasteriser: one open font ----------------------------------------
typedef struct GlyphSlot // one entry of the glyph cache
{
	Glyph_t g;
	struct GlyphSlot* next; // the next less recently used glyph
} GlyphSlot_t;

typedef struct FontRaster
{
	int open;
	int size;     // cell height in pixels
	int widthPct; // width in percent of the size
	int bold;
	int italic;         // 1.494 on: the lfItalic flag; the DIB is a quarter wider for the shear
	int renderW;        // columns of the cell the DIB reduces to (glyphW, or 5/4 of it when italic)
	int extraW[2];      // 1.529 on ("91 0F"): extra pixels per fixed-pitch advance, [0] upright, [1] italic
	int ssW, ssH;       // the oversampled glyph size in pixels
	int ssPitch;        // row pitch of the oversampled buffer, bytes
	int ssBytes;        // its size
	uint8_t* pool;      // all glyph cells, capacity * cellBytes
	int cellW;          // 2 * size
	int cellBytes;      // cellW * size
	int glyphW;         // size * widthPct / 100
	GlyphSlot_t* lru;   // most recently used glyph
	GlyphSlot_t* slots; // capacity entries
	int used;           // entries rendered so far
	int capacity;       // glyphs the cache holds
	uint8_t* ss;        // oversampled 8-bit buffer (the DIB bits)
	OsFont_t* os;       // the OS font and its device context
} FontRaster_t;

FontRaster_t* FontRaster_New(void);
void FontRaster_Delete(FontRaster_t* f);
/* 0 ok, 0x80000001 capacity < 2, 0x80000002 size not 4..200,
 * 0x80000003 width not 25..200, 0x80000004 the font could not be created.
 * `italic` is the lfItalic of the 1.494+ builds (the inline <i> markup and
 * the link font); the earlier builds never set it. */
int FontRaster_Init(FontRaster_t* f, const char* face, int size, int widthPct, int bold, int italic, int capacity);
void FontRaster_Close(FontRaster_t* f);
// fetch (render if needed) the glyph of a Shift-JIS code; *out is a copy, the result points to it
const Glyph_t* FontRaster_Glyph(FontRaster_t* f, Glyph_t* out, int code);
/* OpenBGI extension: the font's own advance of a character in pixels,
 * rounded down (so that letters which join overlap rather than gap); the
 * original places every glyph by its cell or its ink and never asks.  0
 * for a character the font cannot measure. */
int FontRaster_Advance(FontRaster_t* f, int code);

// ---- the cache of open fonts (owned by the bitmap manager) -----------------
typedef struct FontReq // a pre-build request of "90 0E"
{
	char name[0x20]; // face name
	int size, widthPct, bold, unused;
	int amount; // glyphs to cache for that font
	struct FontReq* next;
} FontReq_t;

typedef struct FontEntry // an open font
{
	int handle;      // 1, 2,...
	char name[0x20]; // face name
	int size, widthPct, bold;
	FontRaster_t* font;
	struct FontEntry* next;
} FontEntry_t;

typedef struct FontInfo // what FontCache_GetInfo copies out of an entry
{
	char name[0x20];
	int size, widthPct, bold;
	int italic; // 1.494 on: 0 for a cache font, set by the layout's temporary fonts
	FontRaster_t* font;
} FontInfo_t;

typedef struct FontCache
{
	int defaultAmount;    // glyphs cached for a font without a request: 0x40
	FontReq_t* requests;  // the "90 0E" requests
	int lastHandle;       // the handle before the first entry's (0)
	FontEntry_t* entries; // the open fonts, in opening order
} FontCache_t;

FontCache_t* FontCache_New(void);
void FontCache_Delete(FontCache_t* c);
/* "90 0E": remember how many glyphs to cache for a font.
 * 0 ok, 0x80000001 amount < 2, 0x80000002 bad size, 0x80000003 bad width */
int FontCache_Request(FontCache_t* c, const char* name, int size, int widthPct, int bold, int amount);
// the requested amount for a font, or the default
int FontCache_AmountFor(FontCache_t* c, const char* name, int size, int widthPct, int bold);
/* open (or find) a font; *handle receives its number. 0 ok or
 * the FontRaster_Init error */
int FontCache_Open(FontCache_t* c, int* handle, const char* name, int size, int widthPct, int bold);
int FontCache_GetInfo(FontCache_t* c, FontInfo_t* out, int handle); // 1 found
/* "91 0F" of 1.529 on: open the font and store the two extra-width
 * values on its rasteriser (extraW); the FontCache_Open result */
int FontCache_OpenExtra(FontCache_t* c, int* handle, const char* name, int size, int widthPct, int bold, int extraUpright,
	int extraItalic);

// ---- drawing helpers of the bitmap manager ---------------------------------
/* paint the glyph into a (2*size) x size view in the given colour
 * ARGB32 gets colour + coverage alpha, RGB16 the scaled colour, RGB32 a
 * 0/1 mask */
void Font_PaintGlyph(Bmp_t* cell, Glyph_t* g, int code, FontRaster_t* font, uint32_t colour);
int Font_GapFor(const Glyph_t* g, int size);    // proportional gap (the bitmap manager's text, the plain output)
int Font_GapFor2(const Glyph_t* g, int size);   // proportional gap (the rich layout and Text_Measure)
int Font_Percent(int a, int pct);               // a * pct / 100
void Bmp_CropGlyph(Bmp_t* b, const Glyph_t* g); // narrow the view to the columns left..right

// ---- the 1-bit font of the "92 1E" text instruction (src/gfx/monofont.c) ----
typedef struct MonoGlyph // one cached glyph
{
	int32_t code;           // Shift-JIS code
	int32_t dbcs;           // code >= 0x100
	uint8_t* bits;          // size rows of bpr bytes, MSB first
	Rect_t rect;            // inked columns (padded), rows 0.. size-1
	struct MonoGlyph* next; // most-recently-used list
} MonoGlyph_t;

typedef struct MonoFont
{
	int open;
	int size;           // cell size in pixels (square)
	uint8_t* pool;      // capacity glyph cells
	uint8_t* spare;     // allocated, never used
	int bpr;            // bytes per glyph row: (size + 7) / 8
	int bytesPerGlyph;  // bpr * size
	MonoGlyph_t* lru;   // most recently used glyph
	MonoGlyph_t* slots; // capacity entries
	int used;           // entries rendered so far
	int capacity;
	void* fixed;    // a list of 0x1C-byte entries searched first; stays empty
	int fixedCount; // always 0
	OsFont_t* os;   // the OS font, its palette and device context
	uint8_t* dib;   // the 1-bit DIB bits
	int dibPitch;   // dibW / 8 bytes
	int dibW;       // ((size + 31) & ~31)
} MonoFont_t;

void MonoFont_Ctor(MonoFont_t* f);
void MonoFont_Dtor(MonoFont_t* f);
void MonoFont_Close(MonoFont_t* f);
// 0 ok, 0x80000001 capacity < 2, 0x80000002 size not 8..200, 0x80000004 the font could not be created
int MonoFont_Init(MonoFont_t* f, const char* name, int size, int bold, int capacity);
void MonoFont_Glyph(MonoFont_t* f, MonoGlyph_t* out, int code); // fetch (render if needed) a glyph
void MonoGlyph_Paint(Bmp_t* cell, MonoGlyph_t* g, int code, MonoFont_t* font, uint32_t colour);
// draw `str` at (x, y) with `spacing` extra pixels per character; *outWidth gets the advance
void MonoText_Draw(Bmp_t* dst, int x, int y, const char* str, MonoFont_t* font, int spacing, uint32_t colour,
	int32_t* outWidth);

#endif // BGI_GFX_FONT_H_
