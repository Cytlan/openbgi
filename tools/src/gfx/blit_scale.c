/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * blit_scale.c - the whole-picture scalers of "91 1C" / BmpOp_Resample and
 * the gathering add of "91 1B" (inc/bgi/gfx/bitmap.h; Blit_InitMmxTables
 * is declared in inc/bgi/gfx.h):
 *
 *   Blit_InitMmxTables  the 257-entry MMX weight table
 *   Blit_ScaleNearest   point sampling
 *   Blit_Scale          filtered, with three samplers:
 *     SampleBox         plain box average   (scale < 1/2)
 *     SampleArea        area-weighted box   (1/2 < scale < 1)
 *     SampleBilinear    bilinear            (scale >= 1)
 *   Blit_GatherAdd      the pixels drawn toward the centre and added
 *
 * Both scalers centre the scaled picture in the destination: the result is
 * (src.w * sx, src.h * sy) pixels (16.16 factors, rounded) placed at
 * (dst.w / 2 - sw / 2, dst.h / 2 - sh / 2) and clipped; the rest of the
 * destination is left alone.  Pixels are read and written as 32-bit words
 * whatever the modes say - the callers only ever pass RGB32 / ARGB32.
 *
 * The source position of the first written column / row is computed in
 * double arithmetic from the clipped offset ((xs - x0) / sw * span) in the
 * order the original uses, because that rounding decides which source
 * pixel a border column shows.
 */
#include "bgi/gfx/bitmap.h"
#include "bgi/gfx.h"

// entry i holds the 16-bit word i in all four lanes (pmullw multiplier)
static uint16_t gScaleWeights[0x101][4];

/* what the bilinear sampler reads as the right / bottom neighbour
 * outside the picture.  It is the low dword of gScaleWeights[0x100], i.e.
 * the pixel 0x01000100 once the table has been built - the original most
 * likely meant a zero dword that the table initialisation (one entry too
 * many for a 256-entry array) overwrites.  Reproduced as is. */
#define EDGE_PIXEL ((const uint8_t*)gScaleWeights[0x100])

// build the weight table; called once at start-up before any scaling
void Blit_InitMmxTables(void)
{
	int i, k;
	for(i = 0; i <= 0x100; i++)
		for(k = 0; k < 4; k++)
			gScaleWeights[i][k] = (uint16_t)i;
}

// bits per pixel by mode, used by the nearest scaler's mode check
static const int32_t kScaleBits[4] = {16, 32, 32, 8};

// the samplers: (src, box{l, t, r, b}, x16, y16) -> 32-bit pixel
typedef uint32_t (*Sampler_t)(const Bmp_t* src, Rect_t box, int32_t x16, int32_t y16);

/* average of the ceil(box.r / 256) x ceil(box.b / 256) source
 * pixels at (x16 >> 16, y16 >> 16), clamped to the picture.  box.l / box.t
 * are not used (the caller passes 0). */
static uint32_t SampleBox(const Bmp_t* src, Rect_t box, int32_t x16, int32_t y16)
{
	int32_t l = (int32_t)((uint32_t)x16 >> 16), t = (int32_t)((uint32_t)y16 >> 16);
	uint32_t r = (uint32_t)(l + ((box.r + 0xff) >> 8)), b = (uint32_t)(t + ((box.b + 0xff) >> 8));
	uint32_t sum[4] = {0, 0, 0, 0}, count = 0, y, x;
	const uint8_t* row;

	if(l < 0)
		l = 0;
	if(t < 0)
		t = 0;
	if(r > (uint32_t)src->w)
		r = (uint32_t)src->w;
	if(b > (uint32_t)src->h)
		b = (uint32_t)src->h;
	row = src->pixels + (size_t)t * (size_t)src->pitch;
	for(y = (uint32_t)t; y < b; y++, row += src->pitch)
	{
		if((uint32_t)l >= r)
			continue;
		count += r - (uint32_t)l;
		for(x = (uint32_t)l; x < r; x++)
		{
			const uint8_t* p = row + x * 4;
			sum[0] += p[0];
			sum[1] += p[1];
			sum[2] += p[2];
			sum[3] += p[3];
		}
	}
	if(count == 0) // the original divides by zero here (never reached by Blit_Scale)
		return 0;
	return (sum[3] / count) << 24 | (sum[2] / count) << 16 | (sum[1] / count) << 8 | (sum[0] / count);
}

/* the box [x16 >> 8, x16 >> 8 + box.r) x [y16 >> 8, .. + box.b)
 * in 24.8 source coordinates; every source pixel it touches is weighted by
 * its covered area (in 1/256 of the box) through the MMX weight table and
 * the sums are saturated 16-bit words.  box.l / box.t are not used. */
