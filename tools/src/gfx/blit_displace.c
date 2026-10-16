/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * blit_displace.c - the displacement by vector maps (effector mode 0, background
 *                   type 6, "90 1A") and the horizontal box blur (effector
 *                   mode 1, background type 7, "90 1B") of
 *                   inc/bgi/gfx/bitmap.h
 */
#include "blit_internal.h"

// ---- displacement by vector maps (effector mode 0, background type 6) --------------------------

// a PM_VECTOR pixel: low 16 bits dx, high 16 bits dy, 12.4 fixed point
static inline int VecX(uint32_t v)
{
	return (int16_t)(v & 0xffff);
}

static inline int VecY(uint32_t v)
{
	return (int16_t)(v >> 16);
}

/* fetch a 32-bit source pixel at byte offsets (0 outside) as the original
 * does: the row offset is checked against the whole bitmap, the column
 * offset against the row width */
static inline uint32_t FetchOff(const Bmp_t* src, int rowOff, int colOff, int rowBytes, int totalBytes)
{
	if((unsigned)colOff >= (unsigned)rowBytes || (unsigned)rowOff >= (unsigned)totalBytes)
		return 0;
	return *(const uint32_t*)(src->pixels + rowOff + colOff);
}

// nearest-pixel displacement, one vector scaled by level: d(x, y) = s(x + dx, y + dy) with (v * level + 0x800) >> 12
static void Displace_Int(Bmp_t* dst, const Bmp_t* src, const Bmp_t* vec, int level)
{
	int rowBytes = src->w * 4, totalBytes = src->h * src->pitch, y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* v = (const uint32_t*)ROW(vec, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			int dy = ((VecY(v[x]) * level) + 0x800) >> 12;
			int dx = ((VecX(v[x]) * level) + 0x800) >> 12;
			d[x] = FetchOff(src, y * src->pitch + dy * src->pitch, (x + dx) * 4, rowBytes, totalBytes);
		}
	}
}

/* nearest pixel, two vectors mixed by level: the vector is v1 + (v2 - v1)
 * * level / 256 with v1 truncated to whole pixels first (12.4 with the
 * fraction cleared), rounded to the nearest pixel in 16-bit arithmetic */
static void Displace_Int2(Bmp_t* dst, const Bmp_t* src, const Bmp_t* vec1, const Bmp_t* vec2, int level)
{
	int rowBytes = src->w * 4, totalBytes = src->h * src->pitch, y, x;
	int L = level * 64;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* v1 = (const uint32_t*)ROW(vec1, y);
		const uint32_t* v2 = (const uint32_t*)ROW(vec2, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			int a1x = (int16_t)(VecX(v1[x]) & 0xfffc), a1y = (int16_t)(VecY(v1[x]) & 0xfffc);
			int tx = (int16_t)(VecX(v2[x]) - a1x), ty = (int16_t)(VecY(v2[x]) - a1y);
			int dx = ((int16_t)((tx * L) >> 16) + (a1x >> 2) + 2);
			int dy = ((int16_t)((ty * L) >> 16) + (a1y >> 2) + 2);
			dx = (int16_t)dx >> 2;
			dy = (int16_t)dy >> 2;
			d[x] = FetchOff(src, y * src->pitch + dy * src->pitch, (x + dx) * 4, rowBytes, totalBytes);
		}
	}
}

/* bilinear sample shared by the three bilinear displacers: v is the
 * displacement in 13.3 fixed point per 16-bit half; the 2 x 2
 * neighbourhood is read with FetchOff's bounds rules (0 outside) and
 * mixed with 3-bit fractions, all four bytes */
