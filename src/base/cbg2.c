/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cbg2.c - "CompressedBG___" version 2, the DCT image codec of 1.573 on
 *          (interface in codec.h; BGI_CbgDecode hands a version 2 file here)
 *
 * The original wraps the file into a one-frame BF_Movie of type 0x10001 and
 * runs that movie's DCT frame decoder on it, one job per 8-pixel band.
 * The codec is JPEG-like:
 * YCbCr with full-resolution chroma, 8 x 8 blocks, the two quantisation
 * tables (luma, chroma) pre-scaled by the AAN factors and the float IDCT of
 * libjpeg's jidctflt, DC differences and run / size AC pairs coded with
 * the engine's weight-built Huffman trees (16 DC categories, 176 AC
 * symbols: the run in the low nibble, the size in the high one, 0 = end of
 * block, 0x0F = sixteen zeros), the bits taken MSB first.  The alpha plane
 * of a 32-bit image is a separate LZ-coded byte stream.
 *
 * File layout after the 0x30-byte header: encLen (132) encrypted bytes -
 * the two 8 x 8 quantisation tables and four unused bytes, decrypted and
 * checked like the version 1 weight table -, then the stream: 16 + 176
 * varint weights, (rows + 1) u32 offsets relative to the stream start, and
 * one band per 8-pixel row at its offset: a bitmap with one bit per block
 * column (all set in a still image; a movie frame clears the unchanged
 * blocks), a varint with the number of coefficients (columns * 3 * 64),
 * the DC bit stream (one predictor across the band's blocks: the Y
 * blocks, then Cb, then Cr) and the AC bit stream from the next byte
 * boundary.  A band holds coefficients only for the blocks its bitmap
 * names.  offsets[rows] is the alpha stream of a 32-bit image: a u32
 * mode, 1 for the literal / LZ token stream of the whole plane, 2 for a
 * Huffman coded stream that gives the alpha of some blocks only (a movie's
 * frames; AlphaBlocks).
 *
 * Cbg2_Frame decodes one such stream over a picture that is already
 * there, which is all a frame of the streamed BF_Movie is (bfmovie.c).
 *
 * The decoded image is a RawImageHeader and tightly packed rows, as for
 * version 1.  Every read is bounded by the file size; a malformed file
 * yields garbage pixels rather than an out-of-bounds read.
 */
#include "bgi/codec.h"
#include "bgi/bytes.h"

#define DC_SYMBOLS 16
#define AC_SYMBOLS 176

static const uint8_t gZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41,
	34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38,
	31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// the AAN scale factors (jfdctflt / jidctflt); the table entry i gets aan[i / 8] * aan[i % 8]
static const double gAan[8] = {1.0, 1.387039845, 1.306562965, 1.175875602, 1.0, 0.785694958, 0.541196100, 0.275899379};

// ---- the bit stream ----------------------------------------------------------------------

typedef struct Bits
{
	const uint8_t* p;
	uint32_t len; // bytes readable from p
	uint32_t pos; // the byte
	uint32_t bit; // bits of it consumed, 0 .. 7
} Bits_t;

// n bits (n <= 24), MSB first; bytes past the end read as 0
static uint32_t ReadBits(Bits_t* b, uint32_t n)
{
	uint32_t v = 0;
	while(n)
	{
		uint32_t byte = b->pos < b->len ? b->p[b->pos] : 0;
		uint32_t avail = 8 - b->bit;
		uint32_t take = avail < n ? avail : n;
		v = (v << take) | (((byte << b->bit) & 0xffu) >> (8 - take));
		n -= take;
		b->bit += take;
		if(b->bit == 8)
		{
			b->bit = 0;
			b->pos++;
		}
	}
	return v;
}

// the leaf reached from the root, a set bit taking the right child
static uint32_t ReadSymbol(Bits_t* b, const CbgNode_t* nodes, uint32_t root, uint32_t leaves)
{
	uint32_t node = root;
	while(node >= leaves && node != 0xffffffffu)
		node = ReadBits(b, 1) ? nodes[node].right : nodes[node].left;
	return node < leaves ? node : 0;
}

