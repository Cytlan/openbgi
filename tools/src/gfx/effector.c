/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * effector.c - Effector, the full-screen geometric post-process (class
 * order 6).  Interface in inc/bgi/gfx/objects.h.
 *
 * An effector is a screen-sized object that snapshots everything painted
 * below it and writes back a distorted version, in one of four modes:
 * 0 a displacement through vector maps ("91 65"), 1 the "gradient", a
 * horizontal blur ("91 66"), 2 a ripple ("91 67"), 3 a rotation / zoom
 * about a point, animatable through the progress ("91 68").  The level is
 * the strength of the distortion.  Every effector is on the global list
 * `gEffectors`: while any of them is visible the compositor paints whole
 * frames on one thread (Effector_AnyVisible, compositor.c), so the
 * destination handed to draw() is always the complete back buffer.
 */
#include <string.h>

#include "bgi/common.h"
#include "bgi/gfx/objects.h"
#include "bgi/gfx/compositor.h"

EffNode_t* gEffectors; // every live effector, newest first

// the descriptor of bitmap `no`, but only while it still is the bitmap of generation `gen` the effector was set with
static int EffCurrent(Bmp_t* out, int no, int gen)
{
	return BmpMgr_GetInfo(gDispBmpMgr, out, no) && BmpMgr_Generation(gDispBmpMgr, no) == gen;
}

/* "91 60": DispObj(6, slotId), put on the list, with a screen-sized
 * snapshot buffer, in gradient mode with type 0, level 0 and priority 0 */
void Effector_Ctor(Effector_t* e, int slotId)
{
	EffNode_t* n;
	DispObj_Ctor(&e->obj, 6, slotId);
	e->obj.vt = &Effector_Vtbl;
	memset(&e->snap, 0, sizeof e->snap);
	e->mode = -1;
	e->ringTable = NULL;
	n = (EffNode_t*)BGI_Alloc(sizeof *n);
	n->obj = e;
	n->next = gEffectors;
	gEffectors = n;
	Effector_MatchScreenSize(e);
	Effector_SetGradient(e, 0, 0, 0);
}

// frees the snapshot and the ring table and takes the effector off the list
void Effector_Dtor(Effector_t* e)
{
	EffNode_t** pp;
	e->obj.vt = &Effector_Vtbl;
	Effector_FreeSnapshot(e);
	Effector_FreeTable(e);
	for(pp = &gEffectors; *pp; pp = &(*pp)->next)
	{
		if((*pp)->obj == e)
		{
			EffNode_t* n = *pp;
			*pp = n->next;
			BGI_Free(n);
			break;
		}
	}
	DispObj_Dtor(&e->obj);
}

