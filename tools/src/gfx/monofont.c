/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * monofont.c - the 1-bit font class of the "92 1E" text instruction and
 *              the text drawer built on it (inc/bgi/gfx/font.h)
 *
 * Unlike the anti-aliased FontRaster of font.c this class draws a glyph into
 * a monochrome DIB and keeps the raw bits: a glyph cell is `size` rows of
 * (size + 7) / 8 bytes, most significant bit first.  Up to `capacity` glyphs
 * are cached in a most-recently-used list.  The painter turns the bits into
 * solid pixels of one colour (no blending).  BmpOp_TextMono (src/gfx/bmptext.c)
 * opens one of these per call, draws the string and closes it again.
 */
#include <string.h>

#include "bgi/gfx/font.h"
#include "bgi/strutil.h"
#include "bgi/os.h"

// construct a closed font; the placeholder buffers are replaced by MonoFont_Init
void MonoFont_Ctor(MonoFont_t* f)
{
	f->open = 0;
	f->pool = (uint8_t*)BGI_Alloc(1);
	f->spare = (uint8_t*)BGI_Alloc(1);
	f->slots = (MonoGlyph_t*)BGI_Alloc(0x20);
	f->fixed = BGI_Alloc(0x1c);
}

// release the DIB, the font and the device context
void MonoFont_Close(MonoFont_t* f)
{
	if(!f->open)
		return;
	OS_FontClose(f->os);
	f->os = NULL;
	BGI_Free(f->dib);
	f->dib = NULL;
	f->open = 0;
}

void MonoFont_Dtor(MonoFont_t* f)
{
	MonoFont_Close(f);
	BGI_Free(f->pool);
	BGI_Free(f->spare);
	BGI_Free(f->slots);
	BGI_Free(f->fixed);
}

// the glyph pool and the slot table for `capacity` glyphs of `size`
static void MonoFont_AllocSlots(MonoFont_t* f, int size, int capacity)
{
	int i;

	f->bpr = (size + 7) / 8;
	f->bytesPerGlyph = f->bpr * size;
	BGI_Free(f->pool);
	f->pool = (uint8_t*)BGI_Alloc((size_t)f->bytesPerGlyph * (size_t)capacity);
	BGI_Free(f->slots);
	f->slots = (MonoGlyph_t*)BGI_Alloc((size_t)capacity * sizeof(MonoGlyph_t));
	f->capacity = capacity;
	f->used = 0;
	for(i = 0; i < capacity; i++)
		f->slots[i].bits = f->pool + (size_t)i * (size_t)f->bytesPerGlyph;
}

/* open the OS font (the original's CreateFontA(size, size / 2, bold ? 700
 * : 400, SHIFTJIS_CHARSET, DRAFT_QUALITY, FIXED_PITCH | FF_ROMAN) with a
 * two-entry palette) and allocate the 1-bit top-down DIB of
 * ((size + 31) & ~31) x size pixels.  1 ok, 0 when the OS refuses the
 * font. */
static int MonoFont_CreateFont(MonoFont_t* f, const char* name, int size, int bold)
{
	f->os = OS_FontOpenMono(name, size, bold != 0);
	if(!f->os)
		return 0;
	f->dibW = (size + 31) & ~31;
	f->dibPitch = f->dibW / 8;
	f->dib = (uint8_t*)BGI_Alloc((size_t)f->dibPitch * (size_t)size);
	f->size = size;
	return 1;
}

/* (re)open the font with a cache of `capacity` glyphs.  0 ok, 0x80000001
 * capacity < 2, 0x80000002 size outside 8..200, 0x80000004 the font could
 * not be created */
int MonoFont_Init(MonoFont_t* f, const char* name, int size, int bold, int capacity)
{
	MonoFont_Close(f);
	if(capacity < 2)
		return (int)0x80000001;
	if(size < 8 || size > 200)
		return (int)0x80000002;
	MonoFont_AllocSlots(f, size, capacity);
	if(!MonoFont_CreateFont(f, name, size, bold))
		return (int)0x80000004;
	f->open = 1;
	f->fixedCount = 0;
	return 0;
}

/* draw one character into the DIB, copy its bits into `bits`
 * (bpr bytes per row) and report the inked extent in *out, padded by a
 * byte's worth of pixels on both sides and clamped to the cell. */
static void MonoFont_Render(MonoFont_t* f, Rect_t* out, uint8_t* bits, int code)
{
	char str[8];
	int len, y, x, minCol = f->size - 1, maxCol = 0, pad;

	memset(f->dib, 0, (size_t)f->dibPitch * (size_t)f->size);
	len = TextPutChar(str, (uint32_t)code); // the bytes in the text encoding
	OS_FontRenderMono(f->os, str, len, f->dib, f->dibPitch, f->dibW, f->size);
	for(y = 0; y < f->size; y++)
		memcpy(bits + (size_t)y * (size_t)f->bpr, f->dib + (size_t)y * (size_t)f->dibPitch, (size_t)f->bpr);
	for(y = 0; y < f->size; y++)
	{
		const uint8_t* row = f->dib + (size_t)y * (size_t)f->dibPitch;
		int col = 0;
		for(x = 0; x < f->bpr; x++)
		{
			uint8_t mask;
			for(mask = 0x80; mask; mask >>= 1, col++)
			{
				if(row[x] & mask)
				{
					if(col < minCol)
						minCol = col;
					if(col > maxCol)
						maxCol = col;
				}
			}
		}
	}
	pad = (f->size + 7) / 8;
	if(pad <= 0)
		pad = 1;
	out->l = minCol - pad <= 0 ? 0 : minCol - pad;
	out->t = 0;
	out->r = maxCol + pad < f->size - 1 ? maxCol + pad : f->size - 1;
	out->b = f->size - 1;
}

