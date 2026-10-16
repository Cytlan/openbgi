/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * compositor.c - the compositor: the display objects sorted by key, the
 * overlap-free list of dirty rectangles and the band renderer that repaints
 * them, plus the worker pool it paints with (synchronous here).  Interface
 * in inc/bgi/gfx/compositor.h.
 *
 * Objects report the rectangles they change through Compositor_AddDirty
 * (via DispObj_Invalidate).  Once per frame the graphics manager calls
 * Compositor_Render: the dirty rectangles are cut into horizontal bands of
 * about `bandPixels` pixels, and for every band each object's draw method
 * is called in ascending key order through DispObj_DrawClipped.  A full
 * redraw (Compositor_InvalidateAll, or too many dirty rectangles) paints
 * the whole screen instead.  Banding is given up whenever an object reads
 * what the others painted (an effector, or a background type that
 * processes the screen): the frame is then painted as one job.
 */
#include "bgi/gfx/compositor.h"

// ========================================================================
// worker pool
// ========================================================================

WorkerPool_t* WorkerPool_New(int threads)
{
	WorkerPool_t* p = (WorkerPool_t*)BGI_Alloc(sizeof *p);
	p->threads = threads;
	p->fn = NULL;
	p->arg = NULL;
	p->running = 0;
	return p;
}

void WorkerPool_Delete(WorkerPool_t* p)
{
	BGI_Free(p);
}

void WorkerPool_SetCallback(WorkerPool_t* p, PoolFn_t fn, void* arg)
{
	p->fn = fn;
	p->arg = arg;
}

/* Run the callback until it returns 0.  The original wakes its threads and
 * lets every one of them, including the caller, call fn(arg) until it
 * returns 0; running them one after another on this thread consumes the
 * same job queue in order and paints the same pixels, because jobs never
 * overlap.  `parallel` is only recorded in `running`. */
void WorkerPool_Run(WorkerPool_t* p, int parallel)
{
	p->running = parallel;
	if(p->fn)
		while(p->fn(p->arg))
			;
	p->running = 0;
}

// take the job-queue lock; returns whether one was taken (nothing to lock on one thread)
int WorkerPool_Lock(WorkerPool_t* p)
{
	return p->threads > 1 && p->running;
}

void WorkerPool_Unlock(WorkerPool_t* p, int locked)
{
	(void)p;
	(void)locked;
}

// ========================================================================
// life cycle
// ========================================================================

/* `maxDirty` dirty rectangles are kept before a full redraw is requested
 * instead; `screen` is the back buffer with its rectangle; `bandPixels`
 * the pixel budget of one band; `sharedPool` a pool to paint with, or NULL
 * for a private single-threaded one. */
void Compositor_Ctor(Compositor_t* c, int maxDirty, ScreenBmp_t* screen, uint32_t bandPixels, WorkerPool_t* sharedPool)
{
	c->maxDirty = maxDirty;
	c->screen = screen;
	c->bandPixels = bandPixels;
	if(sharedPool)
	{
		c->pool = sharedPool;
		c->ownsPool = 0;
	}
	else
	{
		c->pool = WorkerPool_New(1);
		c->ownsPool = 1;
	}
	c->objects = NULL;
	c->objectCount = 0;
	c->dirty = NULL;
	c->dirtyCount = 0;
	Compositor_ClearDirty(c);
	Compositor_SetDrawLimit(c, 0);
	Compositor_SetBgType(c, 0);
	Compositor_InvalidateAll(c);
	c->jobCount = 0;
	c->jobs = NULL;
	c->nextJob = 0;
}

// the objects are unlisted, not destroyed
void Compositor_Dtor(Compositor_t* c)
{
	Compositor_RemoveAll(c);
	if(c->ownsPool && c->pool)
		WorkerPool_Delete(c->pool);
}

void Compositor_RemoveAll(Compositor_t* c)
{
	while(c->objects)
		Compositor_Remove(c, c->objects->obj);
	Compositor_ClearDirty(c);
}