static uint32_t SampleArea(const Bmp_t* src, Rect_t box, int32_t x16, int32_t y16)
{
	int32_t l = x16 >> 8, t = y16 >> 8;
	uint32_t r = (uint32_t)(l + box.r), b = (uint32_t)(t + box.b);
	uint32_t area, acc[4] = {0, 0, 0, 0}, y, x;
	const uint8_t* row;
	int k;

	if(l < 0)
		l = 0;
	if(t < 0)
		t = 0;
	if(r > (uint32_t)src->w << 8)
		r = (uint32_t)src->w << 8;
	if(b > (uint32_t)src->h << 8)
		b = (uint32_t)src->h << 8;
	area = (uint32_t)(((int32_t)b - t) * ((int32_t)r - l)) >> 8;
	if(area == 0) // the original divides by zero here (never reached by Blit_Scale)
		return 0;
	row = src->pixels + (size_t)(t >> 8) * (size_t)src->pitch;
	for(y = (uint32_t)t; y < b; row += src->pitch)
	{
		uint32_t ny = (y + 0x100) & ~0xffu, dy;
		if(ny > b)
			ny = b;
		dy = ny - y;
		for(x = (uint32_t)l; x < r;)
		{
			uint32_t nx = (x + 0x100) & ~0xffu, w;
			const uint8_t* p;
			if(nx > r)
				nx = r;
			// coverage / area in 1/256, then the table lookup
			w = ((((nx - x) * dy) << 8) / area) >> 8;
			p = row + ((x >> 6) & 0x3fffffcu); // (x >> 8) * 4
			for(k = 0; k < 4; k++)
			{
				uint32_t v = acc[k] + (uint16_t)(p[k] * gScaleWeights[w][k]); // pmullw / paddusw
				acc[k] = v > 0xffff ? 0xffff : v;
			}
			x = nx;
		}
		y = ny;
	}
	// psrlw 8 + packuswb
	return (acc[3] >> 8) << 24 | (acc[2] >> 8) << 16 | (acc[1] >> 8) << 8 | (acc[0] >> 8);
}

/* bilinear sample at the 16.16 position with 8-bit fractions.
 * On the last column / row the missing neighbours read EDGE_PIXEL. */
static uint32_t SampleBilinear(const Bmp_t* src, Rect_t box, int32_t x16, int32_t y16)
{
	uint32_t fx = ((uint32_t)x16 >> 8) & 0xff, fy = ((uint32_t)y16 >> 8) & 0xff;
	uint32_t ix = (uint32_t)x16 >> 16, iy = (uint32_t)y16 >> 16;
	const uint8_t* p00 = src->pixels + (size_t)iy * (size_t)src->pitch + (size_t)ix * 4;
	const uint8_t* p10 = p00 + 4;
	const uint8_t* p01 = p00 + src->pitch;
	const uint8_t* p11 = p01 + 4;
	uint32_t out = 0;
	int k;

	(void)box;
	if(ix >= (uint32_t)(src->w - 1))
		p10 = p11 = EDGE_PIXEL;
	if(iy >= (uint32_t)(src->h - 1))
		p01 = p11 = EDGE_PIXEL;
	for(k = 0; k < 4; k++)
	{
		uint32_t top = p00[k] * (0x100 - fx) + p10[k] * fx;
		uint32_t bottom = p01[k] * (0x100 - fx) + p11[k] * fx;
		out |= ((top * (0x100 - fy) + bottom * fy) >> 16) << (8 * k);
	}
	return out;
}

/* Nearest-neighbour scale of src by the 16.16 factors into the centre of
 * a 32-bit destination (16- and 8-bit destinations are rejected by the
 * kScaleBits check; nothing is drawn for them or for a zero result
 * size). */
