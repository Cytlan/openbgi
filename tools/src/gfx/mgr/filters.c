/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * filters.c - the graphics manager's filter and effector operations: the
 *             "90 6x" (filter) and "91 6x" (effector) instructions
 *             (inc/bgi/gfx/gfxmgr.h)
 *
 * Both classes are full-screen post-processes on everything drawn below
 * their priority: a filter is a colour operation, an effector a geometric
 * one.  Eight slots each, addressed by H_FILTER | index and H_EFFECTOR |
 * index.  Because an effector distorts what lies below it, any change to
 * a visible one repaints the whole screen.
 */
#include "mgr_internal.h"

// ---- filters -----------------------------------------------------------------------------------

void Gfx_DeleteAllFilters(Gfx_t* g)
{
	Mgr_DeleteAll(g->filters, GFX_FILTERS, &g->filterCount, &g->filterNextId);
}

// "90 60": a new filter in the first free slot; the handle, 0 when all 8 are in use
uint32_t Gfx_FilterCreate(Gfx_t* g)
{
	Filter_t* f;
	uint32_t i;

	if(g->filterCount >= GFX_FILTERS)
		return 0;
	i = Mgr_FreeSlot(g->filters);
	f = (Filter_t*)BGI_Calloc(sizeof(Filter_t));
	Filter_Ctor(f, g->filterNextId++);
	g->filters[i] = &f->obj;
	Compositor_Add(g->comp, &f->obj);
	g->filterCount++;
	return H_FILTER | i;
}

// "90 61": 1 ok, 0 unknown handle
int Gfx_FilterDelete(Gfx_t* g, uint32_t h)
{
	return Mgr_DeleteFromTable(g, g->filters, Gfx_FindFilter(g, h), h, &g->filterCount, 0);
}

/* "90 65" / "90 66": kind, colour (0x00RRGGBB), level and priority are
 * set unconditionally; then the optional grayscale bitmap that modulates
 * the tint per pixel (bmp -1 = none, `param` its transition curve).  0 ok,
 * 1 no such bitmap, 2 not grayscale, 3 not screen sized, 0xFF unknown
 * handle.  No repaint of the old state: a filter covers the whole screen
 * anyway. */
