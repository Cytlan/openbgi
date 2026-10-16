/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * blit.c - the effect routines of Bmp_BlitEffect (inc/bgi/gfx/bitmap.h):
 * copies, alpha blending, cross fades, additive drawing, punch-through,
 * dimming and tinting.  The arithmetic blend modes (subtract, multiply,
 * screen, overlay ...) are in blit_modes.c; the filters, gray conversions
 * and the blit through a gray map in blit_filter.c; the displacement and
 * blur in blit_displace.c; the ripples in blit_ripple.c; the rotation,
 * scaled copy and mosaic in blit_rotate.c; the masks, wipes, transforms
 * and skew in blit_xform.c; the whole-picture scalers in blit_scale.c.
 *
 * Reading guide for the pixel formats
 *   RGB16   : 16-bit words, R = bits 10..14, G = 5..9, B = 0..4
 *   RGB32   : bytes B, G, R, x
 *   ARGB32  : bytes B, G, R, A
 * "level" is 0..0x100; the MMX code of the original scales through
 * tables that hold level >> 1 or (i * level) >> 8 in 16-bit lanes and then
 * shifts by 7 or 8; the C code below repeats those exact steps.
 *
 * All multiplies that the original performs with pmullw/psraw (signed
 * 16-bit lanes) are done here in int with an arithmetic shift right, which
 * yields the same floor division for negative intermediates.
 *
 * None of the routines clips or checks sizes: the caller hands them two
 * views of the same size (Bmp_Blit does that for a positioned blit).  A
 * routine given a mode combination it does not implement draws nothing.
 */
#include "bgi/gfx/bitmap.h"

// pack a lane to a byte with unsigned saturation (packuswb)
static inline uint8_t Sat8(int v)
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255
										 : v);
}

#define ROW(b, y) ((b)->pixels + (size_t)(y) * (size_t)(b)->pitch)

// ========================================================================
// 0x80 raw copy and 0x00 alpha copy
// ========================================================================

// ARGB32 <- RGB32: the colour bytes with the alpha forced to 0xff
static void Copy32_SetAlpha(Bmp_t* dst, const Bmp_t* src)
{
	int y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
			d[x] = s[x] | 0xff000000u;
	}
}

// RGB32 <- ARGB32: every colour byte * (alpha >> 1) >> 7, the top byte -> 0
static void Copy32_Premultiply(Bmp_t* dst, const Bmp_t* src)
{
	int y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t v = s[x];
			int a = (int)(v >> 25);
			d[x] = (uint32_t)(((v & 0xff) * a) >> 7) | (uint32_t)((((v >> 8) & 0xff) * a) >> 7) << 8 | (uint32_t)((((v >> 16) & 0xff) * a) >> 7) << 16;
		}
	}
}

/* Effect 0x80: a raw copy between equal modes; between the two 32-bit
 * modes the alpha is added (0xff) or multiplied into the colour. */
void Blit_Copy(Bmp_t* dst, const Bmp_t* src)
{
	if(dst->mode == src->mode)
		Bmp_CopyRect(dst, src);
	else if(src->mode == PM_RGB32)
		Copy32_SetAlpha(dst, src); // ARGB32 <- RGB32
	else if(src->mode == PM_ARGB32)
		Copy32_Premultiply(dst, src); // RGB32 <- ARGB32
}

// RGB16 <- RGB16: copy the non-zero pixels (0 is the colour key)
static void AlphaCopy16(Bmp_t* dst, const Bmp_t* src)
{
	int y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint16_t* s = (const uint16_t*)ROW(src, y);
		uint16_t* d = (uint16_t*)ROW(dst, y);
		for(x = src->w - 1; x >= 0; x--)
			if(s[x])
				d[x] = s[x];
	}
}

/* RGB32 <- ARGB32: d += (s - d) * (a >> 1) >> 7 for pixels with
 * alpha >= 2 (lanes B G R; the top byte keeps d's value) */
