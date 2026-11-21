/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bfmovieenc.c - BF_Movie encoder (type 0, the frame sequences decoded by
 *                bfmovie.c), for the tools that write game archives;
 *                interface in bfmovie.h
 *
 * A movie is built frame by frame (BfMovieEnc_Begin / AddFrame / Finish).
 * The first frame is stored whole, filtered like a CompressedBG picture;
 * every later frame as the 8 x 8 blocks that differ from the frame before,
 * each pixel of such a block in the cheapest of the three forms the
 * decoder knows that fits it: unchanged (a copy with a displacement of
 * zero, two bytes), all zero (the byte 0x80) or the previous pixel plus a
 * delta per channel.  The original's encoder also searches for moved
 * pixels; this one does not, so its files are larger than the originals
 * but decode to the same pictures.  Each frame then goes through the run
 * coding (runs of the byte 0x80 against literal bytes) and the Huffman
 * stage, whose tree is built exactly as the decoder builds it from the
 * weights stored in the frame.
 */
#include "bgi/bfmovie.h"

struct BfMovieEnc
{
	uint32_t width, height, bpp, fps;
	uint8_t* prev;    // the frame before: width x height pixels of 4 bytes, as the decoder's slot holds it
	uint8_t** frames; // the packed frames
	uint32_t* sizes;  // their sizes in bytes
	uint32_t count, cap;
};

// append `v` as a varint (7 bits per byte, the low group first, bit 7 set while more follow)
static uint8_t* PutVarint(uint8_t* p, uint32_t v)
{
	do
	{
		uint8_t b = (uint8_t)(v & 0x7f);
		v >>= 7;
		*p++ = (uint8_t)(b | (v ? 0x80 : 0));
	} while(v);
	return p;
}

// ---- the Huffman stage ---------------------------------------------------------------------

typedef struct EncNode
{
	int valid;       // still to be paired
	uint32_t weight; // a leaf's byte count, the sum of its children for a parent
	int parent;      // -1 for the root
	int isRight;     // the child taken on a 1 bit: the lighter of its parent's two
} EncNode_t;

#define ENC_NODES 511 // 256 leaves + 255 parents

/* The code of every byte value under the tree the decoder builds from
 * `weights`: parents are numbered down from 510, each joining the two
 * lightest nodes still unpaired (the lowest index among equals), the
 * lightest as the child of bit 1.  A code's bits are returned first bit in
 * bit 0, as the stream takes them from the low end of each byte. */
static void BuildCodes(const uint32_t* weights, uint64_t* codes, uint8_t* lens)
{
	EncNode_t nodes[ENC_NODES];
	uint32_t total = 0;
	int i, parent = ENC_NODES - 1;
	for(i = 0; i < ENC_NODES; i++)
	{
		nodes[i].weight = i < 256 ? weights[i] : 0;
		nodes[i].valid = nodes[i].weight != 0;
		nodes[i].parent = -1;
		nodes[i].isRight = 0;
		total += nodes[i].weight;
	}
	for(;;)
	{
		int pick[2], k;
		uint32_t sum = 0;
		for(k = 0; k < 2; k++)
		{
			uint32_t best = 0xffffffffu;
			int j;
			pick[k] = -1;
			for(j = 0; j < ENC_NODES; j++)
				if(nodes[j].valid && nodes[j].weight < best)
				{
					best = nodes[j].weight;
					pick[k] = j;
				}
			if(pick[k] >= 0)
			{
				nodes[pick[k]].valid = 0;
				nodes[pick[k]].parent = parent;
				nodes[pick[k]].isRight = k == 0;
				sum += nodes[pick[k]].weight;
			}
		}
		nodes[parent].valid = sum != 0;
		nodes[parent].weight = sum;
		if(sum == total || parent <= 256)
			break;
		parent--;
	}
	for(i = 0; i < 256; i++)
	{
		uint64_t code = 0;
		int len = 0, node = i;
		while(nodes[node].parent >= 0)
		{ // collected leaf first: shifting up puts the root's bit lowest
			code = (code << 1) | (uint64_t)nodes[node].isRight;
			len++;
			node = nodes[node].parent;
		}
		codes[i] = code;
		lens[i] = (uint8_t)len;
	}
}

/* pack one frame: `payload` (the pixel stage's bytes) through the run
 * coding and the Huffman stage; returns the frame (BGI_Free) and its size */