// ========================================================================
// object list
// ========================================================================

// insert at the place its current sort key gives it; the key is recorded in the node
void Compositor_Add(Compositor_t* c, DispObj_t* o)
{
	uint32_t key = o->vt->sortKey(o);
	ObjNode_t **link = &c->objects, *n;
	while(*link && (*link)->key <= key) // equal keys: the newcomer goes on top
		link = &(*link)->next;
	n = (ObjNode_t*)BGI_Alloc(sizeof *n);
	n->key = key;
	n->unused04 = 0;
	n->obj = o;
	n->next = *link;
	*link = n;
	c->objectCount++;
}

// 1 if the object was listed
int Compositor_Remove(Compositor_t* c, DispObj_t* o)
{
	ObjNode_t **link, *n;
	for(link = &c->objects; (n = *link) != NULL; link = &n->next)
	{
		if(n->obj == o)
		{
			*link = n->next;
			BGI_Free(n);
			c->objectCount--;
			return 1;
		}
	}
	return 0;
}

// after the object's priority or depth changed: remove and re-insert; 0 if it was not listed
int Compositor_Resort(Compositor_t* c, DispObj_t* o)
{
	if(!Compositor_Remove(c, o))
		return 0;
	Compositor_Add(c, o);
	return 1;
}

// call every object's notify method (0xF0000000 after each rendered frame)
void Compositor_NotifyAll(Compositor_t* c, uint32_t what, int a, int b)
{
	ObjNode_t* n;
	for(n = c->objects; n; n = n->next)
		n->obj->vt->notify(n->obj, what, a, b);
}

// ========================================================================
// dirty rectangles
// ========================================================================

// drop every dirty rectangle and the full-redraw request
void Compositor_ClearDirty(Compositor_t* c)
{
	DirtyNode_t* d = c->dirty;
	while(d)
	{
		DirtyNode_t* next = d->next;
		BGI_Free(d);
		d = next;
	}
	c->dirty = NULL;
	c->dirtyCount = 0;
	c->fullRedraw = 0;
}

// the next render paints the whole screen
void Compositor_InvalidateAll(Compositor_t* c)
{
	c->fullRedraw = 1;
}

// "90 09": objects whose key is below priority `p` are not drawn (backgrounds always are)
void Compositor_SetDrawLimit(Compositor_t* c, int p)
{
	c->drawLimit = (uint32_t)p << 20;
}

// the current background type, which decides whether banding is possible
void Compositor_SetBgType(Compositor_t* c, int type)
{
	c->bgType = type;
}

static inline int32_t Imin(int32_t a, int32_t b)
{
	return a < b ? a : b;
}

static inline int32_t Imax(int32_t a, int32_t b)
{
	return a > b ? a : b;
}