// 7 bits per byte, low group first; the length in *len
static uint32_t ReadVarint(const uint8_t* p, const uint8_t* end, uint32_t* len)
{
	uint32_t v = 0, shift = 0, n = 0;
	while(p + n < end)
	{
		uint8_t c = p[n++];
		if(shift < 32)
			v |= (uint32_t)(c & 0x7f) << shift;
		shift += 7;
		if(!(c & 0x80))
			break;
	}
	*len = n;
	return v;
}

// a `size`-bit magnitude: the JPEG sign convention, a clear top bit means negative
static int32_t SignExtend(uint32_t v, uint32_t size)
{
	if(size == 0 || (v & (1u << (size - 1))))
		return (int32_t)v;
	return (int32_t)((~0u << size) | v) + 1;
}

// ---- the Huffman trees --------------------------------------------------------------------------

/* The tree builder of this codec: the pairing of
 * the two lightest valid nodes as in cbg.c, with one quirk that changes
 * the tree whenever it bites - the scan for the k-th pick (k = 0, 1)
 * starts at node k + 1, the first valid node being the initial candidate,
 * so the second pick never considers node 1 unless node 1 is the first
 * valid one.  The DC tree of a sprite with a few large categories comes
 * out differently from the textbook construction, and a decoder without
 * the quirk desynchronises on exactly those images.  Nodes 0 .. leaves-1
 * are the leaves, the parents follow; the root, -1 for all-zero weights. */
static int BuildTree(CbgNode_t* nodes, const uint32_t* weights, uint32_t leaves)
{
	uint32_t total = 0, i, n;
	for(i = 0; i < 2 * leaves - 1; i++)
	{
		nodes[i].valid = 0;
		nodes[i].weight = 0;
		nodes[i].isParent = i >= leaves;
		nodes[i].parent = nodes[i].left = nodes[i].right = 0xffffffffu;
	}
	for(i = 0; i < leaves; i++)
	{
		nodes[i].weight = weights[i];
		nodes[i].valid = weights[i] != 0;
		nodes[i].left = nodes[i].right = i;
		total += weights[i];
	}
	if(total == 0)
		return -1;
	for(n = leaves; n < 2 * leaves - 1; n++)
	{
		uint32_t pick[2], k, sum = 0;
		for(k = 0; k < 2; k++)
		{
			uint32_t cand = 0xffffffffu, j;
			for(j = 0; j < n; j++)
				if(nodes[j].valid)
				{
					cand = j;
					break;
				}
			for(j = k + 1; j < n; j++)
				if(nodes[j].valid && cand != 0xffffffffu && nodes[j].weight < nodes[cand].weight)
					cand = j;
			pick[k] = cand;
			if(cand != 0xffffffffu)
			{
				nodes[cand].valid = 0;
				nodes[cand].parent = n;
				sum += nodes[cand].weight;
			}
		}
		nodes[n].valid = 1;
		nodes[n].weight = sum;
		nodes[n].left = pick[0];
		nodes[n].right = pick[1];
		if(sum >= total)
			return (int)n;
	}
	return (int)(2 * leaves - 2);
}

// ---- the coefficients of a band ------------------------------------------------------------

/* the DC values: one difference per block, one predictor across the band
 * (the original keeps the low 16 bits); the AC stream starts at the next
 * byte boundary */
static void DecodeDc(int16_t* coef, uint32_t count, Bits_t* b, const CbgNode_t* nodes, uint32_t root)
{
	int32_t pred = 0;
	uint32_t i;
	for(i = 0; i < count && b->pos < b->len; i += 64)
	{
		uint32_t s = ReadSymbol(b, nodes, root, DC_SYMBOLS);
		uint32_t raw = ReadBits(b, s);
		pred = (int16_t)(pred + SignExtend(raw, s));
		coef[i] = (int16_t)pred;
	}
	if(b->bit)
	{
		b->bit = 0;
		b->pos++;
	}
}

// the AC values of every block in zigzag order: run / size symbols, 0 ends the block, 0x0F skips sixteen
static void DecodeAc(int16_t* coef, uint32_t count, Bits_t* b, const CbgNode_t* nodes, uint32_t root)
{
	uint32_t i;
	for(i = 0; i < count && b->pos < b->len; i += 64)
	{
		uint32_t k = 1;
		while(k < 64)
		{
			uint32_t s = ReadSymbol(b, nodes, root, AC_SYMBOLS), size;
			if(s == 0)
				break;
			if(s == 0x0f)
			{
				k += 16;
				continue;
			}
			k += s & 15;
			size = s >> 4;
			if(k < 64)
				coef[i + gZigzag[k]] = (int16_t)SignExtend(ReadBits(b, size), size);
			else
				ReadBits(b, size);
			k++;
		}
	}
}

