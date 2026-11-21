/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wait_icon.c - the icon selections in a message window and their starters
 *
 * Interface: waitobj.h (StartIconSelect, StartIconSelect2, Icon_Draw);
 * IconSelect_SetRequireActive is in gfx.h.
 *
 *   WaitIconSelect   "90 B0": up to 64 icons, each a bitmap drawn into the
 *                    window's text layer plus a virtual hit object; a focus
 *                    bitmap marks the icon under the mouse
 *   WaitIconSelect2  "90 B1": the same with 0x40-byte records whose overlay
 *                    triples go into the window's item layers
 *   Icon_Draw        "90 B4" / "90 B5": draw the icons, no wait
 *
 * The wait ends on a click on an icon, on a hot key, on a cancel key (when
 * allowed), or on a "80 4C" notification with a = 0x200; it pushes the
 * click position inside the icon (x, y) and the icon index (-1 for none /
 * cancel; also -1 when waits are not blocking).  A change of the icon under
 * the mouse is reported to the thread's message queue as 0x20000001.  A
 * selection takes input only while its window holds the input focus and,
 * with "90 AF", while the engine window is the active one.
 */
#include "bgi/wait.h"
#include "bgi/waitobj.h"
#include "bgi/input.h"
#include "bgi/inputmap.h"
#include "bgi/display.h"
#include "bgi/sysobj.h"
#include "bgi/gfx.h"
#include "bgi/gfx/text.h"
#include "bgi/gfx/window.h"
#include "bgi/gfx/objects.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/core.h"

#define ICON_MAX 64 // icons at most

typedef struct IconRec // 16 bytes, the "90 B0" record
{
	int32_t x, y;     // relative to the window's text area (pixels)
	int32_t bmp;      // the icon bitmap
	int32_t bmpFocus; // drawn (effect 0x40) while the icon has the focus; -1 none
} IconRec_t;

typedef struct IconRec2 // 0x40 bytes, the "90 B1" record
{
	int32_t x, y;        // relative to the window's text area (pixels)
	int32_t bmp;         // the icon bitmap (also its focus bitmap)
	int32_t items[4][3]; // (x, y, bitmap) for the window's item layers while focused
	int32_t hitMask;     // bitmap of the hit mask; -1 = the icon bitmap
} IconRec2_t;

typedef struct WaitIconSelect // 0x658 bytes
{
	Wait_t w;
	Window_t* win;             // the message window the icons live in
	DispObj_t* firstVirtual;   // its layer key is the one polled for input
	int mode;                  // 0 none, 1/2 mouse layer on the window, 3 own key + mouse layer
	uint32_t input;            // the input bits of the pass
	int32_t clickX, clickY;    // click position inside the icon (results)
	int count;                 // icons
	IconRec_t icons[ICON_MAX]; // the icons (a "90 B1" record is reduced to this)
	DispObj_t* virt[ICON_MAX]; // the virtual hit objects
	int keyLayerPushed;        // the window's key is on the key layer stack
	int hotkeysOn;             // hotkeys[] is in use
	int32_t hotkeys[ICON_MAX]; // one virtual key per icon
	int virtCount;             // hit objects created
	int focus;                 // icon under the mouse, -1
	int allowCancel;           // the cancel keys (bits 1 and 9) end the wait
	int cancelled;             // a "80 4C" notification with a = 0x200 arrived
	int active;                // the window has the input focus
} WaitIconSelect_t;

typedef struct WaitIconSelect2 // 0x1658 bytes
{
	WaitIconSelect_t s;
	IconRec2_t recs[ICON_MAX]; // the records as passed
	IconRec2_t spill;          // the original's highlight reads 8 item triples per record
							   // although a record holds 4; the overrun reads the next record
							   // (or, for the last one, whatever follows the object)
} WaitIconSelect2_t;

typedef struct IconVtbl // the base table plus the focus mark
{
	WaitVtbl_t base;
	void (*highlight)(OpenBGI_t* o, WaitIconSelect_t* w, int idx); // move the focus mark to icon idx (-1 none) and record it
} IconVtbl_t;
#define HIGHLIGHT(o, w, i) (((const IconVtbl_t*)(w)->w.vt)->highlight(o, (w), (i)))

