/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * frame.c - the graphics manager's frame cycle: rendering through the
 *           compositor, the scaled copy of the back buffer, presenting
 *           to the window, snapshots (inc/bgi/gfx/gfxmgr.h)
 *
 * A frame is Gfx_Render (the compositor repaints the dirty rectangles of
 * the back buffer and, when the window is scaled, Gfx_UpdateScaled
 * stretches them into the scaled surface) followed by Gfx_Present, which
 * copies the rectangles to the window.  Gfx_RenderAll / Gfx_PresentAll do
 * the same for the whole screen.  The scaled presentation keeps the
 * horizontal size and stretches vertically by the factor of the scale
 * mode, centred on the back buffer's centre; the stretch is cut into band
 * jobs for the worker pool.
 */
#include <string.h>

#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/objects.h"
#include "bgi/gfx/text.h"
#include "bgi/gfx.h"
#include "bgi/engine.h"
#include "bgi/file.h"
#include "bgi/sys.h"

// repaint the whole back buffer (and the scaled surface); 0 without a back buffer
int Gfx_RenderAll(Gfx_t* g)
{
	if(!g->back.pixels)
		return 0;
	Compositor_RenderAll(g->comp);
	Gfx_UpdateScaled(g, 0, NULL);
	return 1;
}

/* paint what is dirty; *count receives the number of repainted rectangles
 * written to `rects`, or -1 when the compositor repainted everything.
 * 0 without a back buffer. */
int Gfx_Render(Gfx_t* g, int* count, Rect_t* rects)
{
	if(!g->back.pixels)
		return 0;
	*count = Compositor_Render(g->comp, rects);
	if(*count == -1)
		Gfx_UpdateScaled(g, 0, NULL);
	else
		Gfx_UpdateScaled(g, *count, rects);
	return 1;
}

// the back buffer as an OS surface for the window blit
static OsSurface_t BackSurface(Gfx_t* g)
{
	OsSurface_t s;
	s.pixels = g->back.pixels;
	s.pitch = g->back.pitch;
	s.width = g->back.w;
	s.height = g->back.h;
	s.bpp = ModeBits(g->back.mode);
	return s;
}

// the scaled surface as an OS surface for the window blit
static OsSurface_t ScaledSurface(Gfx_t* g)
{
	OsSurface_t s;
	s.pixels = g->scaled.pixels;
	s.pitch = g->scaled.pitch;
	s.width = g->scaled.w;
	s.height = g->scaled.h;
	s.bpp = ModeBits(g->scaled.mode);
	return s;
}

/* one BitBlt of the whole surface (the scaled one when the window is
 * scaled) to (x, y) of the window.  1 ok, 0 without a back buffer. */
int Gfx_PresentAll(Gfx_t* g, OsDc_t* dc, int x, int y)
{
	OsSurface_t s;

	if(!g->backMem)
		return 0;
	if(!g->scaledMem)
	{
		s = BackSurface(g);
		OS_BlitToWindow(dc, x, y, g->back.w, g->back.h, &s, 0, 0);
	}
	else
	{
		s = ScaledSurface(g);
		OS_BlitToWindow(dc, x, y, g->scaled.w, g->scaled.h, &s, 0, 0);
	}
	return 1;
}

int Gfx_PresentAllAt0(Gfx_t* g, OsDc_t* dc)
{
	return Gfx_PresentAll(g, dc, 0, 0);
}

/* BitBlt each of the `count` back-buffer rectangles to the window; from
 * the scaled surface, after mapping the rectangle, when the window is
 * scaled.  1 ok, 0 without a back buffer. */
int Gfx_Present(Gfx_t* g, OsDc_t* dc, const Rect_t* rects, int count)
{
	OsSurface_t s;
	Rect_t m;
	uint32_t i;

	if(!g->backMem)
		return 0;
	if(!g->scaledMem)
	{
		s = BackSurface(g);
		for(i = 0; i < (uint32_t)count; i++)
		{
			const Rect_t* r = &rects[i];
			OS_BlitToWindow(dc, r->l, r->t, r->r - r->l + 1, r->b - r->t + 1, &s, r->l, r->t);
		}
	}
	else
	{
		s = ScaledSurface(g);
		for(i = 0; i < (uint32_t)count; i++)
		{
			Gfx_MapRectScaled(g, &m, &rects[i]);
			OS_BlitToWindow(dc, m.l, m.t, m.r - m.l + 1, m.b - m.t + 1, &s, m.l, m.t);
		}
	}
	return 1;
}

// the vertical stretch factor (16.16) of each scale mode: 1.0, 0.9375, 1.2, 1.25, 1.28, 1.28
int32_t Gfx_ScaleFactor(Gfx_t* g)
{
	static const int32_t table[6] = {0x10000, 0xf000, 0x13333, 0x14000, 0x147ae, 0x147ae};
	return table[g->scaleMode];
}

