/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/present.c - the picture on a canvas of another size
 *                   (src/host/os_present.c)
 *
 *   size    - the size a picture is drawn at: the largest power of two,
 *             the largest whole number, the fill that keeps the
 *             proportions, none; a picture larger than the canvas is
 *             halved (or cut, with none)
 *   plain   - a picture at its own size: in the middle, black around it,
 *             a second drawing without a change reported as none; a place
 *             on the canvas as a place in the picture
 *   changes - only the rows said to have changed are taken again, and the
 *             result of a drawing in steps is that of one drawing
 *   scaled  - twice the size: the nearest pixel doubled; the weighted
 *             filters keep a flat colour flat, stay within the pixel's
 *             range at an edge and agree with a straightforward
 *             calculation; half the size averages a pattern of single
 *             pixels away
 *   forms   - a 5-5-5 picture; no picture (black, once); another set-up
 *             or another picture's size redraws everything; a picture cut
 *             by the canvas
 */
#include "host/os_present.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

static int SizeIs(int w, int h, int cw, int ch, int steps, int ew, int eh)
{
	int ow = -1, oh = -1;
	OsPresent_Size(w, h, cw, ch, steps, &ow, &oh);
	return ow == ew && oh == eh;
}

static void TestSize(void)
{
	printf("size\n");
	CHECK(SizeIs(1280, 720, 1920, 1080, OS_PRESENT_POW2, 1280, 720));
	CHECK(SizeIs(1280, 720, 2560, 1440, OS_PRESENT_POW2, 2560, 1440));
	CHECK(SizeIs(1280, 720, 3840, 2160, OS_PRESENT_POW2, 2560, 1440)); // three times is no power of two
	CHECK(SizeIs(1280, 720, 3840, 2160, OS_PRESENT_WHOLE, 3840, 2160));
	CHECK(SizeIs(800, 600, 1920, 1080, OS_PRESENT_POW2, 800, 600)); // twice is too high
	CHECK(SizeIs(800, 600, 1920, 1200, OS_PRESENT_POW2, 1600, 1200));
	CHECK(SizeIs(640, 480, 3840, 2160, OS_PRESENT_POW2, 2560, 1920));
	CHECK(SizeIs(640, 480, 3840, 2160, OS_PRESENT_WHOLE, 2560, 1920));
	CHECK(SizeIs(100, 100, 5000, 5000, OS_PRESENT_WHOLE, 800, 800)); // eight at most
	CHECK(SizeIs(1280, 720, 1920, 1080, OS_PRESENT_FILL, 1920, 1080));
	CHECK(SizeIs(800, 600, 1920, 1080, OS_PRESENT_FILL, 1440, 1080));
	CHECK(SizeIs(1280, 720, 1024, 768, OS_PRESENT_FILL, 1024, 576));
	CHECK(SizeIs(1280, 720, 1920, 1080, OS_PRESENT_NONE, 1280, 720));
	CHECK(SizeIs(1920, 1080, 1024, 576, OS_PRESENT_STRETCH, 1024, 576)); // the canvas, whatever it is
	CHECK(SizeIs(800, 600, 1000, 301, OS_PRESENT_STRETCH, 1000, 301));
	CHECK(SizeIs(1920, 1080, 1366, 768, OS_PRESENT_NONE, 1920, 1080));
	// larger than the canvas
	CHECK(SizeIs(1920, 1080, 1366, 768, OS_PRESENT_POW2, 960, 540));
	CHECK(SizeIs(1920, 1080, 1366, 768, OS_PRESENT_WHOLE, 960, 540));
	CHECK(SizeIs(1920, 1080, 800, 600, OS_PRESENT_POW2, 480, 270));
	CHECK(SizeIs(1920, 1080, 800, 600, OS_PRESENT_WHOLE, 640, 360));
	CHECK(SizeIs(1281, 721, 1280, 720, OS_PRESENT_POW2, 641, 361));
	CHECK(SizeIs(0, 0, 100, 100, OS_PRESENT_POW2, 0, 0));
}

