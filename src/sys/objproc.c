/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * objproc.c - the controller base of the panels, the id registry and the
 *             panel settings; interface in bgi/panel.h
 *
 * An ObjProc occupies a display object's `proc` slot (DispObj_SetProc) for
 * its lifetime and is reachable from scripts by the id the registry hands
 * out ("90 B8" / "91 B8" create one, "90 B9" deletes it, "80 A8" / "80 A9"
 * switch it).  Messages posted with "80 AC" queue up as dword lists and
 * are delivered by ObjProc_ProcessMessages at the start of the proc's
 * poll: a list whose first dword is 0 is the built-in "set enabled" (two
 * dwords), anything else goes to the subclass.  The scheduler polls every
 * enabled proc once per pass (Panel_PollAll, or Panel_PumpAll plus a late
 * Panel_PollAll in the "80 AF" mode 1).  The script entry points at the
 * end wrap the two panel kinds (button panels, src/sys/panel.c, and sprite
 * panels, src/sys/sprpanel.c) behind one id.
 */
#include <string.h>
#include "bgi/panel.h"
#include "bgi/inputmap.h"
#include "bgi/gfx.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/window.h"
#include "bgi/display.h"
#include "bgi/core.h"

void Panel_SetGfxPtr(OpenBGI_t* o, struct Gfx* g)
{
	o->panel.panelGfx = g;
}

void Panel_SetBmpMgrPtr(OpenBGI_t* o, struct BmpMgr* m)
{
	o->panel.panelBmpMgr = m;
}

// ---- the base class --------------------------------------------------------

// the deleting destructor of the base
static void ObjProc_Destroy(OpenBGI_t* o, ObjProc_t* p)
{
	ObjProc_Dtor(p);
	BGI_Free(p);
}

// the base poll: only the messages; never asks to stop
static int ObjProc_Poll(OpenBGI_t* o, ObjProc_t* p)
{
	ObjProc_ProcessMessages(o, p);
	return 0;
}

// the base ignores every subclass message
static int ObjProc_OnMessage(OpenBGI_t* o, ObjProc_t* p, int n, const uint32_t* data)
{
	return 1;
}

static const ObjProcVtbl_t ObjProc_Vtbl = {ObjProc_Destroy, ObjProc_Poll, ObjProc_OnMessage};

// Attach a new proc of `kind` to `obj` (enabled, no messages); the id is the next serial.
void ObjProc_Ctor(OpenBGI_t* o, ObjProc_t* p, int kind, DispObj_t* obj)
{
	p->vt = &ObjProc_Vtbl;
	DispObj_SetProc(obj, p);
	p->id = ++o->panel.procSerial;
	p->obj = obj;
	p->kind = kind;
	p->enabled = 1;
	p->dirty = 0;
	p->msgHead.n = 0;
	p->msgHead.data = NULL;
	p->msgHead.next = NULL;
}

// detach the oldest message; its dword count (0 when none), the data copied to `out` (0x100 dwords)
static int ObjProc_PopMessage(ObjProc_t* p, uint32_t* out)
{
	ProcMsg_t* m = p->msgHead.next;
	int n;
	if(!m)
		return 0;
	n = m->n;
	memcpy(out, m->data, (size_t)n * 4);
	p->msgHead.next = m->next;
	BGI_Free(m->data);
	BGI_Free(m);
	return n;
}

// Drop the queued messages and detach the proc from its object (the vtable falls back to the base).
void ObjProc_Dtor(ObjProc_t* p)
{
	uint32_t buf[0x100];
	p->vt = &ObjProc_Vtbl;
	while(ObjProc_PopMessage(p, buf) > 0)
		;
	DispObj_ClearProc(p->obj, p);
}

// "80 AC": append a message of n dwords (1 .. 0x100) to the proc's queue; 1 ok, 0 for a bad count
int ObjProc_PostMessage(ObjProc_t* p, int n, const uint32_t* data)
{
	ProcMsg_t* tail = &p->msgHead;
	ProcMsg_t* m;
	if(n <= 0 || n > 0x100)
		return 0;
	while(tail->next)
		tail = tail->next;
	m = (ProcMsg_t*)BGI_Alloc(sizeof *m);
	m->n = n;
	m->data = (uint32_t*)BGI_Alloc((size_t)n * 4);
	m->next = NULL;
	memcpy(m->data, data, (size_t)n * 4);
	tail->next = m;
	return 1;
}

