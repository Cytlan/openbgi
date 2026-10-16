/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * blit_rotate.c - rotation / zoom ("stretch": "90 1D", background type 10), the
 *                 scaled copy ("transform": "90 1C", background type 9) and
 *                 the mosaic ("flip", background type 11) of
 *                 inc/bgi/gfx/bitmap.h
 */
#include "blit_internal.h"

// ---- rotate / zoom ("stretch") and scale ("transform") -----------------------------------------
#include <math.h>

#define DEG16_TO_RAD 2.663161090079238e-07  // 2*pi / (360 << 16)
#define INV_2_32     2.3283064365386963e-10 // 1 / 2^32
#define INV_65536    1.52587890625e-05      // 1 / 65536
#define HALF_PI      1.5707963267948966     // pi / 2

// _ftol: truncation toward zero
#define FtoL(d)      BGI_Ftol(d)

/* bilinear fetch for the 16.16 sampling positions of the rotate / scale
 * blitters.  The integer parts are packed with signed saturation to 16
 * bits and compared unsigned, so negative positions fall outside (0);
 * neighbours outside the picture read as 0.  Fractions have 7 significant
 * bits; all four bytes are mixed. */
static uint32_t SampleXY(const Bmp_t* src, uint32_t posX, uint32_t posY)
{
	int ix = (int)(posX >> 16), iy = (int)(posY >> 16);
	unsigned ux, uy;
	int fx, fy, off, i, out = 0;
	uint32_t p00 = 0, p01 = 0, p10 = 0, p11 = 0;
	if(ix > 0x7fff)
		ix = 0x7fff;
	if(iy > 0x7fff)
		iy = 0x7fff;
	ux = (unsigned)ix & 0xffff;
	uy = (unsigned)iy & 0xffff;
	off = ix * 4 + iy * src->pitch;
	fx = (int)((posX & 0xfe00) >> 9);
	fy = (int)((posY & 0xfe00) >> 9);
	if(uy < (unsigned)src->h)
	{
		if(ux < (unsigned)src->w)
			p00 = *(const uint32_t*)(src->pixels + off);
		if(fx && ux + 1 < (unsigned)src->w)
			p01 = *(const uint32_t*)(src->pixels + off + 4);
	}
	if(fy && uy + 1 < (unsigned)src->h)
	{
		if(ux < (unsigned)src->w)
			p10 = *(const uint32_t*)(src->pixels + off + src->pitch);
		if(ux + 1 < (unsigned)src->w)
			p11 = *(const uint32_t*)(src->pixels + off + src->pitch + 4);
	}
	for(i = 0; i < 32; i += 8)
	{
		int a = (int)((p00 >> i) & 0xff), b = (int)((p01 >> i) & 0xff);
		int c = (int)((p10 >> i) & 0xff), e = (int)((p11 >> i) & 0xff);
		int h0 = (((a - b) * (128 - fx)) >> 7) + b;
		int h1 = (((c - e) * (128 - fx)) >> 7) + e;
		out |= Sat8((((h0 - h1) * (128 - fy)) >> 7) + h1) << i;
	}
	return (uint32_t)out;
}

// the nearest fetch of Blit_Rotate: 0 unless 0 <= pos <= (size << 16) - 1
static uint32_t SampleNearest(const Bmp_t* src, int32_t posX, int32_t posY)
{
	if(posX < 0 || posY < 0 || posX > (src->w << 16) - 1 || posY > (src->h << 16) - 1)
		return 0;
	return *(const uint32_t*)(src->pixels + (posX >> 16) * 4 + (posY >> 16) * src->pitch);
}

/* the nearest fetch of Blit_ScaleTo compares the integer parts unsigned
 * against width and height after signed saturation (0 outside) */
static uint32_t SampleNearest2(const Bmp_t* src, uint32_t posX, uint32_t posY)
{
	int ix = (int)(posX >> 16), iy = (int)(posY >> 16);
	if(ix > 0x7fff)
		ix = 0x7fff;
	if(iy > 0x7fff)
		iy = 0x7fff;
	if((unsigned)iy >= (unsigned)src->h || (unsigned)ix >= (unsigned)src->w)
		return 0;
	return *(const uint32_t*)(src->pixels + ix * 4 + iy * src->pitch);
}