static inline uint32_t SampleBilinear(const Bmp_t* src, int rowOff, int x, uint32_t v, int rowBytes, int totalBytes)
{
	int dx = (int16_t)(v & 0xffff), dy = (int16_t)(v >> 16);
	int fx = dx & 7, fy = dy & 7;
	int colOff = (x + (dx >> 3)) * 4;
	int r0 = rowOff + (dy >> 3) * src->pitch;
	uint32_t p00 = 0, p01 = 0, p10 = 0, p11 = 0;
	int i, out = 0;
	if((unsigned)r0 < (unsigned)totalBytes)
	{
		if((unsigned)colOff < (unsigned)rowBytes)
			p00 = *(const uint32_t*)(src->pixels + r0 + colOff);
		if((unsigned)(colOff + 4) < (unsigned)rowBytes)
			p01 = *(const uint32_t*)(src->pixels + r0 + colOff + 4);
	}
	if(fy)
	{
		int r1 = r0 + src->pitch;
		if((unsigned)r1 < (unsigned)totalBytes)
		{
			if((unsigned)colOff < (unsigned)rowBytes)
				p10 = *(const uint32_t*)(src->pixels + r1 + colOff);
			if((unsigned)(colOff + 4) < (unsigned)rowBytes)
				p11 = *(const uint32_t*)(src->pixels + r1 + colOff + 4);
		}
	}
	for(i = 0; i < 32; i += 8)
	{
		int a = (int)((p00 >> i) & 0xff), b = (int)((p01 >> i) & 0xff);
		int c = (int)((p10 >> i) & 0xff), e = (int)((p11 >> i) & 0xff);
		int h0 = (((a - b) * (8 - fx)) >> 3) + b;
		int h1 = (((c - e) * (8 - fx)) >> 3) + e;
		out |= Sat8((((h0 - h1) * (8 - fy)) >> 3) + h1) << i;
	}
	return (uint32_t)out;
}

// bilinear, full vector (vector >> 1 per half: 12.4 -> 13.3)
static void Displace_Lin(Bmp_t* dst, const Bmp_t* src, const Bmp_t* vec)
{
	int rowBytes = src->w * 4, totalBytes = src->h * src->pitch, y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* v = (const uint32_t*)ROW(vec, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t h = (uint32_t)(uint16_t)(VecX(v[x]) >> 1) | (uint32_t)(uint16_t)(VecY(v[x]) >> 1) << 16;
			d[x] = SampleBilinear(src, y * src->pitch, x, h, rowBytes, totalBytes);
		}
	}
}

// bilinear, vector scaled by level: (v * (level << 7)) >> 16
static void Displace_LinLevel(Bmp_t* dst, const Bmp_t* src, const Bmp_t* vec, int level)
{
	int rowBytes = src->w * 4, totalBytes = src->h * src->pitch, y, x, L = level << 7;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* v = (const uint32_t*)ROW(vec, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			uint32_t h = (uint32_t)(uint16_t)((VecX(v[x]) * L) >> 16) | (uint32_t)(uint16_t)((VecY(v[x]) * L) >> 16) << 16;
			d[x] = SampleBilinear(src, y * src->pitch, x, h, rowBytes, totalBytes);
		}
	}
}

// bilinear, two vectors mixed by level: (v1 + (v2 - v1) * level / 256) >> 1, v1 with its lowest bit cleared first
static void Displace_Lin2(Bmp_t* dst, const Bmp_t* src, const Bmp_t* vec1, const Bmp_t* vec2, int level)
{
	int rowBytes = src->w * 4, totalBytes = src->h * src->pitch, y, x, L = level << 7;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* v1 = (const uint32_t*)ROW(vec1, y);
		const uint32_t* v2 = (const uint32_t*)ROW(vec2, y);
		uint32_t* d = (uint32_t*)ROW(dst, y);
		for(x = 0; x < src->w; x++)
		{
			int a1x = (int16_t)(VecX(v1[x]) & 0xfffe), a1y = (int16_t)(VecY(v1[x]) & 0xfffe);
			int tx = (int16_t)(VecX(v2[x]) - a1x), ty = (int16_t)(VecY(v2[x]) - a1y);
			int hx = (int16_t)(((tx * L) >> 16) + (a1x >> 1));
			int hy = (int16_t)(((ty * L) >> 16) + (a1y >> 1));
			uint32_t h = (uint32_t)(uint16_t)hx | (uint32_t)(uint16_t)hy << 16;
			d[x] = SampleBilinear(src, y * src->pitch, x, h, rowBytes, totalBytes);
		}
	}
}

/*
 * Read src through the PM_VECTOR map vec1 - or through the mix of vec1
 * and vec2 by level / 256 when vec2 is given - into dst, a picture of
 * src's mode and size.  With one map, `level` scales the vector (0 copies
 * src); amount != 0 selects bilinear sampling.  Results: 0 ok, 3 bad
 * level, 1 mode mismatch, 0xc / 0xd vec1 / vec2 is not a vector map of
 * src's size.  Only 32-bit pictures are drawn; other matching modes
 * return 0 untouched.
 */
