/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/codec.c - unit test of the hash generator, the DSC and CBG decoders
 *                 (src/base/hash.c, dsc.c, cbg.c, declared in
 *                 src/include/bgi/codec.h) and the PackFile archive reader
 *                 (src/fs/arc.c, src/include/bgi/file.h); built as test_codec
 *                 by `make test`
 *
 * The decoders are exercised with encoders written here from the inverse
 * of their arithmetic: a DSC stream with a three-level canonical code and
 * LZ back references, and CBG images (8, 24 and 32 bits) through the delta
 * filter, the zero-run RLE and a Huffman code derived from the decoder's
 * own tree construction.  Whatever the encoder produces has to decode back
 * to the input, so a failure in those groups means the decoder and the
 * format description in codec.h disagree.
 *
 * The groups of tests, in the order they run:
 *
 *   hash - the generator on a caller-owned state and on the global state
 *          agree, its output is 15 bits wide and its state always moves
 *   dsc  - text-like data with repeats, a stream without a single match,
 *          and the longest back reference at the farthest distance
 *   cbg  - images of odd sizes and every depth with gradients, noise and
 *          flat blocks, a one-pixel image, a stream with a single symbol
 *          (the one-leaf tree), and the two error codes of the decoder
 *   arc  - a small PackFile is written and read back through the file-set
 *          manager: case-insensitive names, sizes, whole and partial
 *          reads, and every error code of the archive layer
 *
 * With BGI_GAME_DIR set to a directory holding the game's archives, every
 * DSC and CBG entry of system.arc / sysprg.arc (and any further *.arc
 * named in BGI_GAME_ARCS, space separated) is decoded as a smoke test: the
 * decoded size must match the header (programs are recognised by their
 * 16-byte header).
 *
 * The arc group writes its file under bin/, so the test runs from the
 * project root (as `make test` does).
 */
#include "bgi/codec.h"
#include "bgi/core.h"
#include "bgi/file.h"
#include "bgi/strutil.h"
#include "host/os.h"

#include <stdio.h>

#include "check.h"

/* a small deterministic generator for test data (a linear congruential generator; the
 * same sequence every run) */
static uint32_t gSeed = 12345;
static uint32_t Rnd(void)
{
	gSeed = gSeed * 1103515245u + 12345u;
	return (gSeed >> 8) & 0x7fffffffu;
}

// an MSB-first bit writer, as both decoders read their streams
typedef struct BitWriter
{
	uint8_t* buf; // the output, written byte by byte
	size_t len;   // bytes written
	uint32_t acc; // the bits not yet forming a byte, the newest in bit 0
	int nacc;     // how many of them (0 .. 7)
} BitWriter_t;

// append the low `bits` bits of `value`, most significant first
static void BwPut(BitWriter_t* w, uint32_t value, int bits)
{
	while(bits-- > 0)
	{
		w->acc = (w->acc << 1) | ((value >> bits) & 1);
		if(++w->nacc == 8)
		{
			w->buf[w->len++] = (uint8_t)w->acc;
			w->acc = 0;
			w->nacc = 0;
		}
	}
}

// pad the last byte with zero bits
static void BwFlush(BitWriter_t* w)
{
	if(w->nacc)
		BwPut(w, 0, 8 - w->nacc);
}

// ---- the hash generator ----------------------------------------------------

/* BGI_HashSeed / BGI_HashNext on the global state and BGI_HashUpdate on a
 * caller-owned state are the same arithmetic, the output fits in 15 bits
 * and the state changes on every step.  A failure means every obfuscated
 * format (DSC code lengths, CBG weight tables, the save files) decodes to
 * garbage. */