static void AlphaCopy32_To1(Bmp_t* dst, const Bmp_t* src)
{
	int y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x];
			int a = (int)(sv >> 25), i, out = (int)(dv & 0xff000000u);
			if(!(sv & 0xfe000000u))
				continue;
			for(i = 0; i < 24; i += 8)
			{
				int sc = (int)((sv >> i) & 0xff), dc = (int)((dv >> i) & 0xff);
				out |= Sat8((((sc - dc) * a) >> 7) + dc) << i;
			}
			d[x] = (uint32_t)out;
		}
	}
}

/* ARGB32 <- ARGB32: "over" with the destination alpha.  With sa, da the
 * two alphas, t = (256 - sa) * da / 256 is the remaining destination
 * weight, ra = sa + t the resulting alpha and the colour is
 * (s * sa + d * t) / ra; a source alpha of 0xff copies, 0 skips. */
static void AlphaCopy32_To2(Bmp_t* dst, const Bmp_t* src)
{
	int y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x];
			unsigned sa = sv >> 24, da, t, ra, fs, fd;
			int i, out;
			if(!(sv & 0xff000000u))
				continue;
			if(sa == 0xff)
			{
				d[x] = sv;
				continue;
			}
			da = dv >> 24;
			t = ((0x100 - sa) * da) >> 8; // remaining destination weight
			ra = t + sa;                  // resulting alpha
			fs = (sa << 8) / ra;
			fd = (t << 8) / ra;
			out = (int)(ra << 24);
			for(i = 0; i < 24; i += 8)
			{
				unsigned sc = (sv >> i) & 0xff, dc = (dv >> i) & 0xff;
				out |= (int)(((sc * fs + dc * fd) >> 8) & 0xff) << i;
			}
			d[x] = (uint32_t)out;
		}
	}
}

/* Effect 0x00: the copy that honours transparency - the colour key 0 of
 * RGB16 and the alpha of ARGB32.  RGB16 <- RGB16, RGB32 <- RGB32 (a plain
 * copy), ARGB32 <- RGB32 (alpha 0xff), RGB32 <- ARGB32 and ARGB32 <-
 * ARGB32 (composited). */
void Blit_AlphaCopy(Bmp_t* dst, const Bmp_t* src)
{
	switch(src->mode)
	{
		case PM_RGB16:
			AlphaCopy16(dst, src);
			break;
		case PM_RGB32:
			if(dst->mode == PM_RGB32)
				Bmp_CopyRect(dst, src);
			else if(dst->mode == PM_ARGB32)
				Copy32_SetAlpha(dst, src);
			break;
		case PM_ARGB32:
			if(dst->mode == PM_RGB32)
				AlphaCopy32_To1(dst, src);
			else if(dst->mode == PM_ARGB32)
				AlphaCopy32_To2(dst, src);
			break;
		default:
			break;
	}
}

// ========================================================================
// 0x01 / 0x20 blend (level = transparency) and 0xF0 over
// ========================================================================

/* RGB16 <- RGB16 blend: (s * inv + d * level) >> 8 per channel,
 * computed on the masked channel bits; skipZero skips source pixels that
 * are 0 (effect 0x01), 0xF0 draws them too */
static void Blend16(Bmp_t* dst, const Bmp_t* src, int level, int skipZero)
{
	int inv = 0x100 - level, y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint16_t* s = (const uint16_t*)ROW(src, y);
		uint16_t* d = (uint16_t*)ROW(dst, y);
		for(x = src->w - 1; x >= 0; x--)
		{
			unsigned sv = s[x], dv = d[x], g, r, b;
			if(skipZero && sv == 0)
				continue;
			g = ((((sv & 0x3e0) * inv) + ((dv & 0x3e0) * level)) >> 8) & 0x3e0;
			r = ((((sv & 0x7c00) * inv) + ((dv & 0x7c00) * level)) >> 8) & 0x7c00;
			b = (((sv & 0x1f) * inv) + ((dv & 0x1f) * level)) >> 8;
			d[x] = (uint16_t)(g + r + b);
		}
	}
}

