/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tonecurve.c - the tone curves of "90 CC" / "90 CD" (1.616 on; interface
 *               in bgi/gfx/bmpops.h)
 *
 * A tone curve is registered under a number from three via points, one
 * per channel (R, G, B in that order): each channel's curve is the natural
 * cubic spline through (0, 0), the via point and (255, 255), sampled at
 * the 256 integer positions and clamped to 0 .. 255.  "90 CD" then makes a
 * copy of a bitmap through a curve: every pixel is first pulled towards
 * its tinted monochrome (the luminance weighted 77 / 150 / 29 for R / G /
 * B, multiplied by a colour), then put through the curve, then mixed with
 * the "colour film" - an overlay blend of the pixel with a second colour -
 * by a second level.  The original keeps the curves on its graphics
 * manager; here they are a list of this module, freed with the manager.
 */
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/bmpmgr.h"

typedef struct ToneCurve
{
	int32_t id;
	uint8_t lut[3][256]; // per channel (R, G, B): the output value of every input value
	struct ToneCurve* next;
} ToneCurve_t;

static ToneCurve_t* gToneCurves;

static ToneCurve_t* ToneCurve_Find(int32_t id)
{
	ToneCurve_t* c;
	for(c = gToneCurves; c; c = c->next)
		if(c->id == id)
			return c;
	return NULL;
}