/* back-buffer rectangle -> scaled-surface rectangle.  x is centred (the
 * left edge rounded down to even, the right up to odd), y is scaled about
 * the centre (bottom rounded up). */
void Gfx_MapRectScaled(Gfx_t* g, Rect_t* out, const Rect_t* in)
{
	int32_t f = Gfx_ScaleFactor(g);
	int32_t dx = (int32_t)((uint32_t)g->scaled.w >> 1) - (g->back.w >> 1);
	int32_t sh = (int32_t)((uint32_t)g->scaled.h >> 1);
	int32_t bh = g->back.h >> 1;

	out->l = (dx + in->l) & ~1;
	out->r = (dx + in->r) | 1;
	out->t = (((in->t - bh) * f) >> 16) + sh;
	out->b = ((((in->b - bh) * f) + 0xffff) >> 16) + sh;
}

/* map a point between back-buffer and window coordinates when the window
 * is scaled (dir 0: back -> window, 1: window -> back); the identity
 * otherwise. */
void Gfx_MapPoint(Gfx_t* g, int32_t out[2], int x, int y, int dir)
{
	int32_t f;

	if(g->scaledMem && dir == 0)
	{
		f = Gfx_ScaleFactor(g);
		out[0] = x + (int32_t)((uint32_t)(g->scaled.w - g->back.w) >> 1);
		out[1] = ((f * (y - (g->back.h >> 1))) >> 16) + (int32_t)((uint32_t)g->scaled.h >> 1);
		return;
	}
	if(g->scaledMem && dir == 1)
	{
		double d = (double)(y - (int32_t)((uint32_t)g->scaled.h >> 1)) * 65536.0;
		f = Gfx_ScaleFactor(g);
		out[0] = x - (int32_t)((uint32_t)(g->scaled.w - g->back.w) >> 1);
		out[1] = BGI_Ftol(d / (double)f + (double)(g->back.h >> 1));
		return;
	}
	out[0] = x;
	out[1] = y;
}

/* cut `r` into horizontal bands of at most pixelsPerJob pixels (at least
 * one row each); writes them to `out` when given and returns their
 * number, so a first call with NULL counts and a second one fills. */
uint32_t Gfx_SplitCopyJobs(Rect_t* out, const Rect_t* r, uint32_t pixelsPerJob)
{
	uint32_t width = (uint32_t)(r->r - r->l + 1);
	uint32_t rowsPer = pixelsPerJob / width;
	uint32_t height = (uint32_t)(r->b - r->t); // exclusive: rows - 1
	uint32_t count, remaining, i, y;

	if(rowsPer == 0)
		rowsPer = 1;
	count = (height + rowsPer) / rowsPer;
	if(out && count > 0)
	{
		remaining = height + 1;
		y = (uint32_t)r->t;
		for(i = 0; i < count; i++)
		{
			uint32_t rows = remaining < rowsPer ? remaining : rowsPer;
			out[i].l = r->l;
			out[i].t = (int32_t)y;
			out[i].r = r->r;
			out[i].b = (int32_t)(y + rows - 1);
			y += rows;
			remaining -= rows;
		}
	}
	return count;
}

// the worker pool callback: one copy job per call
static int CopyJobThunk(void* arg)
{
	return Gfx_CopyJob((Gfx_t*)arg);
}

/* stretch the changed rectangles into the scaled surface.  NULL rects
 * means the whole screen; count 0 with a rectangle list does nothing.
 * The mapped rectangles are split into band jobs (one band per job when
 * the pool has a single thread) and run through the pool.  Nothing
 * happens when the window is not scaled. */
void Gfx_UpdateScaled(Gfx_t* g, int count, const Rect_t* rects)
{
	Rect_t whole, *mapped, *p;
	uint32_t ppj, total = 0, i, n;

	if(!g->scaled.pixels)
		return;
	if(rects == NULL)
	{
		Rect_FromBmp(&whole, &g->back);
		rects = &whole;
		count = 1;
	}
	else if(count == 0)
	{
		return;
	}
	n = (uint32_t)count;
	mapped = (Rect_t*)BGI_Alloc((size_t)n * sizeof(Rect_t));
	ppj = g->pool->threads > 1 ? Gfx_GetBandPixels(g) : 0xffffffffu;
	for(i = 0; i < n; i++)
	{
		Gfx_MapRectScaled(g, &mapped[i], &rects[i]);
		total += Gfx_SplitCopyJobs(NULL, &mapped[i], ppj);
	}
	g->copyJobCount = (int32_t)total;
	g->copyJobs = (Rect_t*)BGI_Alloc((size_t)total * sizeof(Rect_t));
	g->copyJobNext = 0;
	p = g->copyJobs;
	for(i = 0; i < n; i++)
		p += Gfx_SplitCopyJobs(p, &mapped[i], ppj);

	WorkerPool_SetCallback(g->pool, CopyJobThunk, g);
	WorkerPool_Run(g->pool, 1);
	WorkerPool_SetCallback(g->pool, NULL, NULL);
	BGI_Free(g->copyJobs);
	BGI_Free(mapped);
}