/*
 * Rotate by `angle` (16.16 degrees) and zoom by `zoom` (16.16) around the
 * destination centre, with the source point (cx, cy) (16.16) at the
 * centre; every destination pixel is written (0 outside the source).
 * smooth selects bilinear sampling.  0 ok, 1 mode mismatch, 0x13 zoom <=
 * 0.  Only 32-bit pictures are drawn; other matching modes return 0
 * untouched.
 */
int Blit_Rotate(Bmp_t* dst, const Bmp_t* src, int32_t zoom, int32_t angle, int32_t cx, int32_t cy, int smooth)
{
	double hw, hh, zd, zi, theta, c, s, CX, CY, X0, Y0;
	int32_t startX, startY, colX, colY, rowX, rowY;
	int y, x;
	if(zoom <= 0)
		return 0x13;
	if(dst->mode != src->mode)
		return 1;
	if(src->mode != PM_RGB32 && src->mode != PM_ARGB32)
		return 0;

	hw = -((double)(dst->w - 1) * 0.5);
	hh = (double)(dst->h - 1) * 0.5;
	zd = (double)zoom;
	zi = 65536.0 / zd;
	theta = -((double)angle * DEG16_TO_RAD);
	c = cos(theta);
	s = sin(theta);
	CY = (double)cy * zd * INV_2_32;
	Y0 = CY - (s * hw + c * hh);
	startY = FtoL(Y0 * zi * 65536.0);
	CX = (double)cx * zd * INV_2_32;
	X0 = (c * hw - s * hh) + CX;
	startX = FtoL(X0 * zi * 65536.0);
	colY = FtoL((s * zi) * -65536.0);
	colX = FtoL((c * zi) * 65536.0);
	rowX = FtoL(sin(theta + HALF_PI) * zi * 65536.0);
	rowY = FtoL(cos(theta + HALF_PI) * zi * -65536.0);

	for(y = 0; y < dst->h; y++)
	{
		uint32_t* d = (uint32_t*)ROW(dst, y);
		uint32_t px = (uint32_t)startX, py = (uint32_t)startY;
		for(x = 0; x < dst->w; x++)
		{
			d[x] = smooth ? SampleXY(src, px, py) : SampleNearest(src, (int32_t)px, (int32_t)py);
			px += (uint32_t)colX;
			py += (uint32_t)colY;
		}
		startX += rowX;
		startY += rowY;
	}
	return 0;
}

// "90 1D" and background type 10: Blit_Rotate around the source centre
int Blit_RotateCentred(Bmp_t* dst, const Bmp_t* src, int32_t zoom, int32_t angle, int smooth)
{
	return Blit_Rotate(dst, src, zoom, angle, src->w << 15, src->h << 15, smooth);
}

/*
 * Scale by (sx, sy) (16.16 factors) with the source point (hx, hy) (16.16)
 * at the destination point (cx, cy) (16.16), shifted by (ox, oy) (16.16);
 * every destination pixel is written (0 outside the source).  A factor
 * below 1.0 shifts the sampling by half a source pixel.  Blit_Scale2
 * passes the destination centre for (cx, cy) and half of (fullW, fullH)
 * for (hx, hy).  0 ok, 1 mode mismatch, 0x13 a factor <= 0, 0x14 full
 * size below 2.0.  Only 32-bit pictures are drawn; other matching modes
 * return 0 untouched.
 */
