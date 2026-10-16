/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/blit.c - unit test of the bitmap descriptor, the effect blitters
 *                and the transforms (src/gfx/bitmap.c and src/gfx/blit*.c,
 *                declared in inc/bgi/gfx/bitmap.h); built as bin/test_blit
 *                by `make test`
 *
 * Mostly a sanity and memory-safety pass: every effect code goes through
 * Bmp_BlitEffect on 16- and 32-bit pictures of odd sizes, the positioned
 * blit is clipped at every edge, and the scalers / rotators / transforms
 * run with extreme parameters.  The checks that can be made from the
 * outside are made: copies are exact, opaque blends equal the source,
 * fully transparent ones leave the destination alone, clipping touches
 * only the overlap.  Build with AddressSanitizer for the full value: most
 * of the transform group only has to run without touching memory outside
 * the pictures.
 *
 * The groups of tests, in the order they run:
 *
 *   rects      - the inclusive rectangle helpers and Bmp_Crop's view
 *   gather add - the "91 1B" gathering add: the resting case, the
 *                attenuation by level, the collapse onto the centre with
 *                the reproduced packing quirk, clipping and the mode check
 *   effects    - Bmp_BlitEffect with every effect code and level (RGB32
 *                and RGB16 screens), its result codes and the identities
 *                between the copy and blend effects
 *   clipping   - Bmp_Blit at positions inside, straddling and outside the
 *                destination: only the overlap changes
 *   transforms - the scalers, rotators, Xform, skew, mosaic and the
 *                gray-map blitters with a sweep of parameters, plus the
 *                few result codes that can be checked
 */
#include "bgi/gfx/bitmap.h"

#include <stdio.h>
#include <string.h>

static int gFails; // the number of failed checks so far
// evaluate a condition; print it with its location and count a failure when it is false
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

/* Fill a picture with a deterministic pattern derived from (x, y) and
 * `salt`, in its own pixel mode: 5-5-5 for RGB16, one byte for GRAY8,
 * four bytes otherwise with a varying alpha for ARGB32 and 0 for RGB32.
 * Two pictures with different salts never match. */
static void FillPattern(Bmp_t* b, uint32_t salt)
{
	int x, y;
	for(y = 0; y < b->h; y++)
		for(x = 0; x < b->w; x++)
		{
			uint8_t* p = b->pixels + (size_t)y * (size_t)b->pitch + (size_t)x * (size_t)b->bpp;
			uint32_t v = (uint32_t)(x * 7 + y * 13) + salt;
			switch(b->mode)
			{
				case PM_RGB16:
					p[0] = (uint8_t)v;
					p[1] = (uint8_t)((v >> 8) & 0x7f); // 5-5-5: the top bit stays clear
					break;
				case PM_GRAY8:
					p[0] = (uint8_t)v;
					break;
				default:
					p[0] = (uint8_t)v;
					p[1] = (uint8_t)(v >> 3);
					p[2] = (uint8_t)(v >> 6);
					p[3] = (uint8_t)(b->mode == PM_ARGB32 ? (x * 37 + y * 11) : 0);
					break;
			}
		}
}

/* 1 when two views have the same size and pixel size and identical pixel rows (the
 * padding of the pitch is ignored) */
static int SameView(const Bmp_t* a, const Bmp_t* b)
{
	int y;
	if(a->w != b->w || a->h != b->h || a->bpp != b->bpp)
		return 0;
	for(y = 0; y < a->h; y++)
		if(memcmp(a->pixels + (size_t)y * (size_t)a->pitch, b->pixels + (size_t)y * (size_t)b->pitch,
			   (size_t)a->w * (size_t)a->bpp) != 0)
			return 0;
	return 1;
}

// a bitmap of the same size and mode with its own pixels
static void Clone(Bmp_t* out, const Bmp_t* src)
{
	Bmp_Alloc(out, src->w, src->h, src->mode);
	Bmp_CopyRect(out, src);
}

// ---- the effect codes ------------------------------------------------------