// ---- the inverse DCT (jidctflt) -------------------------------------------------------------

// v / 8 (truncated like the original's _ftol, then an arithmetic shift) + 128, clamped
static uint8_t Sample(float v)
{
	int t = ((int)v >> 3) + 128;
	return (uint8_t)(t < 0 ? 0 : t > 255 ? 255
										 : t);
}

/* the 8 x 8 block `coef` (natural order) through the quantisation table q
 * (already scaled by the AAN factors) into out: the AAN float IDCT of
 * libjpeg's jidctflt, columns first into a workspace, then the rows */
static void Idct(uint8_t out[64], const int16_t* coef, const float* q)
{
	float ws[64];
	int c, r;
	for(c = 0; c < 8; c++)
	{
		const int16_t* in = coef + c;
		const float* qt = q + c;
		float* w = ws + c;
		float tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7, tmp10, tmp11, tmp12, tmp13, z5, z10, z11, z12, z13;
		if(in[8] == 0 && in[16] == 0 && in[24] == 0 && in[32] == 0 && in[40] == 0 && in[48] == 0 && in[56] == 0)
		{
			float dc = (float)in[0] * qt[0];
			for(r = 0; r < 8; r++)
				w[r * 8] = dc;
			continue;
		}
		tmp0 = (float)in[0] * qt[0];
		tmp1 = (float)in[16] * qt[16];
		tmp2 = (float)in[32] * qt[32];
		tmp3 = (float)in[48] * qt[48];
		tmp10 = tmp0 + tmp2;
		tmp11 = tmp0 - tmp2;
		tmp13 = tmp1 + tmp3;
		tmp12 = (tmp1 - tmp3) * 1.414213562f - tmp13;
		tmp0 = tmp10 + tmp13;
		tmp3 = tmp10 - tmp13;
		tmp1 = tmp11 + tmp12;
		tmp2 = tmp11 - tmp12;
		tmp4 = (float)in[8] * qt[8];
		tmp5 = (float)in[24] * qt[24];
		tmp6 = (float)in[40] * qt[40];
		tmp7 = (float)in[56] * qt[56];
		z13 = tmp6 + tmp5;
		z10 = tmp6 - tmp5;
		z11 = tmp4 + tmp7;
		z12 = tmp4 - tmp7;
		tmp7 = z11 + z13;
		tmp11 = (z11 - z13) * 1.414213562f;
		z5 = (z10 + z12) * 1.847759065f;
		tmp10 = 1.082392200f * z12 - z5;
		tmp12 = -2.613125930f * z10 + z5;
		tmp6 = tmp12 - tmp7;
		tmp5 = tmp11 - tmp6;
		tmp4 = tmp10 + tmp5;
		w[0] = tmp0 + tmp7;
		w[56] = tmp0 - tmp7;
		w[8] = tmp1 + tmp6;
		w[48] = tmp1 - tmp6;
		w[16] = tmp2 + tmp5;
		w[40] = tmp2 - tmp5;
		w[32] = tmp3 + tmp4;
		w[24] = tmp3 - tmp4;
	}
	for(r = 0; r < 8; r++)
	{
		const float* w = ws + r * 8;
		uint8_t* dst = out + r * 8;
		float tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7, tmp10, tmp11, tmp12, tmp13, z5, z10, z11, z12, z13;
		tmp10 = w[0] + w[4];
		tmp11 = w[0] - w[4];
		tmp13 = w[2] + w[6];
		tmp12 = (w[2] - w[6]) * 1.414213562f - tmp13;
		tmp0 = tmp10 + tmp13;
		tmp3 = tmp10 - tmp13;
		tmp1 = tmp11 + tmp12;
		tmp2 = tmp11 - tmp12;
		z13 = w[5] + w[3];
		z10 = w[5] - w[3];
		z11 = w[1] + w[7];
		z12 = w[1] - w[7];
		tmp7 = z11 + z13;
		tmp11 = (z11 - z13) * 1.414213562f;
		z5 = (z10 + z12) * 1.847759065f;
		tmp10 = 1.082392200f * z12 - z5;
		tmp12 = -2.613125930f * z10 + z5;
		tmp6 = tmp12 - tmp7;
		tmp5 = tmp11 - tmp6;
		tmp4 = tmp10 + tmp5;
		dst[0] = Sample(tmp0 + tmp7);
		dst[7] = Sample(tmp0 - tmp7);
		dst[1] = Sample(tmp1 + tmp6);
		dst[6] = Sample(tmp1 - tmp6);
		dst[2] = Sample(tmp2 + tmp5);
		dst[5] = Sample(tmp2 - tmp5);
		dst[4] = Sample(tmp3 + tmp4);
		dst[3] = Sample(tmp3 - tmp4);
	}
}

