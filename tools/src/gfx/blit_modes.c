/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * blit_modes.c - the arithmetic blend modes of Bmp_BlitEffect
 * (inc/bgi/gfx/bitmap.h):
 *
 *   0x03 / 0x22  Blit_Sub   saturating subtract         (ARGB32 -> RGB32)
 *   0x04 / 0x23  Blit_Mul   multiply                    (RGB32, ARGB32, GRAY8)
 *   0x06 / 0x24  Blit_Fx06  screen                      (RGB32, ARGB32 -> RGB32)
 *   0x07 / 0x25  Blit_Fx07  alpha erase                 (ARGB32 -> RGB32/ARGB32)
 *   0x08 / 0x26  Blit_Fx08  overlay (destination picks) (RGB32, ARGB32 -> RGB32)
 *   0x09 / 0x27  Blit_Fx09  hard light (source picks)   (RGB32, ARGB32 -> RGB32)
 *
 * The 0x2x numbers are the same routines called with 0x100 - level.  Every
 * routine repeats the MMX code of the original step by step: 16-bit lanes,
 * a weight table indexed by the top seven bits of the source alpha
 * ("a7" = alpha >> 1), pmullw/psraw 7 or pmulhw and packuswb.  The weight
 * tables are
 *
 *   level <  0x100 : w[a7] = (a7 * level) >> 8           (0 .. 126)
 *   level >= 0x100 : w[a7] = a7                           (the identity table)
 *
 * with the alpha lane always weighted 0, so the top byte of an RGB32
 * destination keeps its old value.  Other mode combinations than the ones
 * listed draw nothing, as does a level of 0.
 */
#include "bgi/gfx/bitmap.h"

// pack a lane to a byte with unsigned saturation (packuswb)
static inline uint8_t Sat8(int v)
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255
										 : v);
}

/* pmullw + psraw 7 on one lane: the low 16 bits of the product, shifted
 * arithmetically.  Products never exceed 16 bits in these routines except
 * in the odd-level lanes of fx08/fx09, which is why the truncation is kept. */
static inline int Mul7(int a, int w)
{
	return (int)(int16_t)(uint16_t)(a * w) >> 7;
}

#define ROW(b, y) ((b)->pixels + (size_t)(y) * (size_t)(b)->pitch)

// alpha weight of an ARGB32 source pixel for the given level (the tables of the header)
static inline int AlphaWeight(uint32_t sv, int level)
{
	int a7 = (int)(sv >> 25);
	return level >= 0x100 ? a7 : (a7 * level) >> 8;
}

// ========================================================================
// 0x03 subtract
// ========================================================================

/* RGB32 <- ARGB32: for every source pixel with alpha >= 2
 *   t   = (s * w) >> 7           per lane (B, G, R; alpha lane 0)
 *   d   = sat_sub(d, t)          per byte (psubusb), top byte unchanged */
static void Sub32(Bmp_t* dst, const Bmp_t* src, int level)
{
	int y, x, i;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x], out = dv & 0xff000000u;
			int w;
			if(!(sv & 0xfe000000u))
				continue;
			w = AlphaWeight(sv, level);
			for(i = 0; i < 24; i += 8)
			{
				int t = Sat8((int)((sv >> i) & 0xff) * w >> 7);
				int dc = (int)((dv >> i) & 0xff) - t;
				out |= (uint32_t)(dc < 0 ? 0 : dc) << i;
			}
			d[x] = out;
		}
	}
}

// effects 0x03 / 0x22: subtract the alpha-weighted source from an RGB32 destination; ARGB32 sources only
void Blit_Sub(Bmp_t* dst, const Bmp_t* src, int level)
{
	if(level == 0)
		return;
	if(src->mode == PM_ARGB32 && dst->mode == PM_RGB32)
		Sub32(dst, src, level);
}

// ========================================================================
// 0x04 multiply
// ========================================================================

/* RGB32 <- RGB32, all four lanes
 *   d + (((s - 256) * (level >> 1)) * (2 d)) >> 16        (pmulhw)
 * which is d + (s - 256) * d * level / 65536, i.e. s * d / 256 at full level */
static void Mul32_Plain(Bmp_t* dst, const Bmp_t* src, int level)
{
	int l2 = level >> 1, y, x, i;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x], out = 0;
			for(i = 0; i < 32; i += 8)
			{
				int sc = (int)((sv >> i) & 0xff), dc = (int)((dv >> i) & 0xff);
				int p = (int16_t)((sc - 256) * l2); // fits: >= -32768
				int hi = (p * (dc << 1)) >> 16;     // signed high word
				out |= (uint32_t)Sat8(dc + hi) << i;
			}
			d[x] = out;
		}
	}
}

/* RGB32 <- ARGB32: for alpha >= 2,
 *   d + ((((s * d) >> 8) - d) * w) >> 7        (B, G, R; top byte -> d) */
