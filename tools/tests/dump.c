/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/dump.c - unit test of the dump and pack tools (src/dump/, declared
 *                in inc/bgi/dump.h) and of the codecs written for them
 *                (deflate.h, png.h, the DSC / CompressedBG / BF_Movie
 *                encoders); built as bin/test_dump by `make test`
 *
 *   codecs  - CRC-32 against its check value; zlib streams through the
 *             compressor and the decompressor at several levels, a stream
 *             made by zlib itself, and damaged streams that have to be
 *             refused; PNG files of the three depths both ways, a palette
 *             file with transparency and an interlaced 16-bit file made by
 *             other encoders, and the channel conversions; the DSC,
 *             CompressedBG and BF_Movie encoders against the engine's
 *             decoders
 *   dump    - a small game is written into a scratch directory under
 *             /tmp: five archives of both index formats holding a program,
 *             a scenario file, two tables, images of every depth and
 *             storage, a movie, an SDC block with a prefix, two sounds,
 *             plain files, a Shift-JIS name and two names that want the
 *             same file - plus a file that only looks like an archive.
 *             Dump_Run has to identify the game, convert every entry to
 *             its kind and refuse to write over the dump a second time
 *   pack    - Pack_Run on the untouched dump gives archives identical to
 *             the originals; with rebuildAll (no originals read) archives
 *             whose entries decode to the same content
 *   edit    - files of the dump are changed (a picture, a mask given in
 *             colour, the scenario's text, the program's text, a sound's
 *             loop point, a table row, a movie frame) and Pack_Run with
 *             changedOnly has to write exactly the archives concerned,
 *             with the changes in and everything else byte for byte as it
 *             was; a source that does not compile keeps the original
 *             entry and says so through the exit status
 *
 * A failure names the check.  The scratch directory is removed at the end.
 */
#include "bgi/dump.h"
#include "bgi/arcread.h"
#include "bgi/bfmovie.h"
#include "bgi/bpc.h"
#include "bgi/codec.h"
#include "bgi/deflate.h"
#include "bgi/png.h"
#include "bgi/scn.h"
#include "bgi/snd/bw.h"
#include "bgi/os.h"

#include <time.h>
#include <unistd.h>

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

static uint32_t gSeed = 2026;
// a small deterministic generator for test data
static uint32_t Rnd(void)
{
	gSeed = gSeed * 1103515245u + 12345u;
	return (gSeed >> 16) & 0x7fff;
}

static void Put32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t Get32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---- the codecs ------------------------------------------------------------------------------------

// zlib.compress(b"hello hello hello hello", 9): a fixed-Huffman block with a match
static const uint8_t kZlibStream[] = {0x78, 0xda, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x27, 0x01, 0x68, 0x03, 0x08, 0xb1};