static void TestHash(OpenBGI_t* o)
{
	uint32_t key = 0x12345678, i;
	printf("hash\n");
	// the global generator and the explicit one are the same arithmetic
	BGI_HashSeed(o, key);
	for(i = 0; i < 1000; i++)
		CHECK(BGI_HashNext(o) == BGI_HashUpdate(&key));
	// the output is 15 bits wide and the state moves on every step
	key = 1;
	for(i = 0; i < 100; i++)
	{
		uint32_t before = key, v = BGI_HashUpdate(&key);
		CHECK(v < 0x8000);
		CHECK(key != before);
	}
}

// ---- DSC --------------------------------------------------------------------

/*
 * The test's code: literals 0..127 are 8 bits long, 128..255 nine bits,
 * the 256 back-reference symbols ten bits.  That is a complete code, and
 * the decoder's level-by-level layout gives it the canonical codes
 * 0..127, 256..383 and 768..1023.  Returns the code and its length for
 * symbol `sym` (0..255 a literal byte, 256 + n a back reference of n + 2
 * bytes).
 */
static void DscCodeFor(uint32_t sym, uint32_t* code, int* len)
{
	if(sym < 128)
	{
		*code = sym;
		*len = 8;
	}
	else if(sym < 256)
	{
		*code = 256 + (sym - 128);
		*len = 9;
	}
	else
	{
		*code = 768 + (sym - 256);
		*len = 10;
	}
}

/* Encode `n` bytes of `in` as a DSC file into `out`: the header, the 512
 * code lengths obfuscated with the generator seeded by `key`, then the
 * bit stream.  Greedy LZ: at each position the longest match within the
 * 12-bit window (distance 2 .. 4097, length 2 .. 257) becomes a
 * back-reference symbol followed by the 12-bit distance - 2, anything
 * shorter a literal.  Returns the encoded size and the number of symbols
 * in `*decCount` (also stored in the header). */
static size_t DscEncode(OpenBGI_t* o, uint8_t* out, const uint8_t* in, uint32_t n, uint32_t key, uint32_t* decCount)
{
	DscHeader_t* h = (DscHeader_t*)out;
	BitWriter_t w;
	uint32_t i, count = 0;

	memset(h, 0, sizeof *h);
	memcpy(h->magic, DSC_MAGIC, 16);
	h->key = key;
	h->outSize = n;

	// the obfuscated code lengths: each length plus the generator's next value (the
	// decoder subtracts it)
	BGI_HashSeed(o, key);
	for(i = 0; i < 512; i++)
	{
		uint32_t code;
		int len;
		DscCodeFor(i, &code, &len);
		out[0x20 + i] = (uint8_t)(len + (uint8_t)BGI_HashNext(o));
	}

	w.buf = out + 0x220;
	w.len = 0;
	w.acc = 0;
	w.nacc = 0;
	i = 0;
	while(i < n)
	{
		uint32_t bestLen = 0, bestDist = 0, dist, code;
		int len;
		for(dist = 2; dist <= 4097 && dist <= i; dist++)
		{
			uint32_t l = 0;
			while(l < 257 && i + l < n && in[i + l] == in[i + l - dist])
				l++;
			if(l > bestLen)
			{
				bestLen = l;
				bestDist = dist;
			}
		}
		if(bestLen >= 2)
		{
			DscCodeFor(256 + bestLen - 2, &code, &len);
			BwPut(&w, code, len);
			BwPut(&w, bestDist - 2, 12);
			i += bestLen;
		}
		else
		{
			DscCodeFor(in[i], &code, &len);
			BwPut(&w, code, len);
			i++;
		}
		count++;
	}
	BwFlush(&w);
	h->decCount = count;
	*decCount = count;
	return 0x220 + w.len;
}

/* Three DSC streams through BGI_IsDsc, BGI_DscOutSize and BGI_DscDecode:
 * 20000 bytes of text-like data with repeats and bytes of both literal
 * code lengths (the decode must use back references: fewer symbols than
 * bytes), 600 bytes without a single match, and a run that needs the
 * longest back reference (257 bytes) at the farthest distance (4097).
 * Each has to decode to exactly the input. */
