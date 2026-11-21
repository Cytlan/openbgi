/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * blit_xform.c - masks, wipes and the geometric blitters of the sprite
 * object (src/include/bgi/gfx/bitmap.h):
 *
 *   Blit_ApplyMask    copy a gray bitmap into the alpha channel
 *   Blit_GrayWipe     reveal through a gray pattern (sprite mode 3)
 *   Blit_GrayWipeTo   the same into a temporary ARGB32 picture
 *   Blit_Masked       alpha blend where a gray mask is non-zero
 *   Blit_MaskedTo     keep the pixels where the mask is non-zero
 *   Blit_Xform        rotate + scale, blended onto RGB32
 *   Blit_XformCopy    rotate + scale into a picture of equal mode
 *   Blit_XformVscale  vertical scale of an unchecked picture
 *   Blit_Skew         sine-wave horizontal displacement
 *
 * The transforms share one setup routine (Xform_Setup) that computes, in
 * double arithmetic in the order of the original's x87 code, the 16.16
 * source position of destination pixel (0, 0) and the per-column /
 * per-row increments.  Every destination pixel then maps to one source
 * position; the "smooth" variants sample four source pixels with 4-bit
 * fractions, the others take the nearest one.  As everywhere in this
 * renderer, the MMX arithmetic is repeated lane by lane in plain C with
 * identical shifts and saturation.
 */
#include <math.h>
#include "blit_internal.h"

// pmullw + psraw 7 (products stay within 16 bits here)
static inline int Mul7(int a, int w)
{
	return (a * w) >> 7;
}

#define PIX32(b, x, y) (*(const uint32_t*)(ROW(b, y) + (size_t)(x) * 4))

// ========================================================================
// masks
// ========================================================================

// the mask value at (x, y) of a GRAY8 map, or the alpha of an ARGB32 one (1.599 on: a sprite mask may be either)
static inline uint32_t MaskAt(const Bmp_t* m, const uint8_t* row, int x)
{
	return m->mode == PM_GRAY8 ? row[x] : ((const uint32_t*)row)[x] >> 24;
}

/* dst (ARGB32) = src colour with the mask bitmap (GRAY8, or the alpha of
 * an ARGB32 one) as alpha; an ARGB32 source multiplies its own alpha by
 * the mask value (an RGB32 one takes it as is).  dst and src may be the
 * same view; the mask must cover src.  7 when the mask is neither kind,
 * 0 otherwise (also when nothing was done: dst not ARGB32 or src not
 * 32-bit). */
int Blit_ApplyMask(Bmp_t* dst, const Bmp_t* src, const Bmp_t* gray)
{
	int y, x;
	if(gray->mode != PM_GRAY8 && gray->mode != PM_ARGB32)
		return 7;
	if(dst->mode != PM_ARGB32)
		return 0;
	if(src->mode == PM_RGB32)
	{
		for(y = 0; y < src->h; y++)
		{
			const uint32_t* s = (const uint32_t*)ROW(src, y);
			const uint8_t* g = ROW(gray, y);
			uint32_t* d = (uint32_t*)ROW(dst, y);
			for(x = 0; x < src->w; x++)
				d[x] = (s[x] & 0xffffffu) | MaskAt(gray, g, x) << 24;
		}
	}
	else if(src->mode == PM_ARGB32)
	{
		for(y = 0; y < src->h; y++)
		{
			const uint32_t* s = (const uint32_t*)ROW(src, y);
			const uint8_t* g = ROW(gray, y);
			uint32_t* d = (uint32_t*)ROW(dst, y);
			for(x = 0; x < src->w; x++)
				d[x] = (s[x] & 0xffffffu) | ((((s[x] >> 24) * MaskAt(gray, g, x)) << 16) & 0xff000000u);
		}
	}
	return 0;
}

// ========================================================================
// gray wipes (sprite mode 3)
// ========================================================================