/* 32-bit <- 32-bit blend ignoring alpha: every byte becomes
 * s + ((d - s) * (level >> 1) >> 7); the top byte of the destination is
 * replaced by the source's (its lane multiplies by 0) */
static void Blend32_Plain(Bmp_t* dst, const Bmp_t* src, int level)
{
	int l2 = level >> 1, y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x];
			int i, out = (int)(sv & 0xff000000u);
			for(i = 0; i < 24; i += 8)
			{
				int sc = (int)((sv >> i) & 0xff), dc = (int)((dv >> i) & 0xff);
				out |= Sat8((((dc - sc) * l2) >> 7) + sc) << i;
			}
			d[x] = (uint32_t)out;
		}
	}
}

/* ARGB32 <- RGB32 blend: the source is opaque with weight
 * inv = 0x100 - level; the destination alpha takes part as in
 * AlphaCopy32_To2, with 16-bit fractions */
static void Blend32_1to2(Bmp_t* dst, const Bmp_t* src, int level)
{
	unsigned inv = (unsigned)(0x100 - level), sw = inv * 255; // source weight, 8.8
	int y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x];
			unsigned da = dv >> 24;
			unsigned dw = ((0x10000 - sw) * da) >> 8;
			unsigned total = dw + sw;
			unsigned fs = (sw << 16) / total, fd = (dw << 16) / total;
			int i, out = (int)((total >> 8) << 24);
			for(i = 0; i < 24; i += 8)
			{
				unsigned sc = (sv >> i) & 0xff, dc = (dv >> i) & 0xff;
				out |= (int)(((sc * fs + dc * fd) >> 16) & 0xff) << i;
			}
			d[x] = (uint32_t)out;
		}
	}
}

// RGB32 <- ARGB32 blend: d += (s - d) * w >> 7 with w = ((a >> 1) * inv) >> 8, alpha >= 2 only
static void Blend32_2to1(Bmp_t* dst, const Bmp_t* src, int level)
{
	int inv = 0x100 - level, y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x];
			int w, i, out = (int)(dv & 0xff000000u);
			if(!(sv & 0xfe000000u))
				continue;
			w = ((int)(sv >> 25) * inv) >> 8;
			for(i = 0; i < 24; i += 8)
			{
				int sc = (int)((sv >> i) & 0xff), dc = (int)((dv >> i) & 0xff);
				out |= Sat8((((sc - dc) * w) >> 7) + dc) << i;
			}
			d[x] = (uint32_t)out;
		}
	}
}

/* ARGB32 <- ARGB32 blend: "over" (AlphaCopy32_To2) with the source alpha
 * first scaled by inv = 0x100 - level; a result alpha of 0 is bumped to 1
 * before the divisions */
static void Blend32_2to2(Bmp_t* dst, const Bmp_t* src, int level)
{
	unsigned inv = (unsigned)(0x100 - level);
	int y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x], out;
			unsigned sa, da, t, ra, fs, fd;
			int i;
			if(!(sv & 0xff000000u))
				continue;
			// mul by inv, high dword: (A * inv) >> 8 including the carry
			// from the colour bytes
			sa = (unsigned)(((uint64_t)sv * inv) >> 32);
			da = dv >> 24;
			t = ((0x100 - sa) * da) >> 8;
			ra = t + sa;
			if(ra < 1)
				ra++;
			fs = (sa << 8) / ra;
			fd = (t << 8) / ra;
			out = 0;
			for(i = 0; i < 24; i += 8)
			{
				unsigned sc = (sv >> i) & 0xff, dc = (dv >> i) & 0xff;
				out |= (((sc * fs + dc * fd) >> 8) & 0xff) << i;
			}
			out |= (uint32_t)Sat8((int)ra) << 24;
			d[x] = out;
		}
	}
}

/* Effects 0x01 / 0x20: src over dst at transparency `level` (0 opaque:
 * Blit_AlphaCopy; 0x100 and above: nothing).  RGB16 <- RGB16 (colour key
 * 0), RGB32 <- RGB32, ARGB32 <- RGB32, RGB32 <- ARGB32, ARGB32 <-
 * ARGB32. */
