/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bfmovie.c - BF_Movie frame decoder; interface and file layout in bfmovie.h
 *
 * A frame is unpacked in stages (Unpack runs the first three):
 *
 *   ReadVarint   7 bits per byte, low group first
 *   Huffman      256 varint weights, the tree built by pairing the two
 *                lightest nodes (the lower index on ties; the parents are
 *                allocated from the top of the node array downwards), then
 *                the symbols with bits taken from the low end of each byte,
 *                bit 1 = the lighter child
 *   Unrle        run-length (fill of 0x80, literal copy) pairs
 *   KeyFrame     frame 0: the CompressedBG neighbour-average filter
 *   DeltaFrame   a later frame: 8 x 8 blocks against a copy of the previous
 *                frame; Block decodes one changed block
 *
 * BfMovie_DecodeFrame is the entry; bmseq.c calls it for the "90 F6"
 * instruction and extract.c for the --extract conversion.  Every stage is
 * bounded by the frame's length, so a malformed frame yields garbage pixels
 * rather than an out-of-bounds read.
 */
#include "bgi/bfmovie.h"

// 1 when the magic matches and the header with its offset table fits in `size`
int BfMovie_Check(const void* data, uint32_t size)
{
	const BfMovieHeader_t* h = (const BfMovieHeader_t*)data;
	if(size < 0x40 || memcmp(h->magic, BFMOVIE_MAGIC, 16) != 0)
		return 0;
	return size >= 0x40 + (uint32_t)h->frames * 4;
}

// 1 for type 0, the only type decodable here
int BfMovie_IsSimple(const void* data)
{
	return ((const BfMovieHeader_t*)data)->type == 0;
}

// ---- stage 0: varints ----------------------------------------------------------------

/* the varint at p, never reading at or past `end`; *len receives the bytes
 * consumed.  Stops after five bytes (32 bits) whatever the continuation bit
 * says. */
static uint32_t ReadVarint(const uint8_t* p, const uint8_t* end, uint32_t* len)
{
	uint32_t v = 0, n = 0;
	int shift = 0;
	while(p + n < end)
	{
		uint8_t b = p[n++];
		v |= (uint32_t)(b & 0x7f) << shift;
		shift += 7;
		if(!(b & 0x80) || shift > 28)
			break;
	}
	*len = n;
	return v;
}

// ---- stage 1: Huffman ----------------------------------------------------------------

typedef struct BfNode
{
	int valid;       // still to be paired
	uint8_t sym;     // the byte value of a leaf (0 for a parent)
	uint32_t weight; // leaf: from the file; parent: the sum of its children
	int left, right; // children (-1: none); a node without a left child is a leaf
} BfNode_t;

#define BF_NODES 511 // 256 leaves + 255 parents

/* read the 256 weights at src, build the tree and decode `count` symbols
 * from the bit stream that follows the weights into dst.  1 on success, 0
 * when the stream ends before `count` symbols or a code leads nowhere. */
static int Huffman(uint8_t* dst, uint32_t count, const uint8_t* src, const uint8_t* end)
{
	BfNode_t nodes[BF_NODES];
	uint32_t total = 0, i;
	int parent = BF_NODES - 1, root;
	const uint8_t* p = src;
	uint8_t bits = 0;
	int left = 0;

	for(i = 0; i < 256; i++)
	{
		uint32_t n;
		nodes[i].weight = ReadVarint(p, end, &n);
		p += n;
		nodes[i].valid = nodes[i].weight != 0;
		nodes[i].sym = (uint8_t)i;
		nodes[i].left = nodes[i].right = -1;
		total += nodes[i].weight;
	}
	for(i = 256; i < BF_NODES; i++)
	{
		nodes[i].valid = 0;
		nodes[i].sym = 0;
		nodes[i].weight = 0;
		nodes[i].left = nodes[i].right = -1;
	}
	// the tree: the parents fill the array from the top down, each joining the
	// two lightest valid nodes; the parent that carries the whole weight is the
	// root (the index bound stops a tree whose weights do not add up)
	root = parent;
	for(;;)
	{
		int pick[2], k;
		uint32_t sum = 0;
		for(k = 0; k < 2; k++)
		{
			uint32_t best = 0xffffffffu;
			int j;
			pick[k] = -1;
			for(j = 0; j < BF_NODES; j++)
				if(nodes[j].valid && nodes[j].weight < best)
				{
					best = nodes[j].weight;
					pick[k] = j;
				}
			if(pick[k] >= 0)
			{
				nodes[pick[k]].valid = 0;
				sum += nodes[pick[k]].weight;
			}
		}
		nodes[parent].valid = sum != 0;
		nodes[parent].weight = sum;
		nodes[parent].left = pick[1];  // bit 0: the second (heavier) pick
		nodes[parent].right = pick[0]; // bit 1: the lightest
		root = parent;
		if(sum == total || parent <= 256)
			break;
		parent--;
	}
	// the symbols, low bit of each byte first
	for(i = 0; i < count; i++)
	{
		int node = root;
		while(nodes[node].left >= 0)
		{
			if(left == 0)
			{
				if(p >= end)
					return 0;
				bits = *p++;
				left = 8;
			}
			node = (bits & 1) ? nodes[node].right : nodes[node].left;
			bits >>= 1;
			left--;
			if(node < 0)
				return 0;
		}
		dst[i] = nodes[node].sym;
	}
	return 1;
}