static void TestDsc(OpenBGI_t* o)
{
	enum
	{
		N = 20000
	};
	uint8_t* in = (uint8_t*)malloc(N);
	uint8_t* enc = (uint8_t*)malloc(0x220 + N * 2 + 16);
	uint8_t* dec = (uint8_t*)malloc(N);
	uint32_t i, count, size;
	printf("dsc\n");

	// text-like data with plenty of repeats, bytes of both code lengths (the UTF-8 bytes
	// are above 0x7f)
	for(i = 0; i < N; i++)
	{
		if((i & 0x3ff) < 0x80)
			in[i] = (uint8_t)(Rnd() & 0xff); // noise
		else
			in[i] = (uint8_t)("the quick brown fox 日本語"[Rnd() % 25]); // "Japanese"
	}
	size = (uint32_t)DscEncode(o, enc, in, N, 0x8f3a, &count);
	CHECK(BGI_IsDsc(enc));
	CHECK(BGI_DscOutSize(enc) == N);
	CHECK(count < N); // the back references did something
	memset(dec, 0xaa, N);
	CHECK(BGI_DscDecode(o, enc, dec) == N);
	CHECK(memcmp(dec, in, N) == 0);
	BGI_UNUSED(size);

	// a stream without any match: every symbol a literal
	for(i = 0; i < 600; i++)
		in[i] = (uint8_t)(i * 7 + (i >> 3));
	size = (uint32_t)DscEncode(o, enc, in, 600, 1, &count);
	CHECK(BGI_DscDecode(o, enc, dec) == 600);
	CHECK(memcmp(dec, in, 600) == 0);

	// the longest back reference (257 bytes) and the farthest distance (4097)
	memset(in, 0x5a, 4097);
	for(i = 4097; i < 4097 + 257; i++)
		in[i] = 0x5a;
	in[4097 + 257] = 1;
	size = (uint32_t)DscEncode(o, enc, in, 4097 + 258, 77, &count);
	CHECK(BGI_DscDecode(o, enc, dec) == 4097 + 258);
	CHECK(memcmp(dec, in, 4097 + 258) == 0);

	free(in);
	free(enc);
	free(dec);
}

// ---- CBG --------------------------------------------------------------------

/* a node of the Huffman tree, the decoder's construction repeated here to derive the
 * codes */
typedef struct TNode
{
	uint32_t valid, weight, isParent, parent, left, right; // `valid`: still to be joined; indices are 0xffffffff for none
} TNode_t;

/* Build the tree as the decoder does from the 256 leaf weights: the two
 * valid nodes with the smallest weights (the lower index on a tie) are
 * joined under the next parent from 256 on, the lighter as the left
 * child, until a parent carries the total weight.  Parent links are kept
 * for CbgCodeFor.  Returns the root index. */
static int CbgTree(TNode_t* nodes, const uint32_t* weights)
{
	uint32_t sum = 0, i, n;
	for(i = 0; i < 256; i++)
	{
		nodes[i].valid = weights[i] != 0;
		nodes[i].weight = weights[i];
		nodes[i].isParent = 0;
		nodes[i].parent = 0xffffffffu;
		nodes[i].left = nodes[i].right = i;
		sum += weights[i];
	}
	for(i = 256; i < 511; i++)
	{
		nodes[i].valid = 0;
		nodes[i].weight = 0;
		nodes[i].isParent = 1;
		nodes[i].parent = nodes[i].left = nodes[i].right = 0xffffffffu;
	}
	n = 256;
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
		nodes[n].weight = nodes[idx[0]].weight + (idx[1] != 0xffffffffu ? nodes[idx[1]].weight : 0);
		nodes[n].left = idx[0];
		nodes[n].right = idx[1];
		if(nodes[n].weight == sum)
			return (int)n;
		n++;
	}
}

/* the code of a leaf: the path from the root, read off the parent links (a right turn is
 * a 1 bit), MSB first */
