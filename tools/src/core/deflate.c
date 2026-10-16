/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * deflate.c - zlib streams (RFC 1950 / 1951) and CRC-32; interface in
 *             deflate.h
 *
 * The compressor: a hash table of three-byte prefixes with chains through
 * a 32 KB window finds the matches (the longest of a bounded number of
 * candidates, and from level 4 on the match one byte later when that is
 * longer); the literals and matches of up to 32768 symbols form a block
 * with its own pair of Huffman codes, limited to 15 bits, whose lengths are
 * sent run-length coded under a third code - or a stored block when the
 * data does not compress.  The decompressor reads the three block types
 * with a nine-bit lookup table in front of the canonical decoding.
 */
#include "bgi/deflate.h"

// ---- CRC-32 and Adler-32 ---------------------------------------------------------------------

static uint32_t gCrcTable[8][256]; // slicing by eight: table k advances a byte k positions further on
static int gCrcReady;

static void CrcInit(void)
{
	uint32_t i, k;
	for(i = 0; i < 256; i++)
	{
		uint32_t c = i;
		for(k = 0; k < 8; k++)
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		gCrcTable[0][i] = c;
	}
	for(i = 0; i < 256; i++)
		for(k = 1; k < 8; k++)
			gCrcTable[k][i] = (gCrcTable[k - 1][i] >> 8) ^ gCrcTable[0][gCrcTable[k - 1][i] & 0xff];
	gCrcReady = 1;
}

