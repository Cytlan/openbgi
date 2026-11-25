/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/bfmovie.c - unit test of the BF_Movie frame decoder (src/base/
 *                   bfmovie.c, src/include/bgi/bfmovie.h) and the sequence
 *                   registry behind "90 F4" .. "90 F7" (src/gfx/bmseq.c,
 *                   src/include/bgi/gfx/bmseq.h); built as test_bfmovie by
 *                   `make test`
 *
 * The test has no sample movie to rely on, so it carries an encoder written
 * as the inverse of the decoder: a two-frame picture is turned into the
 * key frame (through the neighbour-average filter) and a delta frame with
 * the three pixel kinds (a delta against the previous frame, a transparent
 * pixel, a displaced copy), each frame packed as a single literal run and
 * Huffman-coded with weights = byte counts, the codes derived from the
 * decoder's own tree construction.  The decoded frames must then match
 * the pictures the encoder started from.
 *
 * The groups of tests, each run at 32 and at 24 bits per pixel:
 *
 *   movie   - BfMovie_Check / IsSimple / DecodeFrame on the built file:
 *             both frames decode to the source pictures, a frame index
 *             out of range is refused
 *   registry - BmSeq_Register / Decode / Clone / Free: the info array, a
 *             decode into a slot of the movie's size and mode, every
 *             result code of the original, and the clone keeping the data
 *             alive after the first number is freed
 *   waits   - the wait objects of "90 F4" (the load on the loader thread)
 *             and of the deferred "90 F6" decode, polled as the scheduler
 *             would, with the values they push on the thread's stack
 *
 *   streamed - a movie of the streamed form put together byte by byte
 *             (three frames of 16 x 8 pixels: a whole one, one that
 *             changes a block's colour and the other block's alpha, one
 *             that changes nothing): each frame's pixels; the same in a
 *             target that is no whole number of blocks or has wider rows;
 *             the form whose alpha plane has no leading word; a 24-bit
 *             one; a frame cut short; the registry's decode
 *
 * With BGI_BM_FILE naming a real .bm file every frame of it is decoded as
 * a smoke test (and the picture after the last frame written to BGI_BM_OUT
 * as a PPM when set).
 *
 * main runs in a scratch directory in the temporary directory that it
 * removes afterwards, because the wait tests write files there and read
 * them through the engine's drive, which OS_SetGameDir makes of it.
 */
#include "bgi/bfmovie.h"
#include "bgi/bytes.h"
#include "bgi/core.h"
#include "bgi/gfx/bmseq.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/vm.h"
#include "bgi/wait.h"
#include "bgi/waitobj.h"
#include "bgi/file.h"
#include "host/os.h"

#include "testhost.h"

#include <stdio.h>

#include "check.h"

// ---- the encoder ---------------------------------------------------------------------

/* write `v` as a varint (7 bits per byte, least significant group first, bit 7 set while
 * more follow); returns the byte count */
static size_t PutVarint(uint8_t* out, uint32_t v)
{
	size_t n = 0;
	do
	{
		uint8_t b = (uint8_t)(v & 0x7f);
		v >>= 7;
		out[n++] = (uint8_t)(b | (v ? 0x80 : 0));
	} while(v);
	return n;
}

// a node of the Huffman tree: 0..255 the symbol leaves, 256..510 the inner nodes
typedef struct TNode
{
	int valid;               // still to be picked (a leaf with a weight, or an inner node not yet joined)
	uint8_t sym;             // the byte value of a leaf
	uint32_t weight;         // the byte count of a leaf, the sum of its children for an inner node
	int left, right, parent; // node indices, -1 for none; `right` is the lighter child
} TNode_t;

/* Build the Huffman tree exactly as the decoder does from the 256 weights:
 * inner nodes are numbered down from 510, each joining the two lightest
 * valid nodes (the lightest as the right child), until the sum reaches the
 * total weight or the inner node numbers are used up.  Parent links are
 * kept so that CodeFor can walk from a leaf to the root.  Returns the
 * index of the root. */
static int Tree(TNode_t* nodes, const uint32_t* weights)
{
	uint32_t total = 0;
	int i, parent = 510, root;
	for(i = 0; i < 256; i++)
	{
		nodes[i].valid = weights[i] != 0;
		nodes[i].sym = (uint8_t)i;
		nodes[i].weight = weights[i];
		nodes[i].left = nodes[i].right = nodes[i].parent = -1;
		total += weights[i];
	}
	for(i = 256; i < 511; i++)
	{
		nodes[i].valid = 0;
		nodes[i].sym = 0;
		nodes[i].weight = 0;
		nodes[i].left = nodes[i].right = nodes[i].parent = -1;
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
			for(j = 0; j < 511; j++)
				if(nodes[j].valid && nodes[j].weight < best)
				{
					best = nodes[j].weight;
					pick[k] = j;
				}
			if(pick[k] >= 0)
			{
				nodes[pick[k]].valid = 0;
				nodes[pick[k]].parent = parent;
				sum += nodes[pick[k]].weight;
			}
		}
		nodes[parent].valid = sum != 0;
		nodes[parent].weight = sum;
		nodes[parent].left = pick[1];
		nodes[parent].right = pick[0];
		root = parent;
		if(sum == total || parent <= 256)
			break;
		parent--;
	}
	return root;
}

