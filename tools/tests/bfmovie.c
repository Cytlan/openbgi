/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/bfmovie.c - unit test of the BF_Movie frame decoder (src/core/
 *                   bfmovie.c, inc/bgi/bfmovie.h) and the sequence
 *                   registry behind "90 F4" .. "90 F7" (src/gfx/bmseq.c,
 *                   inc/bgi/gfx/bmseq.h); built as bin/test_bfmovie by
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
 * With BGI_BM_FILE naming a real .bm file every frame of it is decoded as
 * a smoke test (and the picture after the last frame written to BGI_BM_OUT
 * as a PPM when set).
 *
 * main runs in a scratch directory under /tmp that it removes afterwards,
 * because the wait tests write files and the POSIX OS layer presents the
 * working directory at OS_Init as the engine's drive.
 */
#include "bgi/bfmovie.h"
#include "bgi/gfx/bmseq.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/vm.h"
#include "bgi/wait.h"
#include "bgi/waitobj.h"
#include "bgi/file.h"
#include "bgi/os.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

static int gFails; // the number of failed checks so far
// evaluate a condition; print it with its location and count a failure when it is false
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

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
	size_t o = 0;
	uint32_t acc = 0;
	int accBits = 0;
	memcpy(huff, &payloadLen, 4); // the length field in front of the run-length stream
	memcpy(huff + 4, payload, payloadLen);
	memset(weights, 0, sizeof weights);
	for(i = 0; i < n; i++)
		weights[huff[i]]++;
	Tree(nodes, weights);
	o += PutVarint(out + o, n);
	for(i = 0; i < 256; i++)
		o += PutVarint(out + o, weights[i]);
	for(i = 0; i < n; i++)
	{
		uint32_t code;
		int len;
		CodeFor(nodes, huff[i], &code, &len);
		acc |= code << accBits;
		accBits += len;
		while(accBits >= 8)
		{
			out[o++] = (uint8_t)acc;
			acc >>= 8;
			accBits -= 8;
		}
	}
	if(accBits)
		out[o++] = (uint8_t)acc;
	free(huff);
	return o;
}

/* the run-length layer: one (fill 0, copy `len`) pair, so a single literal run holding
 * everything */
