/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/blitrows.c - the fast rows of the blitters against the plain ones
 *                    (src/gfx/blit_rows.c); built as test_blitrows by
 *                    `make test`
 *
 * kBlitRows holds the build's SSE2 or NEON loops, kBlitRowsPlain the
 * word-lane ones, and both must give the same bytes.  Each case runs one
 * loop of each over the same random rows - every length from 0 to 70,
 * rows that start at any byte, runs of clear, solid and black pixels,
 * every level the fast loop takes - and compares the rows and the
 * pixels on both sides of them, which no loop may touch.
 *
 * `test_blitrows time` prints instead what a 1920 x 1080 pass of each
 * loop takes, plain and fast, in milliseconds (the best of seven).
 */
#include "bgi/gfx/bitmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"

enum
{
	MAXN = 70,
	EDGE = 5,    // pixels kept on each side of a row
	CASES = 40000 // for each loop
};

static uint32_t gSeed = 0x2545f491u;

static uint32_t Next(void)
{
	gSeed ^= gSeed << 13;
	gSeed ^= gSeed >> 17;
	gSeed ^= gSeed << 5;
	return gSeed;
}

// noise with what the loops branch on: runs of clear pixels (alpha 0 and 1), of solid ones, of black ones
static void Noise(uint32_t* p, int n)
{
	int x, run = 0;
	uint32_t kind = 0;
	for(x = 0; x < n; x++)
	{
		uint32_t v = Next();
		if(run-- <= 0)
		{
			kind = Next() % 8;
			run = (int)(Next() % 11);
		}
		switch(kind)
		{
			case 0: v &= 0x00ffffffu; break;
			case 1: v = (v & 0x00ffffffu) | 0x01000000u; break;
			case 2: v |= 0xff000000u; break;
			case 3: v = (v & 0x00ffffffu) | 0xfe000000u; break;
			case 4: v &= 0xff000000u; break;
			case 5: v = 0; break;
			default: break;
		}
		p[x] = v;
	}
}

/* rows that begin at any byte: the pixels are copied to and from a byte
 * buffer at offset 0 .. 15, so that no loop can count on alignment */
typedef struct Row
{
	uint8_t bytes[(MAXN + 2 * EDGE) * 4 + 16];
	uint32_t* p; // the first pixel of the row proper
} Row_t;

static void Row_Set(Row_t* r, const uint32_t* pixels, int offset)
{
	memcpy(r->bytes + offset, pixels, (MAXN + 2 * EDGE) * 4);
	r->p = (uint32_t*)(void*)(r->bytes + offset + EDGE * 4);
}

static uint32_t gA[MAXN + 2 * EDGE], gB[MAXN + 2 * EDGE], gD[MAXN + 2 * EDGE];
static Row_t gRowA, gRowB, gPlain, gFast;
static int gN;

static void Begin(void)
{
	int off = (int)(Next() % 16);
	Noise(gA, MAXN + 2 * EDGE);
	Noise(gB, MAXN + 2 * EDGE);
	Noise(gD, MAXN + 2 * EDGE);
	gN = (int)(Next() % (MAXN + 1));
	Row_Set(&gRowA, gA, (int)(Next() % 16));
	Row_Set(&gRowB, gB, (int)(Next() % 16));
	Row_Set(&gPlain, gD, off);
	Row_Set(&gFast, gD, off);
}

static int Same(void)
{
	return memcmp(gPlain.p - EDGE, gFast.p - EDGE, (MAXN + 2 * EDGE) * 4) == 0 // the rows, and what lies beside them
		&& memcmp(gPlain.p - EDGE, gD, EDGE * 4) == 0 && memcmp(gPlain.p + gN, gD + EDGE + gN, (size_t)(MAXN + EDGE - gN) * 4) == 0;
}

static void Report(const char* what, int differ)
{
	if(differ)
	{
		printf("  FAIL %s: %d of %d rows differ\n", what, differ, CASES);
		gFails++;
	}
}

