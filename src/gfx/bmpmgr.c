/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bmpmgr.c - the bitmap manager (inc/bgi/gfx/bmpmgr.h): slots, generation
 * counters, raw pixel import/export, ripple tables and the text drawing
 * front end
 */
#include "bgi/gfx/bmpmgr.h"
#include <math.h>
#include "bgi/strutil.h"
#include "bgi/gfx/text.h" // the text direction of the OpenBGI extensions

BmpMgr_t* gBmpMgr;

// ========================================================================
// manager life cycle
// ========================================================================

// a manager with `count` empty slots, a fresh font cache and no ripples
BmpMgr_t* BmpMgr_New(int count)
{
	BmpMgr_t* m = (BmpMgr_t*)BGI_Alloc(sizeof *m);
	int i;
	m->fonts = FontCache_New();
	m->slots = (BmpSlot_t*)BGI_Alloc((size_t)count * sizeof(BmpSlot_t));
	m->count = count;
	for(i = 0; i < count; i++)
	{
		m->slots[i].buf = NULL;
		m->slots[i].baseW = m->slots[i].baseH = 0;
	}
	m->nextGen = 0;
	m->keepGen = 0;
	memset(m->ripples, 0, sizeof m->ripples);
	return m;
}

// release the pixels of slot `no` and mark its base size -1; 1 when it held a bitmap
int BmpMgr_FreeSlot(BmpMgr_t* m, int no)
{
	BmpSlot_t* s;
	if(no < 0 || no >= m->count)
		return 0;
	s = &m->slots[no];
	if(!s->buf)
		return 0;
	PixBuf_Free(s->buf);
	BGI_Free(s->buf);
	s->buf = NULL;
	s->baseW = s->baseH = -1;
	return 1;
}

void BmpMgr_FreeAll(BmpMgr_t* m)
{
	int i;
	for(i = 0; i < m->count; i++)
		BmpMgr_FreeSlot(m, i);
}

void BmpMgr_Delete(BmpMgr_t* m)
{
	BmpMgr_FreeAll(m);
	BmpMgr_FreeRipples(m);
	if(m->fonts)
		FontCache_Delete(m->fonts);
	BGI_Free(m->slots);
	BGI_Free(m);
}

// ========================================================================
// slots
// ========================================================================

// the generation of the bitmap in slot `no`; -1 for an empty slot or an invalid number
int BmpMgr_Generation(BmpMgr_t* m, int no)
{
	if(no < 0 || no >= m->count || !m->slots[no].buf)
		return -1;
	return m->slots[no].generation;
}

/* Allocate slot `no` as a w x h bitmap in pixel mode `mode`, freeing the
 * bitmap that was there; the pixels are not initialised.  1 ok, 0 for an
 * invalid slot number, a mode above 6 or a failed allocation.  The old
 * bitmap is freed before the mode is checked, so a bad mode still empties
 * the slot. */
int BmpMgr_Create(BmpMgr_t* m, int no, int w, int h, int mode)
{
	BmpSlot_t* s;
	int gen, bpp, pitch;
	uint8_t* pixels;
	if(no < 0 || no >= m->count)
		return 0;
	// a slot that is re-created keeps its generation only with "90 0B"
	gen = BmpMgr_Generation(m, no);
	if(gen == -1 || !m->keepGen)
		gen = m->nextGen++;
	BmpMgr_FreeSlot(m, no);
	if((uint32_t)mode > 6u)
		return 0;
	s = &m->slots[no];
	bpp = ModeBytes(mode);
	pitch = bpp * w;
	s->buf = (PixBuf_t*)BGI_Alloc(sizeof(PixBuf_t));
	PixBuf_Ctor(s->buf);
	pixels = PixBuf_Alloc(s->buf, (uint32_t)(pitch * h));
	s->bmp.pixels = pixels;
	if(!pixels)
	{
		PixBuf_Free(s->buf);
		BGI_Free(s->buf);
		s->buf = NULL;
		return 0;
	}
	s->bmp.mode = mode;
	s->bmp.pitch = pitch;
	s->bmp.w = w;
	s->bmp.h = h;
	s->bmp.bpp = bpp;
	s->generation = gen;
	return 1;
}