static size_t Runs(uint8_t* out, const uint8_t* data, uint32_t len)
{
	size_t o = PutVarint(out, 0);
	o += PutVarint(out + o, len);
	memcpy(out + o, data, len);
	return o + len;
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
	size_t stride = ((size_t)bytes * W + 3) & ~(size_t)3, o = 0;
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
	o = stride * H;
	return o;
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
	size_t maskBytes = ((size_t)blocksX * blocksY + 7) / 8, o;
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
	o = 0;
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
							stream[o++] = (uint8_t)(((want >> (8 * c)) & 0xff) - ((old >> (8 * c)) & 0xff));
						if(bytes == 3)
							want &= 0xffffffu; // a 24-bit movie decodes with alpha 0
						next[py * W + px] = want;
					}
					else if(kind == 1 && x % 2 == 0)
					{ // transparent: the byte 0x80, the pixel decodes to 0
						stream[o++] = 0x80;
						next[py * W + px] = 0;
					}
					else
					{ // a copy from the previous frame at (dx, dy), two signed bytes
						int dx = -(px % 3), dy = py > 2 ? -2 : 1;
						stream[o++] = (uint8_t)(int8_t)dx;
						stream[o++] = (uint8_t)(int8_t)dy;
						next[py * W + px] = prev[(py + dy) * W + px + dx];
					}
				}
			rowMasks += 8;
		}
	return (size_t)(stream - out) + o;
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
static void TestWaits(const uint8_t* file, size_t size, const uint32_t* pix1, int bpp)
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
	SetBaseDir(NULL);
	gBmpMgr = BmpMgr_New(4);
	gArcMgr = FileSet_New();
	Loader_Start();
	t = Thread_New(0x100, 0x100, 0x100);
	// the load: polled as the scheduler would until it reports done (1); the result is
	// pushed
	w = WaitSeqLoad_New(t, "0", "clip.bm", &no, info);
	for(polls = 0, r = 0; r == 0 && polls < 2000; polls++)
	{
		r = w->vt->poll(w);
		if(r == 0)
			OS_SleepMs(1);
	}
	CHECK(r == 1);
	Wait_Release(w);
	CHECK(t->sp == 1 && t->stack[0] == 0);
	CHECK(no > 0 && info[0] == W && info[1] == H && info[4] == 2); // width, height, frame count
	// a file that is not a movie: result 2, the number untouched
	f = fopen("text.bm", "wb");
	fputs("not a movie at all, just some bytes", f);
	fclose(f);
	t->sp = 0;
	w = WaitSeqLoad_New(t, "0", "text.bm", &no, info);
	for(polls = 0, r = 0; r == 0 && polls < 2000; polls++)
	{
		r = w->vt->poll(w);
		if(r == 0)
			OS_SleepMs(1);
	}
	CHECK(r == 1);
	Wait_Release(w);
	CHECK(t->sp == 1 && t->stack[0] == 2);
	// the deferred decode of frame 1 after frame 0, into a slot of the movie's size and
	// mode
	BmpMgr_Create(gBmpMgr, 2, W, H, bpp == 32 ? PM_ARGB32 : PM_RGB32);
	t->sp = 0;
	w = WaitSeqDecode_New(t, 2, no, 0);
	CHECK(w->vt->poll(w) == 1);
	Wait_Release(w);
	t->sp = 0;
	w = WaitSeqDecode_New(t, 2, no, 1);
	CHECK(w->vt->poll(w) == 1);
	Wait_Release(w);
	CHECK(t->sp == 1 && t->stack[0] == 0);
	BmpMgr_GetInfo(gBmpMgr, &b, 2);
	CHECK(memcmp(b.pixels, pix1, sizeof(uint32_t) * W * H) == 0);
	BmSeq_FreeAll();
	Thread_Delete(t);
	Loader_Stop();
	BmpMgr_Delete(gBmpMgr);
	gBmpMgr = NULL;
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
static void TestMovie(int bpp)
{
	uint32_t pix0[W * H], pix1[W * H], decoded[W * H];
	uint8_t payload[0x1000], runs[0x1100], file[0x3000];
	size_t n, o = 0x40 + 2 * 4; // the header and two frame offsets
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
	offsets[0] = (uint32_t)o;
	n = KeyPayload(payload, pix0, bpp);
	n = Runs(runs, payload, (uint32_t)n);
	o += PackFrame(file + o, runs, (uint32_t)n);
	offsets[1] = (uint32_t)o;
	n = DeltaPayload(payload, pix0, pix1, bpp);
	n = Runs(runs, payload, (uint32_t)n);
	o += PackFrame(file + o, runs, (uint32_t)n);
	memcpy(file + 0x40, offsets, sizeof offsets);

	CHECK(BfMovie_Check(file, (uint32_t)o));
	CHECK(BfMovie_IsSimple(file));
	memset(decoded, 0xaa, sizeof decoded);
	ok = BfMovie_DecodeFrame(file, (uint32_t)o, 0, (uint8_t*)decoded, W * 4);
	CHECK(ok);
	CHECK(memcmp(decoded, pix0, sizeof pix0) == 0);
	ok = BfMovie_DecodeFrame(file, (uint32_t)o, 1, (uint8_t*)decoded, W * 4);
	CHECK(ok);
	CHECK(memcmp(decoded, pix1, sizeof pix1) == 0);
	CHECK(!BfMovie_DecodeFrame(file, (uint32_t)o, 2, (uint8_t*)decoded, W * 4));

	// the registry: a slot of the right size and mode, the error codes, the clone
	{
		int32_t no = 0, no2 = 0, info[5];
		gBmpMgr = BmpMgr_New(4);
		// "90 F4": the number is positive, info holds width, height, pixel mode, frame
		// rate, frame count
		CHECK(BmSeq_Register(file, (uint32_t)o, &no, info) == 0 && no > 0);
		CHECK(info[0] == W && info[1] == H && info[2] == (bpp == 32 ? 2 : 1) && info[3] == 30 && info[4] == 2);
		CHECK(BmSeq_Register(file, 8, &no2, info) == 0x80000002u); // too short for the header: not a movie
		CHECK(BmSeq_Decode(0, no, 0) == 0x80000005u);              // no bitmap in the slot
		BmpMgr_Create(gBmpMgr, 0, W, H, bpp == 32 ? PM_ARGB32 : PM_RGB32);
		CHECK(BmSeq_Decode(0, no, 5) == 0x80000004u);       // no such frame
		CHECK(BmSeq_Decode(0, no + 100, 0) == 0x80000003u); // no such number
		CHECK(BmSeq_Decode(0, no, 0) == 0);
		CHECK(BmSeq_Decode(0, no, 1) == 0);
		{
			Bmp_t b;
			BmpMgr_GetInfo(gBmpMgr, &b, 0);
			CHECK(memcmp(b.pixels, pix1, sizeof pix1) == 0);
		}
		BmpMgr_Create(gBmpMgr, 1, W + 1, H, PM_ARGB32);
		CHECK(BmSeq_Decode(1, no, 0) == 0x80000005u); // a slot one pixel too wide does not match
		// "90 F7": the clone takes the next number; a record has one clone at most
		CHECK(BmSeq_Clone(no, &no2) == 0 && no2 == no + 1);
		CHECK(BmSeq_Clone(no, &no2) == 0x80000007u);
		CHECK(BmSeq_Decode(0, no2, 0) == 0);
		CHECK(BmSeq_Free(no) == 0);
		CHECK(BmSeq_Decode(0, no2, 1) == 0); // the data lives on with the clone
		CHECK(BmSeq_Free(no2) == 0);
		CHECK(BmSeq_Free(no2) == 0x80000003u); // freed twice: no such number
		BmpMgr_Delete(gBmpMgr);
		gBmpMgr = NULL;
	}
	TestWaits(file, o, pix1, bpp);
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
	setenv("BGI_MSGBOX_STDERR", "1", 1); // message boxes go to stderr instead of a dialog
	// the scratch directory becomes the engine's drive (see TestWaits) and is removed at
	// the end
	sprintf(dir, "/tmp/bgi_bfmovie_%d", (int)getpid());
	if(mkdir(dir, 0700) != 0 || chdir(dir) != 0)
	{
		printf("cannot make the scratch directory %s\n", dir);
		return 1;
	}
	OS_Init();
	TestMovie(32);
	TestMovie(24);
	TestRealFile();
	if(chdir("/") == 0)
		remove(dir);
	if(gFails)
	{
		printf("%d failure(s)\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