/* The code of symbol leaf `leaf`: the path from the root, one bit per
 * level (1 = the right, lighter child), in `*code` with the root's bit in
 * bit 0 and its length in `*len`. */
static void CodeFor(const TNode_t* nodes, int leaf, uint32_t* code, int* len)
{
	uint32_t bits = 0;
	int n = 0, node = leaf;
	while(nodes[node].parent >= 0)
	{
		int p = nodes[node].parent;
		bits |= (uint32_t)(nodes[p].right == node ? 1 : 0) << n; // bit 1 = the lighter child
		n++;
		node = p;
	}
	// the bits were collected leaf-first; the decoder reads root-first
	*len = n;
	*code = 0;
	for(node = 0; node < n; node++)
		*code |= ((bits >> (n - 1 - node)) & 1) << node;
}

/* Pack one frame into `out`: varint N, the 256 varint weights, then the
 * bit stream (low bit first) of the N Huffman-coded bytes.  The N bytes
 * are a 32-bit length followed by `payload`, the run-length stream.  The
 * length written is the size of the run-length stream itself; the
 * decoder expands the stream into that many bytes and pads the two or
 * three it does not fill with 0x80, past the pixels, so the surplus is
 * harmless.  Returns the number of bytes written. */
static size_t PackFrame(uint8_t* out, const uint8_t* payload, uint32_t payloadLen)
{
	uint8_t* huff = (uint8_t*)malloc(payloadLen + 4);
	uint32_t weights[256], i, n = payloadLen + 4;
	TNode_t nodes[511];
	size_t off = 0;
	uint32_t acc = 0;
	int accBits = 0;
	memcpy(huff, &payloadLen, 4); // the length field in front of the run-length stream
	memcpy(huff + 4, payload, payloadLen);
	memset(weights, 0, sizeof weights);
	for(i = 0; i < n; i++)
		weights[huff[i]]++;
	Tree(nodes, weights);
	off += PutVarint(out + off, n);
	for(i = 0; i < 256; i++)
		off += PutVarint(out + off, weights[i]);
	for(i = 0; i < n; i++)
	{
		uint32_t code;
		int len;
		CodeFor(nodes, huff[i], &code, &len);
		acc |= code << accBits;
		accBits += len;
		while(accBits >= 8)
		{
			out[off++] = (uint8_t)acc;
			acc >>= 8;
			accBits -= 8;
		}
	}
	if(accBits)
		out[off++] = (uint8_t)acc;
	free(huff);
	return off;
}

/* the run-length layer: one (fill 0, copy `len`) pair, so a single literal run holding
 * everything */
static size_t Runs(uint8_t* out, const uint8_t* data, uint32_t len)
{
	size_t off = PutVarint(out, 0);
	off += PutVarint(out + off, len);
	memcpy(out + off, data, len);
	return off + len;
}

#define W 20 // the test picture's width in pixels (not a multiple of 8: the right blocks are partial)
#define H 12 // its height in pixels (likewise: the bottom blocks are 4 rows high)

/* the source picture: a deterministic ARGB value for (x, y) that differs between the two
 * frames */
static uint32_t Pixel(int x, int y, int frame)
{
	uint32_t r = (uint32_t)(x * 11 + y * 3 + frame * 7) & 0xff, g = (uint32_t)(x * 5 + y * 13) & 0xff,
			 b = (uint32_t)(x + y * 17 + frame) & 0xff, a = (uint32_t)(x * y + 100) & 0xff;
	return b | (g << 8) | (r << 16) | (a << 24);
}

/* The key frame's payload: `pix` as rows of `bpp` / 8 bytes per pixel,
 * padded to 4 bytes, each byte minus the decoder's prediction (the
 * average of the left and upper neighbours, one of them alone at an edge,
 * 0 at the corner).  Returns the payload size. */
static size_t KeyPayload(uint8_t* out, const uint32_t* pix, int bpp)
{
	int bytes = bpp / 8, channels = bytes == 4 ? 4 : 3, x, y, c;
	size_t stride = ((size_t)bytes * W + 3) & ~(size_t)3, off = 0;
	memset(out, 0, stride * H);
	for(y = 0; y < H; y++)
		for(x = 0; x < W; x++)
			for(c = 0; c < channels; c++)
			{
				int cur = (int)((pix[y * W + x] >> (8 * c)) & 0xff), pred;
				int left = x ? (int)((pix[y * W + x - 1] >> (8 * c)) & 0xff) : -1;
				int up = y ? (int)((pix[(y - 1) * W + x] >> (8 * c)) & 0xff) : -1;
				if(left >= 0 && up >= 0)
					pred = (left + up) >> 1;
				else if(left >= 0)
					pred = left;
				else if(up >= 0)
					pred = up;
				else
					pred = 0;
				out[y * stride + (size_t)x * bytes + c] = (uint8_t)(cur - pred);
			}
	off = stride * H;
	return off;
}

/* The delta frame's payload, and in `next` the picture it has to decode
 * to (built from `prev`): block (0,0) changes with per-pixel deltas, block
 * (1,0) holds transparent pixels and displaced copies, block (2,1) a mix
 * of all three kinds; the other blocks are untouched.  The layout is the
 * decoder's: a bit per 8 x 8 block, eight row masks per changed block,
 * then the pixel stream.  Returns the payload size. */
