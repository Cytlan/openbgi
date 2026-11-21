/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/screen.c - unit test of the window's picture (src/sys/screen.c,
 *                  bgi/display.h) and what a frame reports of it
 *                  (OpenBGI_Frame_t); built as test_screen by `make test`
 *
 * The groups of tests, in the order they run:
 *
 *   setup   - a new picture is black, padded to 4 bytes a row and all of
 *             it changed; the next frame reports nothing; a dropped one
 *             has no pixels
 *   copy    - BitBlt: clipped to the picture, black where the source ends,
 *             the 16- and 32-bit modes converted both ways, the change
 *             reported as the part that was drawn
 *   changes - a change inside a reported one adds nothing, more changes
 *             than fit are merged and still cover every changed pixel
 *   stretch - nearest-neighbour without a clip, bilinear and clipped with
 *             one, a plain copy at the same size
 */
#include "bgi/common.h"
#include "bgi/core.h"
#include "host/os.h"
#include "bgi/display.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

// pixel (x, y) of a frame's 32-bit picture
static uint32_t Px32(const OpenBGI_Frame_t* f, int x, int y)
{
	return ((const uint32_t*)(f->pixels + (size_t)y * (size_t)f->pitch))[x] & 0xffffff;
}

static uint16_t Px16(const OpenBGI_Frame_t* f, int x, int y)
{
	return ((const uint16_t*)(f->pixels + (size_t)y * (size_t)f->pitch))[x];
}

// a bitmap view of `px` (w x h, packed rows)
static Bmp_t View(void* px, int w, int h, int mode)
{
	Bmp_t b;
	b.pixels = (uint8_t*)px;
	b.w = w;
	b.h = h;
	b.mode = mode;
	b.bpp = ModeBytes(mode);
	b.pitch = w * b.bpp;
	return b;
}

