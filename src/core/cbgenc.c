/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cbgenc.c - "CompressedBG___" version 1 encoder, for the tools that write
 *            game archives (the engine itself only decodes); interface in
 *            codec.h, the format in cbg.c
 *
 * The decoder's steps in reverse: the delta filter (each byte minus the
 * rounded-down average of its left and upper neighbours), the zero-run
 * coding (alternating literal and zero runs, each with a varint length, a
 * literal run first), and a Huffman code over the bytes of that stream
 * whose weights are the byte counts - the decoder rebuilds the tree from
 * the weights, so the codes here are read off the tree CbgBuildTree makes
 * from the same table.  The weight table goes in front of the bit stream
 * as 256 varints, encrypted with the hash generator, with its sum and xor
 * in the header.
 */
#include "bgi/codec.h"

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

uint8_t* BGI_CbgEncode(const void* image, uint32_t key, uint32_t* size)
{
	const RawImageHeader_t* ih = (const RawImageHeader_t*)image;
	const uint8_t* pix = (const uint8_t*)image + sizeof *ih;
	uint32_t bytesPP = (ih->bpp & 0xffffu) >> 3, stride = (uint32_t)ih->width * bytesPP, n = stride * ih->height;
	uint8_t* filtered = (uint8_t*)BGI_Alloc((size_t)n + 1);
	uint8_t* rle = (uint8_t*)BGI_Alloc((size_t)n + (size_t)n / 2 + 16); // a literal run costs its length plus a varint
	uint8_t table[256 * 5];
	uint32_t weights[256], x, y, c, i, rleLen, tableLen, k = key;
	uint64_t codes[256], bits = 0, acc = 0;
	uint8_t lens[256];
	CbgNode_t* nodes = (CbgNode_t*)BGI_Alloc(sizeof(CbgNode_t) * CBG_NODES);
	CbgHeader_t hdr;
	uint8_t *out, *p, *q;
	int nacc = 0;

	// the delta filter
	for(y = 0; y < ih->height; y++)
	{
		const uint8_t* row = pix + (size_t)y * stride;
		uint8_t* dst = filtered + (size_t)y * stride;
		for(x = 0; x < ih->width; x++)
			for(c = 0; c < bytesPP; c++)
			{
				uint32_t o = x * bytesPP + c;
				int up = y ? row[(ptrdiff_t)o - (ptrdiff_t)stride] : -1;
				int left = x ? row[o - bytesPP] : -1;
				int pred;
				if(up >= 0)
					pred = left >= 0 ? (left + up) >> 1 : up;
				else
					pred = left >= 0 ? left : 0;
				dst[o] = (uint8_t)(row[o] - pred);
			}
	}

	/* the runs: literal, zeros, literal, ...; a literal run ends where two
	 * zeros begin, so a lone zero stays a literal (a run of one zero would
	 * cost two length bytes) */
	p = rle;
	i = 0;
	{
		int copy = 1;
		while(i < n)
		{
			uint32_t len = 0;
			if(copy)
			{
				while(i + len < n && !(filtered[i + len] == 0 && i + len + 1 < n && filtered[i + len + 1] == 0))
					len++;
				p = PutVarint(p, len);
				memcpy(p, filtered + i, len);
				p += len;
			}
			else
			{
				while(i + len < n && filtered[i + len] == 0)
					len++;
				p = PutVarint(p, len);
			}
			i += len;
			copy = !copy;
		}
	}
	rleLen = (uint32_t)(p - rle);
	BGI_Free(filtered);

	// the weights, and each byte's code as the path from the root (a right turn is a 1 bit)
	memset(weights, 0, sizeof weights);
	for(i = 0; i < rleLen; i++)
		weights[rle[i]]++;
	memset(lens, 0, sizeof lens);
	memset(codes, 0, sizeof codes);
	if(CbgBuildTree(nodes, weights, 256) >= 0)
		for(i = 0; i < 256; i++)
		{
			uint32_t node = i;
			uint64_t code = 0;
			int len = 0;
			if(!weights[i])
				continue;
			while(nodes[node].parent != 0xffffffffu)
			{
				uint32_t parent = nodes[node].parent;
				code |= (uint64_t)(nodes[parent].right == node && nodes[parent].left != node) << len;
				len++;
				node = parent;
			}
			lens[i] = (uint8_t)len; // at most 63: a deeper tree needs more bytes than an image has
			codes[i] = code;        // the leaf's bit is bit 0: written from the top bit down
			bits += (uint64_t)len * weights[i];
		}

	q = table;
	for(i = 0; i < 256; i++)
		q = PutVarint(q, weights[i]);
	tableLen = (uint32_t)(q - table);

	memset(&hdr, 0, sizeof hdr);
	memcpy(hdr.magic, CBG_MAGIC, 16);
	hdr.width = ih->width;
	hdr.height = ih->height;
	hdr.bpp = ih->bpp;
	hdr.unused18 = ih->unused08;
	hdr.unused1c = ih->unused0c;
	hdr.interLen = rleLen;
	hdr.key = key;
	hdr.encLen = tableLen;
	hdr.version = 1;
	for(i = 0; i < tableLen; i++)
	{
		hdr.sum = (uint8_t)(hdr.sum + table[i]);
		hdr.xor ^= table[i];
	}

	*size = 0x30 + tableLen + (uint32_t)((bits + 7) / 8);
	out = (uint8_t*)BGI_Calloc((size_t)*size + 1);
	memcpy(out, &hdr, sizeof hdr);
	for(i = 0; i < tableLen; i++) // the decoder subtracts the generator's values
		out[0x30 + i] = (uint8_t)(table[i] + (uint8_t)BGI_HashUpdate(&k));
	p = out + 0x30 + tableLen;
	for(i = 0; i < rleLen; i++)
	{
		int len = lens[rle[i]];
		uint64_t code = codes[rle[i]];
		while(len-- > 0)
		{
			acc = (acc << 1) | ((code >> len) & 1);
			if(++nacc == 8)
			{
				*p++ = (uint8_t)acc;
				acc = 0;
				nacc = 0;
			}
		}
	}
	if(nacc)
		*p++ = (uint8_t)(acc << (8 - nacc));
	BGI_Free(rle);
	BGI_Free(nodes);
	return out;
}