/* Deliver every queued message, oldest first: a first dword of 0 with
 * one more dword sets the enabled flag (a 0 with any other count is
 * dropped), anything else goes to the subclass's onMessage. */
void ObjProc_ProcessMessages(OpenBGI_t* o, ObjProc_t* p)
{
	uint32_t buf[0x100];
	int n;
	while((n = ObjProc_PopMessage(p, buf)) > 0)
	{
		if(buf[0] != 0)
			p->vt->onMessage(o, p, n, buf);
		else if(n == 2)
			ObjProc_SetEnabled(p, (int)buf[1]);
	}
}

void ObjProc_MarkDirty(ObjProc_t* p)
{
	p->dirty = 1;
}

// whether the proc's object is drawn at all: its sort key is at or above the "90 09" draw limit
int ObjProc_IsVisible(OpenBGI_t* o, ObjProc_t* p)
{
	if(!p->obj)
		return 0;
	return p->obj->vt->sortKey(o, p->obj) >= Gfx_GetDrawLimit(o->panel.panelGfx);
}

// a visible proc that drew something asks the display for a present; the dirty flag is cleared either way
void ObjProc_Flush(OpenBGI_t* o, ObjProc_t* p)
{
	if(p->dirty && o->panel.procPresent && ObjProc_IsVisible(o, p))
		Present_RequestDirty(o);
	p->dirty = 0;
}

// ---- the registry ----------------------------------------------

typedef struct ProcNode
{
	uint32_t id;           // the proc's id
	ObjProc_t* proc;       // the proc (owned: deleted with the node)
	struct ProcNode* next; // registered earlier
} ProcNode_t;

// put a proc in the registry; returns its id
static uint32_t SubObj_Register(OpenBGI_t* o, ObjProc_t* p)
{
	ProcNode_t* n = (ProcNode_t*)BGI_Alloc(sizeof *n);
	n->id = p->id;
	n->proc = p;
	n->next = o->panel.procHead;
	o->panel.procHead = n;
	o->panel.procCount++;
	return n->id;
}

// the proc with that id; NULL for an unknown id
ObjProc_t* SubObj_Find(OpenBGI_t* o, uint32_t id)
{
	ProcNode_t* n;
	for(n = o->panel.procHead; n; n = n->next)
		if(n->id == id)
			return n->proc;
	return NULL;
}

// "90 B9": destroy the proc with that id and drop it from the registry; 1 ok, 0 unknown id
int SubObj_Delete(OpenBGI_t* o, uint32_t id)
{
	ProcNode_t** link = &o->panel.procHead;
	ProcNode_t* n;
	for(n = o->panel.procHead; n; link = &n->next, n = n->next)
	{
		if(n->id != id)
			continue;
		*link = n->next;
		if(n->proc)
			n->proc->vt->destroy(o, n->proc);
		BGI_Free(n);
		return 1;
	}
	return 0;
}

// Destroy every registered proc (the engine's reset and shutdown).
void Panel_DeleteAll(OpenBGI_t* o)
{
	while(o->panel.procHead)
		SubObj_Delete(o, o->panel.procHead->id);
	o->panel.procCount = 0;
}

// Once per pass: poll the enabled procs, newest first, until one of them returns non-zero; 0 then, else 1.
int Panel_PollAll(OpenBGI_t* o)
{
	ProcNode_t* n;
	int go = 1;
	for(n = o->panel.procHead; n && go; n = n->next)
	{
		if(ObjProc_GetEnabled(n->proc))
			go = n->proc->vt->poll(o, n->proc) == 0;
	}
	return go;
}

/* "80 AF" of 1.573 on: when the panels are polled in a pass.  Mode 0
 * polls them before the input (Panel_PollAll where 1.69 has it); mode 1
 * only delivers their messages at that point (Panel_PumpAll) and polls
 * them at the end of the pass, after the knobs. */
// 1 when the mode is 0 or 1 and was stored, 0 otherwise
int Panel_SetPollMode(OpenBGI_t* o, int mode)
{
	if((unsigned)mode > 1)
		return 0;
	o->panel.panelPollMode = mode;
	return 1;
}

int Panel_GetPollMode(OpenBGI_t* o)
{
	return o->panel.panelPollMode;
}