uint32_t BGI_Crc32(uint32_t crc, const void* data, size_t n)
{
	const uint8_t* p = (const uint8_t*)data;
	if(!gCrcReady)
		CrcInit();
	crc = ~crc;
	while(n >= 8)
	{
		uint32_t a = crc ^ ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
		uint32_t b = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
		crc = gCrcTable[7][a & 0xff] ^ gCrcTable[6][(a >> 8) & 0xff] ^ gCrcTable[5][(a >> 16) & 0xff] ^ gCrcTable[4][a >> 24] ^
			gCrcTable[3][b & 0xff] ^ gCrcTable[2][(b >> 8) & 0xff] ^ gCrcTable[1][(b >> 16) & 0xff] ^ gCrcTable[0][b >> 24];
		p += 8;
		n -= 8;
	}
	while(n--)
		crc = gCrcTable[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

// the check value of a zlib stream: two 16-bit sums modulo 65521
static uint32_t Adler32(const uint8_t* p, size_t n)
{
	uint32_t a = 1, b = 0;
	while(n)
	{
		size_t k = n < 5552 ? n : 5552; // the most bytes before the sums can overflow 32 bits
		n -= k;
		while(k--)
		{
			a += *p++;
			b += a;
		}
		a %= 65521u;
		b %= 65521u;
	}
	return (b << 16) | a;
}

// ---- the tables of the format ---------------------------------------------------------------------

#define LIT_CODES  286 // literals, the end-of-block code 256, the length codes 257 .. 285
#define DIST_CODES 30
#define CL_CODES   19 // the code of the code lengths
#define MAX_BITS   15
#define END_BLOCK  256
#define MIN_MATCH  3
#define MAX_MATCH  258
#define WINDOW     32768

static const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t kClOrder[CL_CODES] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15}; // the order the code-length code's lengths are sent in

static uint8_t gLenCode[MAX_MATCH + 1]; // match length -> index into kLenBase
static uint8_t gDistCode[512];          // distance - 1 below 256, else 256 + ((distance - 1) >> 7) -> index into kDistBase
static int gCodesReady;

static void CodesInit(void)
{
	int i, k;
	for(i = 0; i < 29; i++)
		for(k = kLenBase[i]; k <= MAX_MATCH && (i == 28 || k < kLenBase[i + 1]); k++)
			gLenCode[k] = (uint8_t)i;
	gLenCode[MAX_MATCH] = 28;
	for(i = 0; i < 30; i++)
	{
		int end = i == 29 ? 32768 : kDistBase[i + 1] - 1; // the last distance of the code
		for(k = kDistBase[i]; k <= end; k++)
		{
			if(k <= 256)
				gDistCode[k - 1] = (uint8_t)i;
			else
				gDistCode[256 + ((k - 1) >> 7)] = (uint8_t)i;
		}
	}
	gCodesReady = 1;
}

static int DistCode(uint32_t dist)
{
	return dist <= 256 ? gDistCode[dist - 1] : gDistCode[256 + ((dist - 1) >> 7)];
}

// ---- the output ---------------------------------------------------------------------------------

typedef struct Out
{
	uint8_t* buf;
	size_t len, cap;
	uint64_t acc; // bits not yet written, the next one in bit 0
	int nacc;
	int failed; // an allocation failed: the output is dropped
} Out_t;

static void OutByte(Out_t* o, uint8_t b)
{
	if(o->len == o->cap)
	{
		size_t cap = o->cap ? o->cap * 2 : 4096;
		uint8_t* buf = o->failed ? NULL : (uint8_t*)realloc(o->buf, cap);
		if(!buf)
		{
			o->failed = 1;
			return;
		}
		o->buf = buf;
		o->cap = cap;
	}
	o->buf[o->len++] = b;
}

// append the low `n` bits of `v`, least significant first (n <= 32)
static void PutBits(Out_t* o, uint32_t v, int n)
{
	o->acc |= (uint64_t)v << o->nacc;
	o->nacc += n;
	while(o->nacc >= 8)
	{
		OutByte(o, (uint8_t)o->acc);
		o->acc >>= 8;
		o->nacc -= 8;
	}
}

// pad to a byte boundary with zero bits
static void FlushBits(Out_t* o)
{
	if(o->nacc)
		OutByte(o, (uint8_t)o->acc);
	o->acc = 0;
	o->nacc = 0;
}

// ---- Huffman codes ---------------------------------------------------------------------------------

typedef struct SymFreq
{
	uint32_t freq;
	uint16_t sym;
} SymFreq_t;

static int CompareFreq(const void* a, const void* b)
{
	const SymFreq_t* x = (const SymFreq_t*)a;
	const SymFreq_t* y = (const SymFreq_t*)b;
	if(x->freq != y->freq)
		return x->freq < y->freq ? -1 : 1;
	return (int)x->sym - (int)y->sym;
}

/* The code lengths of an optimal prefix code for the `n` ascending weights
 * in A, in place (Moffat and Katajainen's algorithm): A[i] becomes the
 * length of the code of weight i. */
static void MinimumRedundancy(uint32_t* A, int n)
{
	int root, leaf, next, avbl, used, dpth;
	if(n == 0)
		return;
	if(n == 1)
	{
		A[0] = 1;
		return;
	}
	A[0] += A[1];
	root = 0;
	leaf = 2;
	for(next = 1; next < n - 1; next++)
	{
		if(leaf >= n || A[root] < A[leaf])
		{
			A[next] = A[root];
			A[root++] = (uint32_t)next;
		}
		else
			A[next] = A[leaf++];
		if(leaf >= n || (root < next && A[root] < A[leaf]))
		{
			A[next] += A[root];
			A[root++] = (uint32_t)next;
		}
		else
			A[next] += A[leaf++];
	}
	A[n - 2] = 0;
	for(next = n - 3; next >= 0; next--)
		A[next] = A[A[next]] + 1;
	avbl = 1;
	used = dpth = 0;
	root = n - 2;
	next = n - 1;
	while(avbl > 0)
	{
		while(root >= 0 && (int)A[root] == dpth)
		{
			used++;
			root--;
		}
		while(avbl > used)
		{
			A[next--] = (uint32_t)dpth;
			avbl--;
		}
		avbl = 2 * used;
		dpth++;
		used = 0;
	}
}

/* The code lengths of deflate.h: an optimal code first; a code longer than
 * the limit is then cut to it and others are lengthened until the lengths
 * are a complete code again. */
void BGI_HuffmanLengths(const uint32_t* freq, int n, int maxBits, uint8_t* lens)
{
	SymFreq_t syms[HUFFMAN_SYMBOLS_MAX];
	uint32_t work[HUFFMAN_SYMBOLS_MAX];
	int counts[64];
	int used = 0, i, j, k;
	uint32_t total;
	memset(lens, 0, (size_t)n);
	memset(work, 0, sizeof work);
	for(i = 0; i < n; i++)
		if(freq[i])
		{
			syms[used].freq = freq[i];
			syms[used].sym = (uint16_t)i;
			used++;
		}
	if(used == 0)
		return;
	qsort(syms, (size_t)used, sizeof syms[0], CompareFreq);
	for(i = 0; i < used; i++)
		work[i] = syms[i].freq;
	MinimumRedundancy(work, used);
	memset(counts, 0, sizeof counts);
	for(i = 0; i < used; i++)
		counts[work[i] < 63 ? work[i] : 63]++;
	// everything beyond the limit is cut to it, then the Kraft sum is brought back to one
	for(i = maxBits + 1; i < 64; i++)
	{
		counts[maxBits] += counts[i];
		counts[i] = 0;
	}
	total = 0;
	for(i = maxBits; i > 0; i--)
		total += (uint32_t)counts[i] << (maxBits - i);
	while(total > (1u << maxBits))
	{
		counts[maxBits]--;
		for(i = maxBits - 1; i > 0; i--)
			if(counts[i])
			{
				counts[i]--;
				counts[i + 1] += 2;
				break;
			}
		total--;
	}
	// the shortest codes go to the most frequent symbols: the end of the sorted list
	j = used;
	for(i = 1; i <= maxBits; i++)
		for(k = counts[i]; k > 0; k--)
			lens[syms[--j].sym] = (uint8_t)i;
}

// the canonical codes of the lengths, bit-reversed for the least-significant-first output
static void BuildCodes(const uint8_t* lens, int n, uint16_t* codes)
{
	uint32_t next[MAX_BITS + 2], code = 0;
	int counts[MAX_BITS + 2];
	int i, b;
	memset(counts, 0, sizeof counts);
	for(i = 0; i < n; i++)
		counts[lens[i]]++;
	counts[0] = 0;
	for(b = 1; b <= MAX_BITS; b++)
	{
		code = (code + (uint32_t)counts[b - 1]) << 1;
		next[b] = code;
	}
	for(i = 0; i < n; i++)
	{
		uint32_t c, r = 0;
		if(!lens[i])
		{
			codes[i] = 0;
			continue;
		}
		c = next[lens[i]]++;
		for(b = 0; b < lens[i]; b++, c >>= 1)
			r = (r << 1) | (c & 1);
		codes[i] = (uint16_t)r;
	}
}

// ---- the compressor ---------------------------------------------------------------------------------

#define HASH_BITS  15
#define HASH_SIZE  (1 << HASH_BITS)
#define BLOCK_SYMS 32768 // literals and matches per block

typedef struct Deflater
{
	const uint8_t* data;
	size_t n;
	Out_t out;
	int32_t* head;     // HASH_SIZE: the latest position of each hash, -1 for none
	int32_t* prev;     // WINDOW: the previous position with the same hash, by position modulo the window
	uint16_t* symLit;  // BLOCK_SYMS: a literal, or a match length
	uint16_t* symDist; // BLOCK_SYMS: 0 for a literal, else the distance
	int symCount;
	uint32_t litFreq[LIT_CODES];
	uint32_t distFreq[DIST_CODES];
	size_t blockStart; // the input position the open block starts at
	int maxChain;      // candidates looked at per position
	int nice;          // a match this long is taken at once
	int lazy;          // look one byte ahead for a longer match
} Deflater_t;

static uint32_t Hash3(const uint8_t* p)
{
	uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
	return (v * 0x9E3779B1u) >> (32 - HASH_BITS);
}

// enter position i (which has three bytes) into the hash chains
static void Insert(Deflater_t* d, size_t i)
{
	uint32_t h = Hash3(d->data + i);
	d->prev[i & (WINDOW - 1)] = d->head[h];
	d->head[h] = (int32_t)i;
}

// the longest match for position i among the chain's candidates: its length (0 for none worth taking) and distance
static int FindMatch(const Deflater_t* d, size_t i, uint32_t* dist)
{
	const uint8_t* data = d->data;
	size_t maxLen = d->n - i < MAX_MATCH ? d->n - i : MAX_MATCH;
	int best = MIN_MATCH - 1, chain = d->maxChain;
	int32_t cand = d->head[Hash3(data + i)];
	uint32_t bestDist = 0;
	while(cand >= 0 && i - (size_t)cand <= WINDOW && chain-- > 0)
	{
		const uint8_t* a = data + cand;
		const uint8_t* b = data + i;
		int32_t older;
		if(a[best] == b[best] && a[0] == b[0] && a[1] == b[1])
		{
			size_t len = 2;
			while(len < maxLen && a[len] == b[len])
				len++;
			if((int)len > best)
			{
				best = (int)len;
				bestDist = (uint32_t)(i - (size_t)cand);
				if(best >= d->nice || (size_t)best == maxLen)
					break;
			}
		}
		older = d->prev[cand & (WINDOW - 1)];
		if(older >= cand) // the slot was reused by a later position: the chain ends
			break;
		cand = older;
	}
	if(best < MIN_MATCH || (best == MIN_MATCH && bestDist > 4096)) // a far three-byte match costs more than three literals
		return 0;
	*dist = bestDist;
	return best;
}

// a stored block (or several: one holds at most 65535 bytes) of data[from, to)
static void WriteStored(Deflater_t* d, size_t from, size_t to, int final)
{
	Out_t* o = &d->out;
	do
	{
		size_t k = to - from < 65535 ? to - from : 65535;
		size_t j;
		PutBits(o, (uint32_t)(final && from + k == to), 1);
		PutBits(o, 0, 2);
		FlushBits(o);
		OutByte(o, (uint8_t)k);
		OutByte(o, (uint8_t)(k >> 8));
		OutByte(o, (uint8_t)~k);
		OutByte(o, (uint8_t)(~k >> 8));
		for(j = 0; j < k; j++)
			OutByte(o, d->data[from + j]);
		from += k;
	} while(from < to);
}

/* close the open block, which covers the input up to `end`: a dynamic
 * Huffman block of the collected symbols, or a stored block when that takes
 * fewer bits */
static void FlushBlock(Deflater_t* d, size_t end, int final)
{
	Out_t* o = &d->out;
	uint8_t litLens[LIT_CODES], distLens[DIST_CODES], clLens[CL_CODES];
	uint16_t litCodes[LIT_CODES], distCodes[DIST_CODES], clCodes[CL_CODES];
	uint8_t seq[LIT_CODES + DIST_CODES]; // the two length lists in one
	uint8_t rle[LIT_CODES + DIST_CODES]; // code-length symbols
	uint8_t rleExtra[LIT_CODES + DIST_CODES];
	uint32_t clFreq[CL_CODES];
	int hlit, hdist, hclen, total, rleCount = 0, i;
	uint64_t bits;

	d->litFreq[END_BLOCK]++;
	BGI_HuffmanLengths(d->litFreq, LIT_CODES, MAX_BITS, litLens);
	BGI_HuffmanLengths(d->distFreq, DIST_CODES, MAX_BITS, distLens);
	for(hlit = LIT_CODES; hlit > 257 && !litLens[hlit - 1]; hlit--)
		;
	for(hdist = DIST_CODES; hdist > 1 && !distLens[hdist - 1]; hdist--)
		;
	memcpy(seq, litLens, (size_t)hlit);
	memcpy(seq + hlit, distLens, (size_t)hdist);
	total = hlit + hdist;
	// runs: 16 repeats the previous length 3 .. 6 times, 17 / 18 are 3 .. 10 / 11 .. 138 zeros
	memset(clFreq, 0, sizeof clFreq);
	for(i = 0; i < total;)
	{
		int len = seq[i], run = 1;
		while(i + run < total && seq[i + run] == len)
			run++;
		if(len == 0 && run >= 3)
		{
			int k = run > 138 ? 138 : run;
			rle[rleCount] = (uint8_t)(k >= 11 ? 18 : 17);
			rleExtra[rleCount++] = (uint8_t)(k >= 11 ? k - 11 : k - 3);
			i += k;
		}
		else if(len != 0 && run >= 4)
		{
			int k = run - 1 > 6 ? 6 : run - 1;
			rle[rleCount] = (uint8_t)len;
			rleExtra[rleCount++] = 0;
			rle[rleCount] = 16;
			rleExtra[rleCount++] = (uint8_t)(k - 3);
			i += 1 + k;
		}
		else
		{
			rle[rleCount] = (uint8_t)len;
			rleExtra[rleCount++] = 0;
			i++;
		}
	}
	for(i = 0; i < rleCount; i++)
		clFreq[rle[i]]++;
	BGI_HuffmanLengths(clFreq, CL_CODES, 7, clLens);
	for(hclen = CL_CODES; hclen > 4 && !clLens[kClOrder[hclen - 1]]; hclen--)
		;

	// what the block costs coded, against stored
	bits = 3 + 5 + 5 + 4 + (uint64_t)hclen * 3;
	for(i = 0; i < rleCount; i++)
		bits += clLens[rle[i]] + (rle[i] == 16 ? 2u : rle[i] == 17 ? 3u
										 : rle[i] == 18            ? 7u
																   : 0u);
	for(i = 0; i < LIT_CODES; i++)
		bits += (uint64_t)d->litFreq[i] * (litLens[i] + (i > 256 ? kLenExtra[i - 257] : 0u));
	for(i = 0; i < DIST_CODES; i++)
		bits += (uint64_t)d->distFreq[i] * (distLens[i] + (uint32_t)kDistExtra[i]);
	if(bits > ((uint64_t)(end - d->blockStart) + 5 * ((end - d->blockStart) / 65535 + 1)) * 8)
		WriteStored(d, d->blockStart, end, final);
	else
	{
		BuildCodes(litLens, LIT_CODES, litCodes);
		BuildCodes(distLens, DIST_CODES, distCodes);
		BuildCodes(clLens, CL_CODES, clCodes);
		PutBits(o, (uint32_t) final, 1);
		PutBits(o, 2, 2);
		PutBits(o, (uint32_t)(hlit - 257), 5);
		PutBits(o, (uint32_t)(hdist - 1), 5);
		PutBits(o, (uint32_t)(hclen - 4), 4);
		for(i = 0; i < hclen; i++)
			PutBits(o, clLens[kClOrder[i]], 3);
		for(i = 0; i < rleCount; i++)
		{
			PutBits(o, clCodes[rle[i]], clLens[rle[i]]);
			if(rle[i] == 16)
				PutBits(o, rleExtra[i], 2);
			else if(rle[i] == 17)
				PutBits(o, rleExtra[i], 3);
			else if(rle[i] == 18)
				PutBits(o, rleExtra[i], 7);
		}
		for(i = 0; i < d->symCount; i++)
		{
			uint32_t lit = d->symLit[i], dist = d->symDist[i];
			if(!dist)
				PutBits(o, litCodes[lit], litLens[lit]);
			else
			{
				int lc = gLenCode[lit], dc = DistCode(dist);
				PutBits(o, litCodes[257 + lc], litLens[257 + lc]);
				if(kLenExtra[lc])
					PutBits(o, lit - kLenBase[lc], kLenExtra[lc]);
				PutBits(o, distCodes[dc], distLens[dc]);
				if(kDistExtra[dc])
					PutBits(o, dist - kDistBase[dc], kDistExtra[dc]);
			}
		}
		PutBits(o, litCodes[END_BLOCK], litLens[END_BLOCK]);
	}
	d->symCount = 0;
	memset(d->litFreq, 0, sizeof d->litFreq);
	memset(d->distFreq, 0, sizeof d->distFreq);
	d->blockStart = end;
}

// add a literal (dist 0) or a match to the open block; `next` is the input position after it
static void Emit(Deflater_t* d, uint32_t lit, uint32_t dist, size_t next)
{
	d->symLit[d->symCount] = (uint16_t)lit;
	d->symDist[d->symCount] = (uint16_t)dist; // 32768 fits
	d->symCount++;
	if(dist)
	{
		d->litFreq[257 + gLenCode[lit]]++;
		d->distFreq[DistCode(dist)]++;
	}
	else
		d->litFreq[lit]++;
	if(d->symCount == BLOCK_SYMS)
		FlushBlock(d, next, 0);
}

uint8_t* Deflate_Compress(const uint8_t* data, size_t n, int level, size_t* outSize)
{
	static const int kChain[10] = {0, 4, 6, 8, 16, 24, 32, 64, 128, 512};
	static const int kNice[10] = {0, 8, 16, 32, 32, 64, 128, 128, 258, 258};
	Deflater_t d;
	uint32_t adler;
	size_t i;
	if(!gCodesReady)
		CodesInit();
	level = level < 0 ? 0 : level > 9 ? 9
									  : level;
	memset(&d, 0, sizeof d);
	d.data = data;
	d.n = n;
	d.maxChain = kChain[level];
	d.nice = kNice[level];
	d.lazy = level >= 4;
	OutByte(&d.out, 0x78); // deflate, a 32 KB window
	OutByte(&d.out, level >= 7 ? 0xDA : level >= 5 ? 0x9C
			: level >= 2                           ? 0x5E
												   : 0x01);
	if(level == 0 || n == 0)
		WriteStored(&d, 0, n, 1);
	else
	{
		d.head = (int32_t*)malloc(sizeof(int32_t) * HASH_SIZE);
		d.prev = (int32_t*)malloc(sizeof(int32_t) * WINDOW);
		d.symLit = (uint16_t*)malloc(sizeof(uint16_t) * BLOCK_SYMS);
		d.symDist = (uint16_t*)malloc(sizeof(uint16_t) * BLOCK_SYMS);
		if(!d.head || !d.prev || !d.symLit || !d.symDist)
			d.out.failed = 1;
		else
		{
			memset(d.head, 0xff, sizeof(int32_t) * HASH_SIZE);
			for(i = 0; i < n;)
			{
				uint32_t dist = 0;
				int len = 0;
				if(i + MIN_MATCH <= n)
				{
					len = FindMatch(&d, i, &dist);
					Insert(&d, i);
				}
				if(len)
				{
					int k;
					// while the match one byte on is longer, this byte goes out as a literal
					while(d.lazy && len < d.nice && i + 1 + MIN_MATCH <= n)
					{
						uint32_t dist2 = 0;
						int len2 = FindMatch(&d, i + 1, &dist2);
						if(len2 <= len)
							break;
						Emit(&d, data[i], 0, i + 1);
						i++;
						Insert(&d, i);
						len = len2;
						dist = dist2;
					}
					for(k = 1; k < len; k++)
						if(i + (size_t)k + MIN_MATCH <= n)
							Insert(&d, i + (size_t)k);
					i += (size_t)len;
					Emit(&d, (uint32_t)len, dist, i);
				}
				else
				{
					i++;
					Emit(&d, data[i - 1], 0, i);
				}
			}
			FlushBlock(&d, n, 1);
		}
		free(d.head);
		free(d.prev);
		free(d.symLit);
		free(d.symDist);
	}
	FlushBits(&d.out);
	adler = Adler32(data, n);
	OutByte(&d.out, (uint8_t)(adler >> 24));
	OutByte(&d.out, (uint8_t)(adler >> 16));
	OutByte(&d.out, (uint8_t)(adler >> 8));
	OutByte(&d.out, (uint8_t)adler);
	if(d.out.failed)
	{
		free(d.out.buf);
		return NULL;
	}
	*outSize = d.out.len;
	return d.out.buf;
}

// ---- the decompressor ---------------------------------------------------------------------------------

#define FAST_BITS 9

typedef struct In
{
	const uint8_t* p;
	const uint8_t* end;
	uint64_t acc; // bits read ahead, the next one in bit 0
	int nacc;
	int over; // zero bytes supplied beyond the end of the input
} In_t;

/* make at least `n` bits available (n <= 32); beyond the end of the input
 * zero bytes are supplied and counted, because the decoder looks ahead by
 * more than it uses: Overrun says whether bits of them were consumed */
static void Need(In_t* in, int n)
{
	while(in->nacc < n)
	{
		if(in->p < in->end)
			in->acc |= (uint64_t)*in->p++ << in->nacc;
		else
			in->over++;
		in->nacc += 8;
	}
}

static int Overrun(const In_t* in)
{
	return in->nacc < in->over * 8;
}

static uint32_t GetBits(In_t* in, int n)
{
	uint32_t v;
	if(n == 0)
		return 0;
	Need(in, n);
	v = (uint32_t)(in->acc & ((1u << n) - 1u));
	in->acc >>= n;
	in->nacc -= n;
	return v;
}

typedef struct Huff
{
	uint16_t fast[1 << FAST_BITS];  // by the next FAST_BITS bits: length << 12 | symbol, 0 for a longer code
	uint16_t count[MAX_BITS + 1];   // codes of each length
	uint16_t symbol[LIT_CODES + 2]; // the symbols in canonical order
} Huff_t;

// build the decoding tables of `n` code lengths; 0 when the lengths are over-subscribed
static int HuffBuild(Huff_t* h, const uint8_t* lens, int n)
{
	uint16_t offs[MAX_BITS + 2];
	uint32_t code = 0;
	int i, b, left = 1;
	memset(h, 0, sizeof *h);
	for(i = 0; i < n; i++)
		h->count[lens[i]]++;
	h->count[0] = 0;
	for(b = 1; b <= MAX_BITS; b++)
	{
		left = (left << 1) - h->count[b];
		if(left < 0)
			return 0;
	}
	offs[1] = 0;
	for(b = 1; b < MAX_BITS; b++)
		offs[b + 1] = (uint16_t)(offs[b] + h->count[b]);
	for(i = 0; i < n; i++)
		if(lens[i])
			h->symbol[offs[lens[i]]++] = (uint16_t)i;
	// the short codes into the lookup table, at every index whose low bits are the reversed code
	i = 0;
	for(b = 1; b <= FAST_BITS; b++)
	{
		int k;
		for(k = 0; k < h->count[b]; k++, i++, code++)
		{
			uint32_t r = 0, c = code, j;
			int t;
			for(t = 0; t < b; t++, c >>= 1)
				r = (r << 1) | (c & 1);
			for(j = r; j < (1u << FAST_BITS); j += 1u << b)
				h->fast[j] = (uint16_t)((b << 12) | h->symbol[i]);
		}
		code <<= 1;
	}
	return 1;
}

// the next symbol, -1 when the bits are no code
static int HuffDecode(In_t* in, const Huff_t* h)
{
	uint32_t e;
	int code = 0, first = 0, index = 0, len;
	Need(in, MAX_BITS);
	e = h->fast[in->acc & ((1u << FAST_BITS) - 1u)];
	if(e)
	{
		in->acc >>= e >> 12;
		in->nacc -= (int)(e >> 12);
		return (int)(e & 0xfff);
	}
	for(len = 1; len <= MAX_BITS; len++)
	{
		int count = h->count[len];
		code |= (int)((in->acc >> (len - 1)) & 1);
		if(code - count < first)
		{
			in->acc >>= len;
			in->nacc -= len;
			return h->symbol[index + (code - first)];
		}
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

// the growing output of the decompressor
typedef struct Buf
{
	uint8_t* p;
	size_t len, cap;
} Buf_t;

static int BufRoom(Buf_t* b, size_t more)
{
	if(b->len + more > b->cap)
	{
		size_t cap = b->cap ? b->cap : 4096;
		uint8_t* p;
		while(cap < b->len + more)
			cap *= 2;
		p = (uint8_t*)realloc(b->p, cap);
		if(!p)
			return 0;
		b->p = p;
		b->cap = cap;
	}
	return 1;
}

// the symbols of one block under the two codes; 0 on a bad code, a bad distance or no memory
static int InflateCodes(In_t* in, Buf_t* out, const Huff_t* lit, const Huff_t* dist)
{
	for(;;)
	{
		int sym = HuffDecode(in, lit);
		if(sym < 0 || Overrun(in))
			return 0;
		if(sym < 256)
		{
			if(!BufRoom(out, 1))
				return 0;
			out->p[out->len++] = (uint8_t)sym;
		}
		else if(sym == END_BLOCK)
			return 1;
		else
		{
			uint32_t len, d;
			int ds;
			size_t k;
			sym -= 257;
			if(sym >= 29)
				return 0;
			len = kLenBase[sym] + GetBits(in, kLenExtra[sym]);
			ds = HuffDecode(in, dist);
			if(ds < 0 || ds >= 30)
				return 0;
			d = kDistBase[ds] + GetBits(in, kDistExtra[ds]);
			if(d > out->len || !BufRoom(out, len))
				return 0;
			for(k = 0; k < len; k++, out->len++) // byte-wise: the copy may overlap its own output
				out->p[out->len] = out->p[out->len - d];
		}
	}
}

uint8_t* Deflate_Uncompress(const uint8_t* data, size_t n, size_t hint, size_t* outSize)
{
	In_t in;
	Buf_t out;
	Huff_t* lit;
	Huff_t* dist;
	int final, ok = 1;
	if(n < 6 || (data[0] & 0x0f) != 8 || ((data[0] << 8) | data[1]) % 31 != 0 || (data[1] & 0x20))
		return NULL;
	memset(&in, 0, sizeof in);
	in.p = data + 2;
	in.end = data + n;
	memset(&out, 0, sizeof out);
	lit = (Huff_t*)malloc(sizeof *lit);
	dist = (Huff_t*)malloc(sizeof *dist);
	if(!lit || !dist || !BufRoom(&out, hint ? hint : 4096))
		ok = 0;
	while(ok)
	{
		uint32_t type;
		final = (int)GetBits(&in, 1);
		type = GetBits(&in, 2);
		if(type == 0)
		{ // stored: the bits to the byte boundary are dropped, then LEN, ~LEN and the bytes
			uint32_t len, nlen;
			GetBits(&in, in.nacc & 7);
			len = GetBits(&in, 16);
			nlen = GetBits(&in, 16);
			if((len ^ nlen) != 0xffff || Overrun(&in) || !BufRoom(&out, len))
				ok = 0;
			else
			{
				uint32_t k;
				for(k = 0; k < len && ok; k++)
				{
					out.p[out.len++] = (uint8_t)GetBits(&in, 8);
					if(Overrun(&in))
						ok = 0;
				}
			}
		}
		else if(type == 1)
		{ // the fixed codes
			uint8_t lens[288 + 30];
			int i;
			for(i = 0; i < 288; i++)
				lens[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9
						: i < 280                         ? 7
														  : 8);
			for(i = 0; i < 30; i++)
				lens[288 + i] = 5;
			HuffBuild(lit, lens, 288);
			HuffBuild(dist, lens + 288, 30);
			ok = InflateCodes(&in, &out, lit, dist);
		}
		else if(type == 2)
		{ // the codes are in the block: their lengths under the code-length code
			uint8_t lens[288 + 32], clLens[CL_CODES];
			int hlit = (int)GetBits(&in, 5) + 257, hdist = (int)GetBits(&in, 5) + 1, hclen = (int)GetBits(&in, 4) + 4, i;
			memset(clLens, 0, sizeof clLens);
			for(i = 0; i < hclen; i++)
				clLens[kClOrder[i]] = (uint8_t)GetBits(&in, 3);
			if(hlit > 286 || hdist > 30 || !HuffBuild(lit, clLens, CL_CODES))
				ok = 0;
			for(i = 0; ok && i < hlit + hdist;)
			{
				int sym = HuffDecode(&in, lit), rep = 0, val = 0;
				if(sym < 0 || Overrun(&in))
					ok = 0;
				else if(sym < 16)
					lens[i++] = (uint8_t)sym;
				else
				{
					if(sym == 16)
					{
						if(i == 0)
							ok = 0;
						else
							val = lens[i - 1];
						rep = 3 + (int)GetBits(&in, 2);
					}
					else if(sym == 17)
						rep = 3 + (int)GetBits(&in, 3);
					else
						rep = 11 + (int)GetBits(&in, 7);
					if(i + rep > hlit + hdist)
						ok = 0;
					while(ok && rep--)
						lens[i++] = (uint8_t)val;
				}
			}
			if(ok && (!HuffBuild(lit, lens, hlit) || !HuffBuild(dist, lens + hlit, hdist)))
				ok = 0;
			if(ok)
				ok = InflateCodes(&in, &out, lit, dist);
		}
		else
			ok = 0;
		if(!ok || final)
			break;
	}
	if(ok)
	{ // the check value follows at the next byte boundary, most significant byte first
		uint32_t adler = 0;
		int i;
		GetBits(&in, in.nacc & 7);
		for(i = 0; i < 4; i++)
			adler = (adler << 8) | GetBits(&in, 8);
		if(Overrun(&in) || adler != Adler32(out.p, out.len))
			ok = 0;
	}
	free(lit);
	free(dist);
	if(!ok)
	{
		free(out.p);
		return NULL;
	}
	*outSize = out.len;
	return out.p;
}
