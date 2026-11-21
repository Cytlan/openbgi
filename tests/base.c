/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/base.c - src/base/ on its own; built as test_base by `make test`
 *
 * This program is linked with the files of src/base/ and nothing else of
 * OpenBGI: the link fails when one of them calls for a function of the
 * engine.  (That they include none of its headers is `make check-core`'s
 * to say, tools/checkcore.py.)  What it runs is one thing of each kind,
 * to see the parts work without an engine around them; the tests of the
 * parts themselves are sjis.c, codec.c, picture.c, bidi.c, bfmovie.c and
 * fonts.c:
 *
 *   text     - Shift-JIS to UTF-8 and back, a right-to-left run reordered
 *   json     - an object parsed and read
 *   deflate  - a zlib stream made and read back
 *   pictures - a picture as a PNG file and as a CompressedBG file, each
 *              decoded again
 *   movie    - a two-frame BF_Movie file made and its second frame read
 */
#include "bgi/bfmovie.h"
#include "bgi/bidi.h"
#include "bgi/codec.h"
#include "bgi/deflate.h"
#include "bgi/fontfile.h"
#include "bgi/json.h"
#include "bgi/picture.h"
#include "bgi/png.h"
#include "bgi/sjis.h"

#include "check.h"

enum
{
	W = 24,
	H = 10
};

// a picture with something in every channel, 4 bytes a pixel
static void Paint(uint8_t* p, int seed)
{
	for(int y = 0; y < H; ++y)
	{
		for(int x = 0; x < W; ++x)
		{
			uint8_t* q = p + (y * W + x) * 4;
			q[0] = (uint8_t)(x * 9 + seed);
			q[1] = (uint8_t)(y * 23);
			q[2] = (uint8_t)((x ^ y) * 5 + seed);
			q[3] = 0xff;
		}
	}
}

static void TestText(void)
{
	static const char kSjis[] = "\x82\xa0\x83\x41"; // hiragana A, katakana A
	char utf8[16], back[16];
	CHECK(Sjis_ToUtf8(kSjis, utf8, sizeof utf8, 0) == 6);
	CHECK(Sjis_FromUtf8(utf8, back, sizeof back) == 4 && strcmp(back, kSjis) == 0);

	// two Hebrew letters between Latin ones come out in the other order
	static const uint32_t kLine[4] = {'a', 0x5d0, 0x5d1, 'b'};
	uint8_t levels[4];
	int order[4];
	Bidi_Levels(kLine, 4, 0, levels);
	Bidi_Reorder(kLine, levels, 4, 0, order);
	CHECK(order[0] == 0 && order[1] == 2 && order[2] == 1 && order[3] == 3);
}

static void TestJson(void)
{
	static const char kText[] = "{\"name\": \"base\", \"n\": 3, \"list\": [1, 2]}";
	size_t err = 0;
	JsonNode_t* j = Json_Parse(kText, sizeof kText - 1, &err);
	CHECK(j != NULL);
	if(j)
	{
		CHECK(strcmp(Json_String(j, "name", ""), "base") == 0);
		CHECK(Json_Number(j, "n", 0) == 3);
		CHECK(Json_Count(Json_Get(j, "list")) == 2);
		Json_Free(j);
	}
}

static void TestDeflate(void)
{
	uint8_t data[4096];
	for(size_t i = 0; i < sizeof data; ++i)
		data[i] = (uint8_t)(i % 97 + i / 512);
	size_t packed = 0, size = 0;
	uint8_t* z = Deflate_Compress(data, sizeof data, 6, &packed);
	CHECK(z != NULL && packed < sizeof data);
	uint8_t* back = z ? Deflate_Uncompress(z, packed, sizeof data, &size) : NULL;
	CHECK(back != NULL && size == sizeof data && memcmp(back, data, size) == 0);
	free(back);
	free(z);
}

static void TestPictures(void)
{
	uint8_t pixels[W * H * 4];
	Paint(pixels, 1);

	// a PNG file, read back by the decoder of every picture format
	size_t size = 0;
	uint8_t* png = Png_Encode(pixels, W, H, 4, W * 4, 6, &size);
	CHECK(png != NULL);
	Picture_t pic;
	memset(&pic, 0, sizeof pic);
	if(png && Picture_Decode(png, size, &pic))
	{
		CHECK(pic.width == W && pic.height == H && memcmp(pic.bgra, pixels, sizeof pixels) == 0);
		Picture_Free(&pic);
	}
	else
	{
		CHECK(!"the PNG file decodes");
	}
	free(png);

	// a CompressedBG file: an image is its header and the pixels
	uint8_t image[sizeof(RawImageHeader_t) + W * H * 4];
	RawImageHeader_t head;
	memset(&head, 0, sizeof head);
	head.width = W;
	head.height = H;
	head.bpp = 32;
	memcpy(image, &head, sizeof head);
	memcpy(image + sizeof head, pixels, sizeof pixels);
	uint32_t cbgSize = 0;
	uint8_t* cbg = (uint8_t*)BGI_CbgEncode(image, 0x1234, &cbgSize);
	CHECK(cbg != NULL && BGI_IsCbg(cbg));
	if(cbg)
	{
		uint32_t n = BGI_CbgOutSize(cbg);
		uint8_t* out = (uint8_t*)BGI_Alloc(n);
		CHECK(BGI_CbgDecode(cbg, out) == 0);
		CHECK(n >= sizeof pixels && memcmp(out + (n - sizeof pixels), pixels, sizeof pixels) == 0);
		BGI_Free(out);
		BGI_Free(cbg);
	}
}

static void TestMovie(void)
{
	uint8_t a[W * H * 4], b[W * H * 4], out[W * H * 4];
	Paint(a, 1);
	Paint(b, 77);
	BfMovieEnc_t* e = BfMovieEnc_Begin(W, H, 32, 30);
	CHECK(e != NULL);
	if(!e)
		return;
	BfMovieEnc_AddFrame(e, a, W * 4);
	BfMovieEnc_AddFrame(e, b, W * 4);
	uint32_t size = 0;
	uint8_t* file = BfMovieEnc_Finish(e, &size);
	CHECK(file != NULL && BfMovie_Check(file, size) && BfMovie_IsSimple(file));
	if(file)
	{
		memset(out, 0, sizeof out);
		CHECK(BfMovie_DecodeFrame(file, size, 0, out, W * 4)); // (a frame is decoded over the one before it)
		CHECK(BfMovie_DecodeFrame(file, size, 1, out, W * 4));
		CHECK(memcmp(out, b, sizeof b) == 0);
		BGI_Free(file);
	}
}

int main(void)
{
	TestText();
	TestJson();
	TestDeflate();
	TestPictures();
	TestMovie();
	// (a file that is no font has no faces)
	CHECK(FontFile_Faces((const uint8_t*)"none", 4) == 0);
	if(gFails)
	{
		printf("base: %d checks failed\n", gFails);
		return 1;
	}
	printf("base: all tests passed\n");
	return 0;
}
