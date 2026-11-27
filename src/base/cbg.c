/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cbg.c - "CompressedBG___" image decoder; interface in codec.h
 *
 * The compressed image format of the game data (backgrounds, sprites, ...).
 * BGI_CbgDecode runs the stages in order:
 *
 *   * decrypt the weight table with the hash generator (CbgDecrypt)
 *   * verify the sum / xor checks of the decrypted table (CbgCheck)
 *   * read 256 variable-length (7 bits per byte) leaf weights (CbgReadWeights)
 *   * build the Huffman tree from the weights (CbgBuildTree)
 *   * Huffman-decode the bit stream (CbgHuffDecode)
 *   * expand the zero-run RLE (CbgUnrle)
 *   * undo the per-channel delta filter, average of left and up (CbgUnfilter)
 *
 * The decoded result is a RawImageHeader followed by tightly packed pixel
 * rows, which is what the bitmap manager expects from a loaded image file.
 * A version 2 file (the DCT codec of 1.573 on) is handed to cbg2.c.
 * The decoder trusts the header like the original does: there are no bounds
 * checks against a malformed file beyond the checksum of the weight table.
 */
#include "bgi/codec.h"

int BGI_IsCbg(const void* data)
{
	return memcmp(data, CBG_MAGIC, 16) == 0;
}

// buf[i] -= next hash value, for len bytes, with the generator seeded by key
void CbgDecrypt(uint8_t* buf, uint32_t len, uint32_t key)
{
	uint32_t i;
	for(i = 0; i < len; i++)
		buf[i] = (uint8_t)(buf[i] - (uint8_t)BGI_HashUpdate(&key));
}

// 1 when the byte sum and the byte xor of buf match the header's values
int CbgCheck(const uint8_t* buf, uint32_t len, uint8_t sum, uint8_t xr)
{
	uint8_t s = 0, x = 0;
	uint32_t i;
	for(i = 0; i < len; i++)
	{
		s = (uint8_t)(s + buf[i]);
		x ^= buf[i];
	}
	return s == sum && x == xr;
}

/* Variable-length unsigned integer: 7 bits per byte, LSB group first,
 * high bit set means "more bytes follow".  Advances *pp past it. */
static uint32_t CbgReadVarint(const uint8_t** pp)
{
	const uint8_t* p = *pp;
	uint32_t v = 0;
	int shift = 0;
	uint8_t b;
	do
	{
		b = *p++;
		v |= (uint32_t)(b & 0x7f) << shift;
		shift += 7;
	} while(b & 0x80);
	*pp = p;
	return v;
}

// the 256 leaf weights (one varint per byte value) from the decrypted table
static void CbgReadWeights(uint32_t* weights, const uint8_t* buf)
{
	uint32_t i;
	for(i = 0; i < 256; i++)
		weights[i] = CbgReadVarint(&buf);
}

// a node of the Huffman tree, 24 bytes

/*
 * Classic Huffman construction: repeatedly take the two valid nodes with the
 * smallest weights (the lower index on a tie) and join them under the next
 * free parent.  Nodes 0 .. leaves-1 are the leaves, the parents follow
 * (2 * leaves - 1 nodes in all).  Returns the root index, or -1 when all
 * weights are zero.  The version 2 codec (cbg2.c) builds its 16- and
 * 176-symbol trees with it.
 */