/* the glyph cache.  A hit moves the entry to the front of the
 * most-recently-used list.  A miss takes a fresh slot, or - once all slots
 * are in use - the last entry of the list.  The original does not unlink
 * that last entry before moving it to the front, so the list then contains
 * a cycle; it is only ever walked `used` steps, so the behaviour is kept
 * (and harmless).  *out receives a copy of the entry. */
static void MonoFont_Lookup(MonoFont_t* f, MonoGlyph_t* out, int code)
{
	MonoGlyph_t* n = f->lru;
	MonoGlyph_t* prev = NULL; // "prev == NULL" stands for the list head itself
	int i;

	for(i = 0; i < f->used && n; i++)
	{
		if(n->code == code)
			break;
		prev = n;
		n = n->next;
	}
	if(i < f->used && n)
	{
		// hit: unlink
		if(prev)
			prev->next = n->next;
		else
			f->lru = n->next;
	}
	else
	{
		if(f->used < f->capacity)
			n = &f->slots[f->used++];
		else
			n = prev; // the last entry, reused in place (see above)
		n->code = code;
		n->dbcs = code >= 0x100;
		MonoFont_Render(f, &n->rect, n->bits, code);
	}
	n->next = f->lru;
	f->lru = n;
	*out = *n;
}

/* the fixed list in front of the cache (never filled: fixedCount
 * stays 0), kept for the call sequence; 1 when `code` was found there */
static int MonoFont_LookupFixed(MonoFont_t* f, MonoGlyph_t* out, int code)
{
	int i;
	for(i = 0; i < f->fixedCount; i++)
	{
		const MonoGlyph_t* e = (const MonoGlyph_t*)((const uint8_t*)f->fixed + (size_t)i * 0x1c);
		if(e->code == code)
		{
			memcpy(out, e, 0x1c);
			return 1;
		}
	}
	return 0;
}

// fetch (render if needed) the glyph of `code` into *out
void MonoFont_Glyph(MonoFont_t* f, MonoGlyph_t* out, int code)
{
	if(!MonoFont_LookupFixed(f, out, code))
		MonoFont_Lookup(f, out, code);
}

/* fetch the glyph of `code` into *g and paint it into the top-left of
 * `cell` in `colour` (clipped to the cell and the font size): set bits
 * become solid pixels (RGB16 gets the 5-5-5 colour, RGB32 the colour,
 * ARGB32 the colour with full alpha), clear bits become zero. */
void MonoGlyph_Paint(Bmp_t* cell, MonoGlyph_t* g, int code, MonoFont_t* font, uint32_t colour)
{
	int rows, cols, y;
	const uint8_t* bits;
	uint16_t c16;

	MonoFont_Glyph(font, g, code);
	bits = g->bits;
	rows = font->size < cell->h ? font->size : cell->h;
	cols = font->size < cell->w ? font->size : cell->w;
	c16 = (uint16_t)(((colour >> 9) & 0x7c00) | ((colour >> 6) & 0x3e0) | ((colour >> 3) & 0x1f));
	for(y = 0; y < rows; y++)
	{
		uint8_t* p = cell->pixels + (size_t)y * (size_t)cell->pitch;
		int left = cols;
		while(left > 0)
		{
			int n = left < 8 ? left : 8, k;
			uint8_t mask = 0x80;
			for(k = 0; k < n; k++, mask >>= 1, p += cell->bpp)
			{
				if(*bits & mask)
				{
					switch(cell->mode)
					{
						case PM_RGB16: *(uint16_t*)p = c16; break;
						case PM_RGB32: *(uint32_t*)p = colour; break;
						case PM_ARGB32: *(uint32_t*)p = colour | 0xff000000u; break;
						default: break;
					}
				}
				else
					memset(p, 0, (size_t)cell->bpp);
			}
			bits++;
			left -= n;
		}
	}
}

/* "92 1E": draw `str` into `dst` at (x, y) with `spacing` extra pixels per
 * character; a single-byte character is half a cell wide.  *outWidth
 * receives the advance.  Stops at the first glyph the blitter rejects
 * (incompatible pixel modes, or nothing of it inside the bitmap). */
void MonoText_Draw(Bmp_t* dst, int x, int y, const char* str, MonoFont_t* font, int spacing, uint32_t colour,
	int32_t* outWidth)
{
	Bmp_t cell;
	int size = font->size;

	Bmp_AllocScreen(&cell, size, size, 1);
	*outWidth = 0;
	while(*str)
	{
		MonoGlyph_t g;
		Bmp_t view;
		uint32_t code;
		int len = TextGetChar(&code, str);
		int dbcs = TextIsWide(code, len);
		MonoGlyph_Paint(&cell, &g, (int)code, font, colour);
		view = cell;
		if(!dbcs)
			view.w = cell.w / 2;
		if(Bmp_Blit(dst, x, y, &view, 0, 0) != 0)
			break;
		x += view.w + spacing;
		*outWidth += view.w + spacing;
		str += len;
	}
	BGI_Free(cell.pixels);
}
