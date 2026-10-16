/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * blit_filter.c - the filter blitters (inc/bgi/gfx/bitmap.h): the colour
 *                 filters of the Filter display object and of "91 1D", the
 *                 gray conversions, the blit through a gray map and the
 *                 channel extraction of effect 0xFF
 */
#include "blit_internal.h"

// ---- filters (class Filter, src/gfx/filter.c) ----------------------------------------------------

/*
 * Filter kind 0 driven by a gray bitmap of the destination's size.  For
 * every pixel v = (gray << param) + 0x100 - ((1 << param) + 1) * level:
 *   v <= 0     -> colour
 *   v >= 0x100 -> unchanged
 *   otherwise  -> colour + (d - colour) * (v >> 1) >> 7
 * The top byte becomes 0 (RGB32 destinations only; others are left alone).
 */
void Blit_TintByGray(Bmp_t* dst, uint32_t colour, const Bmp_t* gray, int param, int level)
{
	int base, y, x;
	if(dst->mode != PM_RGB32)
		return;
	base = 0x100 - ((1 << param) + 1) * level;
	for(y = 0; y < dst->h; y++)
	{
		const uint8_t* g = ROW(gray, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < dst->w; x++)
		{
			int v = (g[x] << param) + base, w, i, out;
			if(v <= 0)
			{
				d[x] = colour;
				continue;
			}
			if(v >= 0x100)
				continue;
			w = v >> 1;
			out = 0;
			for(i = 0; i < 24; i += 8)
			{
				int cc = (int)((colour >> i) & 0xff), dc = (int)((d[x] >> i) & 0xff);
				out |= Sat8((((dc - cc) * w) >> 7) + cc) << i;
			}
			d[x] = (uint32_t)out;
		}
	}
}

// RGB32 <- RGB32, the dim of blit.c as the filters use it: src * inv >> 8 per colour byte, top byte 0
static void Dim32_Local(Bmp_t* dst, const Bmp_t* src, int level)
{
	int inv = 0x100 - level, y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x];
			d[x] = (uint32_t)(((sv & 0xff) * inv) >> 8) | (uint32_t)((((sv >> 8) & 0xff) * inv) >> 8) << 8 | (uint32_t)((((sv >> 16) & 0xff) * inv) >> 8) << 16;
		}
	}
}

/* Filter kind 1, RGB32 <- RGB32: dst = src +sat ((colour * level) >> 8)
 * per byte (the top byte too); a black colour dims instead.  Other modes
 * are left alone. */
void Blit_FilterKind1(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level)
{
	int y, x;
	uint32_t add;
	if(src->mode != PM_RGB32 || dst->mode != PM_RGB32)
		return;
	if(colour == 0)
	{
		Dim32_Local(dst, src, level);
		return;
	}
	add = (uint32_t)Sat8((int)(((colour & 0xff) * (unsigned)level) >> 8)) | (uint32_t)Sat8((int)((((colour >> 8) & 0xff) * (unsigned)level) >> 8)) << 8 | (uint32_t)Sat8((int)((((colour >> 16) & 0xff) * (unsigned)level) >> 8)) << 16;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x];
			int i, out = 0;
			for(i = 0; i < 32; i += 8)
				out |= Sat8((int)((sv >> i) & 0xff) + (int)((add >> i) & 0xff)) << i;
			d[x] = (uint32_t)out;
		}
	}
}

/* Filter kind 2 and "91 1D" mode 2: the monochrome tint.  luma = (B*28 +
 * G*151 + R*77) >> 9 selects a table entry (((luma * level) * c) >> 16)
 * << 1 per channel, added to src * inv >> 8.  RGB32 or ARGB32 with the
 * same mode on both sides; the top byte becomes 0. */
void Blit_FilterKind2(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level)
{
	int inv = 0x100 - level, y, x, i;
	int table[0x80][3];
	if(src->mode < PM_RGB32 || src->mode > PM_ARGB32 || dst->mode != src->mode)
		return;
	for(i = 0; i < 0x80; i++)
	{
		int v = (i + 1) * level, c; // (the original's running sum starts at `level`: entry i is (i + 1) * level)
		if(v == 0x8000)
			v = 0x7fff; // keeps the signed multiply positive
		for(c = 0; c < 3; c++)
		{
			int col = (int)((colour >> (c * 8)) & 0xff);
			table[i][c] = (((v * col) >> 16) << 1) & 0xffff;
		}
	}
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x];
			int b = (int)(sv & 0xff), g = (int)((sv >> 8) & 0xff), r = (int)((sv >> 16) & 0xff);
			int luma = (b * 28 + g * 151 + r * 77) >> 9;
			int out = 0;
			out |= Sat8(((b * inv) >> 8) + table[luma][0]);
			out |= Sat8(((g * inv) >> 8) + table[luma][1]) << 8;
			out |= Sat8(((r * inv) >> 8) + table[luma][2]) << 16;
			d[x] = (uint32_t)out; // top byte: 0 * inv + 0
		}
	}
}