static void TestRows(void)
{
	int i, differ;

	for(i = differ = 0; i < CASES; i++)
	{
		Begin();
		kBlitRowsPlain.alphaCopy(gPlain.p, gRowA.p, gN);
		kBlitRows.alphaCopy(gFast.p, gRowA.p, gN);
		differ += !Same();
	}
	Report("alpha copy", differ);

	for(i = differ = 0; i < CASES; i++)
	{
		int inv = (int)(Next() % 0x101);
		Begin();
		kBlitRowsPlain.blendAlpha(gPlain.p, gRowA.p, gN, inv);
		kBlitRows.blendAlpha(gFast.p, gRowA.p, gN, inv);
		differ += !Same();
	}
	Report("blend with alpha", differ);

	for(i = differ = 0; i < CASES; i++)
	{
		int l2 = (int)(Next() % 129);
		Begin();
		kBlitRowsPlain.blendPlain(gPlain.p, gRowA.p, gN, l2);
		kBlitRows.blendPlain(gFast.p, gRowA.p, gN, l2);
		differ += !Same();
	}
	Report("blend without alpha", differ);

	for(i = differ = 0; i < CASES; i++)
	{
		int ratio = (int)(Next() % 0x101), inv = (int)(Next() % 0x101);
		if(i % 16 == 0)
			ratio = i % 32 ? 0 : 0x100; // the ends
		if(i % 24 == 0)
			inv = i % 48 ? 0 : 0x100;
		Begin();
		kBlitRowsPlain.crossfade(gPlain.p, gRowA.p, gRowB.p, gN, ratio, inv);
		kBlitRows.crossfade(gFast.p, gRowA.p, gRowB.p, gN, ratio, inv);
		differ += !Same();
	}
	Report("cross fade", differ);

	for(i = differ = 0; i < CASES; i++)
	{
		int level = (int)(Next() % 0x180), withAlpha = (int)(Next() & 1);
		Begin();
		kBlitRowsPlain.addAlpha(gPlain.p, gRowA.p, gN, level, withAlpha);
		kBlitRows.addAlpha(gFast.p, gRowA.p, gN, level, withAlpha);
		differ += !Same();
	}
	Report("add with alpha", differ);

	for(i = differ = 0; i < CASES; i++)
	{
		int level = (int)(Next() % 0x101);
		uint32_t top = Next() & 1 ? (uint32_t)(level > 0xff ? 0xff : level) << 24 : 0;
		Begin();
		kBlitRowsPlain.addPlain(gPlain.p, gRowA.p, gN, level, top);
		kBlitRows.addPlain(gFast.p, gRowA.p, gN, level, top);
		differ += !Same();
	}
	Report("add without alpha", differ);
}

// the test must see a fault: another level gives other rows
static void TestSees(void)
{
	int i, differ = 0;
	for(i = 0; i < 2000; i++)
	{
		Begin();
		kBlitRowsPlain.blendAlpha(gPlain.p, gRowA.p, gN, 0x80);
		kBlitRows.blendAlpha(gFast.p, gRowA.p, gN, 0x60);
		differ += !Same();
	}
	CHECK(differ > 1000);
}

// ---- `time` ------------------------------------------------------------------------------------

enum
{
	TW = 1920,
	TH = 1080
};

static uint32_t *gTa, *gTb, *gTd, *gTsave;
static const BlitRows_t* gRows;

static double Now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static void Pass(int which)
{
	int y;
	for(y = 0; y < TH; y++)
	{
		uint32_t* d = gTd + (size_t)y * TW;
		const uint32_t* a = gTa + (size_t)y * TW;
		const uint32_t* b = gTb + (size_t)y * TW;
		switch(which)
		{
			case 0: gRows->alphaCopy(d, a, TW); break;
			case 1: gRows->blendAlpha(d, a, TW, 0x80); break;
			case 2: gRows->blendPlain(d, a, TW, 0x40); break;
			case 3: gRows->crossfade(d, a, b, TW, 0x60, 0x100); break;
			case 4: gRows->addAlpha(d, a, TW, 0x100, 0); break;
			default: gRows->addPlain(d, a, TW, 0x80, 0); break;
		}
	}
}

static double Best(int which)
{
	double best = 1e9;
	int i;
	for(i = 0; i < 7; i++)
	{
		double t;
		memcpy(gTd, gTsave, (size_t)TW * TH * 4);
		t = Now();
		Pass(which);
		t = Now() - t;
		if(t < best)
			best = t;
	}
	return best;
}

static void Time(void)
{
	static const char* const kNames[6] = {"alpha copy", "blend with alpha", "blend without alpha", "cross fade", "add with alpha", "add without alpha"};
	size_t count = (size_t)TW * TH, i;
	int which;
	gTa = malloc(count * 4);
	gTb = malloc(count * 4);
	gTd = malloc(count * 4);
	gTsave = malloc(count * 4);
	if(!gTa || !gTb || !gTd || !gTsave)
		return;
	// a sprite's kind of picture: long runs of clear and of solid pixels, soft ones between
	for(i = 0; i < count;)
	{
		uint32_t kind = Next() % 10, run = Next() % 200 + 1;
		for(; run && i < count; run--, i++)
		{
			uint32_t v = Next(), w = Next();
			gTa[i] = kind < 4 ? v & 0x00ffffffu : kind < 9 ? v | 0xff000000u : v;
			gTb[i] = kind < 4 ? w & 0x00ffffffu : kind < 9 ? w | 0xff000000u : w;
			gTsave[i] = Next() | 0xff000000u;
		}
	}
	printf("one %d x %d pass, milliseconds:\n%-22s %8s %8s\n", TW, TH, "", "plain", kBlitRows.name);
	for(which = 0; which < 6; which++)
	{
		double plain, fast;
		gRows = &kBlitRowsPlain;
		plain = Best(which);
		gRows = &kBlitRows;
		fast = Best(which);
		printf("%-22s %8.2f %8.2f\n", kNames[which], plain, fast);
	}
}

int main(int argc, char** argv)
{
	if(argc > 1 && strcmp(argv[1], "time") == 0)
	{
		Time();
		return 0;
	}
	TestRows();
	TestSees();
	if(gFails)
	{
		printf("blitrows: %d checks failed\n", gFails);
		return 1;
	}
	printf("blitrows: all tests passed (%d rows a loop, fast rows: %s)\n", CASES, kBlitRows.name);
	return 0;
}