static void TestDeflate(void)
{
	static const int levels[] = {0, 1, 4, 6, 9};
	uint8_t* data = (uint8_t*)malloc(200000);
	uint8_t *z, *back;
	size_t zn, bn, i, k;
	CHECK(BGI_Crc32(0, "123456789", 9) == 0xCBF43926u);
	CHECK(BGI_Crc32(BGI_Crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
	// text-like data, noise and a long run, in one buffer
	for(i = 0; i < 200000; i++)
		data[i] = i < 80000 ? (uint8_t) "the quick brown fox "[(i * 7 + i / 300) % 20] : i < 140000 ? (uint8_t)Rnd()
																									: 0x55;
	for(k = 0; k < sizeof levels / sizeof levels[0]; k++)
	{
		static const size_t sizes[] = {0, 1, 3, 70000, 200000};
		size_t s;
		for(s = 0; s < sizeof sizes / sizeof sizes[0]; s++)
		{
			z = Deflate_Compress(data, sizes[s], levels[k], &zn);
			CHECK(z != NULL);
			back = z ? Deflate_Uncompress(z, zn, 0, &bn) : NULL;
			CHECK(back != NULL && bn == sizes[s] && memcmp(back, data, bn) == 0);
			if(levels[k] >= 4 && sizes[s] == 200000)
				CHECK(zn < 70000); // 60000 bytes of noise, the rest all but free
			free(z);
			free(back);
		}
	}
	back = Deflate_Uncompress(kZlibStream, sizeof kZlibStream, 0, &bn);
	CHECK(back != NULL && bn == 23 && memcmp(back, "hello hello hello hello", 23) == 0);
	free(back);
	// a wrong check value, a truncated stream, a bad header
	{
		uint8_t bad[sizeof kZlibStream];
		memcpy(bad, kZlibStream, sizeof bad);
		bad[sizeof bad - 1] ^= 1;
		CHECK(Deflate_Uncompress(bad, sizeof bad, 0, &bn) == NULL);
		CHECK(Deflate_Uncompress(kZlibStream, sizeof kZlibStream - 5, 0, &bn) == NULL);
		memcpy(bad, kZlibStream, sizeof bad);
		bad[0] = 0x79;
		CHECK(Deflate_Uncompress(bad, sizeof bad, 0, &bn) == NULL);
	}
	free(data);
}

// 3 x 2, palette of three colours with a transparency chunk (written by PIL): pixels 0 1 2 / 2 1 0
static const uint8_t kPngPalette[] = {
	0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02,
	0x08, 0x03, 0x00, 0x00, 0x00, 0xaa, 0xaa, 0x96, 0x28, 0x00, 0x00, 0x03, 0x00, 0x50, 0x4c, 0x54, 0x45, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
	0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0xcf, 0x8b, 0x43, 0x00, 0x00, 0x00,
	0x03, 0x74, 0x52, 0x4e, 0x53, 0xff, 0x80, 0x00, 0x7f, 0x6d, 0x68, 0x78, 0x00, 0x00, 0x00, 0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60,
	0x60, 0x64, 0x62, 0x60, 0x62, 0x64, 0x00, 0x00, 0x00, 0x20, 0x00, 0x07, 0x1d, 0x2b, 0x70, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
	0xae, 0x42, 0x60, 0x82};

static void TestPng(void)
{
	static const uint32_t dims[][2] = {{1, 1}, {7, 3}, {64, 33}, {301, 17}};
	uint32_t d, i;
	int ch;
	char err[128];
	for(d = 0; d < sizeof dims / sizeof dims[0]; d++)
		for(ch = 1; ch <= 4; ch += ch == 1 ? 2 : 1)
		{
			uint32_t w = dims[d][0], h = dims[d][1], pitch = w * (uint32_t)ch + 5; // rows with padding the encoder has to skip
			uint8_t* pix = (uint8_t*)malloc((size_t)pitch * h);
			uint8_t* png;
			size_t n = 0;
			PngImage_t img;
			for(i = 0; i < pitch * h; i++)
				pix[i] = (uint8_t)((i % 13 == 0) ? Rnd() : i / (uint32_t)ch + i % (uint32_t)ch * 50);
			png = Png_Encode(pix, w, h, ch, pitch, 6, &n);
			CHECK(png != NULL && n > 8 + 25 + 12 + 12 && memcmp(png, "\x89PNG\r\n\x1a\n", 8) == 0);
			CHECK(png && Png_Decode(png, n, &img, err, sizeof err));
			if(png && img.pixels)
			{
				uint32_t y;
				CHECK(img.width == w && img.height == h && img.channels == ch);
				for(y = 0; y < h; y++)
					CHECK(memcmp(img.pixels + (size_t)y * w * (uint32_t)ch, pix + (size_t)y * pitch, (size_t)w * (uint32_t)ch) == 0);
				free(img.pixels);
			}
			// a flipped bit is caught by the chunk's CRC
			if(png)
			{
				png[n - 20] ^= 4;
				CHECK(!Png_Decode(png, n, &img, err, sizeof err));
			}
			free(png);
			free(pix);
		}
	{ // another encoder's file: a palette with alphas 255, 128, 0 becomes B G R A
		PngImage_t img;
		static const uint8_t expect[6][4] = {{0, 0, 255, 255}, {0, 255, 0, 128}, {255, 0, 0, 0}, {255, 0, 0, 0}, {0, 255, 0, 128}, {0, 0, 255, 255}};
		CHECK(Png_Decode(kPngPalette, sizeof kPngPalette, &img, err, sizeof err));
		CHECK(img.width == 3 && img.height == 2 && img.channels == 4 && img.pixels && memcmp(img.pixels, expect, sizeof expect) == 0);
		// to three channels: the alpha is lost and that is said; to grey: the colours too
		CHECK(Png_Convert(&img, 3) == PNG_LOST_ALPHA && img.channels == 3 && img.pixels[0] == 0 && img.pixels[2] == 255);
		CHECK(Png_Convert(&img, 1) == PNG_LOST_COLOUR && img.channels == 1 && img.pixels[0] == 76 && img.pixels[1] == 150 && img.pixels[2] == 27);
		CHECK(Png_Convert(&img, 4) == 0 && img.channels == 4 && img.pixels[0] == 76 && img.pixels[1] == 76 && img.pixels[3] == 255);
		free(img.pixels);
		CHECK(!Png_Decode(kPngPalette, sizeof kPngPalette - 30, &img, err, sizeof err));
		CHECK(!Png_Decode((const uint8_t*)"GIF89a..........", 16, &img, err, sizeof err));
	}
}

static void TestEncoders(void)
{
	uint32_t n = 300000, size = 0, i;
	uint8_t* data = (uint8_t*)malloc(n);
	uint8_t* back = (uint8_t*)malloc(n + 16);
	uint8_t* z;
	static const uint32_t sizes[] = {0, 1, 2, 37, 5000, 300000};
	// DSC: structured data with noise in it, at sizes from nothing up
	for(i = 0; i < n; i++)
		data[i] = (uint8_t)(Rnd() % 9 == 0 ? Rnd() : i / 700);
	for(i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
	{
		z = BGI_DscEncode(data, sizes[i], 0x12345 + i, &size);
		CHECK(BGI_IsDsc(z) && BGI_DscOutSize(z) == sizes[i]);
		CHECK(BGI_DscDecode(z, back) == sizes[i] && memcmp(back, data, sizes[i]) == 0);
		if(sizes[i] == 300000)
			CHECK(size < 200000);
		BGI_Free(z);
	}
	memset(data, 'a', 1000); // a run: copies at the smallest distance
	z = BGI_DscEncode(data, 1000, 7, &size);
	CHECK(BGI_DscDecode(z, back) == 1000 && memcmp(back, data, 1000) == 0 && size < 0x220 + 40);
	BGI_Free(z);
	free(data);
	free(back);
	{ // CompressedBG: every depth, flat, noisy and smooth pictures, the extra header words kept
		static const uint32_t shapes[][3] = {{1, 1, 8}, {1, 1, 32}, {17, 9, 24}, {64, 64, 32}, {333, 21, 8}, {5, 300, 24}};
		uint32_t s, pattern;
		for(s = 0; s < sizeof shapes / sizeof shapes[0]; s++)
			for(pattern = 0; pattern < 3; pattern++)
			{
				uint32_t w = shapes[s][0], h = shapes[s][1], bpp = shapes[s][2], bytes = w * h * (bpp / 8);
				uint8_t* img = (uint8_t*)calloc(1, 16 + bytes);
				uint8_t* out = (uint8_t*)calloc(1, 16 + bytes + 16);
				RawImageHeader_t ih;
				memset(&ih, 0, sizeof ih);
				ih.width = (uint16_t)w;
				ih.height = (uint16_t)h;
				ih.bpp = bpp;
				ih.unused08 = 0x10000;
				ih.unused0c = 0x00340012;
				memcpy(img, &ih, 16);
				for(i = 0; i < bytes; i++)
					img[16 + i] = (uint8_t)(pattern == 0 ? 0x40 : pattern == 1 ? Rnd()
																			   : i / (bpp / 8) % w + i % 3);
				z = BGI_CbgEncode(img, 0xabc + s, &size);
				CHECK(BGI_IsCbg(z) && ((const CbgHeader_t*)z)->version == 1 && BGI_CbgOutSize(z) == 16 + bytes);
				CHECK(BGI_CbgDecode(z, out) == 0 && memcmp(out, img, 16 + bytes) == 0);
				BGI_Free(z);
				free(img);
				free(out);
			}
	}
	{ // BF_Movie: frames that change in places, with zero pixels and unchanged ones, at both depths
		uint32_t bpp;
		for(bpp = 24; bpp <= 32; bpp += 8)
		{
			enum
			{
				W = 37,
				H = 21,
				FRAMES = 4
			};
			static uint8_t pix[FRAMES][W * H * 4];
			uint8_t slot[W * H * 4];
			BfMovieEnc_t* enc = BfMovieEnc_Begin(W, H, bpp, 15);
			uint8_t* file;
			uint32_t f;
			CHECK(BfMovieEnc_Begin(0, 5, 32, 30) == NULL && BfMovieEnc_Begin(4, 4, 16, 30) == NULL);
			for(f = 0; f < FRAMES; f++)
			{
				if(f)
					memcpy(pix[f], pix[f - 1], sizeof pix[f]);
				for(i = 0; i < W * H; i++)
				{
					uint32_t r = Rnd();
					uint8_t* p = pix[f] + i * 4;
					if(f == 0 || r % 11 == 0)
					{
						p[0] = (uint8_t)Rnd();
						p[1] = (uint8_t)(i + f);
						p[2] = 0x80;
						p[3] = (uint8_t)(bpp == 32 ? Rnd() : 0);
					}
					else if(r % 17 == 0)
						memset(p, 0, 4);
				}
				BfMovieEnc_AddFrame(enc, pix[f], W * 4);
			}
			file = BfMovieEnc_Finish(enc, &size);
			CHECK(BfMovie_Check(file, size) && BfMovie_IsSimple(file));
			CHECK(((const BfMovieHeader_t*)file)->frames == FRAMES && ((const BfMovieHeader_t*)file)->fps == 15 &&
				((const BfMovieHeader_t*)file)->mode == (bpp == 32 ? 2u : 1u));
			memset(slot, 0, sizeof slot);
			for(f = 0; f < FRAMES; f++)
				CHECK(BfMovie_DecodeFrame(file, size, f, slot, W * 4) && memcmp(slot, pix[f], sizeof slot) == 0);
			BGI_Free(file);
		}
	}
}

// ---- files of the scratch directory ----------------------------------------------------------------

static char gRoot[0x200]; // the scratch directory

static const char* Path(const char* rel)
{
	static char bufs[8][0x400];
	static int next;
	char* p = bufs[next++ & 7];
	snprintf(p, 0x400, "%s/%s", gRoot, rel);
	return p;
}

// the same in a string of its own, for a path that is kept (an option of a run)
static const char* Keep(const char* rel)
{
	const char* p = Path(rel);
	char* copy = (char*)malloc(strlen(p) + 1);
	strcpy(copy, p);
	return copy;
}

static uint8_t* ReadAll(const char* rel, uint32_t* size)
{
	*size = 0;
	return OS_ReadNativeFile(Path(rel), size);
}

static int Exists(const char* rel)
{
	return OS_NativeStat(Path(rel), NULL) != OS_NATIVE_NONE;
}

// 1 when both files exist and hold the same bytes
static int SameFile(const char* a, const char* b)
{
	uint32_t an, bn;
	uint8_t* x = ReadAll(a, &an);
	uint8_t* y = ReadAll(b, &bn);
	int same = x && y && an == bn && memcmp(x, y, an) == 0;
	free(x);
	free(y);
	return same;
}

// remove a directory with everything in it
static void RemoveTreeEntry(void* ctx, const char* name, int isDir);
static void RemoveTree(const char* path)
{
	OS_NativeList(path, RemoveTreeEntry, (void*)path);
	remove(path);
}

static void RemoveTreeEntry(void* ctx, const char* name, int isDir)
{
	char path[0x800];
	snprintf(path, sizeof path, "%s/%s", (const char*)ctx, name);
	if(isDir)
		RemoveTree(path);
	else
		remove(path);
}

// replace the first occurrence of `from` in a text file of the dump by `to`; 1 when it was there
static int EditText(const char* rel, const char* from, const char* to)
{
	uint32_t n;
	char* text = (char*)ReadAll(rel, &n);
	char* at = text ? strstr(text, from) : NULL;
	FILE* f;
	if(!at)
	{
		free(text);
		return 0;
	}
	f = fopen(Path(rel), "wb");
	fwrite(text, 1, (size_t)(at - text), f);
	fwrite(to, 1, strlen(to), f);
	fwrite(at + strlen(from), 1, strlen(at + strlen(from)), f);
	fclose(f);
	free(text);
	return 1;
}

// an entry of an archive of the scratch directory as the engine reads it (BGI_Free it); NULL when it is not there
static uint8_t* Entry(const char* arcRel, const char* name, uint32_t* size, int* dsc)
{
	ArcRead_t* a = ArcRead_Open(Path(arcRel));
	const ArcEntry_t* e = a ? ArcRead_Find(a, name) : NULL;
	uint8_t* data = e ? ArcRead_Load(a, e, size, dsc) : NULL;
	if(a)
		ArcRead_Close(a);
	return data;
}

// 1 when the bytes hold the text somewhere
static int Holds(const uint8_t* data, uint32_t n, const char* text)
{
	uint32_t len = (uint32_t)strlen(text), i;
	for(i = 0; data && i + len <= n; i++)
		if(memcmp(data + i, text, len) == 0)
			return 1;
	return 0;
}

// ---- the game ------------------------------------------------------------------------------------

#define MAX_ENTRIES 16

typedef struct TestArc
{
	ArcWriteEntry_t entries[MAX_ENTRIES];
	int count;
} TestArc_t;

// add an entry; `dsc` stores it compressed
static void Add(TestArc_t* a, const char* name, const uint8_t* data, uint32_t size, int dsc)
{
	ArcWriteEntry_t* e = &a->entries[a->count++];
	e->name = name;
	if(dsc)
		e->data = BGI_DscEncode(data, size, 0x1000 + (uint32_t)a->count, &e->size);
	else
	{
		uint8_t* copy = (uint8_t*)BGI_Alloc(size + 1);
		memcpy(copy, data, size);
		e->data = copy;
		e->size = size;
	}
}

static void WriteArc(TestArc_t* a, const char* rel, int arc20)
{
	uint32_t size;
	uint8_t* image = ArcWrite_Build(a->entries, a->count, arc20, &size);
	int i;
	CHECK(OS_WriteNativeFile(Path(rel), image, size));
	free(image);
	for(i = 0; i < a->count; i++)
		BGI_Free((void*)a->entries[i].data);
	a->count = 0;
}

static const char* kProgram = "#engine 1.69/444\n\nmain() { int a;\n    a = 3;\n    debug_msg(\"Tayutama\");\n}\n";

static const char* kScenario = "#style library\n"
							   "#source \"main.bss\"\n"
							   "#line 1\n"
							   "greet(p1) { int a;\n"
							   "    a = p1 + 2;\n"
							   "    cmd_0x140(\"hello\", a);\n"
							   "    return a;\n"
							   "}\n"
							   "main() {\n"
							   "    greet(1);\n"
							   "}\n";

// a raw image of w x h pixels at `bpp` bits: the header and a pattern
static uint8_t* RawImage(uint32_t w, uint32_t h, uint32_t bpp, uint32_t extra0, uint32_t extra1, uint32_t* size)
{
	uint32_t bytes = w * h * (bpp / 8), i;
	uint8_t* img = (uint8_t*)BGI_Calloc(16 + bytes);
	RawImageHeader_t ih;
	memset(&ih, 0, sizeof ih);
	ih.width = (uint16_t)w;
	ih.height = (uint16_t)h;
	ih.bpp = bpp;
	ih.unused08 = extra0;
	ih.unused0c = extra1;
	memcpy(img, &ih, 16);
	for(i = 0; i < bytes; i++)
		img[16 + i] = (uint8_t)(i * 37 + bpp);
	*size = 16 + bytes;
	return img;
}

/* a BW file of codec 3: the header and a minimal Ogg stream of two pages -
 * the Vorbis identification packet (stereo, 44100 Hz) and a last page whose
 * granule position, the stream's length, is 1000 */
static uint8_t* VorbisSound(uint32_t* size)
{
	uint8_t* snd = (uint8_t*)BGI_Calloc(0x40 + 58 + 33);
	uint8_t* p = snd + 0x40;
	BwHeader_t h;
	memset(&h, 0, sizeof h);
	h.headerSize = 0x40;
	memcpy(h.magic, "bw  ", 4);
	h.dataSize = 58 + 33;
	h.frames = 1000;
	h.rate = 44100;
	h.channels = 2;
	h.loopFlag = 1;
	h.loopStart = 500;
	h.codec = BW_CODEC_VORBIS;
	memcpy(snd, &h, 0x40);
	memcpy(p, "OggS", 4);
	p[5] = 2;              // the first page of the stream
	Put32(p + 14, 0x1234); // the stream's serial number
	p[26] = 1;             // one segment
	p[27] = 30;            // of 30 bytes: the identification packet
	p[28] = 1;
	memcpy(p + 29, "vorbis", 6);
	p[39] = 2; // channels
	Put32(p + 40, 44100);
	p += 58;
	memcpy(p, "OggS", 4);
	p[5] = 4; // the last page
	Put32(p + 6, 1000);
	Put32(p + 14, 0x1234);
	Put32(p + 18, 1);
	p[26] = 1;
	p[27] = 5;
	memcpy(p + 28, "audio", 5);
	*size = 0x40 + 58 + 33;
	return snd;
}

// the archives of the game into <root>/game
static void MakeGame(void)
{
	TestArc_t a;
	uint32_t size, i;
	uint8_t* data;
	memset(&a, 0, sizeof a);
	OS_NativeMkdir(Path("game"));

	{ // system.arc: the boot program, whose string names the game
		BpcResult_t r;
		Bpc_Compile(kProgram, strlen(kProgram), "ipl", GEN_1_69_444, NULL, stdout, &r);
		CHECK(r.errors == 0 && r.file != NULL);
		BGI_Free(r.labels);
		if(r.file)
			Add(&a, "ipl._bp", r.file, r.size, 1);
		BGI_Free(r.file);
		WriteArc(&a, "game/system.arc", 0);
	}
	{ // data01000.arc: a scenario file and the two tables
		ScnCompileResult_t r;
		uint8_t table[4 + 3 * 32], scenes[4 + 2 * 64];
		Scn_Compile(kScenario, strlen(kScenario), "main", NULL, NULL, stdout, &r);
		CHECK(r.errors == 0);
		data = Scn_Save(&r.file, &size);
		Scn_Free(&r.file);
		Add(&a, "main", data, size, 1);
		BGI_Free(data);
		memset(table, 0, sizeof table);
		Put32(table, 3);
		for(i = 0; i < 3; i++)
		{
			Put32(table + 4 + i * 32, 1000 + i);
			sprintf((char*)table + 4 + i * 32 + 4, "bg%02u_day", (unsigned)i);
		}
		Add(&a, "StringsDB", table, sizeof table, 1);
		memset(scenes, 0, sizeof scenes);
		Put32(scenes, 2);
		memcpy(scenes + 4, "\x83\x76\x83\x8d\x83\x8d\x81\x5b\x83\x4f", 10); // "Prologue" in katakana, Shift-JIS
		Put32(scenes + 4 + 48, 5);
		Put32(scenes + 4 + 60, 0xfffffffe);
		strcpy((char*)scenes + 4 + 64, "chapter1");
		Add(&a, "ScenarioDB", scenes, sizeof scenes, 0);
		WriteArc(&a, "game/data01000.arc", 0);
	}
	{ // sysgrp.arc: the images, a movie, an SDC block, plain files
		uint8_t* cbg;
		uint8_t frame[16 * 8 * 4];
		uint8_t blob[8 + 0x20 + 600], payload[500];
		BfMovieEnc_t* enc = BfMovieEnc_Begin(16, 8, 32, 30);
		uint32_t f;
		data = RawImage(5, 3, 32, 0, 0, &size);
		Add(&a, "pic", data, size, 1);
		BGI_Free(data);
		Add(&a, "pic.png", (const uint8_t*)"not a picture", 13, 0); // wants the file name the entry above gets
		data = RawImage(7, 5, 8, 0x10000, 0x00050003, &size);
		cbg = BGI_CbgEncode(data, 99, &size);
		Add(&a, "mask", cbg, size, 0);
		BGI_Free(cbg);
		BGI_Free(data);
		data = RawImage(4, 4, 24, 0, 0, &size);
		Add(&a, "bg24", data, size, 1);
		BGI_Free(data);
		for(f = 0; f < 3; f++)
		{
			for(i = 0; i < sizeof frame; i++)
				frame[i] = (uint8_t)(f == 0 || i / 64 == f ? i * 3 + f * 40 : frame[i]);
			BfMovieEnc_AddFrame(enc, frame, 16 * 4);
		}
		data = BfMovieEnc_Finish(enc, &size);
		Add(&a, "anim.bm", data, size, 0);
		BGI_Free(data);
		for(i = 0; i < sizeof payload; i++)
			payload[i] = (uint8_t)(i / 9);
		Put32(blob, 7);
		Put32(blob + 4, 0x1234);
		size = SdcEncode(blob + 8, payload, sizeof payload);
		CHECK(size != 0 && size <= sizeof blob - 8);
		Add(&a, "blob", blob, 8 + size, 0);
		Add(&a, "notes.txt", (const uint8_t*)"plain text\r\n", 12, 1);
		Add(&a, "\x83\x5e\x83\x86.dat", (const uint8_t*)"sjis name", 9, 0); // two katakana and an extension
		Add(&a, "empty", (const uint8_t*)"", 0, 0);
		WriteArc(&a, "game/sysgrp.arc", 0);
	}
	{ // data03000.arc, the newer index format: a Vorbis sound that loops and a PCM one
		uint8_t pcm[0x40 + 200];
		BwHeader_t h;
		data = VorbisSound(&size);
		Add(&a, "bgm_with_a_name_longer_than_fifteen", data, size, 0);
		BGI_Free(data);
		memset(&h, 0, sizeof h);
		h.headerSize = 0x40;
		memcpy(h.magic, "bw  ", 4);
		h.dataSize = 200;
		h.frames = 100;
		h.rate = 22050;
		h.channels = 1;
		h.codec = BW_CODEC_PCM;
		memcpy(pcm, &h, 0x40);
		for(i = 0; i < 100; i++)
		{
			int16_t v = (int16_t)(i * 300 - 15000);
			memcpy(pcm + 0x40 + i * 2, &v, 2);
		}
		Add(&a, "se_pcm", pcm, sizeof pcm, 0);
		WriteArc(&a, "game/data03000.arc", 1);
	}
	// data04000.arc: one file nobody edits; junk.arc: not an archive
	Add(&a, "voice.bin", (const uint8_t*)"\x01\x02\x03\x04\x05", 5, 0);
	WriteArc(&a, "game/data04000.arc", 1);
	CHECK(OS_WriteNativeFile(Path("game/junk.arc"), "this is not an archive at all", 29));
}

static const char* const kArchives[] = {"system.arc", "data01000.arc", "sysgrp.arc", "data03000.arc", "data04000.arc"};
#define ARCHIVES (sizeof kArchives / sizeof kArchives[0])

// the decoded picture of an image entry (RawImageHeader + pixels; BGI_Free it)
static uint8_t* DecodedImage(const char* arcRel, const char* name, uint32_t* size, int* cbgVersion)
{
	int dsc;
	uint8_t* data = Entry(arcRel, name, size, &dsc);
	*cbgVersion = 0;
	if(data && *size >= 0x30 && BGI_IsCbg(data))
	{
		uint32_t n = BGI_CbgOutSize(data);
		uint8_t* out = (uint8_t*)BGI_Calloc(n + 16);
		*cbgVersion = ((const CbgHeader_t*)data)->version;
		CHECK(BGI_CbgDecode(data, out) == 0);
		BGI_Free(data);
		*size = n;
		return out;
	}
	return data;
}

static void TestDump(void)
{
	DumpOptions_t opt;
	uint32_t n;
	char* text;
	FILE* log = fopen(Path("dump.log"), "w");
	memset(&opt, 0, sizeof opt);
	opt.gameDir = Keep("game");
	opt.outDir = Keep("dump");
	opt.jobs = 1;
	opt.log = log;
	CHECK(Dump_Run(&opt) == 0);
	// every entry as its kind
	CHECK(Exists("dump/bgidump.json"));
	CHECK(Exists("dump/system/ipl.bpc"));
	CHECK(Exists("dump/data01000/main.bsc") && Exists("dump/data01000/StringsDB.json") && Exists("dump/data01000/ScenarioDB.json"));
	CHECK(Exists("dump/sysgrp/pic.png") && Exists("dump/sysgrp/pic.png~2") && !Exists("dump/sysgrp/pic.json"));
	CHECK(Exists("dump/sysgrp/mask.png") && Exists("dump/sysgrp/mask.json") && Exists("dump/sysgrp/bg24.png"));
	CHECK(Exists("dump/sysgrp/anim.bm.json") && Exists("dump/sysgrp/anim.bm/0000.png") && Exists("dump/sysgrp/anim.bm/0002.png") &&
		!Exists("dump/sysgrp/anim.bm/0003.png"));
	CHECK(Exists("dump/sysgrp/blob.bin") && Exists("dump/sysgrp/blob.json") && Exists("dump/sysgrp/notes.txt") && Exists("dump/sysgrp/empty"));
	CHECK(Exists("dump/sysgrp/\xe3\x82\xbf\xe3\x83\xa6.dat")); // the Shift-JIS name in UTF-8
	CHECK(Exists("dump/data03000/bgm_with_a_name_longer_than_fifteen.ogg") && Exists("dump/data03000/bgm_with_a_name_longer_than_fifteen.json"));
	CHECK(Exists("dump/data03000/se_pcm.wav") && Exists("dump/data03000/se_pcm.json"));
	CHECK(Exists("dump/data04000/voice.bin") && !Exists("dump/junk"));
	CHECK(!Exists("dump/.bgidump-0.part") && !Exists("dump/.bgidump-0.tmp")); // the workers clean up

	text = (char*)ReadAll("dump/bgidump.json", &n);
	CHECK(text != NULL);
	if(text)
	{
		CHECK(strstr(text, "\"format\": \"openbgi-dump\"") != NULL);
		CHECK(strstr(text, "\"product\": \"Tayutama\"") != NULL && strstr(text, "\"profile\": \"1.69/4") != NULL);
		CHECK(strstr(text, "\"skipped\": [\"junk.arc\"]") != NULL);
		CHECK(strstr(text, "\"name\": \"ipl._bp\", \"kind\": \"program\"") != NULL);
		CHECK(strstr(text, "\"name\": \"main\", \"kind\": \"scenario\"") != NULL);
		CHECK(strstr(text, "\"name\": \"StringsDB\", \"kind\": \"table\"") != NULL && strstr(text, "\"name\": \"ScenarioDB\", \"kind\": \"table\"") != NULL);
		CHECK(strstr(text, "\"name\": \"pic\", \"kind\": \"image\"") != NULL && strstr(text, "\"codec\": \"raw\", \"bpp\": 32") != NULL);
		CHECK(strstr(text, "\"name\": \"pic.png\", \"kind\": \"data\"") != NULL);
		CHECK(strstr(text, "\"codec\": \"cbg1\", \"bpp\": 8") != NULL);
		CHECK(strstr(text, "\"name\": \"anim.bm\", \"kind\": \"movie\"") != NULL && strstr(text, "\"name\": \"blob\", \"kind\": \"sdc\"") != NULL);
		CHECK(strstr(text, "\"kind\": \"sound\"") != NULL && strstr(text, "\"codec\": \"vorbis\"") != NULL && strstr(text, "\"codec\": \"pcm\"") != NULL);
		CHECK(strstr(text, "\"roundtrip\": false") == NULL && strstr(text, "\"blocks\"") == NULL);
		CHECK(strstr(text, "\"type\": \"BURIKO ARC20\"") != NULL && strstr(text, "\"type\": \"PackFile\"") != NULL);
		free(text);
	}
	// what the converted files hold
	text = (char*)ReadAll("dump/system/ipl.bpc", &n);
	CHECK(text && strstr(text, "debug_msg(\"Tayutama\")") != NULL && strstr(text, "#engine 1.69/4") != NULL);
	free(text);
	text = (char*)ReadAll("dump/data01000/main.bsc", &n);
	CHECK(text && strstr(text, "\"hello\"") != NULL && strstr(text, "greet(") != NULL);
	free(text);
	text = (char*)ReadAll("dump/data01000/StringsDB.json", &n);
	CHECK(text && strstr(text, "{\"name\": \"bg01_day\", \"value\": 1001}") != NULL && strstr(text, "\"record_size\": 32") != NULL);
	free(text);
	text = (char*)ReadAll("dump/data01000/ScenarioDB.json", &n);
	CHECK(text && strstr(text, "\xe3\x83\x97\xe3\x83\xad\xe3\x83\xad\xe3\x83\xbc\xe3\x82\xb0") != NULL && strstr(text, "\"values\": [5, 0, 0, 4294967294]") != NULL);
	free(text);
	text = (char*)ReadAll("dump/sysgrp/mask.json", &n);
	CHECK(text && strstr(text, "\"extra\": [65536, 327683]") != NULL);
	free(text);
	text = (char*)ReadAll("dump/sysgrp/blob.json", &n);
	CHECK(text && strstr(text, "\"prefix\": \"0700000034120000\"") != NULL);
	free(text);
	text = (char*)ReadAll("dump/sysgrp/anim.bm.json", &n);
	CHECK(text && strstr(text, "\"fps\": 30") != NULL && strstr(text, "\"bpp\": 32") != NULL && strstr(text, "\"width\": 16") != NULL);
	free(text);
	text = (char*)ReadAll("dump/data03000/bgm_with_a_name_longer_than_fifteen.json", &n);
	CHECK(text && strstr(text, "\"loop\": true") != NULL && strstr(text, "\"loop_start\": 500") != NULL && strstr(text, "frames") == NULL);
	free(text);
	{ // the .ogg is the stream behind the header; the .wav 100 frames of 16-bit mono
		uint8_t* f = ReadAll("dump/data03000/bgm_with_a_name_longer_than_fifteen.ogg", &n);
		CHECK(f && n == 58 + 33 && memcmp(f, "OggS", 4) == 0);
		free(f);
		f = ReadAll("dump/data03000/se_pcm.wav", &n);
		CHECK(f && n == 44 + 200 && memcmp(f, "RIFF", 4) == 0 && Get32(f + 24) == 22050 && f[22] == 1 && f[34] == 16);
		free(f);
		f = ReadAll("dump/sysgrp/blob.bin", &n);
		CHECK(f && n == 500 && f[499] == 499 / 9);
		free(f);
	}
	{ // the pictures: size and channels as stored
		PngImage_t img;
		char err[64];
		uint8_t* f = ReadAll("dump/sysgrp/pic.png", &n);
		CHECK(f && Png_Decode(f, n, &img, err, sizeof err) && img.width == 5 && img.height == 3 && img.channels == 4 && img.pixels[1] == (uint8_t)(37 + 32));
		free(img.pixels);
		free(f);
		f = ReadAll("dump/sysgrp/mask.png", &n);
		CHECK(f && Png_Decode(f, n, &img, err, sizeof err) && img.width == 7 && img.height == 5 && img.channels == 1);
		free(img.pixels);
		free(f);
		f = ReadAll("dump/sysgrp/anim.bm/0001.png", &n);
		CHECK(f && Png_Decode(f, n, &img, err, sizeof err) && img.width == 16 && img.height == 8 && img.channels == 4);
		free(img.pixels);
		free(f);
	}
	// a dump is not written over, unless told to
	CHECK(Dump_Run(&opt) == 2);
	opt.force = 1;
	CHECK(Dump_Run(&opt) == 0);
	// no archives, an unknown profile
	opt.gameDir = Keep("dump");
	opt.outDir = Keep("dump_none");
	CHECK(Dump_Run(&opt) == 2);
	opt.gameDir = Keep("game");
	opt.engine = "9.99";
	CHECK(Dump_Run(&opt) == 2);
	fclose(log);
}

static void TestPack(void)
{
	PackOptions_t opt;
	FILE* log = fopen(Path("pack.log"), "w");
	char a[64], b[64];
	uint32_t i, n, m;
	int dsc, version;
	uint8_t *x, *y;
	memset(&opt, 0, sizeof opt);
	opt.dumpDir = Keep("dump");
	opt.outDir = Keep("pack");
	opt.log = log;
	// untouched: the archives come back byte for byte
	CHECK(Pack_Run(&opt) == 0);
	for(i = 0; i < ARCHIVES; i++)
	{
		snprintf(a, sizeof a, "game/%s", kArchives[i]);
		snprintf(b, sizeof b, "pack/%s", kArchives[i]);
		CHECK(SameFile(a, b));
	}
	CHECK(!Exists("pack/junk.arc"));
	// and nothing is written when only changed archives are wanted
	opt.outDir = Keep("pack_changed");
	opt.changedOnly = 1;
	CHECK(Pack_Run(&opt) == 0);
	CHECK(!Exists("pack_changed/system.arc") && !Exists("pack_changed/sysgrp.arc"));
	opt.changedOnly = 0;
	// the output may not be the directory of the originals
	opt.outDir = Keep("game");
	CHECK(Pack_Run(&opt) == 2);
	CHECK(SameFile("game/system.arc", "pack/system.arc"));
	// not a dump
	opt.dumpDir = Keep("game");
	opt.outDir = Keep("pack_none");
	CHECK(Pack_Run(&opt) == 2);

	// everything from the files alone: the same content, entry by entry
	opt.dumpDir = Keep("dump");
	opt.outDir = Keep("pack_all");
	opt.rebuildAll = 1;
	opt.sourceDir = Keep("nowhere");
	CHECK(Pack_Run(&opt) == 0);
	CHECK(SameFile("game/data03000.arc", "pack_all/data03000.arc")); // the sounds' headers are made again exactly
	CHECK(SameFile("game/data04000.arc", "pack_all/data04000.arc"));
	{
		static const char* const same[][2] = {{"system.arc", "ipl._bp"}, {"data01000.arc", "main"}, {"data01000.arc", "StringsDB"},
			{"data01000.arc", "ScenarioDB"}, {"sysgrp.arc", "pic.png"}, {"sysgrp.arc", "notes.txt"}, {"sysgrp.arc", "\x83\x5e\x83\x86.dat"},
			{"sysgrp.arc", "empty"}};
		static const char* const images[] = {"pic", "mask", "bg24"};
		for(i = 0; i < sizeof same / sizeof same[0]; i++)
		{
			int dsc2;
			snprintf(a, sizeof a, "game/%s", same[i][0]);
			snprintf(b, sizeof b, "pack_all/%s", same[i][0]);
			x = Entry(a, same[i][1], &n, &dsc);
			y = Entry(b, same[i][1], &m, &dsc2);
			CHECK(x && y && n == m && memcmp(x, y, n) == 0 && dsc == dsc2);
			BGI_Free(x);
			BGI_Free(y);
		}
		for(i = 0; i < sizeof images / sizeof images[0]; i++)
		{
			int v2;
			x = DecodedImage("game/sysgrp.arc", images[i], &n, &version);
			y = DecodedImage("pack_all/sysgrp.arc", images[i], &m, &v2);
			CHECK(x && y && n == m && memcmp(x, y, n) == 0 && version == v2);
			BGI_Free(x);
			BGI_Free(y);
		}
	}
	{ // the SDC block decodes to the same bytes behind the same prefix; the movie to the same frames
		uint8_t out[600], slotA[16 * 8 * 4], slotB[16 * 8 * 4];
		uint32_t f;
		y = Entry("pack_all/sysgrp.arc", "blob", &m, &dsc);
		CHECK(y && Get32(y) == 7 && Get32(y + 4) == 0x1234 && SdcIsFormat(y + 8) && SdcDecode(out, y + 8) == 500 && out[499] == 499 / 9);
		BGI_Free(y);
		x = Entry("game/sysgrp.arc", "anim.bm", &n, &dsc);
		y = Entry("pack_all/sysgrp.arc", "anim.bm", &m, &dsc);
		CHECK(x && y && BfMovie_Check(y, m) && ((const BfMovieHeader_t*)y)->frames == 3 && ((const BfMovieHeader_t*)y)->fps == 30);
		memset(slotA, 0, sizeof slotA);
		memset(slotB, 0, sizeof slotB);
		for(f = 0; x && y && f < 3; f++)
			CHECK(BfMovie_DecodeFrame(x, n, f, slotA, 64) && BfMovie_DecodeFrame(y, m, f, slotB, 64) && memcmp(slotA, slotB, sizeof slotA) == 0);
		BGI_Free(x);
		BGI_Free(y);
	}
	fclose(log);
}

static void TestEdit(void)
{
	PackOptions_t opt;
	FILE* log = fopen(Path("edit.log"), "w");
	struct timespec pause = {0, 30 * 1000 * 1000};
	uint32_t n, m, i;
	int dsc, version;
	uint8_t *x, *y;
	nanosleep(&pause, NULL); // the edits are later than the dump by more than the clock's step
	{                        // a new picture of another size, and the grey mask given in colour
		uint8_t pic[6 * 2 * 4], mask[7 * 5 * 3];
		size_t size;
		uint8_t* png;
		for(i = 0; i < sizeof pic; i++)
			pic[i] = (uint8_t)(200 - i);
		png = Png_Encode(pic, 6, 2, 4, 24, 6, &size);
		CHECK(OS_WriteNativeFile(Path("dump/sysgrp/pic.png"), png, (uint32_t)size));
		free(png);
		for(i = 0; i < 7 * 5; i++)
		{
			mask[i * 3] = (uint8_t)(i * 5);
			mask[i * 3 + 1] = (uint8_t)(i == 3 ? 0 : i * 5); // one pixel that is not grey
			mask[i * 3 + 2] = (uint8_t)(i * 5);
		}
		png = Png_Encode(mask, 7, 5, 3, 21, 6, &size);
		CHECK(OS_WriteNativeFile(Path("dump/sysgrp/mask.png"), png, (uint32_t)size));
		free(png);
		for(i = 0; i < 16 * 8 * 4; i++)
			pic[i % sizeof pic] = 0;
	}
	{ // a frame of the movie
		uint8_t frame[16 * 8 * 4];
		size_t size;
		uint8_t* png;
		memset(frame, 0x33, sizeof frame);
		png = Png_Encode(frame, 16, 8, 4, 64, 6, &size);
		CHECK(OS_WriteNativeFile(Path("dump/sysgrp/anim.bm/0001.png"), png, (uint32_t)size));
		free(png);
	}
	CHECK(EditText("dump/data01000/main.bsc", "\"hello\"", "\"HELLO, WORLD\""));
	CHECK(EditText("dump/system/ipl.bpc", "\"Tayutama\"", "\"Tayutama in English\""));
	CHECK(EditText("dump/data03000/bgm_with_a_name_longer_than_fifteen.json", "500", "777"));
	CHECK(EditText("dump/data01000/StringsDB.json", "bg01_day", "bg01_night"));

	memset(&opt, 0, sizeof opt);
	opt.dumpDir = Keep("dump");
	opt.outDir = Keep("edit");
	opt.changedOnly = 1;
	opt.log = log;
	CHECK(Pack_Run(&opt) == 0);
	CHECK(Exists("edit/system.arc") && Exists("edit/data01000.arc") && Exists("edit/sysgrp.arc") && Exists("edit/data03000.arc"));
	CHECK(!Exists("edit/data04000.arc")); // nothing of it changed

	// the changes are in
	x = Entry("edit/system.arc", "ipl._bp", &n, &dsc);
	CHECK(x && dsc && Holds(x, n, "Tayutama in English"));
	BGI_Free(x);
	x = Entry("edit/data01000.arc", "main", &n, &dsc);
	CHECK(x && dsc && memcmp(x, SCN_MAGIC, 27) == 0 && Holds(x, n, "HELLO, WORLD") && !Holds(x, n, "hello"));
	BGI_Free(x);
	x = Entry("edit/data01000.arc", "StringsDB", &n, &dsc);
	CHECK(x && dsc && n == 4 + 3 * 32 && strcmp((const char*)x + 4 + 32 + 4, "bg01_night") == 0 && Get32(x + 4 + 32) == 1001);
	BGI_Free(x);
	x = Entry("edit/data03000.arc", "bgm_with_a_name_longer_than_fifteen", &n, &dsc);
	CHECK(x && n == 0x40 + 91 && ((const BwHeader_t*)x)->loopStart == 777 && ((const BwHeader_t*)x)->loopFlag == 1 && ((const BwHeader_t*)x)->frames == 1000);
	BGI_Free(x);
	x = DecodedImage("edit/sysgrp.arc", "pic", &n, &version);
	CHECK(x && version == 0 && n == 16 + 6 * 2 * 4 && x[0] == 6 && x[2] == 2 && x[4] == 32 && x[16] == 200 && x[16 + 47] == 200 - 47);
	BGI_Free(x);
	x = DecodedImage("edit/sysgrp.arc", "mask", &n, &version);
	// still a CompressedBG of 8 bits with its extra words; the coloured pixel became its luma
	CHECK(x && version == 1 && n == 16 + 35 && x[4] == 8 && Get32(x + 8) == 0x10000 && Get32(x + 12) == 0x00050003);
	CHECK(x && x[16 + 2] == 10 && x[16 + 3] == (uint8_t)((28 * 15 + 77 * 15) >> 8) && x[16 + 34] == 170);
	BGI_Free(x);
	{
		uint8_t slot[16 * 8 * 4], first[16 * 8 * 4];
		x = Entry("edit/sysgrp.arc", "anim.bm", &n, &dsc);
		y = Entry("game/sysgrp.arc", "anim.bm", &m, &dsc);
		memset(slot, 0, sizeof slot);
		memset(first, 0, sizeof first);
		CHECK(x && y && BfMovie_DecodeFrame(x, n, 0, slot, 64) && BfMovie_DecodeFrame(y, m, 0, first, 64) && memcmp(slot, first, sizeof slot) == 0);
		CHECK(x && BfMovie_DecodeFrame(x, n, 1, slot, 64) && slot[0] == 0x33 && slot[sizeof slot - 1] == 0x33);
		BGI_Free(x);
		BGI_Free(y);
	}
	{ // every entry that was not touched is the original's bytes, stored as it was
		static const char* const kept[][2] = {{"data01000.arc", "ScenarioDB"}, {"sysgrp.arc", "pic.png"}, {"sysgrp.arc", "bg24"},
			{"sysgrp.arc", "blob"}, {"sysgrp.arc", "notes.txt"}, {"data03000.arc", "se_pcm"}};
		for(i = 0; i < sizeof kept / sizeof kept[0]; i++)
		{
			char a[64], b[64];
			ArcRead_t *p, *q;
			const ArcEntry_t *e, *g;
			snprintf(a, sizeof a, "game/%s", kept[i][0]);
			snprintf(b, sizeof b, "edit/%s", kept[i][0]);
			p = ArcRead_Open(Path(a));
			q = ArcRead_Open(Path(b));
			e = p ? ArcRead_Find(p, kept[i][1]) : NULL;
			g = q ? ArcRead_Find(q, kept[i][1]) : NULL;
			CHECK(e && g && e->size == g->size);
			if(e && g)
			{
				x = (uint8_t*)malloc(e->size + 1);
				y = (uint8_t*)malloc(e->size + 1);
				fseek(p->f, (long)e->offset, SEEK_SET);
				fseek(q->f, (long)g->offset, SEEK_SET);
				CHECK(fread(x, 1, e->size, p->f) == e->size && fread(y, 1, e->size, q->f) == e->size && memcmp(x, y, e->size) == 0);
				free(x);
				free(y);
			}
			if(p)
				ArcRead_Close(p);
			if(q)
				ArcRead_Close(q);
		}
	}

	// a source that does not compile: the archive is written with the original entry, and the status says so
	CHECK(EditText("dump/data01000/main.bsc", "return l4;", "return l4 +;"));
	opt.outDir = Keep("edit_bad");
	CHECK(Pack_Run(&opt) == 1);
	CHECK(SameFile("edit_bad/data01000.arc", "edit/data01000.arc") == 0);
	x = Entry("edit_bad/data01000.arc", "main", &n, &dsc);
	y = Entry("game/data01000.arc", "main", &m, &dsc);
	CHECK(x && y && n == m && memcmp(x, y, n) == 0);
	BGI_Free(x);
	BGI_Free(y);
	// without an original to fall back on there is no archive
	opt.outDir = Keep("edit_bad2");
	opt.rebuildAll = 1;
	opt.changedOnly = 0;
	CHECK(Pack_Run(&opt) == 2);
	CHECK(!Exists("edit_bad2/data01000.arc") && Exists("edit_bad2/sysgrp.arc"));
	fclose(log);
}

int main(void)
{
	snprintf(gRoot, sizeof gRoot, "/tmp/bgi_dump_%ld", (long)getpid());
	OS_Init();
	if(!OS_NativeMkdir(gRoot))
		return 1;
	TestDeflate();
	TestPng();
	TestEncoders();
	MakeGame();
	TestDump();
	TestPack();
	TestEdit();
	if(gFails == 0)
		RemoveTree(gRoot);
	else
		printf("  the scratch directory %s is kept\n", gRoot);
	printf("%s\n", gFails ? "FAILED" : "ok");
	return gFails != 0;
}