static inline uint32_t Umin(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static inline uint32_t RectArea(const Rect_t* r)
{
	return (uint32_t)(r->b - r->t + 1) * (uint32_t)(r->r - r->l + 1);
}

/* Insert `r` (clipped to the screen in place; nothing happens when it lies
 * outside) into the dirty list so that no two rectangles overlap.
 * `minKey` is the sort key of the object that dirtied it; a merged
 * rectangle keeps the lowest.  For every existing rectangle N that the new
 * rectangle R meets:
 *   - R inside N: nothing to do.
 *   - the bounding box U of the two costs little (area(U) <= larger +
 *     smaller / 2) or both span the same rows: N is replaced by U.
 *   - R's rows lie within N's rows: R grows to the horizontal union, N is
 *     cut (or split) around R's rows, and the scan continues.
 *   - N's rows lie within R's rows: N is replaced by the horizontal union
 *     over its rows; R's rows above and below are inserted separately.
 *   - partial row overlap: whichever rectangle is horizontally inside the
 *     other is trimmed to its non-overlapping rows; when neither is, the
 *     overlapping rows become the union and both remainders are inserted.
 * Replacements and remainders go through this function again, so the
 * invariant holds after every call.  A full list requests a full redraw
 * instead. */
void Compositor_AddDirty(Compositor_t* c, uint32_t minKey, Rect_t* r)
{
	Rect_t R, U;
	DirtyNode_t **link, *n;

	if(c->dirtyCount >= c->maxDirty)
	{
		Compositor_InvalidateAll(c);
		return;
	}
	if(!Rect_Clip(r, &c->screen->screen))
		return;
	R = *r;

	for(link = &c->dirty; (n = *link) != NULL; link = &n->next)
	{
		Rect_t* N = &n->r;
		uint32_t areaU, areaN, areaR, limit, key;

		if(!Rect_Intersects(&R, N))
			continue;
		if(Rect_Inside(&R, N))
			return;

		U.l = Imin(R.l, N->l);
		U.r = Imax(R.r, N->r);
		U.t = Imin(R.t, N->t);
		U.b = Imax(R.b, N->b);
		areaU = RectArea(&U);
		areaN = RectArea(N);
		areaR = RectArea(&R);
		limit = areaN <= areaR ? areaR + areaN / 2 : areaN + areaR / 2;

		if(areaU <= limit || (R.t == N->t && R.b == N->b))
		{
			// merge both into the bounding box
			key = Umin(minKey, n->minKey);
			*link = n->next;
			c->dirtyCount--;
			Compositor_AddDirty(c, key, &U);
			BGI_Free(n);
			return;
		}

		if(R.t >= N->t && R.b <= N->b)
		{
			// R's rows inside N's rows: R takes the horizontal union, N
			// gives up those rows
			if(R.l > N->l)
				R.l = N->l;
			if(R.r < N->r)
				R.r = N->r;
			if(N->t < R.t)
			{
				if(R.b < N->b)
				{ // N is split around R
					U.l = N->l;
					U.r = N->r;
					U.t = R.b + 1;
					U.b = N->b;
					N->b = R.t - 1;
					Compositor_AddDirty(c, n->minKey, &U);
				}
				else
				{
					N->b = R.t - 1;
				}
			}
			else
			{
				N->t = R.b + 1;
			}
		}
		else if(N->t >= R.t && N->b <= R.b)
		{
			// N's rows inside R's rows
			*link = n->next;
			c->dirtyCount--;
			U.t = N->t;
			U.b = N->b;
			key = Umin(minKey, n->minKey);
			Compositor_AddDirty(c, key, &U); // the union over N's rows
			U.l = R.l;
			U.r = R.r;
			if(R.t < N->t)
			{
				U.t = R.t;
				U.b = N->t - 1;
				Compositor_AddDirty(c, minKey, &U); // R above N
			}
			if(N->b < R.b)
			{
				U.t = N->b + 1;
				U.b = R.b;
				Compositor_AddDirty(c, minKey, &U); // R below N
			}
			BGI_Free(n);
			return;
		}
		else if(R.l >= N->l && R.r <= N->r)
		{
			// partial rows, R horizontally inside N: keep R's free rows
			if(R.t < N->t)
				R.b = N->t - 1;
			else
				R.t = N->b + 1;
		}
		else if(R.l <= N->l && N->r <= R.r)
		{
			// partial rows, N horizontally inside R: trim N
			if(R.t < N->t)
				N->t = R.b + 1;
			else
				N->b = R.t - 1;
		}
		else
		{
			// partial rows, neither inside the other: the shared rows
			// become the union, the two remainders are re-inserted.  (The
			// original reads N's key after the first re-insertion, which
			// may already have freed N; the key is taken beforehand.)
			uint32_t nKey = n->minKey;
			if(R.t < N->t)
			{
				U.t = N->t;
				U.b = R.b;
				N->t = R.b + 1;
				R.b = U.t - 1;
			}
			else
			{
				U.t = R.t;
				U.b = N->b;
				N->b = R.t - 1;
				R.t = U.b + 1;
			}
			Compositor_AddDirty(c, minKey, &R);
			Compositor_AddDirty(c, Umin(minKey, nKey), &U);
			return;
		}
		if(n->minKey < minKey)
			minKey = n->minKey;
	}

	n = (DirtyNode_t*)BGI_Alloc(sizeof *n);
	n->r = R;
	n->minKey = minKey;
	n->next = c->dirty;
	c->dirty = n;
	c->dirtyCount++;
}

// ========================================================================
// rendering
// ========================================================================

/* Cut `r` into horizontal bands of about bandPixels pixels (at least one
 * row each); returns the number of bands and, when `out` is given, writes
 * them there. */
int Compositor_SplitBands(Compositor_t* c, Rect_t* out, const Rect_t* r)
{
	uint32_t w = (uint32_t)(r->r - r->l + 1);
	uint32_t rows = c->bandPixels / w;
	int32_t h = r->b - r->t + 1, n, i, y = r->t;
	if(rows == 0)
		rows = 1;
	n = (int32_t)(((int64_t)h + (int64_t)rows - 1) / (int64_t)rows);
	if(out && n > 0)
	{
		for(i = 0; i < n; i++)
		{
			int32_t take = h < (int32_t)rows ? h : (int32_t)rows;
			out[i].l = r->l;
			out[i].r = r->r;
			out[i].t = y;
			out[i].b = y + take - 1;
			y += take;
			h -= take;
		}
	}
	return n;
}

/* Whether the frame may be painted in bands: not while the background is
 * of a type that reads the screen around each pixel (6 displacement, 7
 * gradient, 9 stretched view, 10 zoom, 11 mosaic) and not while an
 * effector is visible. */
int Compositor_CanBand(Compositor_t* c)
{
	int t = c->bgType;
	if(t == 6 || t == 7 || t == 9 || t == 10 || t == 11)
		return 0;
	return !Effector_AnyVisible();
}

/* The pool callback: take the next band job and paint every object into
 * it, in key order; objects below the draw limit are skipped unless they
 * are backgrounds (class order 0).  Returns 0 when the queue is empty. */
static int Compositor_Worker(void* arg)
{
	Compositor_t* c = (Compositor_t*)arg;
	BandJob_t job;
	ObjNode_t* n;
	int got = 0, locked = WorkerPool_Lock(c->pool);
	if(c->nextJob < c->jobCount)
	{
		job = c->jobs[c->nextJob++];
		got = 1;
	}
	WorkerPool_Unlock(c->pool, locked);
	if(!got)
		return 0;
	for(n = c->objects; n; n = n->next)
	{
		DispObj_t* o = n->obj;
		if(o->vt->sortKey(o) < c->drawLimit && o->classOrder != 0)
			continue;
		DispObj_DrawClipped(o, c->screen, &job.r, job.minKey);
	}
	return 1;
}

// paint the queued jobs through the pool
static void Compositor_RunJobs(Compositor_t* c, int parallel)
{
	WorkerPool_SetCallback(c->pool, Compositor_Worker, c);
	WorkerPool_Run(c->pool, parallel);
	WorkerPool_SetCallback(c->pool, NULL, NULL);
}

/* Paint the whole screen: in bands when banding is possible, else as one
 * job.  The dirty list is cleared afterwards and every object notified
 * (0xF0000000). */
void Compositor_RenderAll(Compositor_t* c)
{
	int parallel = Compositor_CanBand(c), i;
	c->jobCount = parallel ? Compositor_SplitBands(c, NULL, &c->screen->screen) : 1;
	c->jobs = (BandJob_t*)BGI_Alloc((size_t)c->jobCount * sizeof(BandJob_t));
	c->nextJob = 0;
	if(c->jobCount > 1)
	{
		Rect_t* bands = (Rect_t*)BGI_Alloc((size_t)c->jobCount * sizeof(Rect_t));
		Compositor_SplitBands(c, bands, &c->screen->screen);
		for(i = 0; i < c->jobCount; i++)
		{
			c->jobs[i].r = bands[i];
			c->jobs[i].minKey = 0;
		}
		BGI_Free(bands);
	}
	else
	{
		c->jobs[0].r = c->screen->screen;
		c->jobs[0].minKey = 0;
	}
	Compositor_RunJobs(c, parallel);
	BGI_Free(c->jobs);
	c->jobs = NULL;
	Compositor_ClearDirty(c);
	Compositor_NotifyAll(c, 0xf0000000u, 0, 0);
}

/* Paint one frame: the dirty rectangles, each cut into bands, with the
 * objects that redraw every frame (setParam 0x7FFF0000, 1.494 on) dirtied
 * first.  Writes the repainted rectangles to `outRects` (room for maxDirty
 * of them) and returns their count, so that the caller can copy just
 * those to the window; -1 when the whole screen was painted instead
 * (a full redraw was requested, or banding is not possible). */
int Compositor_Render(Compositor_t* c, Rect_t* outRects)
{
	int* counts;
	uint32_t* keys;
	Rect_t* bands;
	DirtyNode_t* d;
	ObjNode_t* n;
	int count = 0, total = 0, i, k = 0;

	// 1.494 on: the objects that redraw every frame (setParam 0x7FFF0000)
	for(n = c->objects; n; n = n->next)
		if(n->obj->redrawEveryFrame)
			n->obj->vt->invalidate(n->obj);
	if(c->fullRedraw || !Compositor_CanBand(c))
	{
		Compositor_RenderAll(c);
		return -1;
	}
	counts = (int*)BGI_Alloc((size_t)c->maxDirty * sizeof(int));
	keys = (uint32_t*)BGI_Alloc((size_t)c->maxDirty * sizeof(uint32_t));
	for(d = c->dirty; d; d = d->next, count++)
	{
		counts[count] = Compositor_SplitBands(c, NULL, &d->r);
		total += counts[count];
		keys[count] = d->minKey;
		outRects[count] = d->r;
	}
	c->jobCount = total;
	c->jobs = (BandJob_t*)BGI_Alloc((size_t)(total ? total : 1) * sizeof(BandJob_t));
	c->nextJob = 0;
	// room for one band per screen row, the most any rectangle can split into
	bands = (Rect_t*)BGI_Alloc((size_t)(c->screen->screen.b - c->screen->screen.t + 1) * sizeof(Rect_t) + sizeof(Rect_t));
	for(i = 0; i < count; i++)
	{
		if(counts[i] > 1)
		{
			int j, m = Compositor_SplitBands(c, bands, &outRects[i]);
			for(j = 0; j < m; j++, k++)
			{
				c->jobs[k].r = bands[j];
				c->jobs[k].minKey = keys[i];
			}
		}
		else
		{
			c->jobs[k].r = outRects[i];
			c->jobs[k].minKey = keys[i];
			k++;
		}
	}
	Compositor_RunJobs(c, 1);
	BGI_Free(counts);
	BGI_Free(keys);
	BGI_Free(c->jobs);
	c->jobs = NULL;
	BGI_Free(bands);
	Compositor_ClearDirty(c);
	Compositor_NotifyAll(c, 0xf0000000u, 0, 0);
	return count;
}

/* "90 05": paint every object with key <= (prio << 20) | 0xFFFFF -
 * everything up to and including priority `prio` - into `dst`, which
 * stands in for the screen; the dirty list is not touched. */
void Compositor_Snapshot(Compositor_t* c, const Bmp_t* dst, int prio)
{
	ScreenBmp_t s;
	ObjNode_t* n;
	uint32_t limit = ((uint32_t)prio << 20) | 0xfffffu;
	s.bmp = *dst;
	Rect_FromBmp(&s.screen, dst);
	for(n = c->objects; n; n = n->next)
	{
		DispObj_t* o = n->obj;
		if(o->vt->sortKey(o) <= limit)
			DispObj_DrawClipped(o, &s, &s.screen, 0);
	}
}