static uint8_t* PackFrame(const uint8_t* payload, uint32_t len, uint32_t* outSize)
{
	uint8_t* runs = (uint8_t*)BGI_Alloc((size_t)len + (size_t)len / 2 + 32);
	uint8_t *p = runs + 4, *out, *q;
	uint32_t i = 0, n, weights[256];
	uint64_t codes[256], bits = 0, acc = 0;
	uint8_t lens[256];
	int nacc = 0;
	// the size the runs expand to, then (fill, copy) pairs: `fill` bytes of 0x80, `copy` literal bytes
	memcpy(runs, &len, 4);
	while(i < len)
	{
		uint32_t fill = 0, copy = 0;
		while(i + fill < len && payload[i + fill] == 0x80)
			fill++;
		i += fill;
		// the literals run up to the next three bytes of 0x80 (a shorter run costs more as a pair than as it is)
		while(i + copy < len && !(payload[i + copy] == 0x80 && i + copy + 2 < len && payload[i + copy + 1] == 0x80 && payload[i + copy + 2] == 0x80))
			copy++;
		p = PutVarint(p, fill);
		p = PutVarint(p, copy);
		memcpy(p, payload + i, copy);
		p += copy;
		i += copy;
	}
	n = (uint32_t)(p - runs);
	memset(weights, 0, sizeof weights);
	for(i = 0; i < n; i++)
		weights[runs[i]]++;
	BuildCodes(weights, codes, lens);
	for(i = 0; i < 256; i++)
		bits += (uint64_t)lens[i] * weights[i];
	out = (uint8_t*)BGI_Calloc(5 + 256 * 5 + (size_t)((bits + 7) / 8) + 1);
	q = PutVarint(out, n);
	for(i = 0; i < 256; i++)
		q = PutVarint(q, weights[i]);
	for(i = 0; i < n; i++)
	{
		acc |= codes[runs[i]] << nacc;
		nacc += lens[runs[i]];
		while(nacc >= 8)
		{
			*q++ = (uint8_t)acc;
			acc >>= 8;
			nacc -= 8;
		}
	}
	if(nacc)
		*q++ = (uint8_t)acc;
	BGI_Free(runs);
	*outSize = (uint32_t)(q - out);
	return out;
}

// ---- the pixel stage ---------------------------------------------------------------------

/* the first frame: rows of bpp / 8 bytes per pixel padded to 4 bytes, each
 * byte minus the average of its left and upper neighbours (one of them
 * alone at an edge, nothing at the corner) */
static uint8_t* KeyPayload(const BfMovieEnc_t* e, const uint8_t* pix, uint32_t* size)
{
	uint32_t bytes = e->bpp >> 3, channels = bytes < 4 ? 3 : 4, w = e->width, h = e->height, x, y, c;
	uint32_t stride = (bytes * w + 3) & ~3u;
	uint8_t* out = (uint8_t*)BGI_Calloc((size_t)stride * h + 1);
	for(y = 0; y < h; y++)
	{
		const uint8_t* row = pix + (size_t)y * w * 4;
		uint8_t* d = out + (size_t)y * stride;
		for(x = 0; x < w; x++, row += 4, d += bytes)
			for(c = 0; c < channels; c++)
			{
				int pred;
				if(x > 0 && y > 0)
					pred = (row[(int)c - 4] + row[(ptrdiff_t)c - (ptrdiff_t)w * 4]) >> 1;
				else if(x > 0)
					pred = row[(int)c - 4];
				else if(y > 0)
					pred = row[(ptrdiff_t)c - (ptrdiff_t)w * 4];
				else
					pred = 0;
				d[c] = (uint8_t)(row[c] - pred);
			}
	}
	*size = stride * h;
	return out;
}

/* a later frame against the one before: a bit per 8 x 8 block (set: it
 * changes), eight row masks per changed block (bit x set: pixel x is a
 * delta), then the pixel stream */
static uint8_t* DeltaPayload(const BfMovieEnc_t* e, const uint8_t* pix, uint32_t* size)
{
	uint32_t bytes = e->bpp >> 3, w = e->width, h = e->height;
	uint32_t blocksX = (w + 7) >> 3, blocksY = (h + 7) >> 3, maskBytes = (blocksX * blocksY + 7) >> 3;
	uint32_t bx, by, x, y, present = 0, index = 0;
	uint8_t* out;
	uint8_t *rowMasks, *stream;
	uint8_t* changed = (uint8_t*)BGI_Calloc((size_t)blocksX * blocksY + 1);
	for(by = 0; by < blocksY; by++)
		for(bx = 0; bx < blocksX; bx++)
		{
			uint32_t bw = w - bx * 8 < 8 ? w - bx * 8 : 8, bh = h - by * 8 < 8 ? h - by * 8 : 8;
			for(y = 0; y < bh && !changed[by * blocksX + bx]; y++)
				if(memcmp(pix + ((size_t)(by * 8 + y) * w + bx * 8) * 4, e->prev + ((size_t)(by * 8 + y) * w + bx * 8) * 4, (size_t)bw * 4) != 0)
					changed[by * blocksX + bx] = 1;
			present += changed[by * blocksX + bx];
		}
	// at most 4 stream bytes per pixel of a changed block
	out = (uint8_t*)BGI_Calloc((size_t)maskBytes + (size_t)present * (8 + 64 * 4) + 1);
	rowMasks = out + maskBytes;
	stream = rowMasks + (size_t)present * 8;
	for(by = 0; by < blocksY; by++)
		for(bx = 0; bx < blocksX; bx++, index++)
		{
			uint32_t bw = w - bx * 8 < 8 ? w - bx * 8 : 8, bh = h - by * 8 < 8 ? h - by * 8 : 8;
			if(!changed[index])
				continue;
			out[index >> 3] |= (uint8_t)(1u << (index & 7));
			for(y = 0; y < bh; y++)
				for(x = 0; x < bw; x++)
				{
					const uint8_t* cur = pix + ((size_t)(by * 8 + y) * w + bx * 8 + x) * 4;
					const uint8_t* old = e->prev + ((size_t)(by * 8 + y) * w + bx * 8 + x) * 4;
					if(memcmp(cur, old, 4) == 0)
					{ // unchanged: a copy from the previous frame, displaced by (0, 0)
						*stream++ = 0;
						*stream++ = 0;
					}
					else if(!cur[0] && !cur[1] && !cur[2] && !cur[3])
						*stream++ = 0x80; // a zero pixel
					else
					{
						uint32_t c;
						rowMasks[y] |= (uint8_t)(1u << x);
						for(c = 0; c < bytes; c++)
							*stream++ = (uint8_t)(cur[c] - old[c]);
					}
				}
			rowMasks += 8;
		}
	BGI_Free(changed);
	*size = (uint32_t)(stream - out);
	return out;
}