// ---- stage 2: runs ---------------------------------------------------------------------

/* expand the (varint fill, varint copy) pairs at src into exactly dstSize
 * bytes: the fill writes 0x80, the copy takes literal bytes; runs are clamped
 * to what is left of either buffer and the remainder is filled with 0x80 */
static void Unrle(uint8_t* dst, uint32_t dstSize, const uint8_t* src, uint32_t srcLen)
{
	const uint8_t* end = src + srcLen;
	uint32_t out = 0;
	while(out < dstSize && src < end)
	{
		uint32_t n, fill, copy;
		fill = ReadVarint(src, end, &n);
		src += n;
		copy = ReadVarint(src, end, &n);
		src += n;
		if(fill > dstSize - out)
			fill = dstSize - out;
		memset(dst + out, 0x80, fill);
		out += fill;
		if(copy > dstSize - out)
			copy = dstSize - out;
		if(copy > (uint32_t)(end - src))
			copy = (uint32_t)(end - src);
		memcpy(dst + out, src, copy);
		src += copy;
		out += copy;
	}
	if(out < dstSize)
		memset(dst + out, 0x80, dstSize - out);
}

/* the stages every frame goes through: varint N, Huffman to N bytes, the
 * first 4 of which give the run-length output size L; returns a buffer of
 * L (+ `spare` + 4 zero) bytes with *outSize = L (BGI_Free it), or NULL when
 * the frame is malformed or claims more than 256 MB */
static uint8_t* Unpack(const uint8_t* data, uint32_t len, uint32_t spare, uint32_t* outSize)
{
	const uint8_t* end = data + len;
	uint32_t hdr, n1, n2;
	uint8_t *stage1, *stage2;
	n1 = ReadVarint(data, end, &hdr);
	if(hdr == 0 || data + hdr > end || n1 < 4 || n1 > 0x10000000u)
		return NULL;
	stage1 = (uint8_t*)BGI_Alloc(n1);
	if(!Huffman(stage1, n1, data + hdr, end))
	{
		BGI_Free(stage1);
		return NULL;
	}
	memcpy(&n2, stage1, 4);
	if(n2 > 0x10000000u)
	{
		BGI_Free(stage1);
		return NULL;
	}
	stage2 = (uint8_t*)BGI_Alloc(n2 + spare + 4);
	Unrle(stage2, n2, stage1 + 4, n1 - 4);
	memset(stage2 + n2, 0, spare + 4);
	BGI_Free(stage1);
	*outSize = n2;
	return stage2;
}

// ---- stage 3: pixels -------------------------------------------------------------------

/* the key frame: `src` holds rows of bpp/8-byte pixels padded to 4 bytes,
 * filtered as in CompressedBG (each byte the difference to the average of
 * its left and upper neighbour).  Writes w x h pixels of 4 bytes into dst;
 * a 24-bit source gets alpha 0.  Nothing is written when `src` is too short
 * for the picture. */
static void KeyFrame(uint8_t* dst, int pitch, const uint8_t* src, uint32_t srcSize, int w, int h, int bpp)
{
	int bytes = bpp >> 3, channels = bytes < 4 ? 3 : 4;
	uint32_t stride = ((uint32_t)bytes * (uint32_t)w + 3) & ~3u;
	int x, y, c;
	if((uint64_t)stride * (uint32_t)h > srcSize)
		return;
	for(y = 0; y < h; y++)
	{
		uint8_t* row = dst + (size_t)y * pitch;
		const uint8_t* s = src + (size_t)y * stride;
		for(x = 0; x < w; x++, row += 4, s += bytes)
		{
			for(c = 0; c < channels; c++)
			{
				int pred;
				if(x > 0 && y > 0)
					pred = (row[c - 4] + row[c - pitch]) >> 1;
				else if(x > 0)
					pred = row[c - 4];
				else if(y > 0)
					pred = row[c - pitch];
				else
					pred = 0;
				row[c] = (uint8_t)(s[c] + pred);
			}
			if(channels == 3)
				row[3] = 0;
		}
	}
}

/* one changed 8 x 8 block (bw x bh pixels at the picture's edge) of a delta
 * frame.  `dst` is the block in the target, `ref` the previous frame at the
 * block's position (its pitch is `pitch` too) inside [refBase, refEnd);
 * `rowMask` holds one mask byte per block row, bit x set when pixel x is
 * coded as a delta.  `bytes` is bpp / 8.  Returns the stream bytes consumed
 * from src. */
