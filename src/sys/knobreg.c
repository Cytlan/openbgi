/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * knobreg.c - the knob (slider) registry of the main window; interface in
 *             bgi/gfx.h
 *
 * A knob created by "90 D0" is registered here so the window's events can
 * find the knob under the mouse, start a drag on a left click and flag a
 * right click ("90 DB" reads the flags); "90 DE" / "90 DF" keep a second
 * list of "captured" knobs, the most recent of which receives the mouse
 * wheel while the "90 DD" switch is on.  Both lists live outside the
 * graphics manager: they hold the object pointer and the handle, nothing
 * more.  Knob_Update advances a drag once per pass (scheduler).
 */
#include "bgi/gfx.h"
#include "bgi/gfx/objects.h"
#include "bgi/input.h"
#include "bgi/inputmap.h"
#include "bgi/sysobj.h"
#include "bgi/display.h"
#include "bgi/core.h"

// start (e != NULL) or end a drag; event 0x1000 carries the handle of the knob grabbed
void Knob_BeginDrag(OpenBGI_t* o, KnobReg_t* e, int x, int y)
{
	if(!e)
	{
		o->input.knobDrag = NULL;
		return;
	}
	Knob_Grab(o, (Knob_t*)e->knob, x, y);
	MsgQueue_Post(o, 0x1000, e->handle, 0);
	o->input.knobDragX = x;
	o->input.knobDragY = y;
	o->input.knobDrag = e;
}

KnobReg_t* Knob_ActiveDrag(OpenBGI_t* o)
{
	return o->input.knobDrag;
}

// "91 DB" (1.588 on): the handle of the knob being dragged, 0 when none
uint32_t Knob_DragHandle(OpenBGI_t* o)
{
	return o->input.knobDrag ? o->input.knobDrag->handle : 0;
}

// Drop every registration (and the drag); shutdown and restart.
void Knob_UnregisterAll(OpenBGI_t* o)
{
	KnobReg_t* e = o->input.knobHead.next;
	while(e)
	{
		KnobReg_t* next = e->next;
		MouseLayer_PopSprite(o, e->knob);
		BGI_Free(e);
		e = next;
	}
	o->input.knobHead.next = NULL;
	Knob_BeginDrag(o, NULL, 0, 0);
}

/* "90 D0": register knob `h`, keeping the list sorted by layer key
 * (highest first) and pushing the knob onto the mouse layer stack.  1 ok,
 * 0 when the handle is not a knob. */
int Knob_Register(OpenBGI_t* o, uint32_t h)
{
	DispObj_t* k = Gfx_FindKnob(o->gfx.gfx, h);
	KnobReg_t *prev, *cur, *e;
	uint32_t key;
	if(!k)
		return 0;
	key = k->vt->sortKey(o, k);
	prev = &o->input.knobHead;
	for(cur = o->input.knobHead.next; cur && key < cur->sortKey; cur = cur->next)
		prev = cur;
	e = (KnobReg_t*)BGI_Alloc(sizeof *e);
	e->handle = h;
	e->knob = k;
	e->sortKey = key;
	e->changed = 0;
	e->next = cur;
	prev->next = e;
	MouseLayer_PushSprite(o, k);
	return 1;
}

// "90 D1": 1 when the knob was registered (an active drag on it ends), 0 otherwise
int Knob_Unregister(OpenBGI_t* o, uint32_t h)
{
	KnobReg_t *prev = &o->input.knobHead, *e;
	for(e = o->input.knobHead.next; e; prev = e, e = e->next)
	{
		if(e->handle != h)
			continue;
		if(Knob_ActiveDrag(o) && Knob_ActiveDrag(o) == e)
			Knob_BeginDrag(o, NULL, 0, 0);
		prev->next = e->next;
		MouseLayer_PopSprite(o, e->knob);
		BGI_Free(e);
		return 1;
	}
	return 0;
}

// the first registered knob whose layer key wins the mouse hit test; NULL when none
KnobReg_t* Knob_UnderMouse(OpenBGI_t* o)
{
	KnobReg_t* e;
	for(e = o->input.knobHead.next; e; e = e->next)
		if(MouseLayer_Hit(o, e->knob->vt->sortKey(o, e->knob)))
			break;
	return e;
}

// the first registered knob whose layer key wins the hit test at back-buffer position (x, y); NULL when none
static KnobReg_t* Knob_At(OpenBGI_t* o, int x, int y)
{
	KnobReg_t* e;
	for(e = o->input.knobHead.next; e; e = e->next)
		if(MouseLayer_HitAt(o, e->knob->vt->sortKey(o, e->knob), x, y))
			break;
	return e;
}

// a press at (x, y) takes hold of the thumb of knob d->ctx
static int ReachKnob(OpenBGI_t* o, const TargetDesc_t* d, int x, int y)
{
	return Knob_At(o, x, y) == (const KnobReg_t*)d->ctx;
}

/* The registered knobs whose thumbs are shown, as clickable targets
 * (bgi/inputmap.h): a left press on the thumb starts a drag, which moves
 * it within its travel (the knob's position plus its range). */