static void Effector_Destroy(DispObj_t* o, int flags)
{
	Effector_Dtor((Effector_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

// 1 while some effector is visible: the compositor then gives up banding (Compositor_CanBand)
int Effector_AnyVisible(void)
{
	EffNode_t* n;
	for(n = gEffectors; n; n = n->next)
		if(n->obj->obj.vt->isVisible(&n->obj->obj))
			return 1;
	return 0;
}

/* Copy the back buffer into the snapshot, then write the distorted
 * snapshot back over it.  A vector or ripple map that was freed or
 * reloaded leaves the screen as it is (the snapshot has already been
 * taken). */
static void Effector_Draw(DispObj_t* o, Bmp_t* dst, const Rect_t* local, uint32_t minKey)
{
	Effector_t* e = (Effector_t*)o;
	int level = DispObj_EffectiveLevel(o);
	Bmp_t v1, v2, map;
	(void)local;
	(void)minKey;
	Bmp_BlitEffect(&e->snap, dst, 0x80, 0);
	switch(e->mode)
	{
		case 0:
			if(!EffCurrent(&v1, e->vec1, e->vec1Gen))
				return;
			if(e->vec2 != -1 && !EffCurrent(&v2, e->vec2, e->vec2Gen))
				return;
			Blit_Displace(dst, &e->snap, &v1, e->vec2 != -1 ? &v2 : NULL, level, e->amount);
			break;
		case 1:
			Blit_Gradient(dst, &e->snap, e->gradType, level);
			break;
		case 2:
			if(!EffCurrent(&map, e->rippleMap, e->rippleMapGen))
				return;
			Blit_Ripple(dst, &e->snap, &e->snap, &map, e->ringTable);
			break;
		case 3:
			// the snapshot point (curCx, curCy) lands on the screen centre
			Blit_XformCopy(dst, (e->snap.w & ~1) << 15, (e->snap.h & ~1) << 15, &e->snap,
				e->curCx, e->curCy, e->curAngle, e->curSx, e->curSy, 0, e->smooth);
			break;
		default:
			break;
	}
}

/* "91 65", mode 0: displacement through one or two screen-sized vector
 * maps (pixel mode 4), mixed by the level; `amount` is passed to
 * Blit_Displace (non-zero samples bilinearly); vec2 -1 = none.  0 ok,
 * 0x80000001 / 0x80000002 no such bitmap, 0x80000003 / 0x80000004 not a
 * vector map of the screen's size (vec1 / vec2). */
int Effector_SetVector(Effector_t* e, int vec1, int vec2, int level, int amount, int prio)
{
	Bmp_t back, info;
	Gfx_GetBackBmp(&back);
	if(!BmpMgr_GetInfo(gDispBmpMgr, &info, vec1))
		return (int)0x80000001;
	if(info.mode != 4 || info.w != back.w || info.h != back.h)
		return (int)0x80000003;
	if(vec2 != -1)
	{
		if(!BmpMgr_GetInfo(gDispBmpMgr, &info, vec2))
			return (int)0x80000002;
		if(info.mode != 4 || info.w != back.w || info.h != back.h)
			return (int)0x80000004;
	}
	Effector_FreeTable(e);
	e->mode = 0;
	e->vec1 = vec1;
	e->vec1Gen = BmpMgr_Generation(gDispBmpMgr, vec1);
	e->vec2 = vec2;
	e->vec2Gen = BmpMgr_Generation(gDispBmpMgr, vec2);
	e->amount = amount;
	e->obj.vt->setLevel(&e->obj, level);
	DispObj_SetPriority(&e->obj, (uint32_t)prio);
	return 0;
}

/* "91 66", mode 1: the "gradient", a horizontal blur of width 2 * level + 1;
 * type 0 pads with black, 1 repeats the edge.  0 ok, 0x80000005 unknown
 * type. */
int Effector_SetGradient(Effector_t* e, int type, int level, int prio)
{
	if((uint32_t)type > 1)
		return (int)0x80000005;
	Effector_FreeTable(e);
	e->mode = 1;
	e->gradType = type;
	e->obj.vt->setLevel(&e->obj, level);
	DispObj_SetPriority(&e->obj, (uint32_t)prio);
	return 0;
}

/* "91 67", mode 2: a ripple through a screen-sized vector + distance map
 * (pixel mode 6), driven by ripple definition `rippleNo` ("92 00") with
 * `rings` rings; the level is the amplitude.  0 ok, 0x80000001 no such
 * map, 0x80000003 not a mode 6 map of the screen's size, 0x80000006 no
 * rings, 0x80000007 no such ripple definition, 0x80000008 the definition
 * has fewer rings than asked for. */
int Effector_SetRipple(Effector_t* e, int map, int rings, int rippleNo, int level, int prio)
{
	Bmp_t info, back;
	uint32_t* table;
	int ok = 0;
	if(!BmpMgr_GetInfo(gDispBmpMgr, &info, map))
		return (int)0x80000001;
	Gfx_GetBackBmp(&back);
	if(info.mode != 6 || info.w != back.w || info.h != back.h)
		return (int)0x80000003;
	if((uint32_t)rings == 0)
		return (int)0x80000006;
	table = (uint32_t*)BGI_Alloc((size_t)rings * 16);
	if(BmpMgr_RippleCheck(gDispBmpMgr, &ok, rippleNo, 0, rings) != 0)
	{
		BGI_Free(table);
		return (int)0x80000007;
	}
	if(!ok)
	{
		BGI_Free(table); // the original leaks it
		return (int)0x80000008;
	}
	Effector_FreeTable(e);
	e->mode = 2;
	e->rippleMap = map;
	e->rippleMapGen = BmpMgr_Generation(gDispBmpMgr, map);
	e->rippleNo = rippleNo;
	e->rings = rings;
	e->ringTable = table;
	e->rippleSel = 0;
	e->obj.vt->setLevel(&e->obj, level);
	DispObj_SetPriority(&e->obj, (uint32_t)prio);
	return 0;
}

/* "91 68", mode 3: rotation by `angle` (16.16 degrees) and zoom by (sx, sy)
 * (16.16) about the screen point (cx, cy) (16.16), with `smooth` selecting
 * bilinear sampling; the deltas and the curve are cleared and the progress
 * reset.  The level is stored through the base class: the transform in
 * effect is computed by the setProgress call.  0 ok, 0x80000009 a zero
 * scale. */
int Effector_SetZoom(Effector_t* e, int32_t cx, int32_t cy, int32_t angle, int32_t sx, int32_t sy,
	int smooth, int level, int prio)
{
	if((uint32_t)sx == 0 || (uint32_t)sy == 0)
		return (int)0x80000009;
	Effector_FreeTable(e);
	e->cy = cy;
	e->cx = cx;
	e->mode = 3;
	e->angle = angle;
	e->sx = sx;
	e->sy = sy;
	e->smooth = smooth;
	e->dCx = e->dCy = e->dAngle = e->dSx = e->dSy = 0;
	e->curve = 0;
	DispObj_SetLevel(&e->obj, level);
	DispObj_SetProgressInt(&e->obj, 0);
	DispObj_SetPriority(&e->obj, (uint32_t)prio);
	return 0;
}

// "90 32" and the fades: the level is the strength of the distortion - the ripple amplitude in mode 2, the blend toward the transform in mode 3
static void Effector_SetLevel(DispObj_t* o, int level)
{
	Effector_t* e = (Effector_t*)o;
	DispObj_SetLevel(o, level);
	if(e->mode == 2)
		BmpMgr_RippleFill(gDispBmpMgr, e->ringTable, e->rippleNo, e->rippleSel, level, e->rings);
	else if(e->mode == 3)
		Effector_RecalcZoom(e);
}

// "90 35": a new progress moves the animated transform of mode 3
static void Effector_SetProgress(DispObj_t* o, int raw, int v)
{
	Effector_t* e = (Effector_t*)o;
	DispObj_SetProgress(o, raw, v);
	if(e->mode == 3)
		Effector_RecalcZoom(e);
}

/* "90 38": the effector's parameters - 0x80 the centre delta (a, c), 0x81
 * the angle delta, 0x82 the scale delta (a, c) (all mode 3 only, ignored
 * otherwise), 0x8F the easing curve of the angle delta, 0x100 the ripple
 * phase (a the selector, c the level; mode 2 only).  Anything else goes to
 * the base class.  0 ok, 0xffff0002 when the ripple selection failed. */
static int Effector_SetParam(DispObj_t* o, int no, int a, int c)
{
	Effector_t* e = (Effector_t*)o;
	switch(no)
	{
		case 0x80:
			if(e->mode == 3)
			{
				e->dCx = a;
				e->dCy = c;
			}
			return 0;
		case 0x81:
			if(e->mode == 3)
				e->dAngle = a;
			return 0;
		case 0x82:
			if(e->mode == 3)
			{
				e->dSx = a;
				e->dSy = c;
			}
			return 0;
		case 0x8f:
			e->curve = a;
			return 0;
		case 0x100:
			if(e->mode != 2)
				return 0;
			return Effector_SelectRipple(e, a, c) != 0 ? (int)0xffff0002 : 0;
		default:
			return DispObj_SetParam(o, no, a, c);
	}
}

// after a resolution change: take the back buffer's size and reallocate the snapshot; 1 if anything changed
int Effector_MatchScreenSize(Effector_t* e)
{
	Bmp_t back, own;
	Gfx_GetBackBmp(&back);
	DispObj_CopyBmp(&e->obj, &own);
	if(back.w == own.w && back.h == own.h && back.mode == own.mode)
		return 0;
	e->obj.vt->setSize(&e->obj, back.w, back.h);
	Effector_FreeSnapshot(e);
	Bmp_AllocScreen(&e->snap, back.w, back.h, 0);
	return 1;
}

/* setParam 0x100: move the ripple of mode 2 to phase `selector` and set its
 * amplitude.  0 ok (also outside mode 2, where nothing happens),
 * 0x80000007 no such ripple definition, 0x80000008 not enough rings left
 * from that phase. */
int Effector_SelectRipple(Effector_t* e, int selector, int level)
{
	int ok = 0;
	if(e->mode != 2)
		return 0;
	if(BmpMgr_RippleCheck(gDispBmpMgr, &ok, e->rippleNo, selector, e->rings) != 0)
		return (int)0x80000007;
	if(!ok)
		return (int)0x80000008;
	e->rippleSel = selector;
	e->obj.vt->setLevel(&e->obj, level);
	return 0;
}

// free the snapshot buffer; 1 if there was one
int Effector_FreeSnapshot(Effector_t* e)
{
	if(!e->snap.pixels)
		return 0;
	BGI_Free(e->snap.pixels);
	memset(&e->snap, 0, sizeof e->snap);
	return 1;
}

void Effector_FreeTable(Effector_t* e)
{
	BGI_Free(e->ringTable);
	e->ringTable = NULL;
}

// (a * b) >> sh with a sign-extended and b zero-extended to 64 bits
static int64_t MulShr64(int64_t a, uint32_t b, int sh)
{
	return (int64_t)((uint64_t)a * (uint64_t)b) >> sh;
}

/* The transform in effect in mode 3.  The deltas scaled by the progress
 * (the angle delta through the easing curve) are added to the base values;
 * the level then interpolates between the identity (angle 0, scale 1.0)
 * and that transform, so level 0 shows the screen unchanged.  Intermediate
 * sums are 64 bits wide, as in the original. */
void Effector_RecalcZoom(Effector_t* e)
{
	int level = e->obj.vt->getLevel(&e->obj);
	uint32_t t = (uint32_t)DispObj_GetProgress(&e->obj, 1);
	int64_t a, s;
	e->curCx = e->cx + (int32_t)MulShr64(e->dCx, t, 24);
	e->curCy = e->cy + (int32_t)MulShr64(e->dCy, t, 24);
	a = (((int64_t)Ease((int32_t)t, e->curve) * (int64_t)e->dAngle) >> 16) + (int64_t)e->angle;
	e->curAngle = (int32_t)MulShr64(a, (uint32_t)level, 8);
	s = MulShr64(e->dSx, t, 24) + (int64_t)(uint32_t)e->sx - 0x10000;
	e->curSx = 0x10000 + (int32_t)MulShr64(s, (uint32_t)level, 8);
	s = MulShr64(e->dSy, t, 24) + (int64_t)(uint32_t)e->sy - 0x10000;
	e->curSy = 0x10000 + (int32_t)MulShr64(s, (uint32_t)level, 8);
}

const DispObjVtbl_t Effector_Vtbl = {
	Effector_Destroy,
	DispObj_SetVisible,
	DispObj_IsVisible,
	DispObj_Invalidate,
	Effector_Draw,
	DispObj_SortKey,
	DispObj_LocalRect,
	DispObj_ScreenRect,
	DispObj_SetPosEx,
	DispObj_SetPos,
	DispObj_GetPos,
	DispObj_GetDrawPos,
	DispObj_SetOffset,
	DispObj_SetFixedPos,
	DispObj_GetFixedPos,
	Effector_SetLevel,
	DispObj_GetLevel,
	Effector_SetProgress,
	Effector_SetParam,
	DispObj_HitTest,
	DispObj_NopNotify,
	DispObj_BuildCache,
	DispObj_SetSize,
	DispObj_GetParam,
};
