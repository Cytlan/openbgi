/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/pattern.c - unit test of the gray patterns of "EF 30"
 *                   (src/gfx/pattern.c, declared in src/include/bgi/gfx/pattern.h);
 *                   built as test_pattern by `make test`
 *
 * The groups of tests, in the order they run:
 *
 *   ramps     - a linear ramp runs from v0 to v1 in its direction, a
 *               repeat runs it again, a mirror brings it back; rings are
 *               v0 in the middle and v1 from the ellipse on; a sweep goes
 *               once round clockwise from its angle
 *   modes     - how a pattern meets what the bitmap holds
 *   noise     - the same seed gives the same picture, another seed another;
 *               the values stay between v0 and v1; polar noise comes round
 *   tiles     - every tile takes one value, that of its centre; columns
 *               cut only across; the chance amount stays within its bounds
 *               and is the same for the same seed
 *   shapes    - a polygon covers its inside fully and its edge in part; a
 *               line is as wide as it says; triangles side by side leave
 *               no seam and no pixel drawn twice
 *   refused   - a bitmap that is not gray, an unknown kind or mode, too
 *               few values: nothing is drawn
 */
#include "bgi/gfx/pattern.h"
#include "bgi/gfx/bitmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

#define W 64
#define H 48

static Bmp_t gBmp; // the gray bitmap every test draws into

static int At(int x, int y)
{
	return gBmp.pixels[(size_t)y * (size_t)gBmp.pitch + (size_t)x];
}

static void Fill(int v)
{
	memset(gBmp.pixels, v, (size_t)gBmp.pitch * (size_t)gBmp.h);
}

static int Draw(int kind, int mode, const int32_t* p, int count)
{
	return Pattern_Generate(&gBmp, kind, mode, p, count);
}

static void TestRamps(void)
{
	int x, y, ok;
	{ // left to right, 10 .. 250
		const int32_t p[] = {0, 10, 250, 1, 0};
		Fill(0);
		CHECK(Draw(PAT_LINEAR, PAT_SET, p, 5));
		CHECK(At(0, 0) >= 10 && At(0, 0) <= 13 && At(W - 1, 0) >= 247 && At(W - 1, 0) <= 250);
		for(ok = 1, y = 0; y < H; y++)
			for(x = 1; x < W; x++)
				ok &= At(x, y) >= At(x - 1, y) && At(x, y) == At(x, 0);
		CHECK(ok); // it rises across and is the same in every row
	}
	{ // top to bottom, falling
		const int32_t p[] = {90 * 65536, 200, 0, 1, 0};
		CHECK(Draw(PAT_LINEAR, PAT_SET, p, 5));
		CHECK(At(5, 0) > 195 && At(5, H - 1) < 5 && At(5, H / 2) > 90 && At(5, H / 2) < 110 && At(60, H / 2) == At(5, H / 2));
	}
	{ // twice, and back again within each run: 0 at both ends and in the middle
		const int32_t p[] = {0, 0, 255, 2, PAT_MIRROR};
		CHECK(Draw(PAT_LINEAR, PAT_SET, p, 5));
		CHECK(At(0, 0) < 20 && At(W / 2, 0) < 20 && At(W - 1, 0) < 20 && At(W / 4, 0) > 235 && At(3 * W / 4, 0) > 235);
	}
	{ // a diagonal: the corners are the two ends
		const int32_t p[] = {45 * 65536, 0, 255, 1, PAT_SMOOTH};
		CHECK(Draw(PAT_LINEAR, PAT_SET, p, 5));
		CHECK(At(0, 0) < 3 && At(W - 1, H - 1) > 252 && At(W - 1, 0) > 60 && At(W - 1, 0) < 200);
	}
	{ // rings
		const int32_t p[] = {32, 24, 20, 10, 255, 0, 1, 0};
		CHECK(Draw(PAT_RADIAL, PAT_SET, p, 8));
		CHECK(At(32, 24) > 235 && At(32 + 21, 24) == 0 && At(32, 24 + 11) == 0 && At(0, 0) == 0);
		CHECK(At(32 + 10, 24) > 100 && At(32 + 10, 24) < 150 && At(32, 24 + 5) > 100 && At(32, 24 + 5) < 150);
		CHECK(At(32 - 10, 24) == At(32 + 9, 24)); // the same on both sides
	}
	{ // a clock hand from 3 o'clock, clockwise: little just below it, most just above it
		const int32_t p[] = {32, 24, 0, 0, 255, 1, 0};
		CHECK(Draw(PAT_SWEEP, PAT_SET, p, 7));
		CHECK(At(60, 25) < 10 && At(60, 22) > 245 && At(32, 44) > 55 && At(32, 44) < 72 && At(4, 24) > 120 && At(4, 24) < 135 && At(32, 4) > 185 && At(32, 4) < 200);
	}
	{ // dithered: only the two nearest levels
		const int32_t p[] = {0, 100, 101, 1, PAT_DITHER};
		CHECK(Draw(PAT_LINEAR, PAT_SET, p, 5));
		for(ok = 1, y = 0; y < H; y++)
			for(x = 0; x < W; x++)
				ok &= At(x, y) == 100 || At(x, y) == 101;
		CHECK(ok && At(W - 1, 0) + At(W - 1, 1) + At(W - 2, 0) + At(W - 2, 1) > 400);
	}
}

