/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/blitgold.c - the blitters' pixels, byte for byte (src/gfx/blit*.c);
 *                    built as test_blitgold by `make test`
 *
 * The blitters repeat the original's arithmetic lane by lane, and a
 * faster way of writing a loop must give the same bytes.  This test
 * holds them to that: each case blits pictures of noise - every alpha,
 * runs of clear and of solid pixels, an odd size - and compares a sum of
 * the result with the one the plain lane-by-lane code gave when the test
 * was written.  `test_blitgold print` prints the table instead of
 * checking it (for a new case, or a blitter whose result is meant to
 * change).
 *
 * The cases: the alpha copy, the blend, the blend without alpha, the
 * add and the screen, each for every pair of 32-bit modes and a row of
 * levels; the rotating and scaling blit and its copy, with and without
 * smoothing, over transforms that leave the source on every side.
 */
#include "bgi/gfx/bitmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

enum
{
	W = 61,
	H = 37
};

static uint32_t gSeed;

static uint32_t Next(void)
{
	gSeed ^= gSeed << 13;
	gSeed ^= gSeed >> 17;
	gSeed ^= gSeed << 5;
	return gSeed;
}

/* noise with what the loops branch on: runs of clear pixels (alpha 0 and
 * 1), of solid ones (0xfe, 0xff), black ones, and everything between */
static void Noise(Bmp_t* b, uint32_t seed)
{
	int x, y, run = 0;
	uint32_t kind = 0;
	gSeed = seed * 2654435761u + 1;
	for(y = 0; y < b->h; y++)
		for(x = 0; x < b->w; x++)
		{
			uint32_t v = Next();
			if(run-- <= 0)
			{
				kind = Next() % 7;
				run = (int)(Next() % 9);
			}
			switch(kind)
			{
				case 0: v &= 0x00ffffffu; break;                    // clear
				case 1: v = (v & 0x00ffffffu) | 0x01000000u; break; // alpha 1: still clear to the blends
				case 2: v |= 0xff000000u; break;                    // solid
				case 3: v = (v & 0x00ffffffu) | 0xfe000000u; break;
				case 4: v &= 0xff000000u; break; // black
				default: break;
			}
			*(uint32_t*)(b->pixels + (size_t)y * (size_t)b->pitch + (size_t)x * 4) = v;
		}
}

static uint64_t Sum(const Bmp_t* b)
{
	uint64_t h = 1469598103934665603ull;
	int x, y;
	for(y = 0; y < b->h; y++)
		for(x = 0; x < b->w * 4; x++)
		{
			h ^= b->pixels[(size_t)y * (size_t)b->pitch + (size_t)x];
			h *= 1099511628211ull;
		}
	return h;
}

// the sums the lane-by-lane code gave, in the order the cases run
static const uint64_t kGold[] = {
#include "blitgold.inc"
};

static int gCase, gPrint;

static void Case(const char* name, const Bmp_t* dst)
{
	uint64_t sum = Sum(dst);
	if(gPrint)
		printf("\t0x%016llxull, // %s\n", (unsigned long long)sum, name);
	else if(gCase >= (int)(sizeof kGold / sizeof kGold[0]) || kGold[gCase] != sum)
	{
		printf("  FAIL case %d (%s): 0x%016llx\n", gCase, name, (unsigned long long)sum);
		gFails++;
	}
	gCase++;
}

static const int kModes[2] = {PM_RGB32, PM_ARGB32};
static const char* const kModeNames[2] = {"rgb", "argb"};

typedef void (*LevelBlit)(Bmp_t* dst, const Bmp_t* src, int level);

static void Levels(const char* what, LevelBlit blit, const int* levels, int count)
{
	char name[96];
	for(int dm = 0; dm < 2; dm++)
		for(int sm = 0; sm < 2; sm++)
			for(int i = 0; i < count; i++)
			{
				Bmp_t d, s;
				Bmp_Alloc(&d, W, H, kModes[dm]);
				Bmp_Alloc(&s, W, H, kModes[sm]);
				Noise(&d, 11 + (uint32_t)i);
				Noise(&s, 77 + (uint32_t)i * 3);
				blit(&d, &s, levels[i]);
				snprintf(name, sizeof name, "%s %s <- %s, level 0x%x", what, kModeNames[dm], kModeNames[sm], (unsigned)levels[i]);
				Case(name, &d);
				Bmp_Free(&d);
				Bmp_Free(&s);
			}
}