// ---- colour ----------------------------------------------------------------------------------

// the original's tables: the chroma terms carry the + 0.5, the sum is truncated and clamped
static uint8_t Clamp(float v)
{
	int t = (int)(v + 256.0f) - 256;
	return (uint8_t)(t < 0 ? 0 : t > 255 ? 255
										 : t);
}

// one 8 x 8 block of Y, Cb, Cr samples to B G R (A untouched) at dst, rows `pitch` apart
static void ToBgr(uint8_t* dst, int pitch, const uint8_t* y, const uint8_t* cb, const uint8_t* cr)
{
	int r, c;
	for(r = 0; r < 8; r++)
	{
		uint8_t* d = dst + r * pitch;
		for(c = 0; c < 8; c++, d += 4)
		{
			float fy = (float)y[r * 8 + c];
			float fcb = (float)((int)cb[r * 8 + c] - 128), fcr = (float)((int)cr[r * 8 + c] - 128);
			d[0] = Clamp(fy + (1.772f * fcb + 0.5f));
			d[1] = Clamp(fy + (-0.34414f * fcb) + (0.5f - 0.71414f * fcr));
			d[2] = Clamp(fy + (1.402f * fcr + 0.5f));
		}
	}
}

// ---- the alpha plane --------------------------------------------------------------------------

/* mode 1: the alpha bytes of the 32-bit rows, a
 * flag byte per eight tokens (bit set: a 16-bit copy token - a column
 * offset in the low six bits, signed, a row offset above it in three
 * bits, 0 or negative, and the count minus three in the top seven -,
 * clear: a literal byte) */
static void AlphaLz(uint8_t* buf, uint32_t bufLen, int pitch, const uint8_t* s, uint32_t len)
{
	uint32_t flagPos = 0, bitIdx = 0, pos = 1, out = 3;
	while(pos < len && out < bufLen)
	{
		if(s[flagPos] & (1u << bitIdx))
		{
			uint32_t tok, n;
			int dx, dy;
			uint8_t* d;
			const uint8_t* src;
			if(pos + 1 >= len)
				break;
			tok = s[pos] | ((uint32_t)s[pos + 1] << 8);
			pos += 2;
			dx = (int)(tok & 0x3f);
			if(dx > 0x1f)
				dx -= 0x40;
			dy = (int)((tok >> 6) & 7);
			if(dy > 0)
				dy -= 8;
			n = (tok >> 9) + 3;
			d = buf + out;
			src = d + dy * pitch + dx * 4;
			if(src < buf || src >= buf + bufLen)
				break;
			while(n-- && out < bufLen)
			{
				*d = *src;
				d += 4;
				src += 4;
				out += 4;
			}
		}
		else
		{
			buf[out] = s[pos++];
			out += 4;
		}
		if(++bitIdx == 8)
		{
			if(pos >= len || out >= bufLen)
				break;
			flagPos = pos++;
			bitIdx = 0;
		}
	}
}

/* mode 2 (a movie's frames between its whole ones): a 32-bit count N, 256
 * varint weights and the bit stream of N bytes: first a bit for every
 * block of the picture, row by row (the low bit of a byte first), then the
 * 64 alpha bytes of every block whose bit is set.  The other blocks keep
 * the alpha they have. */