// "90 AF" (through Select_RequireActive)
void IconSelect_SetRequireActive(OpenBGI_t* o, int on)
{
	o->wait.selectRequireActive = on;
}

// may a selection react to input right now?
static int SelectionAllowed(OpenBGI_t* o)
{
	return o->wait.selectRequireActive ? Window_IsActive(o) : 1;
}

// the window's sort key: the selection's input layer key and the key of its dirty rectangles
#define WIN_KEY(o, w) ((w)->win->obj.vt->sortKey(o, &(w)->win->obj))

static void WaitIconSelect_Destroy(OpenBGI_t* o, Wait_t* w);
static int WaitIconSelect_Poll(OpenBGI_t* o, Wait_t* w);
static void WaitIconSelect_OnMessage(OpenBGI_t* o, Wait_t* w, WaitMsg_t* m);
static void WaitIconSelect_Highlight(OpenBGI_t* o, WaitIconSelect_t* w, int idx);
static void WaitIconSelect2_Destroy(OpenBGI_t* o, Wait_t* w);
static void WaitIconSelect2_Highlight(OpenBGI_t* o, WaitIconSelect_t* w, int idx);

static const IconVtbl_t WaitIconSelect_Vtbl = {
	{WaitIconSelect_Destroy, WaitIconSelect_Poll, Wait_SetTimer, Wait_IsDirtyBase, WaitIconSelect_OnMessage},
	WaitIconSelect_Highlight};
static const IconVtbl_t WaitIconSelect2_Vtbl = {
	{WaitIconSelect2_Destroy, WaitIconSelect_Poll, Wait_SetTimer, Wait_IsDirtyBase, WaitIconSelect_OnMessage},
	WaitIconSelect2_Highlight};

// ========================================================================
// WaitIconSelect
// ========================================================================

static void WaitIconSelect_SetAllowCancel(WaitIconSelect_t* w, int on)
{
	w->allowCancel = on;
}

/* the shared constructor: the window's text layer is reset to opaque and
 * visible, its item layers switched off, window message 0x200 (mouse
 * moves) subscribed; mode 3 takes the input through the window's own layer
 * key on both stacks (and joins the input-focus list), modes 1 and 2
 * through the window sprite on the mouse layer stack, mode 0 only through
 * the icons' hit objects */
static void WaitIconSelect_Ctor(OpenBGI_t* o, WaitIconSelect_t* w, Thread_t* t, Window_t* win, int mode)
{
	Wait_Ctor(o, &w->w, t);
	w->w.vt = &WaitIconSelect_Vtbl.base;
	w->win = win;
	w->mode = mode;
	w->count = 0;
	w->keyLayerPushed = 0;
	w->hotkeysOn = 0;
	w->virtCount = 0;
	w->focus = -1;
	WaitIconSelect_SetAllowCancel(w, 0);
	Window_SetTextLevel(win, 0);
	Window_ShowText(win, 1);
	Window_RecompositeAll(o, win);
	Window_ItemsDisableAll(o, win);
	WinMsgWait_Register(o, t, 0x200);
	if(mode == 3)
	{
		MouseLayer_Push(o, WIN_KEY(o, w));
		KeyLayer_Push(o, WIN_KEY(o, w));
		w->keyLayerPushed = 1;
		InputFocus_Add(o, w->w.serial, WIN_KEY(o, w));
	}
	else if(mode != 0)
		MouseLayer_PushSprite(o, &win->obj);
	w->cancelled = 0;
	w->active = 0;
}

// drop the virtual hit objects
static void WaitIconSelect_FreeVirtuals(OpenBGI_t* o, WaitIconSelect_t* w)
{
	int i;
	for(i = 0; i < w->virtCount; i++)
	{
		MouseLayer_PopSprite(o, w->virt[i]);
		DispObj_DetachChild(&w->win->obj, w->virt[i]);
		if(w->virt[i])
			w->virt[i]->vt->destroy(o, w->virt[i], 1);
	}
	w->count = 0;
	w->virtCount = 0;
}