/* threshold of the wipe: pixels whose pattern value g satisfies
 * (g << mode) < ((1 << mode) + 1) * progress are (partially) revealed; the
 * soft edge is min(thr - (g << mode), 0x100) / 0x100 wide. */
static inline int WipeThreshold(int mode, int progress)
{
	return ((1 << mode) + 1) * progress;
}

/* RGB32 <- ARGB32 through a GRAY8 pattern, level < 0x100: with v the
 * clamped distance below the threshold, the pixel is blended in with
 * weight ((v * alpha) >> 9) * inv >> 8 (B, G, R; the top byte keeps d's) */
static void GrayWipe32(Bmp_t* dst, const Bmp_t* src, const Bmp_t* gray, int mode, int progress, int level)
{
	int inv = 0x100 - level, thr = WipeThreshold(mode, progress), y, x, i;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		const uint8_t* g = ROW(gray, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			int v = thr - (g[x] << mode), idx, w;
			uint32_t sv, dv, out;
			if(v <= 0)
				continue;
			if(v > 0x100)
				v = 0x100;
			sv = s[x];
			dv = d[x];
			idx = (v * (int)(sv >> 24)) >> 9; // 0 .. 0x7f
			w = (idx * inv) >> 8;
			out = dv & 0xff000000u;
			for(i = 0; i < 24; i += 8)
			{
				int sc = (int)((sv >> i) & 0xff), dc = (int)((dv >> i) & 0xff);
				out |= (uint32_t)Sat8(dc + Mul7(sc - dc, w)) << i;
			}
			d[x] = out;
		}
	}
}

/* Reveal src (ARGB32) over dst (RGB32) through the GRAY8 pattern `gray`:
 * `progress` runs from 0 (nothing) to 0x100 (a plain Blit_Blend), `mode`
 * (0 ..) widens the threshold, `level` is the transparency of the revealed
 * part (0x100 and above draws nothing).  Always 0; other modes draw
 * nothing. */
int Blit_GrayWipe(Bmp_t* dst, const Bmp_t* src, const Bmp_t* gray, int mode, int progress, int level)
{
	if(gray->mode != PM_GRAY8 || dst->mode != PM_RGB32 || src->mode != PM_ARGB32)
		return 0;
	if((uint32_t)level >= 0x100u)
		return 0;
	if((uint32_t)progress >= 0x100u)
		Blit_Blend(dst, src, level);
	else
		GrayWipe32(dst, src, gray, mode, progress, level);
	return 0;
}

// tmp (ARGB32) = src with alpha * clamp(thr - (g << mode), 0, 0x100) >> 8
static void GrayWipeTo32(Bmp_t* tmp, const Bmp_t* src, const Bmp_t* gray, int mode, int progress)
{
	int thr = WipeThreshold(mode, progress), y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		const uint8_t* g = ROW(gray, y);
		uint32_t* d = (uint32_t*)ROW(tmp, y);
		for(x = 0; x < src->w; x++)
		{
			int v = thr - (g[x] << mode);
			if(v < 0)
				v = 0;
			if(v > 0x100)
				v = 0x100;
			d[x] = (s[x] & 0xffffffu) | ((((s[x] >> 24) * (uint32_t)v) << 16) & 0xff000000u);
		}
	}
}

/* The wipe as a picture: tmp (ARGB32) = src (ARGB32) with its alpha scaled
 * by how far each pixel is revealed (progress 0 clears tmp, 0x100 and
 * above copies src).  Always 0; other modes draw nothing. */
int Blit_GrayWipeTo(Bmp_t* tmp, const Bmp_t* src, const Bmp_t* gray, int mode, int progress)
{
	if(gray->mode != PM_GRAY8 || tmp->mode != PM_ARGB32 || src->mode != PM_ARGB32)
		return 0;
	if(progress == 0)
		Bmp_Clear(tmp, NULL);
	else if((uint32_t)progress >= 0x100u)
		Blit_Copy(tmp, src);
	else
		GrayWipeTo32(tmp, src, gray, mode, progress);
	return 0;
}