static void CbgCodeFor(const TNode_t* nodes, uint32_t leaf, uint32_t* code, int* len)
{
	uint32_t bits[64];
	int n = 0;
	uint32_t node = leaf;
	while(nodes[node].parent != 0xffffffffu)
	{
		uint32_t p = nodes[node].parent;
		bits[n++] = nodes[p].right == node && nodes[p].left != node ? 1 : 0;
		node = p;
	}
	*code = 0;
	*len = n;
	while(n-- > 0)
		*code = (*code << 1) | bits[n];
}

/* write `v` as a varint (7 bits per byte, least significant group first, bit 7 set while
 * more follow) and advance *pp */
static void PutVarint(uint8_t** pp, uint32_t v)
{
	uint8_t* p = *pp;
	do
	{
		uint8_t b = (uint8_t)(v & 0x7f);
		v >>= 7;
		if(v)
			b |= 0x80;
		*p++ = b;
	} while(v);
	*pp = p;
}

/* Encode a `w` x `h` image of `bpp` bits per pixel (tightly packed rows
 * in `pix`) as a CompressedBG file into `out`: the delta filter, the RLE
 * (alternating literal / zero runs, a literal first), the Huffman stream
 * with weights = symbol counts, and the weight table encrypted with the
 * generator seeded by `key` in front of it, with its sum and xor checks
 * in the header.  Returns the encoded size. */
static size_t CbgEncode(uint8_t* out, const uint8_t* pix, uint32_t w, uint32_t h, uint32_t bpp, uint32_t key)
{
	uint32_t bytesPP = bpp >> 3, stride = w * bytesPP, n = stride * h;
	uint8_t* filtered = (uint8_t*)malloc(n);
	uint8_t* rle = (uint8_t*)malloc(n * 2 + 16);
	uint8_t* table = (uint8_t*)malloc(256 * 5);
	uint32_t weights[256];
	TNode_t* nodes = (TNode_t*)malloc(sizeof(TNode_t) * 511);
	CbgHeader_t* hdr = (CbgHeader_t*)out;
	uint8_t *p, *q;
	uint32_t x, y, c, i, rleLen, tableLen, k;
	uint8_t sum = 0, xr = 0;
	BitWriter_t bw;

	// the delta filter: difference to the rounded-down average of left and up (one of
	// them alone at an edge, none at the corner)
	for(y = 0; y < h; y++)
		for(x = 0; x < w; x++)
			for(c = 0; c < bytesPP; c++)
			{
				uint32_t off = y * stride + x * bytesPP + c;
				int up = y ? pix[off - stride] : -1;
				int left = x ? pix[off - bytesPP] : -1;
				int pred;
				if(up >= 0)
					pred = left >= 0 ? (left + up) >> 1 : up;
				else
					pred = left >= 0 ? left : 0;
				filtered[off] = (uint8_t)(pix[off] - pred);
			}

	// the RLE: literal run, zero run, literal run, ...; a literal run ends where two
	// zeros start, so a lone zero stays literal
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
				PutVarint(&p, len);
				memcpy(p, filtered + i, len);
				p += len;
			}
			else
			{
				while(i + len < n && filtered[i + len] == 0)
					len++;
				PutVarint(&p, len);
			}
			i += len;
			copy = !copy;
		}
	}
	rleLen = (uint32_t)(p - rle);

	// weights = counts of the RLE bytes (at least one symbol must have a weight)
	memset(weights, 0, sizeof weights);
	for(i = 0; i < rleLen; i++)
		weights[rle[i]]++;
	{
		int root = CbgTree(nodes, weights);
		bw.buf = out + 0x30;
		bw.len = 0;
		bw.acc = 0;
		bw.nacc = 0;
		q = table;
		for(i = 0; i < 256; i++)
			PutVarint(&q, weights[i]);
		tableLen = (uint32_t)(q - table);
		for(i = 0; i < tableLen; i++)
		{
			sum = (uint8_t)(sum + table[i]);
			xr ^= table[i];
		}
		// the table goes in front of the stream, encrypted (each byte plus the
		// generator's next value)
		k = key;
		for(i = 0; i < tableLen; i++)
			out[0x30 + i] = (uint8_t)(table[i] + (uint8_t)BGI_HashUpdate(&k));
		bw.buf = out + 0x30 + tableLen;
		for(i = 0; i < rleLen; i++)
		{
			uint32_t code;
			int len;
			if(root == (int)rle[i])
				len = 0; // a single-symbol tree: the root is the leaf, no bits
			else
				CbgCodeFor(nodes, rle[i], &code, &len);
			if(len)
				BwPut(&bw, code, len);
		}
		BwFlush(&bw);
	}

	memset(hdr, 0, sizeof *hdr);
	memcpy(hdr->magic, CBG_MAGIC, 16);
	hdr->width = (uint16_t)w;
	hdr->height = (uint16_t)h;
	hdr->bpp = bpp;
	hdr->unused18 = 0x11223344; // copied verbatim into the decoded header: checked there
	hdr->unused1c = 0x55667788;
	hdr->interLen = rleLen;
	hdr->key = key;
	hdr->encLen = tableLen;
	hdr->sum = sum;
	hdr->xor = xr;
	hdr->version = 1;

	free(filtered);
	free(rle);
	free(table);
	free(nodes);
	return 0x30 + tableLen + bw.len;
}