void Blit_Blend(Bmp_t* dst, const Bmp_t* src, int level)
{
	if(level <= 0)
	{
		Blit_AlphaCopy(dst, src);
		return;
	}
	if(level >= 0x100)
		return;
	switch(src->mode)
	{
		case PM_RGB16:
			if(dst->mode == PM_RGB16)
				Blend16(dst, src, level, 1);
			break;
		case PM_RGB32:
			if(dst->mode == PM_RGB32)
				Blend32_Plain(dst, src, level);
			else if(dst->mode == PM_ARGB32)
				Blend32_1to2(dst, src, level);
			break;
		case PM_ARGB32:
			if(dst->mode == PM_RGB32)
				Blend32_2to1(dst, src, level);
			else if(dst->mode == PM_ARGB32)
				Blend32_2to2(dst, src, level);
			break;
		default:
			break;
	}
}

/* Effect 0xF0: the blend without the colour key / alpha - every pixel is
 * mixed by `level` (0: Blit_Copy; 0x100 and above: nothing).  RGB16 and
 * the two 32-bit modes (Blend32_Plain, whatever the destination mode). */
void Blit_Over(Bmp_t* dst, const Bmp_t* src, int level)
{
	if(level <= 0)
	{
		Blit_Copy(dst, src);
		return;
	}
	if(level >= 0x100)
		return;
	if(src->mode == PM_RGB16)
		Blend16(dst, src, level, 0);
	else if(src->mode == PM_RGB32 || src->mode == PM_ARGB32)
		Blend32_Plain(dst, src, level);
}

// ========================================================================
// cross fades (sprite drawing mode 1)
// ========================================================================

/* dst (RGB32) = lerp(a * alpha_a, b * alpha_b, ratio) over dst,
 * with both alphas scaled by inv = 0x100 - level: with va, vb the scaled
 * alphas and ra their lerp, the colour is lerp(a * va, b * vb, ratio) +
 * d * (256 - ra) / 256 */
static void Crossfade32(Bmp_t* dst, const Bmp_t* a, const Bmp_t* b, int ratio, int level)
{
	int inv = 0x100 - level, y, x;
	for(y = 0; y < a->h; y++)
	{
		const uint32_t* pa = (const uint32_t*)ROW(a, y);
		const uint32_t* pb = (const uint32_t*)ROW(b, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < a->w; x++)
		{
			uint32_t av = pa[x], bv = pb[x], dv = d[x];
			int va = ((int)(av >> 24) * inv) >> 8; // table 1: (alpha * inv) >> 8
			int vb = ((int)(bv >> 24) * inv) >> 8;
			int ra = va + (((vb - va) * ratio) >> 8); // alpha lane of the lerp
			int i, out, wd;
			if(ra == 0)
				continue;
			wd = 0x100 - ra;
			out = 0;
			for(i = 0; i < 24; i += 8)
			{
				int ac = (int)((av >> i) & 0xff), bc = (int)((bv >> i) & 0xff);
				int dc = (int)((dv >> i) & 0xff);
				int a1 = (ac * va) >> 8, b1 = (bc * vb) >> 8;
				int c = a1 + (((b1 - a1) * ratio) >> 8);
				c += (dc * wd) >> 8;
				out |= Sat8(c) << i;
			}
			d[x] = (uint32_t)out;
		}
	}
}

/* Draw the cross fade of two ARGB32 pictures (ratio 0 = a, 0x100 = b) over
 * an RGB32 destination at transparency `level`; a level of 0x100 or more
 * draws nothing.  0 ok, 9 a or b is not ARGB32, 0xa dst is not RGB32. */
int Blit_Crossfade(Bmp_t* dst, const Bmp_t* a, const Bmp_t* b, int ratio, int level)
{
	if(dst->mode != PM_RGB32)
		return 0xa;
	if(a->mode != PM_ARGB32 || b->mode != PM_ARGB32)
		return 9;
	if((unsigned)level < 0x100u)
		Crossfade32(dst, a, b, ratio, level);
	return 0;
}