// ---- the interface ---------------------------------------------------------------------------

BfMovieEnc_t* BfMovieEnc_Begin(uint32_t width, uint32_t height, uint32_t bpp, uint32_t fps)
{
	BfMovieEnc_t* e;
	if(!width || !height || (bpp != 24 && bpp != 32))
		return NULL;
	e = (BfMovieEnc_t*)BGI_Calloc(sizeof *e);
	e->width = width;
	e->height = height;
	e->bpp = bpp;
	e->fps = fps;
	e->prev = (uint8_t*)BGI_Calloc((size_t)width * height * 4);
	return e;
}

void BfMovieEnc_AddFrame(BfMovieEnc_t* e, const uint8_t* pixels, size_t pitch)
{
	size_t count = (size_t)e->width * e->height;
	uint8_t* pix = (uint8_t*)BGI_Alloc(count * 4);
	uint8_t* payload;
	uint32_t payloadSize, y;
	for(y = 0; y < e->height; y++)
		memcpy(pix + (size_t)y * e->width * 4, pixels + (size_t)y * pitch, (size_t)e->width * 4);
	if(e->bpp == 24)
	{ // a 24-bit movie decodes with alpha 0
		size_t i;
		for(i = 0; i < count; i++)
			pix[i * 4 + 3] = 0;
	}
	payload = e->count ? DeltaPayload(e, pix, &payloadSize) : KeyPayload(e, pix, &payloadSize);
	if(e->count == e->cap)
	{
		uint32_t cap = e->cap ? e->cap * 2 : 32;
		uint8_t** frames = (uint8_t**)BGI_Calloc(sizeof(uint8_t*) * cap);
		uint32_t* sizes = (uint32_t*)BGI_Calloc(sizeof(uint32_t) * cap);
		if(e->count)
		{
			memcpy(frames, e->frames, sizeof(uint8_t*) * e->count);
			memcpy(sizes, e->sizes, sizeof(uint32_t) * e->count);
		}
		BGI_Free(e->frames);
		BGI_Free(e->sizes);
		e->frames = frames;
		e->sizes = sizes;
		e->cap = cap;
	}
	e->frames[e->count] = PackFrame(payload, payloadSize, &e->sizes[e->count]);
	e->count++;
	BGI_Free(payload);
	BGI_Free(e->prev);
	e->prev = pix;
}

uint8_t* BfMovieEnc_Finish(BfMovieEnc_t* e, uint32_t* size)
{
	uint32_t total = 0x40 + e->count * 4, i, off;
	uint8_t* out;
	BfMovieHeader_t h;
	for(i = 0; i < e->count; i++)
		total += e->sizes[i];
	out = (uint8_t*)BGI_Calloc((size_t)total + 1);
	memset(&h, 0, sizeof h);
	memcpy(h.magic, BFMOVIE_MAGIC, 16);
	h.width = e->width;
	h.height = e->height;
	h.bpp = e->bpp;
	h.mode = e->bpp == 32 ? 2 : 1; // the pixel mode the target slot must have: ARGB32 / RGB32
	h.fps = e->fps;
	h.frames = e->count;
	memcpy(out, &h, 0x40);
	off = 0x40 + e->count * 4;
	for(i = 0; i < e->count; i++)
	{
		memcpy(out + 0x40 + i * 4, &off, 4);
		memcpy(out + off, e->frames[i], e->sizes[i]);
		off += e->sizes[i];
		BGI_Free(e->frames[i]);
	}
	BGI_Free(e->frames);
	BGI_Free(e->sizes);
	BGI_Free(e->prev);
	BGI_Free(e);
	*size = total;
	return out;
}