/* One image through CbgEncode and BGI_CbgDecode: `pattern` 0 draws
 * gradients, 1 noise, anything else flat 8 x 8 blocks (long zero runs
 * after the filter).  The decoder must accept the file, announce the
 * right size, reproduce the header fields and the pixels exactly, refuse
 * a damaged weight table with 0x80000004 and a wrong magic with
 * 0x80000003. */
static void TestCbgImage(uint32_t w, uint32_t h, uint32_t bpp, int pattern)
{
	uint32_t n = w * h * (bpp >> 3), x, y, c;
	uint8_t* pix = (uint8_t*)malloc(n);
	uint8_t* enc = (uint8_t*)malloc(0x30 + 256 * 5 + n * 3 + 64);
	uint8_t* dec = (uint8_t*)malloc(0x10 + n);
	size_t size;
	for(y = 0; y < h; y++)
		for(x = 0; x < w; x++)
			for(c = 0; c < (bpp >> 3); c++)
			{
				uint8_t v;
				switch(pattern)
				{
					case 0: v = (uint8_t)((x * 3 + y * 5 + c * 40) & 0xff); break; // gradients
					case 1: v = (uint8_t)(Rnd() & 0xff); break;                    // noise
					default: v = (uint8_t)((x / 8 + y / 8) & 1 ? 200 : 20); break; // flat blocks: long zero runs
				}
				pix[(y * w + x) * (bpp >> 3) + c] = v;
			}
	size = CbgEncode(enc, pix, w, h, bpp, 0x4242 + w);
	CHECK(BGI_IsCbg(enc));
	CHECK(BGI_CbgOutSize(enc) == 0x10 + n);
	memset(dec, 0xcc, 0x10 + n);
	CHECK(BGI_CbgDecode(enc, dec) == 0);
	CHECK(((RawImageHeader_t*)dec)->width == w);
	CHECK(((RawImageHeader_t*)dec)->height == h);
	CHECK(((RawImageHeader_t*)dec)->bpp == bpp);
	CHECK(((RawImageHeader_t*)dec)->unused08 == 0x11223344);
	CHECK(memcmp(dec + 0x10, pix, n) == 0);

	// a damaged table fails the checksum; a wrong magic is refused before that
	enc[0x30] ^= 0x10;
	CHECK(BGI_CbgDecode(enc, dec) == 0x80000004u);
	enc[0x30] ^= 0x10;
	enc[0] = 'X';
	CHECK(BGI_CbgDecode(enc, dec) == 0x80000003u);
	BGI_UNUSED(size);
	free(pix);
	free(enc);
	free(dec);
}

