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
#include "bgi/sysobj.h"
#include "bgi/display.h"

struct KnobReg
{
	uint32_t handle;      // the knob's object handle
	DispObj_t* knob;      // the knob object
	uint32_t sortKey;     // the knob's layer key, the list is sorted by it
	int changed;          // a right click happened; "90 DB" reports and clears it
	struct KnobReg* next; // lower keys follow
};

typedef struct KnobCapture
{
	DispObj_t* knob;
	struct KnobCapture* next; // captured earlier
} KnobCapture_t;

/* a dummy list element whose `next` is the list head,
 * so the insertion loop never special-cases the head */
static KnobReg_t gKnobHead;
#define gKnobList (gKnobHead.next)

static KnobReg_t* gKnobDrag;       // the knob being dragged, NULL when none
static int gKnobDragX, gKnobDragY; // last mouse position seen by the drag (back-buffer coordinates)
static KnobCapture_t gCaptureHead; // (its `next` is the list)
#define gCaptureList (gCaptureHead.next)
int gKnobGlobal; // "90 DD": the wheel goes to the captured knob instead of the wheel keys

// start (e != NULL) or end a drag; event 0x1000 carries the handle of the knob grabbed
void Knob_BeginDrag(KnobReg_t* e, int x, int y)
{
	if(!e)
	{
		gKnobDrag = NULL;
		return;
	}
	Knob_Grab((Knob_t*)e->knob, x, y);
	MsgQueue_Post(0x1000, e->handle, 0);
	gKnobDragX = x;
	gKnobDragY = y;
	gKnobDrag = e;
}

KnobReg_t* Knob_ActiveDrag(void)
{
	return gKnobDrag;
}

// "91 DB" (1.588 on): the handle of the knob being dragged, 0 when none
uint32_t Knob_DragHandle(void)
{
	return gKnobDrag ? gKnobDrag->handle : 0;
}

// Drop every registration (and the drag); shutdown and restart.
void Knob_UnregisterAll(void)
{
	KnobReg_t* e = gKnobList;
	while(e)
	{
		KnobReg_t* next = e->next;
		MouseLayer_PopSprite(e->knob);
		BGI_Free(e);
		e = next;
	}
	gKnobList = NULL;
	Knob_BeginDrag(NULL, 0, 0);
}

/* "90 D0": register knob `h`, keeping the list sorted by layer key
 * (highest first) and pushing the knob onto the mouse layer stack.  1 ok,
 * 0 when the handle is not a knob. */
int Knob_Register(uint32_t h)
{
	DispObj_t* k = Gfx_FindKnob(gGfx, h);
	KnobReg_t *prev, *cur, *e;
	uint32_t key;
	if(!k)
		return 0;
	key = k->vt->sortKey(k);
	prev = &gKnobHead;
	for(cur = gKnobList; cur && key < cur->sortKey; cur = cur->next)
		prev = cur;
	e = (KnobReg_t*)BGI_Alloc(sizeof *e);
	e->handle = h;
	e->knob = k;
	e->sortKey = key;
	e->changed = 0;
	e->next = cur;
	prev->next = e;
	MouseLayer_PushSprite(k);
	return 1;
}

// "90 D1": 1 when the knob was registered (an active drag on it ends), 0 otherwise
int Knob_Unregister(uint32_t h)
{
	KnobReg_t *prev = &gKnobHead, *e;
	for(e = gKnobList; e; prev = e, e = e->next)
	{
		if(e->handle != h)
			continue;
		if(Knob_ActiveDrag() && Knob_ActiveDrag() == e)
			Knob_BeginDrag(NULL, 0, 0);
		prev->next = e->next;
		MouseLayer_PopSprite(e->knob);
		BGI_Free(e);
		return 1;
	}
	return 0;
}

// the first registered knob whose layer key wins the mouse hit test; NULL when none
KnobReg_t* Knob_UnderMouse(void)
{
	KnobReg_t* e;
	for(e = gKnobList; e; e = e->next)
		if(MouseLayer_Hit(e->knob->vt->sortKey(e->knob)))
			break;
	return e;
}

// a right click on a knob (from the window's button events)
void Knob_MarkChanged(KnobReg_t* e)
{
	e->changed = 1;
}

void Knob_SetGlobal(int on)
{
	gKnobGlobal = on;
}

int Knob_GetGlobal(void)
{
	return gKnobGlobal;
}

// Empty the captured list (shutdown).
void Knob_ReleaseAll(void)
{
	KnobCapture_t* c = gCaptureList;
	while(c)
	{
		KnobCapture_t* next = c->next;
		BGI_Free(c);
		c = next;
	}
	gCaptureList = NULL;
}

// the most recently captured knob; NULL when none
DispObj_t* Knob_TopCaptured(void)
{
	return gCaptureList ? gCaptureList->knob : NULL;
}

// "90 DF": take the knob off the captured list; 1 when it was captured, 0 otherwise (also for a non-knob)
int Knob_Release(uint32_t h)
{
	DispObj_t* k = Gfx_FindKnob(gGfx, h);
	KnobCapture_t *prev = &gCaptureHead, *c;
	if(!k)
		return 0;
	for(c = gCaptureList; c; prev = c, c = c->next)
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
int Knob_Capture(uint32_t h)
{
	DispObj_t* k = Gfx_FindKnob(gGfx, h);
	KnobCapture_t* c;
	if(!k)
		return 0;
	c = (KnobCapture_t*)BGI_Alloc(sizeof *c);
	c->knob = k;
	c->next = gCaptureList;
	gCaptureList = c;
	return 1;
}

// the mouse wheel moves the top captured knob one step (down: +1, up: -1); 1 if there was one
int Knob_NudgeCaptured(int down)
{
	KnobCapture_t* c = gCaptureList;
	if(c)
	{
		DispObj_t* k = c->knob;
		k->vt->invalidate(k); // the old and the new place of the thumb are redrawn
		Knob_Nudge((Knob_t*)k, 0, down ? 1 : -1);
		k->vt->invalidate(k);
		Present_RequestDirty();
	}
	return c != NULL;
}

/* "90 DB": the handle of the last knob (in list order) flagged by a
 * right click, 0 when none; every flag is cleared */
uint32_t Knob_PollChanged(void)
{
	uint32_t h = 0;
	KnobReg_t* e;
	for(e = gKnobList; e; e = e->next)
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
void Knob_Update(void)
{
	KnobReg_t* e = Knob_ActiveDrag();
	int pos[2];
	if(!e)
		return;
	if(!(GetKeyStateEx(1) & 0x8000))
	{
		MsgQueue_Post(0x1001, e->handle, 0);
		Knob_BeginDrag(NULL, 0, 0);
		return;
	}
	GetMouseClientPos(pos);
	if(pos[0] == gKnobDragX && pos[1] == gKnobDragY)
		return;
	e->knob->vt->invalidate(e->knob);
	if(Knob_Drag((Knob_t*)e->knob, pos[0], pos[1]))
		e->knob->vt->invalidate(e->knob);
	Present_RequestDirty();
	gKnobDragX = pos[0];
	gKnobDragY = pos[1];
}
