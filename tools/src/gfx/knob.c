/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * knob.c - Knob, Group and VirtualObject, the small display object classes
 * without a picture of their own.  Interface in inc/bgi/gfx/objects.h.
 *
 * A knob turns another display object (its "target", the thumb) into a
 * slider: it becomes the target's owner, keeps the thumb inside a travel
 * rectangle relative to the knob's own position (the track origin) and
 * converts between pixel displacement and a logical value in steps.  Most of
 * its virtual methods forward to the target.  The mouse plumbing that calls
 * Knob_Grab / Knob_Drag / Knob_Nudge lives in sys/knobreg.c; the scripts
 * reach the knob through "90 D0".."90 DF".
 *
 * A group is a plain DispObj of class order 9 that only exists to carry
 * children ("90 E0".."90 E9"); a virtual object is an invisible stand-in
 * sorted 0x8000 above its target - the panels and the wait icon attach them
 * to their window as hit-test proxies for the buttons.
 */
#include "bgi/common.h"
#include "bgi/gfx/objects.h"

// ========================================================================
// Group
// ========================================================================

static void Group_Destroy(DispObj_t* o, int flags)
{
	o->vt = &Group_Vtbl;
	DispObj_Dtor(o);
	if(flags & 1)
		BGI_Free(o);
}

void Group_Ctor(Group_t* g, int slotId)
{
	DispObj_Ctor(&g->obj, 9, slotId);
	g->obj.vt = &Group_Vtbl;
}

const DispObjVtbl_t Group_Vtbl = {
	Group_Destroy,
	DispObj_SetVisible,
	DispObj_IsVisible,
	DispObj_Invalidate,
	DispObj_NopDraw,
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
	DispObj_SetLevel,
	DispObj_GetLevel,
	DispObj_SetProgress,
	DispObj_SetParam,
	DispObj_HitTest,
	DispObj_NopNotify,
	DispObj_BuildCache,
	DispObj_SetSize,
	DispObj_GetParam,
};

// ========================================================================
// VirtualObject
// ========================================================================

static void VirtualObject_Destroy(DispObj_t* o, int flags)
{
	o->vt = &VirtualObject_Vtbl;
	DispObj_Dtor(o);
	if(flags & 1)
		BGI_Free(o);
}

// DispObj(8, slotId) with the target's priority
void VirtualObject_Ctor(VirtualObject_t* v, int slotId, DispObj_t* target)
{
	DispObj_Ctor(&v->obj, 8, slotId);
	v->obj.vt = &VirtualObject_Vtbl;
	v->target = target;
	DispObj_SetPriority(&v->obj, DispObj_GetPriority(target));
}

// just above the target: hit tests find the stand-in first
static uint32_t VirtualObject_SortKey(DispObj_t* o)
{
	DispObj_t* t = ((VirtualObject_t*)o)->target;
	return t->vt->sortKey(t) + 0x8000;
}

const DispObjVtbl_t VirtualObject_Vtbl = {
	VirtualObject_Destroy,
	DispObj_SetVisible,
	DispObj_IsVisible,
	DispObj_Invalidate,
	DispObj_NopDraw,
	VirtualObject_SortKey,
	DispObj_LocalRect,
	DispObj_ScreenRect,
	DispObj_SetPosEx,
	DispObj_SetPos,
	DispObj_GetPos,
	DispObj_GetDrawPos,
	DispObj_SetOffset,
	DispObj_SetFixedPos,
	DispObj_GetFixedPos,
	DispObj_SetLevel,
	DispObj_GetLevel,
	DispObj_SetProgress,
	DispObj_SetParam,
	DispObj_HitTest,
	DispObj_NopNotify,
	DispObj_BuildCache,
	DispObj_SetSize,
	DispObj_GetParam,
};

// ========================================================================
// Knob
// ========================================================================

#define TARGET(k) ((k)->target)