// gives the layers and the subscription back and drops the hit objects
static void WaitIconSelect_Dtor(OpenBGI_t* o, WaitIconSelect_t* w)
{
	if(w->mode != 0)
	{
		if(w->mode == 3)
			MouseLayer_Pop(o, WIN_KEY(o, w));
		else
			MouseLayer_PopSprite(o, &w->win->obj);
	}
	if(w->keyLayerPushed)
		KeyLayer_Pop(o, WIN_KEY(o, w));
	WinMsgWait_Unregister(o, w->w.thread, 0x200);
	WaitIconSelect_FreeVirtuals(o, w);
	Wait_Dtor(o, &w->w);
}

static void WaitIconSelect_Destroy(OpenBGI_t* o, Wait_t* w)
{
	WaitIconSelect_Dtor(o, (WaitIconSelect_t*)w);
	BGI_Free(w);
}

/* one icon's virtual hit object: the size of its bitmap, attached to the
 * window at the icon position, optionally with a pixel hit mask; pushed on
 * the mouse layer stack */
static void IconSelect_AddVirtual(OpenBGI_t* o, WaitIconSelect_t* w, int x, int y, const Bmp_t* bmp, const Bmp_t* hitMask)
{
	VirtualObject_t* v = (VirtualObject_t*)BGI_Alloc(sizeof *v);
	VirtualObject_Ctor(o, v, 0, &w->win->obj);
	w->virt[w->virtCount] = &v->obj;
	DispObj_SetVirtualSize(&v->obj, bmp->w, bmp->h);
	v->obj.vt->setVisible(o, &v->obj, w->win->obj.vt->isVisible(o, &w->win->obj));
	DispObj_AttachChild(o, &w->win->obj, &v->obj, x, y);
	if(hitMask)
		DispObj_BuildHitMask(&v->obj, hitMask);
	MouseLayer_PushSprite(o, &v->obj);
	w->virtCount++;
}

/* the icon at back-buffer position (x, y), -1 for none; `off` (may be
 * NULL) receives the position inside it.  Nothing changes. */
static int WaitIconSelect_HitAt(OpenBGI_t* o, const WaitIconSelect_t* w, int x, int y, int32_t off[2])
{
	int i;
	for(i = 0; i < w->count; i++)
	{
		DispObj_t* v = w->virt[i];
		Rect_t r;
		v->vt->screenRect(o, v, &r);
		if(r.l > x || x > r.r || r.t > y || y > r.b)
			continue;
		if(v->vt->hitTest(o, v, x - r.l, y - r.t))
		{
			if(off)
			{
				off[0] = x - r.l;
				off[1] = y - r.t;
			}
			return i;
		}
	}
	return -1;
}

// the icon under the mouse (records the position inside it), -1 for none (or while inactive)
static int WaitIconSelect_HitTest(OpenBGI_t* o, WaitIconSelect_t* w)
{
	int mouse[2], i;
	int32_t off[2];
	if(!w->active)
		return -1;
	GetMouseClientPos(o, mouse);
	i = WaitIconSelect_HitAt(o, w, mouse[0], mouse[1], off);
	if(i != -1)
	{
		w->clickX = off[0];
		w->clickY = off[1];
	}
	return i;
}

// the common tail of the two setups: re-attach the hit objects, poll the input away, first highlight
static void IconSelect_Finish(OpenBGI_t* o, WaitIconSelect_t* w)
{
	int32_t pos[2];
	Rect_t r;
	w->firstVirtual = w->virt[0];
	w->win->obj.vt->getPos(o, &w->win->obj, pos);
	w->win->obj.vt->setPos(o, &w->win->obj, pos[0], pos[1]); // re-attach the children
	Input_Poll(o, WIN_KEY(o, w), WIN_KEY(o, w));
	Input_Poll(o, 0, w->firstVirtual->vt->sortKey(o, w->firstVirtual));
	Window_TextScreenRect(o, w->win, &r);
	Gfx_AddDirty(o->text.textGfx, WIN_KEY(o, w), &r);
	HIGHLIGHT(o, w, WaitIconSelect_HitTest(o, w));
	Wait_MarkDirty(&w->w);
}