static uint32_t Block(uint8_t* dst, const uint8_t* ref, const uint8_t* refBase, const uint8_t* refEnd, int pitch,
	const uint8_t* src, const uint8_t* srcEnd, const uint8_t* rowMask, int bw, int bh, int bytes)
{
	const uint8_t* s = src;
	int x, y;
	for(y = 0; y < bh; y++)
	{
		uint8_t mask = rowMask[y];
		uint8_t* d = dst + (size_t)y * pitch;
		const uint8_t* r = ref + (size_t)y * pitch;
		for(x = 0; x < bw; x++, d += 4, r += 4, mask >>= 1)
		{
			if(mask & 1)
			{ // the previous pixel plus a delta per channel
				if(s + bytes > srcEnd)
					return (uint32_t)(s - src);
				d[0] = (uint8_t)(r[0] + s[0]);
				d[1] = (uint8_t)(r[1] + s[1]);
				d[2] = (uint8_t)(r[2] + s[2]);
				d[3] = bytes == 4 ? (uint8_t)(r[3] + s[3]) : 0;
				s += bytes;
			}
			else if(s < srcEnd && *s == 0x80)
			{ // transparent
				memset(d, 0, 4);
				s++;
			}
			else
			{ // a copy from the previous frame at (dx, dy), signed bytes
				const uint8_t* from;
				if(s + 2 > srcEnd)
					return (uint32_t)(s - src);
				from = r + (ptrdiff_t)(int8_t)s[1] * pitch + (ptrdiff_t)(int8_t)s[0] * 4;
				s += 2;
				if(from >= refBase && from + 4 <= refEnd)
					memcpy(d, from, 4);
				else
					memset(d, 0, 4); // off the frame (the original reads whatever lies there)
			}
		}
	}
	return (uint32_t)(s - src);
}

/* a delta frame: `src` holds a bit per 8 x 8 block (set: changed), eight row
 * masks per changed block and the pixel stream.  `dst` holds the previous
 * frame on entry; the changed blocks are decoded against a copy of it. */
static void DeltaFrame(uint8_t* dst, int pitch, const uint8_t* src, uint32_t srcSize, int w, int h, int bpp)
{
	int blocksX = (w + 7) >> 3, blocksY = (h + 7) >> 3;
	uint32_t maskBytes = ((uint32_t)(blocksX * blocksY) + 7) >> 3;
	const uint8_t *blockMask = src, *rowMasks = src + maskBytes, *stream, *end = src + srcSize;
	uint32_t present = 0, i;
	uint8_t* ref;
	size_t refSize = (size_t)pitch * (size_t)h;
	int bx, by, bytes = bpp >> 3;
	uint32_t bit = 0, maskIndex = 0;

	if(maskBytes > srcSize)
		return;
	for(i = 0; i < maskBytes; i++)
	{ // count the changed blocks, to find the pixel stream behind the row masks
		uint8_t m = blockMask[i];
		int k;
		for(k = 0; k < 8; k++, m >>= 1)
			present += m & 1;
	}
	stream = rowMasks + (size_t)present * 8;
	if(stream > end)
		return;
	ref = (uint8_t*)BGI_Alloc(refSize);
	memcpy(ref, dst, refSize); // the previous frame
	for(by = 0; by < blocksY; by++)
	{
		int bh = h - by * 8 < 8 ? h - by * 8 : 8;
		for(bx = 0; bx < blocksX; bx++)
		{
			int bw = w - bx * 8 < 8 ? w - bx * 8 : 8, set;
			if(maskIndex >= maskBytes)
				break;
			set = (blockMask[maskIndex] >> bit) & 1;
			if(++bit == 8)
			{
				bit = 0;
				maskIndex++;
			}
			if(!set)
				continue;
			if(stream >= end)
				goto done;
			stream += Block(dst + (size_t)by * 8 * pitch + (size_t)bx * 32, ref + (size_t)by * 8 * pitch + (size_t)bx * 32, ref,
				ref + refSize, pitch, stream, end, rowMasks, bw, bh, bytes);
			rowMasks += 8;
		}
	}
done:
	BGI_Free(ref);
}

/* decode frame `frame` into dst (see bfmovie.h); a frame runs from its
 * offset to the next frame's offset or the end of the file.  1 on success,
 * 0 when the header, the frame index or the frame's bounds are bad or the
 * frame does not unpack. */
int BfMovie_DecodeFrame(const void* data, uint32_t size, uint32_t frame, uint8_t* dst, int pitch)
{
	const BfMovieHeader_t* h = (const BfMovieHeader_t*)data;
	const uint8_t* file = (const uint8_t*)data;
	uint32_t start, len, unpacked;
	uint8_t* pixels;
	if(!BfMovie_Check(data, size) || frame >= h->frames)
		return 0;
	start = h->offset[frame];
	if(start >= size)
		return 0;
	len = (frame + 1 < h->frames ? h->offset[frame + 1] : size);
	if(len > size || len <= start)
		return 0;
	len -= start;
	// a delta frame's pixel stream may read two bytes past its end
	pixels = Unpack(file + start, len, frame ? 2 : 0, &unpacked);
	if(!pixels)
		return 0;
	if(frame == 0)
		KeyFrame(dst, pitch, pixels, unpacked, (int)h->width, (int)h->height, (int)h->bpp);
	else
		DeltaFrame(dst, pitch, pixels, unpacked, (int)h->width, (int)h->height, (int)h->bpp);
	BGI_Free(pixels);
	return 1;
}