/* "90 D0": the knob takes ownership of the thumb and mirrors its effect,
 * level and priority; the travel is exactly the thumb's size (no movement
 * yet), the steps are continuous, the track origin is where the thumb is
 * and the value is (0, 0). */
void Knob_Ctor(Knob_t* k, int slotId, DispObj_t* target)
{
	Rect_t lr;
	int32_t p[2];
	DispObj_Ctor(&k->obj, 10, slotId);
	k->obj.vt = &Knob_Vtbl;
	DispObj_SetOwner(target, &k->obj);
	k->target = target;
	DispObj_SetEffect(&k->obj, DispObj_GetEffect(target));
	DispObj_SetLevel(&k->obj, target->vt->getLevel(target));
	DispObj_SetPriority(&k->obj, DispObj_GetPriority(target));
	Knob_SetSteps(k, 0, 0);
	target->vt->localRect(target, &lr);
	Knob_SetRange(k, lr.r - lr.l + 1, lr.b - lr.t + 1);
	Knob_SetDraggable(k, 1);
	target->vt->getPos(target, p);
	DispObj_SetPos(&k->obj, p[0], p[1]); // virtual setPosEx: Knob_SetPosEx
	Knob_SetValue(k, 0, 0);
	Knob_TakeNudge(k, NULL, NULL);
}

// release the thumb (it stays where it is)
void Knob_Dtor(Knob_t* k)
{
	k->obj.vt = &Knob_Vtbl;
	DispObj_ClearOwner(k->target, &k->obj);
	DispObj_Dtor(&k->obj);
}

