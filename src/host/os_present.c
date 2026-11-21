/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_present.c - the game's picture laid on a canvas of another size
 *                (os_present.h); not part of the original engine, whose
 *                window is the picture
 *
 * The picture is resampled in two passes, across and then down, each
 * with the same kind of table: for every pixel of the result the source
 * pixels it is made of and their weights (12 bits, summing to one).  The
 * rows scaled across are kept, so that a later frame scales only the rows
 * that changed and the rows of the result those reach.  A picture that is
 * shrunk gets a filter as much wider as it is shrunk, so that it does not
 * shimmer.  A large job is shared out to a few threads, row by row; the
 * threads live for the job only.
 *
 * The pixels are those of the engine's picture and of every target this
 * is built for: 32 bits with blue in the lowest byte.
 */
#include "host/os_present.h"
#include "host/os.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WEIGHT_BITS  12
#define WEIGHT_ONE   (1 << WEIGHT_BITS)
#define MAX_SHRINK   8     // the smallest scale is 1 / MAX_SHRINK
#define MAX_GROW     8     // the largest whole or power-of-two factor
#define MAX_SIDE     16384 // of a picture or a canvas
#define MAX_THREADS  4
#define THREAD_BYTES 2000000 // a job's bytes read and written per thread before a further thread is worth its start

// one direction's table
typedef struct Axis
{
	int src, dst;  // the lengths
	int taps;      // source pixels per pixel of the result
	int* index;    // dst x taps: which (kept inside the source)
	short* weight; // dst x taps
} Axis_t;

struct OsPresent
{
	int cw, ch, steps, filter; // the set-up
	int sw, sh, bpp;           // the picture the tables are for
	int dx, dy, dw, dh;        // where it lies on the canvas (beyond it with OS_PRESENT_NONE)
	int x0, x1, y0, y1;        // the part of dw x dh that is on the canvas
	int ready;                 // the tables and buffers fit the set-up and the picture
	int all;                   // everything is to be drawn anew
	int black;                 // the canvas is black all over: no picture is on it
	int scaled;                // dw x dh is not the picture's size
	int drawnY, drawnH;        // the canvas rows the last drawing wrote
	uint32_t* canvas;
	uint8_t* rowDirty; // per row of the picture
	uint8_t* outDirty; // per row of the result
	uint32_t* wide;    // a 5-5-5 picture's rows widened (sw x sh)
	short* across;     // the rows scaled across: dw x sh pixels of three values (KEEP_BITS)
	int* sums;         // per thread: a row of sums, 3 per pixel
	Axis_t ax, ay;
	int threads;
	const uint8_t* pixels; // of the drawing under way
	int pitch;
};

// ---- the size ---------------------------------------------------------------------------------

void OsPresent_Size(int w, int h, int cw, int ch, int steps, int* outW, int* outH)
{
	int n, d;
	*outW = w;
	*outH = h;
	if(w <= 0 || h <= 0 || cw <= 0 || ch <= 0 || steps == OS_PRESENT_NONE)
		return;
	if(steps == OS_PRESENT_STRETCH)
	{
		*outW = cw;
		*outH = ch;
		return;
	}
	if(steps == OS_PRESENT_FILL)
	{
		if((int64_t)cw * h <= (int64_t)ch * w)
		{ // the canvas is the narrower: its width decides
			*outW = cw;
			*outH = (int)(((int64_t)h * cw + w / 2) / w);
		}
		else
		{
			*outH = ch;
			*outW = (int)(((int64_t)w * ch + h / 2) / h);
		}
		if(*outW < 1)
			*outW = 1;
		if(*outH < 1)
			*outH = 1;
		return;
	}
	if(w <= cw && h <= ch)
	{ // the largest factor that fits
		n = cw / w < ch / h ? cw / w : ch / h;
		if(n > MAX_GROW)
			n = MAX_GROW;
		if(steps == OS_PRESENT_POW2)
			for(d = 1; d * 2 <= n; d *= 2)
				;
		else
			d = n;
		*outW = w * d;
		*outH = h * d;
		return;
	}
	// the smallest divisor at which it fits
	for(d = 2; d < MAX_SHRINK && ((w + d - 1) / d > cw || (h + d - 1) / d > ch); d = steps == OS_PRESENT_POW2 ? d * 2 : d + 1)
		;
	*outW = (w + d - 1) / d;
	*outH = (h + d - 1) / d;
}