/* draw the icons of the 16-byte records and create their hit objects
 * (hit by the bitmap's pixels with useHitMask, by the rectangle otherwise).
 * 0 ok, 0x80000001 count outside 1..64, 0x80000002 an icon bitmap is
 * invalid (the icons before it stay drawn). */
static int WaitIconSelect_Setup(OpenBGI_t* o, WaitIconSelect_t* w, int n, const IconRec_t* icons, int useHitMask)
{
	Rect_t area, r;
	int i;
	WaitIconSelect_FreeVirtuals(o, w);
	if(n < 1 || n > ICON_MAX)
		return (int)0x80000001;
	w->count = n;
	Window_ClearText(o, w->win);
	Window_GetTextArea(w->win, &area);
	for(i = 0; i < n; i++)
	{
		Bmp_t bmp;
		if(!BmpMgr_GetInfo(o->text.textBmpMgr, &bmp, icons[i].bmp))
			return (int)0x80000002;
		w->icons[i] = icons[i];
		Window_DrawBmpToText(o, w->win, &r, icons[i].x + area.l, icons[i].y + area.t, &bmp, 0, 0);
		IconSelect_AddVirtual(o, w, icons[i].x + area.l, icons[i].y + area.t, &bmp, useHitMask ? &bmp : NULL);
	}
	IconSelect_Finish(o, w);
	return 0;
}

// one virtual key per icon (pending presses are cleared); the key layer is pushed for them when mode 3 has not
static void WaitIconSelect_SetHotkeys(OpenBGI_t* o, WaitIconSelect_t* w, const int32_t* keys)
{
	int i;
	for(i = 0; i < w->count; i++)
	{
		w->hotkeys[i] = keys[i];
		GetKeyTrigger(o, keys[i]); // clear a pending press
	}
	if(!w->keyLayerPushed)
	{
		KeyLayer_Push(o, WIN_KEY(o, w));
		w->keyLayerPushed = 1;
	}
	w->hotkeysOn = 1;
}

// a "80 4C" notification with a = 0x200 ends the selection with icon b (none when b is out of range)
static void WaitIconSelect_OnMessage(OpenBGI_t* o, Wait_t* base, WaitMsg_t* m)
{
	WaitIconSelect_t* w = (WaitIconSelect_t*)base;
	if(m->a != 0x200)
		return;
	w->cancelled = 1;
	if((int32_t)m->b >= 0 && (int32_t)m->b < w->count)
	{
		w->focus = (int)m->b;
		w->clickX = w->clickY = 0;
	}
	else
		w->focus = -1;
}

/* a hot key (pressed, or a standard key in the input bits) focuses its
 * icon and ends the wait with click position (0, 0); 1 when one did.
 * Nothing while the window's key layer is not on top. */
static int WaitIconSelect_CheckHotkeys(OpenBGI_t* o, WaitIconSelect_t* w)
{
	int i;
	if(!KeyLayer_IsTop(o, WIN_KEY(o, w)))
		return 0;
	for(i = 0; i < w->count; i++)
	{
		if(GetKeyTrigger(o, w->hotkeys[i]) || StdKey_MaskBit(w->input, w->hotkeys[i]))
		{
			HIGHLIGHT(o, w, i);
			w->clickX = w->clickY = 0;
			Wait_MarkDirty(&w->w);
			return 1;
		}
	}
	return 0;
}

/* one pass with something to do: the cancel keys end the wait without a
 * selection; otherwise the icon under the mouse is highlighted (message
 * 0x20000001 reports a change: bit 16 = the icon has a focus bitmap, low
 * word = index) and a click on an icon ends the wait.  1 when it is over. */
