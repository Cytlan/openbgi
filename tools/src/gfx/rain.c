/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * rain.c - the rain generator behind the rain screens ("C0 4x") and the
 *          per-pass driver that redraws them at the effect frame rate.
 *          Interface: inc/bgi/gfx/screens.h (RainGen_*, Fx_SetEffectFps)
 *          and bgi/gfx.h (Rain_UpdateAll, Rain_FrameTick).
 *
 * A raindrop is a 3-D segment that falls along a direction vector; every
 * `stageMs` milliseconds the generator moves all drops by `fall`, removes
 * the ones whose tail passed `yBottom` and spawns `perStage` new ones on
 * the `yTop` plane.  Rendering projects both ends through a rotated camera
 * and draws a 2-D line (LineRenderer, a small rasteriser with its own
 * clipping) whose alpha fades with the depth of the tail.  World
 * coordinates are plain integers, y up; `fall` and `length` are 24.8 and
 * only their integer parts are used.  See docs/particle_rain.md.
 */
#include "bgi/gfx.h"
#include "bgi/gfx/screens.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/display.h"
#include "bgi/sys.h"
#include <math.h>

//  --- the line rasteriser -----------------------------

typedef struct LineRenderer // 0x2C bytes
{
	Rect_t clip;          // inclusive, within the surface
	int32_t w, h;         // the surface, pixels
	uint32_t* pixels;     // a 32-bit surface
	int32_t stride;       // bytes per row
	uint32_t colour;      // the line colour, ARGB
	uint32_t clearColour; // the fill colour of a clear; set but never used (the rain screen clears its surface itself)
} LineRenderer_t;

static void LineRenderer_Ctor(LineRenderer_t* lr)
{
	memset(lr, 0, sizeof *lr);
	lr->clearColour = 0xff000000u;
}

// clamp the rectangle to the surface and store it; *old receives the previous clip
static void LineRenderer_SetClip(LineRenderer_t* lr, Rect_t* old, int l, int t, int r, int b)
{
	Rect_t prev = lr->clip;
	if(l < 0)
		l = 0;
	if(r >= lr->w)
		r = lr->w - 1;
	if(t < 0)
		t = 0;
	if(b >= lr->h)
		b = lr->h - 1;
	lr->clip.l = l;
	lr->clip.t = t;
	lr->clip.r = r;
	lr->clip.b = b;
	*old = prev;
}

// point the renderer at a surface and clip to all of it; 0 without pixels or when the width exceeds the stride
static int LineRenderer_SetTarget(LineRenderer_t* lr, uint32_t* pixels, int w, int h, int stride)
{
	Rect_t old;
	if(!pixels || (uint32_t)w > (uint32_t)stride)
		return 0;
	lr->pixels = pixels;
	lr->h = h;
	lr->w = w;
	lr->stride = stride;
	LineRenderer_SetClip(lr, &old, 0, 0, w - 1, h - 1);
	return 1;
}

static void LineRenderer_SetColour(LineRenderer_t* lr, uint32_t* old, uint32_t c) // *old receives the previous colour
{
	*old = lr->colour;
	lr->colour = c;
}

static void LineRenderer_SetClearColour(LineRenderer_t* lr, uint32_t* old, uint32_t c)
{
	*old = lr->clearColour;
	lr->clearColour = c;
}

static uint32_t* LineRenderer_Row(LineRenderer_t* lr, int y) // pixels + stride * y bytes
{
	return (uint32_t*)((uint8_t*)lr->pixels + lr->stride * y);
}

/* (The original's renderer also has a clear - fill the clip rectangle
 * with the clear colour - that nothing calls; the rain screen clears its
 * surface with Bmp_Clear before rendering.) */

/* draw a line in `colour` from (x0, y0) to (x1, y1): a DDA along the
 * major axis with the minor coordinate in 20.12 fixed point, one pixel
 * per column (wide) or per row (steep).  The clipping is the original's,
 * including its asymmetries: the left and top limits are assumed to be 0
 * where it extrapolates the minor coordinate to the clip edge, the last
 * row of the clip is never drawn by a wide line, and a steep line stops at
 * x < 0 rather than at the clip's left edge. */