// RGB32: tmp = a + ((b - a) * ratio >> 8), all four bytes
static void CrossfadeTo32(Bmp_t* tmp, const Bmp_t* a, const Bmp_t* b, int ratio)
{
	int y, x;
	for(y = 0; y < a->h; y++)
	{
		const uint32_t* pa = (const uint32_t*)ROW(a, y);
		const uint32_t* pb = (const uint32_t*)ROW(b, y);
		uint32_t* d = (uint32_t*)ROW(tmp, y);
		for(x = 0; x < a->w; x++)
		{
			uint32_t av = pa[x], bv = pb[x];
			int i, out = 0;
			for(i = 0; i < 32; i += 8)
			{
				int ac = (int)((av >> i) & 0xff), bc = (int)((bv >> i) & 0xff);
				out |= Sat8(ac + (((bc - ac) * ratio) >> 8)) << i;
			}
			d[x] = (uint32_t)out;
		}
	}
}

/* ARGB32: alpha-weighted mix of a and b.  wa = alpha_a * (256 - ratio),
 * wb = alpha_b * ratio; the result alpha is (wa + wb) >> 8 (a 16-bit lane,
 * zero when both weights are) and the colour b + (a - b) * w >> 7 with
 * w = wa * 128 / (wa + wb) */
static void CrossfadeTo32A(Bmp_t* tmp, const Bmp_t* a, const Bmp_t* b, int ratio)
{
	int inv = 0x100 - ratio, y, x;
	for(y = 0; y < a->h; y++)
	{
		const uint32_t* pa = (const uint32_t*)ROW(a, y);
		const uint32_t* pb = (const uint32_t*)ROW(b, y);
		uint32_t* d = (uint32_t*)ROW(tmp, y);
		for(x = 0; x < a->w; x++)
		{
			uint32_t av = pa[x], bv = pb[x];
			unsigned wa = (av >> 24) * (unsigned)inv, wb = (bv >> 24) * (unsigned)ratio;
			unsigned sum = (wa + wb) & 0xffffu; // 16-bit lane
			int i, out, w;
			if(sum == 0)
			{
				d[x] = 0;
				continue;
			}
			w = (int)((wa << 7) / sum); // 0..0x80
			out = (int)(((sum >> 8) & 0xff) << 24);
			for(i = 0; i < 24; i += 8)
			{
				int ac = (int)((av >> i) & 0xff), bc = (int)((bv >> i) & 0xff);
				out |= Sat8(bc + (((ac - bc) * w) >> 7)) << i;
			}
			d[x] = (uint32_t)out;
		}
	}
}

/* Write the cross fade of a and b (ratio 0 = a, 0x100 = b) into tmp; all
 * three must share one mode, RGB32 (a plain lerp) or ARGB32 (weighted by
 * the alphas).  0 ok, 9 the modes differ, 0xa tmp is not 32-bit. */
int Blit_CrossfadeTo(Bmp_t* tmp, const Bmp_t* a, const Bmp_t* b, int ratio)
{
	if(tmp->mode != PM_RGB32 && tmp->mode != PM_ARGB32)
		return 0xa;
	if(a->mode != b->mode || a->mode != tmp->mode)
		return 9;
	if(tmp->mode == PM_RGB32)
		CrossfadeTo32(tmp, a, b, ratio);
	else
		CrossfadeTo32A(tmp, a, b, ratio);
	return 0;
}

// ========================================================================
// 0x02 / 0x21 additive
// ========================================================================

/* RGB16 <- RGB16: d += (s * level) >> 8 per channel, saturated, skipping
 * zero source pixels */