// the descriptor of the bitmap in slot `no` into *out; 1 when the slot is in use
int BmpMgr_GetInfo(BmpMgr_t* m, Bmp_t* out, int no)
{
	if(no < 0 || no >= m->count || !m->slots[no].buf)
		return 0;
	*out = m->slots[no].bmp;
	return 1;
}

// the slot need not hold a bitmap: any valid number takes the size
int BmpMgr_SetBaseSize(BmpMgr_t* m, int no, int w, int h)
{
	if(no < 0 || no >= m->count)
		return 0;
	m->slots[no].baseW = w;
	m->slots[no].baseH = h;
	return 1;
}

// the slot need not hold a bitmap: any valid number answers
int BmpMgr_GetBaseSize(BmpMgr_t* m, int32_t* out, int no)
{
	if(no < 0 || no >= m->count)
		return 0;
	out[0] = m->slots[no].baseW;
	out[1] = m->slots[no].baseH;
	return 1;
}

/* "90 17" of 1.494 on: relabel a 32-bit bitmap
 * with alpha (mode 2) as one without (mode 1); the pixels stay.  0 ok,
 * 0xb no bitmap, 0x15 any other conversion (the same mode is ok). */
uint32_t BmpMgr_SetMode(BmpMgr_t* m, int no, int mode)
{
	BmpSlot_t* s;
	if(no < 0 || no >= m->count || !m->slots[no].buf)
		return 0xb;
	s = &m->slots[no];
	if(s->bmp.mode == mode)
		return 0;
	if(s->bmp.mode != 2 || mode != 1)
		return 0x15;
	s->bmp.mode = 1;
	s->bmp.bpp = ModeBytes(1);
	return 0;
}

// zero the bitmap for colour 0, else Bmp_Fill it; 1 ok, 0 no bitmap
int BmpMgr_Clear(BmpMgr_t* m, int no, uint32_t colour)
{
	Bmp_t b;
	if(!BmpMgr_GetInfo(m, &b, no))
		return 0;
	if(colour == 0)
		Bmp_Clear(&b, NULL);
	else
		Bmp_Fill(&b, NULL, colour);
	return 1;
}

/* An ARGB32 picture that was composited over the key colour
 * gBmpOption gets its original colours back:
 *   c = clamp((c - key * (255 - a) / 255) * 255 / a)   for 0 < a < 255
 * 1 when done, 0 when there is no key colour or the picture is not
 * ARGB32. */
int Bmp_Uncomposite(Bmp_t* b)
{
	uint32_t key = (uint32_t)Gfx_GetBmpOption();
	int y, x, i;
	if(key == 0 || b->mode != PM_ARGB32)
		return 0;
	for(y = 0; y < b->h; y++)
	{
		uint8_t* p = b->pixels + (size_t)y * b->pitch;
		for(x = 0; x < b->w; x++, p += b->bpp)
		{
			int a = p[3];
			if(a <= 0 || a >= 255)
				continue;
			for(i = 0; i < 3; i++)
			{
				int k = (int)((key >> (8 * i)) & 0xff);
				int t = (k * (255 - a)) / 255;
				int v = ((p[i] - t) * 255) / a;
				if(v > 255)
					v = 255;
				if(v < 0)
					v = 0;
				p[i] = (uint8_t)v;
			}
		}
	}
	return 1;
}

/* Create slot `no` and fill it from packed rows of pixels: 24 bpp B G R
 * for RGB32, the mode's own pixel size otherwise.  An ARGB32 result is
 * un-composited against gBmpOption.  1 ok, 0 when the slot could not be
 * created. */