// 1 when (x, y) lies in one of the frame's changed rectangles
static int Covered(const OpenBGI_Frame_t* f, int x, int y)
{
	int i;
	if(f->changes < 0)
		return 1;
	for(i = 0; i < f->changes; i++)
	{
		const OpenBGI_Rect_t* r = &f->change[i];
		if(x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
			return 1;
	}
	return 0;
}

static void TestSetup(OpenBGI_t* o)
{
	OpenBGI_Frame_t f = {.size = sizeof f};
	int x, y, black = 1;
	Screen_Setup(o, 10, 6, PM_RGB32);
	Screen_TakeFrame(o, &f);
	CHECK(f.pixels != NULL && f.width == 10 && f.height == 6 && f.pitch == 40 && f.bpp == 32);
	CHECK(f.changes == -1);
	for(y = 0; y < 6; y++)
		for(x = 0; x < 10; x++)
			black &= Px32(&f, x, y) == 0;
	CHECK(black);
	Screen_TakeFrame(o, &f);
	CHECK(f.changes == 0);
	Screen_Setup(o, 5, 3, PM_RGB16);
	Screen_TakeFrame(o, &f);
	CHECK(f.pitch == 12 && f.bpp == 16 && f.changes == -1); // 10 bytes of pixels, padded
	Screen_Free(o);
	Screen_TakeFrame(o, &f);
	CHECK(f.pixels == NULL && f.changes == 0);
}

static void TestCopy(OpenBGI_t* o)
{
	OpenBGI_Frame_t f = {.size = sizeof f};
	uint32_t src[3 * 2] = {0x112233, 0x445566, 0x778899, 0xaabbcc, 0xddeeff, 0x102030};
	uint16_t src16[2] = {0x7fff, (uint16_t)(1 << 10 | 16 << 5 | 31)};
	Bmp_t b = View(src, 3, 2, PM_RGB32), b16 = View(src16, 2, 1, PM_RGB16);

	Screen_Setup(o, 8, 4, PM_RGB32);
	Screen_TakeFrame(o, &f);
	// clipped at the left: the first column of the source is cut off
	Screen_Copy(o, -1, 1, &b, 0, 0, 3, 2);
	Screen_TakeFrame(o, &f);
	CHECK(f.changes == 1 && f.change[0].x == 0 && f.change[0].y == 1 && f.change[0].w == 2 && f.change[0].h == 2);
	CHECK(Px32(&f, 0, 1) == 0x445566 && Px32(&f, 1, 1) == 0x778899 && Px32(&f, 1, 2) == 0x102030);
	CHECK(Px32(&f, 2, 1) == 0);
	// clipped at the bottom right; beyond the source's end is black
	Screen_Copy(o, 6, 3, &b, 1, 1, 4, 4);
	Screen_TakeFrame(o, &f);
	CHECK(f.changes == 1 && f.change[0].x == 6 && f.change[0].y == 3 && f.change[0].w == 2 && f.change[0].h == 1);
	CHECK(Px32(&f, 6, 3) == 0xddeeff && Px32(&f, 7, 3) == 0x102030);
	Screen_Copy(o, 5, 0, &b, 2, 0, 2, 1); // the second pixel lies past the source
	Screen_TakeFrame(o, &f);
	CHECK(Px32(&f, 5, 0) == 0x778899 && Px32(&f, 6, 0) == 0);
	// nothing of it on the picture: no change
	Screen_Copy(o, 8, 0, &b, 0, 0, 3, 2);
	Screen_Copy(o, 0, -2, &b, 0, 0, 3, 2);
	Screen_TakeFrame(o, &f);
	CHECK(f.changes == 0);
	// 16 bits into 32: each channel widened by repeating its top bits
	Screen_Copy(o, 0, 0, &b16, 0, 0, 2, 1);
	Screen_TakeFrame(o, &f);
	CHECK(Px32(&f, 0, 0) == 0xffffff && Px32(&f, 1, 0) == 0x0884ff);

	// 32 bits into 16: the top five bits of each channel
	Screen_Setup(o, 4, 2, PM_RGB16);
	Screen_Copy(o, 0, 0, &b, 0, 0, 3, 2);
	Screen_TakeFrame(o, &f);
	CHECK(f.changes == -1); // still the new picture's
	CHECK(Px16(&f, 0, 0) == (uint16_t)((0x11 >> 3) << 10 | (0x22 >> 3) << 5 | (0x33 >> 3)));
	CHECK(Px16(&f, 2, 1) == (uint16_t)((0x10 >> 3) << 10 | (0x20 >> 3) << 5 | (0x30 >> 3)));
	CHECK(Px16(&f, 3, 0) == 0);
	// 16 into 16: as it is
	Screen_Copy(o, 2, 0, &b16, 0, 0, 2, 1);
	Screen_TakeFrame(o, &f);
	CHECK(Px16(&f, 2, 0) == 0x7fff && Px16(&f, 3, 0) == src16[1]);
}

static void TestChanges(OpenBGI_t* o)
{
	OpenBGI_Frame_t f = {.size = sizeof f};
	uint32_t one = 0x123456;
	Bmp_t b = View(&one, 1, 1, PM_RGB32), big;
	static uint32_t block[4 * 4];
	int i, all = 1;

	Screen_Setup(o, 64, 64, PM_RGB32);
	Screen_TakeFrame(o, &f);
	big = View(block, 4, 4, PM_RGB32);
	Screen_Copy(o, 10, 10, &big, 0, 0, 4, 4);
	Screen_Copy(o, 11, 12, &b, 0, 0, 1, 1); // inside the first: nothing new
	Screen_TakeFrame(o, &f);
	CHECK(f.changes == 1 && f.change[0].x == 10 && f.change[0].w == 4);
	// more single pixels than the list holds: merged, every one still covered
	for(i = 0; i < 50; i++)
		Screen_Copy(o, (i * 7) % 64, (i * 13) % 64, &b, 0, 0, 1, 1);
	Screen_TakeFrame(o, &f);
	CHECK(f.changes == OPENBGI_FRAME_CHANGES);
	for(i = 0; i < 50; i++)
		all &= Covered(&f, (i * 7) % 64, (i * 13) % 64) && Px32(&f, (i * 7) % 64, (i * 13) % 64) == 0x123456;
	CHECK(all);
	for(i = 0; i < f.changes; i++)
		all &= f.change[i].x >= 0 && f.change[i].y >= 0 && f.change[i].x + f.change[i].w <= 64 && f.change[i].y + f.change[i].h <= 64;
	CHECK(all);
}

static void TestStretch(OpenBGI_t* o)
{
	OpenBGI_Frame_t f = {.size = sizeof f};
	uint32_t src[2 * 2] = {0x000000, 0xff0000, 0x00ff00, 0x0000ff};
	uint32_t row[2] = {0x000000, 0xfefefe};
	Bmp_t b = View(src, 2, 2, PM_RGB32), r = View(row, 2, 1, PM_RGB32);
	int clip[4] = {2, 0, 6, 1};
	uint32_t mid;

	Screen_Setup(o, 8, 8, PM_RGB32);
	Screen_TakeFrame(o, &f);
	// nearest-neighbour, twice the size
	Screen_Stretch(o, 1, 1, 4, 4, &b, NULL);
	Screen_TakeFrame(o, &f);
	CHECK(f.changes == 1 && f.change[0].x == 1 && f.change[0].y == 1 && f.change[0].w == 4 && f.change[0].h == 4);
	CHECK(Px32(&f, 1, 1) == 0 && Px32(&f, 2, 2) == 0 && Px32(&f, 3, 1) == 0xff0000 && Px32(&f, 4, 2) == 0xff0000);
	CHECK(Px32(&f, 1, 3) == 0x00ff00 && Px32(&f, 4, 4) == 0x0000ff && Px32(&f, 5, 5) == 0);
	// bilinear and clipped: the middle of a two-pixel row stretched to eight is grey
	Screen_Stretch(o, 0, 0, 8, 1, &r, clip);
	Screen_TakeFrame(o, &f);
	CHECK(f.changes == 1 && f.change[0].x == 2 && f.change[0].w == 4 && f.change[0].h == 1);
	mid = Px32(&f, 4, 0);
	CHECK((mid & 0xff) > 0x40 && (mid & 0xff) < 0xc0 && (mid >> 8 & 0xff) == (mid & 0xff));
	CHECK(Px32(&f, 2, 0) < Px32(&f, 5, 0));            // rising from left to right
	CHECK(Px32(&f, 0, 0) == 0 && Px32(&f, 7, 0) == 0); // outside the clip: untouched
	// the same size and no clip: a plain copy
	Screen_Stretch(o, 6, 6, 2, 2, &b, NULL);
	Screen_TakeFrame(o, &f);
	CHECK(Px32(&f, 7, 6) == 0xff0000 && Px32(&f, 6, 7) == 0x00ff00);
}

/* The pixels a frame handed over are the host's to read until the next
 * frame, whatever becomes of the picture in between (an event that
 * changes the display mode makes a new one). */
static void TestHanded(OpenBGI_t* o)
{
	OpenBGI_Frame_t f = {.size = sizeof f}, g = {.size = sizeof g};
	Bmp_t src;
	uint32_t red[4] = {0xff0000, 0xff0000, 0xff0000, 0xff0000};
	memset(&src, 0, sizeof src);
	src.pixels = (uint8_t*)red;
	src.pitch = 8;
	src.w = src.h = 2;
	src.mode = PM_RGB32;
	src.bpp = 4;
	Screen_NextFrame(o);
	Screen_Setup(o, 8, 4, PM_RGB32);
	Screen_Copy(o, 0, 0, &src, 0, 0, 2, 2);
	Screen_TakeFrame(o, &f);
	CHECK(f.pixels != NULL && Px32(&f, 1, 1) == 0xff0000);
	Screen_Setup(o, 16, 8, PM_RGB32);                         // between two frames: another picture
	CHECK(Px32(&f, 1, 1) == 0xff0000 && Px32(&f, 7, 3) == 0); // the one handed over is still there
	Screen_Setup(o, 4, 4, PM_RGB32);                          // .. and after another one
	CHECK(Px32(&f, 1, 1) == 0xff0000);
	Screen_Free(o);
	CHECK(Px32(&f, 1, 1) == 0xff0000);
	Screen_NextFrame(o); // the next frame begins: gone
	Screen_Setup(o, 4, 4, PM_RGB32);
	Screen_TakeFrame(o, &g);
	CHECK(g.pixels != NULL && g.width == 4 && g.changes == -1);
	Screen_NextFrame(o);
	Screen_TakeFrame(o, &g);
	Screen_Free(o); // dropped while handed over, and the engine goes without another frame
	CHECK(Px32(&g, 0, 0) == 0);
	Screen_NextFrame(o);
}

int main(void)
{
	OpenBGI_t* o = OpenBGI_New(OS_Host());
	if(!o)
	{
		printf("screen: no engine\n");
		return 1;
	}
	printf("setup\n");
	TestSetup(o);
	printf("copy\n");
	TestCopy(o);
	printf("changes\n");
	TestChanges(o);
	printf("stretch\n");
	TestStretch(o);
	printf("handed\n");
	TestHanded(o);
	OpenBGI_Delete(o);
	if(gFails)
	{
		printf("screen: %d checks failed\n", gFails);
		return 1;
	}
	printf("screen: all tests passed\n");
	return 0;
}