/* the CBG cases: odd sizes, every depth, every pattern, the smallest image and the
 * one-leaf tree */
static void TestCbg(void)
{
	printf("cbg\n");
	TestCbgImage(37, 23, 8, 0);
	TestCbgImage(64, 48, 24, 0);
	TestCbgImage(50, 31, 32, 1);
	TestCbgImage(40, 40, 24, 2);
	TestCbgImage(1, 1, 8, 1);
	TestCbgImage(16, 2, 8, 2); // a single symbol in the stream: the one-leaf tree
}

/* The unpackers' table loops and their bit-by-bit ends: streams of every
 * small size and some large ones, plain, repetitive and with byte values
 * so unevenly spread that codes grow longer than the tables' 12 bits, each
 * unpacked from a block of exactly the stream's size - a build watched for
 * memory errors (SAN=address) then sees a read behind the stream. */
static uint32_t gFastSeed;

static uint32_t FastRnd(void)
{
	gFastSeed = gFastSeed * 1103515245u + 12345u;
	return gFastSeed >> 8;
}

static void FastFill(uint8_t* p, uint32_t n, int kind)
{
	uint32_t i;
	for(i = 0; i < n; i++)
		if(kind == 0)
			p[i] = (uint8_t)FastRnd(); // plain
		else if(kind == 1)
			p[i] = (uint8_t)(i % 7 == 0 ? FastRnd() : i / 5); // runs and repeats
		else
		{ // value k about half as often as k - 1: long codes for the rare ones
			uint32_t r = FastRnd() | 0x800000u, k = 0;
			while(!(r & 1))
			{
				r >>= 1;
				k++;
			}
			p[i] = (uint8_t)(k * 11 + (FastRnd() % 23 == 0 ? FastRnd() % 200 : 0));
		}
}

static void TestFastOne(uint32_t n, int kind)
{
	uint8_t *in = (uint8_t*)malloc(0x10 + n + 1), *out = (uint8_t*)malloc(0x10 + n + 1), *enc, *own;
	uint32_t size, w = n < 64 ? n : 64, h = n < 64 ? 1 : n / 64;
	FastFill(in + 0x10, n, kind);
	// DSC: the bytes as they are
	enc = BGI_DscEncode(in + 0x10, n, 0x77 + n, &size);
	own = (uint8_t*)malloc(size);
	memcpy(own, enc, size);
	out[n] = 0x5a;
	CHECK(BGI_DscDecode(NULL, own, out) == n && memcmp(out, in + 0x10, n) == 0 && out[n] == 0x5a);
	free(own);
	BGI_Free(enc);
	// CompressedBG: the bytes as an 8-bit picture
	memset(in, 0, 0x10);
	in[0] = (uint8_t)w;
	in[2] = (uint8_t)h;
	in[3] = (uint8_t)(h >> 8);
	in[4] = 8;
	enc = BGI_CbgEncode(in, 0x99 + n, &size);
	own = (uint8_t*)malloc(size);
	memcpy(own, enc, size);
	out[0x10 + w * h] = 0x5a;
	CHECK(BGI_CbgDecode(own, out) == 0 && memcmp(out, in, 0x10 + w * h) == 0 && out[0x10 + w * h] == 0x5a);
	free(own);
	BGI_Free(enc);
	free(in);
	free(out);
}

static void TestFast(void)
{
	uint32_t n;
	int kind;
	printf("fast\n");
	gFastSeed = 4711;
	for(kind = 0; kind < 3; kind++)
	{
		for(n = 1; n <= 200; n++)
			TestFastOne(n, kind);
		for(n = 1000; n <= 300000; n = n * 7 / 2 + 13)
			TestFastOne(n, kind);
	}
}