void Knob_Targets(OpenBGI_t* o, TargetSink_t* s)
{
	KnobReg_t* e;
	for(e = o->input.knobHead.next; e; e = e->next)
	{
		Knob_t* k = (Knob_t*)e->knob;
		TargetDesc_t d;
		int32_t origin[2];
		if(!e->knob->vt->isVisible(o, e->knob))
			continue;
		TargetDesc_Init(&d, OPENBGI_TARGET_KNOB, e->knob->vt->sortKey(o, e->knob));
		e->knob->vt->screenRect(o, e->knob, &d.rect);
		e->knob->vt->getPos(o, e->knob, origin);
		d.hasDrag = 1;
		d.drag.l = origin[0] + k->range.l;
		d.drag.t = origin[1] + k->range.t;
		d.drag.r = origin[0] + k->range.r;
		d.drag.b = origin[1] + k->range.b;
		if(Knob_ActiveDrag(o) == e)
			d.flags |= OPENBGI_TARGET_HELD;
		d.owner = e->handle;
		d.reach = ReachKnob;
		d.ctx = e;
		TargetSink_Add(s, &d);
	}
}

// a right click on a knob (from the window's button events)
void Knob_MarkChanged(KnobReg_t* e)
{
	e->changed = 1;
}

void Knob_SetGlobal(OpenBGI_t* o, int on)
{
	o->input.knobGlobal = on;
}

int Knob_GetGlobal(OpenBGI_t* o)
{
	return o->input.knobGlobal;
}

// Empty the captured list (shutdown).
void Knob_ReleaseAll(OpenBGI_t* o)
{
	KnobCapture_t* c = o->input.captureHead.next;
	while(c)
	{
		KnobCapture_t* next = c->next;
		BGI_Free(c);
		c = next;
	}
	o->input.captureHead.next = NULL;
}

// the most recently captured knob; NULL when none
DispObj_t* Knob_TopCaptured(OpenBGI_t* o)
{
	return o->input.captureHead.next ? o->input.captureHead.next->knob : NULL;
}

// "90 DF": take the knob off the captured list; 1 when it was captured, 0 otherwise (also for a non-knob)
int Knob_Release(OpenBGI_t* o, uint32_t h)
{
	DispObj_t* k = Gfx_FindKnob(o->gfx.gfx, h);
	KnobCapture_t *prev = &o->input.captureHead, *c;
	if(!k)
		return 0;
	for(c = o->input.captureHead.next; c; prev = c, c = c->next)
	{
		if(c->knob != k)
			continue;
		prev->next = c->next;
		BGI_Free(c);
		return 1;
	}
	return 0;
}

// "90 DE": put the knob on top of the captured list; 1 when the handle is a knob (nothing is deduplicated)
int Knob_Capture(OpenBGI_t* o, uint32_t h)
{
	DispObj_t* k = Gfx_FindKnob(o->gfx.gfx, h);
	KnobCapture_t* c;
	if(!k)
		return 0;
	c = (KnobCapture_t*)BGI_Alloc(sizeof *c);
	c->knob = k;
	c->next = o->input.captureHead.next;
	o->input.captureHead.next = c;
	return 1;
}

// the mouse wheel moves the top captured knob one step (down: +1, up: -1); 1 if there was one
int Knob_NudgeCaptured(OpenBGI_t* o, int down)
{
	KnobCapture_t* c = o->input.captureHead.next;
	if(c)
	{
		DispObj_t* k = c->knob;
		k->vt->invalidate(o, k); // the old and the new place of the thumb are redrawn
		Knob_Nudge(o, (Knob_t*)k, 0, down ? 1 : -1);
		k->vt->invalidate(o, k);
		Present_RequestDirty(o);
	}
	return c != NULL;
}

/* "90 DB": the handle of the last knob (in list order) flagged by a
 * right click, 0 when none; every flag is cleared */
uint32_t Knob_PollChanged(OpenBGI_t* o)
{
	uint32_t h = 0;
	KnobReg_t* e;
	for(e = o->input.knobHead.next; e; e = e->next)
	{
		if(e->changed)
		{
			h = e->handle;
			e->changed = 0;
		}
	}
	return h;
}

/* Once per pass: while the left button stays down the dragged knob
 * follows the mouse; when it is released event 0x1001 is posted with the
 * handle and the drag ends. */
void Knob_Update(OpenBGI_t* o)
{
	KnobReg_t* e = Knob_ActiveDrag(o);
	int pos[2];
	if(!e)
		return;
	if(!(GetKeyStateEx(o, 1) & 0x8000))
	{
		MsgQueue_Post(o, 0x1001, e->handle, 0);
		Knob_BeginDrag(o, NULL, 0, 0);
		return;
	}
	GetMouseClientPos(o, pos);
	if(pos[0] == o->input.knobDragX && pos[1] == o->input.knobDragY)
		return;
	e->knob->vt->invalidate(o, e->knob);
	if(Knob_Drag(o, (Knob_t*)e->knob, pos[0], pos[1]))
		e->knob->vt->invalidate(o, e->knob);
	Present_RequestDirty(o);
	o->input.knobDragX = pos[0];
	o->input.knobDragY = pos[1];
}