// The messages (and presents) of every enabled proc, without its input: mode 1's early pass.
void Panel_PumpAll(OpenBGI_t* o)
{
	ProcNode_t* n;
	for(n = o->panel.procHead; n; n = n->next)
	{
		if(ObjProc_GetEnabled(n->proc))
		{
			ObjProc_ProcessMessages(o, n->proc); // the messages, then the flush while still enabled
			if(ObjProc_GetEnabled(n->proc))
				ObjProc_Flush(o, n->proc);
		}
	}
}

// The clickable targets of every panel (bgi/inputmap.h), newest first as they are polled.
void Panel_TargetsAll(OpenBGI_t* o, TargetSink_t* s)
{
	ProcNode_t* n;
	for(n = o->panel.procHead; n; n = n->next)
		if(n->proc && n->proc->kind == 0x80)
			ButtonPanel_Targets(o, (ButtonPanel_t*)n->proc, s);
}

// whether a display object has a proc attached ("90 51" / "90 81" refuse to delete one that has)
int Gfx_ObjHasProc(OpenBGI_t* o, uint32_t h)
{
	DispObj_t* obj = Gfx_FindObject(o->gfx.gfx, h);
	if(!obj)
		return 0;
	return obj->proc != NULL;
}

// the proc with that id when it is a panel (kind 0x80); NULL otherwise
static ObjProc_t* SubObj_FindPanel(OpenBGI_t* o, uint32_t id)
{
	ObjProc_t* p = SubObj_Find(o, id);
	if(p && p->kind == 0x80)
		return p;
	return NULL;
}

/* "90 B8" (kind 0, a button panel) / "91 B8" (kind 1, a sprite panel): a
 * panel on window `h`, registered; returns its id, or 0 for another kind.
 * An unknown handle still creates one, on a NULL window. */
uint32_t SubObj_Create(OpenBGI_t* o, uint32_t h, int kind)
{
	Window_t* win = (Window_t*)Gfx_FindWindow(o->gfx.gfx, h);
	ObjProc_t* p;
	switch(kind)
	{
		case 0: p = (ObjProc_t*)ButtonPanel_New(o, win); break;
		case 1: p = (ObjProc_t*)SpritePanel_New(o, win); break;
		default: return 0;
	}
	return SubObj_Register(o, p);
}

// ---- the panel settings --------------------------------------------------------

// Clear the user key maps once (the first panel, or the first "91 BF").
void Panel_InitKeyMaps(OpenBGI_t* o)
{
	if(o->panel.panelKeyMapsStale)
	{
		memset(o->panel.panelKeyMaps, 0, sizeof o->panel.panelKeyMaps);
		o->panel.panelKeyMapsStale = 0;
	}
}

// "91 BF": replace user key map `no` (4 .. 7) with 24 actions; 1 accepted, 0 bad number
int Panel_SetKeyMap(OpenBGI_t* o, int no, const uint32_t* actions)
{
	Panel_InitKeyMaps(o);
	if((uint32_t)(no - 4) >= 4)
		return 0;
	memcpy(o->panel.panelKeyMaps[no - 4], actions, sizeof o->panel.panelKeyMaps[0]);
	return 1;
}

// user key map `no` (4 .. 7) for the panel update's table of maps
const uint32_t* Panel_UserKeyMap(OpenBGI_t* o, int no)
{
	return o->panel.panelKeyMaps[no - 4];
}

void Panel_SetRequireActive(OpenBGI_t* o, int on)
{
	o->panel.panelRequireActive = on;
}

// whether the panels may take input now
int Panel_Allowed(OpenBGI_t* o)
{
	return o->panel.panelRequireActive ? Window_IsActive(o) : 1;
}

// ---- the script entry points shared by both kinds -------------

/* "90 BA": start (or restart) button panel `id` from the script's table
 * at `desc` (copied with its tagged pointers resolved through thread t).
 * 0 ok, 1 no panel with that id, 2 the group table is unusable, 3 a
 * button table is, 4 the id is a sprite panel. */
int Panel_Start(OpenBGI_t* o, uint32_t id, const void* desc, struct Thread* t)
{
	ObjProc_t* p = SubObj_FindPanel(o, id);
	PanelDesc_t* copy;
	int r;
	if(!p)
		return 1;
	if(ButtonPanel_IsSprite((ButtonPanel_t*)p))
		return 4;
	r = Panel_CopyDesc(o, &copy, (const PanelDescScript_t*)desc, t);
	if(r)
		return r;
	switch(ButtonPanel_Start(o, (ButtonPanel_t*)p, copy))
	{
		case 0x80000001u: r = 2; break;
		case 0x80000002u: r = 3; break;
		default: r = 0; break;
	}
	Panel_FreeDesc(copy);
	return r;
}