static void Add16(Bmp_t* dst, const Bmp_t* src, int level)
{
	int y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint16_t* s = (const uint16_t*)ROW(src, y);
		uint16_t* d = (uint16_t*)ROW(dst, y);
		for(x = src->w - 1; x >= 0; x--)
		{
			unsigned sv = s[x], dv = d[x], b, g, r;
			if(sv == 0)
				continue;
			b = (((sv & 0x1f) * level) >> 8) + (dv & 0x1f);
			if(b >= 0x20)
				b = 0x1f;
			g = (((sv & 0x3e0) * level) >> 8) + (dv & 0x3e0);
			g = g >= 0x400 ? 0x3e0 : (g & 0x3e0);
			r = (((sv & 0x7c00) * level) >> 8) + (dv & 0x7c00);
			r = r >= 0x8000 ? 0x7c00 : (r & 0x7c00);
			d[x] = (uint16_t)(b + g + r);
		}
	}
}

/* 32-bit <- RGB32: bytes B G R += (s * level) >> 8
 * (saturated) when the source is not black; ARGB32 destinations also add
 * level to the alpha byte */
static void Add32_From1(Bmp_t* dst, const Bmp_t* src, int level, int withAlpha)
{
	int y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint8_t* s = ROW(src, y);
		uint8_t* d = ROW(dst, y);
		for(x = 0; x < src->w; x++, s += 4, d += 4)
		{
			int i;
			if(s[0] + s[1] + s[2] <= 0)
				continue;
			for(i = 0; i < 3; i++)
			{
				int v = ((s[i] * level) >> 8) + d[i];
				d[i] = (uint8_t)(v >= 0x100 ? 0xff : v);
			}
			if(withAlpha)
			{
				int v = d[3] + level;
				d[3] = (uint8_t)(v >= 0x100 ? 0xff : v);
			}
		}
	}
}

/* 32-bit <- ARGB32: d += Sat8((s * w(a)) >> 7) where
 * w(a) = ((a >> 1) * level) >> 8 (or a >> 1 when level is 0x100), for
 * pixels with alpha >= 2.  An ARGB32 destination adds min(255, 2 * w) to
 * its alpha; an RGB32 one keeps its top byte. */
static void Add32_From2(Bmp_t* dst, const Bmp_t* src, int level, int withAlpha)
{
	int y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x];
			int a = (int)(sv >> 25), w, i, out = 0;
			if(!(sv & 0xfe000000u))
				continue;
			w = level >= 0x100 ? a : (a * level) >> 8;
			for(i = 0; i < 24; i += 8)
			{
				int sc = (int)((sv >> i) & 0xff), dc = (int)((dv >> i) & 0xff);
				out |= Sat8(dc + Sat8((sc * w) >> 7)) << i;
			}
			if(withAlpha)
				out |= Sat8((int)(dv >> 24) + Sat8(w << 1)) << 24;
			else
				out |= (int)(dv & 0xff000000u);
			d[x] = (uint32_t)out;
		}
	}
}

/* Effects 0x02 / 0x21: add src scaled by `level` / 256 to dst with
 * saturation (level 0: nothing).  RGB16 <- RGB16 and every 32-bit
 * combination; an ARGB32 source is weighted by its alpha. */
void Blit_Add(Bmp_t* dst, const Bmp_t* src, int level)
{
	if(level <= 0)
		return;
	switch(src->mode)
	{
		case PM_RGB16:
			Add16(dst, src, level);
			break;
		case PM_RGB32:
			if(dst->mode == PM_RGB32)
				Add32_From1(dst, src, level, 0);
			else if(dst->mode == PM_ARGB32)
				Add32_From1(dst, src, level, 1);
			break;
		case PM_ARGB32:
			if(dst->mode == PM_RGB32)
				Add32_From2(dst, src, level, 0);
			else if(dst->mode == PM_ARGB32)
				Add32_From2(dst, src, level, 1);
			break;
		default:
			break;
	}
}

// ========================================================================
// 0x40 punch, 0x41 clear
// ========================================================================

/* Effect 0x40: clear the destination (to 0) where the source is set -
 * non-zero for RGB16, not black for RGB32.  For ARGB32 sources `level`
 * selects the test: non-zero alpha (level != 0) or alpha == 0xff (level
 * == 0).  32-bit sources need a 32-bit destination. */