static int WaitIconSelect_Update(OpenBGI_t* o, WaitIconSelect_t* w)
{
	int idx;
	if(w->input & 0x202)
	{
		HIGHLIGHT(o, w, -1);
		Wait_MarkDirty(&w->w);
		return 1;
	}
	idx = WaitIconSelect_HitTest(o, w);
	if(idx != w->focus)
	{
		uint32_t flag = idx != -1 && w->icons[idx].bmpFocus != -1;
		MsgQueue_Post(o, 0x20000001, Thread_GetId(w->w.thread), (flag << 16) | ((uint32_t)idx & 0xffff));
		HIGHLIGHT(o, w, idx);
		Wait_MarkDirty(&w->w);
	}
	return (w->input & 1) && w->focus != -1;
}

/* the shared poll: gather the pass's input from the window's layer (modes
 * 1 .. 3) and the hit objects' layer, without the skip bit and - unless
 * allowCancel - the cancel keys; a notification, a hot key or the update
 * (run on input, a mouse move or a focus change) ends the wait, which
 * pushes clickX, clickY and the focused icon */
static int WaitIconSelect_Poll(OpenBGI_t* o, Wait_t* base)
{
	WaitIconSelect_t* w = (WaitIconSelect_t*)base;
	uint32_t input = 0;
	WinMsgRec_t rec;
	int done, active, wasActive;
	Wait_Tick(o, base);
	if(w->mode != 0)
		input = Input_Poll(o, WIN_KEY(o, w), WIN_KEY(o, w)) & 0x7fffffff;
	input |= Input_Poll(o, 0, w->firstVirtual->vt->sortKey(o, w->firstVirtual)) & 0x7fffffff;
	if(!w->allowCancel)
		input &= ~0x202u;
	w->input = input;
	rec.arrived = 0;
	WinMsgWait_Poll(o, &rec, base->thread, 0x200);
	done = w->cancelled;
	if(!done)
	{
		if(w->hotkeysOn)
			done = WaitIconSelect_CheckHotkeys(o, w);
		wasActive = w->active;
		active = InputFocus_IsTop(o, WIN_KEY(o, w)) && SelectionAllowed(o);
		w->active = active;
		if(!done && (input != 0 || rec.arrived || active != wasActive))
			done = WaitIconSelect_Update(o, w);
	}
	Wait_Flush(o, base);
	if(!done && Wait_IsBlocking(o, base))
		return 0;
	Thread_Push(base->thread, (uint32_t)w->clickX);
	Thread_Push(base->thread, (uint32_t)w->clickY);
	Thread_Push(base->thread, Wait_IsBlocking(o, base) ? (uint32_t)w->focus : 0xffffffffu);
	return 1;
}

// one icon's focus bitmap (effect 0x40, level 1) followed by its normal bitmap
static void IconSelect_DrawFocusPair(OpenBGI_t* o, WaitIconSelect_t* w, int idx, const Rect_t* area, const Bmp_t* focusBmp,
	const Bmp_t* bmp)
{
	Rect_t r;
	int x = w->icons[idx].x + area->l, y = w->icons[idx].y + area->t;
	Window_DrawBmpToText(o, w->win, &r, x, y, focusBmp, 0x40, 1);
	Gfx_AddDirty(o->text.textGfx, WIN_KEY(o, w), &r);
	Window_DrawBmpToText(o, w->win, &r, x, y, bmp, 0, 0);
	Gfx_AddDirty(o->text.textGfx, WIN_KEY(o, w), &r);
}

/* move the focus mark from the old icon to `idx`.  The old icon gets its
 * focus bitmap punched out (effect 0x40) and its normal bitmap redrawn
 * when it has a focus bitmap; the new one the same when it has both. */
