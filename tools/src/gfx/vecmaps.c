/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * vecmaps.c - the generators of the vector maps (the displacement fields the
 * effectors, backgrounds and "90 1A" read through) and of the vector +
 * distance maps of the ripples and wipes, plus the loader of the "bwef"
 * files.  Interface in inc/bgi/gfx/bmpops.h.
 *
 * A PM_VECTOR map holds a 12.4 fixed-point (dx, dy) pair per pixel (hence
 * the factor 16 in the formulas below): the displacement the blitter adds
 * to the pixel's position before sampling.  A PM_VECDIST map holds three
 * words per pixel: a direction as a unit vector scaled to 0x7FFF and a
 * distance in quarter pixels, which the ripple ring tables index.
 *
 * Every generator comes in two forms: Vec_* fills a bitmap descriptor
 * (0x80000003 when it is not a non-empty map of the right mode), BmpOp_*
 * takes a bitmap slot instead (0x80000001 when the slot is empty) and is
 * what the instructions "91 10".."91 16" and "92 10" / "92 11" call.  The
 * result codes are the instructions' own numbering.
 */
#include <string.h>
#include <math.h>

#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/font.h"
#include "bgi/gfx/text.h"
#include "bgi/gfx/dispobj.h"
#include "bgi/file.h"
#include "bgi/codec.h"
#include "bgi/strutil.h"
#include "bgi/msg.h"
#include "bgi/os.h"

// ---- vector map generators ---------------------------------------------------------------------

// the destination must be a non-empty PM_VECTOR bitmap
#define VEC_CHECK(b)                                                                 \
	do                                                                               \
	{                                                                                \
		if((b)->mode != PM_VECTOR || (uint32_t)(b)->w == 0 || (uint32_t)(b)->h == 0) \
			return (int)0x80000003;                                                  \
	} while(0)

// row y of a vector map as (dx, dy) pairs
static int16_t* VecRow(const Bmp_t* b, int y)
{
	return (int16_t*)(b->pixels + (size_t)y * (size_t)b->pitch);
}

/* "91 10": a field that maps the bitmap onto the rectangle (ox, oy, rw, rh)
 * - u = 16 * ((x * rw) / w + ox - x), v likewise - so that sampling through
 * it stretches that rectangle of the source over the whole picture.
 * 0x80000005 for an empty range. */
int Vec_StretchRange(Bmp_t* b, int ox, int oy, int rw, int rh)
{
	int x, y;
	uint32_t xAcc, yAcc = 0;

	VEC_CHECK(b);
	if((uint32_t)rw == 0 || (uint32_t)rh == 0)
		return (int)0x80000005;
	for(y = 0; y < b->h; y++, yAcc += (uint32_t)rh)
	{
		int16_t* p = VecRow(b, y);
		int v = (int)((yAcc << 4) / (uint32_t)b->h) + ((oy - y) << 4);
		xAcc = 0;
		for(x = 0; x < b->w; x++, xAcc += (uint32_t)rw)
		{
			p[2 * x] = (int16_t)((int)((xAcc << 4) / (uint32_t)b->w) + ((ox - x) << 4));
			p[2 * x + 1] = (int16_t)v;
		}
	}
	return 0;
}

int BmpOp_VecStretchRange(int slot, int ox, int oy, int rw, int rh)
{
	Bmp_t b;
	if(!BmpMgr_GetInfo(gBmpMgr, &b, slot))
		return (int)0x80000001;
	return Vec_StretchRange(&b, ox, oy, rw, rh);
}

/* "91 11": every component uniformly random in -amplitude..amplitude (whole
 * map units, not pixels); two 15-bit draws of BGI_Rand make one 30-bit
 * number per component */
int Vec_Random(Bmp_t* b, int amplitude)
{
	int x, y;
	uint32_t range = (uint32_t)(2 * amplitude + 1);

	VEC_CHECK(b);
	for(y = 0; y < b->h; y++)
	{
		int16_t* p = VecRow(b, y);
		for(x = 0; x < b->w; x++)
		{
			uint32_t hi = (uint32_t)BGI_Rand() << 15, r;
			r = hi | (uint32_t)BGI_Rand();
			p[2 * x] = (int16_t)(amplitude - (int)(r % range));
			hi = (uint32_t)BGI_Rand() << 15;
			r = hi | (uint32_t)BGI_Rand();
			p[2 * x + 1] = (int16_t)(amplitude - (int)(r % range));
		}
	}
	return 0;
}