// ---- the archive reader --------------------------------------------------

/* Write a PackFile with three entries of mixed-case names and sizes 100,
 * 1 and 3000 to bin/, then read it through a FileSet_t chain: names are
 * found regardless of case, sizes and whole / partial reads return the
 * stored bytes, and the error codes come back for a missing entry
 * (ARC_ERR_NOT_FOUND), a size beyond the entry (ARC_ERR_OFFSET), a range
 * running past its end (ARC_ERR_RANGE) and an archive that does not
 * exist (ARC_ERR_OPEN), after which the first archive still answers.  A
 * failure means the index parsing, the case folding or the chain of
 * nodes changed. */
static void TestArc(OpenBGI_t* o)
{
	static const char* path = "bin/test_codec_tmp.arc";
	static const char* names[3] = {"First.dat", "second", "ThIrD.bin"};
	static const uint32_t sizes[3] = {100, 1, 3000};
	FileSet_t* mgr;
	File_t f;
	uint8_t hdr[16];
	uint8_t* data;
	uint32_t i, off = 0, total = 0, r;
	char arcName[0x104];
	printf("arc\n");

	for(i = 0; i < 3; i++)
		total += sizes[i];
	data = (uint8_t*)malloc(total);
	for(i = 0; i < total; i++)
		data[i] = (uint8_t)(i * 13 + 7);

	// "PackFile    " + count, 32-byte entries (offsets from the end of the index), then
	// the data
	File_Ctor(&f);
	CHECK(File_Create(o, &f, path));
	memcpy(hdr, PACKFILE_MAGIC, 12);
	i = 3;
	memcpy(hdr + 12, &i, 4);
	File_Write(&f, hdr, 16);
	for(i = 0; i < 3; i++)
	{
		PackEntry_t e;
		memset(&e, 0, sizeof e);
		strcpy(e.name, names[i]);
		e.offset = off;
		e.size = sizes[i];
		File_Write(&f, &e, sizeof e);
		off += sizes[i];
	}
	File_Write(&f, data, total);
	File_Close(&f);
	File_Dtor(&f);

	mgr = FileSet_New();
	CHECK(ArcMgr_Exists(o, mgr, path, "first.dat"));
	CHECK(ArcMgr_Exists(o, mgr, path, "FIRST.DAT")); // names are compared lower-cased
	CHECK(!ArcMgr_Exists(o, mgr, path, "fourth"));
	CHECK(ArcMgr_SizeOf(o, mgr, path, "third.bin") == 3000);
	CHECK(ArcMgr_SizeOf(o, mgr, path, "nope") == ARC_ERR_NOT_FOUND);
	CHECK(ArcMgr_ArcNameFor(o, mgr, arcName, path, "second") == 1);
	CHECK(strcmp(arcName, path) == 0);
	{
		uint8_t buf[3000];
		r = ArcMgr_Read(o, mgr, buf, path, "second");
		CHECK(r == 1 && buf[0] == data[100]); // the second entry starts after the 100 bytes of the first
		r = ArcMgr_Read(o, mgr, buf, path, "third.bin");
		CHECK(r == 3000 && memcmp(buf, data + 101, 3000) == 0);
		r = ArcMgr_ReadRange(o, mgr, buf, path, "third.bin", 10, 20);
		CHECK(r == 20 && memcmp(buf, data + 111, 20) == 0);
		CHECK(ArcMgr_ReadRange(o, mgr, buf, path, "third.bin", 0, 3001) == ARC_ERR_OFFSET); // a size larger than the entry
		CHECK(ArcMgr_ReadRange(o, mgr, buf, path, "third.bin", 2990, 20) == ARC_ERR_RANGE); // offset + size beyond the entry
		CHECK(ArcMgr_Read(o, mgr, buf, path, "missing") == ARC_ERR_NOT_FOUND);
	}
	// a second archive name opens a new node in the chain; a missing file is ARC_ERR_OPEN
	CHECK(ArcMgr_Read(o, mgr, NULL, "bin/does_not_exist.arc", "x") == ARC_ERR_OPEN);
	CHECK(ArcMgr_Exists(o, mgr, path, "first.dat")); // the first node still answers
	FileSet_Delete(o, mgr);
	free(data);
	remove(path);
}