static void WaitIconSelect_Highlight(OpenBGI_t* o, WaitIconSelect_t* w, int idx)
{
	Rect_t area;
	Bmp_t focusBmp, bmp;
	Window_GetTextArea(w->win, &area);
	if(w->focus != -1 && BmpMgr_GetInfo(o->text.textBmpMgr, &focusBmp, w->icons[w->focus].bmpFocus))
	{
		Rect_t r;
		int x = w->icons[w->focus].x + area.l, y = w->icons[w->focus].y + area.t;
		Window_DrawBmpToText(o, w->win, &r, x, y, &focusBmp, 0x40, 1);
		Gfx_AddDirty(o->text.textGfx, WIN_KEY(o, w), &r);
		if(BmpMgr_GetInfo(o->text.textBmpMgr, &bmp, w->icons[w->focus].bmp))
		{
			Window_DrawBmpToText(o, w->win, &r, x, y, &bmp, 0, 0);
			Gfx_AddDirty(o->text.textGfx, WIN_KEY(o, w), &r);
		}
	}
	if(idx != -1 && BmpMgr_GetInfo(o->text.textBmpMgr, &focusBmp, w->icons[idx].bmpFocus) &&
		BmpMgr_GetInfo(o->text.textBmpMgr, &bmp, w->icons[idx].bmp))
		IconSelect_DrawFocusPair(o, w, idx, &area, &focusBmp, &bmp);
	w->focus = idx;
}

// ========================================================================
// WaitIconSelect2
// ========================================================================

static void WaitIconSelect2_Ctor(OpenBGI_t* o, WaitIconSelect2_t* w, Thread_t* t, Window_t* win, int mode)
{
	WaitIconSelect_Ctor(o, &w->s, t, win, mode);
	w->s.w.vt = &WaitIconSelect2_Vtbl.base;
	memset(&w->spill, 0, sizeof w->spill);
}

static void WaitIconSelect2_Destroy(OpenBGI_t* o, Wait_t* w)
{
	WaitIconSelect_Dtor(o, (WaitIconSelect_t*)w);
	BGI_Free(w);
}

/* as WaitIconSelect_Setup for the 0x40-byte records (same codes); the
 * icon table gets the bitmap as its focus bitmap too, the hit mask comes
 * from the record (its own bitmap when -1, none when the mask bitmap is
 * unknown) */
static int WaitIconSelect2_Setup(OpenBGI_t* o, WaitIconSelect2_t* w, int n, const IconRec2_t* recs, int useHitMask)
{
	WaitIconSelect_t* s = &w->s;
	Rect_t area, r;
	int i;
	WaitIconSelect_FreeVirtuals(o, s);
	if(n < 1 || n > ICON_MAX)
		return (int)0x80000001;
	s->count = n;
	Window_ClearText(o, s->win);
	Window_GetTextArea(s->win, &area);
	for(i = 0; i < n; i++)
	{
		Bmp_t bmp, mask;
		const Bmp_t* hit = NULL;
		if(!BmpMgr_GetInfo(o->text.textBmpMgr, &bmp, recs[i].bmp))
			return (int)0x80000002;
		s->icons[i].x = recs[i].x;
		s->icons[i].y = recs[i].y;
		s->icons[i].bmp = recs[i].bmp;
		s->icons[i].bmpFocus = recs[i].bmp;
		w->recs[i] = recs[i];
		Window_DrawBmpToText(o, s->win, &r, recs[i].x + area.l, recs[i].y + area.t, &bmp, 0, 0);
		if(useHitMask)
		{
			if(recs[i].hitMask == -1)
				hit = &bmp;
			else if(BmpMgr_GetInfo(o->text.textBmpMgr, &mask, recs[i].hitMask))
				hit = &mask;
		}
		IconSelect_AddVirtual(o, s, recs[i].x + area.l, recs[i].y + area.t, &bmp, hit);
	}
	IconSelect_Finish(o, s);
	return 0;
}

/* the focus shows through the window's item layers: layer j gets the j-th
 * (x, y, bitmap) triple of the record (eight layers are filled although a
 * record holds four triples - the last four read the following record, or
 * the zeroed `spill` after the last one); -1 switches them all off */