static void AlphaCopy(Bmp_t* dst, const Bmp_t* src, int level)
{
	(void)level;
	Blit_AlphaCopy(dst, src);
}

static void TestLevels(void)
{
	static const int kOne[1] = {0};
	static const int kBlend[7] = {1, 2, 0x3f, 0x80, 0x81, 0xfe, 0xff};
	static const int kAdd[8] = {1, 0x40, 0x7f, 0x80, 0xff, 0x100, 0x101, 0x1c0};
	Levels("alpha copy", AlphaCopy, kOne, 1);
	Levels("blend", Blit_Blend, kBlend, 7);
	Levels("over", Blit_Over, kBlend, 7);
	Levels("add", Blit_Add, kAdd, 8);
	Levels("screen", Blit_Fx06, kAdd, 8);
	Levels("dim", Blit_Dim, kBlend, 7);
}

// the transforms: where the source's centre goes, the turn and the two scales (16.16)
typedef struct Turn
{
	int32_t x, y, cx, cy, angle, sx, sy;
} Turn_t;

static const Turn_t kTurns[] = {
	{30 << 16, 18 << 16, 20 << 16, 12 << 16, 0, 0x10000, 0x10000},                       // a whole-pixel move: the plain copy
	{(30 << 16) + 0x4000, (18 << 16) + 0xc000, 20 << 16, 12 << 16, 0, 0x10000, 0x10000}, // a fraction of a pixel
	{30 << 16, 18 << 16, 20 << 16, 12 << 16, 0, 0x18000, 0x14000},                       // larger
	{30 << 16, 18 << 16, 20 << 16, 12 << 16, 0, 0x9000, 0xb000},                         // smaller: the source ends inside
	{28 << 16, 20 << 16, 20 << 16, 12 << 16, 17 << 16, 0x12000, 0x12000},                // turned
	{10 << 16, 30 << 16, 5 << 16, 30 << 16, 200 << 16, 0x20000, 0x8000},                 // turned far, stretched
	{-(3 << 16), -(2 << 16), 0, 0, 0, 0x10800, 0x10800},                                 // off the top left corner
	{(W - 2) << 16, (H - 2) << 16, 39 << 16, 22 << 16, 359 << 16, 0xffff, 0x10001},      // at the far corner
};

static void TestTurns(void)
{
	static const int kLevels[4] = {0, 1, 0x80, 0xff};
	char name[96];
	for(int copy = 0; copy < 2; copy++)
		for(int sm = 0; sm < 2; sm++)
			for(int smooth = 0; smooth < 2; smooth++)
				for(int t = 0; t < (int)(sizeof kTurns / sizeof kTurns[0]); t++)
					for(int l = 0; l < 4; l++)
					{
						const Turn_t* k = &kTurns[t];
						Bmp_t d, s;
						Bmp_Alloc(&d, W, H, copy ? kModes[sm] : PM_RGB32);
						Bmp_Alloc(&s, 40, 24, kModes[sm]);
						Noise(&d, 5 + (uint32_t)t);
						Noise(&s, 900 + (uint32_t)t * 7 + (uint32_t)l);
						if(copy)
							Blit_XformCopy(&d, k->x, k->y, &s, k->cx, k->cy, k->angle, k->sx, k->sy, kLevels[l], smooth);
						else
							Blit_Xform(&d, k->x, k->y, &s, k->cx, k->cy, k->angle, k->sx, k->sy, kLevels[l], smooth);
						snprintf(name, sizeof name, "%s <- %s, %s, turn %d, level 0x%x", copy ? "xform copy" : "xform", kModeNames[sm],
							smooth ? "smooth" : "nearest", t, (unsigned)kLevels[l]);
						Case(name, &d);
						Bmp_Free(&d);
						Bmp_Free(&s);
					}
}

int main(int argc, char** argv)
{
	gPrint = argc > 1 && strcmp(argv[1], "print") == 0;
	if(gPrint)
		printf("// This file is part of OpenBGI (https://openbgi.net).\n// SPDX-License-Identifier: GPL-2.0-only\n//\n"
			   "// what the blitters draw, as a hash for each case: written by `test_blitgold print` (tests/blitgold.c)\n");
	TestLevels();
	TestTurns();
	if(gPrint)
		return 0;
	CHECK(gCase == (int)(sizeof kGold / sizeof kGold[0]));
	if(gFails)
	{
		printf("blitgold: %d checks failed\n", gFails);
		return 1;
	}
	printf("blitgold: all tests passed (%d cases)\n", gCase);
	return 0;
}