/* Filter kind 3 and "91 1D" mode 1: s + ((s ^ colour) - s) * (level >> 1)
 * >> 7 per colour byte (the top byte keeps s's value).  RGB32 or ARGB32
 * with the same mode on both sides. */
void Blit_FilterKind3(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level)
{
	int l2 = level >> 1, y, x;
	if(src->mode < PM_RGB32 || src->mode > PM_ARGB32 || dst->mode != src->mode)
		return;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x];
			int i, out = (int)(sv & 0xff000000u);
			for(i = 0; i < 24; i += 8)
			{
				int sc = (int)((sv >> i) & 0xff), cc = (int)((colour >> i) & 0xff);
				out |= Sat8(((((sc ^ cc) - sc) * l2) >> 7) + sc) << i;
			}
			d[x] = (uint32_t)out;
		}
	}
}

/* The two remaining colour modes of "91 1D" (1.494 on): RGB32 or ARGB32
 * with the same mode on both sides, the alpha byte carried over
 * unchanged. */

// mode 3: (s * (0x100 - level) >> 8) + (colour * level >> 8) per colour byte
void Blit_TintKeepAlpha(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level)
{
	int inv = 0x100 - level, y, x, i;
	int add[3];
	if(src->mode < PM_RGB32 || src->mode > PM_ARGB32 || dst->mode != src->mode)
		return;
	for(i = 0; i < 3; i++)
		add[i] = (int)(((colour >> (8 * i)) & 0xff) * (unsigned)level) >> 8;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x];
			int out = (int)(sv & 0xff000000u);
			for(i = 0; i < 3; i++)
				out |= Sat8((int)((((sv >> (8 * i)) & 0xff) * (unsigned)inv) >> 8) + add[i]) << (8 * i);
			d[x] = (uint32_t)out;
		}
	}
}

// mode 4: s +sat (colour * level >> 8) per colour byte
void Blit_AddColour(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level)
{
	int y, x, i;
	int add[3];
	if(src->mode < PM_RGB32 || src->mode > PM_ARGB32 || dst->mode != src->mode)
		return;
	for(i = 0; i < 3; i++)
		add[i] = (int)(((colour >> (8 * i)) & 0xff) * (unsigned)level) >> 8;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x];
			int out = (int)(sv & 0xff000000u);
			for(i = 0; i < 3; i++)
				out |= Sat8((int)((sv >> (8 * i)) & 0xff) + add[i]) << (8 * i);
			d[x] = (uint32_t)out;
		}
	}
}

// ---- gray conversions --------------------------------------------------------------------------

/* "92 18": gray = (R*77 + G*151 + B*28) >> 8, times alpha / 256 for
 * ARGB32, into a GRAY8 bitmap of the same size.  0 ok, 9 the source is
 * not 32-bit, 0xa the destination is not GRAY8. */
int Blit_ToGray(Bmp_t* dst, const Bmp_t* src)
{
	int y, x;
	if(dst->mode != PM_GRAY8)
		return 0xa;
	if(src->mode != PM_RGB32 && src->mode != PM_ARGB32)
		return 9;
	for(y = 0; y < src->h; y++)
	{
		const uint8_t* s = ROW(src, y);
		uint8_t* d = ROW(dst, y);
		for(x = 0; x < src->w; x++, s += 4)
		{
			int v = s[1] * 151 + s[2] * 77 + s[0] * 28;
			if(src->mode == PM_ARGB32)
				v = (v * s[3]) >> 16;
			else
				v >>= 8;
			d[x] = (uint8_t)v;
		}
	}
	return 0;
}

// "92 19": invert a GRAY8 bitmap in place (255 - g); 1 when it was GRAY8, 0 (nothing done) otherwise
int Blit_InvertGray(Bmp_t* b)
{
	int y, x;
	if(b->mode != PM_GRAY8)
		return 0;
	for(y = 0; y < b->h; y++)
	{
		uint8_t* p = ROW(b, y);
		for(x = 0; x < b->w; x++)
			p[x] = (uint8_t)(0xff - p[x]);
	}
	return 1;
}

/* Paint `colour` (0x00RRGGBB) through a GRAY8 alpha mask of the
 * destination's size (the glyph drawing of the text layer).  RGB32:
 * d += (c - d) * (g >> 1) >> 7 where g >= 2, the top byte kept; ARGB32:
 * proper "over" with alpha g, as in Blit_AlphaCopy.  Other destination
 * modes, or a mask that is not GRAY8, are left alone. */