// ---- the tables -------------------------------------------------------------------------------

// Catmull-Rom's curve: 1 at 0, 0 at the other whole numbers, nothing from 2 on
static double Cubic(double x)
{
	x = fabs(x);
	if(x <= 1)
		return (1.5 * x - 2.5) * x * x + 1;
	if(x < 2)
		return ((-0.5 * x + 2.5) * x - 4) * x + 2;
	return 0;
}

static double Tent(double x)
{
	x = fabs(x);
	return x < 1 ? 1 - x : 0;
}

static void Axis_Free(Axis_t* a)
{
	free(a->index);
	free(a->weight);
	memset(a, 0, sizeof *a);
}

// the table that makes `dst` pixels of `src`; 0 when memory ran out
static int Axis_Make(Axis_t* a, int src, int dst, int filter)
{
	double stretch = dst < src ? (double)src / dst : 1; // a shrinking filter is as much wider
	double reach = filter == OS_PRESENT_BICUBIC ? 2 : 1;
	int taps = filter == OS_PRESENT_NEAREST ? 1 : (int)ceil(2 * reach * stretch), i, k;
	Axis_Free(a);
	a->index = (int*)malloc((size_t)dst * (size_t)taps * sizeof *a->index);
	a->weight = (short*)malloc((size_t)dst * (size_t)taps * sizeof *a->weight);
	if(!a->index || !a->weight)
	{
		Axis_Free(a);
		return 0;
	}
	a->src = src;
	a->dst = dst;
	a->taps = taps;
	for(i = 0; i < dst; i++)
	{
		double centre = (i + 0.5) * src / dst - 0.5, w[4 * MAX_SHRINK], sum = 0;
		int first = filter == OS_PRESENT_NEAREST ? (int)floor(centre + 0.5) : (int)floor(centre - reach * stretch) + 1;
		int total = 0, best = 0;
		for(k = 0; k < taps; k++)
		{
			double x = (first + k - centre) / stretch;
			w[k] = filter == OS_PRESENT_NEAREST ? 1 : filter == OS_PRESENT_BICUBIC ? Cubic(x)
																				   : Tent(x);
			sum += w[k];
		}
		for(k = 0; k < taps; k++)
		{
			int at = first + k, v = (int)floor(w[k] / sum * WEIGHT_ONE + 0.5);
			a->index[i * taps + k] = at < 0 ? 0 : at >= src ? src - 1
															: at;
			a->weight[i * taps + k] = (short)v;
			total += v;
			if(w[k] > w[best])
				best = k;
		}
		a->weight[i * taps + best] = (short)(a->weight[i * taps + best] + WEIGHT_ONE - total); // the rounding goes to the heaviest
	}
	return 1;
}

// ---- the passes -------------------------------------------------------------------------------

/* Between the passes a value keeps KEEP_BITS bits behind the point and
 * whatever the first pass made of it, below 0 and above 255 too: cut off
 * there, an edge that the second pass would bring back into range would
 * come out wrong. */
#define KEEP_BITS 4

// row y of the picture as 32 bits
static const uint8_t* SourceRow(const OsPresent_t* p, int y)
{
	return p->bpp == 16 ? (const uint8_t*)(p->wide + (size_t)y * (size_t)p->sw) : p->pixels + (size_t)y * (size_t)p->pitch;
}