int BmpOp_VecRandom(int slot, int amplitude)
{
	Bmp_t b;
	if(!BmpMgr_GetInfo(gBmpMgr, &b, slot))
		return (int)0x80000001;
	return Vec_Random(&b, amplitude);
}

// one component: (hi^2 - lo^2) / (2 * scale), saturated to 15 bits, negative when the first sample is the higher one
static int16_t HeightSlope(uint32_t a, uint32_t b, int32_t scale)
{
	uint32_t big = a > b ? a : b, small = a > b ? b : a;
	uint64_t q;
	int32_t r;

	if(a == b)
		return 0;
	q = (uint64_t)(big - small) * (uint64_t)(big + small) / (uint64_t)(uint32_t)(2 * scale);
	r = q >= 0x8000u ? 0x7fff : (int32_t)q;
	return (int16_t)(a > b ? -r : r);
}

/* The gradient of a squared height field: u from the next sample to the
 * right, v from the one below, each (h1^2 - h0^2) / (2 * scale).  `height`
 * must be a PM_HEIGHT bitmap one sample larger than `vec` in both
 * directions (else 0x80000004); a zero scale counts as 1. */
int Vec_FromHeight(Bmp_t* vec, const Bmp_t* height, int32_t scale)
{
	int x, y;

	VEC_CHECK(vec);
	if(height->mode != PM_HEIGHT || height->w != vec->w + 1 || height->h != vec->h + 1)
		return (int)0x80000004;
	if((uint32_t)scale == 0)
		scale = 1;
	for(y = 0; y < vec->h; y++)
	{
		int16_t* p = VecRow(vec, y);
		const uint32_t* h0 = (const uint32_t*)(height->pixels + (size_t)y * (size_t)height->pitch);
		const uint32_t* h1 = (const uint32_t*)((const uint8_t*)h0 + height->pitch);
		for(x = 0; x < vec->w; x++)
		{
			p[2 * x] = HeightSlope(h0[x], h0[x + 1], scale);
			p[2 * x + 1] = HeightSlope(h0[x], h1[x], scale);
		}
	}
	return 0;
}

// a (w + 1) x (h + 1) height field for the generators that go through Vec_FromHeight
static void AllocHeight(Bmp_t* h, const Bmp_t* vec)
{
	Bmp_Alloc(h, vec->w + 1, vec->h + 1, PM_HEIGHT);
}

/* "91 12": concentric waves around (cx, cy): height = amplitude *
 * (1 - cos(2 pi (dist - phase) / period)), turned into a field by
 * Vec_FromHeight with scale 1.0.  0x80000006 for period 0. */
int Vec_Ripple(Bmp_t* b, int cx, int cy, int period, int phase, int amplitude)
{
	Bmp_t hgt;
	int x, y, r;

	VEC_CHECK(b);
	if((uint32_t)period == 0)
		return (int)0x80000006;
	AllocHeight(&hgt, b);
	for(y = 0; y < hgt.h; y++)
	{
		uint32_t* row = (uint32_t*)(hgt.pixels + (size_t)y * (size_t)hgt.pitch);
		int dy = cy - y;
		for(x = 0; x < hgt.w; x++)
		{
			int dx = cx - x;
			double dist = sqrt((double)(uint32_t)(dx * dx + dy * dy));
			row[x] = (uint32_t)BGI_Ftol((1.0 - cos((dist - (double)phase) * 6.283185307179586 / (double)period)) * (double)amplitude);
		}
	}
	r = Vec_FromHeight(b, &hgt, 0x10000);
	BGI_Free(hgt.pixels);
	return r;
}

int BmpOp_VecRipple(int slot, int cx, int cy, int period, int phase, int amplitude)
{
	Bmp_t b;
	if(!BmpMgr_GetInfo(gBmpMgr, &b, slot))
		return (int)0x80000001;
	return Vec_Ripple(&b, cx, cy, period, phase, amplitude);
}

// saturate to a signed 16-bit value
static int16_t Sat16(int32_t v)
{
	return (int16_t)(v >= 0x8000 ? 0x7fff : v < -0x8000 ? -0x8000
														: v);
}