static void AlphaBlocks(uint8_t* buf, uint32_t padW, uint32_t cols, uint32_t rows, const uint8_t* s, uint32_t len)
{
	uint32_t blocks = cols * rows, bmBytes = (blocks + 7) / 8, total, used = 4, i, k, pos;
	uint32_t weights[256];
	CbgNode_t* nodes;
	uint8_t* bytes;
	int root;
	if(len < 4)
		return;
	total = Le32(s);
	if(total > bmBytes + blocks * 64)
		total = bmBytes + blocks * 64;
	for(i = 0; i < 256; i++)
	{
		uint32_t n;
		weights[i] = ReadVarint(s + used, s + len, &n);
		used += n;
	}
	nodes = (CbgNode_t*)BGI_Alloc(sizeof(CbgNode_t) * (2 * 256 - 1));
	root = BuildTree(nodes, weights, 256);
	bytes = (uint8_t*)BGI_Alloc(bmBytes + blocks * 64);
	memset(bytes, 0, bmBytes); // (a stream that ends early names no further block)
	if(root >= 0 && used < len)
	{
		Bits_t b;
		b.p = s + used;
		b.len = len - used;
		b.pos = 0;
		b.bit = 0;
		for(i = 0; i < total && b.pos < b.len; i++)
			bytes[i] = (uint8_t)ReadSymbol(&b, nodes, (uint32_t)root, 256);
		total = i;
	}
	else
		total = 0;
	pos = bmBytes;
	for(k = 0; k < blocks && pos + 64 <= total; k++)
		if(bytes[k >> 3] & (1u << (k & 7)))
		{
			uint8_t* d = buf + ((size_t)(k / cols) * 8 * padW + (size_t)(k % cols) * 8) * 4 + 3;
			uint32_t x, y;
			for(y = 0; y < 8; y++, d += (size_t)padW * 4)
				for(x = 0; x < 8; x++)
					d[x * 4] = bytes[pos++];
		}
	BGI_Free(bytes);
	BGI_Free(nodes);
}

// ---- the decoder ------------------------------------------------------------------------------

int Cbg2_Frame(const uint8_t* stream, uint32_t len, const uint8_t* quant, uint32_t w, uint32_t hgt, int alpha, uint8_t* buf)
{
	const uint8_t* end = stream + len;
	uint32_t weights[AC_SYMBOLS], n, i, r, c;
	uint32_t padW = (w + 7) & ~7u, padH = (hgt + 7) & ~7u, cols = padW / 8, rows = padH / 8;
	uint32_t bmBytes = (cols + 7) / 8, bandCoefs = cols * 3 * 64, bufLen = padW * padH * 4;
	const uint8_t* offsets;
	float q[2][64];
	CbgNode_t *dcNodes, *acNodes;
	int dcRoot, acRoot, ok = 1;
	int16_t* coef;

	for(i = 0; i < 64; i++)
	{
		double aan = gAan[i / 8] * gAan[i % 8];
		q[0][i] = (float)(quant[i] * aan);
		q[1][i] = (float)(quant[64 + i] * aan);
	}
	n = 0;
	for(i = 0; i < DC_SYMBOLS; i++)
	{
		uint32_t used;
		weights[i] = ReadVarint(stream + n, end, &used);
		n += used;
	}
	dcNodes = (CbgNode_t*)BGI_Alloc(sizeof(CbgNode_t) * (2 * DC_SYMBOLS - 1));
	dcRoot = BuildTree(dcNodes, weights, DC_SYMBOLS);
	for(i = 0; i < AC_SYMBOLS; i++)
	{
		uint32_t used;
		weights[i] = ReadVarint(stream + n, end, &used);
		n += used;
	}
	acNodes = (CbgNode_t*)BGI_Alloc(sizeof(CbgNode_t) * (2 * AC_SYMBOLS - 1));
	acRoot = BuildTree(acNodes, weights, AC_SYMBOLS);
	offsets = stream + n; // rows + 1 little-endian u32, relative to `stream`
	if(n > len || (len - n) / 4 < rows + 1)
	{
		BGI_Free(acNodes);
		BGI_Free(dcNodes);
		return 0;
	}
	coef = (int16_t*)BGI_Alloc(bandCoefs * sizeof(int16_t));

	for(r = 0; r < rows; r++)
	{
		uint32_t off = Le32(offsets + r * 4), next = Le32(offsets + (r + 1) * 4), count, used, planeCoefs, k = 0;
		const uint8_t* band = stream + off;
		const uint8_t* bitmap = band;
		Bits_t b;
		uint8_t plane[3][64];
		if(off >= len || len - off <= bmBytes)
			continue; // the band is not there
		if(next < off || next > len)
			next = len; // (the last band of a 24-bit image is followed by 0xffffffff)
		count = ReadVarint(band + bmBytes, end, &used);
		if(count > bandCoefs)
			count = bandCoefs;
		planeCoefs = count / 8 / 3 * 8; // the blocks the band holds are the ones its bitmap names: Y, then Cb, then Cr of them
		memset(coef, 0, bandCoefs * sizeof(int16_t));
		b.p = band + bmBytes + used;
		b.len = next > off + bmBytes + used ? next - off - bmBytes - used : 0;
		b.pos = 0;
		b.bit = 0;
		if(dcRoot >= 0)
			DecodeDc(coef, count, &b, dcNodes, (uint32_t)dcRoot);
		if(acRoot >= 0)
		{
			b.p += b.pos;
			b.len = b.len > b.pos ? b.len - b.pos : 0;
			b.pos = 0;
			DecodeAc(coef, count, &b, acNodes, (uint32_t)acRoot);
		}
		for(c = 0; c < cols && (k + 1) * 64 <= planeCoefs; c++)
		{
			int p;
			if(!(bitmap[c >> 3] & (1u << (c & 7))))
				continue; // not in this frame: the block stays as it is
			for(p = 0; p < 3; p++)
				Idct(plane[p], coef + (uint32_t)p * planeCoefs + k * 64, q[p ? 1 : 0]);
			ToBgr(buf + ((size_t)r * 8 * padW + (size_t)c * 8) * 4, (int)padW * 4, plane[0], plane[1], plane[2]);
			k++;
		}
	}

	if(alpha == CBG2_ALPHA_NONE)
	{
		for(i = 3; i < bufLen; i += 4)
			buf[i] = 0;
	}
	else
	{
		uint32_t off = Le32(offsets + rows * 4), mode = 1;
		if(alpha == CBG2_ALPHA_MODE)
		{
			mode = off < len && len - off >= 4 ? Le32(stream + off) : 0;
			off += 4;
		}
		if(off >= len)
			ok = 0;
		else if(mode == 1)
			AlphaLz(buf, bufLen, (int)padW * 4, stream + off, len - off);
		else if(mode == 2)
			AlphaBlocks(buf, padW, cols, rows, stream + off, len - off);
		else
			ok = 0;
	}
	BGI_Free(coef);
	BGI_Free(acNodes);
	BGI_Free(dcNodes);
	return ok;
}