// ========================================================================
// masked blends (rain screen)
// ========================================================================

/* RGB32 <- ARGB32 where the mask byte is non-zero, for alpha >= 2
 *   d + ((s - d) * ((a7 * inv) >> 8)) >> 7        (B, G, R; top byte -> d)
 * Always 0; a level of 0x100 or more, or other modes, draw nothing. */
int Blit_Masked(Bmp_t* dst, const Bmp_t* src, const Bmp_t* gray, int level)
{
	int inv = 0x100 - level, y, x, i;
	if(gray->mode != PM_GRAY8 || dst->mode != PM_RGB32 || src->mode != PM_ARGB32)
		return 0;
	if((uint32_t)level >= 0x100u)
		return 0;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		const uint8_t* g = ROW(gray, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv, out;
			int w;
			if(!g[x] || !(sv & 0xfe000000u))
				continue;
			dv = d[x];
			w = ((int)(sv >> 25) * inv) >> 8;
			out = dv & 0xff000000u;
			for(i = 0; i < 24; i += 8)
			{
				int sc = (int)((sv >> i) & 0xff), dc = (int)((dv >> i) & 0xff);
				out |= (uint32_t)Sat8(dc + Mul7(sc - dc, w)) << i;
			}
			d[x] = out;
		}
	}
	return 0;
}

// tmp (ARGB32) = mask ? src (ARGB32) : 0; always 0, other modes draw nothing
int Blit_MaskedTo(Bmp_t* tmp, const Bmp_t* src, const Bmp_t* gray)
{
	int y, x;
	if(gray->mode != PM_GRAY8 || tmp->mode != PM_ARGB32 || src->mode != PM_ARGB32)
		return 0;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		const uint8_t* g = ROW(gray, y);
		uint32_t* d = (uint32_t*)ROW(tmp, y);
		for(x = 0; x < src->w; x++)
			d[x] = g[x] ? s[x] : 0;
	}
	return 0;
}

// ========================================================================
// rotate + scale
// ========================================================================

#define DEG16_TO_RAD 2.663161090079238e-07 // 2*pi / (360 << 16)
#define INV_65536    1.52587890625e-05     // 1 / 65536
#define HALF_PI      1.5707963267948966    // pi / 2

/* The inverse mapping.  Destination pixel (i, j) samples the
 * source at  start + i * col + j * row  (all 16.16, wrapping adds).
 *
 * The original evaluates this on the x87 stack; the sequence of operations
 * is kept so that doubles truncate the same way (the scale factors are
 * loaded as *unsigned* 64-bit integers, which is why a negative scale
 * collapses the picture instead of mirroring it). */
typedef struct XformMap
{
	int32_t startX, startY, colX, colY, rowX, rowY; // 16.16
} XformMap_t;

// fill the map for the arguments of Blit_Xform (angle 16.16 degrees clockwise, sx / sy 16.16 scale)
static void Xform_Setup(XformMap_t* m, int32_t x16, int32_t y16, int32_t cx16, int32_t cy16,
	int32_t angle, int32_t sx, int32_t sy)
{
	double theta = -(angle * DEG16_TO_RAD);
	double c = cos(theta), s = sin(theta);
	double isx = 65536.0 / (double)(uint32_t)sx;
	double isy = 65536.0 / (double)(uint32_t)sy;
	double ax = -(x16 * INV_65536);
	double by = y16 * INV_65536;
	double cy = cy16 * INV_65536;
	double cx = cx16 * INV_65536;
	double t2 = theta + HALF_PI;

	m->startY = BGI_Ftol((cy - (ax * s + by * c) * isy) * 65536.0);
	m->startX = BGI_Ftol(((ax * c - by * s) * isx + cx) * 65536.0);
	m->colY = BGI_Ftol(isy * s * -65536.0);
	m->colX = BGI_Ftol(isx * c * 65536.0);
	m->rowY = BGI_Ftol(sin(t2) * isy * 65536.0);
	m->rowX = BGI_Ftol(cos(t2) * isx * -65536.0);
}