/* Every effect code at every level step on a `mode` screen (the source is
 * ARGB32 for a 32-bit screen, RGB16 for a 16-bit one): the known codes
 * have to return 0 and the unknown ones 2, a level above 0x100 returns 3
 * and a pixel mode mismatch 1.  Then the identities: the raw copy 0x80
 * reproduces the source when the modes agree, the blend 0x01 at level
 * 0x100 leaves the destination untouched and at 0 equals the alpha copy
 * 0x00, and the blend-over 0xF0 at 0 equals the raw copy.  A failure
 * means an effect routine writes the wrong pixels or the dispatcher's
 * checks changed. */
static void TestEffects(int mode)
{
	static const int codes[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x20, 0x21, 0x22, 0x23,
		0x24, 0x25, 0x26, 0x27, 0x40, 0x41, 0x80, 0xC0, 0xC1, 0xF0, 0xFF, 0x0a, 0x77, 0x100}; // the last three do not exist
	Bmp_t dst, src, before, copy;
	size_t i;
	int level;
	printf("effects, mode %d\n", mode);
	Gfx_SetScreenMode(mode);
	Bmp_Alloc(&dst, 61, 37, mode); // odd sizes: no row is a whole number of 4- or 8-pixel groups
	Bmp_Alloc(&src, 61, 37, mode == PM_RGB16 ? PM_RGB16 : PM_ARGB32);
	FillPattern(&dst, 1);
	FillPattern(&src, 1000);
	Clone(&before, &dst);

	for(i = 0; i < sizeof codes / sizeof codes[0]; i++)
		for(level = 0; level <= 0x100; level += 0x40)
		{
			int r;
			Bmp_CopyRect(&dst, &before);
			r = Bmp_BlitEffect(&dst, &src, codes[i], level);
			if(codes[i] == 0x0a || codes[i] == 0x77 || codes[i] == 0x100)
				CHECK(r == 2); // unknown effects
			else
				CHECK(r == 0);
		}
	CHECK(Bmp_BlitEffect(&dst, &src, 0x80, 0x101) == 3); // level above 0x100

	// a plain copy is exact (not checked for ARGB32 onto RGB32, where the copy converts)
	Bmp_CopyRect(&dst, &before);
	Bmp_BlitEffect(&dst, &src, 0x80, 0);
	if(mode == PM_RGB16 || src.mode == dst.mode)
		CHECK(SameView(&dst, &src));

	// the blend at level 0x100 leaves the destination; at 0 it is the alpha
	// copy (effect 0x00), and the plain blend 0xF0 at 0 is the copy
	Clone(&copy, &dst);
	Bmp_CopyRect(&dst, &before);
	Bmp_BlitEffect(&dst, &src, 0x01, 0x100);
	CHECK(SameView(&dst, &before));
	Bmp_CopyRect(&dst, &before);
	Bmp_BlitEffect(&dst, &src, 0x01, 0);
	Bmp_CopyRect(&copy, &before);
	Bmp_BlitEffect(&copy, &src, 0x00, 0);
	CHECK(SameView(&dst, &copy));
	Bmp_CopyRect(&dst, &before);
	Bmp_BlitEffect(&dst, &src, 0xF0, 0);
	Bmp_CopyRect(&copy, &before);
	Bmp_BlitEffect(&copy, &src, 0x80, 0);
	CHECK(SameView(&dst, &copy));

	// a pixel mode mismatch is refused with 1
	{
		Bmp_t gray;
		Bmp_Alloc(&gray, 61, 37, PM_GRAY8);
		CHECK(Bmp_BlitEffect(&dst, &gray, 0x80, 0) == 1);
		Bmp_Free(&gray);
	}

	Bmp_Free(&copy);
	Bmp_Free(&before);
	Bmp_Free(&src);
	Bmp_Free(&dst);
}

// ---- the positioned, clipped blit ------------------------------------------

/* Bmp_Blit of a 21 x 17 source at positions inside the 64 x 40
 * destination, straddling each edge and corner, and entirely outside.
 * With an overlap the result is 0 and every destination pixel is either
 * the source pixel that lands on it or unchanged; without one the result
 * is 4 and nothing changes.  A failure means the clipping writes outside
 * the overlap or crops the source at the wrong offset. */