static void TestModes(void)
{
	static const int32_t p[] = {0, 100, 100, 1, 0}; // 100 everywhere
	static const struct
	{
		int mode, was, now;
	} cases[] = {{PAT_SET, 30, 100}, {PAT_LIGHTEN, 30, 100}, {PAT_LIGHTEN, 200, 200}, {PAT_DARKEN, 30, 30}, {PAT_DARKEN, 200, 100},
		{PAT_ADD, 30, 130}, {PAT_ADD, 200, 255}, {PAT_SUBTRACT, 30, 0}, {PAT_SUBTRACT, 200, 100}, {PAT_MULTIPLY, 255, 100}, {PAT_MULTIPLY, 51, 20}};
	size_t i;
	for(i = 0; i < sizeof cases / sizeof cases[0]; i++)
	{
		Fill(cases[i].was);
		CHECK(Draw(PAT_LINEAR, cases[i].mode, p, 5));
		CHECK(At(10, 10) == cases[i].now && At(W - 1, H - 1) == cases[i].now);
	}
}

static void TestNoise(void)
{
	static uint8_t first[W * H];
	const int32_t a[] = {7, 8, 8, 40, 200, 3, PAT_SMOOTH};
	const int32_t b[] = {8, 8, 8, 40, 200, 3, PAT_SMOOTH};
	const int32_t polar[] = {7, 30, 6, 0, 255, 1, PAT_POLAR};
	int x, y, lo = 255, hi = 0, same;
	Fill(0);
	CHECK(Draw(PAT_NOISE, PAT_SET, a, 7));
	memcpy(first, gBmp.pixels, sizeof first);
	for(y = 0; y < H; y++)
		for(x = 0; x < W; x++)
		{
			lo = At(x, y) < lo ? At(x, y) : lo;
			hi = At(x, y) > hi ? At(x, y) : hi;
		}
	CHECK(lo >= 40 && hi <= 200 && hi - lo > 50); // within its bounds, and not flat
	Fill(0);
	CHECK(Draw(PAT_NOISE, PAT_SET, a, 7) && memcmp(first, gBmp.pixels, sizeof first) == 0);
	CHECK(Draw(PAT_NOISE, PAT_SET, b, 7) && memcmp(first, gBmp.pixels, sizeof first) != 0);
	// around the centre the noise meets itself: just above and just below the line to the left differ little
	CHECK(Draw(PAT_NOISE, PAT_SET, polar, 7));
	for(same = 1, x = 2; x < 20; x++)
		same &= abs(At(x, 23) - At(x, 24)) < 60;
	CHECK(same);
}