// a picture of w x h whose pixel (x, y) is unlike its neighbours
static uint32_t* Picture(int w, int h, uint32_t salt)
{
	uint32_t* p = (uint32_t*)malloc((size_t)w * (size_t)h * 4);
	int x, y;
	for(y = 0; y < h; y++)
		for(x = 0; x < w; x++)
			p[y * w + x] = (((uint32_t)x * 7 + (uint32_t)y * 13 + salt) & 255) << 16 | (((uint32_t)x * 3 + salt * 5) & 255) << 8 | (((uint32_t)y * 11 + salt) & 255);
	return p;
}

static void TestPlain(void)
{
	OsPresent_t* p = OsPresent_New();
	uint32_t* pic = Picture(8, 4, 1);
	const uint32_t* c;
	int changed = -1, x, y, w, h, ok = 1;
	printf("plain\n");
	CHECK(p != NULL);
	CHECK(OsPresent_Draw(p, (const uint8_t*)pic, 8, 4, 32, 32, &changed) == NULL); // nothing set up
	OsPresent_Place(p, &x, &y, &w, &h);
	CHECK(w == 0 && h == 0);
	OsPresent_Setup(p, 12, 10, OS_PRESENT_POW2, OS_PRESENT_BICUBIC);
	c = OsPresent_Draw(p, (const uint8_t*)pic, 8, 4, 32, 32, &changed);
	CHECK(c != NULL && changed == 1);
	OsPresent_Place(p, &x, &y, &w, &h);
	CHECK(x == 2 && y == 3 && w == 8 && h == 4);
	for(y = 0; c && y < 10; y++)
		for(x = 0; x < 12; x++)
		{
			int inside = x >= 2 && x < 10 && y >= 3 && y < 7;
			uint32_t want = inside ? pic[(y - 3) * 8 + (x - 2)] & 0xffffff : 0;
			ok &= (c[y * 12 + x] & 0xffffff) == want;
		}
	CHECK(ok);
	CHECK(OsPresent_Draw(p, (const uint8_t*)pic, 8, 4, 32, 32, &changed) == c && changed == 0);
	// places
	CHECK(OsPresent_ToPicture(p, 2, 3, &x, &y) == 1 && x == 0 && y == 0);
	CHECK(OsPresent_ToPicture(p, 9, 6, &x, &y) == 1 && x == 7 && y == 3);
	CHECK(OsPresent_ToPicture(p, 0, 0, &x, &y) == 0 && x == 0 && y == 0); // beside it: kept inside
	CHECK(OsPresent_ToPicture(p, 11, 9, &x, &y) == 0 && x == 7 && y == 3);
	CHECK(OsPresent_ToPicture(p, 5, 9, &x, &y) == 0 && x == 3 && y == 3);
	OsPresent_Delete(p);
	OsPresent_Delete(NULL);
	free(pic);
}

static int Same(const uint32_t* a, const uint32_t* b, int n)
{
	int i;
	for(i = 0; i < n; i++)
		if((a[i] & 0xffffff) != (b[i] & 0xffffff))
			return 0;
	return 1;
}

