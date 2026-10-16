/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * png.c - the PNG reader and writer of the tools; interface in png.h
 *
 * A PNG file is an eight-byte signature and a list of chunks (length,
 * type, data, CRC-32 of type and data): IHDR describes the picture, PLTE
 * and tRNS a palette and its (or a colour key's) transparency, the IDAT
 * chunks together are one zlib stream of the scanlines - each with a
 * filter byte in front, the bytes being differences to their neighbours
 * as the filter says - and IEND ends the file.
 *
 * The writer converts the rows to red-green-blue order, filters each one
 * with the filter whose output has the smallest sum of magnitudes and
 * compresses the lot.  The reader inflates the stream, undoes the filters
 * (pass by pass for an interlaced file) and expands every pixel to four
 * 8-bit channels before it settles on the channel count to hand out.
 */
#include "bgi/png.h"
#include "bgi/deflate.h"

static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

static uint32_t Be32(const uint8_t* p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void PutBe32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

// the predictor of filter 4: whichever of left, up and upper-left is closest to left + up - upper-left
static int Paeth(int a, int b, int c)
{
	int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	return pa <= pb && pa <= pc ? a : pb <= pc ? b
											   : c;
}

// ---- writing -------------------------------------------------------------------------------------

// append a chunk at `p`; returns the bytes written (12 + len)
static size_t PutChunk(uint8_t* p, const char* type, const uint8_t* data, size_t len)
{
	PutBe32(p, (uint32_t)len);
	memcpy(p + 4, type, 4);
	if(len)
		memcpy(p + 8, data, len);
	PutBe32(p + 8 + len, BGI_Crc32(0, p + 4, len + 4));
	return len + 12;
}

/* filter the row `cur` (its predecessor `prev`, `bpp` bytes per pixel) with
 * filter `type` into `out`; returns the sum of the magnitudes of the output
 * bytes taken as signed, the measure the filter is chosen by */
static uint32_t FilterRow(uint8_t* out, const uint8_t* cur, const uint8_t* prev, size_t n, size_t bpp, int type)
{
	uint32_t sum = 0;
	size_t i;
	for(i = 0; i < n; i++)
	{
		int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, pred;
		uint8_t v;
		switch(type)
		{
			case 0: pred = 0; break;
			case 1: pred = a; break;
			case 2: pred = b; break;
			case 3: pred = (a + b) >> 1; break;
			default: pred = Paeth(a, b, c); break;
		}
		v = (uint8_t)(cur[i] - pred);
		out[i] = v;
		sum += v < 128 ? v : 256u - v;
	}
	return sum;
}

uint8_t* Png_Encode(const uint8_t* pixels, uint32_t width, uint32_t height, int channels, size_t pitch, int level, size_t* size)
{
	size_t row = (size_t)width * (size_t)channels, rawSize = (row + 1) * height, zSize = 0, o;
	uint8_t *raw, *cur, *prev, *trial, *z, *file;
	uint8_t ihdr[13];
	uint32_t y;
	if((channels != 1 && channels != 3 && channels != 4) || !width || !height)
		return NULL;
	raw = (uint8_t*)malloc(rawSize);
	cur = (uint8_t*)malloc(row * 3);
	if(!raw || !cur)
	{
		free(raw);
		free(cur);
		return NULL;
	}
	prev = cur + row;
	trial = prev + row;
	memset(prev, 0, row);
	for(y = 0; y < height; y++)
	{
		const uint8_t* src = pixels + (size_t)y * pitch;
		uint8_t* dst = raw + (size_t)y * (row + 1);
		uint32_t best = 0xffffffffu;
		size_t x;
		int f, bestType = 0;
		if(channels == 1)
			memcpy(cur, src, row);
		else
			for(x = 0; x < width; x++)
			{ // blue and red change places
				const uint8_t* s = src + x * (size_t)channels;
				uint8_t* d = cur + x * (size_t)channels;
				d[0] = s[2];
				d[1] = s[1];
				d[2] = s[0];
				if(channels == 4)
					d[3] = s[3];
			}
		for(f = 0; f < 5; f++)
		{
			uint32_t sum = FilterRow(trial, cur, prev, row, (size_t)channels, f);
			if(sum < best)
			{
				best = sum;
				bestType = f;
				memcpy(dst + 1, trial, row);
			}
		}
		dst[0] = (uint8_t)bestType;
		{ // the row becomes the next one's predecessor
			uint8_t* t = cur;
			cur = prev;
			prev = t;
		}
	}
	free(cur < prev ? cur : prev);
	z = Deflate_Compress(raw, rawSize, level, &zSize);
	free(raw);
	if(!z)
		return NULL;
	file = (uint8_t*)malloc(8 + (13 + 12) + (zSize + 12) + 12);
	if(!file)
	{
		free(z);
		return NULL;
	}
	memcpy(file, kSignature, 8);
	o = 8;
	PutBe32(ihdr, width);
	PutBe32(ihdr + 4, height);
	ihdr[8] = 8; // bits per sample
	ihdr[9] = (uint8_t)(channels == 1 ? 0 : channels == 3 ? 2
														  : 6); // greyscale, RGB, RGBA
	ihdr[10] = ihdr[11] = ihdr[12] = 0;                         // deflate, the five filters, no interlacing
	o += PutChunk(file + o, "IHDR", ihdr, 13);
	o += PutChunk(file + o, "IDAT", z, zSize);
	o += PutChunk(file + o, "IEND", NULL, 0);
	free(z);
	*size = o;
	return file;
}

// ---- reading -------------------------------------------------------------------------------------

static int Fail(char* err, size_t errSize, const char* msg)
{
	if(err && errSize)
		snprintf(err, errSize, "%s", msg);
	return 0;
}

typedef struct PngInfo
{
	uint32_t width, height;
	int depth;               // bits per sample: 1, 2, 4, 8, 16
	int colour;              // 0 grey, 2 RGB, 3 palette, 4 grey + alpha, 6 RGBA
	int interlace;           // 1: Adam7
	uint8_t palette[256][4]; // R G B A
	int paletteCount;
	int hasTrns;     // a tRNS chunk: palette alphas, or the colour key below
	uint16_t key[3]; // the transparent sample values of colour types 0 and 2
} PngInfo_t;

// samples per pixel of a colour type
static int Samples(int colour)
{
	return colour == 0 || colour == 3 ? 1 : colour == 2 ? 3
		: colour == 4                                   ? 2
														: 4;
}

// undo the filter of one scanline in place (`row` bytes after the filter byte `type`); 0 for an unknown filter
static int Unfilter(uint8_t* cur, const uint8_t* prev, size_t row, size_t bpp, int type)
{
	size_t i;
	switch(type)
	{
		case 0: break;
		case 1:
			for(i = bpp; i < row; i++)
				cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
			break;
		case 2:
			for(i = 0; i < row; i++)
				cur[i] = (uint8_t)(cur[i] + prev[i]);
			break;
		case 3:
			for(i = 0; i < row; i++)
				cur[i] = (uint8_t)(cur[i] + (((i >= bpp ? cur[i - bpp] : 0) + prev[i]) >> 1));
			break;
		case 4:
			for(i = 0; i < row; i++)
				cur[i] = (uint8_t)(cur[i] + Paeth(i >= bpp ? cur[i - bpp] : 0, prev[i], i >= bpp ? prev[i - bpp] : 0));
			break;
		default: return 0;
	}
	return 1;
}

// sample `i` of a scanline of `depth`-bit samples, as stored (16-bit ones whole)
static uint32_t Sample(const uint8_t* row, size_t i, int depth)
{
	switch(depth)
	{
		case 16: return ((uint32_t)row[i * 2] << 8) | row[i * 2 + 1];
		case 8: return row[i];
		case 4: return (row[i >> 1] >> ((1 - (i & 1)) * 4)) & 15u;
		case 2: return (row[i >> 2] >> ((3 - (i & 3)) * 2)) & 3u;
		default: return (row[i >> 3] >> (7 - (i & 7))) & 1u;
	}
}

// a sample scaled to eight bits
static uint8_t To8(uint32_t v, int depth)
{
	switch(depth)
	{
		case 16: return (uint8_t)(v >> 8);
		case 8: return (uint8_t)v;
		case 4: return (uint8_t)(v * 17);
		case 2: return (uint8_t)(v * 85);
		default: return (uint8_t)(v * 255);
	}
}

/* expand the `count` pixels of an unfiltered scanline into R G B A bytes at
 * dst, `step` pixels apart (the columns of an interlace pass) */
static void ExpandRow(uint8_t* dst, size_t step, const uint8_t* row, uint32_t count, const PngInfo_t* in)
{
	uint32_t x;
	for(x = 0; x < count; x++, dst += step * 4)
	{
		uint32_t s0, s1, s2;
		switch(in->colour)
		{
			case 0:
				s0 = Sample(row, x, in->depth);
				dst[0] = dst[1] = dst[2] = To8(s0, in->depth);
				dst[3] = (uint8_t)(in->hasTrns && s0 == in->key[0] ? 0 : 255);
				break;
			case 2:
				s0 = Sample(row, (size_t)x * 3, in->depth);
				s1 = Sample(row, (size_t)x * 3 + 1, in->depth);
				s2 = Sample(row, (size_t)x * 3 + 2, in->depth);
				dst[0] = To8(s0, in->depth);
				dst[1] = To8(s1, in->depth);
				dst[2] = To8(s2, in->depth);
				dst[3] = (uint8_t)(in->hasTrns && s0 == in->key[0] && s1 == in->key[1] && s2 == in->key[2] ? 0 : 255);
				break;
			case 3:
				s0 = Sample(row, x, in->depth);
				if((int)s0 < in->paletteCount)
					memcpy(dst, in->palette[s0], 4);
				else
				{ // an index beyond the palette: black
					dst[0] = dst[1] = dst[2] = 0;
					dst[3] = 255;
				}
				break;
			case 4:
				dst[0] = dst[1] = dst[2] = To8(Sample(row, (size_t)x * 2, in->depth), in->depth);
				dst[3] = To8(Sample(row, (size_t)x * 2 + 1, in->depth), in->depth);
				break;
			default:
				dst[0] = To8(Sample(row, (size_t)x * 4, in->depth), in->depth);
				dst[1] = To8(Sample(row, (size_t)x * 4 + 1, in->depth), in->depth);
				dst[2] = To8(Sample(row, (size_t)x * 4 + 2, in->depth), in->depth);
				dst[3] = To8(Sample(row, (size_t)x * 4 + 3, in->depth), in->depth);
				break;
		}
	}
}

int Png_Decode(const uint8_t* data, size_t size, PngImage_t* out, char* err, size_t errSize)
{
	// the Adam7 passes: first column and row, column and row step
	static const uint8_t kPass[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
	PngInfo_t in;
	uint8_t *idat = NULL, *raw = NULL, *rgba = NULL, *prev = NULL;
	size_t idatLen = 0, idatCap = 0, rawLen = 0, pos = 8, off = 0, bpp, expect = 0, i, count;
	int haveHeader = 0, done = 0, pass, passes, ok = 0, bits;
	const char* msg = "damaged PNG file";

	memset(out, 0, sizeof *out);
	memset(&in, 0, sizeof in);
	if(size < 8 || memcmp(data, kSignature, 8) != 0)
		return Fail(err, errSize, "not a PNG file");
	while(!done)
	{
		uint32_t len;
		const uint8_t* body;
		if(pos + 12 > size)
		{
			msg = "truncated PNG file";
			goto end;
		}
		len = Be32(data + pos);
		body = data + pos + 8;
		if(len > size - pos - 12)
		{
			msg = "truncated PNG file";
			goto end;
		}
		if(BGI_Crc32(0, data + pos + 4, (size_t)len + 4) != Be32(body + len))
		{
			msg = "a chunk's CRC does not match";
			goto end;
		}
		if(memcmp(data + pos + 4, "IHDR", 4) == 0)
		{
			if(len != 13 || haveHeader)
				goto end;
			in.width = Be32(body);
			in.height = Be32(body + 4);
			in.depth = body[8];
			in.colour = body[9];
			in.interlace = body[12];
			if(!in.width || !in.height || in.width > 0x100000 || in.height > 0x100000 || (uint64_t)in.width * in.height > 0x10000000u)
			{
				msg = "unreasonable picture size";
				goto end;
			}
			if(body[10] != 0 || body[11] != 0 || in.interlace > 1 ||
				!((in.colour == 0 && (in.depth == 1 || in.depth == 2 || in.depth == 4 || in.depth == 8 || in.depth == 16)) ||
					(in.colour == 3 && (in.depth == 1 || in.depth == 2 || in.depth == 4 || in.depth == 8)) ||
					((in.colour == 2 || in.colour == 4 || in.colour == 6) && (in.depth == 8 || in.depth == 16))))
			{
				msg = "unsupported colour type, bit depth or method";
				goto end;
			}
			haveHeader = 1;
		}
		else if(!haveHeader)
			goto end;
		else if(memcmp(data + pos + 4, "PLTE", 4) == 0)
		{
			if(len % 3 || len > 768)
				goto end;
			in.paletteCount = (int)(len / 3);
			for(i = 0; i < len / 3; i++)
			{
				memcpy(in.palette[i], body + i * 3, 3);
				in.palette[i][3] = 255;
			}
		}
		else if(memcmp(data + pos + 4, "tRNS", 4) == 0)
		{
			if(in.colour == 3)
			{
				for(i = 0; i < len && i < 256; i++)
					in.palette[i][3] = body[i];
				in.hasTrns = 1;
			}
			else if(in.colour == 0 && len >= 2)
			{
				in.key[0] = (uint16_t)((body[0] << 8) | body[1]);
				in.hasTrns = 1;
			}
			else if(in.colour == 2 && len >= 6)
			{
				for(i = 0; i < 3; i++)
					in.key[i] = (uint16_t)((body[i * 2] << 8) | body[i * 2 + 1]);
				in.hasTrns = 1;
			}
		}
		else if(memcmp(data + pos + 4, "IDAT", 4) == 0)
		{
			if(idatLen + len > idatCap)
			{
				uint8_t* p;
				idatCap = (idatLen + len) * 2 + 4096;
				p = (uint8_t*)realloc(idat, idatCap);
				if(!p)
				{
					msg = "out of memory";
					goto end;
				}
				idat = p;
			}
			memcpy(idat + idatLen, body, len);
			idatLen += len;
		}
		else if(memcmp(data + pos + 4, "IEND", 4) == 0)
			done = 1;
		else if(!(data[pos + 4] & 0x20))
		{ // an upper-case first letter: a chunk the picture cannot be read without
			msg = "unknown critical chunk";
			goto end;
		}
		pos += (size_t)len + 12;
	}
	if(!haveHeader || !idatLen || (in.colour == 3 && !in.paletteCount))
		goto end;

	// the size of the scanline data: one filter byte and the packed samples per row, per pass
	bits = Samples(in.colour) * in.depth;
	bpp = (size_t)(bits + 7) / 8;
	passes = in.interlace ? 7 : 1;
	for(pass = 0; pass < passes; pass++)
	{
		uint32_t w = in.interlace ? (in.width + kPass[pass][2] - 1 - kPass[pass][0]) / kPass[pass][2] : in.width;
		uint32_t h = in.interlace ? (in.height + kPass[pass][3] - 1 - kPass[pass][1]) / kPass[pass][3] : in.height;
		if(w && h)
			expect += (((size_t)w * (size_t)bits + 7) / 8 + 1) * h;
	}
	raw = Deflate_Uncompress(idat, idatLen, expect, &rawLen);
	if(!raw || rawLen < expect)
	{
		msg = "the picture data does not decompress";
		goto end;
	}
	count = (size_t)in.width * in.height;
	rgba = (uint8_t*)malloc(count * 4);
	prev = (uint8_t*)malloc(((size_t)in.width * (size_t)bits + 7) / 8 + 1);
	if(!rgba || !prev)
	{
		msg = "out of memory";
		goto end;
	}
	for(pass = 0; pass < passes; pass++)
	{
		uint32_t x0 = in.interlace ? kPass[pass][0] : 0, y0 = in.interlace ? kPass[pass][1] : 0;
		uint32_t dx = in.interlace ? kPass[pass][2] : 1, dy = in.interlace ? kPass[pass][3] : 1;
		uint32_t w = (in.width + dx - 1 - x0) / dx, h = (in.height + dy - 1 - y0) / dy, y;
		size_t row = ((size_t)w * (size_t)bits + 7) / 8;
		if(!w || !h)
			continue;
		memset(prev, 0, row);
		for(y = 0; y < h; y++)
		{
			uint8_t* cur = raw + off + 1;
			if(!Unfilter(cur, prev, row, bpp, raw[off]))
			{
				msg = "unknown scanline filter";
				goto end;
			}
			ExpandRow(rgba + ((size_t)(y0 + y * dy) * in.width + x0) * 4, dx, cur, w, &in);
			memcpy(prev, cur, row);
			off += row + 1;
		}
	}

	// hand out as few channels as the file has: grey, colour, or either with alpha
	out->width = in.width;
	out->height = in.height;
	out->channels = (in.colour == 4 || in.colour == 6 || in.hasTrns) ? 4 : in.colour == 0 ? 1
																						  : 3;
	out->pixels = (uint8_t*)malloc(count * (size_t)out->channels);
	if(!out->pixels)
	{
		msg = "out of memory";
		goto end;
	}
	for(i = 0; i < count; i++)
	{
		const uint8_t* s = rgba + i * 4;
		uint8_t* d = out->pixels + i * (size_t)out->channels;
		if(out->channels == 1)
			d[0] = s[0];
		else
		{
			d[0] = s[2];
			d[1] = s[1];
			d[2] = s[0];
			if(out->channels == 4)
				d[3] = s[3];
		}
	}
	ok = 1;
end:
	free(idat);
	free(raw);
	free(rgba);
	free(prev);
	if(!ok)
		return Fail(err, errSize, msg);
	return 1;
}

int Png_Convert(PngImage_t* img, int channels)
{
	size_t count = (size_t)img->width * img->height, i;
	uint8_t* dst;
	int lost = 0, from = img->channels;
	if(channels == from)
		return 0;
	dst = (uint8_t*)malloc(count * (size_t)channels + 1);
	if(!dst)
		return 0;
	for(i = 0; i < count; i++)
	{
		const uint8_t* s = img->pixels + i * (size_t)from;
		uint8_t* d = dst + i * (size_t)channels;
		uint8_t b = s[0], g = from == 1 ? s[0] : s[1], r = from == 1 ? s[0] : s[2], a = from == 4 ? s[3] : 255;
		if(a != 255 && channels != 4)
			lost |= PNG_LOST_ALPHA;
		if(channels == 1)
		{
			if(b != g || g != r)
			{ // the luma weights of the engine's own greyscale conversions
				lost |= PNG_LOST_COLOUR;
				d[0] = (uint8_t)((28 * b + 151 * g + 77 * r) >> 8);
			}
			else
				d[0] = g;
		}
		else
		{
			d[0] = b;
			d[1] = g;
			d[2] = r;
			if(channels == 4)
				d[3] = a;
		}
	}
	free(img->pixels);
	img->pixels = dst;
	img->channels = channels;
	return lost;
}