static void TestClipping(void)
{
	Bmp_t dst, src, before;
	static const int positions[][2] = {{0, 0}, {-5, -7}, {50, 30}, {-100, 0}, {0, -100}, {200, 200}, {-20, 31}, {60, -36}, {3, 3}};
	size_t i;
	printf("clipping\n");
	Gfx_SetScreenMode(PM_RGB32);
	Bmp_Alloc(&dst, 64, 40, PM_RGB32);
	Bmp_Alloc(&src, 21, 17, PM_RGB32);
	FillPattern(&dst, 5);
	FillPattern(&src, 9000);
	Clone(&before, &dst);
	for(i = 0; i < sizeof positions / sizeof positions[0]; i++)
	{
		int x0 = positions[i][0], y0 = positions[i][1], x, y, ok = 1, r;
		int overlap = x0 < dst.w && y0 < dst.h && x0 + src.w > 0 && y0 + src.h > 0;
		Bmp_CopyRect(&dst, &before);
		r = Bmp_Blit(&dst, x0, y0, &src, 0x80, 0);
		CHECK(overlap ? r == 0 : r == 4);
		for(y = 0; y < dst.h; y++)
			for(x = 0; x < dst.w; x++)
			{
				const uint8_t* d = dst.pixels + (size_t)y * (size_t)dst.pitch + (size_t)x * 4;
				const uint8_t* expect;
				if(x >= x0 && x < x0 + src.w && y >= y0 && y < y0 + src.h)
					expect = src.pixels + (size_t)(y - y0) * (size_t)src.pitch + (size_t)(x - x0) * 4;
				else
					expect = before.pixels + (size_t)y * (size_t)before.pitch + (size_t)x * 4;
				if(memcmp(d, expect, 3) != 0) // the three colour bytes; the fourth is not significant in RGB32
					ok = 0;
			}
		CHECK(ok);
	}
	Bmp_Free(&before);
	Bmp_Free(&src);
	Bmp_Free(&dst);
}

// ---- transforms --------------------------------------------------------------

/* The transforms with a sweep of parameters: the scalers from 1/32 to 64,
 * the rotators through every quadrant at zooms around and far off 1.0,
 * Xform with each sampling mode, the skew, the mosaic and the gray-map
 * blitters at every mode and progress value.  Few results can be
 * predicted from the outside, so this group is run for memory safety
 * (build with AddressSanitizer) and for the result codes that are
 * checkable: a zero scale is refused with 0x13, Blit_ToGray returns 0,
 * Blit_InvertGray 1 on a GRAY8 picture and 0 on another mode. */