int CbgBuildTree(CbgNode_t* nodes, const uint32_t* weights, uint32_t leaves)
{
	uint32_t sum = 0, i, n, total = 2 * leaves - 1;

	for(i = 0; i < leaves; i++)
	{
		nodes[i].valid = weights[i] != 0;
		nodes[i].weight = weights[i];
		nodes[i].isParent = 0;
		nodes[i].parent = 0xffffffffu;
		nodes[i].left = i;
		nodes[i].right = i;
		sum += weights[i];
	}
	if(sum == 0)
		return -1;
	for(i = leaves; i < total; i++)
	{
		nodes[i].valid = 0;
		nodes[i].weight = 0;
		nodes[i].isParent = 1;
		nodes[i].parent = nodes[i].left = nodes[i].right = 0xffffffffu;
	}

	n = leaves;
	for(;;)
	{
		uint32_t idx[2], k;
		for(k = 0; k < 2; k++)
		{
			uint32_t best = 0xffffffffu, bestW = 0xffffffffu, j;
			for(j = 0; j < n; j++)
				if(nodes[j].valid && nodes[j].weight < bestW)
				{
					bestW = nodes[j].weight;
					best = j;
				}
			idx[k] = best;
			if(best != 0xffffffffu)
			{
				nodes[best].valid = 0;
				nodes[best].parent = n;
			}
		}
		nodes[n].valid = 1;
		// a single valid node left over has no partner
		nodes[n].weight = nodes[idx[0]].weight +
			(idx[1] != 0xffffffffu ? nodes[idx[1]].weight : 0);
		nodes[n].isParent = 1;
		nodes[n].parent = 0xffffffffu;
		nodes[n].left = idx[0];
		nodes[n].right = idx[1];
		if(nodes[n].weight == sum) // the node that carries every weight is the root
			return (int)n;
		n++;
	}
}

/* The stream's codes through a table: the next CBG_FAST_BITS bits of the
 * stream say which leaf they reach and after how many bits (most codes are
 * shorter than that), or which parent they lead to, from where the tree is
 * walked on.  The table loop reads up to three bytes ahead of the bit it is
 * at; the stream has them while more than CBG_FAST_TAIL symbols are still
 * to come (each takes a bit at least), and the last symbols are taken bit
 * by bit, so nothing behind the stream is read. */
#define CBG_FAST_BITS 12
#define CBG_FAST_TAIL 32

/* decode `count` symbols from the bit stream at src into dst, bits taken MSB
 * first; a symbol is the index of the leaf reached, i.e. the byte value */
static void CbgHuffDecode(uint8_t* dst, const uint8_t* src, const CbgNode_t* nodes,
	uint32_t root, uint32_t count)
{
	uint8_t mask = 0x80;
	uint32_t i = 0;
	if(nodes[root].isParent == 1 && count > CBG_FAST_TAIL)
	{
		uint16_t* tab = (uint16_t*)BGI_Alloc(sizeof(uint16_t) << CBG_FAST_BITS); // the bits taken << 12 | the leaf; or the parent, with 0 for the bits
		uint32_t p, fast = count - CBG_FAST_TAIL;
		uint64_t acc = 0; // the bytes read, the stream's next bit at bit `cnt` - 1
		int cnt = 0;
		for(p = 0; p < 1u << CBG_FAST_BITS; p++)
		{
			uint32_t node = root, len = 0;
			while(nodes[node].isParent == 1 && len < CBG_FAST_BITS)
			{
				node = (p >> (CBG_FAST_BITS - 1 - len)) & 1 ? nodes[node].right : nodes[node].left;
				len++;
			}
			tab[p] = (uint16_t)((nodes[node].isParent == 1 ? 0 : len << 12) | node);
		}
		for(; i < fast; i++)
		{
			uint32_t e, node;
			while(cnt < CBG_FAST_BITS)
			{
				acc = (acc << 8) | *src++;
				cnt += 8;
			}
			e = tab[(acc >> (cnt - CBG_FAST_BITS)) & ((1u << CBG_FAST_BITS) - 1)];
			node = e & 0xfff;
			if(e >> 12)
				cnt -= (int)(e >> 12);
			else
			{ // a code longer than the table's bits: on through the tree
				cnt -= CBG_FAST_BITS;
				do
				{
					if(cnt == 0)
					{
						acc = (acc << 8) | *src++;
						cnt = 8;
					}
					cnt--;
					node = (acc >> cnt) & 1 ? nodes[node].right : nodes[node].left;
				} while(nodes[node].isParent == 1);
			}
			dst[i] = (uint8_t)node;
		}
		BGI_Free(tab);
		if(cnt)
		{ // back to the byte and the bit that come next
			src -= 1 + (cnt - 1) / 8;
			mask = (uint8_t)(1u << ((cnt - 1) & 7));
		}
	}
	for(; i < count; i++)
	{
		uint32_t node = root;
		while(nodes[node].isParent == 1)
		{
			uint32_t bit = (*src & mask) ? 1 : 0;
			node = bit ? nodes[node].right : nodes[node].left;
			mask >>= 1;
			if(mask == 0)
			{
				src++;
				mask = 0x80;
			}
		}
		dst[i] = (uint8_t)node;
	}
}