static void Mul32_Alpha(Bmp_t* dst, const Bmp_t* src, int level)
{
	int y, x, i;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x], out = dv & 0xff000000u;
			int w;
			if(!(sv & 0xfe000000u))
				continue;
			w = AlphaWeight(sv, level);
			for(i = 0; i < 24; i += 8)
			{
				int sc = (int)((sv >> i) & 0xff), dc = (int)((dv >> i) & 0xff);
				int m = (sc * dc) >> 8; // (s<<4 * d<<4) >> 16
				out |= (uint32_t)Sat8(dc + Mul7(m - dc, w)) << i;
			}
			d[x] = out;
		}
	}
}

// GRAY8 <- GRAY8: d = (d * s) / 255 (the level is ignored)
static void Mul8(Bmp_t* dst, const Bmp_t* src)
{
	int y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint8_t* s = ROW(src, y);
		uint8_t* d = ROW(dst, y);
		for(x = 0; x < src->w; x++)
			d[x] = (uint8_t)((d[x] * s[x]) / 255);
	}
}

/* Effects 0x04 / 0x23: multiply the destination by the source, mixed in by
 * `level`.  RGB32 <- RGB32, RGB32 <- ARGB32 (weighted by the alpha) and
 * GRAY8 <- GRAY8 (always at full strength). */
void Blit_Mul(Bmp_t* dst, const Bmp_t* src, int level)
{
	if(level == 0)
		return;
	switch(src->mode)
	{
		case PM_RGB32:
			if(dst->mode == PM_RGB32)
				Mul32_Plain(dst, src, level);
			break;
		case PM_ARGB32:
			if(dst->mode == PM_RGB32)
				Mul32_Alpha(dst, src, level);
			break;
		case PM_GRAY8:
			if(dst->mode == PM_GRAY8)
				Mul8(dst, src);
			break;
		default:
			break;
	}
}

// ========================================================================
// 0x06 screen
// ========================================================================

// one lane of  d + s' - ((d * s') >> 8)  with 16-bit saturation (psubsw)
static inline int Screen(int dc, int s2)
{
	int v = dc + s2 - ((dc * s2) >> 8);
	return v < -32768 ? -32768 : v > 32767 ? 32767
										   : v;
}

// RGB32 <- RGB32, four lanes, s' = (s * level) >> 8
static void Screen32_Plain(Bmp_t* dst, const Bmp_t* src, int level)
{
	int y, x, i;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x], out = 0;
			for(i = 0; i < 32; i += 8)
			{
				int s2 = ((int)((sv >> i) & 0xff) * level) >> 8;
				out |= (uint32_t)Sat8(Screen((int)((dv >> i) & 0xff), s2)) << i;
			}
			d[x] = out;
		}
	}
}

// RGB32 <- ARGB32: alpha >= 2, s' = (s * w) >> 7, alpha lane 0
static void Screen32_Alpha(Bmp_t* dst, const Bmp_t* src, int level)
{
	int y, x, i;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x], out = dv & 0xff000000u;
			int w;
			if(!(sv & 0xfe000000u))
				continue;
			w = AlphaWeight(sv, level);
			for(i = 0; i < 24; i += 8)
			{
				int s2 = ((int)((sv >> i) & 0xff) * w) >> 7;
				out |= (uint32_t)Sat8(Screen((int)((dv >> i) & 0xff), s2)) << i;
			}
			d[x] = out;
		}
	}
}

// effects 0x06 / 0x24: screen (d + s - d * s) onto an RGB32 destination from an RGB32 or ARGB32 source
void Blit_Fx06(Bmp_t* dst, const Bmp_t* src, int level)
{
	if(level == 0 || dst->mode != PM_RGB32)
		return;
	if(src->mode == PM_RGB32)
		Screen32_Plain(dst, src, level);
	else if(src->mode == PM_ARGB32)
		Screen32_Alpha(dst, src, level);
}

// ========================================================================
// 0x07 alpha erase
// ========================================================================

/* RGB32 <- ARGB32: pixels with alpha != 0 are darkened by
 *   v = 0x100 - ((a * level) >> 8);  d = (d * v) >> 8   (B, G, R; top -> 0) */
static void Erase32_To1(Bmp_t* dst, const Bmp_t* src, int level)
{
	int y, x, i;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x], out = 0;
			int v;
			if(!(sv & 0xff000000u))
				continue;
			v = 0x100 - (((int)(sv >> 24) * level) >> 8);
			for(i = 0; i < 24; i += 8)
				out |= (uint32_t)(((int)((dv >> i) & 0xff) * v) >> 8) << i;
			d[x] = out;
		}
	}
}

/* ARGB32 <- ARGB32: only the destination alpha is
 * reduced, by the (level-scaled) source alpha: d.a = (d.a * v) >> 8 with
 * v = 0x100 - ((a * level) >> 8), or 0x100 - a at full level */
static void Erase32_To2(Bmp_t* dst, const Bmp_t* src, int level)
{
	int y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint8_t* s = ROW(src, y);
		uint8_t* d = ROW(dst, y);
		for(x = 0; x < src->w; x++, s += 4, d += 4)
		{
			int a = s[3], v;
			if(!a)
				continue;
			v = level < 0x100 ? 0x100 - ((a * level) >> 8) : 0x100 - a;
			d[3] = (uint8_t)((d[3] * v) >> 8);
		}
	}
}