/* "91 13": a polar remapping around (cx, cy).  angle16 is a 16.16 degree
 * angle whose sine and cosine shape the field: with k' = h / cos(angle)
 * (INT_MAX when the cosine is zero)
 *   u = 16 (w - 1) (1 - (180 - atan2(dy, dx) in degrees) / 360) - 16 dx - 16 cx
 *   v = 16 (k + k') dist / (dist sin(angle) + k)       - 16 dy - 16 cy
 * both saturated to 16 bits; zero when the cosine is zero. */
int Vec_Polar(Bmp_t* b, int cx, int cy, int32_t angle16, int k)
{
	double rad, s, c, kk;
	int x, y;

	VEC_CHECK(b);
	rad = (double)angle16 * 2.663161090079238e-07; // 16.16 degrees to radians
	s = sin(rad);
	c = cos(rad);
	kk = c == 0.0 ? 2147483647.0 : (double)(uint32_t)b->h / c;
	for(y = 0; y < b->h; y++)
	{
		int16_t* p = VecRow(b, y);
		int dy = y - cy;
		for(x = 0; x < b->w; x++)
		{
			int dx = x - cx;
			if(c == 0.0)
			{
				p[2 * x] = 0;
				p[2 * x + 1] = 0;
				continue;
			}
			{
				double dist = sqrt((double)(int32_t)(dy * dy + dx * dx));
				double kd = (double)k;
				int32_t q = BGI_Ftol(((kd + kk) * dist) * 16.0 / (dist * s + kd));
				double deg = atan2((double)dy, (double)dx) * 57.29577951308232;
				int32_t u = BGI_Ftol((double)(16 * (b->w - 1)) * (1.0 - (180.0 - deg) * 0.002777777777777778));
				p[2 * x] = Sat16(u - 16 * dx - 16 * cx);
				p[2 * x + 1] = Sat16(q - 16 * dy - 16 * cy);
			}
		}
	}
	return 0;
}

int BmpOp_VecPolar(int slot, int cx, int cy, int32_t angle16, int k)
{
	Bmp_t b;
	if(!BmpMgr_GetInfo(gBmpMgr, &b, slot))
		return (int)0x80000001;
	return Vec_Polar(&b, cx, cy, angle16, k);
}

/* "91 14": a lens-like bend inside `radius` of (cx, cy), tilted by angle16
 * (16.16 degrees).  With s = dist sin(angle) / radius, c = sqrt(1 - s^2),
 * t = (c - cos(angle)) s / c and theta = atan2(|dy|, |dx|):
 *   u = cos(theta) * 16 (cx - x) * t,  v = sin(theta) * 16 (cy - y) * t
 * Outside the radius (or at the centre) the field is zero.  0x80000008 for
 * a zero angle or radius. */
int Vec_BendAngle(Bmp_t* b, int cx, int cy, int32_t angle16, int radius)
{
	double rad, sn, cs;
	int x, y;

	VEC_CHECK(b);
	if((uint32_t)angle16 == 0 || (uint32_t)radius == 0)
		return (int)0x80000008;
	rad = (double)angle16 * 2.663161090079238e-07; // 16.16 degrees to radians
	sn = sin(rad);
	cs = cos(rad);
	for(y = 0; y < b->h; y++)
	{
		int16_t* p = VecRow(b, y);
		int dy = y - cy;
		for(x = 0; x < b->w; x++)
		{
			int dx = x - cx;
			double dist = sqrt((double)(dy * dy + dx * dx));
			double r = (double)radius;
			if(dist != 0.0 && dist < r)
			{
				double s = dist * sn / r;
				double c = sqrt(1.0 - s * s);
				double t = (c - cs) * (s / c);
				double theta = atan2((double)abs(dy), (double)abs(dx));
				p[2 * x] = (int16_t)BGI_Ftol(cos(theta) * (double)(16 * (cx - x)) * t);
				p[2 * x + 1] = (int16_t)BGI_Ftol(sin(theta) * (double)(16 * (cy - y)) * t);
			}
			else
			{
				p[2 * x] = 0;
				p[2 * x + 1] = 0;
			}
		}
	}
	return 0;
}

int BmpOp_VecBendAngle(int slot, int cx, int cy, int32_t angle16, int radius)
{
	Bmp_t b;
	if(!BmpMgr_GetInfo(gBmpMgr, &b, slot))
		return (int)0x80000001;
	return Vec_BendAngle(&b, cx, cy, angle16, radius);
}

/* "91 15": a dome: height = 4194304 (1 - sin(pi dist / (2 height))) inside
 * `height` of (cx, cy), through Vec_FromHeight with scale 2^30 / radius.
 * 0x80000008 for a zero radius or height. */