// ---- optional: the game's own archives -------------------------------------

/* Decode every entry of the archive `arc` in `dir`: a DSC entry has to
 * decode to its announced size, a CBG entry has to decode without an
 * error; other entries are only read.  Prints a count of scripts (DSC
 * entries that decode to a program with the 16-byte header), images and
 * others, or "not found" when the archive is not there. */
static void SmokeArchive(OpenBGI_t* o, const char* dir, const char* arc)
{
	char path[0x200];
	FileSet_t* mgr = FileSet_New();
	int i, dsc = 0, cbg = 0, other = 0;
	snprintf(path, sizeof path, "%s/%s", dir, arc);
	if(!mgr->vt->matches(o, mgr, path)) // an unloaded node adopts the path: this loads the index
	{
		printf("  %s: not found\n", arc);
		FileSet_Delete(o, mgr);
		return;
	}
	for(i = 0; i < mgr->count; i++)
	{
		const ArcEntry_t* e = &mgr->entries[i];
		uint8_t* raw = (uint8_t*)malloc(e->size + 16);
		uint32_t r = ArcMgr_Read(o, mgr, raw, path, e->name);
		CHECK(r == e->size);
		if(e->size >= 0x220 && BGI_IsDsc(raw))
		{
			uint32_t outSize = BGI_DscOutSize(raw);
			uint8_t* out = (uint8_t*)malloc(outSize + 16);
			CHECK(BGI_DscDecode(o, raw, out) == outSize);
			// a program of this engine version starts with its 16-byte header (u32 header
			// size = 0x10)
			if(outSize >= 16 && out[0] == 0x10 && out[1] == 0 && out[2] == 0 && out[3] == 0)
				dsc++;
			else
				other++;
			free(out);
		}
		else if(e->size >= 0x30 && BGI_IsCbg(raw))
		{
			uint32_t outSize = BGI_CbgOutSize(raw);
			uint8_t* out = (uint8_t*)malloc(outSize + 16);
			CHECK(BGI_CbgDecode(raw, out) == 0);
			cbg++;
			free(out);
		}
		else
			other++;
		free(raw);
	}
	printf("  %s: %d entries, %d scripts, %d images, %d other\n", arc, mgr->count, dsc, cbg, other);
	FileSet_Delete(o, mgr);
}

// the smoke test over the game's archives; does nothing without BGI_GAME_DIR
static void TestGameData(OpenBGI_t* o)
{
	const char* dir = getenv("BGI_GAME_DIR");
	const char* arcs = getenv("BGI_GAME_ARCS");
	if(!dir)
		return;
	printf("game data in %s\n", dir);
	SmokeArchive(o, dir, "system.arc");
	SmokeArchive(o, dir, "sysprg.arc");
	if(arcs)
	{
		char list[0x400];
		char* tok;
		snprintf(list, sizeof list, "%s", arcs);
		for(tok = strtok(list, " "); tok; tok = strtok(NULL, " "))
			SmokeArchive(o, dir, tok);
	}
}

// run every group; exit status 1 when any check failed
int main(void)
{
	OpenBGI_t* o = OpenBGI_New(OS_Host()); // the engine whose code the test runs
	if(!o)
		return 1;
	OS_Init();
	TestHash(o);
	TestDsc(o);
	TestCbg();
	TestFast();
	TestArc(o);
	TestGameData(o);
	OS_Shutdown();
	if(gFails)
	{
		printf("%d failure(s)\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
