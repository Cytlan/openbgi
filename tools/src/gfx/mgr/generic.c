/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * generic.c - the graphics manager's operations on any display object by
 *             handle (0 = the background): show, position, level, priority,
 *             the parameters, the queries (inc/bgi/gfx/gfxmgr.h)
 *
 * These are the "90 3x" / "91 3x" object instructions.  Each wrapper
 * resolves the handle with Gfx_FindObject and brackets the object's own
 * method with the repaints the change needs.  Unless noted, the result is
 * 1 ok, 0 unknown handle; the wrappers that map an object's result codes
 * return 0 ok, 0xFF unknown handle and small numbers for the errors.
 */
#include "mgr_internal.h"

// "90 30": show / hide; 1 ok, 0 unknown handle
int Gfx_ObjShow(Gfx_t* g, uint32_t h, int on)
{
	return Mgr_ShowBracket(Gfx_FindObject(g, h), on);
}

// "90 31": the enabled flag; 1 ok, 0 unknown handle
int Gfx_ObjSetEnabled(Gfx_t* g, uint32_t h, int v)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	int was, now;

	if(!o)
		return 0;
	// the enabled flag is part of the effective visibility, so the same
	// "repaint only when the visibility changed" rule as objShow applies
	was = IsVisible(o);
	DispObj_SetEnabled(o, v);
	now = IsVisible(o);
	if(was != now)
		Invalidate(o);
	return 1;
}

// "90 32": the level (transparency or effect strength); 1 ok, 0 unknown handle
int Gfx_ObjSetLevel(Gfx_t* g, uint32_t h, int level)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	int was, now;

	if(!o)
		return 0;
	was = IsVisible(o);
	o->vt->setLevel(o, level);
	now = IsVisible(o);
	// a level change alters the pixels but not the footprint: one repaint
	// if the object was or is visible
	if(was || now)
		Invalidate(o);
	return 1;
}

// "90 33": the position (pixels; what it means is up to the object); 1 ok, 0 unknown handle
int Gfx_ObjSetPos(Gfx_t* g, uint32_t h, int x, int y)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	int was;

	if(!o)
		return 0;
	// repaint the old footprint, move, repaint the new one; both tests
	// use the visibility *before* the move
	was = IsVisible(o);
	if(was)
		Invalidate(o);
	o->vt->setPos(o, x, y);
	if(was)
		Invalidate(o);
	return 1;
}

// the object's position; 1 ok, 0 unknown handle
int Gfx_ObjGetPos(Gfx_t* g, int32_t out[2], uint32_t h)
{
	DispObj_t* o = Gfx_FindObject(g, h);

	if(o)
		o->vt->getPos(o, out);
	return o != NULL;
}

/* "91 33": the fixed-point (16.16) 3D position; it also feeds the depth
 * sort, hence the re-sort.  1 ok, 0 unknown handle. */
int Gfx_ObjSetFixedPos(Gfx_t* g, uint32_t h, int32_t fx, int32_t fy, int32_t fz)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	int was;

	if(!o)
		return 0;
	was = IsVisible(o);
	if(was)
		Invalidate(o);
	o->vt->setFixedPos(o, fx, fy, fz);
	if(was)
		Invalidate(o);
	Gfx_Resort(g, o);
	return 1;
}

/* offset `which` of the two 16.16 offsets added to the fixed position:
 * 1 for "91 37" (1.69 build 472 on), 2 for "91 36" (1.494 on; "92 37" in
 * build 472).  The object's setFixedPos is re-applied so that a projected
 * sprite recomputes its placement.  1 ok, 0 unknown handle. */
int Gfx_ObjSetFixedOffset(Gfx_t* g, uint32_t h, int which, int32_t x, int32_t y, int32_t z)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	int32_t fp[4];
	int was;

	if(!o)
		return 0;
	was = IsVisible(o);
	if(was)
		Invalidate(o);
	DispObj_SetFixedOffset(o, which, x, y, z);
	o->vt->getFixedPos(o, fp);
	o->vt->setFixedPos(o, fp[0], fp[1], fp[2]);
	if(was)
		Invalidate(o);
	Gfx_Resort(g, o);
	return 1;
}

// "90 3A" of 1.69 build 472 on: the draw priority; 0 ok, 0xff unknown handle
int Gfx_ObjSetPriority(Gfx_t* g, uint32_t h, int prio)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	int was;

	if(!o)
		return 0xff;
	was = IsVisible(o);
	if(was)
		Invalidate(o);
	DispObj_SetPriority(o, prio);
	if(was)
		Invalidate(o);
	Gfx_Resort(g, o);
	return 0;
}

/* "91 38" of 1.69 build 472 on: read parameter `no` into *out.  0 ok, 5
 * the object does not support the parameter (getParam 0xFFFF0001), 0xfe
 * any other failure, 0xff unknown handle. */
int Gfx_ObjGetParam(Gfx_t* g, uint32_t h, int no, int32_t* out)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	uint32_t r;
	if(!o)
		return 0xff;
	r = (uint32_t)o->vt->getParam(o, no, out);
	if(r == 0)
		return 0;
	return r == 0xffff0001u ? 5 : 0xfe;
}