static void TestTiles(void)
{
	static uint8_t first[W * H];
	int x, y, ok;
	{ // 16 x 16 squares over a ramp: one value to a square, rising from square to square
		const int32_t ramp[] = {0, 0, 255, 1, 0};
		const int32_t tiles[] = {PAT_TILE_RECT, 16, 16, 0, 0, 0, 1, 0};
		Draw(PAT_LINEAR, PAT_SET, ramp, 5);
		CHECK(Draw(PAT_TILES, PAT_SET, tiles, 8));
		for(ok = 1, y = 0; y < H; y++)
			for(x = 0; x < W; x++)
				ok &= At(x, y) == At(x / 16 * 16, 0);
		CHECK(ok && At(0, 0) < At(16, 0) && At(16, 0) < At(32, 0) && At(32, 0) < At(48, 0));
		CHECK(abs(At(0, 0) - 32) < 6 && abs(At(48, 0) - 223) < 6); // the value at the square's centre
	}
	{ // columns over a ramp that runs down: every column keeps the ramp
		const int32_t ramp[] = {90 * 65536, 0, 255, 1, 0};
		const int32_t tiles[] = {PAT_TILE_COLUMNS, 8, 0, 0, 0, 0, 1, 0};
		Draw(PAT_LINEAR, PAT_SET, ramp, 5);
		memcpy(first, gBmp.pixels, sizeof first);
		CHECK(Draw(PAT_TILES, PAT_SET, tiles, 8) && memcmp(first, gBmp.pixels, sizeof first) == 0);
	}
	{ // a chance amount: within its bounds, the same for the same seed
		const int32_t tiles[] = {PAT_TILE_CELLS, 12, 12, 30, 0, 0, 5, 0};
		const int32_t other[] = {PAT_TILE_CELLS, 12, 12, 30, 0, 0, 6, 0};
		int lo = 255, hi = 0;
		Fill(100);
		CHECK(Draw(PAT_TILES, PAT_SET, tiles, 8));
		memcpy(first, gBmp.pixels, sizeof first);
		for(y = 0; y < H; y++)
			for(x = 0; x < W; x++)
			{
				lo = At(x, y) < lo ? At(x, y) : lo;
				hi = At(x, y) > hi ? At(x, y) : hi;
			}
		CHECK(lo >= 70 && hi <= 130 && hi - lo > 20);
		Fill(100);
		CHECK(Draw(PAT_TILES, PAT_SET, tiles, 8) && memcmp(first, gBmp.pixels, sizeof first) == 0);
		Fill(100);
		CHECK(Draw(PAT_TILES, PAT_SET, other, 8) && memcmp(first, gBmp.pixels, sizeof first) != 0);
	}
	{ // a ramp of its own across every tile: 40 levels from its left edge to its right
		const int32_t tiles[] = {PAT_TILE_RECT, 32, 24, 0, 40, 0, 1, 0};
		Fill(100);
		CHECK(Draw(PAT_TILES, PAT_SET, tiles, 8));
		CHECK(abs(At(0, 5) - 80) <= 2 && abs(At(31, 5) - 120) <= 2 && abs(At(32, 5) - 80) <= 2 && At(16, 5) == At(16, 20));
	}
	{ // every shape draws
		int shape;
		for(shape = PAT_TILE_RECT; shape <= PAT_TILE_CELLS; shape++)
		{
			const int32_t ramp[] = {30 * 65536, 0, 255, 1, 0};
			const int32_t tiles[] = {shape, 10, 10, 10, 20, 0, 3, PAT_TURNED};
			Draw(PAT_LINEAR, PAT_SET, ramp, 5);
			CHECK(Draw(PAT_TILES, PAT_SET, tiles, 8));
		}
	}
}