/* When the transform is a pure integer translation (no fraction,
 * angle a multiple of 360 degrees, unit scale) and the destination lies
 * completely inside the source, the blit degenerates to a plain copy of a
 * source sub-view.  Returns 1 and fills *view in that case. */
static int Xform_TrivialView(Bmp_t* view, const Bmp_t* dst, int32_t x16, int32_t y16, const Bmp_t* src,
	int32_t cx16, int32_t cy16, int32_t angle, int32_t sx, int32_t sy)
{
	Rect_t dr, sr;
	int32_t x, y, cx, cy;
	if((x16 & 0xffff) || (y16 & 0xffff) || (cx16 & 0xffff) || (cy16 & 0xffff))
		return 0;
	if(angle % (360 << 16) != 0)
		return 0;
	if(sx != 0x10000 || sy != 0x10000)
		return 0;
	x = x16 >> 16;
	y = y16 >> 16;
	cx = cx16 >> 16;
	cy = cy16 >> 16;
	Rect_FromBmp(&dr, dst);
	Rect_Offset(&dr, -x, -y); // destination relative to the origin
	Rect_FromBmp(&sr, src);
	Rect_Offset(&sr, -cx, -cy); // source relative to its centre
	if(!Rect_Inside(&dr, &sr))
		return 0;
	if(view)
	{
		Rect_Clip(&sr, &dr); // = dr, since dr is inside sr
		Rect_Offset(&sr, cx, cy);
		*view = *src;
		Bmp_Crop(view, &sr);
	}
	return 1;
}

// source position of destination pixel (i, j), 16.16
typedef struct XformPos
{
	int32_t x, y;
} XformPos_t;

/* 4-bit bilinear sample of a 32-bit picture at the 16.16 position; `lanes`
 * receives B, G, R, A (0 .. 255).  The neighbourhood is read with the
 * rules of the original's smooth samplers: integer parts of -2 and below,
 * or at or beyond the far edge, give a black/transparent sample; a part
 * of -1 or of w - 1 / h - 1 blends with black on that side. */
static void Xform_Sample(const Bmp_t* src, XformPos_t p, int lanes[4])
{
	int x = p.x >> 16, y = p.y >> 16;
	int fx = (p.x >> 12) & 15, fy = (p.y >> 12) & 15;
	int w = src->w, h = src->h, i;
	uint32_t p00 = 0, p01 = 0, p10 = 0, p11 = 0;

	lanes[0] = lanes[1] = lanes[2] = lanes[3] = 0;
	if(y <= -2 || x <= -2 || y >= h)
		return;
	if(y >= 0)
	{
		if(x >= w)
			return;
		if(x >= 0)
			p00 = PIX32(src, x, y);
		if(x + 1 < w)
			p01 = PIX32(src, x + 1, y);
	}
	if(y + 1 < h)
	{
		if(fy == 0)
		{ // one row is enough
			for(i = 0; i < 4; i++)
			{
				int a = (int)((p00 >> (8 * i)) & 0xff), b = (int)((p01 >> (8 * i)) & 0xff);
				lanes[i] = b + (((a - b) * (16 - fx)) >> 4);
			}
			return;
		}
		if(x >= w)
			return;
		if(x >= 0)
			p10 = PIX32(src, x, y + 1);
		if(x + 1 < w)
			p11 = PIX32(src, x + 1, y + 1);
	}
	for(i = 0; i < 4; i++)
	{
		int a = (int)((p00 >> (8 * i)) & 0xff), b = (int)((p01 >> (8 * i)) & 0xff);
		int c = (int)((p10 >> (8 * i)) & 0xff), d = (int)((p11 >> (8 * i)) & 0xff);
		int r0 = b + (((a - b) * (16 - fx)) >> 4);
		int r1 = d + (((c - d) * (16 - fx)) >> 4);
		lanes[i] = r1 + (((r0 - r1) * (16 - fy)) >> 4);
	}
}