void Blit_Punch(Bmp_t* dst, const Bmp_t* src, int level)
{
	int y, x;
	switch(src->mode)
	{
		case PM_RGB16:
			for(y = 0; y < src->h; y++)
			{
				const uint16_t* s = (const uint16_t*)ROW(src, y);
				uint16_t* d = (uint16_t*)ROW(dst, y);
				for(x = src->w - 1; x >= 0; x--)
					if(s[x])
						d[x] = 0;
			}
			break;
		case PM_RGB32:
			if(dst->mode != PM_RGB32 && dst->mode != PM_ARGB32)
				return;
			for(y = 0; y < src->h; y++)
			{
				const uint8_t* s = ROW(src, y);
				uint32_t* d = (uint32_t*)ROW(dst, y);
				for(x = 0; x < src->w; x++, s += 4)
					if(s[0] + s[1] + s[2] > 0)
						d[x] = 0;
			}
			break;
		case PM_ARGB32:
			if(dst->mode != PM_RGB32 && dst->mode != PM_ARGB32)
				return;
			for(y = 0; y < src->h; y++)
			{
				const uint32_t* s = (const uint32_t*)ROW(src, y);
				uint32_t* d = (uint32_t*)ROW(dst, y);
				for(x = 0; x < src->w; x++)
				{
					int hit = level ? (s[x] & 0xff000000u) != 0 : (s[x] >> 24) == 0xff;
					if(hit)
						d[x] = 0;
				}
			}
			break;
		default:
			break;
	}
}

// effect 0x41: zero the destination over the rectangle common to the two views (any mode)
void Blit_Fx41(Bmp_t* dst, const Bmp_t* src)
{
	Rect_t a, b;
	Rect_FromBmp(&a, dst);
	Rect_FromBmp(&b, src);
	if(Rect_Clip(&a, &b))
		Bmp_Clear(dst, &a);
}

// ========================================================================
// 0x05 / 0xC0 dim and 0xC1 tint
// ========================================================================

// RGB32 <- RGB32: d = (s * inv) >> 8 per colour byte with inv = 0x100 - level, top byte -> 0
static void Dim32(Bmp_t* dst, const Bmp_t* src, int level)
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

// RGB32 <- ARGB32: d += ((s * inv >> 8) - d) * (a >> 1) >> 7 for alpha >= 2; the top byte keeps d's value
static void Dim32_2to1(Bmp_t* dst, const Bmp_t* src, int level)
{
	int inv = 0x100 - level, y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x];
			int a = (int)(sv >> 25), i, out = (int)(dv & 0xff000000u);
			if(!(sv & 0xfe000000u))
				continue;
			for(i = 0; i < 24; i += 8)
			{
				int sc = (int)((sv >> i) & 0xff), dc = (int)((dv >> i) & 0xff);
				int sd = (sc * inv) >> 8;
				out |= Sat8((((sd - dc) * a) >> 7) + dc) << i;
			}
			d[x] = (uint32_t)out;
		}
	}
}

/* ARGB32 <- ARGB32 dim: "over" compositing (the alphas as in
 * AlphaCopy32_To2) with the source's colour weight reduced by inv =
 * 0x100 - level; pixels with alpha 0 are skipped */
static void Dim32_2to2(Bmp_t* dst, const Bmp_t* src, int level)
{
	unsigned inv = (unsigned)(0x100 - level);
	int y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint8_t* s = ROW(src, y);
		uint8_t* d = ROW(dst, y);
		for(x = 0; x < src->w; x++, s += 4, d += 4)
		{
			unsigned sa, da, ws, wd, total, fs, fd;
			int i;
			if(!(s[3]))
				continue;
			sa = s[3];
			da = d[3];
			ws = sa * inv;          // source weight * 256
			wd = (0x100 - sa) * da; // destination weight * 256
			total = (sa << 8) + wd;
			fs = (ws << 16) / total;
			fd = (wd << 16) / total;
			for(i = 0; i < 3; i++)
				d[i] = (uint8_t)((s[i] * fs + d[i] * fd) >> 16);
			d[3] = (uint8_t)(total >> 8);
		}
	}
}