uint32_t Cbg2_Decode(const void* src, void* dst)
{
	const CbgHeader_t* h = (const CbgHeader_t*)src;
	const uint8_t* s = (const uint8_t*)src;
	uint8_t* table;
	uint32_t i;
	uint32_t w = h->width, hgt = h->height, bpp = h->bpp & 0xffffu;
	uint32_t padW = (w + 7) & ~7u, padH = (hgt + 7) & ~7u, bufLen = padW * padH * 4;
	uint8_t* buf;

	if(h->encLen < 128) // the two quantisation tables must be there
		return 0x80000003u;
	table = (uint8_t*)BGI_Alloc(h->encLen);
	memcpy(table, s + 0x30, h->encLen);
	CbgDecrypt(table, h->encLen, h->key);
	if(!CbgCheck(table, h->encLen, h->sum, h->xor))
	{
		BGI_Free(table);
		return 0x80000004u;
	}
	memcpy(dst, s + 0x10, 0x10); // the in-memory image header, as for version 1
	buf = (uint8_t*)BGI_Alloc(bufLen);
	memset(buf, 0, bufLen);
	// the stream: the file size is not in the header, so the readers stop at a generous bound
	if(!Cbg2_Frame(s + 0x30 + h->encLen, 0x7fffffffu, table, w, hgt, bpp == 32 ? CBG2_ALPHA_MODE : CBG2_ALPHA_NONE, buf))
		for(i = 3; i < bufLen; i += 4) // no alpha plane that can be read: opaque
			buf[i] = 0xff;
	BGI_Free(table);

	{ // the padded 32-bit rows to the tightly packed output
		uint8_t* d = (uint8_t*)dst + 0x10;
		uint32_t y, x, bytes = bpp / 8;
		for(y = 0; y < hgt; y++)
		{
			const uint8_t* row = buf + y * padW * 4;
			for(x = 0; x < w; x++, row += 4)
			{
				memcpy(d, row, bytes);
				d += bytes;
			}
		}
	}
	BGI_Free(buf);
	return 0;
}