int Vec_Bend(Bmp_t* b, int cx, int cy, int radius, int height)
{
	Bmp_t hgt;
	int x, y, r;

	VEC_CHECK(b);
	if((uint32_t)radius == 0 || (uint32_t)height == 0)
		return (int)0x80000008;
	AllocHeight(&hgt, b);
	for(y = 0; y < hgt.h; y++)
	{
		uint32_t* row = (uint32_t*)(hgt.pixels + (size_t)y * (size_t)hgt.pitch);
		int dy = y - cy;
		for(x = 0; x < hgt.w; x++)
		{
			int dx = x - cx;
			double hd = (double)height;
			double dist = sqrt((double)(dx * dx + dy * dy));
			if(dist < hd)
				row[x] = (uint32_t)BGI_Ftol(4194304.0 - 4194304.0 * sin(dist * 3.141592653589793 / (double)(2 * height)));
			else
				row[x] = 0;
		}
	}
	r = Vec_FromHeight(b, &hgt, (int32_t)(0x40000000u / (uint32_t)radius));
	BGI_Free(hgt.pixels);
	return r;
}

/* "91 16" (1.494 on): a sine-wave vector field - the x displacement of row
 * y is 16 sin(2 pi (y + phaseY) / periodY) ampY, the y displacement of
 * column x is 16 sin(2 pi (x + phaseX) / periodX) ampX (truncated); a zero
 * period counts as 1 with the amplitude dropped. */
int Vec_Sine(Bmp_t* b, int periodX, int phaseX, int ampX, int periodY, int phaseY, int ampY)
{
	int16_t *rowDx, *colDy;
	int x, y;
	double twoPi = 6.283185307179586;
	VEC_CHECK(b);
	if(periodX == 0)
	{
		periodX = 1;
		ampX = 0;
	}
	if(periodY == 0)
	{
		periodY = 1;
		ampY = 0;
	}
	rowDx = (int16_t*)BGI_Alloc((size_t)b->h * 2);
	colDy = (int16_t*)BGI_Alloc((size_t)b->w * 2);
	for(y = 0; y < b->h; y++)
		rowDx[y] = (int16_t)BGI_Ftol(sin(((double)y + (double)phaseY) * (1.0 / (double)periodY) * twoPi) * (double)ampY * 16.0);
	for(x = 0; x < b->w; x++)
		colDy[x] = (int16_t)BGI_Ftol(sin(((double)x + (double)phaseX) * (1.0 / (double)periodX) * twoPi) * (double)ampX * 16.0);
	for(y = 0; y < b->h; y++)
	{
		int16_t* row = (int16_t*)(b->pixels + (size_t)y * (size_t)b->pitch);
		for(x = 0; x < b->w; x++)
		{
			row[x * 2] = rowDx[y];
			row[x * 2 + 1] = colDy[x];
		}
	}
	BGI_Free(rowDx);
	BGI_Free(colDy);
	return 0;
}

int BmpOp_VecSine(int slot, int periodX, int phaseX, int ampX, int periodY, int phaseY, int ampY)
{
	Bmp_t b;
	if(!BmpMgr_GetInfo(gBmpMgr, &b, slot))
		return (int)0x80000001;
	return Vec_Sine(&b, periodX, phaseX, ampX, periodY, phaseY, ampY);
}

int BmpOp_VecBend(int slot, int cx, int cy, int radius, int height)
{
	Bmp_t b;
	if(!BmpMgr_GetInfo(gBmpMgr, &b, slot))
		return (int)0x80000001;
	return Vec_Bend(&b, cx, cy, radius, height);
}

/* "92 10": a PM_VECDIST field (direction + distance, 3 words per pixel)
 * radiating from (cx, cy): the direction is the unit vector scaled by
 * 32767 (type 1 swaps its components), the distance word is (4 dist) mod
 * (4 range) (mod 2^32 - 1 for range 0).  0x80000009 for a type other than
 * 0 / 1.  At the centre pixel the direction comes out as (0, 0): the 0 / 0
 * is a NaN, which BGI_Ftol turns into 0x80000000 and the int16 cast into 0. */