int Blit_ScaleTo(Bmp_t* dst, const Bmp_t* src, int32_t ox, int32_t oy, int32_t fullW, int32_t fullH,
	int32_t sx, int32_t sy, int32_t cx, int32_t cy, int32_t hx, int32_t hy, int smooth)
{
	double ix, iy, cxs, cys, hxd, hyd;
	int32_t xcorr = 0, ycorr = 0, startX, startY, stepX, stepY;
	int y, x;
	if((uint32_t)fullW < 0x20000u || (uint32_t)fullH < 0x20000u)
		return 0x14;
	if(sx <= 0 || sy <= 0)
		return 0x13;
	if(dst->mode != src->mode)
		return 1;
	if(src->mode != PM_RGB32 && src->mode != PM_ARGB32)
		return 0;

	ix = 65536.0 / (double)sx;
	iy = 65536.0 / (double)sy;
	cxs = (double)cx * ix * INV_65536;
	cys = (double)cy * iy * INV_65536;
	hxd = (double)hx * INV_65536;
	hyd = (double)hy * INV_65536;
	if((uint32_t)sx < 0x10000u)
		xcorr = FtoL((ix * 0.5 - 0.5) * 65536.0);
	if((uint32_t)sy < 0x10000u)
		ycorr = FtoL((iy * 0.5 - 0.5) * 65536.0);
	startY = ycorr - FtoL((hyd - cys) * -65536.0) + oy;
	startX = xcorr - FtoL((hxd - cxs) * -65536.0) + ox;
	stepX = FtoL(ix * 65536.0);
	stepY = FtoL(iy * 65536.0);

	for(y = 0; y < dst->h; y++)
	{
		uint32_t* d = (uint32_t*)ROW(dst, y);
		uint32_t px = (uint32_t)startX, py = (uint32_t)startY;
		for(x = 0; x < dst->w; x++)
		{
			d[x] = smooth ? SampleXY(src, px, py) : SampleNearest2(src, px, py);
			px += (uint32_t)stepX;
		}
		startY += stepY;
	}
	return 0;
}

// "90 1C" and background type 9: the source's (fullW, fullH) picture (16.16) scaled onto the destination centre
int Blit_Scale2(Bmp_t* dst, const Bmp_t* src, int32_t ox, int32_t oy, int32_t fullW, int32_t fullH,
	int32_t sx, int32_t sy, int smooth)
{
	return Blit_ScaleTo(dst, src, ox, oy, fullW, fullH, sx, sy, dst->w << 15, dst->h << 15,
		(int32_t)((uint32_t)fullW >> 1), (int32_t)((uint32_t)fullH >> 1), smooth);
}

// ---- mosaic ("flip", background type 11) -------------------------------------------------------

/* Blocks of (amount + 1) pixels take the colour of their top-left source
 * pixel; with a level the block colour is mixed into the existing
 * destination: m + (d - m) * level >> 8 (all four bytes) */
static void Mosaic(Bmp_t* dst, const Bmp_t* src, int amount, int level)
{
	int n = amount + 1, y, x;
	uint32_t* tmp = (uint32_t*)BGI_Alloc((size_t)dst->w * 4);
	for(y = 0; y < dst->h; y += n)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		int rows = dst->h - y < n ? dst->h - y : n, r;
		for(x = 0; x < dst->w; x += n)
		{
			int cols = dst->w - x < n ? dst->w - x : n, k;
			for(k = 0; k < cols; k++)
				tmp[x + k] = s[x];
		}
		for(r = 0; r < rows; r++)
		{
			uint32_t* d = (uint32_t*)ROW(dst, y + r);
			if(!level)
			{
				memcpy(d, tmp, (size_t)dst->w * 4);
			}
			else
			{
				for(x = 0; x < dst->w; x++)
				{
					int i, out = 0;
					for(i = 0; i < 32; i += 8)
					{
						int dc = (int)((d[x] >> i) & 0xff), mc = (int)((tmp[x] >> i) & 0xff);
						out |= Sat8((((dc - mc) * level) >> 8) + mc) << i;
					}
					d[x] = (uint32_t)out;
				}
			}
		}
	}
	BGI_Free(tmp);
}

/* The mosaic of src into dst (equal 32-bit modes and sizes): `amount` is
 * the block size less one, `extra` the transparency of the blocks (0
 * replaces, 0x100 and above draws nothing); amount 0 falls back to a
 * Blit_Blend with `extra` as the level.  `style` is ignored.  0 ok, 1 the
 * modes differ; other matching modes return 0 untouched. */
int Blit_Flip(Bmp_t* dst, const Bmp_t* src, int amount, int style, int extra)
{
	(void)style;
	if(dst->mode != src->mode)
		return 1;
	if(src->mode != PM_RGB32 && src->mode != PM_ARGB32)
		return 0;
	if(amount <= 0)
	{
		Blit_Blend(dst, src, extra);
		return 0;
	}
	if(extra <= 0)
		Mosaic(dst, src, amount, 0);
	else if((unsigned)extra < 0x100u)
		Mosaic(dst, src, amount, extra);
	return 0;
}