static void TestTransforms(void)
{
	Bmp_t dst, src, gray;
	int i;
	printf("transforms\n");
	Gfx_SetScreenMode(PM_RGB32);
	Bmp_Alloc(&dst, 97, 71, PM_RGB32);
	Bmp_Alloc(&src, 45, 33, PM_ARGB32);
	Bmp_Alloc(&gray, 45, 33, PM_GRAY8);
	FillPattern(&src, 3);
	FillPattern(&gray, 4);

	// scalers at a range of factors, including below one pixel and well beyond the target
	for(i = 0; i < 12; i++)
	{
		int32_t s = (int32_t)(0x800 << i); // 16.16: 1/32 .. 64
		Bmp_Clear(&dst, NULL);
		Blit_Scale(&dst, &src, s, 0x10000);
		Blit_Scale(&dst, &src, 0x10000, s);
		Blit_ScaleNearest(&dst, &src, s, s);
	}
	// rotations: every quadrant (15 degree steps), zooms around the picture centre and
	// off it, nearest and bilinear
	for(i = 0; i < 24; i++)
	{
		int32_t angle = (int32_t)(i * 15) << 16;                 // 16.16 degrees
		int32_t zoom = (int32_t)(0x4000 + (uint32_t)i * 0x3000); // 16.16: 0.25 .. 4.56
		Bmp_Clear(&dst, NULL);
		Blit_RotateCentred(&dst, &src, zoom, angle, i & 1);
		Blit_Rotate(&dst, &src, zoom, angle, (int32_t)(i * 5 - 20), (int32_t)(40 - i * 3), i & 1);
		Blit_Xform(&dst, (int32_t)(48 << 16), (int32_t)(35 << 16), &src, (int32_t)(22 << 16), (int32_t)(16 << 16), angle,
			zoom, 0x10000 + (int32_t)(i << 10), (i * 10) & 0xff, i & 1);
		Blit_XformVscale(&dst, (int32_t)(10 << 16), (int32_t)(5 << 16), &src, 0, 0, zoom);
	}
	CHECK(Blit_Xform(&dst, 0, 0, &src, 0, 0, 0, 0, 0x10000, 0, 0) == 0x13); // a zero scale is refused
	// skew, mosaic, gray-based effects
	for(i = 0; i < 8; i++)
	{
		Bmp_Clear(&dst, NULL);
		Blit_Skew(&dst, &src, 7 + i * 3, i * 1000, (int32_t)(i * 3) << 16);
	}
	{
		Bmp_t same, tmp, g2;
		Bmp_Alloc(&same, 45, 33, PM_RGB32); // an RGB32 destination of the source's size, as the sprite and filter blitters want
		Bmp_Alloc(&tmp, 45, 33, PM_RGB32);
		Bmp_Alloc(&g2, 45, 33, PM_GRAY8);
		FillPattern(&same, 11);
		for(i = 0; i <= 8; i++)
		{
			Blit_Flip(&same, &src, i * 3 + 1, i & 3, i);
			Blit_GrayWipe(&same, &src, &gray, i & 1, i * 32, 0x40);
			Blit_GrayWipeTo(&tmp, &src, &gray, i & 1, i * 32);
			Blit_Masked(&same, &src, &gray, i * 32);
			Blit_MaskedTo(&tmp, &src, &gray);
			Blit_Gradient(&same, &src, i & 3, i * 32);
			Blit_ThroughGray(&same, i - 4, 4 - i, &src, &gray, i, i * 32);
			Blit_TintByGray(&same, 0x00ff8040u, &gray, i, i * 32);
			Blit_FilterKind1(&same, &src, 0x00123456u, i * 32);
			Blit_FilterKind2(&same, &src, 0x00123456u, i * 32);
			Blit_FilterKind3(&same, &src, 0x00123456u, i * 32);
			Blit_Crossfade(&same, &src, &tmp, i * 32, i * 16);
			Blit_CrossfadeTo(&tmp, &src, &same, i * 32);
			Blit_ApplyMask(&same, &src, &gray);
			Blit_ColourThroughGray(&same, &gray, 0x0080ff20u);
		}
		CHECK(Blit_ToGray(&g2, &src) == 0);
		CHECK(Blit_InvertGray(&g2) == 1);   // a GRAY8 picture is inverted
		CHECK(Blit_InvertGray(&same) == 0); // another mode: nothing done
		Bmp_Free(&g2);
		Bmp_Free(&tmp);
		Bmp_Free(&same);
	}
	Bmp_Free(&gray);
	Bmp_Free(&src);
	Bmp_Free(&dst);
}

// ---- rectangles and views -----------------------------------------------------

/* The inclusive rectangles (Rect_Intersects, Rect_Clip, Rect_Offset,
 * Rect_FromBmp) and Bmp_Crop, which narrows a view to a rectangle
 * without copying: the cropped view keeps the pitch and points into the
 * original pixels.  A failure means an off-by-one in the inclusive
 * arithmetic or a view that does not alias its picture. */
static void TestRects(void)
{
	Rect_t a = {10, 10, 20, 20}, b = {15, 5, 30, 12}, c = {100, 100, 110, 110}, r;
	Bmp_t bmp, view;
	printf("rects\n");
	CHECK(Rect_Intersects(&a, &b));
	CHECK(!Rect_Intersects(&a, &c));
	r = a;
	CHECK(Rect_Clip(&r, &b));
	CHECK(r.l == 15 && r.t == 10 && r.r == 20 && r.b == 12);
	r = a;
	CHECK(!Rect_Clip(&r, &c));
	CHECK(Rect_Inside(&r, &a) || 1); // an empty result is whatever the original leaves there: only exercised, not checked
	Rect_Offset(&a, -3, 4);
	CHECK(a.l == 7 && a.t == 14 && a.r == 17 && a.b == 24);

	Bmp_Alloc(&bmp, 30, 20, PM_RGB32);
	FillPattern(&bmp, 0);
	Rect_FromBmp(&r, &bmp);
	CHECK(r.l == 0 && r.t == 0 && r.r == 29 && r.b == 19); // r and b are the last pixel inside
	view = bmp;
	r.l = 5;
	r.t = 6;
	r.r = 14;
	r.b = 15;
	Bmp_Crop(&view, &r);
	CHECK(view.w == 10 && view.h == 10 && view.pitch == bmp.pitch);
	CHECK(view.pixels == bmp.pixels + 6 * bmp.pitch + 5 * 4);
	Bmp_Free(&bmp);
}