/* The same sample as one pixel (B, G, R, A from the lowest byte).  A
 * position whose four pixels all lie inside the picture - nearly every
 * one - is worked out several lanes to a word: each step of Xform_Sample is
 * b + (((a - b) * (16 - f)) >> 4), which is (a * (16 - f) + b * f) >> 4
 * (blit_internal.h), 4080 at most.  The others go through Xform_Sample;
 * its lanes are bytes too, each lying between the pixels it mixes. */
#if UINTPTR_MAX > 0xffffffffu
#define LANES_4 0x00ff00ff00ff00ffull
// the four bytes of a pixel 16 bits apart: blue and red in the low half of a 64-bit word, green and alpha in the high one
static inline uint64_t Spread4(uint32_t x)
{
	return (x & LANES_RB) | ((uint64_t)((x >> 8) & LANES_RB) << 32);
}
#endif

static inline uint32_t Xform_SamplePixel(const Bmp_t* src, XformPos_t p)
{
	int x = p.x >> 16, y = p.y >> 16;
	if(x >= 0 && y >= 0 && x + 1 < src->w && y + 1 < src->h)
	{
		const uint32_t* top = (const uint32_t*)ROW(src, y) + x;
		const uint32_t* low = (const uint32_t*)ROW(src, y + 1) + x;
		uint32_t fx = (uint32_t)((p.x >> 12) & 15), gx = 16 - fx, fy = (uint32_t)((p.y >> 12) & 15), gy = 16 - fy;
#if UINTPTR_MAX > 0xffffffffu
		uint64_t t = ((Spread4(top[0]) * gx + Spread4(top[1]) * fx) >> 4) & LANES_4;
		uint64_t l = ((Spread4(low[0]) * gx + Spread4(low[1]) * fx) >> 4) & LANES_4;
		uint64_t v = ((t * gy + l * fy) >> 4) & LANES_4;
		return (uint32_t)v | (uint32_t)(v >> 32) << 8;
#else
		uint32_t trb = (((top[0] & LANES_RB) * gx + (top[1] & LANES_RB) * fx) >> 4) & LANES_RB;
		uint32_t tag = ((((top[0] >> 8) & LANES_RB) * gx + ((top[1] >> 8) & LANES_RB) * fx) >> 4) & LANES_RB;
		uint32_t lrb = (((low[0] & LANES_RB) * gx + (low[1] & LANES_RB) * fx) >> 4) & LANES_RB;
		uint32_t lag = ((((low[0] >> 8) & LANES_RB) * gx + ((low[1] >> 8) & LANES_RB) * fx) >> 4) & LANES_RB;
		return (((trb * gy + lrb * fy) >> 4) & LANES_RB) | ((((tag * gy + lag * fy) >> 4) & LANES_RB) << 8);
#endif
	}
	else
	{
		int s[4];
		Xform_Sample(src, p, s);
		return (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
	}
}

// nearest sample; 0 when outside (unsigned compare, so negatives fall out)
static inline int Xform_Nearest(const Bmp_t* src, XformPos_t p, uint32_t* out)
{
	int x = p.x >> 16, y = p.y >> 16;
	if((uint32_t)y >= (uint32_t)src->h || (uint32_t)x >= (uint32_t)src->w)
	{
		*out = 0;
		return 0;
	}
	*out = PIX32(src, x, y);
	return 1;
}

// iterate the destination: for (j) { p = rowStart; for (i) { ...; p += col } rowStart += row }
#define XFORM_ROWS(m, dst)                                                     \
	for(rowStart.x = (m).startX, rowStart.y = (m).startY, j = 0; j < (dst)->h; \
		j++, rowStart.x += (m).rowX, rowStart.y += (m).rowY)
#define XFORM_COLS(m, dst) \
	for(p = rowStart, i = 0; i < (dst)->w; i++, p.x += (m).colX, p.y += (m).colY)

// RGB32 <- RGB32, blended by level with smooth (bilinear) or nearest sampling: d + ((s - d) * inv) >> 8 on B, G, R; the top byte keeps d's
static void Xform_Blend1(Bmp_t* dst, const XformMap_t* m, const Bmp_t* src, int level, int smooth)
{
	uint32_t inv = (uint32_t)(0x100 - level);
	int i, j;
	XformPos_t rowStart, p;
	XFORM_ROWS(*m, dst)
	{
		uint32_t* d = (uint32_t*)ROW(dst, j);
		XFORM_COLS(*m, dst)
		{
			uint32_t sv;
			if(smooth)
				sv = Xform_SamplePixel(src, p);
			else
				Xform_Nearest(src, p, &sv);
			d[i] = (d[i] & 0xff000000u) | Lerp8(d[i], sv, inv);
		}
	}
}

/* RGB32 <- ARGB32 with smooth or nearest sampling: alpha blend with
 * weight ((a >> 1) * inv) >> 8; pixels with alpha < 2 (after filtering)
 * are left alone */
static void Xform_Blend2(Bmp_t* dst, const XformMap_t* m, const Bmp_t* src, int level, int smooth)
{
	uint32_t inv = (uint32_t)(0x100 - level);
	int i, j;
	XformPos_t rowStart, p;
	XFORM_ROWS(*m, dst)
	{
		uint32_t* d = (uint32_t*)ROW(dst, j);
		XFORM_COLS(*m, dst)
		{
			uint32_t sv, a7;
			if(smooth)
				sv = Xform_SamplePixel(src, p);
			else if(!Xform_Nearest(src, p, &sv))
				continue;
			a7 = sv >> 25;
			if(!a7)
				continue;
			d[i] = (d[i] & 0xff000000u) | Lerp7(d[i], sv, (a7 * inv) >> 8);
		}
	}
}

/* Draw src rotated and scaled onto an RGB32 destination (see bitmap.h for
 * the geometry).  A trivial transform (integer translation inside the
 * source) is a Blit_Blend of the matching sub-view in any mode; otherwise
 * RGB32 sources are blended by level (level 0 writes the transformed
 * picture outright, outside pixels black) and ARGB32 sources by their
 * alpha and level.  0 ok, 0x13 a zero scale; a level of 0x100 or more, or
 * other modes, draw nothing. */
int Blit_Xform(Bmp_t* dst, int32_t x16, int32_t y16, const Bmp_t* src, int32_t cx16, int32_t cy16,
	int32_t angle, int32_t sx, int32_t sy, int level, int smooth)
{
	Bmp_t view;
	XformMap_t m;
	if(sx == 0 || sy == 0)
		return 0x13;
	if(Xform_TrivialView(&view, dst, x16, y16, src, cx16, cy16, angle, sx, sy))
	{
		Blit_Blend(dst, &view, level);
		return 0;
	}
	if(dst->mode != PM_RGB32)
		return 0;
	if((uint32_t)level >= 0x100u) // fully transparent: nothing
		return 0;
	Xform_Setup(&m, x16, y16, cx16, cy16, angle, sx, sy);
	if(src->mode == PM_RGB32)
	{
		// level 0 is a plain (unblended) write of the transformed picture
		if(level == 0)
		{
			int i, j;
			XformPos_t rowStart, p;
			XFORM_ROWS(m, dst)
			{
				uint32_t* d = (uint32_t*)ROW(dst, j);
				XFORM_COLS(m, dst)
				{
					if(smooth)
						d[i] = Xform_SamplePixel(src, p);
					else
						Xform_Nearest(src, p, &d[i]);
				}
			}
		}
		else
		{
			Xform_Blend1(dst, &m, src, level, smooth);
		}
	}
	else if(src->mode == PM_ARGB32)
	{
		Xform_Blend2(dst, &m, src, level, smooth);
	}
	return 0;
}

/* dst = src with the colour lanes scaled by (0x100 - level) / 256
 * (alpha kept); used by the trivial path of Blit_XformCopy */
static void Copy32_Dimmed(Bmp_t* dst, const Bmp_t* src, int level)
{
	int inv = 0x100 - level, y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x];
			d[x] = (uint32_t)(((sv & 0xff) * inv) >> 8) | (uint32_t)((((sv >> 8) & 0xff) * inv) >> 8) << 8 | (uint32_t)((((sv >> 16) & 0xff) * inv) >> 8) << 16 | (sv & 0xff000000u);
		}
	}
}

