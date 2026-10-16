/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dscenc.c - "DSC FORMAT 1.00" compressor, for the tools that write game
 *            archives (the engine itself only decodes); interface in
 *            codec.h, the format in dsc.c
 *
 * Two passes over the input.  The first finds the matches - hash chains
 * of three-byte prefixes through the 4096-byte window, the longest of a
 * bounded number of candidates, with one byte of lookahead for a longer
 * one - and turns the data into the 512 symbols of the format: 0x000 ..
 * 0x0FF a literal byte, 0x100 + n a copy of n + 2 bytes, followed in the
 * stream by the 12-bit distance - 2.  The second builds a Huffman code
 * over the symbol counts (BGI_HuffmanLengths, limited to 16 bits; the
 * originals go up to 21), writes its lengths obfuscated with the hash
 * generator and then the symbols.  The codes are the canonical ones the
 * decoder's level-by-level tree gives: ascending by length, then by
 * symbol.
 */
#include "bgi/codec.h"
#include "bgi/deflate.h"

#define DSC_SYMBOLS   512
#define DSC_MAX_BITS  16
#define DSC_WINDOW    4096 // the distances searched: 2 .. 4096 (the format reaches 4097)
#define DSC_MIN_MATCH 3    // a two-byte copy costs more than two literals
#define DSC_MAX_MATCH 257
#define DSC_HASH_BITS 13
#define DSC_CHAIN     48 // candidates looked at per position
#define DSC_NICE      64 // a match this long is taken without looking further

typedef struct DscEnc
{
	const uint8_t* data;
	uint32_t n;
	int32_t head[1 << DSC_HASH_BITS]; // the latest position of each hash, -1 for none
	int32_t prev[DSC_WINDOW];         // the previous position with the same hash, by position modulo the window
	uint16_t* syms;                   // the symbols in order
	uint16_t* dists;                  // the distance - 2 of each copy symbol (unused for a literal)
	uint32_t count;
	uint32_t freq[DSC_SYMBOLS];
} DscEnc_t;

static uint32_t Hash3(const uint8_t* p)
{
	uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
	return (v * 0x9E3779B1u) >> (32 - DSC_HASH_BITS);
}

static void Insert(DscEnc_t* e, uint32_t i)
{
	uint32_t h = Hash3(e->data + i);
	e->prev[i & (DSC_WINDOW - 1)] = e->head[h];
	e->head[h] = (int32_t)i;
}

// the longest match for position i (0 for none of DSC_MIN_MATCH bytes) and its distance
static uint32_t FindMatch(const DscEnc_t* e, uint32_t i, uint32_t* dist)
{
	const uint8_t* data = e->data;
	uint32_t maxLen = e->n - i < DSC_MAX_MATCH ? e->n - i : DSC_MAX_MATCH;
	uint32_t best = DSC_MIN_MATCH - 1, bestDist = 0;
	int32_t cand = e->head[Hash3(data + i)];
	int chain = DSC_CHAIN;
	while(cand >= 0 && i - (uint32_t)cand <= DSC_WINDOW && chain-- > 0)
	{
		const uint8_t* a = data + cand;
		const uint8_t* b = data + i;
		int32_t older;
		// a copy cannot start at the byte before: the smallest distance is 2
		if(i - (uint32_t)cand >= 2 && a[best] == b[best] && a[0] == b[0])
		{
			uint32_t len = 0;
			while(len < maxLen && a[len] == b[len])
				len++;
			if(len > best)
			{
				best = len;
				bestDist = i - (uint32_t)cand;
				if(best >= DSC_NICE || best == maxLen)
					break;
			}
		}
		older = e->prev[cand & (DSC_WINDOW - 1)];
		if(older >= cand) // the slot was reused by a later position: the chain ends
			break;
		cand = older;
	}
	if(best < DSC_MIN_MATCH)
		return 0;
	*dist = bestDist;
	return best;
}

static void Emit(DscEnc_t* e, uint32_t sym, uint32_t dist)
{
	e->syms[e->count] = (uint16_t)sym;
	e->dists[e->count] = (uint16_t)dist;
	e->count++;
	e->freq[sym]++;
}