int Gfx_FilterSet(Gfx_t* g, uint32_t h, int kind, uint32_t colour, int bmp, int param, int level, int prio)
{
	DispObj_t* o = Gfx_FindFilter(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	Filter_Set((Filter_t*)o, kind, colour, level, prio);
	r = (uint32_t)Filter_SetBitmap((Filter_t*)o, bmp, param);
	if(r == 0)
	{
		InvalidateIfVisible(o);
		Compositor_Resort(g->comp, o);
		return 0;
	}
	if(r >= 0x80000001u && r <= 0x80000003u)
		return (int)(r - 0x80000000u);
	return (int)h;
}

// "90 64": show / hide; 1 ok, 0 unknown handle
int Gfx_FilterShow(Gfx_t* g, uint32_t h, int on)
{
	return Mgr_ShowBracket(Gfx_FindFilter(g, h), on);
}

// ---- effectors ---------------------------------------------------------------------------------

void Gfx_DeleteAllEffectors(Gfx_t* g)
{
	Mgr_DeleteAll(g->effectors, GFX_EFFECTORS, &g->effectorCount, &g->effectorNextId);
}

// "91 60": a new effector in the first free slot; the handle, 0 when all 8 are in use
uint32_t Gfx_EffectorCreate(Gfx_t* g)
{
	Effector_t* e;
	uint32_t i;

	if(g->effectorCount >= GFX_EFFECTORS)
		return 0;
	i = Mgr_FreeSlot(g->effectors);
	e = (Effector_t*)BGI_Calloc(sizeof(Effector_t));
	Effector_Ctor(e, g->effectorNextId++);
	g->effectors[i] = &e->obj;
	Compositor_Add(g->comp, &e->obj);
	g->effectorCount++;
	return H_EFFECTOR | i;
}

/* "91 61": an effector distorts everything below it, so deleting a
 * visible one repaints the whole screen.  1 ok, 0 unknown handle. */
int Gfx_EffectorDelete(Gfx_t* g, uint32_t h)
{
	return Mgr_DeleteFromTable(g, g->effectors, Gfx_FindEffector(g, h), h, &g->effectorCount, 1);
}

/* the success tail of the effector setters: full repaint when visible,
 * then re-sort (the priority may have changed); 0 */
static int EffectorSetDone(Gfx_t* g, DispObj_t* o)
{
	if(IsVisible(o))
		Compositor_InvalidateAll(g->comp);
	Compositor_Resort(g->comp, o);
	return 0;
}

/* "91 65": displacement through one or two screen-sized vector maps (vec2
 * -1 = none).  0 ok, 1 / 2 no such vec1 / vec2, 3 / 4 vec1 / vec2 is not
 * a screen-sized vector map, 0xFF unknown handle. */
int Gfx_EffectorSetVector(Gfx_t* g, uint32_t h, int vec1, int vec2, int level, int amount, int prio)
{
	DispObj_t* o = Gfx_FindEffector(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	r = (uint32_t)Effector_SetVector((Effector_t*)o, vec1, vec2, level, amount, prio);
	if(r == 0)
		return EffectorSetDone(g, o);
	if(r >= 0x80000001u && r <= 0x80000004u)
		return (int)(r - 0x80000000u);
	return (int)h;
}

// "91 66": the box blur; 0 ok, 5 the type is not 0 or 1, 0xFF unknown handle
int Gfx_EffectorSetGradient(Gfx_t* g, uint32_t h, int type, int level, int prio)
{
	DispObj_t* o = Gfx_FindEffector(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	r = (uint32_t)Effector_SetGradient((Effector_t*)o, type, level, prio);
	if(r == 0)
		return EffectorSetDone(g, o);
	if(r == 0x80000005u)
		return 5;
	return (int)h;
}

/* "91 67": ripples through a screen-sized vector + distance map.  0 ok,
 * 1 no such map, 3 not a screen-sized vecdist map, 6 zero rings, 7 unknown
 * ripple definition, 8 the definition has fewer rings than asked for, 0xFF
 * unknown handle. */
int Gfx_EffectorSetRipple(Gfx_t* g, uint32_t h, int map, int rings, int rippleNo, int level, int prio)
{
	DispObj_t* o = Gfx_FindEffector(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	r = (uint32_t)Effector_SetRipple((Effector_t*)o, map, rings, rippleNo, level, prio);
	switch(r)
	{
		case 0: return EffectorSetDone(g, o);
		case 0x80000001u: return 1;
		case 0x80000003u: return 3;
		case 0x80000006u: return 6;
		case 0x80000007u: return 7;
		case 0x80000008u: return 8;
		default: return (int)h;
	}
}

/* "91 68": rotation by `angle` and zoom by (sx, sy) about (cx, cy), all
 * 16.16.  0 ok, 9 a zero scale, 0xFF unknown handle. */
int Gfx_EffectorSetZoom(Gfx_t* g, uint32_t h, int32_t cx, int32_t cy, int32_t angle, int32_t sx, int32_t sy,
	int smooth, int level, int prio)
{
	DispObj_t* o = Gfx_FindEffector(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	r = (uint32_t)Effector_SetZoom((Effector_t*)o, cx, cy, angle, sx, sy, smooth, level, prio);
	if(r == 0)
		return EffectorSetDone(g, o);
	if(r == 0x80000009u)
		return 9;
	return (int)h;
}

// "91 64": show / hide; a change of visibility repaints the whole screen.  1 ok, 0 unknown handle
int Gfx_EffectorShow(Gfx_t* g, uint32_t h, int on)
{
	DispObj_t* o = Gfx_FindEffector(g, h);
	int was, now;

	if(!o)
		return 0;
	was = IsVisible(o);
	o->vt->setVisible(o, on);
	now = IsVisible(o);
	if(was != now)
		Compositor_InvalidateAll(g->comp);
	return 1;
}