static size_t DeltaPayload(uint8_t* out, const uint32_t* prev, uint32_t* next, int bpp)
{
	int bytes = bpp / 8, blocksX = (W + 7) / 8, blocksY = (H + 7) / 8, bx, by, x, y;
	size_t maskBytes = ((size_t)blocksX * blocksY + 7) / 8, off;
	uint8_t* rowMasks;
	uint8_t* stream;
	int present = 0;
	memset(out, 0, maskBytes);
	memcpy(next, prev, sizeof(uint32_t) * W * H);
	rowMasks = out + maskBytes;
	// which blocks change
	for(by = 0; by < blocksY; by++)
		for(bx = 0; bx < blocksX; bx++)
			if((bx == 0 && by == 0) || (bx == 1 && by == 0) || (bx == 2 && by == 1))
			{
				int i = by * blocksX + bx;
				out[i / 8] |= (uint8_t)(1 << (i % 8));
				present++;
			}
	stream = rowMasks + present * 8;
	off = 0;
	for(by = 0; by < blocksY; by++)
		for(bx = 0; bx < blocksX; bx++)
		{
			int kind, bw = W - bx * 8 < 8 ? W - bx * 8 : 8, bh = H - by * 8 < 8 ? H - by * 8 : 8;
			if(bx == 0 && by == 0)
				kind = 0;
			else if(bx == 1 && by == 0)
				kind = 1;
			else if(bx == 2 && by == 1)
				kind = 2;
			else
				continue;
			memset(rowMasks, 0, 8);
			for(y = 0; y < bh; y++)
				for(x = 0; x < bw; x++)
				{
					int px = bx * 8 + x, py = by * 8 + y, c;
					uint32_t old = prev[py * W + px];
					if(kind == 0 || (kind == 2 && (x + y) % 3 == 0))
					{ // a delta per channel: the row mask bit is set, one byte per channel follows
						uint32_t want = Pixel(px, py, 1);
						rowMasks[y] |= (uint8_t)(1 << x);
						for(c = 0; c < bytes; c++)
							stream[off++] = (uint8_t)(((want >> (8 * c)) & 0xff) - ((old >> (8 * c)) & 0xff));
						if(bytes == 3)
							want &= 0xffffffu; // a 24-bit movie decodes with alpha 0
						next[py * W + px] = want;
					}
					else if(kind == 1 && x % 2 == 0)
					{ // transparent: the byte 0x80, the pixel decodes to 0
						stream[off++] = 0x80;
						next[py * W + px] = 0;
					}
					else
					{ // a copy from the previous frame at (dx, dy), two signed bytes
						int dx = -(px % 3), dy = py > 2 ? -2 : 1;
						stream[off++] = (uint8_t)(int8_t)dx;
						stream[off++] = (uint8_t)(int8_t)dy;
						next[py * W + px] = prev[(py + dy) * W + px + dx];
					}
				}
			rowMasks += 8;
		}
	return (size_t)(stream - out) + off;
}

/* The wait objects: "90 F4" loads a movie on the loader thread and pushes
 * its result on the script thread's stack when polled done (0 ok, 2 not
 * a movie; the number and the info array are filled on success); the
 * deferred "90 F6" decodes a frame inside a wait object and pushes 0.
 * `file` is the movie built by TestMovie, `pix1` the picture its second
 * frame must decode to.  The files are written to the working directory,
 * which the POSIX OS layer presents as the drive, so "0" as the archive
 * name reaches them as loose files.  A failure means the wait objects do
 * not finish, push the wrong value, or decode through them differs from
 * the direct decode. */
static void TestWaits(OpenBGI_t* o, const uint8_t* file, size_t size, const uint32_t* pix1, int bpp)
{
	FILE* f;
	Thread_t* t;
	Wait_t* w;
	int32_t no = 0, info[5] = {0, 0, 0, 0, 0};
	int polls, r;
	Bmp_t b;
	printf("waits %d bpp\n", bpp);
	// the OS layer presents the working directory (made a scratch directory by main) as
	// the drive
	f = fopen("clip.bm", "wb");
	fwrite(file, 1, size, f);
	fclose(f);
	SetBaseDir(o, NULL);
	o->gfx.bmpMgr = BmpMgr_New(4);
	o->files.arcMgr = FileSet_New();
	Loader_Start(o);
	t = Thread_New(o, 0x100, 0x100, 0x100);
	// the load: polled as the scheduler would until it reports done (1); the result is
	// pushed
	w = WaitSeqLoad_New(o, t, "0", "clip.bm", &no, info);
	for(polls = 0, r = 0; r == 0 && polls < 2000; polls++)
	{
		r = w->vt->poll(o, w);
		if(r == 0)
			OS_SleepMs(1);
	}
	CHECK(r == 1);
	Wait_Release(o, w);
	CHECK(t->sp == 1 && t->stack[0] == 0);
	CHECK(no > 0 && info[0] == W && info[1] == H && info[4] == 2); // width, height, frame count
	// a file that is not a movie: result 2, the number untouched
	f = fopen("text.bm", "wb");
	fputs("not a movie at all, just some bytes", f);
	fclose(f);
	t->sp = 0;
	w = WaitSeqLoad_New(o, t, "0", "text.bm", &no, info);
	for(polls = 0, r = 0; r == 0 && polls < 2000; polls++)
	{
		r = w->vt->poll(o, w);
		if(r == 0)
			OS_SleepMs(1);
	}
	CHECK(r == 1);
	Wait_Release(o, w);
	CHECK(t->sp == 1 && t->stack[0] == 2);
	// the deferred decode of frame 1 after frame 0, into a slot of the movie's size and
	// mode
	BmpMgr_Create(o->gfx.bmpMgr, 2, W, H, bpp == 32 ? PM_ARGB32 : PM_RGB32);
	t->sp = 0;
	w = WaitSeqDecode_New(o, t, 2, no, 0);
	CHECK(w->vt->poll(o, w) == 1);
	Wait_Release(o, w);
	t->sp = 0;
	w = WaitSeqDecode_New(o, t, 2, no, 1);
	CHECK(w->vt->poll(o, w) == 1);
	Wait_Release(o, w);
	CHECK(t->sp == 1 && t->stack[0] == 0);
	BmpMgr_GetInfo(o->gfx.bmpMgr, &b, 2);
	CHECK(memcmp(b.pixels, pix1, sizeof(uint32_t) * W * H) == 0);
	BmSeq_FreeAll(o);
	Thread_Delete(o, t);
	Loader_Stop(o);
	BmpMgr_Delete(o->gfx.bmpMgr);
	o->gfx.bmpMgr = NULL;
	remove("clip.bm");
	remove("text.bm");
}