uint8_t* BGI_DscEncode(const void* src, uint32_t n, uint32_t key, uint32_t* size)
{
	DscEnc_t* e = (DscEnc_t*)BGI_Calloc(sizeof *e);
	uint8_t lens[DSC_SYMBOLS];
	uint32_t codes[DSC_SYMBOLS], next[DSC_MAX_BITS + 2], counts[DSC_MAX_BITS + 2];
	uint32_t i, used = 0, code = 0, k = key;
	uint64_t bits = 0, acc = 0;
	int nacc = 0;
	uint8_t *out, *p;
	DscHeader_t h;

	e->data = (const uint8_t*)src;
	e->n = n;
	e->syms = (uint16_t*)BGI_Alloc(((size_t)n + 1) * sizeof(uint16_t));
	e->dists = (uint16_t*)BGI_Alloc(((size_t)n + 1) * sizeof(uint16_t));
	memset(e->head, 0xff, sizeof e->head);
	for(i = 0; i < n;)
	{
		uint32_t dist = 0, len = 0;
		if(i + DSC_MIN_MATCH <= n)
		{
			len = FindMatch(e, i, &dist);
			Insert(e, i);
		}
		if(len)
		{
			uint32_t j;
			// while the match one byte on is longer, this byte goes out as a literal
			while(len < DSC_NICE && i + 1 + DSC_MIN_MATCH <= n)
			{
				uint32_t dist2 = 0, len2 = FindMatch(e, i + 1, &dist2);
				if(len2 <= len)
					break;
				Emit(e, e->data[i], 0);
				i++;
				Insert(e, i);
				len = len2;
				dist = dist2;
			}
			for(j = 1; j < len; j++)
				if(i + j + DSC_MIN_MATCH <= n)
					Insert(e, i + j);
			Emit(e, 0x100 + len - 2, dist - 2);
			i += len;
		}
		else
		{
			Emit(e, e->data[i], 0);
			i++;
		}
	}

	/* the code: complete even for one symbol or none (a second symbol that
	 * never occurs shares the first level), as every original stream's is */
	for(i = 0; i < DSC_SYMBOLS; i++)
		used += e->freq[i] != 0;
	for(i = 0; used < 2 && i < DSC_SYMBOLS; i++)
		if(!e->freq[i])
		{
			e->freq[i] = 1;
			used++;
		}
	BGI_HuffmanLengths(e->freq, DSC_SYMBOLS, DSC_MAX_BITS, lens);
	memset(counts, 0, sizeof counts);
	for(i = 0; i < DSC_SYMBOLS; i++)
		counts[lens[i]]++;
	counts[0] = 0;
	for(i = 1; i <= DSC_MAX_BITS; i++)
	{
		code = (code + counts[i - 1]) << 1;
		next[i] = code;
	}
	for(i = 0; i < DSC_SYMBOLS; i++)
		codes[i] = lens[i] ? next[lens[i]]++ : 0;
	for(i = 0; i < e->count; i++)
		bits += lens[e->syms[i]] + (e->syms[i] >= 0x100 ? 12u : 0u);

	*size = 0x220 + (uint32_t)((bits + 7) / 8);
	out = (uint8_t*)BGI_Calloc(*size + 1);
	memset(&h, 0, sizeof h);
	memcpy(h.magic, DSC_MAGIC, 16);
	h.key = key;
	h.outSize = n;
	h.decCount = e->count;
	memcpy(out, &h, sizeof h);
	// the code lengths, each plus the generator's next value (the decoder subtracts it)
	for(i = 0; i < DSC_SYMBOLS; i++)
		out[0x20 + i] = (uint8_t)(lens[i] + (uint8_t)BGI_HashUpdate(&k));
	// the symbols, most significant bit first
	p = out + 0x220;
	for(i = 0; i < e->count; i++)
	{
		uint32_t s = e->syms[i];
		acc = (acc << lens[s]) | codes[s];
		nacc += lens[s];
		if(s >= 0x100)
		{
			acc = (acc << 12) | e->dists[i];
			nacc += 12;
		}
		while(nacc >= 8)
		{
			*p++ = (uint8_t)(acc >> (nacc - 8));
			nacc -= 8;
		}
	}
	if(nacc)
		*p++ = (uint8_t)(acc << (8 - nacc));
	BGI_Free(e->syms);
	BGI_Free(e->dists);
	BGI_Free(e);
	return out;
}