/* Effects 0x07 / 0x25: erase the destination by the source's alpha -
 * darken the colour of an RGB32 destination, reduce the alpha of an ARGB32
 * one.  ARGB32 sources only. */
void Blit_Fx07(Bmp_t* dst, const Bmp_t* src, int level)
{
	if(level == 0 || src->mode != PM_ARGB32)
		return;
	if(dst->mode == PM_RGB32)
		Erase32_To1(dst, src, level);
	else if(dst->mode == PM_ARGB32)
		Erase32_To2(dst, src, level);
}

// ========================================================================
// 0x08 overlay and 0x09 hard light
// ========================================================================

/* The core of both effects: the "multiply below the middle, screen above"
 * curve on one lane; `sel` is the lane that chooses (d for overlay, s for
 * hard light).  The high branch is 255 - ((256 - s)(256 - d) >> 7), which
 * can reach -1 and is then saturated to 0 by packuswb. */
static inline int OverlayLane(int sc, int dc, int sel)
{
	if(sel > 127)
		return 255 - (((256 - sc) * (256 - dc)) >> 7);
	return (sc * dc) >> 7;
}

// full level, RGB32 <- RGB32: all four lanes replaced by the curve
static void Overlay32_Full(Bmp_t* dst, const Bmp_t* src, int bySource)
{
	int y, x, i;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x], out = 0;
			for(i = 0; i < 32; i += 8)
			{
				int sc = (int)((sv >> i) & 0xff), dc = (int)((dv >> i) & 0xff);
				out |= (uint32_t)Sat8(OverlayLane(sc, dc, bySource ? sc : dc)) << i;
			}
			d[x] = out;
		}
	}
}

/* level < 0x100, RGB32 <- RGB32
 *   d + ((ov - d) * w) >> 7   with the lane weights built from
 *   level * 0x80008000 + (level >> 1), i.e.
 *     lanes B, G : (level >> 1) + ((level & 1) << 15)
 *     lane  R    :  level >> 1
 *     lane  A    :  0
 * For odd levels the B and G weights have bit 15 set, which the signed
 * multiply treats as negative; the original shows this artefact and so
 * does this code (Mul7 truncates to 16 bits like pmullw). */
static void Overlay32_Level(Bmp_t* dst, const Bmp_t* src, int level, int bySource)
{
	int w[4], y, x, i;
	w[0] = w[1] = (level >> 1) + ((level & 1) << 15);
	w[2] = level >> 1;
	w[3] = 0;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x], out = 0;
			for(i = 0; i < 4; i++)
			{
				int sc = (int)((sv >> (8 * i)) & 0xff), dc = (int)((dv >> (8 * i)) & 0xff);
				int ov = OverlayLane(sc, dc, bySource ? sc : dc);
				int v = (int)(int16_t)(dc + Mul7(ov - dc, w[i])); // paddw wraps
				out |= (uint32_t)Sat8(v) << (8 * i);
			}
			d[x] = out;
		}
	}
}

// RGB32 <- ARGB32, alpha >= 2: d + ((ov - d) * w) >> 7 on B, G, R; the top byte keeps d's value
static void Overlay32_Alpha(Bmp_t* dst, const Bmp_t* src, int level, int bySource)
{
	int y, x, i;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x], out = dv & 0xff000000u;
			int w;
			if(!(sv & 0xfe000000u))
				continue;
			w = AlphaWeight(sv, level);
			for(i = 0; i < 24; i += 8)
			{
				int sc = (int)((sv >> i) & 0xff), dc = (int)((dv >> i) & 0xff);
				int ov = OverlayLane(sc, dc, bySource ? sc : dc);
				out |= (uint32_t)Sat8(dc + Mul7(ov - dc, w)) << i;
			}
			d[x] = out;
		}
	}
}

// the dispatch shared by 0x08 (bySource 0) and 0x09 (bySource 1): RGB32 destinations only
static void Overlay(Bmp_t* dst, const Bmp_t* src, int level, int bySource)
{
	if(level == 0 || dst->mode != PM_RGB32)
		return;
	if(src->mode == PM_RGB32)
	{
		if((uint32_t)level >= 0x100u)
			Overlay32_Full(dst, src, bySource);
		else
			Overlay32_Level(dst, src, level, bySource);
	}
	else if(src->mode == PM_ARGB32)
	{
		Overlay32_Alpha(dst, src, level, bySource);
	}
}

// effects 0x08 / 0x26: overlay - the destination lane chooses multiply or screen
void Blit_Fx08(Bmp_t* dst, const Bmp_t* src, int level)
{
	Overlay(dst, src, level, 0);
}

// effects 0x09 / 0x27: hard light - the source lane chooses multiply or screen
void Blit_Fx09(Bmp_t* dst, const Bmp_t* src, int level)
{
	Overlay(dst, src, level, 1);
}