void Blit_ScaleNearest(Bmp_t* dst, const Bmp_t* src, int32_t sx16, int32_t sy16)
{
	uint32_t sw, sh, xs, ys, xe, ye, stepX, stepY, frac, y, x;
	int32_t x0, y0, srcX16, srcY16, bits;
	const uint8_t* srcRow;
	uint8_t* dstRow;

	/* the table has four entries; for the vector modes the original reads
	 * the data that follows it (mode 4 and 6 fail the test, 5 passes) */
	bits = dst->mode < 4 ? kScaleBits[dst->mode] : dst->mode == PM_HEIGHT ? 32
																		  : 0;
	if((bits & ~7) < 24)
		return;
	sw = ((uint32_t)(src->w * sx16) + 0x8000) >> 16;
	sh = ((uint32_t)(src->h * sy16) + 0x8000) >> 16;
	if(sw == 0 || sh == 0)
		return;
	x0 = (int32_t)((uint32_t)dst->w / 2 - sw / 2);
	y0 = (int32_t)((uint32_t)dst->h / 2 - sh / 2);
	xs = x0 <= 0 ? 0 : (uint32_t)x0;
	ys = y0 <= 0 ? 0 : (uint32_t)y0;
	xe = (uint32_t)x0 + sw;
	ye = (uint32_t)y0 + sh;
	if(xe > (uint32_t)dst->w)
		xe = (uint32_t)dst->w;
	if(ye > (uint32_t)dst->h)
		ye = (uint32_t)dst->h;
	stepX = ((uint32_t)src->w << 16) / sw;
	stepY = ((uint32_t)src->h << 16) / sh;
	srcY16 = BGI_Ftol((double)(int32_t)(ys - (uint32_t)y0) / (double)sh * (double)((uint32_t)src->h << 16));
	srcX16 = BGI_Ftol((double)(int32_t)(xs - (uint32_t)x0) / (double)sw * (double)((uint32_t)src->w << 16));

	srcRow = src->pixels + (size_t)((uint32_t)srcY16 >> 16) * (size_t)src->pitch;
	frac = (uint32_t)srcY16 & 0xffff;
	dstRow = dst->pixels + (size_t)ys * (size_t)dst->pitch;
	if(ys >= ye)
		return;
	for(y = ys; y < ye; y++)
	{
		uint32_t sx = (uint32_t)srcX16;
		for(x = xs; x < xe; x++, sx += stepX)
			((uint32_t*)dstRow)[x] = ((const uint32_t*)srcRow)[sx >> 16];
		frac += stepY;
		srcRow += (((frac >> 16) * (uint32_t)src->pitch) >> 2) * 4;
		frac &= 0xffff;
		dstRow += (dst->pitch >> 2) * 4;
	}
}

/* Filtered scale of src by the 16.16 factors into the centre of a 32-bit
 * destination.  The sampler depends on the factors:
 *   either factor in (1/2, 1)          area-weighted box
 *   both factors >= 1                  bilinear over a 1-pixel box
 *   otherwise (a factor <= 1/2)        plain box average
 * The box is the source span of one destination pixel in 24.8 (0x10000 /
 * (factor >> 8)); the start position is advanced by half a box except in
 * the bilinear case.  Nothing is drawn for a zero result size. */
void Blit_Scale(Bmp_t* dst, const Bmp_t* src, int32_t sx16, int32_t sy16)
{
	Sampler_t sampler = SampleBilinear;
	Rect_t box;
	uint32_t sw, sh, xs, ys, xe, ye, stepX, stepY, rows, y, x, ux = (uint32_t)sx16, uy = (uint32_t)sy16;
	int32_t x0, y0, invSx, invSy, addX, addY, startX16, x16, y16, spanW, spanH;
	int shift = 8;
	uint8_t* dstRow;

	sw = ((uint32_t)(src->w * sx16) + 0x8000) >> 16;
	sh = ((uint32_t)(src->h * sy16) + 0x8000) >> 16;
	if(sw == 0 || sh == 0)
		return;
	invSx = (int32_t)(0x10000u / (ux >> 8));
	invSy = (int32_t)(0x10000u / (uy >> 8));
	x0 = (int32_t)((uint32_t)dst->w / 2 - sw / 2);
	y0 = (int32_t)((uint32_t)dst->h / 2 - sh / 2);
	xe = (uint32_t)x0 + sw;
	ye = (uint32_t)y0 + sh;
	xs = x0 <= 0 ? 0 : (uint32_t)x0;
	ys = y0 <= 0 ? 0 : (uint32_t)y0;
	if(xe > (uint32_t)dst->w)
		xe = (uint32_t)dst->w;
	if(ye > (uint32_t)dst->h)
		ye = (uint32_t)dst->h;
	addX = invSx << 7; // half a box, 16.16
	addY = invSy << 7;
	if((ux < 0x10000 && ux > 0x7fff) || (uy < 0x10000 && uy > 0x7fff))
		sampler = SampleArea;
	else if(ux >= 0x8000 && uy >= 0x8000)
	{
		// both >= 1: bilinear, sampling a one-pixel box (0x200 >> 9)
		invSx = invSy = 0x200;
		shift = 9;
		addX = addY = 0;
	}
	else
		sampler = SampleBox;
	box.l = 0;
	box.t = 0;
	box.r = invSx;
	box.b = invSy;

	// the usable source span is the picture less one box
	spanW = (src->w - (invSx >> shift)) << 16;
	spanH = (src->h - (invSy >> shift)) << 16;
	stepX = (uint32_t)spanW / sw;
	stepY = (uint32_t)spanH / sh;
	y16 = BGI_Ftol((double)(int32_t)(ys - (uint32_t)y0) / (double)sh * (double)(uint32_t)spanH) + addY;
	startX16 = BGI_Ftol((double)(int32_t)(xs - (uint32_t)x0) / (double)sw * (double)(uint32_t)spanW) + addX;

	dstRow = dst->pixels + (size_t)ys * (size_t)dst->pitch;
	if(ys >= ye)
		return;
	rows = ye - ys;
	for(y = 0; y < rows; y++)
	{
		x16 = startX16;
		for(x = xs; x < xe; x++, x16 += (int32_t)stepX)
			((uint32_t*)dstRow)[x] = sampler(src, box, x16, y16);
		y16 += (int32_t)stepY;
		dstRow += (dst->pitch >> 2) * 4;
	}
}