int VecDist_Radial(Bmp_t* b, int type, int cx, int cy, int range)
{
	uint32_t divisor;
	int x, y;

	if(b->mode != PM_VECDIST || (uint32_t)b->w == 0 || (uint32_t)b->h == 0)
		return (int)0x80000003;
	if((uint32_t)type >= 2)
		return (int)0x80000009;
	divisor = (uint32_t)range > 0 ? (uint32_t)range * 4 : 0xffffffffu;
	for(y = 0; y < b->h; y++)
	{
		int16_t* p = (int16_t*)(b->pixels + (size_t)y * (size_t)b->pitch);
		int dy = y - cy;
		for(x = 0; x < b->w; x++, p += 3)
		{
			int dx = x - cx;
			double dist = sqrt((double)(dy * dy + dx * dx));
			double inv = 1.0 / dist;
			double tx = (double)(0x7fff * dx), ty = (double)(0x7fff * dy);
			p[0] = (int16_t)BGI_Ftol((type == 0 ? tx : ty) * inv);
			p[1] = (int16_t)BGI_Ftol(inv * (type == 0 ? ty : tx));
			p[2] = (int16_t)((uint32_t)BGI_Ftol(dist * 4.0) % divisor);
		}
	}
	return 0;
}

int BmpOp_VecDistRadial(int slot, int type, int cx, int cy, int range)
{
	Bmp_t b;
	if(!BmpMgr_GetInfo(gBmpMgr, &b, slot))
		return (int)0x80000001;
	return VecDist_Radial(&b, type, cx, cy, range);
}

/* "92 11": a PM_VECDIST field for the straight wipes: type 0 / 2 point
 * right, 1 / 3 down; the distance word is 4 y for types 0 and 3, 4 x for 1
 * and 2.  0x8000000A for a type above 3. */
int VecDist_Linear(Bmp_t* b, int type)
{
	static const int horizontal[4] = {1, 0, 1, 0};
	static const int useY[4] = {1, 0, 0, 1};
	int x, y;

	if(b->mode != PM_VECDIST || (uint32_t)b->w == 0 || (uint32_t)b->h == 0)
		return (int)0x80000003;
	if((uint32_t)type >= 4)
		return (int)0x8000000a;
	for(y = 0; y < b->h; y++)
	{
		int16_t* p = (int16_t*)(b->pixels + (size_t)y * (size_t)b->pitch);
		for(x = 0; x < b->w; x++, p += 3)
		{
			p[0] = horizontal[type] ? 0x7fff : 0;
			p[1] = horizontal[type] ? 0 : 0x7fff;
			p[2] = (int16_t)((useY[type] ? y : x) << 2);
		}
	}
	return 0;
}

int BmpOp_VecDistLinear(int slot, int type)
{
	Bmp_t b;
	if(!BmpMgr_GetInfo(gBmpMgr, &b, slot))
		return (int)0x80000001;
	return VecDist_Linear(&b, type);
}

// ---- bwef --------------------------------------------------------------------------------------

/* Parse a "bwef" file (particle wind data, "C0 F0") of `size` bytes: an
 * 8-byte magic, the entry count at 0x14, a dword at 0x18 shared by every
 * entry, and from 0x120 one dword per entry, which `base` is added to.
 * Writes the entries to out[] (the caller's array) and their count to
 * *outCount.  0 ok, 0x80000002 bad magic, 0x80000003 the size does not
 * match the count. */
int ParseBwef(BwefEntry_t* out, int32_t* outCount, const void* data, uint32_t size, uint32_t base)
{
	const uint8_t* d = (const uint8_t*)data;
	uint32_t count, i;

	if(memcmp(d, "bwef    ", 8) != 0)
		return (int)0x80000002;
	count = *(const uint32_t*)(d + 0x14);
	if(size != 0x120 + 4 * count)
		return (int)0x80000003;
	for(i = 0; i < count; i++)
	{
		out[i].offset = *(const uint32_t*)(d + 0x120 + 4 * i) + base;
		out[i].common = *(const uint32_t*)(d + 0x18);
	}
	*outCount = (int32_t)count;
	return 0;
}

// load `name` from archive `arc` and parse it; the ParseBwef codes, 0x80000001 for a missing or empty file
int LoadBwef(const char* arc, const char* name, BwefEntry_t* out, int32_t* outCount, uint32_t base)
{
	uint32_t size = GetFileSize_(arc, name), n;
	uint8_t* buf;
	int r;

	if(size == 0)
		return (int)0x80000001;
	buf = (uint8_t*)BGI_Alloc(size);
	n = LoadFile(buf, arc, name);
	r = ParseBwef(out, outCount, buf, n, base);
	BGI_Free(buf);
	return r;
}