/* Build the two-frame movie at `bpp` (24 or 32) bits per pixel and run the
 * decoder, the registry and the wait tests on it.  The decoder part: the
 * file passes BfMovie_Check and BfMovie_IsSimple, frame 0 decodes to the
 * source picture, frame 1 (decoded over frame 0) to the delta target, and
 * frame 2 does not exist.  A failure in the decoder part means the
 * decoder and this encoder disagree on the format; the comments in
 * bfmovie.h describe it. */
static void TestMovie(OpenBGI_t* o, int bpp)
{
	uint32_t pix0[W * H], pix1[W * H], decoded[W * H];
	uint8_t payload[0x1000], runs[0x1100], file[0x3000];
	size_t n, off = 0x40 + 2 * 4; // the header and two frame offsets
	BfMovieHeader_t* h = (BfMovieHeader_t*)file;
	uint32_t offsets[2];
	int x, y, ok;
	printf("movie %d bpp\n", bpp);
	// a 24-bit movie decodes with alpha 0, so the source picture has none either
	for(y = 0; y < H; y++)
		for(x = 0; x < W; x++)
			pix0[y * W + x] = bpp == 32 ? Pixel(x, y, 0) : (Pixel(x, y, 0) & 0xffffffu);
	memset(file, 0, sizeof file);
	memcpy(h->magic, BFMOVIE_MAGIC, 16);
	h->type = 0;
	h->width = W;
	h->height = H;
	h->bpp = (uint32_t)bpp;
	h->mode = bpp == 32 ? 2 : 1; // the pixel mode of the target slot: 2 ARGB32, 1 RGB32
	h->fps = 30;
	h->frames = 2;
	offsets[0] = (uint32_t)off;
	n = KeyPayload(payload, pix0, bpp);
	n = Runs(runs, payload, (uint32_t)n);
	off += PackFrame(file + off, runs, (uint32_t)n);
	offsets[1] = (uint32_t)off;
	n = DeltaPayload(payload, pix0, pix1, bpp);
	n = Runs(runs, payload, (uint32_t)n);
	off += PackFrame(file + off, runs, (uint32_t)n);
	memcpy(file + 0x40, offsets, sizeof offsets);

	CHECK(BfMovie_Check(file, (uint32_t)off));
	CHECK(BfMovie_IsSimple(file));
	memset(decoded, 0xaa, sizeof decoded);
	ok = BfMovie_DecodeFrame(file, (uint32_t)off, 0, (uint8_t*)decoded, W * 4);
	CHECK(ok);
	CHECK(memcmp(decoded, pix0, sizeof pix0) == 0);
	ok = BfMovie_DecodeFrame(file, (uint32_t)off, 1, (uint8_t*)decoded, W * 4);
	CHECK(ok);
	CHECK(memcmp(decoded, pix1, sizeof pix1) == 0);
	CHECK(!BfMovie_DecodeFrame(file, (uint32_t)off, 2, (uint8_t*)decoded, W * 4));

	// the registry: a slot of the right size and mode, the error codes, the clone
	{
		int32_t no = 0, no2 = 0, info[5];
		o->gfx.bmpMgr = BmpMgr_New(4);
		// "90 F4": the number is positive, info holds width, height, pixel mode, frame
		// rate, frame count
		CHECK(BmSeq_Register(o, file, (uint32_t)off, &no, info) == 0 && no > 0);
		CHECK(info[0] == W && info[1] == H && info[2] == (bpp == 32 ? 2 : 1) && info[3] == 30 && info[4] == 2);
		CHECK(BmSeq_Register(o, file, 8, &no2, info) == 0x80000002u); // too short for the header: not a movie
		CHECK(BmSeq_Decode(o, 0, no, 0) == 0x80000005u);              // no bitmap in the slot
		BmpMgr_Create(o->gfx.bmpMgr, 0, W, H, bpp == 32 ? PM_ARGB32 : PM_RGB32);
		CHECK(BmSeq_Decode(o, 0, no, 5) == 0x80000004u);       // no such frame
		CHECK(BmSeq_Decode(o, 0, no + 100, 0) == 0x80000003u); // no such number
		CHECK(BmSeq_Decode(o, 0, no, 0) == 0);
		CHECK(BmSeq_Decode(o, 0, no, 1) == 0);
		{
			Bmp_t b;
			BmpMgr_GetInfo(o->gfx.bmpMgr, &b, 0);
			CHECK(memcmp(b.pixels, pix1, sizeof pix1) == 0);
		}
		BmpMgr_Create(o->gfx.bmpMgr, 1, W + 1, H, PM_ARGB32);
		CHECK(BmSeq_Decode(o, 1, no, 0) == 0x80000005u); // a slot one pixel too wide does not match
		// "90 F7": the clone takes the next number; a record has one clone at most
		CHECK(BmSeq_Clone(o, no, &no2) == 0 && no2 == no + 1);
		CHECK(BmSeq_Clone(o, no, &no2) == 0x80000007u);
		CHECK(BmSeq_Decode(o, 0, no2, 0) == 0);
		CHECK(BmSeq_Free(o, no) == 0);
		CHECK(BmSeq_Decode(o, 0, no2, 1) == 0); // the data lives on with the clone
		CHECK(BmSeq_Free(o, no2) == 0);
		CHECK(BmSeq_Free(o, no2) == 0x80000003u); // freed twice: no such number
		BmpMgr_Delete(o->gfx.bmpMgr);
		o->gfx.bmpMgr = NULL;
	}
	TestWaits(o, file, off, pix1, bpp);
}