// the original's rounding: half away from zero, then truncation
static int RoundToInt(double v)
{
	return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

/* the natural cubic spline through (0, 0), (x1, y1) and (255, 255)
 * evaluated at 0 .. 255 into out, clamped to 0 .. 255 */
static void Spline3(uint8_t out[256], int x1, int y1)
{
	double h0 = x1, h1 = 255.0 - x1;
	// the second-derivative coefficient of the inner knot (the outer ones are 0)
	double c1 = 3.0 * ((255.0 - y1) / h1 - (double)y1 / h0) / (2.0 * (h0 + h1));
	int i;
	for(i = 0; i < 256; i++)
	{
		double x0, y0, yNext, h, cA, cB, b, d, t, y;
		int v;
		if(i < x1)
		{ // the first segment, (0, 0) to the via point
			x0 = 0.0;
			y0 = 0.0;
			yNext = y1;
			h = h0;
			cA = 0.0;
			cB = c1;
		}
		else
		{ // the second, the via point to (255, 255)
			x0 = x1;
			y0 = y1;
			yNext = 255.0;
			h = h1;
			cA = c1;
			cB = 0.0;
		}
		t = (double)i - x0;
		b = (yNext - y0) / h - (2.0 * cA + cB) * h / 3.0;
		d = (cB - cA) / (3.0 * h);
		y = y0 + t * (b + t * (cA + t * d));
		v = RoundToInt(y);
		out[i] = (uint8_t)(v <= 0 ? 0 : v >= 256 ? 255
												 : v);
	}
}

/* "90 CC": register the curve `id` from the via points (x, y of R, G and B:
 * six words), replacing a curve of that number; a NULL `points` removes
 * the curve.  0 ok; 0x16 a via point is out of range (x must be 1 .. 254,
 * y 0 .. 255: nothing is registered - the original's handler does not
 * report this code, so neither does ours); 0x8000000f no curve `id` to
 * remove. */
uint32_t ToneCurve_Register(int32_t id, const int32_t* points)
{
	ToneCurve_t* c;
	int k;
	if(!points)
	{
		ToneCurve_t** link;
		for(link = &gToneCurves; *link; link = &(*link)->next)
		{
			if((*link)->id == id)
			{
				c = *link;
				*link = c->next;
				BGI_Free(c);
				return 0;
			}
		}
		return 0x8000000fu;
	}
	for(k = 0; k < 3; k++)
		if(points[k * 2] <= 0 || points[k * 2] >= 255 || points[k * 2 + 1] < 0 || points[k * 2 + 1] > 255)
			return 0x16;
	c = ToneCurve_Find(id);
	if(!c)
	{
		c = (ToneCurve_t*)BGI_Alloc(sizeof *c);
		c->id = id;
		c->next = gToneCurves;
		gToneCurves = c;
	}
	for(k = 0; k < 3; k++)
		Spline3(c->lut[k], points[k * 2], points[k * 2 + 1]);
	return 0;
}

// drop every curve (the graphics manager goes)
void ToneCurve_FreeAll(void)
{
	while(gToneCurves)
	{
		ToneCurve_t* next = gToneCurves->next;
		BGI_Free(gToneCurves);
		gToneCurves = next;
	}
}

/* one pixel (B G R x bytes) through the three steps; the top byte is the
 * caller's.  The arithmetic follows the original's MMX code: 16-bit
 * channels, the levels halved so that the products fit in a signed word. */
static uint32_t TonePixel(uint32_t px, const ToneCurve_t* c, uint32_t monoColour, int monoHalf, uint32_t filmColour,
	int filmHalf)
{
	int ch[3], k; // B, G, R
	int gray16;
	ch[0] = (int)(px & 0xff);
	ch[1] = (int)((px >> 8) & 0xff);
	ch[2] = (int)((px >> 16) & 0xff);
	// 1. towards the tinted monochrome: gray * tint / 256, mixed by the level
	gray16 = ch[0] * 29 + ch[1] * 150 + ch[2] * 77; // the luminance, times 256
	for(k = 0; k < 3; k++)
	{
		int tint = (int)((monoColour >> (8 * k)) & 0xff);
		int tinted = (gray16 * tint) >> 16;
		ch[k] += ((tinted - ch[k]) * monoHalf) >> 7;
	}
	// 2. the curve (R, G, B tables for the R, G, B channels)
	ch[0] = c->lut[2][ch[0] & 0xff];
	ch[1] = c->lut[1][ch[1] & 0xff];
	ch[2] = c->lut[0][ch[2] & 0xff];
	// 3. the colour film: an overlay of the film colour, mixed by its level
	for(k = 0; k < 3; k++)
	{
		int film = (int)((filmColour >> (8 * k)) & 0xff);
		int over = ch[k] > 127 ? 255 - (((255 - ch[k]) * (255 - film)) >> 7) : (ch[k] * film) >> 7;
		int v = ch[k] + (((over - ch[k]) * filmHalf) >> 7);
		ch[k] = v < 0 ? 0 : v > 255 ? 255
									: v;
	}
	return (px & 0xff000000u) | ((uint32_t)ch[2] << 16) | ((uint32_t)ch[1] << 8) | (uint32_t)ch[0];
}

/* "90 CD": bitmap `dst` becomes a copy of `src` (same size and mode) put
 * through curve `curveId` with the monochrome mix (colour, level 0 .. 256)
 * and the colour film (effect 8 or 0x26 - both the overlay -, colour,
 * level 0 .. 256); ARGB32 pixels with an alpha of 0 are left clear.  0 ok,
 * else the code of the error: 0x80000009 dst cannot be made, 0x8000000a no
 * src, 0x8000000f no such curve, 0x80000011 src is not RGB32 / ARGB32,
 * 0x80000013 bad effect, 0x80000014 bad film level, 0x80000015 bad
 * monochrome level. */
uint32_t BmpOp_ToneCurve(int dst, int src, int32_t curveId, uint32_t monoColour, int monoLevel, uint32_t filmColour,
	int effect, int filmLevel)
{
	Bmp_t s, d;
	const ToneCurve_t* c;
	int y, x, w, h;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 0x8000000au;
	if(!BmpMgr_Create(gBmpMgr, dst, s.w, s.h, s.mode))
		return 0x80000009u;
	BmpMgr_GetInfo(gBmpMgr, &d, dst);
	c = ToneCurve_Find(curveId);
	if(!c)
		return 0x8000000fu;
	if(s.mode != PM_RGB32 && s.mode != PM_ARGB32)
		return 0x80000011u;
	if((uint32_t)monoLevel > 0x100)
		return 0x80000015u;
	if(effect != 8 && effect != 0x26)
		return 0x80000013u;
	if((uint32_t)filmLevel > 0x100)
		return 0x80000014u;
	w = s.w < d.w ? s.w : d.w;
	h = s.h < d.h ? s.h : d.h;
	for(y = 0; y < h; y++)
	{
		const uint32_t* sp = (const uint32_t*)(s.pixels + (size_t)y * (size_t)s.pitch);
		uint32_t* dp = (uint32_t*)(d.pixels + (size_t)y * (size_t)d.pitch);
		for(x = 0; x < w; x++)
		{
			uint32_t px = sp[x];
			if(s.mode == PM_ARGB32 && !(px & 0xff000000u))
				continue;
			dp[x] = TonePixel(px, c, monoColour, monoLevel >> 1, filmColour, filmLevel >> 1);
		}
	}
	return 0;
}
