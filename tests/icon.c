/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/icon.c - unit test of the icons (src/base/icon.c,
 *                src/include/bgi/icon.h); built as test_icon by `make test`
 *
 * The groups of tests, in the order they run:
 *
 *   program  - the icon of a Windows program as an .ico file: a group of
 *              one picture and of several, a header of 32 and of 64 bits,
 *              a picture the group names and the program lacks, a program
 *              without resources, bytes that are no program, and a
 *              program cut short at every length
 *   pictures - an .ico file as pictures: bitmaps of 32, 24, 8, 4 and 1
 *              bits with their masks, a PNG file, the largest first, one
 *              a size (the one with the most colours), no more than asked
 *              for, a picture that is not in the file
 *   own      - OpenBGI's own icon: its sizes, clear corners, a solid
 *              middle
 */
#include "bgi/icon.h"
#include "bgi/png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "iconexe.h"

static uint32_t Le16(const uint8_t* p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t Le32(const uint8_t* p)
{
	return Le16(p) | Le16(p + 2) << 16;
}

static void TestProgram(void)
{
	static uint8_t exe[0x2000];
	const int one[1] = {16}, three[3] = {16, 8, 4};
	size_t n, size = 0, cut;
	uint8_t* ico;
	IconImage_t img[4];

	// one picture
	n = IconExe_Make(exe, 1, one, 0x20, 0, -1, NULL);
	CHECK(Icon_IsExe(exe, n));
	ico = Icon_FromExe(exe, n, &size);
	CHECK(ico != NULL && size == 6 + 16 + 40 + 16 * 16 * 4 + 4 * 16);
	if(ico)
	{
		CHECK(Le16(ico) == 0 && Le16(ico + 2) == 1 && Le16(ico + 4) == 1);
		CHECK(ico[6] == 16 && ico[7] == 16 && Le16(ico + 12) == 32);
		CHECK(Le32(ico + 14) == size - 22 && Le32(ico + 18) == 22);
		CHECK(Le32(ico + 22) == 40 && Le32(ico + 22 + 4) == 16); // the picture begins with its header
		CHECK(Icon_Decode(ico, size, img, 4) == 1 && img[0].w == 16 && img[0].h == 16);
		CHECK(img[0].bgra[3] == 0 && img[0].bgra[4] == 0x20 && img[0].bgra[5] == 0x40 && img[0].bgra[6] == 0x80 && img[0].bgra[7] == 255);
		Icon_Free(img, 1);
	}
	BGI_Free(ico);

	// three, in a program of 64 bits
	n = IconExe_Make(exe, 3, three, 0x20, 1, -1, NULL);
	CHECK(Icon_IsExe(exe, n));
	ico = Icon_FromExe(exe, n, &size);
	CHECK(ico != NULL);
	if(ico)
	{
		CHECK(Le16(ico + 4) == 3 && ico[6] == 16 && ico[6 + 16] == 8 && ico[6 + 32] == 4);
		CHECK(Le32(ico + 6 + 12) == 6 + 48 && Le32(ico + 6 + 16 + 12) == 6 + 48 + Le32(ico + 6 + 8));
		CHECK(Icon_Decode(ico, size, img, 4) == 3 && img[0].w == 16 && img[1].w == 8 && img[2].w == 4);
		CHECK(img[1].bgra[4] == 0x2a && img[2].bgra[4] == 0x34); // each its own colour
		Icon_Free(img, 3);
	}
	BGI_Free(ico);

	// the second picture is not in the program: the file has the other two
	n = IconExe_Make(exe, 3, three, 0x20, 0, 1, NULL);
	ico = Icon_FromExe(exe, n, &size);
	CHECK(ico != NULL);
	if(ico)
	{
		CHECK(Le16(ico + 4) == 2 && ico[6] == 16 && ico[6 + 16] == 4 && Le32(ico + 6 + 12) == 6 + 32);
		CHECK(size == 6 + 32 + Le32(ico + 6 + 8) + Le32(ico + 6 + 16 + 8));
		CHECK(Icon_Decode(ico, size, img, 4) == 2 && img[0].w == 16 && img[1].w == 4 && img[1].bgra[4] == 0x34);
		Icon_Free(img, 2);
	}
	BGI_Free(ico);

	// a program that says it has no resources; bytes that are no program
	n = IconExe_Make(exe, 1, one, 0x20, 0, -1, NULL);
	IconExe_Put32(exe + 0x40 + 24 + 96 + 16, 0);
	CHECK(Icon_IsExe(exe, n) && Icon_FromExe(exe, n, &size) == NULL && size == 0);
	CHECK(!Icon_IsExe((const uint8_t*)"MZ", 2) && Icon_FromExe((const uint8_t*)"MZ", 2, &size) == NULL);
	memset(exe, 0x41, sizeof exe);
	CHECK(!Icon_IsExe(exe, sizeof exe) && Icon_FromExe(exe, sizeof exe, &size) == NULL);

	// cut short at every length: nothing, or a file that reads
	n = IconExe_Make(exe, 3, three, 0x20, 0, -1, NULL);
	for(cut = 0; cut < n; cut++)
	{
		uint8_t* part = (uint8_t*)malloc(cut ? cut : 1); // (its own block: a read past the end is one the sanitizer sees)
		memcpy(part, exe, cut);
		ico = Icon_FromExe(part, cut, &size);
		if(ico)
		{
			int got = Icon_Decode(ico, size, img, 4);
			CHECK(got >= 1 && got <= 3);
			Icon_Free(img, got);
		}
		BGI_Free(ico);
		free(part);
	}
}

// an .ico file of `count` pictures: the directory, then the pictures one after the other
static size_t MakeIco(uint8_t* out, int count, const uint8_t* const* pic, const uint32_t* size)
{
	size_t at = 6 + (size_t)count * 16;
	int i;
	memset(out, 0, at);
	IconExe_Put16(out + 2, 1);
	IconExe_Put16(out + 4, (uint32_t)count);
	for(i = 0; i < count; i++)
	{
		IconExe_Put32(out + 6 + i * 16 + 8, size[i]);
		IconExe_Put32(out + 6 + i * 16 + 12, (uint32_t)at);
		memcpy(out + at, pic[i], size[i]);
		at += size[i];
	}
	return at;
}

/* A bitmap of an icon, 2 x 2 pixels of `bpp` bits: the pixels are the
 * colours 0, 1 / 1, 0 of its table (below 24 bits) or blue and red; the
 * mask clears the bottom right one.  Its size. */
static uint32_t SmallBitmap(uint8_t* out, int bpp)
{
	uint32_t table = bpp <= 8 ? 2 : 0, row = 4, at;
	memset(out, 0, 128);
	IconExe_Put32(out, 40);
	IconExe_Put32(out + 4, 2);
	IconExe_Put32(out + 8, 4);
	IconExe_Put16(out + 12, 1);
	IconExe_Put16(out + 14, (uint32_t)bpp);
	IconExe_Put32(out + 32, table);
	if(table)
	{ // colour 0 blue, colour 1 red
		out[40] = 255;
		out[46] = 255;
	}
	at = 40 + table * 4;
	if(bpp == 24)
		row = 8;
	// the rows from the bottom up: the bottom one is "1, 0", the top one "0, 1"
	switch(bpp)
	{
		case 1:
			out[at] = 0x80;
			out[at + row] = 0x40;
			break;
		case 4:
			out[at] = 0x10;
			out[at + row] = 0x01;
			break;
		case 8:
			out[at] = 1;
			out[at + row + 1] = 1;
			break;
		default: // 24: B G R
			out[at + 2] = 255;
			out[at + 3] = 255;
			out[at + row] = 255;
			out[at + row + 5] = 255;
			break;
	}
	at += row * 2;
	out[at] = 0x40; // the mask's bottom row: the right pixel is clear
	return at + 8;
}

static void TestPictures(void)
{
	static uint8_t ico[0x4000], big[0x800], small32[0x200], bmp[5][128], pix[8 * 8 * 4];
	const uint8_t* pic[6];
	uint32_t size[6];
	IconImage_t img[8];
	size_t n, pngSize = 0;
	uint8_t* png;
	int bits[4] = {24, 8, 4, 1}, i, got;

	// each depth by itself: blue, red / red, clear
	for(i = 0; i < 4; i++)
	{
		size[0] = SmallBitmap(bmp[i], bits[i]);
		pic[0] = bmp[i];
		n = MakeIco(ico, 1, pic, size);
		got = Icon_Decode(ico, n, img, 8);
		CHECK(got == 1 && img[0].w == 2 && img[0].h == 2);
		if(got == 1)
		{
			const uint8_t* p = img[0].bgra;
			CHECK(p[0] == 255 && p[2] == 0 && p[3] == 255);   // top left: blue
			CHECK(p[4] == 0 && p[6] == 255 && p[7] == 255);   // top right: red
			CHECK(p[8] == 0 && p[10] == 255 && p[11] == 255); // bottom left: red
			CHECK(p[12] == 255 && p[14] == 0 && p[15] == 0);  // bottom right: blue, and clear by the mask
		}
		Icon_Free(img, got);
	}

	// 32 bits: the alpha is the picture's own; without any, the mask's
	size[0] = IconExe_Picture(small32, 4, 1, 2, 3);
	pic[0] = small32;
	n = MakeIco(ico, 1, pic, size);
	got = Icon_Decode(ico, n, img, 8);
	CHECK(got == 1 && img[0].bgra[3] == 0 && img[0].bgra[7] == 255 && img[0].bgra[4] == 1 && img[0].bgra[6] == 3);
	Icon_Free(img, got);
	for(i = 0; i < 16; i++)
		small32[40 + i * 4 + 3] = 0;
	small32[40 + 16 * 4] = 0x80; // the mask clears the bottom left pixel
	n = MakeIco(ico, 1, pic, size);
	got = Icon_Decode(ico, n, img, 8);
	CHECK(got == 1 && img[0].bgra[3] == 255 && img[0].bgra[12 * 4 + 3] == 0 && img[0].bgra[13 * 4 + 3] == 255);
	Icon_Free(img, got);

	// a PNG file among bitmaps; the largest first; one a size, the one with the most colours
	for(i = 0; i < 8 * 8; i++)
	{
		pix[i * 4] = 10;
		pix[i * 4 + 1] = 20;
		pix[i * 4 + 2] = 30;
		pix[i * 4 + 3] = (uint8_t)(i ? 255 : 128);
	}
	png = Png_Encode(pix, 8, 8, 4, 8 * 4, 6, &pngSize);
	CHECK(png != NULL);
	size[0] = SmallBitmap(bmp[0], 8); // 2 x 2, 8 bits
	pic[0] = bmp[0];
	size[1] = (uint32_t)pngSize; // 8 x 8
	pic[1] = png;
	size[2] = IconExe_Picture(big, 4, 7, 8, 9); // 4 x 4
	pic[2] = big;
	size[3] = SmallBitmap(bmp[1], 24); // 2 x 2 again, 24 bits: takes the place of the first
	pic[3] = bmp[1];
	size[4] = SmallBitmap(bmp[2], 1); // 2 x 2 once more, 1 bit: does not
	pic[4] = bmp[2];
	n = png ? MakeIco(ico, 5, pic, size) : 0;
	got = Icon_Decode(ico, n, img, 8);
	CHECK(got == 3 && img[0].w == 8 && img[1].w == 4 && img[2].w == 2);
	if(got == 3)
	{
		CHECK(img[0].bgra[0] == 10 && img[0].bgra[1] == 20 && img[0].bgra[2] == 30 && img[0].bgra[3] == 128 && img[0].bgra[7] == 255);
		CHECK(img[1].bgra[4] == 7 && img[1].bgra[5] == 8 && img[1].bgra[6] == 9);
		CHECK(img[2].bgra[0] == 255 && img[2].bgra[6] == 255); // (the same picture at any depth)
	}
	Icon_Free(img, got);
	// no more than asked for: the largest
	got = Icon_Decode(ico, n, img, 2);
	CHECK(got == 2 && img[0].w == 8 && img[1].w == 4);
	Icon_Free(img, got);
	got = Icon_Decode(ico, n, img, 1);
	CHECK(got == 1 && img[0].w == 8);
	Icon_Free(img, got);
	CHECK(Icon_Decode(ico, n, img, 0) == 0);

	// a picture that is not in the file is left out; a file that is none gives nothing
	if(n)
	{
		IconExe_Put32(ico + 6 + 16 + 12, (uint32_t)n + 100);
		got = Icon_Decode(ico, n, img, 8);
		CHECK(got == 2 && img[0].w == 4 && img[1].w == 2);
		Icon_Free(img, got);
		CHECK(Icon_Decode(ico, 5, img, 8) == 0 && Icon_Decode(ico, 6 + 16, img, 8) == 0);
		ico[2] = 2; // a cursor
		CHECK(Icon_Decode(ico, n, img, 8) == 0);
	}
	BGI_Free(png);
}

// the icon the engine carries: four pictures, the largest first, an orange square with a white figure
static void TestOwn(void)
{
	static const int kSizes[4] = {256, 48, 32, 16};
	IconImage_t img[6];
	int i, n;
	memset(img, 0, sizeof img);
	n = Icon_Own(img, 6);
	CHECK(n == 4);
	for(i = 0; i < n && i < 4; i++)
	{
		int s = kSizes[i];
		CHECK(img[i].w == s && img[i].h == s && img[i].bgra != NULL);
		if(img[i].bgra && img[i].w == s && img[i].h == s)
		{
			const uint8_t* corner = img[i].bgra + ((size_t)(s - 1) * (size_t)s) * 4;                           // bottom left: outside the square
			const uint8_t* orange = img[i].bgra + ((size_t)(s * 3 / 4) * (size_t)s + (size_t)(s * 3 / 4)) * 4; // the square, right of the figure
			const uint8_t* white = img[i].bgra + ((size_t)(s / 2) * (size_t)s + (size_t)(s / 4)) * 4;          // the figure
			CHECK(corner[3] == 0);
			CHECK(orange[3] == 255 && orange[2] > 200 && orange[0] < 80 && orange[1] > 100 && orange[1] < 200);
			CHECK(white[3] == 255 && white[0] > 230 && white[1] > 230 && white[2] > 230);
		}
	}
	Icon_Free(img, n);
	memset(img, 0, sizeof img);
	n = Icon_Own(img, 2); // no more than asked for
	CHECK(n == 2 && img[0].w == 256 && img[1].w == 48);
	Icon_Free(img, n);
}

int main(void)
{
	printf("program\n");
	TestProgram();
	printf("pictures\n");
	TestPictures();
	printf("own\n");
	TestOwn();
	if(gFails)
	{
		printf("icon: %d checks failed\n", gFails);
		return 1;
	}
	printf("icon: all tests passed\n");
	return 0;
}