// the trivial path of Blit_XformCopy: a Blit_Copy at level 0, else the dimmed copy of two equal 32-bit modes
static void Copy32_Level(Bmp_t* dst, const Bmp_t* src, int level)
{
	if(level == 0)
	{
		Blit_Copy(dst, src);
		return;
	}
	if(dst->mode == src->mode && (src->mode == PM_RGB32 || src->mode == PM_ARGB32))
		Copy32_Dimmed(dst, src, level);
}

/* Write src rotated and scaled into dst, which must be of the same mode
 * (RGB32 or ARGB32; the geometry as in Blit_Xform).  Every destination
 * pixel is replaced: the sample (black / transparent outside the source)
 * with its colour scaled by (0x100 - level) / 256 and its alpha kept; a
 * level of 0x100 or more clears dst.  0 ok, 0x13 a zero scale; nothing is
 * done when the modes differ or are not 32-bit. */
int Blit_XformCopy(Bmp_t* dst, int32_t x16, int32_t y16, const Bmp_t* src, int32_t cx16, int32_t cy16,
	int32_t angle, int32_t sx, int32_t sy, int level, int smooth)
{
	Bmp_t view;
	XformMap_t m;
	XformPos_t rowStart, p;
	int inv = 0x100 - level, i, j;

	if(sx == 0 || sy == 0)
		return 0x13;
	if(dst->mode != src->mode)
		return 0;
	if(Xform_TrivialView(&view, dst, x16, y16, src, cx16, cy16, angle, sx, sy))
	{
		Copy32_Level(dst, &view, level);
		return 0;
	}
	if(src->mode != PM_RGB32 && src->mode != PM_ARGB32)
		return 0;
	if((uint32_t)level >= 0x100u)
	{
		Bmp_Clear(dst, NULL);
		return 0;
	}
	Xform_Setup(&m, x16, y16, cx16, cy16, angle, sx, sy);
	XFORM_ROWS(m, dst)
	{
		uint32_t* d = (uint32_t*)ROW(dst, j);
		XFORM_COLS(m, dst)
		{
			uint32_t sv;
			if(smooth)
				sv = Xform_SamplePixel(src, p);
			else
				Xform_Nearest(src, p, &sv);
			if(level) // colour * inv >> 8, alpha unchanged
				sv = (sv & 0xff000000u) | Scale3(sv, (uint32_t)inv, 8);
			d[i] = sv;
		}
	}
	return 0;
}