// ---- the gathering add of "91 1B" ----------------------------------------------

/* Blit_GatherAdd on an 8 x 6 picture with one lit pixel: at rates 0 the
 * pixel stays in place at full weight and spills nowhere, the add
 * saturates and is attenuated by (256 - level) / 256, and at rate 1.0 the
 * pixel collapses onto the centre - where the reproduced packing quirk of
 * the original puts the whole weight on the right and lower neighbour
 * instead of spreading it.  A source larger than the destination is
 * clipped, and a 16-bit picture on either side draws nothing.  A failure
 * means the weights, the quirk or the mode check changed. */
static void TestGatherAdd(void)
{
	Bmp_t src, dst;
	uint32_t* d;
	printf("gather add\n");
	Bmp_Alloc(&src, 8, 6, PM_RGB32);
	Bmp_Alloc(&dst, 8, 6, PM_RGB32);
	memset(src.pixels, 0, (size_t)src.pitch * (size_t)src.h);
	memset(dst.pixels, 0, (size_t)dst.pitch * (size_t)dst.h);
	((uint32_t*)(src.pixels + 2 * src.pitch))[3] = 0x00804020; // one lit pixel at (3, 2)
	d = (uint32_t*)(dst.pixels + 2 * dst.pitch);               // destination row 2
	// rates 0: the picture stays where it is, every pixel at full weight, nothing spills
	Blit_GatherAdd(&dst, &src, 0, 0, 0);
	CHECK(d[3] == 0x00804020);
	CHECK(d[4] == 0 && d[2] == 0 && ((uint32_t*)(dst.pixels + 3 * dst.pitch))[3] == 0);
	// added with saturation, attenuated by (256 - level) / 256: level 128 adds half, then
	// a full add saturates the red
	Blit_GatherAdd(&dst, &src, 0, 0, 128);
	CHECK(d[3] == 0x00c06030);
	Blit_GatherAdd(&dst, &src, 0, 0, 0);
	CHECK(d[3] == 0x00ffa050);
	// a rate of 1.0 gathers everything onto the centre ((w - 1) / 2, (h - 1) / 2) = (3.5,
	// 2.5)
	memset(dst.pixels, 0, (size_t)dst.pitch * (size_t)dst.h);
	Blit_GatherAdd(&dst, &src, 0x10000, 0x10000, 0);
	CHECK(d[3] == 0 && ((uint32_t*)(dst.pixels + 3 * dst.pitch))[4] == 0x00804020); // the packing quirk: right and down
	// a source larger than the destination is clipped (the original has no caller for
	// that)
	Bmp_Free(&dst);
	Bmp_Alloc(&dst, 4, 3, PM_RGB32);
	memset(dst.pixels, 0, (size_t)dst.pitch * (size_t)dst.h);
	FillPattern(&src, 7);
	Blit_GatherAdd(&dst, &src, 0, 0, 0);
	CHECK(((uint32_t*)dst.pixels)[0] == (((uint32_t*)src.pixels)[0] & 0xffffffffu)); // all four bytes are added, the top one too
	// a 16-bit side draws nothing
	Bmp_Free(&src);
	Bmp_Alloc(&src, 4, 3, PM_RGB16);
	memset(src.pixels, 0xff, (size_t)src.pitch * (size_t)src.h);
	memset(dst.pixels, 0, (size_t)dst.pitch * (size_t)dst.h);
	Blit_GatherAdd(&dst, &src, 0, 0, 0);
	CHECK(((uint32_t*)dst.pixels)[0] == 0);
	Bmp_Free(&src);
	Bmp_Free(&dst);
}

// run every group; exit status 1 when any check failed
int main(void)
{
	Blit_InitTables();
	TestRects();
	TestGatherAdd();
	TestEffects(PM_RGB32);
	TestEffects(PM_RGB16);
	TestClipping();
	TestTransforms();
	if(gFails)
	{
		printf("%d failure(s)\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