static void Widen(OsPresent_t* p, int y)
{
	const uint8_t* row = p->pixels + (size_t)y * (size_t)p->pitch;
	uint32_t* d = p->wide + (size_t)y * (size_t)p->sw;
	int x;
	for(x = 0; x < p->sw; x++)
	{
		unsigned v = (unsigned)(row[x * 2] | row[x * 2 + 1] << 8);
		unsigned b = v & 31, g = (v >> 5) & 31, r = (v >> 10) & 31;
		d[x] = ((uint32_t)(r << 3 | r >> 2) << 16) | ((uint32_t)(g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
	}
}

// a sum of weighted bytes as a value between the passes
static short Keep(int v)
{
	return (short)((v + (1 << (WEIGHT_BITS - KEEP_BITS - 1))) >> (WEIGHT_BITS - KEEP_BITS));
}

// row y of the picture scaled across, the part that is on the canvas: blue, green, red for each pixel
static void Across(OsPresent_t* p, int y)
{
	const uint8_t* s = SourceRow(p, y);
	short* d = p->across + (size_t)y * (size_t)p->dw * 3;
	const Axis_t* a = &p->ax;
	int taps = a->taps, x, k;
	if(p->dw == p->sw || taps == 1)
	{ // one pixel as it is
		for(x = p->x0; x < p->x1; x++)
		{
			const uint8_t* q = s + (size_t)(p->dw == p->sw ? x : a->index[x]) * 4;
			d[x * 3] = (short)(q[0] << KEEP_BITS);
			d[x * 3 + 1] = (short)(q[1] << KEEP_BITS);
			d[x * 3 + 2] = (short)(q[2] << KEEP_BITS);
		}
		return;
	}
	if(taps == 4)
	{ // (the usual case, spelled out: it is most of the work of a picture that grows)
		for(x = p->x0; x < p->x1; x++)
		{
			const int* at = a->index + (size_t)x * 4;
			const short* w = a->weight + (size_t)x * 4;
			const uint8_t* q0 = s + (size_t)at[0] * 4;
			const uint8_t* q1 = s + (size_t)at[1] * 4;
			const uint8_t* q2 = s + (size_t)at[2] * 4;
			const uint8_t* q3 = s + (size_t)at[3] * 4;
			int w0 = w[0], w1 = w[1], w2 = w[2], w3 = w[3];
			d[x * 3] = Keep(q0[0] * w0 + q1[0] * w1 + q2[0] * w2 + q3[0] * w3);
			d[x * 3 + 1] = Keep(q0[1] * w0 + q1[1] * w1 + q2[1] * w2 + q3[1] * w3);
			d[x * 3 + 2] = Keep(q0[2] * w0 + q1[2] * w1 + q2[2] * w2 + q3[2] * w3);
		}
		return;
	}
	for(x = p->x0; x < p->x1; x++)
	{
		const int* at = a->index + (size_t)x * (size_t)taps;
		const short* w = a->weight + (size_t)x * (size_t)taps;
		int b = 0, g = 0, r = 0;
		for(k = 0; k < taps; k++)
		{
			const uint8_t* q = s + (size_t)at[k] * 4;
			b += q[0] * w[k];
			g += q[1] * w[k];
			r += q[2] * w[k];
		}
		d[x * 3] = Keep(b);
		d[x * 3 + 1] = Keep(g);
		d[x * 3 + 2] = Keep(r);
	}
}

// a sum of `bits` bits behind the point as a byte
static uint32_t Byte(int v, int bits)
{
	v = (v + (1 << (bits - 1))) >> bits;
	return (uint32_t)(v < 0 ? 0 : v > 255 ? 255
										  : v);
}

// row y of the result, from the rows scaled across, into the canvas
static void Down(OsPresent_t* p, int y, int* sums)
{
	const Axis_t* a = &p->ay;
	const int* at = a->index + (size_t)y * (size_t)a->taps;
	const short* w = a->weight + (size_t)y * (size_t)a->taps;
	uint32_t* out = p->canvas + (size_t)(p->dy + y) * (size_t)p->cw + p->dx;
	int count = (p->x1 - p->x0) * 3, x, k, i;
	if(a->taps == 1 || p->dh == p->sh)
	{ // one row as it is
		const short* s = p->across + (size_t)(p->dh == p->sh ? y : at[0]) * (size_t)p->dw * 3;
		for(x = p->x0; x < p->x1; x++)
			out[x] = Byte(s[x * 3], KEEP_BITS) | Byte(s[x * 3 + 1], KEEP_BITS) << 8 | Byte(s[x * 3 + 2], KEEP_BITS) << 16;
		return;
	}
	for(i = 0; i < count; i++)
		sums[i] = 0;
	for(k = 0; k < a->taps; k++)
	{
		const short* s = p->across + (size_t)at[k] * (size_t)p->dw * 3 + (size_t)p->x0 * 3;
		int wk = w[k];
		if(!wk)
			continue;
		for(i = 0; i < count; i++)
			sums[i] += s[i] * wk;
	}
	for(x = p->x0, i = 0; x < p->x1; x++, i += 3)
		out[x] = Byte(sums[i], WEIGHT_BITS + KEEP_BITS) | Byte(sums[i + 1], WEIGHT_BITS + KEEP_BITS) << 8 |
			Byte(sums[i + 2], WEIGHT_BITS + KEEP_BITS) << 16;
}

// a row of a picture drawn at its own size, straight into the canvas
static void Plain(OsPresent_t* p, int y)
{
	uint32_t* out = p->canvas + (size_t)(p->dy + y) * (size_t)p->cw + p->dx + p->x0;
	memcpy(out, SourceRow(p, y) + (size_t)p->x0 * 4, (size_t)(p->x1 - p->x0) * 4);
}

// ---- the work, shared out -----------------------------------------------------------------------

enum
{
	PASS_WIDEN,
	PASS_ACROSS,
	PASS_DOWN,
	PASS_PLAIN
};

typedef struct Share
{
	OsPresent_t* p;
	int pass, from, to; // rows from .. to - 1
	int* sums;
} Share_t;

static void Work(void* arg)
{
	Share_t* s = (Share_t*)arg;
	OsPresent_t* p = s->p;
	int y;
	for(y = s->from; y < s->to; y++)
		switch(s->pass)
		{
			case PASS_WIDEN:
				if(p->rowDirty[y])
					Widen(p, y);
				break;
			case PASS_ACROSS:
				if(p->rowDirty[y])
					Across(p, y);
				break;
			case PASS_DOWN:
				if(p->outDirty[y])
					Down(p, y, s->sums);
				break;
			default:
				if(p->rowDirty[y])
					Plain(p, y);
				break;
		}
}

// rows from .. to - 1 of a pass, `rows` of them to do and `bytes` each: on as many threads as the job is worth
static void Run(OsPresent_t* p, int pass, int from, int to, int rows, size_t bytes)
{
	Share_t share[MAX_THREADS];
	int n = 1, i;
#if !defined(BGI_WASM)
	OsThread_t* thread[MAX_THREADS];
	size_t worth = (size_t)rows * bytes / THREAD_BYTES;
	n = worth < 1 ? 1 : worth > (size_t)p->threads ? p->threads
												   : (int)worth;
#endif
	(void)rows, (void)bytes;
	if(n > to - from)
		n = to - from > 0 ? to - from : 1;
	for(i = 0; i < n; i++)
	{
		share[i].p = p;
		share[i].pass = pass;
		share[i].from = from + (int)((int64_t)(to - from) * i / n);
		share[i].to = from + (int)((int64_t)(to - from) * (i + 1) / n);
		share[i].sums = p->sums + (size_t)i * (size_t)(p->x1 - p->x0) * 3;
	}
#if !defined(BGI_WASM)
	for(i = 1; i < n; i++)
		thread[i] = OS_ThreadStart(Work, &share[i], OS_PRIO_NORMAL);
	Work(&share[0]);
	for(i = 1; i < n; i++)
	{
		if(thread[i])
			OS_ThreadJoin(thread[i]);
		else
			Work(&share[i]); // no thread to be had: here
	}
#else
	Work(&share[0]);
#endif
}

// ---- the presenter --------------------------------------------------------------------------------

OsPresent_t* OsPresent_New(void)
{
	OsPresent_t* p = (OsPresent_t*)calloc(1, sizeof *p);
	if(!p)
		return NULL;
	p->threads = OS_CpuCount();
	if(p->threads > MAX_THREADS)
		p->threads = MAX_THREADS;
	if(p->threads < 1)
		p->threads = 1;
	return p;
}

static void FreePicture(OsPresent_t* p)
{
	free(p->rowDirty);
	free(p->outDirty);
	free(p->wide);
	free(p->across);
	free(p->sums);
	p->rowDirty = p->outDirty = NULL;
	p->wide = NULL;
	p->across = NULL;
	p->sums = NULL;
	Axis_Free(&p->ax);
	Axis_Free(&p->ay);
	p->ready = 0;
	p->sw = p->sh = p->dw = p->dh = 0;
}

void OsPresent_Delete(OsPresent_t* p)
{
	if(!p)
		return;
	FreePicture(p);
	free(p->canvas);
	free(p);
}

void OsPresent_Setup(OsPresent_t* p, int canvasW, int canvasH, int steps, int filter)
{
	if(!p)
		return;
	canvasW = canvasW < 1 ? 1 : canvasW > MAX_SIDE ? MAX_SIDE
												   : canvasW;
	canvasH = canvasH < 1 ? 1 : canvasH > MAX_SIDE ? MAX_SIDE
												   : canvasH;
	if(steps < OS_PRESENT_POW2 || steps > OS_PRESENT_STRETCH)
		steps = OS_PRESENT_POW2;
	if(filter < OS_PRESENT_BICUBIC || filter > OS_PRESENT_NEAREST)
		filter = OS_PRESENT_BICUBIC;
	if(canvasW == p->cw && canvasH == p->ch && steps == p->steps && filter == p->filter && p->canvas)
		return;
	if(canvasW != p->cw || canvasH != p->ch || !p->canvas)
	{
		free(p->canvas);
		p->canvas = (uint32_t*)malloc((size_t)canvasW * (size_t)canvasH * sizeof *p->canvas);
		p->black = 0;
	}
	p->cw = canvasW;
	p->ch = canvasH;
	p->steps = steps;
	p->filter = filter;
	FreePicture(p); // the next drawing makes its tables anew
}

void OsPresent_Changed(OsPresent_t* p, int y, int h)
{
	if(!p)
		return;
	if(h < 0 || !p->ready)
	{
		p->all = 1;
		return;
	}
	if(y < 0)
	{
		h += y;
		y = 0;
	}
	if(y >= p->sh || h <= 0)
		return;
	if(h > p->sh - y)
		h = p->sh - y;
	memset(p->rowDirty + y, 1, (size_t)h);
}

// the tables and buffers for a picture of w x h; 0 when memory ran out
static int Prepare(OsPresent_t* p, int w, int h, int bpp)
{
	FreePicture(p);
	OsPresent_Size(w, h, p->cw, p->ch, p->steps, &p->dw, &p->dh);
	p->sw = w;
	p->sh = h;
	p->bpp = bpp;
	p->dx = (p->cw - p->dw) / 2;
	p->dy = (p->ch - p->dh) / 2;
	p->x0 = p->dx < 0 ? -p->dx : 0;
	p->y0 = p->dy < 0 ? -p->dy : 0;
	p->x1 = p->dx + p->dw > p->cw ? p->cw - p->dx : p->dw;
	p->y1 = p->dy + p->dh > p->ch ? p->ch - p->dy : p->dh;
	p->scaled = p->dw != w || p->dh != h;
	p->rowDirty = (uint8_t*)calloc((size_t)h, 1);
	p->outDirty = (uint8_t*)calloc((size_t)p->dh, 1);
	if(!p->rowDirty || !p->outDirty)
		return 0;
	if(bpp == 16 && !(p->wide = (uint32_t*)malloc((size_t)w * (size_t)h * sizeof *p->wide)))
		return 0;
	if(p->scaled)
	{
		p->across = (short*)malloc((size_t)p->dw * (size_t)h * 3 * sizeof *p->across);
		p->sums = (int*)malloc((size_t)MAX_THREADS * (size_t)(p->x1 - p->x0) * 3 * sizeof *p->sums);
		if(!p->across || !p->sums || !Axis_Make(&p->ax, w, p->dw, p->filter) || !Axis_Make(&p->ay, h, p->dh, p->filter))
			return 0;
	}
	p->ready = 1;
	p->all = 1;
	return 1;
}

static void Black(OsPresent_t* p)
{
	if(!p->black)
		memset(p->canvas, 0, (size_t)p->cw * (size_t)p->ch * sizeof *p->canvas);
	p->black = 1;
}

const uint32_t* OsPresent_Draw(OsPresent_t* p, const uint8_t* pixels, int w, int h, int pitch, int bpp, int* changed)
{
	int y, k, first = -1, last = -1, rows = 0, outFirst = -1, outLast = -1, outRows = 0, fresh = 0;
	if(changed)
		*changed = 0;
	if(!p || !p->canvas)
		return NULL;
	p->drawnY = p->drawnH = 0;
	if(!pixels || w <= 0 || h <= 0 || w > MAX_SIDE || h > MAX_SIDE || (bpp != 32 && bpp != 16))
	{ // no picture: black, once
		if(changed)
			*changed = !p->black;
		p->drawnH = p->black ? 0 : p->ch;
		FreePicture(p);
		Black(p);
		p->all = 0;
		return p->canvas;
	}
	if(!p->ready || w != p->sw || h != p->sh || bpp != p->bpp)
	{
		if(!Prepare(p, w, h, bpp))
		{
			FreePicture(p);
			return NULL;
		}
		Black(p);
		fresh = 1; // (the bars beside the picture too)
	}
	if(p->all)
		memset(p->rowDirty, 1, (size_t)p->sh);
	p->all = 0;
	p->pixels = pixels;
	p->pitch = pitch;
	// the rows to do: of the picture, and of the result those reach
	for(y = 0; y < p->sh; y++)
		if(p->rowDirty[y])
		{
			if(first < 0)
				first = y;
			last = y;
			rows++;
		}
	if(!rows)
		return p->canvas;
	p->black = 0;
	if(changed)
		*changed = 1;
	if(p->bpp == 16)
		Run(p, PASS_WIDEN, first, last + 1, rows, (size_t)p->sw * 6);
	if(!p->scaled)
	{
		int from = first < p->y0 ? p->y0 : first, to = last + 1 > p->y1 ? p->y1 : last + 1;
		if(from < to)
		{
			Run(p, PASS_PLAIN, from, to, rows, (size_t)(p->x1 - p->x0) * 8);
			p->drawnY = p->dy + from;
			p->drawnH = to - from;
		}
		if(fresh)
		{
			p->drawnY = 0;
			p->drawnH = p->ch;
		}
		memset(p->rowDirty, 0, (size_t)p->sh);
		return p->canvas;
	}
	for(y = p->y0; y < p->y1; y++)
	{
		const int* at = p->ay.index + (size_t)y * (size_t)p->ay.taps;
		int need = 0;
		for(k = 0; k < p->ay.taps && !need; k++)
			need = p->rowDirty[at[k]];
		p->outDirty[y] = (uint8_t)need;
		if(need)
		{
			if(outFirst < 0)
				outFirst = y;
			outLast = y;
			outRows++;
		}
	}
	Run(p, PASS_ACROSS, first, last + 1, rows, (size_t)(p->x1 - p->x0) * 4 * (size_t)(p->ax.taps + 1));
	if(outRows)
	{
		Run(p, PASS_DOWN, outFirst, outLast + 1, outRows, (size_t)(p->x1 - p->x0) * 4 * (size_t)(p->ay.taps + 1));
		p->drawnY = p->dy + outFirst;
		p->drawnH = outLast + 1 - outFirst;
	}
	if(fresh)
	{
		p->drawnY = 0;
		p->drawnH = p->ch;
	}
	memset(p->rowDirty, 0, (size_t)p->sh);
	return p->canvas;
}

void OsPresent_Drawn(const OsPresent_t* p, int* y, int* h)
{
	*y = p ? p->drawnY : 0;
	*h = p ? p->drawnH : 0;
}

void OsPresent_Place(const OsPresent_t* p, int* x, int* y, int* w, int* h)
{
	int ready = p && p->ready;
	*x = ready ? p->dx : 0;
	*y = ready ? p->dy : 0;
	*w = ready ? p->dw : 0;
	*h = ready ? p->dh : 0;
}

int OsPresent_ToPicture(const OsPresent_t* p, int canvasX, int canvasY, int* x, int* y)
{
	int inside, px, py;
	*x = *y = 0;
	if(!p || !p->ready || p->dw <= 0 || p->dh <= 0)
		return 0;
	inside = canvasX >= p->dx && canvasX < p->dx + p->dw && canvasY >= p->dy && canvasY < p->dy + p->dh;
	px = (int)floor(((double)(canvasX - p->dx) + 0.5) * p->sw / p->dw);
	py = (int)floor(((double)(canvasY - p->dy) + 0.5) * p->sh / p->dh);
	*x = px < 0 ? 0 : px >= p->sw ? p->sw - 1
								  : px;
	*y = py < 0 ? 0 : py >= p->sh ? p->sh - 1
								  : py;
	return inside;
}