// ---- a streamed movie, by hand ---------------------------------------------------------------

/* A frame of a streamed movie of two blocks in one band at `p`: the 16
 * weights of the DC tree (no AC values: those weights are all zero), the
 * band - the bitmap of its blocks, the number of coefficients, the DC bit
 * stream - and the alpha plane (none when alphaLen is 0); the bytes */
static size_t StreamedFrame(uint8_t* p, const uint8_t* dcWeights, unsigned bitmap, unsigned blocks, const uint8_t* dc, size_t dcLen,
	const uint8_t* alpha, size_t alphaLen)
{
	size_t n = 16 + 176 + 8, count = (size_t)blocks * 3 * 64;
	memset(p, 0, n);
	memcpy(p, dcWeights, 16);
	PutLe32(p + 192, 200);
	p[n++] = (uint8_t)bitmap;
	if(count >= 0x80)
		p[n++] = (uint8_t)(0x80 | (count & 0x7f)), p[n++] = (uint8_t)(count >> 7);
	else
		p[n++] = (uint8_t)count;
	memcpy(p + n, dc, dcLen);
	n += dcLen;
	PutLe32(p + 196, alphaLen ? (uint32_t)n : 0xffffffffu);
	memcpy(p + n, alpha, alphaLen);
	return n + alphaLen;
}

/* The movie at `file`: `w` x `h` pixels (16 x 8 at most), `bpp` bits, of
 * the given type; the bytes.
 *
 *   frame 0  both blocks.  The DC values, one predictor through them: Y
 *            80, 160, Cb 80, 0, Cr 0, 0 - four differences of seven bits
 *            (+80, +80, -80, -80; the heavier symbol, bit 1) and two of
 *            none (bit 0).  Block 0 comes out as blue 156, green 135, red
 *            138, block 1 as gray 148.  The alpha plane, the first way: a
 *            literal 0xC0 and a copy of the pixel before, 127 times.
 *   frame 1  block 1 alone: Y 40, Cb and Cr 0 (two differences of six
 *            bits, one of none): gray 133.  The alpha plane, the second
 *            way: 65 bytes - a bitmap that names block 0, its 64 alpha
 *            bytes 0x40 -, the byte 1 as bit 0 and the byte 0x40 as bit 1.
 *   frame 2  no block, and an alpha plane of the bitmap alone.
 *
 * A movie of type 0x10000 has the alpha plane of frame 0 without its
 * leading word, and in frame 1 the same plane with 0x40; a 24-bit movie
 * has none. */