int Blit_Displace(Bmp_t* dst, const Bmp_t* src, const Bmp_t* vec1, const Bmp_t* vec2, int level, int amount)
{
	if((unsigned)level > 0x100u)
		return 3;
	if(vec1->mode != PM_VECTOR || vec1->w != src->w || vec1->h != src->h)
		return 0xc;
	if(vec2 && (vec2->mode != PM_VECTOR || vec2->w != vec1->w || vec2->h != vec1->h))
		return 0xd;
	if(dst->mode != src->mode)
		return 1;
	if(src->mode != PM_RGB32 && src->mode != PM_ARGB32)
		return 0;
	if(vec2)
	{
		if(amount == 0)
			Displace_Int2(dst, src, vec1, vec2, level);
		else if(level == 0)
			Displace_Lin(dst, src, vec1);
		else if(level == 0x100)
			Displace_Lin(dst, src, vec2);
		else
			Displace_Lin2(dst, src, vec1, vec2, level);
	}
	else if(amount != 0)
	{
		if(level == 0)
			Bmp_CopyRect(dst, src);
		else if(level == 0x100)
			Displace_Lin(dst, src, vec1);
		else
			Displace_LinLevel(dst, src, vec1, level);
	}
	else
	{
		if(level == 0)
			Bmp_CopyRect(dst, src);
		else
			Displace_Int(dst, src, vec1, level);
	}
	return 0;
}

// ---- horizontal box blur ("gradient", effector mode 1, background type 7) ----------------------

/* Horizontal box blur with a window of 2 * level + 1 pixels (level 0
 * copies, level is capped at 0xff); clamp 0 pads with zero outside the
 * row, 1 repeats the edge pixel.  Each byte of the running sum is
 * pre-shifted by 0 .. 2 bits so that it fits a 16-bit lane, and the
 * average is sum * (0x10003 / n) >> 16 as the original's pmulhw does. */
static void BoxBlur(Bmp_t* dst, const Bmp_t* src, int level, int clamp)
{
	int n, half, shift, q, w = src->w, y, x, i;
	uint32_t* tmp;
	if(level <= 0)
	{
		Blit_Copy(dst, src);
		return;
	}
	if(level >= 0x100)
		level = 0xff;
	n = 2 * level + 1;
	half = n >> 1;
	shift = n < 0x40 ? 0 : (n < 0x80 ? 1 : 2);
	q = (0x10003 / n) & 0xffff;
	tmp = (uint32_t*)BGI_Alloc((size_t)w * 4);
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)ROW(src, y);
		int sum[4] = {0, 0, 0, 0};
		for(i = -half; i < -half + n; i++)
		{
			int idx = i;
			if(clamp)
				idx = idx < 0 ? 0 : (idx >= w ? w - 1 : idx);
			else if(idx < 0 || idx >= w)
				continue;
			{
				uint32_t p = s[idx];
				int c;
				for(c = 0; c < 4; c++)
					sum[c] += ((p >> (c * 8)) & 0xff) >> shift;
			}
		}
		for(x = 0; x < w; x++)
		{
			int out = 0, c, idx;
			for(c = 0; c < 4; c++)
				out |= Sat8((((int16_t)sum[c] * q) >> 16) << shift) << (c * 8);
			tmp[x] = (uint32_t)out;
			// slide: drop x - half, take x + half + 1
			idx = x - half;
			if(clamp ? 1 : idx >= 0)
			{
				uint32_t p = s[idx < 0 ? 0 : idx];
				for(c = 0; c < 4; c++)
					sum[c] -= ((p >> (c * 8)) & 0xff) >> shift;
			}
			idx = x + half + 1;
			if(clamp ? 1 : idx < w)
			{
				uint32_t p = s[idx >= w ? w - 1 : idx];
				for(c = 0; c < 4; c++)
					sum[c] += ((p >> (c * 8)) & 0xff) >> shift;
			}
		}
		memcpy(ROW(dst, y), tmp, (size_t)w * 4);
	}
	BGI_Free(tmp);
}

/* The blur of src into dst, a picture of the same mode and size: type 0
 * pads with black, 1 repeats the edge; `level` is the half width.  0 ok,
 * 0xe unknown type, 3 level above 0x100, 0xf mode / size mismatch.  Only
 * 32-bit pictures are drawn; other matching modes return 0 untouched. */
int Blit_Gradient(Bmp_t* dst, const Bmp_t* src, int type, int level)
{
	if(type != 0 && type != 1)
		return 0xe;
	if((unsigned)level > 0x100u)
		return 3;
	if(dst->mode != src->mode || dst->w != src->w || dst->h != src->h)
		return 0xf;
	if(src->mode == PM_RGB32 || src->mode == PM_ARGB32)
		BoxBlur(dst, src, level, type);
	return 0;
}