static void Knob_Destroy(DispObj_t* o, int flags)
{
	Knob_Dtor((Knob_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

// forwarders: the knob's rectangle, key, dirtying and hit test are the thumb's
static void Knob_Invalidate(DispObj_t* o)
{
	DispObj_t* t = TARGET((Knob_t*)o);
	t->vt->invalidate(t);
}

static uint32_t Knob_SortKey(DispObj_t* o)
{
	DispObj_t* t = TARGET((Knob_t*)o);
	return t->vt->sortKey(t);
}

static void Knob_LocalRect(DispObj_t* o, Rect_t* out)
{
	DispObj_t* t = TARGET((Knob_t*)o);
	t->vt->localRect(t, out);
}

static void Knob_ScreenRect(DispObj_t* o, Rect_t* out)
{
	DispObj_t* t = TARGET((Knob_t*)o);
	t->vt->screenRect(t, out);
}

// "90 D4": shown or hidden together with the thumb
static void Knob_SetVisible(DispObj_t* o, int on)
{
	DispObj_t* t = TARGET((Knob_t*)o);
	DispObj_SetVisible(o, on);
	t->vt->setVisible(t, on);
}

static void Knob_SetLevel(DispObj_t* o, int level)
{
	DispObj_t* t = TARGET((Knob_t*)o);
	DispObj_SetLevel(o, level);
	t->vt->setLevel(t, level);
}

static int Knob_HitTest(DispObj_t* o, int x, int y)
{
	DispObj_t* t = TARGET((Knob_t*)o);
	return t->vt->hitTest(t, x, y);
}

// "90 D5": the track origin moves; the thumb follows at origin + pixels(value)
static void Knob_SetPosEx(DispObj_t* o, int x, int y, int tellOwner)
{
	Knob_t* k = (Knob_t*)o;
	int32_t v[2];
	DispObj_SetPosEx(o, x, y, tellOwner);
	Knob_GetValue(k, v);
	if(k->stepsX > 0)
		v[0] = (int32_t)((uint32_t)(Knob_StepSizeX(k) + 1) * (uint32_t)v[0]) >> 16;
	if(k->stepsY > 0)
		v[1] = (int32_t)((uint32_t)(Knob_StepSizeY(k) + 1) * (uint32_t)v[1]) >> 16;
	k->target->vt->setPosEx(k->target, x + v[0], y + v[1], tellOwner);
}

static void Knob_SetPos(DispObj_t* o, int x, int y)
{
	DispObj_SetPos(o, x, y);
}

// "90 D8": positions per axis; 0 = continuous (the value is in pixels), n = n positions.  0 when negative
int Knob_SetSteps(Knob_t* k, int x, int y)
{
	if(x < 0 || y < 0)
		return 0;
	k->stepsX = x;
	k->stepsY = y;
	return 1;
}

// "90 D9": the travel rectangle, w x h pixels from the track origin; it must hold the thumb (else 0)
int Knob_SetRange(Knob_t* k, int w, int h)
{
	Rect_t lr;
	k->obj.vt->localRect(&k->obj, &lr);
	if(w < lr.r - lr.l + 1 || h < lr.b - lr.t + 1)
		return 0;
	k->range.l = 0;
	k->range.t = 0;
	k->range.r = w - 1;
	k->range.b = h - 1;
	return 1;
}

// "90 DC": whether the mouse may move the thumb
void Knob_SetDraggable(Knob_t* k, int f)
{
	k->draggable = f;
}

// remember where inside the thumb the mouse took hold (0, 0 when not draggable)
void Knob_Grab(Knob_t* k, int mouseX, int mouseY)
{
	int32_t p[2];
	if(!k->draggable)
	{
		k->grabX = k->grabY = 0;
		return;
	}
	k->target->vt->getPos(k->target, p);
	k->grabX = mouseX - p[0];
	k->grabY = mouseY - p[1];
}

// pixels per step in 16.16 (the whole travel for 0..2 steps), never below 1
int32_t Knob_StepSizeX(Knob_t* k)
{
	Rect_t lr;
	int32_t s;
	k->obj.vt->localRect(&k->obj, &lr);
	s = (int32_t)((uint32_t)(k->range.r - k->range.l - lr.r) << 16);
	if(k->stepsX > 2)
		s /= k->stepsX - 1;
	return s > 0 ? s : 1;
}

int32_t Knob_StepSizeY(Knob_t* k)
{
	Rect_t lr;
	int32_t s;
	k->obj.vt->localRect(&k->obj, &lr);
	s = (int32_t)((uint32_t)(k->range.b - k->range.t - lr.b) << 16);
	if(k->stepsY > 2)
		s /= k->stepsY - 1;
	return s > 0 ? s : 1;
}

// the largest value on each axis: the free travel in pixels when continuous, 0 for a single step, else steps - 1
void Knob_MaxValue(Knob_t* k, int32_t out[2])
{
	Rect_t lr;
	k->obj.vt->localRect(&k->obj, &lr);
	if(k->stepsX <= 0)
		out[0] = k->range.r - k->range.l - lr.r;
	else if(k->stepsX == 1)
		out[0] = 0;
	else
		out[0] = (int32_t)((uint32_t)(k->range.r - k->range.l - lr.r) << 16) / Knob_StepSizeX(k);
	if(k->stepsY <= 0)
		out[1] = k->range.b - k->range.t - lr.b;
	else if(k->stepsY == 1)
		out[1] = 0;
	else
		out[1] = (int32_t)((uint32_t)(k->range.b - k->range.t - lr.b) << 16) / Knob_StepSizeY(k);
}

/* "90 D6": store the value and move the thumb; 1 when both axes were in
 * range.  An in-range x is stored even when y is not (but the thumb does
 * not move); an in-range y with x out of range is stored too. */
int Knob_SetValue(Knob_t* k, int x, int y)
{
	int32_t mv[2], p[2], px = x, py = y;
	int okX = 1;
	Knob_MaxValue(k, mv);
	if(x >= 0 && x <= mv[0])
	{
		k->valX = x;
		if(k->stepsX > 0)
			px = (int32_t)((uint32_t)(Knob_StepSizeX(k) + 1) * (uint32_t)x) >> 16;
	}
	else
	{
		okX = 0;
	}
	if(y < 0 || y > mv[1])
		return 0;
	k->valY = y;
	if(k->stepsY > 0)
		py = (int32_t)((uint32_t)(Knob_StepSizeY(k) + 1) * (uint32_t)y) >> 16;
	if(okX)
	{
		k->obj.vt->getPos(&k->obj, p);
		k->target->vt->setPos(k->target, p[0] + px, p[1] + py);
	}
	return okX;
}

// "90 D7"
void Knob_GetValue(Knob_t* k, int32_t out[2])
{
	out[0] = k->valX;
	out[1] = k->valY;
}

/* The mouse moved while the thumb is held: the mouse position (minus the
 * grab offset, relative to the track origin) is clamped to the travel and
 * rounded to the nearest step.  1 when the value changed, 0 otherwise. */
int Knob_Drag(Knob_t* k, int mouseX, int mouseY)
{
	int32_t origin[2], cur[2], px, py, hi;
	Rect_t lr;
	k->obj.vt->getPos(&k->obj, origin);
	k->obj.vt->localRect(&k->obj, &lr);

	px = mouseX - k->grabX - origin[0];
	if(px < k->range.l)
		px = k->range.l;
	hi = k->range.r - lr.r;
	if(px > hi)
		px = hi;
	if(k->stepsX > 0)
	{
		int32_t half = k->stepsX > 1 ? hi / (2 * k->stepsX - 2) : 0; // half a step, for the rounding
		px = (int32_t)((uint32_t)(px + half) << 16) / Knob_StepSizeX(k);
	}

	py = mouseY - k->grabY - origin[1];
	if(py < k->range.t)
		py = k->range.t;
	hi = k->range.b - lr.b;
	if(py > hi)
		py = hi;
	if(k->stepsY > 0)
	{
		int32_t half = k->stepsY > 1 ? hi / (2 * k->stepsY - 2) : 0;
		py = (int32_t)((uint32_t)(py + half) << 16) / Knob_StepSizeY(k);
	}

	Knob_GetValue(k, cur);
	if(cur[0] == px && cur[1] == py)
		return 0;
	Knob_SetValue(k, px, py);
	return 1;
}

// the keyboard / wheel moved the thumb by (dx, dy) steps: apply it and remember it for "90 DA"; the Knob_SetValue result
int Knob_Nudge(Knob_t* k, int dx, int dy)
{
	int32_t cur[2];
	int ok;
	Knob_GetValue(k, cur);
	ok = Knob_SetValue(k, cur[0] + dx, cur[1] + dy);
	k->nudgeY = dy;
	k->nudged = 1;
	k->nudgeX = dx;
	k->nudgeFailed = ok == 0;
	return ok;
}

// "90 DA": report (into the optional outputs) and clear the last nudge; returns whether there was one
int Knob_TakeNudge(Knob_t* k, int32_t outDelta[2], int* outFailed)
{
	int was = k->nudged;
	if(outDelta)
	{
		outDelta[0] = k->nudgeX;
		outDelta[1] = k->nudgeY;
	}
	if(outFailed)
		*outFailed = k->nudgeFailed;
	k->nudged = k->nudgeX = k->nudgeY = k->nudgeFailed = 0;
	return was;
}

const DispObjVtbl_t Knob_Vtbl = {
	Knob_Destroy,
	Knob_SetVisible,
	DispObj_IsVisible,
	Knob_Invalidate,
	DispObj_NopDraw,
	Knob_SortKey,
	Knob_LocalRect,
	Knob_ScreenRect,
	Knob_SetPosEx,
	Knob_SetPos,
	DispObj_GetPos,
	DispObj_GetDrawPos,
	DispObj_SetOffset,
	DispObj_SetFixedPos,
	DispObj_GetFixedPos,
	Knob_SetLevel,
	DispObj_GetLevel,
	DispObj_SetProgress,
	DispObj_SetParam,
	Knob_HitTest,
	DispObj_NopNotify,
	DispObj_BuildCache,
	DispObj_SetSize,
	DispObj_GetParam,
};