static void LineRenderer_Line(LineRenderer_t* lr, int x0, int y0, int x1, int y1)
{
	const Rect_t* c = &lr->clip;
	uint32_t dx, dy;
	int32_t fix, step;
	int x, y, end;
	uint32_t* p;

	if((x0 < c->l && x1 < c->l) || (x0 > c->r && x1 > c->r) || (y0 < c->t && y1 < c->t) ||
		(y0 > c->b && y1 > c->b))
		return;
	if(y0 > y1) // make y increase
	{
		int t = x0;
		x0 = x1;
		x1 = t;
		t = y0;
		y0 = y1;
		y1 = t;
	}
	dx = (uint32_t)(x0 <= x1 ? x1 - x0 : x0 - x1) + 1;
	dy = (uint32_t)(y1 - y0) + 1;

	if(dx > dy)
	{ // ---- wide: one pixel per column, y as 20.12 ----
		step = (int32_t)((dy << 12) / dx);
		fix = y0 << 12;
		if(x0 <= x1)
		{ // rightwards
			if(x0 < c->l)
			{
				fix -= step * x0; // y at x = 0
				x = c->l;
			}
			else
				x = x0;
			end = (x1 > c->r ? c->r : x1) + 1;
			while((fix >> 12) < c->t)
			{
				fix += step;
				x++;
			}
			y = fix >> 12;
			if(y >= c->b)
				return;
			p = LineRenderer_Row(lr, y) + x;
			for(; x < end; x++, p++)
			{
				int ny;
				fix += step;
				*p = lr->colour;
				ny = fix >> 12;
				if(ny != y)
				{
					p = (uint32_t*)((uint8_t*)p + lr->stride);
					if(ny > c->b)
						return;
				}
				y = ny;
			}
		}
		else
		{ // leftwards
			if(x0 > c->r)
			{
				fix += (x0 - c->r) * step;
				x = c->r;
			}
			else
				x = x0;
			end = (x1 < c->l ? c->l : x1) - 1;
			while((fix >> 12) < c->t)
			{
				fix += step;
				x--;
			}
			y = fix >> 12;
			if(y >= c->b)
				return;
			p = LineRenderer_Row(lr, y) + x;
			for(; x > end; x--, p--)
			{
				int ny;
				fix += step;
				*p = lr->colour;
				ny = fix >> 12;
				if(ny != y)
				{
					p = (uint32_t*)((uint8_t*)p + lr->stride);
					if(ny > c->b)
						return;
				}
				y = ny;
			}
		}
	}
	else
	{ // ---- steep: one pixel per row, x as 20.12 ----
		int32_t adj;
		step = (int32_t)((dx << 12) / dy);
		fix = x0 << 12;
		if(y0 < c->t)
		{
			adj = -(step * y0); // x at y = 0, relative
			y = c->t;
		}
		else
		{
			adj = 0;
			y = y0;
		}
		end = (y1 > c->b ? c->b : y1) + 1;
		if(x0 <= x1)
		{ // rightwards
			int limit = c->r + 1;
			fix += adj;
			while(fix < c->l) // the original compares the 20.12 x with the plain edge; reproduced
			{
				fix += step;
				y++;
			}
			x = fix >> 12;
			if(x >= limit)
				return;
			p = LineRenderer_Row(lr, y) + x;
			for(; y < end; y++)
			{
				int nx;
				fix += step;
				*p = lr->colour;
				nx = fix >> 12;
				if(nx != x)
				{
					p++;
					if(nx >= limit)
						return;
				}
				x = nx;
				p = (uint32_t*)((uint8_t*)p + lr->stride);
			}
		}
		else
		{ // leftwards
			fix -= adj;
			step = -step;
			while((fix >> 12) > c->r)
			{
				fix += step;
				y++;
			}
			x = fix >> 12;
			if(x <= -1)
				return;
			p = LineRenderer_Row(lr, y) + x;
			for(; y < end; y++)
			{
				int nx;
				*p = lr->colour;
				fix += step;
				nx = fix >> 12;
				if(nx != x)
				{
					p--;
					if(nx <= -1)
						return;
				}
				x = nx;
				p = (uint32_t*)((uint8_t*)p + lr->stride);
			}
		}
	}
}

//  --- the drop list and generator ----------------------------------

typedef struct Drop // 0x1C bytes
{
	int32_t tag;     // always 1
	int32_t tail[3]; // the upper end
	int32_t head[3]; // the lower end: the point that was spawned
} Drop_t;

typedef struct DropNode // 12 bytes, doubly linked
{
	struct DropNode* prev;
	struct DropNode* next;
	Drop_t* drop; // owned
} DropNode_t;