/* take one job off the queue (under the pool's lock) and stretch the back
 * buffer into that band of the scaled surface: the back buffer's centre
 * lands on the scaled surface's centre, x unscaled, y scaled by the
 * mode's factor.  Returns 1 when a job was taken, 0 when the queue is
 * empty (which ends the pool run). */
int Gfx_CopyJob(Gfx_t* g)
{
	Rect_t r, sr;
	Bmp_t dst;
	int got = 0, locked;
	int32_t f;

	locked = WorkerPool_Lock(g->pool);
	if((uint32_t)g->copyJobNext < (uint32_t)g->copyJobCount)
	{
		r = g->copyJobs[g->copyJobNext++];
		got = 1;
	}
	WorkerPool_Unlock(g->pool, locked);
	if(!got)
		return 0;

	Rect_FromBmp(&sr, &g->scaled);
	if(!Rect_Clip(&r, &sr))
		return got;
	dst = g->scaled;
	Bmp_Crop(&dst, &r);
	f = Gfx_ScaleFactor(g);
	// the centres in 16.16, relative to the band: (w / 2 - l, h / 2 - t) << 16
	Blit_XformVscale(&dst,
		(int32_t)((uint32_t)((g->scaled.w & ~1) - 2 * r.l) << 15),
		(int32_t)((uint32_t)((g->scaled.h & ~1) - 2 * r.t) << 15),
		&g->back,
		(int32_t)((uint32_t)g->back.w << 15),
		(int32_t)((uint32_t)g->back.h << 15),
		f);
	return got;
}

/* "90 04": create bitmap bmpNo at screen size and copy the back buffer
 * into it.  Returns the bitmap manager's result: 1 ok, 0 for a bad slot
 * number or a failed allocation. */
int Gfx_Snapshot(Gfx_t* g, int bmpNo)
{
	Bmp_t back, info;
	int r;

	Gfx_CopyBackBmp(g, &back);
	r = BmpMgr_Create(gGfxBmpMgr, bmpNo, back.w, back.h, back.mode);
	if(r)
	{
		BmpMgr_GetInfo(gGfxBmpMgr, &info, bmpNo);
		Bmp_CopyRect(&info, &back);
	}
	return r;
}

/* "90 05": as above, but the picture is re-composed from the objects up
 * to priority `prio` instead of copied. */
int Gfx_SnapshotPrio(Gfx_t* g, int bmpNo, int prio)
{
	Bmp_t back, info;
	int r;

	Gfx_CopyBackBmp(g, &back);
	r = BmpMgr_Create(gGfxBmpMgr, bmpNo, back.w, back.h, back.mode);
	if(r)
	{
		BmpMgr_GetInfo(gGfxBmpMgr, &info, bmpNo);
		Compositor_Snapshot(g->comp, &info, prio);
	}
	return r;
}

// "90 08": the global effect value (stored only; nothing reads it); the whole screen is repainted
void Gfx_SetGlobalEffect(Gfx_t* g, int v)
{
	Gfx_SetGlobalEffectValue(v);
	Compositor_InvalidateAll(g->comp);
}

// "90 09": objects below priority `prio` are not drawn; the whole screen is repainted
void Gfx_SetDrawLimit(Gfx_t* g, int prio)
{
	Compositor_SetDrawLimit(g->comp, prio);
	Compositor_InvalidateAll(g->comp);
}

// the draw limit as the compositor keeps it (priority << 20)
uint32_t Gfx_GetDrawLimit(Gfx_t* g)
{
	return g->comp->drawLimit;
}

// the compositor's pixel budget per band
uint32_t Gfx_GetBandPixels(Gfx_t* g)
{
	return g->comp->bandPixels;
}

// repaint the whole screen at the next render
void Gfx_InvalidateAll(Gfx_t* g)
{
	Compositor_InvalidateAll(g->comp);
}

// hand a dirty rectangle (screen coordinates) with the sort key of the object that changed to the compositor
void Gfx_AddDirty(Gfx_t* g, uint32_t key, const Rect_t* r)
{
	Rect_t tmp = *r; // Compositor_AddDirty clips in place
	Compositor_AddDirty(g->comp, key, &tmp);
}

// re-sort an object whose sort key (priority, depth) may have changed; 1 if the compositor knows it
int Gfx_Resort(Gfx_t* g, DispObj_t* o)
{
	return Compositor_Resort(g->comp, o);
}