static void WaitIconSelect2_Highlight(OpenBGI_t* o, WaitIconSelect_t* s, int idx)
{
	WaitIconSelect2_t* w = (WaitIconSelect2_t*)s;
	Rect_t r, area;
	int j;
	for(j = 0; j < 8; j++)
		if(Window_ItemRect(o, s->win, &r, j))
			Gfx_AddDirty(o->text.textGfx, WIN_KEY(o, s), &r);
	if(idx == -1)
	{
		Window_ItemsDisableAll(o, s->win);
		s->focus = idx;
		return;
	}
	for(j = 0; j < 8; j++)
	{
		const int32_t* triple = &w->recs[idx].items[0][0] + j * 3;
		Bmp_t bmp;
		if(BmpMgr_GetInfo(o->text.textBmpMgr, &bmp, triple[2]))
		{
			Window_GetTextArea(s->win, &area);
			Window_ItemSetBitmap(o, s->win, j, &bmp);
			Window_ItemSet(o, s->win, j, triple[0] + area.l, triple[1] + area.t, 0);
			Window_ItemEnable(o, s->win, j, 1);
			Window_ItemRect(o, s->win, &r, j);
			Gfx_AddDirty(o->text.textGfx, WIN_KEY(o, s), &r);
		}
		else
			Window_ItemEnable(o, s->win, j, 0);
	}
	s->focus = idx;
}

// ========================================================================
// the starters and the draw-only form
// ========================================================================

/* "90 B0": the selection over n 16-byte records (IconRec_t); hotkeys is
 * an array of n virtual keys or NULL.  0 ok, 0x80000001 n outside 1..64,
 * 0x80000002 mode outside 0..3, -1 no such window, 0x80000003 an icon
 * bitmap is invalid. */
int StartIconSelect(OpenBGI_t* o, Thread_t* t, uint32_t h, int n, const void* icons, const int32_t* hotkeys, int mode,
	int allowCancel, int useHitMask)
{
	Window_t* win;
	WaitIconSelect_t* w;
	if(n < 1 || n > ICON_MAX)
		return (int)0x80000001;
	if((uint32_t)mode > 3)
		return (int)0x80000002;
	win = (Window_t*)Gfx_FindWindow(o->gfx.gfx, h);
	if(!win)
		return -1;
	w = (WaitIconSelect_t*)BGI_Alloc(sizeof *w);
	WaitIconSelect_Ctor(o, w, t, win, mode);
	WaitIconSelect_SetAllowCancel(w, allowCancel);
	if(WaitIconSelect_Setup(o, w, n, (const IconRec_t*)icons, useHitMask) != 0)
	{
		Wait_Release(o, &w->w); // (the original leaks the object)
		return (int)0x80000003;
	}
	if(hotkeys)
		WaitIconSelect_SetHotkeys(o, w, hotkeys);
	Thread_SetWait(o, t, &w->w);
	return 0;
}

// "90 B1": as StartIconSelect over n 0x40-byte records (IconRec2_t); the same codes
int StartIconSelect2(OpenBGI_t* o, Thread_t* t, uint32_t h, int n, const void* icons, const int32_t* hotkeys, int mode,
	int allowCancel, int useHitMask)
{
	Window_t* win;
	WaitIconSelect2_t* w;
	if(n < 1 || n > ICON_MAX)
		return (int)0x80000001;
	if((uint32_t)mode > 3)
		return (int)0x80000002;
	win = (Window_t*)Gfx_FindWindow(o->gfx.gfx, h);
	if(!win)
		return -1;
	w = (WaitIconSelect2_t*)BGI_Alloc(sizeof *w);
	WaitIconSelect2_Ctor(o, w, t, win, mode);
	WaitIconSelect_SetAllowCancel(&w->s, allowCancel);
	if(WaitIconSelect2_Setup(o, w, n, (const IconRec2_t*)icons, useHitMask) != 0)
	{
		Wait_Release(o, &w->s.w); // (the original leaks the object)
		return (int)0x80000003;
	}
	if(hotkeys)
		WaitIconSelect_SetHotkeys(o, &w->s, hotkeys);
	Thread_SetWait(o, t, &w->s.w);
	return 0;
}