int BmpMgr_PutPixels(BmpMgr_t* m, int no, int w, int h, int mode, const void* data)
{
	Bmp_t b;
	if(!BmpMgr_Create(m, no, w, h, mode))
		return 0;
	BmpMgr_GetInfo(m, &b, no);
	if(mode != PM_RGB32)
	{
		Bmp_t src;
		src.pixels = (uint8_t*)data;
		src.pitch = b.bpp * w;
		src.w = w;
		src.h = h;
		src.mode = mode;
		src.bpp = b.bpp;
		Bmp_CopyRect(&b, &src);
	}
	else
	{ // 24 bpp packed B G R -> RGB32
		const uint8_t* s = (const uint8_t*)data;
		int y, x;
		for(y = 0; y < h; y++)
		{
			uint32_t* d = (uint32_t*)(b.pixels + (size_t)y * b.pitch);
			for(x = 0; x < w; x++, s += 3)
				d[x] = (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16;
		}
	}
	Bmp_Uncomposite(&b);
	return 1;
}

/* Copy the bitmap of slot `no` out as packed rows (24 bpp for RGB32, the
 * mode's own size otherwise; the pixel modes above 3 use the size of mode
 * `mode & 3` - the original reads past its table there).  0 ok, 9 no
 * bitmap, 0xa `size` is too small for the pixels.  `unused` is never
 * read. */
int BmpMgr_GetPixels(BmpMgr_t* m, void* dst, int32_t unused, int32_t size, int no)
{
	static const int bitsOf[4] = {16, 24, 32, 8};
	Bmp_t b;
	int bytes, y, x;
	uint8_t* d = (uint8_t*)dst;
	if(!BmpMgr_GetInfo(m, &b, no))
		return 9;
	bytes = bitsOf[b.mode & 3] >> 3; // modes above 3 read the stack in the original
	if((uint32_t)size < (uint32_t)(b.w * b.h * bytes))
		return 0xa;
	for(y = 0; y < b.h; y++)
	{
		const uint8_t* s = b.pixels + (size_t)y * b.pitch;
		for(x = 0; x < b.w; x++, s += b.bpp, d += bytes)
			memcpy(d, s, (size_t)bytes);
	}
	return 0;
}

// create dstNo as a GRAY8 bitmap holding the luminance of srcNo (Blit_ToGray); 0 ok, 9 no source, 0xa not created
int BmpMgr_ToGray(BmpMgr_t* m, int dstNo, int srcNo)
{
	Bmp_t s, d;
	if(!BmpMgr_GetInfo(m, &s, srcNo))
		return 9;
	if(!BmpMgr_Create(m, dstNo, s.w, s.h, PM_GRAY8))
		return 0xa;
	BmpMgr_GetInfo(m, &d, dstNo);
	Blit_ToGray(&d, &s);
	return 0;
}

// invert the GRAY8 bitmap of slot `no` in place; 0 ok, 7 not GRAY8, 0xb no bitmap
int BmpMgr_InvertGray(BmpMgr_t* m, int no)
{
	Bmp_t b;
	if(!BmpMgr_GetInfo(m, &b, no))
		return 0xb;
	return Blit_InvertGray(&b) ? 0 : 7;
}

// ========================================================================
// ripples
// ========================================================================

// free the tables of all eight definitions and mark them undefined
void BmpMgr_FreeRipples(BmpMgr_t* m)
{
	int i;
	for(i = 0; i < 8; i++)
	{
		if(m->ripples[i].defined)
		{
			BGI_Free(m->ripples[i].table);
			memset(&m->ripples[i], 0, sizeof m->ripples[i]);
		}
	}
}

/* one period of sin() scaled by amp (and attenuated by 2^-k when k > 0)
 * written as int16 pairs (x = y); the operations are applied in the
 * original's order, sin(i * 2pi / period) * amp [* (1.0 / 2^k)], and the
 * result truncated.  Returns the position after the cycle. */
static int16_t* RippleCycle(int16_t* p, int period, double amp, int k)
{
	int i;
	double scale = k > 0 ? 1.0 / (double)(1 << k) : 0.0;
	for(i = 0; i < period; i++)
	{
		double v = sin((double)i * 6.283185307179586 / (double)period) * amp;
		int16_t s;
		if(k > 0)
			v = v * scale;
		s = (int16_t)BGI_Ftol(v);
		*p++ = s;
		*p++ = s;
	}
	return p;
}

/* "92 00": define ripple `no` (0 .. 7): `rings` repetitions of one sine
 * cycle of period * 4 entries with the amplitude, each followed by count -
 * 1 silent cycles (a zero period, count or rings counts as 1).  Replaces
 * an earlier definition.  0 ok, 0x10 bad number. */
int BmpMgr_DefineRipple(BmpMgr_t* m, int no, int period, int amplitude, int count, int rings)
{
	RippleDef_t* d;
	int16_t* p;
	int r, c;
	if((uint32_t)no >= 8u)
		return 0x10;
	d = &m->ripples[no];
	if(d->defined)
		BGI_Free(d->table);
	if(period == 0)
		period = 1;
	period *= 4;
	if(count == 0)
		count = 1;
	if(rings == 0)
		rings = 1;
	d->defined = 1;
	d->pos = 0;
	d->base = 0;
	d->period = period * count;
	d->total = d->period * rings;
	d->table = (int16_t*)BGI_Alloc((size_t)d->total * 4);
	p = d->table;
	for(r = 0; r < rings; r++)
	{
		p = RippleCycle(p, period, (double)(uint32_t)amplitude, 0);
		for(c = 1; c < count; c++)
		{
			memset(p, 0, (size_t)period * 4);
			p += period * 2;
		}
	}
	return 0;
}

/* "92 01": as BmpMgr_DefineRipple, but every ring is count - 1 silent
 * cycles followed by `fadeIn` growing cycles (amplitude / 2^k, k =
 * fadeIn .. 1), the full cycle and `fadeOut` decaying ones (k = 1 ..
 * fadeOut).  0 ok, 0x10 bad number. */
int BmpMgr_DefineRippleEx(BmpMgr_t* m, int no, int period, int amplitude, int fadeIn, int fadeOut,
	int count, int rings)
{
	RippleDef_t* d;
	int16_t* p;
	int r, c, k, cycle;
	if((uint32_t)no >= 8u)
		return 0x10;
	d = &m->ripples[no];
	if(d->defined)
		BGI_Free(d->table);
	if(period == 0)
		period = 1;
	period *= 4;
	if(count == 0)
		count = 1;
	if(rings == 0)
		rings = 1;
	cycle = (fadeIn + fadeOut + 1) * period;
	d->defined = 1;
	d->pos = 0;
	d->base = 0;
	d->period = cycle * count;
	d->total = d->period * rings;
	d->table = (int16_t*)BGI_Alloc((size_t)d->total * 4);
	p = d->table;
	for(r = 0; r < rings; r++)
	{
		for(c = 1; c < count; c++)
		{ // leading silent cycles
			memset(p, 0, (size_t)cycle * 4);
			p += cycle * 2;
		}
		for(k = fadeIn; k >= 1; k--) // growing: amp / 2^k
			p = RippleCycle(p, period, (double)(uint32_t)amplitude, k);
		p = RippleCycle(p, period, (double)(uint32_t)amplitude, 0);
		for(k = 1; k <= fadeOut; k++) // decaying
			p = RippleCycle(p, period, (double)(uint32_t)amplitude, k);
	}
	return 0;
}

/* start index for `selector` rings before the running position, wrapped
 * into the definition's period (shared by RippleCheck and RippleFill) */
static int RippleIndex(const RippleDef_t* d, int selector)
{
	int idx = d->pos - selector * 4;
	if(idx < d->base)
	{
		uint32_t back = (uint32_t)(d->base - idx) % (uint32_t)d->period;
		idx = (int)((uint32_t)d->period - back) + d->base;
	}
	return idx;
}

/* Whether definition `no` has `rings` rings (4 entries each) left from
 * the phase `selector`: *ok receives 1 / 0.  0 ok, 0x10 bad number, 0x11
 * not defined. */
int BmpMgr_RippleCheck(BmpMgr_t* m, int* ok, int no, int selector, int rings)
{
	const RippleDef_t* d;
	int idx;
	if((uint32_t)no >= 8u)
		return 0x10;
	d = &m->ripples[no];
	if(!d->defined)
		return 0x11;
	idx = RippleIndex(d, selector);
	*ok = (d->total - idx) < rings * 4 ? 0 : 1;
	return 0;
}

/* Fill the ring table of the ripple blitters: rings * 4 entries of
 * {int16 ax, int16 ay}, both halves the x displacement of the definition's
 * entry scaled by level / 256, from the phase `selector`.  The
 * RippleCheck codes, 0x12 when not enough rings are left. */
int BmpMgr_RippleFill(BmpMgr_t* m, uint32_t* table, int no, int selector, int level, int rings)
{
	const RippleDef_t* d;
	int ok, r, idx, i;
	r = BmpMgr_RippleCheck(m, &ok, no, selector, rings);
	if(r != 0)
		return r;
	if(!ok)
		return 0x12;
	d = &m->ripples[no];
	idx = RippleIndex(d, selector);
	for(i = 0; i < rings * 4; i++)
	{
		int v = d->table[(idx + i) * 2]; // the x half of the pair
		uint32_t w = (uint32_t)((v * level) >> 8) & 0xffffu;
		table[i] = w << 16 | w;
	}
	return 0;
}

// ========================================================================
// fonts and text
// ========================================================================

int BmpMgr_FontOpen(BmpMgr_t* m, int* handle, const char* name, int size, int widthPct, int bold)
{
	return FontCache_Open(m->fonts, handle, name, size, widthPct, bold);
}

int BmpMgr_FontOpenExtra(BmpMgr_t* m, int* handle, const char* name, int size, int widthPct, int bold, int extraUpright,
	int extraItalic)
{
	return FontCache_OpenExtra(m->fonts, handle, name, size, widthPct, bold, extraUpright, extraItalic);
}

int BmpMgr_FontInfo(BmpMgr_t* m, FontInfo_t* out, int handle)
{
	return FontCache_GetInfo(m->fonts, out, handle);
}

int BmpMgr_FontRequest(BmpMgr_t* m, const char* name, int size, int widthPct, int bold, int amount)
{
	return FontCache_Request(m->fonts, name, size, widthPct, bold, amount);
}

/* Draw `str` into dst from (x, y) with the font `handle` (see bmpmgr.h for
 * the arguments).  Every glyph is painted into a cell bitmap of the
 * screen's mode with alpha and blitted with effect / level; a glyph that
 * falls off the bitmap ends the drawing.  '\n' starts a new line; with
 * `wrap` a line also breaks when the next glyph would pass the right
 * edge, except that one kinsoku (line-head-prohibited) character may
 * overhang.  1 ok, 0 unknown font. */
int BmpMgr_DrawTextEx(BmpMgr_t* m, Bmp_t* dst, int* measure, int x, int y, const char* str, int handle,
	uint32_t colour, int effect, int level, int proportional, int wrap, int spacingPct)
{
	FontInfo_t info;
	Bmp_t cell;
	Glyph_t glyph;
	int size, glyphW, maxX, spacing, xStart = x, lines = 1, maxWidth = 0, overhang = 0;
	const char* s = str;

	if(!FontCache_GetInfo(m->fonts, &info, handle))
		return 0;
	size = info.size;
	Bmp_AllocScreen(&cell, size * 2, size, 1);
	glyphW = (info.widthPct * size) / 100;
	maxX = dst->w - glyphW;
	spacing = Font_Percent(size, spacingPct);
	*measure = 0;

	while(*s)
	{
		uint32_t code;
		int len = TextGetChar(&code, s);
		int dbcs = TextIsWide(code, len);
		Bmp_t view;
		int gap = 0, advance;

		if(*s == '\n')
		{
			y += size + spacing;
			lines++;
			overhang = 0;
			if(!wrap)
			{
				if((uint32_t)maxWidth < (uint32_t)*measure)
					maxWidth = *measure;
				*measure = 0;
			}
			s += len;
			continue;
		}

		Font_PaintGlyph(&cell, &glyph, (int)code, info.font, colour);
		view = cell;
		if(proportional)
		{
			Bmp_CropGlyph(&view, &glyph);
			gap = Font_GapFor(&glyph, size);
			advance = view.w;
		}
		else
		{
			advance = dbcs ? glyphW : glyphW / 2;
			view.w = advance; // the blit covers only the advance
		}
		if(wrap && (uint32_t)(advance + 2 * gap + x) > (uint32_t)maxX)
		{
			if(!overhang && IsKinsokuChar(code))
			{
				overhang = 1; // a line-head-prohibited character may stick out once
			}
			else
			{
				y += size + spacing;
				lines++;
				x = xStart;
				overhang = 0;
			}
		}
		if(Bmp_Blit(dst, x + gap, y, &view, effect, level) != 0)
			break; // off the bitmap: stop drawing
		x += advance + 2 * gap;
		if(wrap)
			*measure = lines;
		else
			*measure += advance + 2 * gap;
		s += len;
	}
	Bmp_Free(&cell);
	if(!wrap && (uint32_t)maxWidth > (uint32_t)*measure)
		*measure = maxWidth;
	return 1;
}

/* BmpMgr_DrawTextEx without wrapping; *measure receives the widest line.
 * (OpenBGI: while the text direction of "EF 01" is not 0 the string goes
 * through the bidirectional layout, so that a caption in Arabic or Hebrew
 * is joined and ordered like the message text.) */
int BmpMgr_DrawText(BmpMgr_t* m, Bmp_t* dst, int* measure, int x, int y, const char* str, int handle,
	uint32_t colour, int effect, int level, int proportional)
{
	if(gTextDirection != 0 && m == gTextBmpMgr)
		return Text_DrawBidiLine(dst, measure, x, y, str, handle, colour, effect, level, proportional);
	return BmpMgr_DrawTextEx(m, dst, measure, x, y, str, handle, colour, effect, level, proportional, 0, 0);
}

/* "7D": a hex dump of `len` bytes into `dst`: one line of up to
 * 16 bytes per text row ("OOOO : xx xx ..."), each drawn into a black line
 * bitmap of the destination's mode that is then copied to (x, y + n*size).
 * Always 0; an unknown font draws nothing. */
int BmpMgr_DrawHexDump(BmpMgr_t* m, Bmp_t* dst, int x, int y, const void* data, int len, int handle, uint32_t colour)
{
	FontInfo_t fi;
	Bmp_t line;
	const uint8_t* p = (const uint8_t*)data;
	int lines, offset = 0;

	if(!BmpMgr_FontInfo(m, &fi, handle))
		return 0;
	// the template "0000 : 00 00 ..." is 55 characters; half a size each
	Bmp_Alloc(&line, 55 * fi.size / 2, fi.size, dst->mode);
	lines = (len + 15) / 16;
	while(lines-- > 0)
	{
		char hex[0x100], text[0x100], tmp[0x100];
		int n = len >= 16 ? 16 : len, i, measure;

		switch(line.mode)
		{
			case PM_RGB16: Bmp_Fill(&line, NULL, 1); break;
			case PM_RGB32: Bmp_Fill(&line, NULL, 0); break;
			case PM_ARGB32: Bmp_Fill(&line, NULL, 0xa0000000u); break;
			default: break;
		}
		memset(hex, 0, sizeof hex);
		for(i = 0; i < n; i++)
		{
			sprintf(tmp, "%s%.2X ", hex, *p++);
			strcpy(hex, tmp);
		}
		sprintf(text, "%.4X : %s", offset, hex);
		BmpMgr_DrawText(m, &line, &measure, 0, 0, text, handle, colour, 0, 0, 0);
		Bmp_Blit(dst, x, y, &line, 0x80, 0);
		y += fi.size;
		offset += 16;
		len -= 16;
	}
	BGI_Free(line.pixels);
	return 0;
}