struct RainGen // 0xA8 bytes
{
	DropNode_t *first, *last; // the drops, oldest first
	int32_t count;            // drops alive
	LineRenderer_t* lines;    // created by the first render, owned
	uint32_t lastTime;        // the time the stages have been run up to, ms
	RainParams_t p;           // the current parameters
	int32_t step[3];          // dir * (fall >> 8): the motion per stage
	int32_t streak[3];        // dir * (length >> 8): head to tail
	RainView_t v;             // the current view
	int32_t sinA, cosA;       // the view angles, x 256
	int32_t sinB, cosB;
	int32_t sinC, cosC;
};

/* a generator without drops and with placeholder parameters (a +-100 box,
 * straight down, 60 per stage, 350 long, white, 100 drops per 20 ms stage,
 * distance 10, no rotation); the screen replaces all of them before the
 * first update.  The caller frees it with RainGen_Delete. */
RainGen_t* RainGen_New(void)
{
	RainGen_t* g = (RainGen_t*)BGI_Alloc(sizeof *g);
	memset(g, 0, sizeof *g);
	g->p.xMin = -100; // a consistent +-100 box; the screen overwrites all of it
	g->p.yTop = 100;
	g->p.zMin = -100;
	g->p.xMax = 100;
	g->p.yBottom = -100;
	g->p.zMax = 100;
	g->p.dir[1] = -1;
	g->p.fall = 60 << 8;
	g->p.length = 350 << 8;
	g->p.colour = 0xffffffffu;
	g->p.perStage = 100;
	g->p.stageMs = 20;
	g->p.hiResTimer = 1;
	g->step[1] = -60;
	g->streak[1] = -350;
	g->v.dist = 10;
	g->sinA = g->sinB = g->sinC = (int32_t)(sin(0.0) * 256.0);
	g->cosA = g->cosB = g->cosC = (int32_t)(cos(0.0) * 256.0);
	return g;
}

static void RainGen_Clear(RainGen_t* g) // delete every drop
{
	DropNode_t* n = g->first;
	while(n)
	{
		DropNode_t* next = n->next;
		BGI_Free(n->drop);
		BGI_Free(n);
		n = next;
	}
	g->first = g->last = NULL;
	g->count = 0;
}

void RainGen_Delete(RainGen_t* g)
{
	RainGen_Clear(g);
	if(g->lines)
		BGI_Free(g->lines);
	BGI_Free(g);
}

/* install a parameter block (*old receives the previous one) and derive
 * the motion per stage and the streak vector from the direction, `fall`
 * and `length` (integer parts only).  Takes effect at the next stage;
 * the drops already falling keep their positions. */
void RainGen_SetParams(RainGen_t* g, RainParams_t* old, const RainParams_t* p)
{
	int32_t fall, len, k;
	*old = g->p;
	g->p = *p;
	fall = (int32_t)((uint32_t)g->p.fall >> 8);
	len = (int32_t)((uint32_t)g->p.length >> 8);
	for(k = 0; k < 3; k++)
	{
		g->step[k] = g->p.dir[k] * fall;
		g->streak[k] = g->p.dir[k] * len;
	}
}

static void RainGen_Angle(int32_t tenths, int32_t* s, int32_t* c) // 0.1 degree units -> sine and cosine x 256
{
	double rad = (double)tenths * 0.017453292519444445 * 0.1;
	*s = (int32_t)(sin(rad) * 256.0);
	*c = (int32_t)(cos(rad) * 256.0);
}

// install a view (*old receives the previous one) and precompute the sines and cosines of its angles
void RainGen_SetView(RainGen_t* g, RainView_t* old, const RainView_t* v)
{
	*old = g->v;
	g->v = *v;
	RainGen_Angle(g->v.ang[0], &g->sinA, &g->cosA);
	RainGen_Angle(g->v.ang[1], &g->sinB, &g->cosB);
	RainGen_Angle(g->v.ang[2], &g->sinC, &g->cosC);
}

// the clock: the original picks timeGetTime or GetTickCount by `hiResTimer`; both are GetTicks here
static uint32_t RainGen_Now(const RainGen_t* g)
{
	(void)g;
	return GetTicks();
}

/* "C0 42": drop every drop and put the clock `prerollMs` into the past,
 * so that the next update runs that many milliseconds of stages at once.
 * A preroll of 500 ms or more only resynchronises (see RainGen_Update). */
void RainGen_Start(RainGen_t* g, uint32_t prerollMs)
{
	g->lastTime = RainGen_Now(g) - prerollMs;
	RainGen_Clear(g);
}