// a drawing in steps against one drawing, with the given set-up
static void Steps(int cw, int ch, int steps, int filter)
{
	enum
	{
		W = 37,
		H = 29
	};
	OsPresent_t* p = OsPresent_New();
	OsPresent_t* q = OsPresent_New();
	uint32_t* pic = Picture(W, H, 3);
	uint32_t* kept;
	const uint32_t* a;
	const uint32_t* b;
	int changed = 0, x, y, dy = -1, dh = -1;
	OsPresent_Setup(p, cw, ch, steps, filter);
	OsPresent_Setup(q, cw, ch, steps, filter);
	a = OsPresent_Draw(p, (const uint8_t*)pic, W, H, W * 4, 32, &changed);
	CHECK(a && changed == 1);
	OsPresent_Drawn(p, &dy, &dh);
	CHECK(dy == 0 && dh == ch); // a first drawing writes the whole canvas
	kept = (uint32_t*)malloc((size_t)cw * (size_t)ch * 4);
	memcpy(kept, a, (size_t)cw * (size_t)ch * 4);
	// rows 5 and 20 .. 22 change, and row 12 too without a word: that one must not show
	for(x = 0; x < W; x++)
	{
		pic[5 * W + x] ^= 0x00ff00;
		pic[12 * W + x] ^= 0xffffff;
		pic[20 * W + x] = 0xffffff;
		pic[21 * W + x] = 0;
		pic[22 * W + x] ^= 0x0f0f0f;
	}
	OsPresent_Changed(p, 5, 1);
	OsPresent_Changed(p, 20, 3);
	OsPresent_Changed(p, -4, 2);  // outside: nothing
	OsPresent_Changed(p, H, 100); // outside
	a = OsPresent_Draw(p, (const uint8_t*)pic, W, H, W * 4, 32, &changed);
	CHECK(a && changed == 1 && !Same(a, kept, cw * ch));
	// the rows it says it wrote are all that differ from before
	OsPresent_Drawn(p, &dy, &dh);
	CHECK(dy >= 0 && dh > 0 && dy + dh <= ch && dh < ch);
	for(y = 0; a && y < ch; y++)
		if(y < dy || y >= dy + dh)
			CHECK(Same(a + (size_t)y * (size_t)cw, kept + (size_t)y * (size_t)cw, cw));
	for(x = 0; x < W; x++)
		pic[12 * W + x] ^= 0xffffff; // (as it was)
	b = OsPresent_Draw(q, (const uint8_t*)pic, W, H, W * 4, 32, NULL);
	CHECK(a && b && Same(a, b, cw * ch)); // as if drawn at once, row 12 as it was
	OsPresent_Changed(p, 0, -1);
	a = OsPresent_Draw(p, (const uint8_t*)pic, W, H, W * 4, 32, &changed);
	CHECK(changed == 1 && a && b && Same(a, b, cw * ch));
	a = OsPresent_Draw(p, (const uint8_t*)pic, W, H, W * 4, 32, &changed); // nothing changed: nothing written
	OsPresent_Drawn(p, &dy, &dh);
	CHECK(changed == 0 && dh == 0);
	OsPresent_Delete(p);
	OsPresent_Delete(q);
	free(pic);
	free(kept);
}

static void TestChanges(void)
{
	printf("changes\n");
	Steps(100, 60, OS_PRESENT_POW2, OS_PRESENT_BICUBIC);    // twice
	Steps(37, 29, OS_PRESENT_POW2, OS_PRESENT_BICUBIC);     // as it is
	Steps(160, 130, OS_PRESENT_WHOLE, OS_PRESENT_BILINEAR); // four times
	Steps(90, 90, OS_PRESENT_FILL, OS_PRESENT_BICUBIC);     // by 90 / 37
	Steps(20, 20, OS_PRESENT_POW2, OS_PRESENT_BICUBIC);     // halved
	Steps(20, 20, OS_PRESENT_NONE, OS_PRESENT_NEAREST);     // cut
	Steps(200, 31, OS_PRESENT_FILL, OS_PRESENT_NEAREST);    // the height decides
	Steps(20, 16, OS_PRESENT_STRETCH, OS_PRESENT_BILINEAR); // a window smaller than the picture
	Steps(61, 47, OS_PRESENT_STRETCH, OS_PRESENT_BICUBIC);  // and a larger one
}

static double Cubic(double x)
{
	x = fabs(x);
	if(x <= 1)
		return (1.5 * x - 2.5) * x * x + 1;
	if(x < 2)
		return ((-0.5 * x + 2.5) * x - 4) * x + 2;
	return 0;
}