static size_t StreamedMovie(uint8_t* file, uint32_t type, uint32_t w, uint32_t h, uint32_t bpp)
{
	static const uint8_t kDc0[16] = {2, 0, 0, 0, 0, 0, 0, 4}, kDc1[16] = {1, 0, 0, 0, 0, 0, 2}, kDc2[16] = {0};
	static const uint8_t kBits0[] = {0xD0, 0xD0, 0xAF, 0xAF, 0x00}, kBits1[] = {0xD1, 0x5C};
	static const uint8_t kLzC0[] = {1, 0, 0, 0, 0x02, 0xC0, 0x3F, 0xF8}, kLz40[] = {1, 0, 0, 0, 0x02, 0x40, 0x3F, 0xF8};
	uint8_t blocks[4 + 4 + 256 + 9], none[4 + 4 + 256 + 1];
	BfMovieHeader_t* hd = (BfMovieHeader_t*)file;
	size_t n = 0xC0 + 3 * 4;
	int plain = type == 0x10000u; // no leading word
	memset(file, 0, n);
	memcpy(hd->magic, BFMOVIE_MAGIC, 16);
	hd->type = type;
	hd->width = w;
	hd->height = h;
	hd->bpp = bpp;
	hd->mode = bpp == 32 ? 2 : 1;
	hd->fps = 30;
	hd->frames = 3;
	hd->zero[0] = 3;
	memset(file + 0x40, 1, 128); // the quantisation tables: every step 1
	// the two alpha planes of the second way
	memset(blocks, 0, sizeof blocks);
	blocks[0] = 2;
	blocks[4] = 65;
	blocks[8 + 1] = 1;
	blocks[8 + 0x40] = 64;
	blocks[8 + 256] = 0x7f;
	memset(blocks + 8 + 257, 0xff, 7);
	blocks[8 + 264] = 0x80;
	memset(none, 0, sizeof none);
	none[0] = 2;
	none[4] = 1;
	none[8] = 1;
	PutLe32(file + 0xC0, (uint32_t)n);
	n += StreamedFrame(file + n, kDc0, 3, 2, kBits0, sizeof kBits0, kLzC0 + (plain ? 4 : 0), bpp == 32 ? sizeof kLzC0 - (plain ? 4 : 0) : 0);
	PutLe32(file + 0xC4, (uint32_t)n);
	if(plain)
		n += StreamedFrame(file + n, kDc1, 2, 1, kBits1, sizeof kBits1, kLz40 + 4, sizeof kLz40 - 4);
	else
		n += StreamedFrame(file + n, kDc1, 2, 1, kBits1, sizeof kBits1, blocks, bpp == 32 ? sizeof blocks : 0);
	PutLe32(file + 0xC8, (uint32_t)n);
	n += StreamedFrame(file + n, kDc2, 0, 0, kBits0, 0, plain ? kLz40 + 4 : none, bpp != 32 ? 0 : plain ? sizeof kLz40 - 4
																										: sizeof none);
	return n;
}

// whether the 8 x 8 block `bx` of the picture at `pix` (w x h visible pixels, rows `pitch` apart) is of one colour
static int BlockIs(const uint8_t* pix, int pitch, int w, int h, int bx, unsigned b, unsigned g, unsigned r, unsigned a)
{
	int x, y;
	for(y = 0; y < h; y++)
		for(x = bx * 8; x < bx * 8 + 8 && x < w; x++)
		{
			const uint8_t* p = pix + (size_t)y * (size_t)pitch + (size_t)x * 4;
			if(p[0] != b || p[1] != g || p[2] != r || p[3] != a)
				return 0;
		}
	return 1;
}

static void TestStreamed(OpenBGI_t* o)
{
	static uint8_t file[0x1000], pix[8 * 80];
	size_t n;
	printf("streamed\n");
	n = StreamedMovie(file, 0x10001, 16, 8, 32);
	CHECK(BfMovie_Check(file, (uint32_t)n) && !BfMovie_IsSimple(file) && BfMovie_CanDecode(file));
	CHECK(!BfMovie_Check(file, 0xC0 + 8)); // the offsets do not fit
	memset(pix, 0x11, sizeof pix);
	CHECK(BfMovie_DecodeFrame(file, (uint32_t)n, 0, pix, 64));
	CHECK(BlockIs(pix, 64, 16, 8, 0, 156, 135, 138, 0xC0) && BlockIs(pix, 64, 16, 8, 1, 148, 148, 148, 0xC0));
	CHECK(BfMovie_DecodeFrame(file, (uint32_t)n, 1, pix, 64)); // block 1's colour, block 0's alpha: the rest stays
	CHECK(BlockIs(pix, 64, 16, 8, 0, 156, 135, 138, 0x40) && BlockIs(pix, 64, 16, 8, 1, 133, 133, 133, 0xC0));
	CHECK(BfMovie_DecodeFrame(file, (uint32_t)n, 2, pix, 64)); // nothing changes
	CHECK(BlockIs(pix, 64, 16, 8, 0, 156, 135, 138, 0x40) && BlockIs(pix, 64, 16, 8, 1, 133, 133, 133, 0xC0));
	CHECK(!BfMovie_DecodeFrame(file, (uint32_t)n, 3, pix, 64));
	// a frame cut short inside its tables is refused; cut inside its band it leaves the blocks alone
	CHECK(!BfMovie_DecodeFrame(file, 0xCC + 100, 0, pix, 64));
	{
		int32_t no = 0, info[5];
		Bmp_t b;
		o->gfx.bmpMgr = BmpMgr_New(4);
		CHECK(BmSeq_Register(o, file, (uint32_t)n, &no, info) == 0 && info[0] == 16 && info[1] == 8 && info[2] == 2 && info[4] == 3);
		BmpMgr_Create(o->gfx.bmpMgr, 0, 16, 8, PM_ARGB32);
		CHECK(BmSeq_Decode(o, 0, no, 0) == 0 && BmSeq_Decode(o, 0, no, 1) == 0);
		BmpMgr_GetInfo(o->gfx.bmpMgr, &b, 0);
		CHECK(BlockIs(b.pixels, b.pitch, 16, 8, 0, 156, 135, 138, 0x40) && BlockIs(b.pixels, b.pitch, 16, 8, 1, 133, 133, 133, 0xC0));
		CHECK(BmSeq_Free(o, no) == 0);
		file[0x10] = 2; // a type nobody knows
		CHECK(BmSeq_Register(o, file, (uint32_t)n, &no, info) == 0 && BmSeq_Decode(o, 0, no, 0) == 0x8000000au);
		file[0x10] = 1;
		CHECK(BmSeq_Free(o, no) == 0);
		BmpMgr_Delete(o->gfx.bmpMgr);
		o->gfx.bmpMgr = NULL;
	}
	// 12 x 6 pixels in rows 80 bytes apart: no whole blocks; what is beside the picture is not touched
	n = StreamedMovie(file, 0x10001, 12, 6, 32);
	memset(pix, 0x11, sizeof pix);
	CHECK(BfMovie_DecodeFrame(file, (uint32_t)n, 0, pix, 80) && BfMovie_DecodeFrame(file, (uint32_t)n, 1, pix, 80));
	CHECK(BlockIs(pix, 80, 12, 6, 0, 156, 135, 138, 0x40) && BlockIs(pix, 80, 12, 6, 1, 133, 133, 133, 0xC0));
	CHECK(pix[12 * 4] == 0x11 && pix[80 * 6] == 0x11 && pix[80 * 5 + 79] == 0x11);
	// the form without the word before the alpha plane: every frame has a whole plane
	n = StreamedMovie(file, 0x10000, 16, 8, 32);
	CHECK(BfMovie_DecodeFrame(file, (uint32_t)n, 0, pix, 64) && BlockIs(pix, 64, 16, 8, 0, 156, 135, 138, 0xC0));
	CHECK(BfMovie_DecodeFrame(file, (uint32_t)n, 1, pix, 64));
	CHECK(BlockIs(pix, 64, 16, 8, 0, 156, 135, 138, 0x40) && BlockIs(pix, 64, 16, 8, 1, 133, 133, 133, 0x40));
	// 24 bits: no alpha plane, the alpha bytes are 0
	n = StreamedMovie(file, 0x10001, 16, 8, 24);
	memset(pix, 0x11, sizeof pix);
	CHECK(BfMovie_DecodeFrame(file, (uint32_t)n, 0, pix, 64) && BfMovie_DecodeFrame(file, (uint32_t)n, 1, pix, 64));
	CHECK(BlockIs(pix, 64, 16, 8, 0, 156, 135, 138, 0) && BlockIs(pix, 64, 16, 8, 1, 133, 133, 133, 0));
}