/* expand the zero-run RLE: alternating runs - a literal copy first, then
 * zeros, then a copy again ... - each preceded by its varint length.
 * Returns the number of bytes produced. */
static uint32_t CbgUnrle(uint8_t* dst, const uint8_t* src, uint32_t srcLen)
{
	const uint8_t *p = src, *end = src + srcLen;
	uint8_t* out = dst;
	int copy = 1;
	while(p < end)
	{
		uint32_t len = CbgReadVarint(&p);
		if(copy)
		{
			memcpy(out, p, len);
			p += len;
		}
		else
		{
			memset(out, 0, len);
		}
		out += len;
		copy = !copy;
	}
	return (uint32_t)(out - dst);
}

/* undo the delta filter in place: each byte stores the difference to the
 * rounded-down average of its left and upper neighbour (same channel); the
 * first row and the first column use the single available neighbour, the
 * first pixel none */
static void CbgUnfilter(uint8_t* pix, const RawImageHeader_t* h)
{
	uint32_t w = h->width, hgt = h->height;
	uint32_t bpp = (h->bpp & 0xffff) >> 3; // bytes per pixel
	uint32_t stride = w * bpp;
	uint32_t i, y;
	uint8_t* d = pix;

	/* (a neighbour is read only where it exists: the original forms the
	 * index of a missing one with a 32-bit wrap-around, which would be an
	 * out-of-bounds read with 64-bit pointers) */
	if(!stride || !hgt)
		return;
	for(i = bpp; i < stride; i++) // the first row: the left neighbour alone
		d[i] = (uint8_t)(d[i] + d[i - bpp]);
	for(y = 1; y < hgt; y++)
	{
		const uint8_t* up = d;
		d += stride;
		for(i = 0; i < bpp; i++) // the first pixel: the upper neighbour alone
			d[i] = (uint8_t)(d[i] + up[i]);
		for(; i < stride; i++)
			d[i] = (uint8_t)(d[i] + ((d[i - bpp] + up[i]) >> 1));
	}
}

/* decode the CompressedBG file at src into dst (BGI_CbgOutSize(src) bytes:
 * RawImageHeader + pixels).  0 on success, 0x80000003 when the magic is
 * wrong, 0x80000004 when the checksum of the weight table fails. */
uint32_t BGI_CbgDecode(const void* src, void* dst)
{
	const CbgHeader_t* h = (const CbgHeader_t*)src;
	const uint8_t* s = (const uint8_t*)src;
	uint8_t* d = (uint8_t*)dst;
	uint8_t *table, *inter;
	uint32_t weights[256];
	CbgNode_t* nodes;
	int root;

	if(!BGI_IsCbg(src))
		return 0x80000003u;
	if(h->version == 2)
		return Cbg2_Decode(src, dst);

	table = (uint8_t*)BGI_Alloc(h->encLen);
	memcpy(table, s + 0x30, h->encLen);
	CbgDecrypt(table, h->encLen, h->key);
	if(!CbgCheck(table, h->encLen, h->sum, h->xor))
	{
		BGI_Free(table);
		return 0x80000004u;
	}

	// the 16 bytes at file offset 0x10 (width .. unused1c) become the
	// in-memory image header
	memcpy(d, s + 0x10, 0x10);

	CbgReadWeights(weights, table);
	nodes = (CbgNode_t*)BGI_Alloc(sizeof(CbgNode_t) * CBG_NODES);
	root = CbgBuildTree(nodes, weights, 256);

	// the bit stream follows the weight table; it decodes to interLen bytes of
	// RLE data, which expand to the pixel rows
	inter = (uint8_t*)BGI_Alloc(h->interLen);
	CbgHuffDecode(inter, s + 0x30 + h->encLen, nodes, (uint32_t)root, h->interLen);
	CbgUnrle(d + 0x10, inter, h->interLen);
	BGI_Free(inter);
	BGI_Free(nodes);

	CbgUnfilter(d + 0x10, (const RawImageHeader_t*)d);
	BGI_Free(table);
	return 0;
}