// the icons' bitmaps into the cleared text layer (an unknown bitmap is skipped)
static void IconDraw_Paint(OpenBGI_t* o, Window_t* win, int n, const IconRec_t* icons)
{
	Rect_t area, r;
	int i;
	Window_ClearText(o, win);
	Window_GetTextArea(win, &area);
	for(i = 0; i < n; i++)
	{
		Bmp_t bmp;
		if(BmpMgr_GetInfo(o->text.textBmpMgr, &bmp, icons[i].bmp))
			Window_DrawBmpToText(o, win, &r, icons[i].x + area.l, icons[i].y + area.t, &bmp, 0, 0);
	}
	Window_TextScreenRect(o, win, &r);
	Gfx_AddDirty(o->text.textGfx, win->obj.vt->sortKey(o, &win->obj), &r);
}

/* "90 B4" / "90 B5": draw n icons (16-byte records; "90 B5" reduces its
 * 0x40-byte records to these first) and show the text layer, without
 * waiting.  0 ok, 0x80000001 n outside 1..64, -1 no such window. */
int Icon_Draw(OpenBGI_t* o, uint32_t h, int n, const void* icons)
{
	Window_t* win;
	if(n < 1 || n > ICON_MAX)
		return (int)0x80000001;
	win = (Window_t*)Gfx_FindWindow(o->gfx.gfx, h);
	if(!win)
		return -1;
	IconDraw_Paint(o, win, n, (const IconRec_t*)icons);
	Window_SetTextLevel(win, 0);
	Window_ShowText(win, 1);
	Window_RecompositeAll(o, win);
	Window_ItemsDisableAll(o, win);
	return 0;
}

// the name of a wait class of this file, for the debugger (NULL: not one of these)
// a click at (x, y) is on icon d->part: through the selection's layers (the window's, the icons'), on the icon
static int ReachIcon(OpenBGI_t* o, const TargetDesc_t* d, int x, int y)
{
	const WaitIconSelect_t* w = (const WaitIconSelect_t*)d->ctx;
	int layer = MouseLayer_HitAt(o, w->firstVirtual->vt->sortKey(o, w->firstVirtual), x, y);
	if(!layer && w->mode != 0)
		layer = MouseLayer_HitAt(o, WIN_KEY(o, w), x, y);
	return layer && WaitIconSelect_HitAt(o, w, x, y, NULL) == d->part;
}

/* The targets of the icon selections (bgi/inputmap.h): each visible
 * icon, taking a click while the selection's window holds the input
 * focus (and, with "90 AF", the engine's window is the active one); mode
 * 3 claims the window's layer.  1 when `w` is an icon selection. */
int WaitIcon_Targets(OpenBGI_t* o, Wait_t* base, TargetSink_t* s)
{
	const WaitIconSelect_t* w = (const WaitIconSelect_t*)base;
	uint32_t off;
	int i;
	if(WaitIcon_ClassName(base->vt) == NULL)
		return 0;
	if(w->mode == 3)
		TargetSink_Claim(s, WIN_KEY(o, w));
	if(!w->firstVirtual)
		return 1;
	off = InputFocus_IsTop(o, WIN_KEY(o, w)) && SelectionAllowed(o) ? 0 : OPENBGI_TARGET_DISABLED;
	for(i = 0; i < w->count && i < w->virtCount; i++)
	{
		DispObj_t* v = w->virt[i];
		TargetDesc_t d;
		if(!v->vt->isVisible(o, v))
			continue;
		TargetDesc_Init(&d, OPENBGI_TARGET_ICON, v->vt->sortKey(o, v));
		v->vt->screenRect(o, v, &d.rect);
		d.flags = off;
		d.owner = Thread_GetId(base->thread);
		d.index = i;
		d.reach = ReachIcon;
		d.ctx = w;
		d.part = i;
		TargetSink_Add(s, &d);
	}
	return 1;
}

const char* WaitIcon_ClassName(const WaitVtbl_t* vt)
{
	if(vt == &WaitIconSelect_Vtbl.base)
		return "WaitIconSelect";
	if(vt == &WaitIconSelect2_Vtbl.base)
		return "WaitIconSelect2";
	return NULL;
}