/* "91 1B" of 1.494 on: the gathering add.
 *
 * Every source pixel is scattered onto the destination at
 *   x = ((w - 1) cx) / 2 + i (1 - cx),  y = ((h - 1) cy) / 2 + j (1 - cy)
 * (16.16; cx = cy = 0 leaves the picture where it is, 1.0 collapses it
 * onto its centre), added with saturation to the 2 x 2 destination pixels
 * around that point, each contribution weighted by the bilinear share and
 * by (256 - level) / 256: the weight table holds ((128 - i) (256 - level))
 * >> 8 for the 128 "distances" i, and a distance of 0x80 or more adds
 * nothing.  RGB32 on both sides; nothing is drawn otherwise.
 *
 * The weights come out of the MMX code as follows, and the quirk is kept:
 * with w00 .. w11 the four bilinear shares (0 .. 128), the distances are
 * packed through a signed dword-to-word saturation (PACKSSDW where the
 * byte pack was evidently meant), so that whenever the right-hand share of
 * a row is not zero the pair saturates: that row's left pixel gets nothing
 * and its right pixel the full weight.  The visible effect is a brighter,
 * right-leaning gather, which is what the game shows.  The original does
 * not clip - its one caller draws a picture into a copy of the same size -
 * and this one does. */
void Blit_GatherAdd(Bmp_t* dst, const Bmp_t* src, uint32_t cx, uint32_t cy, uint32_t level)
{
	uint32_t table[128];
	uint32_t x0, y0, stepX, stepY, y, x, i;
	if(dst->mode != PM_RGB32 || src->mode != PM_RGB32)
		return;
	for(i = 0; i < 128; i++)
		table[i] = ((128 - i) * (256 - level)) >> 8;
	x0 = (uint32_t)(src->w - 1) * cx >> 1;
	y0 = (uint32_t)(src->h - 1) * cy >> 1;
	stepX = 0x10000 - cx;
	stepY = 0x10000 - cy;
	for(y = y0, i = 0; i < (uint32_t)src->h; i++, y += stepY)
	{
		const uint32_t* s = (const uint32_t*)(src->pixels + (size_t)i * (size_t)src->pitch);
		uint32_t j;
		for(x = x0, j = 0; j < (uint32_t)src->w; j++, x += stepX)
		{
			int32_t xi = (int32_t)x >> 16, yi = (int32_t)y >> 16;
			uint32_t fx = (x & 0xfe00) >> 9, fy = (y & 0xfe00) >> 9; // the top 7 fraction bits
			uint32_t w00 = ((128 - fx) * (128 - fy)) >> 7, w10 = (fx * (128 - fy)) >> 7;
			uint32_t w01 = ((128 - fx) * fy) >> 7, w11 = (fx * fy) >> 7;
			uint32_t idx[4], k;
			uint32_t pix = s[j];
			// the packing quirk (see above): the row pair saturates when its right share is not zero
			if(w10)
			{
				idx[0] = 0x81;
				idx[1] = 0;
			}
			else
			{
				idx[0] = 0x80 - w00;
				idx[1] = 0x80;
			}
			if(w11)
			{
				idx[2] = 0x81;
				idx[3] = 0;
			}
			else
			{
				idx[2] = 0x80 - w01;
				idx[3] = 0x80;
			}
			for(k = 0; k < 4; k++)
			{
				int32_t px = xi + (int32_t)(k & 1), py = yi + (int32_t)(k >> 1);
				uint32_t* d;
				uint32_t wgt, c, out = 0;
				if(idx[k] >= 0x80 || px < 0 || py < 0 || px >= dst->w || py >= dst->h)
					continue;
				wgt = table[idx[k]];
				d = (uint32_t*)(dst->pixels + (size_t)py * (size_t)dst->pitch) + px;
				for(c = 0; c < 32; c += 8)
				{ // every byte, the top one too: (src * weight) >> 7 added with saturation
					uint32_t v = (((pix >> c) & 0xff) * wgt) >> 7;
					v += (*d >> c) & 0xff;
					if(v > 0xff)
						v = 0xff;
					out |= v << c;
				}
				*d = out;
			}
		}
	}
}