static void TestShapes(void)
{
	int x, y, sum;
	{ // a rectangle from (8.5, 8) to (24, 16): inside in full, its left edge by half, nothing outside
		const int32_t p[] = {200, 8 * 256 + 128, 8 * 256, 24 * 256, 8 * 256, 24 * 256, 16 * 256, 8 * 256 + 128, 16 * 256};
		Fill(0);
		CHECK(Draw(PAT_POLYGON, PAT_SET, p, 9));
		CHECK(At(12, 10) == 200 && At(23, 15) == 200 && At(24, 10) == 0 && At(12, 16) == 0 && At(12, 7) == 0 && At(7, 10) == 0);
		CHECK(abs(At(8, 10) - 100) <= 2);
	}
	{ // a figure that goes round twice stays filled
		const int32_t p[] = {255, 10 * 256, 10 * 256, 30 * 256, 10 * 256, 30 * 256, 30 * 256, 10 * 256, 30 * 256, 10 * 256, 10 * 256, 30 * 256, 10 * 256, 30 * 256,
			30 * 256, 10 * 256, 30 * 256};
		Fill(0);
		CHECK(Draw(PAT_POLYGON, PAT_SET, p, 17));
		CHECK(At(20, 20) == 255 && At(11, 11) == 255 && At(5, 5) == 0);
	}
	{ // a level line, 4 pixels wide, from (10, 20) to (40, 20): full across its middle, round ends
		const int32_t p[] = {255, 4 * 256, 0, 10 * 256, 20 * 256, 40 * 256, 20 * 256};
		Fill(0);
		CHECK(Draw(PAT_LINE, PAT_SET, p, 7));
		CHECK(At(25, 18) == 255 && At(25, 21) == 255 && At(25, 16) == 0 && At(25, 23) == 0 && At(8, 19) > 0 && At(6, 20) == 0 && At(43, 20) == 0);
	}
	{ // a closed line: the last point is joined to the first
		const int32_t open[] = {255, 2 * 256, 0, 10 * 256, 10 * 256, 40 * 256, 10 * 256, 40 * 256, 30 * 256};
		const int32_t closed[] = {255, 2 * 256, 1, 10 * 256, 10 * 256, 40 * 256, 10 * 256, 40 * 256, 30 * 256};
		Fill(0);
		CHECK(Draw(PAT_LINE, PAT_SET, open, 9) && At(25, 20) == 0);
		CHECK(Draw(PAT_LINE, PAT_SET, closed, 9) && At(25, 20) > 100);
	}
	{ // two triangles that make a rectangle, added: every pixel of it once, none beside it
		const int32_t p[] = {8 * 256, 8 * 256, 1, 40 * 256, 8 * 256, 1, 40 * 256, 32 * 256, 1, 8 * 256, 8 * 256, 1, 40 * 256, 32 * 256, 1, 8 * 256, 32 * 256, 1};
		Fill(0);
		CHECK(Draw(PAT_TRIANGLES, PAT_ADD, p, 18));
		for(sum = 0, y = 0; y < H; y++)
			for(x = 0; x < W; x++)
			{
				CHECK(At(x, y) == (x >= 8 && x < 40 && y >= 8 && y < 32));
				sum += At(x, y);
			}
		CHECK(sum == 32 * 24);
	}
	{ // the value runs between those of the corners
		const int32_t p[] = {0, 0, 0, 64 * 256, 0, 255, 0, 48 * 256, 0};
		Fill(7);
		CHECK(Draw(PAT_TRIANGLES, PAT_SET, p, 9));
		CHECK(At(0, 0) < 5 && At(60, 0) > 230 && abs(At(30, 0) - 121) < 6 && At(1, 40) < 8 && At(63, 47) == 7);
	}
}

static void TestRefused(void)
{
	static const int32_t ramp[] = {0, 0, 255, 1, 0};
	static const int32_t poly[] = {255, 0, 0, 256, 0, 256, 256};
	static const int32_t radial[] = {1, 1, 0, 5, 0, 255, 1, 0};
	static const int32_t tiles[] = {PAT_TILE_RECT, 0, 8, 0, 0, 0, 1, 0};
	Bmp_t colour;
	Fill(9);
	CHECK(!Draw(PAT_LINEAR, PAT_SET, ramp, 4)); // too few values
	CHECK(!Draw(PAT_LINEAR, 6, ramp, 5));       // no such mode
	CHECK(!Draw(PAT_LINEAR, -1, ramp, 5));
	CHECK(!Draw(8, PAT_SET, ramp, 5)); // no such kind
	CHECK(!Draw(-1, PAT_SET, ramp, 5));
	CHECK(!Draw(PAT_LINEAR, PAT_SET, NULL, 5));
	CHECK(!Draw(PAT_POLYGON, PAT_SET, poly, 5));                    // two points are no polygon
	CHECK(!Draw(PAT_RADIAL, PAT_SET, radial, 8));                   // no radius
	CHECK(!Draw(PAT_TILES, PAT_SET, tiles, 8));                     // no width
	CHECK(!Draw(PAT_TRIANGLES, PAT_SET, ramp, 5));                  // not a whole triangle
	CHECK(At(0, 0) == 9 && At(W - 1, H - 1) == 9 && At(1, 1) == 9); // nothing was drawn
	CHECK(Draw(PAT_POLYGON, PAT_SET, poly, 7));
	memset(&colour, 0, sizeof colour);
	CHECK(Bmp_Alloc(&colour, 8, 8, PM_ARGB32));
	CHECK(!Pattern_Generate(&colour, PAT_LINEAR, PAT_SET, ramp, 5)); // not a gray bitmap
	CHECK(!Pattern_Generate(NULL, PAT_LINEAR, PAT_SET, ramp, 5));
	Bmp_Free(&colour);
}

int main(void)
{
	memset(&gBmp, 0, sizeof gBmp);
	if(!Bmp_Alloc(&gBmp, W, H, PM_GRAY8))
		return 1;
	printf("ramps\n");
	TestRamps();
	printf("modes\n");
	TestModes();
	printf("noise\n");
	TestNoise();
	printf("tiles\n");
	TestTiles();
	printf("shapes\n");
	TestShapes();
	printf("refused\n");
	TestRefused();
	Bmp_Free(&gBmp);
	if(gFails)
	{
		printf("%d check(s) failed\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