// poll a wait as the scheduler would until it reports done, release it, and give what it pushed
static uint32_t RunWait(OpenBGI_t* o, Thread_t* t, Wait_t* w)
{
	int polls, r;
	t->sp = 0;
	for(polls = 0, r = 0; r == 0 && polls < 2000; polls++)
	{
		r = w->vt->poll(o, w);
		if(r == 0)
			OS_SleepMs(1);
	}
	CHECK(r == 1);
	Wait_Release(o, w);
	CHECK(t->sp == 1);
	return t->stack[0];
}

/* "92 F1" with its flag set (WaitSeqOpen): a streamed movie is opened and
 * its frames are read from the file as they are decoded - as a loose file
 * and as an archive entry larger than the loader's limit (the movie with
 * 70 MB behind it).  The frames must decode to what the whole file in
 * memory gives; a clone reads the same file and outlives its original; a
 * movie of type 0, a file that is no movie, a missing file and no name are
 * turned away with the original's codes.  Needs the loader and the drive
 * TestWaits set up. */
static void TestStreamedOpen(OpenBGI_t* o)
{
	static uint8_t file[0x1000];
	Thread_t* t = Thread_New(o, 0x100, 0x100, 0x100);
	int32_t no = 0, clone = 0, info[5] = {0, 0, 0, 0, 0};
	size_t n;
	FILE* f;
	Bmp_t b;
	printf("streamed, opened\n");
	n = StreamedMovie(file, 0x10001, 16, 8, 32);
	f = fopen("s.bm", "wb");
	fwrite(file, 1, n, f);
	fclose(f);
	o->gfx.bmpMgr = BmpMgr_New(4);
	BmpMgr_Create(o->gfx.bmpMgr, 0, 16, 8, PM_ARGB32);
	BmpMgr_GetInfo(o->gfx.bmpMgr, &b, 0);
	// the loose file
	CHECK(RunWait(o, t, WaitSeqOpen_New(o, t, NULL, "s.bm", &no, info)) == 0);
	CHECK(no > 0 && info[0] == 16 && info[1] == 8 && info[2] == 2 && info[4] == 3);
	CHECK(BmSeq_Decode(o, 0, no, 0) == 0);
	CHECK(BlockIs(b.pixels, b.pitch, 16, 8, 0, 156, 135, 138, 0xC0) && BlockIs(b.pixels, b.pitch, 16, 8, 1, 148, 148, 148, 0xC0));
	CHECK(BmSeq_Decode(o, 0, no, 1) == 0 && BmSeq_Decode(o, 0, no, 2) == 0);
	CHECK(BlockIs(b.pixels, b.pitch, 16, 8, 0, 156, 135, 138, 0x40) && BlockIs(b.pixels, b.pitch, 16, 8, 1, 133, 133, 133, 0xC0));
	CHECK(BmSeq_Decode(o, 0, no, 3) == 0x80000004u);
	// the clone reads the same file, also when its original is gone
	CHECK(BmSeq_Clone(o, no, &clone) == 0 && BmSeq_Free(o, no) == 0);
	CHECK(BmSeq_Decode(o, 0, clone, 0) == 0 && BlockIs(b.pixels, b.pitch, 16, 8, 1, 148, 148, 148, 0xC0));
	// the file went away: the frame cannot be read
	remove("s.bm");
	CHECK(BmSeq_Decode(o, 0, clone, 1) == 0x80000008u);
	CHECK(BmSeq_Free(o, clone) == 0);
	// what is turned away: a missing file, no name, a movie of type 0, a file that is no movie
	no = 0;
	CHECK(RunWait(o, t, WaitSeqOpen_New(o, t, NULL, "s.bm", &no, info)) == 2 && no == 0);
	CHECK(RunWait(o, t, WaitSeqOpen_New(o, t, NULL, NULL, &no, info)) == 1 && no == 0);
	CHECK(RunWait(o, t, WaitSeqOpen_New(o, t, NULL, "clip.bm", &no, info)) == 2 && no == 0);
	CHECK(RunWait(o, t, WaitSeqOpen_New(o, t, NULL, "text.bm", &no, info)) == 2 && no == 0);
	{ // an archive entry of 70 MB: the movie with nothing behind it (the last frame runs to the end, so only the first two are read)
		uint8_t entry[0x80];
		uint32_t count = 1, size = (uint32_t)n + (70u << 20);
		memset(entry, 0, sizeof entry);
		strcpy((char*)entry, "big.bm");
		memcpy(entry + 0x64, &size, 4);
		f = fopen("movies.arc", "wb");
		fwrite("BURIKO ARC20", 1, 12, f);
		fwrite(&count, 4, 1, f);
		fwrite(entry, 1, sizeof entry, f);
		fwrite(file, 1, n, f);
		fseek(f, (long)(70u << 20) - 1, SEEK_CUR);
		fputc(0, f);
		fclose(f);
		CHECK(RunWait(o, t, WaitSeqOpen_New(o, t, "movies.arc", "big.bm", &no, info)) == 0 && info[4] == 3);
		CHECK(BmSeq_Decode(o, 0, no, 0) == 0 && BmSeq_Decode(o, 0, no, 1) == 0);
		CHECK(BlockIs(b.pixels, b.pitch, 16, 8, 0, 156, 135, 138, 0x40) && BlockIs(b.pixels, b.pitch, 16, 8, 1, 133, 133, 133, 0xC0));
		CHECK(BmSeq_Free(o, no) == 0);
	}
	BmpMgr_Delete(o->gfx.bmpMgr);
	o->gfx.bmpMgr = NULL;
}