void Blit_ColourThroughGray(Bmp_t* dst, const Bmp_t* gray, uint32_t colour)
{
	int y, x;
	if(gray->mode != PM_GRAY8)
		return;
	if(dst->mode == PM_RGB32)
	{
		for(y = 0; y < dst->h; y++)
		{
			const uint8_t* g = ROW(gray, y);
			uint32_t* d = (uint32_t*)ROW(dst, y);
			for(x = 0; x < dst->w; x++)
			{
				int w = g[x] >> 1, i, out;
				if(!w)
					continue;
				out = (int)(d[x] & 0xff000000u);
				for(i = 0; i < 24; i += 8)
				{
					int cc = (int)((colour >> i) & 0xff), dc = (int)((d[x] >> i) & 0xff);
					out |= Sat8((((cc - dc) * w) >> 7) + dc) << i;
				}
				d[x] = (uint32_t)out;
			}
		}
	}
	else if(dst->mode == PM_ARGB32)
	{
		for(y = 0; y < dst->h; y++)
		{
			const uint8_t* g = ROW(gray, y);
			uint32_t* d = (uint32_t*)ROW(dst, y);
			for(x = 0; x < dst->w; x++)
			{
				unsigned sa = g[x], da, t, ra, fs, fd;
				int i, out;
				if(!sa)
					continue;
				if(sa == 0xff)
				{
					d[x] = (colour & 0xffffffu) | 0xff000000u;
					continue;
				}
				da = d[x] >> 24;
				t = ((0x100 - sa) * da) >> 8;
				ra = t + sa;
				fs = (sa << 8) / ra;
				fd = (t << 8) / ra;
				out = (int)(ra << 24);
				for(i = 0; i < 24; i += 8)
				{
					unsigned cc = (colour >> i) & 0xff, dc = (d[x] >> i) & 0xff;
					out |= (int)(((cc * fs + dc * fd) >> 8) & 0xff) << i;
				}
				d[x] = (uint32_t)out;
			}
		}
	}
}

// ---- blit through a gray map (background type 4, "90 19") --------------------------------------

/* RGB32 <- RGB32, param < 8: v = (gray << param) + 0x100 - ((1 << param)
 * + 1) * level; v <= 0 keeps d, v >= 0x100 takes s, otherwise
 * d + (s - d) * (v >> 1) >> 7 on B, G, R (the top byte keeps d's) */
static void ThroughGray_Small(Bmp_t* dst, const Bmp_t* src, const Bmp_t* gray, int param, int level)
{
	int base = 0x100 - ((1 << param) + 1) * level, y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		const uint8_t* g = ROW(gray, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			int v = (g[x] << param) + base, w, i, out;
			if(v <= 0)
				continue;
			if(v >= 0x100)
			{
				d[x] = s[x];
				continue;
			}
			w = v >> 1;
			out = (int)(d[x] & 0xff000000u);
			for(i = 0; i < 24; i += 8)
			{
				int sc = (int)((s[x] >> i) & 0xff), dc = (int)((d[x] >> i) & 0xff);
				out |= Sat8((((sc - dc) * w) >> 7) + dc) << i;
			}
			d[x] = (uint32_t)out;
		}
	}
}

/*
 * RGB32 <- RGB32, param >= 8 with p = param & 7: the weight table is a
 * triangle wave over the gray value with 2p + 1 segments, built in
 * floating point in the order the original uses (double precision,
 * truncating conversions).  index = gray + 2 * (0x80 - level); an index
 * <= 0 keeps d, >= 0x100 takes s, otherwise d + (s - d) * table[index]
 * >> 8 on B, G, R.
 */
static void ThroughGray_Wave(Bmp_t* dst, const Bmp_t* src, const Bmp_t* gray, int p, int level)
{
	int table[0x100];
	int bias = (0x80 - level) * 2, i, y, x;
	double seg = 256.0 / (double)(2 * p + 1); // segment length
	double k = 1.0 / seg;                     // segments per unit
	for(i = 0; i < 0x100; i++)
	{
		double t = (double)i * k;
		int n = (int)t; // _ftol truncates
		double f = (double)i - (double)n * seg;
		if(n & 1)
			f = seg - f;
		table[i] = (int)(k * f * 256.0);
	}
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		const uint8_t* g = ROW(gray, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			int idx = g[x] + bias, w, out;
			if(idx <= 0)
				continue;
			if(idx >= 0x100)
			{
				d[x] = s[x];
				continue;
			}
			w = table[idx];
			out = (int)(d[x] & 0xff000000u); // lane 3 multiplies by 0
			for(i = 0; i < 24; i += 8)
			{
				int sc = (int)((s[x] >> i) & 0xff), dc = (int)((d[x] >> i) & 0xff);
				// ((s - d) << 4) * (w << 4) >> 16 == ((s - d) * w) >> 8
				out |= Sat8((((sc - dc) * w) >> 8) + dc) << i;
			}
			d[x] = (uint32_t)out;
		}
	}
}