// "90 39" of 1.599 on: the opacity (0 makes the object invisible); 1 ok, 0 unknown handle
int Gfx_ObjSetOpacity(Gfx_t* g, uint32_t h, int v)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	int was, now;

	if(!o)
		return 0;
	was = IsVisible(o);
	DispObj_SetOpacity(o, v);
	now = IsVisible(o);
	if(was || now)
		Invalidate(o);
	return 1;
}

// "90 34": the fade (0x100 makes the object invisible); 1 ok, 0 unknown handle
int Gfx_ObjSetFade(Gfx_t* g, uint32_t h, int v)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	int was, now;

	if(!o)
		return 0;
	was = IsVisible(o);
	DispObj_SetFade(o, v);
	now = IsVisible(o);
	if(was || now)
		Invalidate(o);
	return 1;
}

// "90 35": the animation progress as an integer step; 1 ok, 0 unknown handle
int Gfx_ObjSetProgress(Gfx_t* g, uint32_t h, int v)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	int was;

	if(!o)
		return 0;
	was = IsVisible(o);
	if(was)
		Invalidate(o);
	DispObj_SetProgressInt(o, v); // setProgress(0, v): an integer step
	if(was)
		Invalidate(o);
	return 1;
}

// "90 37": the draw offset added to the position; 1 ok, 0 unknown handle
int Gfx_ObjSetOffset(Gfx_t* g, uint32_t h, int dx, int dy)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	int was;

	if(!o)
		return 0;
	was = IsVisible(o);
	if(was)
		Invalidate(o);
	o->vt->setOffset(o, dx, dy);
	if(was)
		Invalidate(o);
	return 1;
}

// "90 36" of 1.494 on: the second draw offset, passed on to the children; 1 ok, 0 unknown handle
int Gfx_ObjSetOffset2(Gfx_t* g, uint32_t h, int dx, int dy)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	int was;

	if(!o)
		return 0;
	was = IsVisible(o);
	if(was)
		Invalidate(o);
	DispObj_SetOffset2(o, dx, dy);
	if(was)
		Invalidate(o);
	return 1;
}

/* "90 38": set parameter `no` to (a, b).  The object's setParam reports
 * 0xFFFF0001 for a parameter it does not support (-> 5) and 0xFFFF0002
 * for an out-of-range value (-> 0xFE; any other non-zero result maps
 * there too).  0 ok, 0xFF unknown handle. */
int Gfx_ObjSetParam(Gfx_t* g, uint32_t h, int no, int a, int b)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	InvalidateIfVisible(o);
	r = (uint32_t)o->vt->setParam(o, no, a, b);
	if(r != 0)
		return r == 0xffff0001u ? 5 : 0xfe;
	InvalidateIfVisible(o);
	return 0;
}

/* "90 3C": the hit-test mask.  bmp >= 0: build it from that bitmap's
 * alpha; -1: remove it (the whole rectangle hits); -2: a mask that never
 * hits (built from a cleared screen-mode bitmap of the object's size).
 * 0 ok, 1 the object has no mask (class order >= 8: virtual objects,
 * groups, knobs), 2 the bitmap does not exist, 0xFF unknown handle. */
int Gfx_ObjSetHitMask(Gfx_t* g, uint32_t h, int bmp)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	Bmp_t info;
	Rect_t lr;

	if(!o)
		return NO_OBJECT;
	if((uint32_t)o->classOrder >= 8u)
		return 1;
	if(bmp == -2)
	{
		// an all-transparent picture of the object's local size
		o->vt->localRect(o, &lr);
		Bmp_AllocScreen(&info, lr.r + 1, lr.b + 1, 1); // (the original passes 3; only non-zero matters)
		Bmp_Clear(&info, NULL);
		DispObj_BuildHitMask(o, &info);
		BGI_Free(info.pixels);
		return 0;
	}
	if(bmp == -1)
	{
		DispObj_BuildHitMask(o, NULL);
		return 0;
	}
	if(!BmpMgr_GetInfo(gGfxBmpMgr, &info, bmp))
		return 2;
	DispObj_BuildHitMask(o, &info);
	return 0;
}

// "90 3D": *out = whether (x, y), relative to the object, hits it; 1 ok, 0 unknown handle
int Gfx_ObjHitTest(Gfx_t* g, int* out, uint32_t h, int x, int y)
{
	DispObj_t* o = Gfx_FindObject(g, h);

	if(o)
		*out = o->vt->hitTest(o, x, y);
	return o != NULL;
}

// whether the object is attached to an owner (a group or another object); 0 for an unknown handle too
int Gfx_ObjHasOwner(Gfx_t* g, uint32_t h)
{
	DispObj_t* o = Gfx_FindObject(g, h);

	if(!o)
		return 0;
	return o->owner != NULL; // the owner field
}

/* "90 3F": ask the object to build its picture cache (the buildCache
 * method of its class).  0 ok, 3 the class does not support it (buildCache
 * 0x80000001: the default, which no class of this build overrides), 4 an
 * unusable picture size (0x80000002), 0xFF unknown handle or any other
 * result. */
int Gfx_ObjBuildCache(Gfx_t* g, uint32_t h)
{
	DispObj_t* o = Gfx_FindObject(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	r = (uint32_t)o->vt->buildCache(o);
	switch(r)
	{
		case 0: return 0;
		case 0x80000001u: return 3;
		case 0x80000002u: return 4;
		default: return NO_OBJECT;
	}
}