/* Effects 0x05 / 0xC0: src darkened toward black by `level` / 256 (0:
 * Blit_Copy).  RGB16 <- RGB16 (Blit_Tint with black), RGB32 <- RGB32
 * (level 0x100 clears), RGB32 <- ARGB32 and ARGB32 <- ARGB32 (composited
 * with the alpha). */
void Blit_Dim(Bmp_t* dst, const Bmp_t* src, int level)
{
	if(level <= 0)
	{
		Blit_Copy(dst, src);
		return;
	}
	switch(src->mode)
	{
		case PM_RGB16:
			if(dst->mode == PM_RGB16)
				Blit_Tint(dst, src, 0, level); // Blit_Tint16 with black
			break;
		case PM_RGB32:
			if(dst->mode != PM_RGB32)
				return;
			if(level >= 0x100)
				Bmp_Clear(dst, NULL);
			else
				Dim32(dst, src, level);
			break;
		case PM_ARGB32:
			if(dst->mode == PM_RGB32)
				Dim32_2to1(dst, src, level);
			else if(dst->mode == PM_ARGB32)
				Dim32_2to2(dst, src, level);
			break;
		default:
			break;
	}
}

// RGB16 <- RGB16: (s * inv + colour * level) >> 8 per channel, the colour packed to 5-5-5
static void Tint16(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level)
{
	int inv = 0x100 - level, y, x;
	unsigned cb = ((colour >> 3) & 0x1f) * (unsigned)level;
	unsigned cg = ((colour >> 6) & 0x3e0) * (unsigned)level;
	unsigned cr = ((colour >> 9) & 0x7c00) * (unsigned)level;
	for(y = 0; y < src->h; y++)
	{
		const uint16_t* s = (const uint16_t*)ROW(src, y);
		uint16_t* d = (uint16_t*)ROW(dst, y);
		for(x = src->w - 1; x >= 0; x--)
		{
			unsigned sv = s[x];
			unsigned g = (((sv & 0x3e0) * inv + cg) >> 8) & 0x3e0;
			unsigned r = (((sv & 0x7c00) * inv + cr) >> 8) & 0x7c00;
			unsigned b = ((sv & 0x1f) * inv + cb) >> 8;
			d[x] = (uint16_t)(g + r + b);
		}
	}
}

/* RGB32 <- RGB32: ((s * inv) >> 8) + ((colour * level) >> 8)
 * per colour byte (saturated), top byte -> 0 */
static void Tint32(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level)
{
	int inv = 0x100 - level, y, x;
	int cb = (int)((colour & 0xff) * (unsigned)level) >> 8;
	int cg = (int)(((colour >> 8) & 0xff) * (unsigned)level) >> 8;
	int cr = (int)(((colour >> 16) & 0xff) * (unsigned)level) >> 8;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x];
			d[x] = (uint32_t)Sat8((int)(((sv & 0xff) * (unsigned)inv) >> 8) + cb) | (uint32_t)Sat8((int)((((sv >> 8) & 0xff) * (unsigned)inv) >> 8) + cg) << 8 | (uint32_t)Sat8((int)((((sv >> 16) & 0xff) * (unsigned)inv) >> 8) + cr) << 16;
		}
	}
}

/* Effect 0xC1 (with colour 0xFFFFFF) and filter kind 0: dst = src blended
 * toward `colour` (0x00RRGGBB) by `level` / 256.  RGB16 <- RGB16 and
 * RGB32 <- RGB32 only; a black colour is the plain dim. */
void Blit_Tint(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level)
{
	if(src->mode == PM_RGB16)
	{
		if(dst->mode == PM_RGB16)
			Tint16(dst, src, colour, level);
	}
	else if(src->mode == PM_RGB32 && dst->mode == PM_RGB32)
	{
		if(colour == 0)
			Dim32(dst, src, level);
		else
			Tint32(dst, src, colour, level);
	}
}