/* Vertical scale only (angle 0, sx = 1.0), always smooth, of a 32-bit
 * picture; two pixels per step over an even destination width.  The
 * original performs no bounds checks at all - the caller (the zoomed
 * presentation of a display object, frame.c) positions the destination
 * inside the source - and reads the row below the last one when the final
 * row has a zero fraction, which cannot change the result.  This version
 * keeps the arithmetic and clamps the row index instead of reading past
 * the picture; columns outside the source read as 0. */
void Blit_XformVscale(Bmp_t* dst, int32_t x16, int32_t y16, const Bmp_t* src, int32_t cx16, int32_t cy16, int32_t sy)
{
	XformMap_t m;
	XformPos_t rowStart, p;
	int i, j, k, pairs = dst->w >> 1;
	Xform_Setup(&m, x16, y16, cx16, cy16, 0, 0x10000, sy);
	XFORM_ROWS(m, dst)
	{
		uint32_t* d = (uint32_t*)ROW(dst, j);
		for(p = rowStart, i = 0; i < pairs; i++, p.x += 2 * m.colX, p.y += 2 * m.colY)
		{
			int x = p.x >> 16, y = p.y >> 16, fy = (p.y >> 12) & 15;
			int y0 = y < 0 ? 0 : y >= src->h ? src->h - 1
											 : y;
			int y1 = y + 1 >= src->h ? src->h - 1 : y + 1 < 0 ? 0
															  : y + 1;
			for(k = 0; k < 2; k++)
			{
				uint32_t a = 0, b = 0, out = 0;
				int l;
				if((uint32_t)(x + k) < (uint32_t)src->w)
				{
					a = PIX32(src, x + k, y0);
					b = PIX32(src, x + k, y1);
				}
				for(l = 0; l < 32; l += 8)
				{
					int pa = (int)((a >> l) & 0xff), pb = (int)((b >> l) & 0xff);
					out |= (uint32_t)Sat8(pb + (((pa - pb) * (16 - fy)) >> 4)) << l;
				}
				d[2 * i + k] = out;
			}
		}
	}
}

