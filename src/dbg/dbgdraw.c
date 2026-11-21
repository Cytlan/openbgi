/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dbgdraw.c - the debugger's surface: rectangles, text, bitmap previews
 *             (dbg_internal.h)
 *
 * Every primitive honours the clip rectangle of the surface (DbgDraw_Clip);
 * the surface exists between DbgDraw_Init and DbgDraw_Shutdown, which is
 * while the debugger's window is open.
 *
 * Text comes from the mono typeface (Typeface_OpenMono, the same GDI-like
 * path the engine's text instruction uses) at a 8 x 16
 * cell: the ASCII glyphs are rendered once into a cache, two-byte
 * Shift-JIS characters (the games' own strings in the listings) on
 * demand into a small cache of their own.  No font files, no embedded
 * font: "MS Gothic" where the platform has it (its half-width glyphs are
 * exactly 8 x 16), else the platform's monospace face, at the largest
 * size up to 16 whose advance fits the cell; a face that fits at no size
 * (a proportional one) widens the cell to its advance instead.
 */
#include "dbg_internal.h"
#include "bgi/msg.h"
#include "bgi/gfx/typeface.h"
#include "bgi/core.h"

#include <stdarg.h>

// the widest advance of the printable ASCII characters of a font
static int MaxAdvance(Typeface_t* f)
{
	int c, w = 0;
	for(c = 0x21; c < 0x7f; c++)
	{
		char ch = (char)c;
		int a = Typeface_Advance(f, &ch, 1);
		if(a > w)
			w = a;
	}
	return w;
}

/* MS Gothic at 16 when the platform has it; else the monospace face at the
 * largest size up to 16 whose glyphs fit 8 pixels; else whatever fits */
static void OpenFont(OpenBGI_t* o)
{
	const char* face = Typefaces_Exists(o, MSG_FONT_GOTHIC) ? MSG_FONT_GOTHIC : "monospace";
	int size, adv = 0;
	o->dbg.dbgCellW = 8;
	for(size = DBG_CELL_H; size >= 11; size--)
	{
		o->dbg.font = Typeface_OpenMono(o, face, size, 0);
		if(!o->dbg.font)
			return;
		adv = MaxAdvance(o->dbg.font);
		if(adv <= 8)
			return;
		Typeface_Close(o->dbg.font);
		o->dbg.font = NULL;
	}
	o->dbg.font = Typeface_OpenMono(o, face, 14, 0);
	if(o->dbg.font)
	{
		adv = MaxAdvance(o->dbg.font);
		o->dbg.dbgCellW = adv > 8 ? (adv < 16 ? adv : 16) : 8;
	}
}

// the printable ASCII glyphs into o->dbg.ascii, once per font; without a font every character is a box
static void RenderAscii(OpenBGI_t* o)
{
	int c;
	if(o->dbg.asciiReady)
		return;
	o->dbg.asciiReady = 1;
	for(c = 0x20; c < 0x7f; c++)
	{
		char ch = (char)c;
		if(o->dbg.font)
			Typeface_RenderMono(o->dbg.font, &ch, 1, o->dbg.ascii[c], DBG_CELL_PITCH, DBG_CELL_W(o), DBG_CELL_H);
	}
	if(!o->dbg.font)
	{ // no font at all: a box for every character, so that something shows
		for(c = 0x21; c < 0x7f; c++)
		{
			int y;
			for(y = 2; y < DBG_CELL_H - 3; y++)
				o->dbg.ascii[c][y * DBG_CELL_PITCH] = (y == 2 || y == DBG_CELL_H - 4) ? 0x7c : 0x44;
		}
	}
}

// A black surface of w x h pixels, the font and the ASCII glyph cache: 1 ok, 0 when the surface cannot be allocated.
int DbgDraw_Init(OpenBGI_t* o, int w, int h)
{
	o->dbg.dbgSurf.px = (uint32_t*)calloc((size_t)w * h, 4);
	if(!o->dbg.dbgSurf.px)
		return 0;
	o->dbg.dbgSurf.w = w;
	o->dbg.dbgSurf.h = h;
	DbgDraw_Clip(o, 0, 0, 0, 0);
	if(!o->dbg.font)
		OpenFont(o);
	RenderAscii(o);
	return 1;
}

// Free the surface, the font and the glyph caches.
void DbgDraw_Shutdown(OpenBGI_t* o)
{
	free(o->dbg.dbgSurf.px);
	o->dbg.dbgSurf.px = NULL;
	if(o->dbg.font)
		Typeface_Close(o->dbg.font);
	o->dbg.font = NULL;
	o->dbg.asciiReady = 0;
	memset(o->dbg.wide, 0, sizeof o->dbg.wide);
}

// Set the clip rectangle, cut to the surface; an empty one (w or h <= 0) resets it to the whole surface.
void DbgDraw_Clip(OpenBGI_t* o, int x, int y, int w, int h)
{
	if(w <= 0 || h <= 0)
	{
		o->dbg.dbgSurf.clipX = o->dbg.dbgSurf.clipY = 0;
		o->dbg.dbgSurf.clipW = o->dbg.dbgSurf.w;
		o->dbg.dbgSurf.clipH = o->dbg.dbgSurf.h;
		return;
	}
	if(x < 0)
	{
		w += x;
		x = 0;
	}
	if(y < 0)
	{
		h += y;
		y = 0;
	}
	if(x + w > o->dbg.dbgSurf.w)
		w = o->dbg.dbgSurf.w - x;
	if(y + h > o->dbg.dbgSurf.h)
		h = o->dbg.dbgSurf.h - y;
	o->dbg.dbgSurf.clipX = x;
	o->dbg.dbgSurf.clipY = y;
	o->dbg.dbgSurf.clipW = w > 0 ? w : 0;
	o->dbg.dbgSurf.clipH = h > 0 ? h : 0;
}

// Fill a rectangle (clipped).
void DbgDraw_Fill(OpenBGI_t* o, int x, int y, int w, int h, uint32_t rgb)
{
	int x0 = x, y0 = y, x1 = x + w, y1 = y + h, yy;
	if(x0 < o->dbg.dbgSurf.clipX)
		x0 = o->dbg.dbgSurf.clipX;
	if(y0 < o->dbg.dbgSurf.clipY)
		y0 = o->dbg.dbgSurf.clipY;
	if(x1 > o->dbg.dbgSurf.clipX + o->dbg.dbgSurf.clipW)
		x1 = o->dbg.dbgSurf.clipX + o->dbg.dbgSurf.clipW;
	if(y1 > o->dbg.dbgSurf.clipY + o->dbg.dbgSurf.clipH)
		y1 = o->dbg.dbgSurf.clipY + o->dbg.dbgSurf.clipH;
	if(x1 <= x0 || y1 <= y0 || !o->dbg.dbgSurf.px)
		return;
	for(yy = y0; yy < y1; yy++)
	{
		uint32_t* row = o->dbg.dbgSurf.px + (size_t)yy * o->dbg.dbgSurf.w;
		int xx;
		for(xx = x0; xx < x1; xx++)
			row[xx] = rgb;
	}
}

void DbgDraw_HLine(OpenBGI_t* o, int x, int y, int w, uint32_t rgb)
{
	DbgDraw_Fill(o, x, y, w, 1, rgb);
}

void DbgDraw_VLine(OpenBGI_t* o, int x, int y, int h, uint32_t rgb)
{
	DbgDraw_Fill(o, x, y, 1, h, rgb);
}

// A one-pixel outline just inside the rectangle.
void DbgDraw_Frame(OpenBGI_t* o, int x, int y, int w, int h, uint32_t rgb)
{
	DbgDraw_HLine(o, x, y, w, rgb);
	DbgDraw_HLine(o, x, y + h - 1, w, rgb);
	DbgDraw_VLine(o, x, y, h, rgb);
	DbgDraw_VLine(o, x + w - 1, y, h, rgb);
}

// one pixel, clipped
static void Plot(OpenBGI_t* o, int x, int y, uint32_t rgb)
{
	if(x >= o->dbg.dbgSurf.clipX && x < o->dbg.dbgSurf.clipX + o->dbg.dbgSurf.clipW && y >= o->dbg.dbgSurf.clipY &&
		y < o->dbg.dbgSurf.clipY + o->dbg.dbgSurf.clipH)
		o->dbg.dbgSurf.px[(size_t)y * o->dbg.dbgSurf.w + x] = rgb;
}

// a glyph of `cw` pixels from `bits` (MSB first, `pitch` bytes per row)
static void Glyph(OpenBGI_t* o, int x, int y, const uint8_t* bits, int pitch, int cw, uint32_t rgb)
{
	int gx, gy;
	for(gy = 0; gy < DBG_CELL_H; gy++)
		for(gx = 0; gx < cw; gx++)
			if(bits[gy * pitch + (gx >> 3)] & (0x80 >> (gx & 7)))
				Plot(o, x + gx, y + gy, rgb);
}

// the glyph of the two-byte character at s, from the cache or rendered into its next slot
static const uint8_t* WideGlyph(OpenBGI_t* o, const char* s)
{
	uint16_t code = (uint16_t)(((uint8_t)s[0] << 8) | (uint8_t)s[1]);
	int i, slot;
	for(i = 0; i < DBG_WIDE_CACHE; i++)
		if(o->dbg.wide[i].used && o->dbg.wide[i].code == code)
			return o->dbg.wide[i].bits;
	slot = o->dbg.wideNext;
	o->dbg.wideNext = (o->dbg.wideNext + 1) % DBG_WIDE_CACHE;
	o->dbg.wide[slot].used = 1;
	o->dbg.wide[slot].code = code;
	memset(o->dbg.wide[slot].bits, 0, sizeof o->dbg.wide[slot].bits);
	if(o->dbg.font)
		Typeface_RenderMono(o->dbg.font, s, 2, o->dbg.wide[slot].bits, DBG_CELL_PITCH, DBG_CELL_W(o) * 2, DBG_CELL_H);
	return o->dbg.wide[slot].bits;
}

// a Shift-JIS lead byte
static int IsLead(uint8_t c)
{
	return (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc);
}

/* Draw a string at (x, y): ASCII one cell each, a two-byte character two
 * cells, a tab to the next multiple of four cells, a stray high byte as a
 * dot.  Returns the width drawn in pixels. */
int DbgDraw_Text(OpenBGI_t* o, int x, int y, uint32_t rgb, const char* text)
{
	int x0 = x;
	RenderAscii(o);
	while(*text)
	{
		uint8_t c = (uint8_t)*text;
		if(IsLead(c) && text[1])
		{
			if(x + DBG_CELL_W(o) * 2 > o->dbg.dbgSurf.clipX && x < o->dbg.dbgSurf.clipX + o->dbg.dbgSurf.clipW)
				Glyph(o, x, y, WideGlyph(o, text), DBG_CELL_PITCH, DBG_CELL_W(o) * 2, rgb);
			x += DBG_CELL_W(o) * 2;
			text += 2;
			continue;
		}
		if(c == '\t')
		{
			x = x0 + ((x - x0) / (DBG_CELL_W(o) * 4) + 1) * DBG_CELL_W(o) * 4;
			text++;
			continue;
		}
		if(c >= 0x20 && c < 0x7f)
		{
			if(x + DBG_CELL_W(o) > o->dbg.dbgSurf.clipX && x < o->dbg.dbgSurf.clipX + o->dbg.dbgSurf.clipW)
				Glyph(o, x, y, o->dbg.ascii[c], DBG_CELL_PITCH, DBG_CELL_W(o), rgb);
		}
		else if(c >= 0x80)
		{ // a stray byte: a dot
			Plot(o, x + 3, y + DBG_CELL_H - 4, rgb);
		}
		x += DBG_CELL_W(o);
		text++;
	}
	return x - x0;
}

// The width DbgDraw_Text would take, in pixels (tabs count as one cell here).
int DbgDraw_TextWidth(OpenBGI_t* o, const char* text)
{
	int w = 0;
	while(*text)
	{
		uint8_t c = (uint8_t)*text;
		if(IsLead(c) && text[1])
		{
			w += DBG_CELL_W(o) * 2;
			text += 2;
		}
		else
		{
			w += DBG_CELL_W(o);
			text++;
		}
	}
	return w;
}

// printf-style DbgDraw_Text (the line is cut at 0x3ff characters).
int DbgDraw_TextF(OpenBGI_t* o, int x, int y, uint32_t rgb, const char* fmt, ...)
{
	char buf[0x400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return DbgDraw_Text(o, x, y, rgb, buf);
}

// one pixel of a bitmap as 0x00RRGGBB: RGB555 for 16 bits (as the back ends read it), a grey for 8 bits, the low 24 bits for 32
static uint32_t BmpPixel(const Bmp_t* b, int x, int y)
{
	const uint8_t* row = b->pixels + (ptrdiff_t)y * b->pitch;
	if(b->bpp == 2)
	{
		uint32_t v = ((const uint16_t*)row)[x];
		uint32_t r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, bb = v & 0x1f;
		return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (bb << 3 | bb >> 2);
	}
	if(b->bpp == 1)
	{
		uint32_t v = row[x];
		return (v << 16) | (v << 8) | v;
	}
	return ((const uint32_t*)row)[x] & 0xffffff;
}

/* Draw a bitmap at (x, y) scaled by nearest neighbour to fit w x h,
 * keeping its aspect and never enlarging it; an ARGB32 bitmap (mode 2) is
 * composed over a grey checkerboard so that transparency shows.  Nothing
 * is drawn for a bitmap without pixels (a virtual slot). */
void DbgDraw_Bitmap(OpenBGI_t* o, int x, int y, int w, int h, const Bmp_t* b)
{
	double sx, sy, s;
	int dw, dh, yy, xx, check;
	if(!b || !b->pixels || b->w <= 0 || b->h <= 0 || w <= 0 || h <= 0)
		return;
	sx = (double)w / b->w;
	sy = (double)h / b->h;
	s = sx < sy ? sx : sy;
	if(s > 1.0)
		s = 1.0; // never enlarged
	dw = (int)(b->w * s);
	dh = (int)(b->h * s);
	if(dw < 1)
		dw = 1;
	if(dh < 1)
		dh = 1;
	for(yy = 0; yy < dh; yy++)
	{
		int by = (int)(yy / s);
		if(by >= b->h)
			by = b->h - 1;
		for(xx = 0; xx < dw; xx++)
		{
			int bx = (int)(xx / s);
			uint32_t p;
			if(bx >= b->w)
				bx = b->w - 1;
			p = BmpPixel(b, bx, by);
			if(b->bpp == 4 && b->mode == 2)
			{ // ARGB: over a checkerboard, as image viewers do
				uint32_t a = ((const uint32_t*)(b->pixels + (ptrdiff_t)by * b->pitch))[bx] >> 24;
				uint32_t r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, bl = p & 0xff;
				check = ((xx >> 3) + (yy >> 3)) & 1 ? 0x60 : 0x40;
				r = (r * a + (uint32_t)check * (255 - a)) / 255;
				g = (g * a + (uint32_t)check * (255 - a)) / 255;
				bl = (bl * a + (uint32_t)check * (255 - a)) / 255;
				p = (r << 16) | (g << 8) | bl;
			}
			Plot(o, x + xx, y + yy, p);
		}
	}
}
