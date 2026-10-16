/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * knobs.c - the graphics manager's knob (slider) operations: the "90 Dx"
 *           instructions (inc/bgi/gfx/gfxmgr.h)
 *
 * A knob turns another display object (its "thumb") into a slider: the
 * mouse drags the thumb inside a travel rectangle and the keyboard nudges
 * it, and the script reads the value back.  Knobs are not drawn, so they
 * are not added to the compositor; the repaints below are for the thumb
 * the knob moves.  32 slots, addressed by H_KNOB | index.  The window's
 * knob registry (src/sys/knobreg.c) does the mouse handling.
 */
#include "mgr_internal.h"

void Gfx_DeleteAllKnobs(Gfx_t* g)
{
	Mgr_DeleteAll(g->knobs, GFX_KNOBS, &g->knobCount, &g->knobNextId);
}

/* "90 D0": create a knob driving the object `hTarget` (its thumb) in the
 * first free slot and return its handle in *outHandle.  0 ok, 1 no free
 * slot, 2 the target handle does not resolve, 3 the target is itself a
 * virtual object / group / knob (class order >= 8). */
int Gfx_KnobCreate(Gfx_t* g, uint32_t* outHandle, uint32_t hTarget)
{
	DispObj_t* target;
	Knob_t* k;
	uint32_t i;

	if(g->knobCount >= GFX_KNOBS)
		return 1;
	target = Gfx_FindObject(g, hTarget);
	if(!target)
		return 2;
	if((uint32_t)target->classOrder >= 8u)
		return 3;
	i = Mgr_FreeSlot(g->knobs);
	k = (Knob_t*)BGI_Calloc(sizeof(Knob_t));
	Knob_Ctor(k, g->knobNextId++, target);
	g->knobs[i] = &k->obj;
	g->knobCount++;
	*outHandle = H_KNOB | i;
	return 0;
}

// "90 D1": knobs are not in the compositor and leave no footprint, so nothing is repainted.  1 ok, 0 unknown handle
int Gfx_KnobDelete(Gfx_t* g, uint32_t h)
{
	DispObj_t* o = Gfx_FindKnob(g, h);
	uint32_t idx;

	if(!o)
		return 0;
	idx = HANDLE_INDEX(h);
	if(g->knobs[idx])
		g->knobs[idx]->vt->destroy(g->knobs[idx], 1);
	g->knobs[idx] = NULL;
	g->knobCount--;
	return 1;
}

// "90 D4": show / hide; 1 ok, 0 unknown handle
int Gfx_KnobShow(Gfx_t* g, uint32_t h, int on)
{
	return Mgr_ShowBracket(Gfx_FindKnob(g, h), on);
}

// "90 D5": the track origin (pixels); the thumb moves along.  1 ok, 0 unknown handle
int Gfx_KnobSetPos(Gfx_t* g, uint32_t h, int x, int y)
{
	DispObj_t* o = Gfx_FindKnob(g, h);
	int was;

	if(!o)
		return 0;
	was = IsVisible(o);
	if(was)
		Invalidate(o);
	o->vt->setPos(o, x, y); // Knob_SetPos: moves the thumb along
	if(was)
		Invalidate(o);
	return 1;
}

// "90 D8": positions per axis, 0 = continuous (the value is in pixels); 0 ok, 4 a negative count, 0xFF unknown handle
int Gfx_KnobSetSteps(Gfx_t* g, uint32_t h, int x, int y)
{
	DispObj_t* o = Gfx_FindKnob(g, h);

	if(!o)
		return NO_OBJECT;
	return Knob_SetSteps((Knob_t*)o, x, y) ? 0 : 4;
}

// "90 D9": the travel rectangle, w x hgt pixels from the track origin; 0 ok, 5 it does not hold the thumb, 0xFF unknown handle
int Gfx_KnobSetRange(Gfx_t* g, uint32_t h, int w, int hgt)
{
	DispObj_t* o = Gfx_FindKnob(g, h);

	if(!o)
		return NO_OBJECT;
	return Knob_SetRange((Knob_t*)o, w, hgt) ? 0 : 5;
}

// "90 D6": the value (pixels or steps per axis); the thumb moves there.  1 ok, 0 unknown handle (an out-of-range value is not reported)
int Gfx_KnobSetValue(Gfx_t* g, uint32_t h, int x, int y)
{
	DispObj_t* o = Gfx_FindKnob(g, h);
	int was;

	if(!o)
		return 0;
	was = IsVisible(o);
	if(was)
		Invalidate(o);
	Knob_SetValue((Knob_t*)o, x, y);
	if(was)
		Invalidate(o);
	return 1;
}

// "90 D7": the value; 1 ok, 0 unknown handle
int Gfx_KnobGetValue(Gfx_t* g, uint32_t h, int32_t out[2])
{
	DispObj_t* o = Gfx_FindKnob(g, h);

	if(o)
		Knob_GetValue((Knob_t*)o, out);
	return o != NULL;
}

/* "90 DA": report and clear the last keyboard nudge.  *out receives the
 * vertical nudge when that nudge was rejected by the range, 0 otherwise
 * (the original ands the y delta with the "failed" flag).  1 ok, 0
 * unknown handle. */
int Gfx_KnobTakeNudge(Gfx_t* g, uint32_t h, int32_t* out)
{
	DispObj_t* o = Gfx_FindKnob(g, h);
	int32_t delta[2];
	int failed;

	if(!o)
		return 0;
	Knob_TakeNudge((Knob_t*)o, delta, &failed);
	*out = failed ? delta[1] : 0;
	return 1;
}

// "90 DC": whether the mouse may drag the thumb; 1 ok, 0 unknown handle
int Gfx_KnobSetDraggable(Gfx_t* g, uint32_t h, int f)
{
	DispObj_t* o = Gfx_FindKnob(g, h);

	if(o)
		Knob_SetDraggable((Knob_t*)o, f);
	return o != NULL;
}