// channel `shift` of pixel (x, y) of `pic` scaled to twice its size, the straightforward way
static double Twice(const uint32_t* pic, int w, int h, int x, int y, int shift)
{
	double cx = (x + 0.5) / 2 - 0.5, cy = (y + 0.5) / 2 - 0.5, sum = 0;
	int fx = (int)floor(cx) - 1, fy = (int)floor(cy) - 1, i, j;
	for(j = 0; j < 4; j++)
		for(i = 0; i < 4; i++)
		{
			int sx = fx + i < 0 ? 0 : fx + i >= w ? w - 1
												  : fx + i,
				sy = fy + j < 0 ? 0 : fy + j >= h ? h - 1
												  : fy + j;
			sum += Cubic(fx + i - cx) * Cubic(fy + j - cy) * ((pic[sy * w + sx] >> shift) & 255);
		}
	return sum < 0 ? 0 : sum > 255 ? 255
								   : sum;
}

static void TestScaled(void)
{
	enum
	{
		W = 16,
		H = 12
	};
	OsPresent_t* p = OsPresent_New();
	uint32_t* pic = Picture(W, H, 9);
	uint32_t flat[W * H], edge[W * H], dots[W * H];
	const uint32_t* c;
	int x, y, ok = 1, worst = 0, filter;
	printf("scaled\n");
	// the nearest pixel, twice
	OsPresent_Setup(p, W * 2, H * 2, OS_PRESENT_POW2, OS_PRESENT_NEAREST);
	c = OsPresent_Draw(p, (const uint8_t*)pic, W, H, W * 4, 32, NULL);
	for(y = 0; c && y < H * 2; y++)
		for(x = 0; x < W * 2; x++)
			ok &= (c[y * W * 2 + x] & 0xffffff) == (pic[(y / 2) * W + x / 2] & 0xffffff);
	CHECK(c && ok);
	CHECK(OsPresent_ToPicture(p, 31, 23, &x, &y) == 1 && x == 15 && y == 11);
	CHECK(OsPresent_ToPicture(p, 5, 6, &x, &y) == 1 && x == 2 && y == 3);
	for(x = 0; x < W * H; x++)
	{
		flat[x] = 0x4080c0;
		edge[x] = x % W < W / 2 ? 0 : 0xffffff;
		dots[x] = (x % W + x / W) & 1 ? 0xffffff : 0;
	}
	for(filter = OS_PRESENT_BICUBIC; filter <= OS_PRESENT_BILINEAR; filter++)
	{
		OsPresent_Setup(p, W * 2, H * 2, OS_PRESENT_POW2, filter);
		c = OsPresent_Draw(p, (const uint8_t*)flat, W, H, W * 4, 32, NULL);
		for(ok = 1, x = 0; c && x < W * H * 4; x++)
			ok &= (c[x] & 0xffffff) == 0x4080c0; // the weights add up to one
		CHECK(c && ok);
		c = OsPresent_Draw(p, (const uint8_t*)edge, W, H, W * 4, 32, NULL);
		OsPresent_Changed(p, 0, -1);
		c = OsPresent_Draw(p, (const uint8_t*)edge, W, H, W * 4, 32, NULL);
		// black on the left, white on the right, rising in between, the same in every row
		CHECK(c && (c[0] & 0xffffff) == 0 && (c[W * 2 - 1] & 0xffffff) == 0xffffff);
		for(ok = 1, x = 1; c && x < W * 2; x++)
			ok &= (c[x] & 255) >= (c[x - 1] & 255) || filter == OS_PRESENT_BICUBIC; // (the curve dips before it rises)
		CHECK(ok);
		CHECK(c && (c[W - 1] & 255) < 128 && (c[W] & 255) > 127 && (c[W - 1] & 255) + (c[W] & 255) == 255);
		CHECK(c && memcmp(c, c + (size_t)W * 2 * (H * 2 - 1), (size_t)W * 2 * 4) == 0);
	}
	// against the straightforward calculation
	OsPresent_Setup(p, W * 2, H * 2, OS_PRESENT_POW2, OS_PRESENT_BICUBIC);
	c = OsPresent_Draw(p, (const uint8_t*)pic, W, H, W * 4, 32, NULL);
	for(y = 0; c && y < H * 2; y++)
		for(x = 0; x < W * 2; x++)
		{
			int s;
			for(s = 0; s < 24; s += 8)
			{
				int d = (int)((c[y * W * 2 + x] >> s) & 255) - (int)floor(Twice(pic, W, H, x, y, s) + 0.5);
				d = d < 0 ? -d : d;
				worst = d > worst ? d : worst;
			}
		}
	CHECK(c && worst <= 3); // (two passes of 8 bits)
	// half the size: single pixels of black and white become grey
	OsPresent_Setup(p, W / 2, H / 2, OS_PRESENT_POW2, OS_PRESENT_BICUBIC);
	c = OsPresent_Draw(p, (const uint8_t*)dots, W, H, W * 4, 32, NULL);
	OsPresent_Place(p, &x, &y, &ok, &worst);
	CHECK(x == 0 && y == 0 && ok == W / 2 && worst == H / 2);
	for(ok = 1, x = W / 2 + 1; c && x < W / 2 * (H / 2 - 1) - 1; x++)
		ok &= x % (W / 2) == 0 || x % (W / 2) == W / 2 - 1 || ((c[x] & 255) > 110 && (c[x] & 255) < 146);
	CHECK(c && ok);
	c = OsPresent_Draw(p, (const uint8_t*)flat, W, H, W * 4, 32, NULL);
	OsPresent_Changed(p, 0, -1);
	c = OsPresent_Draw(p, (const uint8_t*)flat, W, H, W * 4, 32, NULL);
	for(ok = 1, x = 0; c && x < W * H / 4; x++)
		ok &= (c[x] & 0xffffff) == 0x4080c0;
	CHECK(c && ok);
	OsPresent_Delete(p);
	free(pic);
}