// move a drop one stage; 1 when its tail reached the floor
static int RainGen_MoveDrop(const RainGen_t* g, Drop_t* d)
{
	int k;
	for(k = 0; k < 3; k++)
	{
		d->tail[k] += g->step[k];
		d->head[k] += g->step[k];
	}
	return d->tail[1] <= g->p.yBottom;
}

// remove a node from the list and free it with its drop
static void RainGen_Unlink(RainGen_t* g, DropNode_t* n)
{
	if(n->prev)
		n->prev->next = n->next;
	else
		g->first = n->next;
	if(n->next)
		n->next->prev = n->prev;
	else
		g->last = n->prev;
	g->count--;
	BGI_Free(n->drop);
	BGI_Free(n);
}

// move every drop and cull the ones that reached the floor
static void RainGen_Advance(RainGen_t* g)
{
	DropNode_t* n = g->first;
	while(n)
	{
		DropNode_t* next = n->next;
		if(RainGen_MoveDrop(g, n->drop))
			RainGen_Unlink(g, n);
		n = next;
	}
}

/* spawn `perStage` drops on the yTop plane at random (x, z) inside the
 * box, staggered down by up to 99 units; the tail trails the head by
 * `streak`.  The original divides by zero for a zero-width x or z range;
 * here such a range puts every drop at its minimum. */
static void RainGen_Spawn(RainGen_t* g)
{
	uint32_t xRange = (uint32_t)(g->p.xMax - g->p.xMin);
	uint32_t zRange = (uint32_t)(g->p.zMax - g->p.zMin);
	int i, k;
	for(i = 0; i < g->p.perStage; i++)
	{
		Drop_t* d = (Drop_t*)BGI_Alloc(sizeof *d);
		DropNode_t* n = (DropNode_t*)BGI_Alloc(sizeof *n);
		d->tag = 1;
		d->head[0] = g->p.xMin + (int32_t)(xRange ? (uint32_t)BGI_Rand() % xRange : 0);
		d->head[1] = g->p.yTop - BGI_Rand() % 100;
		d->head[2] = g->p.zMin + (int32_t)(zRange ? (uint32_t)BGI_Rand() % zRange : 0);
		for(k = 0; k < 3; k++)
			d->tail[k] = d->head[k] - g->streak[k];
		n->drop = d;
		n->next = NULL;
		n->prev = g->last;
		if(g->last)
			g->last->next = n;
		else
			g->first = n;
		g->last = n;
		g->count++;
	}
}

// one stage: move and cull, then spawn
static void RainGen_Stage(RainGen_t* g)
{
	RainGen_Advance(g);
	RainGen_Spawn(g);
}

/* once per scheduler pass: turn the time elapsed since the last update
 * into stages.  500 ms or more (a stall, or a preroll that long) only
 * resynchronises the clock; less than a stage leaves everything as is. */
void RainGen_Update(RainGen_t* g)
{
	uint32_t now = RainGen_Now(g), elapsed = now - g->lastTime;
	if(elapsed >= 500)
	{
		g->lastTime = RainGen_Now(g);
		return;
	}
	if(elapsed < g->p.stageMs)
		return;
	do
	{
		RainGen_Stage(g);
		elapsed -= g->p.stageMs;
	} while(elapsed >= g->p.stageMs);
	g->lastTime = now - elapsed;
}

/* world to view space: translate by the camera, rotate about z by C
 * (using the already rotated x in the second line, as the original does),
 * then about x by B, then about y by A; the sines and cosines are x 256. */
static void RainGen_ToView(const RainGen_t* g, const int32_t in[3], int32_t out[3])
{
	int32_t x = in[0] - g->v.cam[0];
	int32_t y = in[1] - g->v.cam[1];
	int32_t z = in[2] - g->v.cam[2];
	int32_t x1, y1, y2, z2;
	x1 = (g->cosC * x - g->sinC * y) >> 8;
	y1 = (g->sinC * x1 + g->cosC * y) >> 8;
	y2 = (y1 * g->cosB - z * g->sinB) >> 8;
	z2 = (y1 * g->sinB + z * g->cosB) >> 8;
	out[0] = (z2 * g->sinA + x1 * g->cosA) >> 8;
	out[1] = y2;
	out[2] = (z2 * g->cosA - x1 * g->sinA) >> 8;
}

static int RainGen_InFront(const RainGen_t* g, const int32_t v[3]) // beyond the near plane (z > dist)
{
	return v[2] > g->v.dist;
}