// ========================================================================
// skew (sprite mode 5 with setParam 0x60)
// ========================================================================

/* Every destination row is the source row shifted by a sine of
 * its index: offset(y) = (sin((y + phase) * 2pi / period) * src.w * amount
 * / 2) + (src.w - dst.w) / 2, in 16.16, sampled with 4-bit fractions.
 *
 * The source pointer of a row walks from column 0 and is advanced only for
 * columns inside the picture, while the bounds test uses the shifted column
 * index.  This is exact for offsets <= 0 - the only ones Sprite_RebuildSkew
 * produces, as it widens the destination by src.w * amount - and is kept
 * as is for positive offsets. */
static void Skew(Bmp_t* dst, const Bmp_t* src, int32_t period, int32_t phase, int32_t amount)
{
	int32_t* offs = (int32_t*)BGI_Alloc((size_t)dst->h * sizeof(int32_t));
	int32_t base = (int32_t)((uint32_t)(src->w - dst->w) << 15); // half the width difference, 16.16
	double fperiod = (double)(uint32_t)period;
	double amp = (double)((uint32_t)src->w * (uint32_t)amount); // 32-bit product
	int y, i, k;

	for(y = 0; y < dst->h; y++)
	{
		double ang = (double)((uint32_t)y + (uint32_t)phase) * 6.283185307179586 / fperiod;
		offs[y] = (BGI_Ftol(sin(ang) * amp) >> 1) + base;
	}
	for(y = 0; y < dst->h; y++)
	{
		int32_t off = offs[y];
		int col = off >> 16, fx = (off & 0xf000) >> 12;
		const uint32_t* sp = (const uint32_t*)ROW(src, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		uint32_t prev = 0, cur;
		if((uint32_t)col < (uint32_t)src->w)
			prev = *sp++;
		col++;
		for(i = 0; i < dst->w; i++, col++)
		{
			uint32_t out = 0;
			cur = 0;
			if((uint32_t)col < (uint32_t)src->w)
				cur = *sp++;
			for(k = 0; k < 32; k += 8)
			{
				int a = (int)((prev >> k) & 0xff), b = (int)((cur >> k) & 0xff);
				out |= (uint32_t)Sat8(b + (((a - b) * (16 - fx)) >> 4)) << k;
			}
			d[i] = out;
			prev = cur;
		}
	}
	BGI_Free(offs);
}

/* The skew of two pictures of one 32-bit mode and the same height:
 * `period` (rows) and `phase` (rows) place the sine wave, `amount` (16.16)
 * scales it - a row is shifted by sin * src.w * amount / 2 pixels.  Always
 * 0; nothing is drawn for other modes. */
int Blit_Skew(Bmp_t* dst, const Bmp_t* src, int32_t period, int32_t phase, int32_t amount)
{
	if(dst->mode != src->mode)
		return 0;
	if(src->mode != PM_RGB32 && src->mode != PM_ARGB32)
		return 0;
	Skew(dst, src, period, phase, amount);
	return 0;
}