static void TestForms(void)
{
	OsPresent_t* p = OsPresent_New();
	uint16_t five[6] = {0x7fff, 0x7c00, 0x03e0, 0x001f, 0x0000, 0x4210}; // white, red, green, blue, black, grey
	uint32_t* pic = Picture(8, 6, 2);
	const uint32_t* c;
	int changed = -1, x, y, w, h, i, ok = 1;
	printf("forms\n");
	OsPresent_Setup(p, 3, 2, OS_PRESENT_POW2, OS_PRESENT_BICUBIC);
	c = OsPresent_Draw(p, (const uint8_t*)five, 3, 2, 6, 16, &changed);
	CHECK(c && changed == 1);
	CHECK(c && (c[0] & 0xffffff) == 0xffffff && (c[1] & 0xffffff) == 0xff0000 && (c[2] & 0xffffff) == 0x00ff00);
	CHECK(c && (c[3] & 0xffffff) == 0x0000ff && (c[4] & 0xffffff) == 0 && (c[5] & 0xffffff) == 0x848484);
	OsPresent_Setup(p, 6, 4, OS_PRESENT_POW2, OS_PRESENT_NEAREST);
	c = OsPresent_Draw(p, (const uint8_t*)five, 3, 2, 6, 16, &changed);
	CHECK(c && changed == 1 && (c[0] & 0xffffff) == 0xffffff && (c[7] & 0xffffff) == 0xffffff && (c[2] & 0xffffff) == 0xff0000);
	CHECK(c && (c[23] & 0xffffff) == 0x848484);
	// no picture: black, said once
	c = OsPresent_Draw(p, NULL, 0, 0, 0, 0, &changed);
	for(i = 0; c && i < 24; i++)
		ok &= (c[i] & 0xffffff) == 0;
	CHECK(c && changed == 1 && ok);
	OsPresent_Place(p, &x, &y, &w, &h);
	CHECK(w == 0 && h == 0 && OsPresent_ToPicture(p, 1, 1, &x, &y) == 0);
	OsPresent_Changed(p, 0, -1);
	CHECK(OsPresent_Draw(p, NULL, 0, 0, 0, 0, &changed) == c && changed == 0);
	CHECK(OsPresent_Draw(p, (const uint8_t*)pic, 8, 6, 32, 24, &changed) == c && changed == 0); // no form it knows
	// a picture again, another one's size, another set-up: each all anew
	c = OsPresent_Draw(p, (const uint8_t*)five, 3, 2, 6, 16, &changed);
	CHECK(c && changed == 1 && (c[0] & 0xffffff) == 0xffffff);
	c = OsPresent_Draw(p, (const uint8_t*)pic, 2, 2, 32, 32, &changed);
	OsPresent_Place(p, &x, &y, &w, &h);
	CHECK(c && changed == 1 && x == 1 && y == 0 && w == 4 && h == 4);
	CHECK(c && (c[0] & 0xffffff) == 0 && (c[1] & 0xffffff) == (pic[0] & 0xffffff) && (c[5] & 0xffffff) == 0); // black where the wider one was
	OsPresent_Setup(p, 6, 4, OS_PRESENT_POW2, OS_PRESENT_NEAREST);                                            // the same again: nothing
	CHECK(OsPresent_Draw(p, (const uint8_t*)pic, 2, 2, 32, 32, &changed) == c && changed == 0);
	OsPresent_Setup(p, 6, 4, OS_PRESENT_NONE, OS_PRESENT_NEAREST);
	c = OsPresent_Draw(p, (const uint8_t*)pic, 2, 2, 32, 32, &changed);
	OsPresent_Place(p, &x, &y, &w, &h);
	CHECK(c && changed == 1 && x == 2 && y == 1 && w == 2 && h == 2 && (c[1] & 0xffffff) == 0 && (c[8] & 0xffffff) == (pic[0] & 0xffffff));
	// larger than the canvas and not scaled: its middle
	OsPresent_Setup(p, 4, 2, OS_PRESENT_NONE, OS_PRESENT_BICUBIC);
	c = OsPresent_Draw(p, (const uint8_t*)pic, 8, 6, 32, 32, &changed);
	OsPresent_Place(p, &x, &y, &w, &h);
	CHECK(c && changed == 1 && x == -2 && y == -2 && w == 8 && h == 6);
	for(ok = 1, y = 0; c && y < 2; y++)
		for(x = 0; x < 4; x++)
			ok &= (c[y * 4 + x] & 0xffffff) == (pic[(y + 2) * 8 + x + 2] & 0xffffff);
	CHECK(c && ok);
	CHECK(OsPresent_ToPicture(p, 0, 0, &x, &y) == 1 && x == 2 && y == 2);
	// the same halved to fit
	OsPresent_Setup(p, 4, 3, OS_PRESENT_POW2, OS_PRESENT_NEAREST);
	c = OsPresent_Draw(p, (const uint8_t*)pic, 8, 6, 32, 32, &changed);
	OsPresent_Place(p, &x, &y, &w, &h);
	CHECK(c && changed == 1 && x == 0 && y == 0 && w == 4 && h == 3);
	CHECK(OsPresent_ToPicture(p, 3, 2, &x, &y) == 1 && x == 7 && y == 5);
	OsPresent_Delete(p);
	free(pic);
}

int main(void)
{
	TestSize();
	TestPlain();
	TestChanges();
	TestScaled();
	TestForms();
	if(gFails)
	{
		printf("present: %d checks failed\n", gFails);
		return 1;
	}
	printf("present: all tests passed\n");
	return 0;
}