/* The smoke test on a real movie: when BGI_BM_FILE names a .bm file, read
 * it, check its header and decode every frame in sequence into one
 * buffer; all of them have to succeed.  With BGI_BM_OUT set the last
 * decoded picture (the state after the final frame) is written there as
 * a binary PPM for a look by eye.  Does nothing when the variable is not
 * set or the file does not open. */
static void TestRealFile(void)
{
	const char* path = getenv("BGI_BM_FILE");
	FILE* f;
	long n;
	uint8_t* d;
	const BfMovieHeader_t* h;
	uint8_t* pix;
	uint32_t i, ok = 0;
	if(!path || (f = fopen(path, "rb")) == NULL)
		return;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = (uint8_t*)malloc((size_t)n);
	if(fread(d, 1, (size_t)n, f) != (size_t)n)
		n = 0;
	fclose(f);
	printf("real movie %s\n", path);
	CHECK(BfMovie_Check(d, (uint32_t)n));
	h = (const BfMovieHeader_t*)d;
	pix = (uint8_t*)calloc((size_t)h->width * h->height * 4, 1);
	for(i = 0; i < h->frames; i++)
		ok += BfMovie_DecodeFrame(d, (uint32_t)n, i, pix, (int)h->width * 4) != 0;
	CHECK(ok == h->frames);
	printf("  %ux%u, %u bpp, %u frames decoded\n", h->width, h->height, h->bpp, ok);
	if(getenv("BGI_BM_OUT") && (f = fopen(getenv("BGI_BM_OUT"), "wb")) != NULL)
	{
		uint32_t k;
		// the pixels are BGRA in memory; PPM wants RGB
		fprintf(f, "P6\n%u %u\n255\n", h->width, h->height);
		for(k = 0; k < h->width * h->height; k++)
		{
			uint8_t rgb[3] = {pix[k * 4 + 2], pix[k * 4 + 1], pix[k * 4]};
			fwrite(rgb, 1, 3, f);
		}
		fclose(f);
	}
	free(pix);
	free(d);
}

// run every group in a scratch directory; exit status 1 when any check failed
int main(void)
{
	char dir[0x100];
	OpenBGI_t* o = OpenBGI_New(OS_Host()); // the engine whose code the test runs
	if(!o)
		return 1;
	TestHost_SetEnv("BGI_MSGBOX_STDERR", "1"); // message boxes go to stderr instead of a dialog
	// the scratch directory becomes the engine's drive (see TestWaits) and is removed at
	// the end
	TestHost_ScratchName(dir, sizeof dir, "bgi_bfmovie");
	if(TestHost_Mkdir(dir) != 0 || TestHost_Chdir(dir) != 0)
	{
		printf("cannot make the scratch directory %s\n", dir);
		return 1;
	}
	OS_Init();
	OS_SetGameDir(dir); // the drive on Windows too, where it is otherwise the binary's directory
	TestMovie(o, 32);
	TestMovie(o, 24);
	TestStreamed(o);
	TestStreamedOpen(o);
	TestRealFile();
	if(TestHost_Chdir(TestHost_TempRoot()) == 0)
		TestHost_RemoveTree(dir);
	if(gFails)
	{
		printf("%d failure(s)\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