// "91 BA": the same for sprite panel `id`; 4 when the id is a button panel
int SprPanel_Start(OpenBGI_t* o, uint32_t id, const void* desc, struct Thread* t)
{
	ObjProc_t* p = SubObj_FindPanel(o, id);
	SpriteDesc_t* copy;
	int r;
	if(!p)
		return 1;
	if(!ButtonPanel_IsSprite((ButtonPanel_t*)p))
		return 4;
	r = SpritePanel_CopyDesc(o, &copy, (const SpriteDescScript_t*)desc, t);
	if(r)
		return r;
	switch(SpritePanel_Start(o, (ButtonPanel_t*)p, copy))
	{
		case 0x80000001u: r = 2; break;
		case 0x80000002u: r = 3; break;
		default: r = 0; break;
	}
	SpritePanel_FreeDesc(copy);
	return r;
}

// "90 BC": the panel's state into six words at `out` (ButtonPanel_GetState); 1 when `id` is a panel
int SubObj_GetCursor(OpenBGI_t* o, int32_t out[6], uint32_t id)
{
	ObjProc_t* p = SubObj_FindPanel(o, id);
	if(p)
		ButtonPanel_GetState((ButtonPanel_t*)p, out);
	return p != NULL;
}

// "90 BD": the index of the panel's current group into *out; 1 when `id` is a panel
int SubObj_GetGroup(OpenBGI_t* o, int32_t* out, uint32_t id)
{
	ObjProc_t* p = SubObj_FindPanel(o, id);
	if(p)
		*out = ButtonPanel_CurrentGroup((ButtonPanel_t*)p);
	return p != NULL;
}

// "90 BE": the selected button of every group into `out` (one word per group); 1 when `id` is a panel
int SubObj_GetSelections(OpenBGI_t* o, int32_t* out, uint32_t id)
{
	ObjProc_t* p = SubObj_FindPanel(o, id);
	if(p)
		ButtonPanel_GetSelections((ButtonPanel_t*)p, out);
	return p != NULL;
}

// "90 BF": the oldest event of the panel's queue into three words at `out`; 1 when `id` is a panel
int SubObj_PopEvent(OpenBGI_t* o, uint32_t out[3], uint32_t id)
{
	ObjProc_t* p = SubObj_FindPanel(o, id);
	if(p)
		ButtonPanel_PopEvent((ButtonPanel_t*)p, out);
	return p != NULL;
}

/* "90 B6": paint the buttons of the script's table at `desc` once into
 * window `h`.  0 ok, 1 no such window, 2 the group table is unusable,
 * 3 a button table is. */
int Panel_Draw(OpenBGI_t* o, uint32_t h, const void* desc, struct Thread* t)
{
	Window_t* win = (Window_t*)Gfx_FindWindow(o->gfx.gfx, h);
	PanelDesc_t* copy;
	int r;
	if(!win)
		return 1;
	r = Panel_CopyDesc(o, &copy, (const PanelDescScript_t*)desc, t);
	if(r)
		return r;
	switch(Panel_Paint(o, win, copy))
	{
		case 0x80000001u: r = 2; break;
		case 0x80000002u: r = 3; break;
		default: r = 0; break;
	}
	Panel_FreeDesc(copy);
	return r;
}

// "90 B7": the same for a sprite-panel table (one sub-sprite per button)
int SprPanel_Draw(OpenBGI_t* o, uint32_t h, const void* desc, struct Thread* t)
{
	Window_t* win = (Window_t*)Gfx_FindWindow(o->gfx.gfx, h);
	SpriteDesc_t* copy;
	int r;
	if(!win)
		return 1;
	r = SpritePanel_CopyDesc(o, &copy, (const SpriteDescScript_t*)desc, t);
	if(r)
		return r;
	switch(SpritePanel_Paint(o, win, copy))
	{
		case 0x80000001u: r = 2; break;
		case 0x80000002u: r = 3; break;
		default: r = 0; break;
	}
	SpritePanel_FreeDesc(copy);
	return r;
}