/* Positioned blit of src at (x, y) of dst through the GRAY8 map `gray`,
 * which must be the size of src: the map decides per pixel how much of
 * src shows, with `param` (0 .. 7 a threshold width, 8 .. 15 a triangle
 * wave) and `level` (0 .. 0x100) setting the threshold.  0 ok, 1 the
 * modes differ, 3 level above 0x100, 4 no overlap, 7 gray not GRAY8, 8
 * gray and src differ in size.  Only RGB32 pictures are drawn; other
 * matching modes return 0 untouched. */
int Blit_ThroughGray(Bmp_t* dst, int x, int y, const Bmp_t* src, const Bmp_t* gray, int param, int level)
{
	Bmp_t d = *dst, s = *src, g = *gray;
	Rect_t dr, sr;
	if(g.mode != PM_GRAY8)
		return 7;
	if(s.w != g.w || s.h != g.h)
		return 8;
	if((unsigned)level > 0x100u)
		return 3;
	Rect_FromBmp(&dr, &d);
	Rect_FromBmp(&sr, &s);
	Rect_Offset(&dr, -x, -y);
	if(!Rect_Clip(&sr, &dr))
		return 4;
	Bmp_Crop(&s, &sr);
	Bmp_Crop(&g, &sr);
	Rect_Offset(&sr, x, y);
	Bmp_Crop(&d, &sr);
	if(d.mode != s.mode)
		return 1;
	if(d.mode == PM_RGB32)
	{
		if((unsigned)param < 8)
			ThroughGray_Small(&d, &s, &g, param, level);
		else
			ThroughGray_Wave(&d, &s, &g, param & 7, level);
	}
	return 0;
}

/* The form of 1.529 on (the blitter's ninth argument): the map is aligned
 * with dst, not with src - gray (0, 0) is dst (0, 0) - and may be of any
 * size; what is drawn is the overlap of dst, src placed at (x, y) and the
 * map.  Results as Blit_ThroughGray without the size code 8. */
int Blit_ThroughGrayScreen(Bmp_t* dst, int x, int y, const Bmp_t* src, const Bmp_t* gray, int param, int level)
{
	Bmp_t d = *dst, s = *src, g = *gray;
	Rect_t r, gr;
	if(g.mode != PM_GRAY8)
		return 7;
	if((unsigned)level > 0x100u)
		return 3;
	Rect_FromBmp(&r, &s);
	Rect_Offset(&r, x, y); // the source's place in dst
	Rect_FromBmp(&gr, &d);
	if(!Rect_Clip(&r, &gr))
		return 4;
	Rect_FromBmp(&gr, &g);
	if(!Rect_Clip(&r, &gr))
		return 4;
	Bmp_Crop(&d, &r);
	Bmp_Crop(&g, &r);
	Rect_Offset(&r, -x, -y);
	Bmp_Crop(&s, &r);
	if(d.mode != s.mode)
		return 1;
	if(d.mode == PM_RGB32)
	{
		if((unsigned)param < 8)
			ThroughGray_Small(&d, &s, &g, param, level);
		else
			ThroughGray_Wave(&d, &s, &g, param & 7, level);
	}
	return 0;
}

// ---- 0xFF channel extraction -------------------------------------------------------------------

/* Effect 0xFF on 32-bit pictures: level 0 .. 2 keeps one colour channel
 * (B, G, R; the others 0, alpha 0xff), 3 spreads the alpha over R G B;
 * 4 and up behave as effect 0x00. */
void Blit_FxFF(Bmp_t* dst, const Bmp_t* src, int level)
{
	int y, x;
	if((unsigned)level >= 4u)
	{
		Blit_AlphaCopy(dst, src);
		return;
	}
	if(src->mode != PM_RGB32 && src->mode != PM_ARGB32)
		return;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t v = s[x], a;
			switch(level)
			{
				case 0: d[x] = (v & 0xff) | 0xff000000u; break;
				case 1: d[x] = (v & 0xff00) | 0xff000000u; break;
				case 2: d[x] = (v & 0xffff0000u) | 0xff000000u; break;
				default:
					a = v >> 24;
					d[x] = 0xff000000u | a << 16 | a << 8 | a;
					break;
			}
		}
	}
}