/* project both ends of a drop and draw its streak with the surface centre
 * as the vanishing point.  The alpha byte of the colour is attenuated by
 * the tail's depth (alpha * dist / (z / 8), never above the configured
 * alpha).  0 when either end is behind the near plane, 1 when drawn. */
static int RainGen_DrawDrop(RainGen_t* g, const Drop_t* d, LineRenderer_t* lr)
{
	int32_t a[3], b[3], d0 = g->v.dist, depth8;
	int hw = (int)((uint32_t)lr->w >> 1), hh = (int)((uint32_t)lr->h >> 1);
	uint32_t colour, alpha, old;
	int32_t lit;

	RainGen_ToView(g, d->tail, a);
	if(!RainGen_InFront(g, a))
		return 0;
	RainGen_ToView(g, d->head, b);
	if(!RainGen_InFront(g, b))
		return 0;
	a[0] = d0 * a[0] / a[2];
	a[1] = d0 * a[1] / a[2];
	b[0] = d0 * b[0] / b[2];
	b[1] = d0 * b[1] / b[2];

	colour = g->p.colour;
	alpha = colour >> 24;
	depth8 = a[2] / 8;
	lit = depth8 ? (int32_t)alpha * d0 / depth8 : (int32_t)alpha; // a tail nearer than 8 keeps the full alpha (the original divides by zero there)
	if(lit > (int32_t)alpha)
		lit = (int32_t)alpha;
	colour = (colour & 0x00ffffffu) | ((uint32_t)lit << 24);
	LineRenderer_SetColour(lr, &old, colour);
	LineRenderer_Line(lr, a[0] + hw, hh - a[1], b[0] + hw, hh - b[1]);
	return 1;
}

// the renderer (created on first use), pointed at the surface with the current colours; `bits` is unused
static LineRenderer_t* RainGen_PrepareRenderer(RainGen_t* g, uint8_t* pixels, int w, int h, int pitch, int bits)
{
	uint32_t old;
	(void)bits;
	if(!g->lines)
	{
		g->lines = (LineRenderer_t*)BGI_Alloc(sizeof *g->lines);
		LineRenderer_Ctor(g->lines);
	}
	LineRenderer_SetTarget(g->lines, (uint32_t*)pixels, w, h, pitch);
	LineRenderer_SetColour(g->lines, &old, g->p.colour);
	LineRenderer_SetClearColour(g->lines, &old, g->p.clearColour);
	return g->lines;
}

/* draw every drop into the 32-bit surface `pixels` of w x h pixels and
 * `pitch` bytes per row (`bits` is unused); the surface is not cleared.
 * The result carries no information (the original returns whatever its
 * last call left behind); callers ignore it. */
int RainGen_Render(RainGen_t* g, uint8_t* pixels, int w, int h, int pitch, int bits)
{
	LineRenderer_t* lr = RainGen_PrepareRenderer(g, pixels, w, h, pitch, bits);
	DropNode_t* n;
	for(n = g->first; n; n = n->next)
		RainGen_DrawDrop(g, n->drop, lr);
	return 1;
}

//  --- the per-pass driver -----------------------------------------------

static uint32_t gEffectFrameMs = 50; // milliseconds between effect frames (20 fps by default)
static uint32_t gEffectNext;         // when the next frame is due

// once per scheduler pass: run the stages of every rain screen
void Rain_UpdateAll(void)
{
	Gfx_UpdateRains(gGfx);
}

/* once per scheduler pass: when an effect frame is due, redraw the rain
 * screens and request a dirty present; the next frame is the first grid
 * point after now.  Nothing happens while the effects are switched off. */
void Rain_FrameTick(void)
{
	uint32_t now;
	if(!Fx_GetEffectsEnabled())
		return;
	now = GetTicks();
	if(gEffectNext > now)
		return;
	if(Gfx_RedrawRains(gGfx))
		Present_RequestDirty();
	gEffectNext += ((now - gEffectNext) / gEffectFrameMs + 1) * gEffectFrameMs;
}

/* "C0 4F": switch the effects (the rain screens' visibility and redraw)
 * on or off and set their frame rate, 1 .. 1000 frames per second; the
 * next frame is due at once and the whole picture is presented.  1 ok, 0
 * for a rate outside that range (nothing changes). */
int Fx_SetEffectFps(int enable, uint32_t fps)
{
	if(fps < 1 || fps > 1000)
		return 0;
	gEffectFrameMs = 1000 / fps;
	gEffectNext = 0;
	Fx_SetEffectsEnabled(enable);
	Present_RequestFull();
	return 1;
}
