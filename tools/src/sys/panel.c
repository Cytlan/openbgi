/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * panel.c - the button panel (the "90 BA" controller) and the "90 B6"
 *           painter; the interface is inc/bgi/panel.h, which describes the
 *           record layout, and docs/panel_system.md the system
 *
 * ButtonPanel_Start copies the group / button tables once more into the
 * panel, paints every button into the window's text layer and gives each
 * one a virtual display object (a child of the window) that carries its hit
 * mask and sits on the mouse layer stack.  From then on the per-pass
 * ButtonPanel_Update tracks the pointer (focus), the keyboard (the
 * selection moves within the current group's grid, groups change with Tab)
 * and the decisions, and reports every change through the event queue the
 * script drains with "90 BF":
 *
 *   0x10000001 (group, index)                  the button under the pointer changed
 *   0x10000002 (group<<16|index, hasFocusBmp)  the focus changed
 *   0x10000003 (group, direction)              the current group changed
 *   0x10000004 (group<<16|index, direction)    the selection changed
 *   0x10000005 (group<<16|index, direction)    the cursor hit the grid's edge
 *
 * The "direction" is the key's: -1 / 1 previous / next, 0xffff / 1 left /
 * right, 0xffff0000 / 0x10000 up / down, 0 for the mouse.  A group or index
 * of -1 is sent as 0xffffffff.
 *
 * The sprite panel (sprpanel.c) derives from this class through the
 * PanelVtbl_t hooks; the functions that are not static here are the ones it
 * shares.
 */
#include <string.h>
#include "bgi/panel.h"
#include "bgi/gfx.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/window.h"
#include "bgi/gfx/objects.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/input.h"
#include "bgi/sys.h"
#include "bgi/vm.h"

// the window's sort key: the layer key the panel uses on the input layer stacks
#define WIN_KEY(p) ((p)->win->obj.vt->sortKey(&(p)->win->obj))

static void ButtonPanel_Destroy(ObjProc_t* p);
static void ButtonPanel_Animate(ButtonPanel_t* p);
static int ButtonPanel_FinishOnDecide(ButtonPanel_t* p);
static int ButtonPanel_SetFocus(ButtonPanel_t* p, int btn, int force);
static int ButtonPanel_GetBitmap(ButtonPanel_t* p, int32_t* out, int g, int i, int focus, int sel);
static int ButtonPanel_SetSelection(ButtonPanel_t* p, int g, int i);
static int ButtonPanel_CanDecide(ButtonPanel_t* p, int g, int i);
static void ButtonPanel_ClickOutside(ButtonPanel_t* p);

static const PanelVtbl_t ButtonPanel_Vtbl = {
	{ButtonPanel_Destroy, ButtonPanel_Poll, ButtonPanel_OnMessage}, ButtonPanel_Animate, ButtonPanel_FinishOnDecide,
	ButtonPanel_SetFocus, ButtonPanel_GetBitmap, ButtonPanel_SetSelection, ButtonPanel_CanDecide,
	ButtonPanel_DecideAt, ButtonPanel_Decide, ButtonPanel_Stop, ButtonPanel_ClickOutside};

// ========================================================================
// construction
// ========================================================================

/* Construct a panel on `win`: it takes the window's proc slot (kind 0x80)
 * and attaches a size-less virtual child whose layer key stands for "the
 * window" on the mouse layer stack.  The panel is idle until
 * ButtonPanel_Start. */
void ButtonPanel_Ctor(ButtonPanel_t* p, Window_t* win)
{
	VirtualObject_t* v;
	ObjProc_Ctor(&p->p, 0x80, &win->obj);
	p->p.vt = &ButtonPanel_Vtbl.base;
	p->isSprite = 0;
	Panel_InitKeyMaps();
	p->started = 0;
	p->win = win;
	v = (VirtualObject_t*)BGI_Alloc(sizeof *v);
	VirtualObject_Ctor(v, 0, &win->obj);
	p->virt = &v->obj;
	DispObj_SetVirtualSize(p->virt, 0, 0);
	DispObj_AttachChild(&win->obj, p->virt, 0, 0);
	p->currentGroup = -1;
	p->focus = -1;
	p->groupCount = 0;
	p->groups = NULL;
	p->modal = p->mouse = 0;
	p->inputMask = 0;
	p->keyMap = 0;
	p->selectOnDecide = 0;
	p->grid = NULL;
	p->total = 0;
	p->buttons = NULL;
	p->inputEnabled = 1;
	p->evHead.msg = p->evHead.a = p->evHead.b = 0;
	p->evHead.next = NULL;
	// the original leaves the decision, click and pointer fields uninitialised until Start;
	// they are zeroed here so that "90 BC" on a panel that was never started reads defined values
	p->clickX = p->clickY = 0;
	p->decidedGroup = p->decidedIndex = p->decidedFlag = 0;
	p->lastMouse[0] = p->lastMouse[1] = 0;
	p->hover = 0;
	p->input = 0;
}

ButtonPanel_t* ButtonPanel_New(Window_t* win)
{
	ButtonPanel_t* p = (ButtonPanel_t*)BGI_Alloc(sizeof *p);
	ButtonPanel_Ctor(p, win);
	return p;
}

// stop the panel, drop its virtual child and the pending events, leave the proc slot
void ButtonPanel_Dtor(ButtonPanel_t* p)
{
	p->p.vt = &ButtonPanel_Vtbl.base; // the base class' hooks from here on, as a C++ destructor would
	ButtonPanel_Stop(p);
	DispObj_DetachChild(&p->win->obj, p->virt);
	if(p->virt)
		p->virt->vt->destroy(p->virt, 1);
	while(ButtonPanel_DropEvent(p))
		;
	ObjProc_Dtor(&p->p);
}

static void ButtonPanel_Destroy(ObjProc_t* p)
{
	ButtonPanel_Dtor((ButtonPanel_t*)p);
	BGI_Free(p);
}

// ========================================================================
// the event queue
// ========================================================================

// append an event (one of the 0x1000000n messages of the file header) to the queue
void ButtonPanel_PushEvent(ButtonPanel_t* p, uint32_t msg, uint32_t a, uint32_t b)
{
	PanelEvent_t* tail = &p->evHead;
	PanelEvent_t* e;
	while(tail->next)
		tail = tail->next;
	e = (PanelEvent_t*)BGI_Alloc(sizeof *e);
	e->msg = msg;
	e->a = a;
	e->b = b;
	e->next = NULL;
	tail->next = e;
	if(getenv("BGI_PANEL_DEBUG"))
		fprintf(stderr, "panel %p event %08x %08x %08x\n", (void*)p, msg, a, b);
}

// discard the oldest event; 0 when the queue was empty
int ButtonPanel_DropEvent(ButtonPanel_t* p)
{
	PanelEvent_t* e = p->evHead.next;
	if(!e)
		return 0;
	p->evHead.next = e->next;
	BGI_Free(e);
	return 1;
}

/* "90 BF": take the oldest event as (msg, a, b) into `out`; 1 when there
 * was one, 0 (and three zeros) when the queue is empty */
int ButtonPanel_PopEvent(ButtonPanel_t* p, uint32_t out[3])
{
	PanelEvent_t* e = p->evHead.next;
	if(!e)
	{
		out[0] = out[1] = out[2] = 0;
		return 0;
	}
	out[0] = e->msg;
	out[1] = e->a;
	out[2] = e->b;
	ButtonPanel_DropEvent(p);
	return 1;
}

// ========================================================================
// queries
// ========================================================================

/* "90 BC": the panel's state as six words: started, the decided group,
 * index and flag (-1, -1 for a cancel), and the click offset inside the
 * decided button */
void ButtonPanel_GetState(ButtonPanel_t* p, int32_t out[6])
{
	out[0] = p->started;
	out[1] = p->decidedGroup;
	out[2] = p->decidedIndex;
	out[3] = p->decidedFlag;
	out[4] = p->clickX;
	out[5] = p->clickY;
}

// "90 BE": the selected index of every group (-1 none) into `out`; the group count
int ButtonPanel_GetSelections(ButtonPanel_t* p, int32_t* out)
{
	int g;
	for(g = 0; g < p->groupCount; g++)
		out[g] = p->groups[g].selected;
	return p->groupCount;
}

// ========================================================================
// the record copies
// ========================================================================

// free a copy made by Panel_CopyDesc (also a partial one)
void Panel_FreeDesc(PanelDesc_t* d)
{
	if(d->groups)
	{
		int g;
		for(g = 0; g < d->groupCount; g++)
			BGI_Free(d->groups[g].buttons);
		BGI_Free(d->groups);
	}
	BGI_Free(d);
}

/* Make a private copy of the three-level table (descriptor, groups,
 * buttons) that the script handed over, with the tagged pointers resolved
 * into host pointers (`t` is the thread whose memory they refer to).
 * Returns 0 and the copy in *out, or 2 when the group table is unusable
 * (null pointer or a group count outside 1..0x100), 3 when a button table
 * is (null pointer or a count outside 1..0x100); *out is NULL then and the
 * partial copy has been freed.  The original keeps the script layout and
 * overwrites the pointer fields in place; here the copy is the host-side
 * type of panel.h.  The caller frees the copy with Panel_FreeDesc. */
int Panel_CopyDesc(PanelDesc_t** out, const PanelDescScript_t* d, Thread_t* t)
{
	PanelDesc_t* c = (PanelDesc_t*)BGI_Alloc(sizeof *c);
	const PanelGroupScript_t* src;
	PanelGroup_t* groups;
	int status = 0, g;
	c->groupCount = d->groupCount;
	c->groups = NULL;
	c->currentGroup = d->currentGroup;
	c->modal = d->modal;
	c->mouse = d->mouse;
	c->inputMask = d->inputMask;
	c->keyMap = d->keyMap;
	c->selectOnDecide = d->selectOnDecide;
	src = (const PanelGroupScript_t*)ResolvePtr(d->groups, t);
	if(!src || d->groupCount <= 0 || d->groupCount > 0x100)
	{
		Panel_FreeDesc(c);
		*out = NULL;
		return 2;
	}
	groups = (PanelGroup_t*)BGI_Calloc((size_t)d->groupCount * sizeof *groups);
	c->groups = groups;
	for(g = 0; g < d->groupCount; g++)
	{
		const PanelButton_t* buttons;
		int count;
		if(status) // after a failure the remaining groups keep their NULL button tables, for Panel_FreeDesc
			continue;
		groups[g].count = src[g].count;
		groups[g].columns = src[g].columns;
		groups[g].selected = src[g].selected;
		groups[g].selectable = src[g].selectable;
		groups[g].hoverSelects = src[g].hoverSelects;
		groups[g].clickDecides = src[g].clickDecides;
		groups[g].linkId = src[g].linkId;
		memcpy(groups[g].ov, src[g].ov, sizeof groups[g].ov);
		buttons = (const PanelButton_t*)ResolvePtr(src[g].buttons, t);
		count = src[g].count;
		if(!buttons || count <= 0 || count > 0x100)
		{
			status = 3;
			continue;
		}
		groups[g].buttons = (PanelButton_t*)BGI_Alloc((size_t)count * sizeof *buttons);
		memcpy(groups[g].buttons, buttons, (size_t)count * sizeof *buttons);
	}
	if(status)
	{
		Panel_FreeDesc(c);
		*out = NULL;
		return status;
	}
	*out = c;
	return 0;
}

// the tables of a copied descriptor / group
#define GROUPS_OF(d)  ((d)->groups)
#define BUTTONS_OF(g) ((g)->buttons)

/* the bitmap number button `i` shows at start: its selected picture when
 * it is the group's selection (`selected`) and has one, else the normal one
 * (-1 when it has none) */
static int StartBitmap(const PanelButton_t* b, int i, int selected)
{
	int no = b->bmpNormal;
	if(no != -1 && i == selected && b->bmpSelected != -1)
		no = b->bmpSelected;
	return no;
}

/* the window reset shared by the starters and painters: all items off, no
 * sub-sprites, the text layer cleared, at level 0 and shown when `showText`
 * is set, then recomposited */
static void Panel_ResetWindow(Window_t* w, int showText)
{
	Window_ItemsDisableAll(w);
	Window_SubAlloc(w, 0);
	Window_ClearText(w);
	Window_SetTextLevel(w, 0);
	Window_ShowText(w, showText);
	Window_RecompositeAll(w);
}

/* "90 B6": paint the enabled buttons of a copied table into the text layer
 * of `w` without creating a panel; each shows its start picture
 * (StartBitmap) at its position relative to the text area.  Returns 0, or
 * 0x80000001 for a group count outside 1..0x100, 0x80000002 for a button
 * count outside 1..0x100 (unreachable after Panel_CopyDesc, which rejects
 * both).  The window is reset first (Panel_ResetWindow) in every case and
 * its text rectangle is marked dirty. */
uint32_t Panel_Paint(Window_t* w, const PanelDesc_t* d)
{
	uint32_t status = 0x80000001;
	Rect_t area, r;
	int g;
	Panel_ResetWindow(w, 1);
	if(d->groupCount > 0 && d->groupCount <= 0x100)
	{
		const PanelGroup_t* groups = GROUPS_OF(d);
		int ok = 1;
		Window_GetTextArea(w, &area);
		for(g = 0; g < d->groupCount && ok; g++)
			ok = groups[g].count >= 1 && groups[g].count <= 0x100;
		if(!ok)
			status = 0x80000002;
		else
		{
			for(g = 0; g < d->groupCount; g++)
			{
				const PanelButton_t* b = BUTTONS_OF(&groups[g]);
				int i;
				for(i = 0; i < (int)groups[g].count; i++)
				{
					Bmp_t bmp;
					if(!b[i].enabled)
						continue;
					if(BmpMgr_GetInfo(gPanelBmpMgr, &bmp, StartBitmap(&b[i], i, groups[g].selected)))
						Window_DrawBmpToText(w, &r, b[i].x + area.l, b[i].y + area.t, &bmp, 0, 0);
				}
			}
			status = 0;
		}
	}
	Window_TextScreenRect(w, &r);
	Gfx_AddDirty(gPanelGfx, w->obj.vt->sortKey(&w->obj), &r);
	return status;
}

// ========================================================================
// start / stop
// ========================================================================

/* the panel's record of a button that is disabled or has no usable bitmap:
 * position 0, every bitmap number -1, no hot key.  `enabled` and
 * `unused34` are left as allocated (uninitialised), as in the original;
 * nothing reads them for such a button. */
static void PanelButton_Blank(PanelButton_t* b)
{
	b->x = b->y = 0;
	b->bmpNormal = b->bmpSelected = b->bmpFocus = b->hitMaskBmp = -1;
	b->ov[0][0] = b->ov[0][1] = 0;
	b->ov[0][2] = -1;
	b->ov[1][0] = b->ov[1][1] = 0;
	b->ov[1][2] = -1;
	b->hotkey = 0;
}

/* Allocate and fill p->grid, the {columns, rows} pair of every group for
 * the cursor-key moves: the column count clamped to 1..count, the row
 * count derived from it.  *total receives the sum of the button counts.
 * Returns 0 when a group's count is outside 1..0x100 (the caller frees the
 * grid then).  The `columns` field of the copied groups is zeroed on the
 * way: the original overwrites the 16-bit count / columns pair with the
 * 32-bit count at this point, and the field is not read again. */
static int Panel_BuildGrid(ButtonPanel_t* p, PanelGroup_t* groups, int groupCount, int* total)
{
	int g, sum = 0, ok = 1;
	p->grid = (int32_t(*)[2])BGI_Alloc((size_t)groupCount * 8);
	for(g = 0; g < groupCount && ok; g++)
	{
		int count = groups[g].count, cols = groups[g].columns;
		if(cols > count)
			cols = count;
		else if(cols < 1)
			cols = 1;
		p->grid[g][0] = cols;
		p->grid[g][1] = (count + cols - 1) / cols;
		groups[g].columns = 0; // see above: the original's 32-bit count overwrites the columns here
		sum += count;
		ok = count > 0 && count <= 0x100;
	}
	*total = sum;
	return ok;
}

/* Create the virtual hit object of one button: a child of the window at
 * (x, y) with the size of `bmp`, as visible as the window, pushed on the
 * mouse layer stack.  `maskBmp` decides how it is hit-tested: -2 never, a
 * known bitmap number by that bitmap's shape, anything else by the
 * rectangle.  The object is owned by the panel and destroyed in
 * ButtonPanel_Stop. */
DispObj_t* Panel_MakeHitObject(ButtonPanel_t* p, int x, int y, const Bmp_t* bmp, int maskBmp)
{
	VirtualObject_t* v = (VirtualObject_t*)BGI_Alloc(sizeof *v);
	Bmp_t mask;
	VirtualObject_Ctor(v, 0, &p->win->obj);
	DispObj_SetVirtualSize(&v->obj, bmp->w, bmp->h);
	v->obj.vt->setVisible(&v->obj, p->win->obj.vt->isVisible(&p->win->obj));
	DispObj_AttachChild(&p->win->obj, &v->obj, x, y);
	if(maskBmp == -2)
		DispObj_DisableHit(&v->obj);
	else if(BmpMgr_GetInfo(gPanelBmpMgr, &mask, maskBmp))
		DispObj_BuildHitMask(&v->obj, &mask);
	MouseLayer_PushSprite(&v->obj);
	return &v->obj;
}

/* The common tail of ButtonPanel_Start and SpritePanel_Start once the
 * buttons exist: re-set the window position (which places the new
 * children), claim the input layers (a modal panel pushes its own mouse and
 * key layers and takes the input focus, a `mouse` panel joins the mouse
 * layer stack with the window), flush the pending input, report the
 * initial focus (event 0x10000002) and prime the hover state.  With
 * `selectedAtStart` (the current group has a selection) the pointer
 * position is recorded so that hover selection waits for a movement; else
 * it is set to an impossible value and the first pass selects at once. */
void ButtonPanel_FinishStart(ButtonPanel_t* p, int selectedAtStart)
{
	int32_t pos[2];
	p->win->obj.vt->getPos(&p->win->obj, pos);
	p->win->obj.vt->setPos(&p->win->obj, pos[0], pos[1]);
	if(p->modal)
	{
		MouseLayer_Push(WIN_KEY(p));
		KeyLayer_Push(WIN_KEY(p));
		InputFocus_Add(p->p.id | 0x80000000u, WIN_KEY(p));
		ButtonPanel_SetCurrentGroup(p, p->currentGroup);
	}
	else if(p->mouse)
		MouseLayer_PushSprite(&p->win->obj);
	Input_Poll(WIN_KEY(p), WIN_KEY(p));
	Input_Poll(0, p->virt->vt->sortKey(p->virt));
	if(PANEL_VT(p)->setFocus(p, ButtonPanel_HitTestStore(p), 1))
	{
		if(p->focus != -1)
		{
			const PanelInfo_t* f = &p->buttons[p->focus];
			ButtonPanel_PushEvent(p, 0x10000002, ((uint32_t)f->group << 16) | (uint32_t)f->index,
				f->button->bmpFocus != -1);
		}
		else
			ButtonPanel_PushEvent(p, 0x10000002, 0xffffffffu, 0);
	}
	// hover selection only reacts to pointer movement away from where it starts
	// (0x7fffffff never matches a real position, so the first pass counts as a move)
	if(selectedAtStart)
		GetMouseClientPos(p->lastMouse);
	else
		p->lastMouse[0] = p->lastMouse[1] = 0x7fffffff;
	p->hover = ButtonPanel_HitTest(p, NULL, 0);
	p->started = 1;
}

/* "90 BA": start (or restart) the panel from a copied table.  A running
 * panel is stopped first and the window reset.  The groups and buttons are
 * copied into the panel's own tables (the descriptor's current group and
 * the groups' selections are checked against their ranges, -1 when
 * outside), the buttons are drawn as Panel_Paint draws them, and each
 * enabled button with a bitmap gets its hit object; its hot key's pending
 * presses are swallowed so that a key held from before does not decide at
 * once.  Returns 0, or 0x80000001 for a group count outside 1..0x100,
 * 0x80000002 for a button count outside 1..0x100 (the window stays reset
 * then).  The copy `d` is not kept; the caller frees it. */
uint32_t ButtonPanel_Start(ButtonPanel_t* p, const PanelDesc_t* d)
{
	uint32_t status = 0x80000001;
	Rect_t area, r;
	PanelGroup_t* src;
	PanelInfo_t* info;
	int g, total, startSel = 0;
	PANEL_VT(p)->stop(p);
	Panel_ResetWindow(p->win, 1);
	if(d->groupCount <= 0 || d->groupCount > 0x100)
		goto done;
	Window_GetTextArea(p->win, &area);
	src = GROUPS_OF(d);
	if(!Panel_BuildGrid(p, src, d->groupCount, &total))
	{
		BGI_Free(p->grid);
		status = 0x80000002;
		goto done;
	}
	p->groupCount = d->groupCount;
	p->groups = (PanelGroup_t*)BGI_Alloc((size_t)d->groupCount * sizeof *p->groups);
	p->currentGroup = (d->currentGroup >= 0 && d->currentGroup < d->groupCount) ? d->currentGroup : -1;
	p->modal = d->modal;
	p->mouse = d->mouse;
	p->inputMask = d->inputMask;
	p->keyMap = d->keyMap & 7; // 0..3 the built-in maps, 4..7 the user maps of "91 BF"
	p->selectOnDecide = d->selectOnDecide;
	p->total = total;
	p->buttons = (PanelInfo_t*)BGI_Alloc((size_t)total * sizeof *p->buttons);
	info = p->buttons;
	for(g = 0; g < d->groupCount; g++)
	{
		const PanelGroup_t* sg = &src[g];
		const PanelButton_t* sb = BUTTONS_OF(sg);
		PanelGroup_t* dg = &p->groups[g];
		PanelButton_t* db;
		int count = sg->count, i;
		dg->count = count;
		dg->columns = sg->columns;
		db = (PanelButton_t*)BGI_Alloc((size_t)count * sizeof *db);
		dg->buttons = db;
		dg->selected = (sg->selected >= 0 && sg->selected < count) ? sg->selected : -1;
		dg->selectable = sg->selectable;
		dg->hoverSelects = sg->hoverSelects;
		dg->clickDecides = sg->clickDecides;
		dg->linkId = sg->linkId;
		memcpy(dg->ov, sg->ov, sizeof dg->ov);
		for(i = 0; i < count; i++, info++)
		{
			Bmp_t bmp;
			int ok = sb[i].enabled && BmpMgr_GetInfo(gPanelBmpMgr, &bmp, StartBitmap(&sb[i], i, dg->selected));
			info->hasBitmap = ok;
			info->group = g;
			info->index = i;
			info->button = &db[i];
			info->virt = NULL;
			if(!ok)
			{
				PanelButton_Blank(&db[i]);
				continue;
			}
			db[i] = sb[i];
			Window_DrawBmpToText(p->win, &r, db[i].x + area.l, db[i].y + area.t, &bmp, 0, 0);
			info->virt = Panel_MakeHitObject(p, db[i].x + area.l, db[i].y + area.t, &bmp, sb[i].hitMaskBmp);
			if(sb[i].hotkey)
				GetKeyTrigger((int)(sb[i].hotkey & 0x7fffffff)); // consume the pending presses of the hot key
		}
	}
	// the descriptor's own current group and selection decide, not the range-checked copies
	if(d->currentGroup != -1 && src[d->currentGroup].selected != -1)
		startSel = 1;
	ButtonPanel_FinishStart(p, startSel);
	status = 0;
done:
	Window_TextScreenRect(p->win, &r);
	Gfx_AddDirty(gPanelGfx, WIN_KEY(p), &r);
	ObjProc_MarkDirty(&p->p);
	return status;
}

/* Stop the panel: flush the input of its layers, destroy the hit objects,
 * free the group, button and grid tables, give the input layers (and the
 * input focus of a modal panel) back and drop the pending events.  The
 * window's pixels are left as they are.  Safe on a panel that is not
 * running. */
void ButtonPanel_Stop(ButtonPanel_t* p)
{
	int g, k = 0;
	p->started = 0;
	Input_Poll(WIN_KEY(p), WIN_KEY(p));
	Input_Poll(0, p->virt->vt->sortKey(p->virt));
	for(g = 0; g < p->groupCount; g++)
	{
		int i;
		for(i = 0; i < (int)p->groups[g].count; i++, k++)
		{
			PanelInfo_t* info = &p->buttons[k];
			if(!info->hasBitmap)
				continue;
			MouseLayer_PopSprite(info->virt);
			DispObj_DetachChild(&p->win->obj, info->virt);
			if(info->virt)
				info->virt->vt->destroy(info->virt, 1);
		}
		BGI_Free(BUTTONS_OF(&p->groups[g]));
	}
	BGI_Free(p->groups);
	BGI_Free(p->buttons);
	BGI_Free(p->grid);
	if(p->modal)
	{
		MouseLayer_Pop(WIN_KEY(p));
		KeyLayer_Pop(WIN_KEY(p));
		InputFocus_Remove(p->p.id | 0x80000000u);
	}
	else if(p->mouse)
		MouseLayer_PopSprite(&p->win->obj);
	p->groupCount = 0;
	p->groups = NULL;
	p->currentGroup = -1;
	p->modal = p->mouse = 0;
	p->inputMask = 0;
	p->keyMap = 0;
	p->selectOnDecide = 0;
	p->grid = NULL;
	p->total = 0;
	p->buttons = NULL;
	p->focus = -1;
	while(ButtonPanel_DropEvent(p))
		;
}

// ========================================================================
// drawing helpers
// ========================================================================

// mark the screen rectangle of window item `item` dirty (nothing for an item without one)
void ButtonPanel_DirtyItem(ButtonPanel_t* p, int item)
{
	Rect_t r;
	if(Window_ItemRect(p->win, &r, item))
	{
		Gfx_AddDirty(gPanelGfx, WIN_KEY(p), &r);
		ObjProc_MarkDirty(&p->p);
	}
}

/* Show the two overlays `ov` (x, y, bitmap number each) of a group or a
 * button in window items firstItem and firstItem + 1, positioned relative
 * to the text area `area`; an overlay whose bitmap is unknown switches its
 * item off instead. */
void ButtonPanel_DrawOverlays(ButtonPanel_t* p, int firstItem, int32_t ov[2][3], const Rect_t* area)
{
	int k;
	for(k = 0; k < 2; k++)
	{
		Bmp_t bmp;
		Rect_t r;
		int item = firstItem + k;
		if(BmpMgr_GetInfo(gPanelBmpMgr, &bmp, ov[k][2]))
		{
			Window_ItemSetBitmap(p->win, item, &bmp);
			Window_ItemSet(p->win, item, area->l + ov[k][0], area->t + ov[k][1], 0);
			Window_ItemEnable(p->win, item, 1);
			Window_ItemRect(p->win, &r, item); // the result is not checked: the rectangle is added even for an empty item
			Gfx_AddDirty(gPanelGfx, WIN_KEY(p), &r);
		}
		else
			Window_ItemEnable(p->win, item, 0);
		ObjProc_MarkDirty(&p->p);
	}
}

/* Replace button `b`'s picture in the text layer: the old bitmap `oldNo`
 * is punched out (drawn with effect 0x40 at level 1, which clears the text
 * layer wherever the bitmap has alpha) and the new bitmap `newNo` drawn in
 * its place; both rectangles are marked dirty.  Returns 0 without drawing
 * when either bitmap number is unknown. */
static int ButtonPanel_Redraw(ButtonPanel_t* p, const PanelButton_t* b, int newNo, int oldNo)
{
	Bmp_t bmpNew, bmpOld;
	Rect_t area, r;
	int x, y;
	if(!BmpMgr_GetInfo(gPanelBmpMgr, &bmpNew, newNo))
		return 0;
	if(!BmpMgr_GetInfo(gPanelBmpMgr, &bmpOld, oldNo))
		return 0;
	Window_GetTextArea(p->win, &area);
	x = b->x + area.l;
	y = b->y + area.t;
	Window_DrawBmpToText(p->win, &r, x, y, &bmpOld, 0x40, 1);
	Gfx_AddDirty(gPanelGfx, WIN_KEY(p), &r);
	Window_DrawBmpToText(p->win, &r, x, y, &bmpNew, 0, 0);
	Gfx_AddDirty(gPanelGfx, WIN_KEY(p), &r);
	ObjProc_MarkDirty(&p->p);
	return 1;
}

/* The `getBitmap` hook: the bitmap number button (g, i) shows into *out:
 * its normal picture, the focus picture when `focus` is set and it has one,
 * the selected picture when `sel` is set, it is the group's selection and
 * it has one (the selected picture wins).  Returns 0 (and leaves *out)
 * when (g, i) is out of range. */
static int ButtonPanel_GetBitmap(ButtonPanel_t* p, int32_t* out, int g, int i, int focus, int sel)
{
	const PanelGroup_t* grp;
	const PanelButton_t* b;
	if(g >= p->groupCount)
		return 0;
	grp = &p->groups[g];
	if(i >= (int)grp->count)
		return 0;
	b = &BUTTONS_OF(grp)[i];
	*out = b->bmpNormal;
	if(focus && b->bmpFocus != -1)
		*out = b->bmpFocus;
	if(sel && i == grp->selected && b->bmpSelected != -1)
		*out = b->bmpSelected;
	return 1;
}

// is button (g, i) the focused one (the one under the pointer)?
static int IsFocused(ButtonPanel_t* p, int g, int i)
{
	return p->focus != -1 && p->buttons[p->focus].group == g && p->buttons[p->focus].index == i;
}

/* The `setFocus` hook: move the focus (the pointer mark) to flat index
 * `btn` (-1 for none).  The old button goes back to its unfocused picture;
 * the new one gets its focused picture and its overlays in window items 2
 * and 3 (which are switched off when there is no new button).  Returns 0
 * when `btn` is already the focus and `force` is clear, 1 when the focus
 * was (re)drawn. */
static int ButtonPanel_SetFocus(ButtonPanel_t* p, int btn, int force)
{
	Rect_t area;
	int32_t a, b;
	if(p->focus == btn && !force)
		return 0;
	Window_GetTextArea(p->win, &area);
	ButtonPanel_DirtyItem(p, 2);
	ButtonPanel_DirtyItem(p, 3);
	if(p->focus != -1)
	{
		const PanelInfo_t* f = &p->buttons[p->focus];
		PANEL_VT(p)->getBitmap(p, &a, f->group, f->index, 0, 1);
		PANEL_VT(p)->getBitmap(p, &b, f->group, f->index, 1, 1);
		ButtonPanel_Redraw(p, f->button, a, b);
	}
	if(btn != -1)
	{
		const PanelInfo_t* f = &p->buttons[btn];
		PANEL_VT(p)->getBitmap(p, &a, f->group, f->index, 1, 1);
		PANEL_VT(p)->getBitmap(p, &b, f->group, f->index, 0, 1);
		ButtonPanel_Redraw(p, f->button, a, b);
		ButtonPanel_DrawOverlays(p, 2, f->button->ov, &area);
	}
	else
	{
		Window_ItemEnable(p->win, 2, 0);
		Window_ItemEnable(p->win, 3, 0);
		ObjProc_MarkDirty(&p->p);
	}
	p->focus = btn;
	return 1;
}

/* Make `g` the current group (the one the keys move in): its overlays go
 * into window items 0 and 1 and the groups linked to it lose their
 * selection (ButtonPanel_ApplyLink).  Returns 0 when `g` is out of range
 * or the group is not selectable, 1 when it became current.  Also reached
 * by message 0x10000001 of "80 AC". */
int ButtonPanel_SetCurrentGroup(ButtonPanel_t* p, int g)
{
	Rect_t area;
	if(g < 0 || g >= p->groupCount)
		return 0;
	if(!p->groups[g].selectable)
		return 0;
	Window_GetTextArea(p->win, &area);
	ButtonPanel_DirtyItem(p, 0);
	ButtonPanel_DirtyItem(p, 1);
	ButtonPanel_DrawOverlays(p, 0, p->groups[g].ov, &area);
	p->currentGroup = g;
	ButtonPanel_ApplyLink(p, g);
	return 1;
}

/* The `setSelection` hook: select button `i` of group `g` (-1 clears the
 * selection).  The old selection is redrawn plain, the new one in its
 * selected picture, and the linked groups lose their selection.  Returns 1
 * when the selection changed; 0 when `g` or `i` is out of range, when `i`
 * is the selection already, or when the new button has no normal picture
 * (its index is recorded as the selection even so, but nothing is drawn
 * and the links are not applied).  Also reached by message 0x10000002 of
 * "80 AC". */
static int ButtonPanel_SetSelection(ButtonPanel_t* p, int g, int i)
{
	PanelGroup_t* grp;
	int old;
	int32_t a, b;
	if(g < 0 || g >= p->groupCount)
		return 0;
	grp = &p->groups[g];
	old = grp->selected;
	if(!(i >= 0 && i < (int)grp->count) && i != -1)
		return 0;
	if(i == old)
		return 0;
	if(old != -1)
	{
		int f = IsFocused(p, g, old);
		PANEL_VT(p)->getBitmap(p, &a, g, old, f, 0);
		PANEL_VT(p)->getBitmap(p, &b, g, old, f, 1);
		ButtonPanel_Redraw(p, &BUTTONS_OF(grp)[old], a, b);
	}
	grp->selected = i;
	if(i == -1)
		return 1;
	if(BUTTONS_OF(grp)[i].bmpNormal == -1)
		return 0;
	{
		int f = IsFocused(p, g, i);
		PANEL_VT(p)->getBitmap(p, &a, g, i, f, 1);
		PANEL_VT(p)->getBitmap(p, &b, g, i, f, 0);
		ButtonPanel_Redraw(p, &BUTTONS_OF(grp)[i], a, b);
	}
	ButtonPanel_ApplyLink(p, g);
	return 1;
}

/* Apply group `g`'s link: when it has a selection and a link id (not -1),
 * every other group with the same id loses its selection, so that linked
 * groups behave like one set of radio buttons.  Returns 0 for a group out
 * of range, else 1. */
int ButtonPanel_ApplyLink(ButtonPanel_t* p, int g)
{
	int link, h;
	if(g < 0 || g >= p->groupCount)
		return 0;
	if(p->groups[g].selected == -1)
		return 1;
	link = p->groups[g].linkId;
	if(link == -1)
		return 1;
	for(h = 0; h < p->groupCount; h++)
		if(h != g && p->groups[h].linkId == link)
			PANEL_VT(p)->setSelection(p, h, -1);
	return 1;
}

// ========================================================================
// input
// ========================================================================

/* The flat index of the first drawn button whose hit object is under the
 * pointer, -1 for none.  -1 also while the panel's input is disabled, while
 * the window does not hold the input focus, or while panels are not
 * allowed at all (Panel_Allowed: the main window is inactive).  Without
 * `useMask` the object's screen rectangle decides, with it the object's
 * own hit test (the mask bitmap) as well.  `outOffset` (may be NULL)
 * receives the pointer position relative to the button's top left. */
int ButtonPanel_HitTest(ButtonPanel_t* p, int32_t* outOffset, int useMask)
{
	int m[2], k;
	if(!p->inputEnabled)
		return -1;
	if(!InputFocus_IsTop(WIN_KEY(p)))
		return -1;
	if(!Panel_Allowed())
		return -1;
	GetMouseClientPos(m);
	for(k = 0; k < p->total; k++)
	{
		const PanelInfo_t* info = &p->buttons[k];
		Rect_t r;
		if(!info->hasBitmap)
			continue;
		info->virt->vt->screenRect(info->virt, &r);
		if(m[0] < r.l || m[0] > r.r || m[1] < r.t || m[1] > r.b)
			continue;
		if(useMask && !info->virt->vt->hitTest(info->virt, m[0] - r.l, m[1] - r.t))
			continue;
		if(outOffset)
		{
			outOffset[0] = m[0] - r.l;
			outOffset[1] = m[1] - r.t;
		}
		return k;
	}
	return -1;
}

/* the masked hit test that also records the pointer offset inside the hit
 * button as the click position "90 BC" reports (-1, -1 for a miss) */
int ButtonPanel_HitTestStore(ButtonPanel_t* p)
{
	int32_t off[2];
	int hit;
	p->clickX = p->clickY = -1;
	hit = ButtonPanel_HitTest(p, off, 1);
	if(hit != -1)
	{
		p->clickX = off[0];
		p->clickY = off[1];
	}
	return hit;
}

/* The flat index of the first drawn button whose hot key is pressed: a
 * fresh press of the virtual key, a standard-key group of this pass's
 * input that contains it, or - when bit 31 of the hot key is set - the key
 * held past the auto-repeat delay.  -1 when none is.  Only a modal panel
 * whose window holds the input focus and owns the top key layer sees the
 * keys. */
int ButtonPanel_CheckHotkeys(ButtonPanel_t* p)
{
	int k;
	if(!p->modal)
		return -1;
	if(!InputFocus_IsTop(WIN_KEY(p)))
		return -1;
	for(k = 0; k < p->total; k++)
	{
		const PanelInfo_t* info = &p->buttons[k];
		uint32_t hk;
		int vk, held = 0;
		if(!KeyLayer_IsTop(WIN_KEY(p)))
			continue;
		if(!info->hasBitmap)
			continue;
		hk = info->button->hotkey;
		vk = (int)(hk & 0x7fffffff);
		if(!vk)
			continue;
		if(hk & 0x80000000u)
			held = Key_IsRepeating(vk);
		if(GetKeyTrigger(vk) || StdKey_MaskBit(p->input, vk) || held)
			return k;
	}
	return -1;
}

/* The `decide` hook: record a decision on the button with flat index `btn`
 * (-1 for a cancel: group and index -1) with `flag`, which
 * ButtonPanel_Update sets to 1 for the mouse and 0 for the keyboard.
 * Returns 0 for an index out of range, 1 when recorded; "90 BC" reads the
 * decision. */
int ButtonPanel_Decide(ButtonPanel_t* p, int btn, int flag)
{
	if(btn != -1 && (btn < 0 || btn >= p->total))
		return 0;
	if(btn == -1)
	{
		p->decidedGroup = p->decidedIndex = -1;
		p->decidedFlag = flag;
		return 1;
	}
	p->decidedGroup = p->buttons[btn].group;
	p->decidedIndex = p->buttons[btn].index;
	p->decidedFlag = flag;
	return 1;
}

/* The `decideAt` hook: record a decision on button `i` of group `g` with
 * `flag`; -1 passes for either (a cancel is -1, -1).  Returns 0 when `g`
 * or `i` is out of range, 1 when recorded.  Reached by the keyboard decide
 * of ButtonPanel_Update and by message 0x10000000 of "80 AC". */
int ButtonPanel_DecideAt(ButtonPanel_t* p, int g, int i, int flag)
{
	if(g != -1 && (g < 0 || g >= p->groupCount))
		return 0;
	if(i != -1)
	{
		if(i < 0)
			return 0;
		// the original reads groups[g].count here even for g == -1; here any index
		// is accepted with g == -1 instead
		if(g != -1 && i >= (int)p->groups[g].count)
			return 0;
	}
	p->decidedGroup = g;
	p->decidedFlag = flag;
	p->decidedIndex = i;
	return 1;
}

// the `canDecide` hook: every button of a button panel may be decided
static int ButtonPanel_CanDecide(ButtonPanel_t* p, int g, int i)
{
	return 1;
}

// the `clickOutside` hook: a button panel ignores a click beside its buttons (the sprite panel reports it)
static void ButtonPanel_ClickOutside(ButtonPanel_t* p)
{
}

// the `finishOnDecide` hook: a decision ends a button panel
static int ButtonPanel_FinishOnDecide(ButtonPanel_t* p)
{
	return 1;
}

// the `animate` hook: a button panel has nothing to animate
static void ButtonPanel_Animate(ButtonPanel_t* p)
{
}

/* The `poll` hook, run once per pass by Panel_PollAll: deliver the queued
 * "80 AC" messages, gather this pass's input bits from the window's layers
 * (a modal panel reads its own key and mouse layers, a non-modal one only
 * the mouse layer of its virtual child and - with `mouse` - the window's,
 * and keeps only the mouse buttons, the wheel and the cancel key, mask
 * 0x2c3), then run ButtonPanel_Update, which ends the panel when it returns
 * 1.  Always returns 0: a panel never stops the polling. */
int ButtonPanel_Poll(ObjProc_t* base)
{
	ButtonPanel_t* p = (ButtonPanel_t*)base;
	ObjProc_ProcessMessages(base);
	if(!p->started)
		return 0;
	if(p->inputEnabled)
	{
		uint32_t mask = 0;
		if(p->modal)
			mask = Input_Poll(WIN_KEY(p), WIN_KEY(p));
		else if(p->mouse)
			mask = Input_Poll(0, WIN_KEY(p));
		mask |= Input_Poll(0, p->virt->vt->sortKey(p->virt));
		p->input = p->modal ? mask : (mask & 0x2c3);
	}
	else
		p->input = 0;
	p->started = ButtonPanel_Update(p) == 0;
	ObjProc_Flush(base);
	return 0;
}

/* The `onMessage` hook for the "80 AC" messages of a panel, by data[0]:
 *   0x10000000 (group<<16 | index, flag)  decide that button (three dwords;
 *              the halves are sign-extended, so -1 cancels) and end the
 *              panel when the class ends on decisions
 *   0x10000001 (group)                    make that group current
 *   0x10000002 (group, index)             select that button
 *   0x10000003 (on)                       enable / disable the panel's input
 * Returns what the operation returned (1 when it took effect); 0 for an
 * unknown message, a bad dword count, or the input switch. */
int ButtonPanel_OnMessage(ObjProc_t* base, int n, const uint32_t* data)
{
	ButtonPanel_t* p = (ButtonPanel_t*)base;
	switch(data[0])
	{
		case 0x10000000:
			if(n != 3)
				return 0;
			if(!PANEL_VT(p)->decideAt(p, (int16_t)(data[1] >> 16), (int16_t)(data[1] & 0xffff), (int)data[2]))
				return 0;
			if(PANEL_VT(p)->finishOnDecide(p))
				p->started = 0;
			return 1;
		case 0x10000001: return ButtonPanel_SetCurrentGroup(p, (int)data[1]);
		case 0x10000002: return PANEL_VT(p)->setSelection(p, (int)data[1], (int)data[2]);
		case 0x10000003: p->inputEnabled = (int)data[1]; return 0;
		default: return 0;
	}
}

// ---- the update -------------------------------------------------------------------

/* What a key map assigns to an input bit; the values are the ones the
 * user maps of "91 BF" use.  "Decide" records the decision and ends the
 * panel when its class ends on decisions. */
enum PanelAction
{
	ACT_NONE,
	ACT_CLICK,      // 1 decide the button under the pointer (flag 1)
	ACT_CANCEL,     // 2 cancel (flag 1; the right button in the built-in maps)
	ACT_DECIDE,     // 3 decide the current group's selection (flag 0)
	ACT_CANCEL_KEY, // 4 cancel (flag 0)
	ACT_PREV,       // 5 the previous button of the current group (wraps around)
	ACT_NEXT,       // 6 the next button (wraps around)
	ACT_PREV_GROUP, // 7 the previous selectable group (wraps around)
	ACT_NEXT_GROUP, // 8 the next selectable group (wraps around)
	ACT_LEFT,       // 9 one column left in the group's grid (wraps within the row)
	ACT_RIGHT,      // 10 one column right (wraps within the row)
	ACT_UP,         // 11 one row up (wraps within the column)
	ACT_DOWN,       // 12 one row down (wraps within the column)
	ACT_PREV_STOP,  // 13 like 5 .. 12 without wrapping: event 0x10000005 reports the edge instead
	ACT_NEXT_STOP,  // 14
	ACT_LEFT_STOP,  // 15
	ACT_RIGHT_STOP, // 16
	ACT_UP_STOP,    // 17
	ACT_DOWN_STOP   // 18
};

// the direction words of events 0x10000003 .. 0x10000005
#define DIR_PREV  0xffffffffu
#define DIR_NEXT  1u
#define DIR_LEFT  0xffffu
#define DIR_RIGHT 1u
#define DIR_UP    0xffff0000u
#define DIR_DOWN  0x10000u

/* the input mask bit of each of the 24 standard keys, in the order of the
 * STDKEY_ enum of input.h (left, right, middle, X1, X2, wheel up, wheel
 * down, decide, cancel, up, down, left, right, 1 .. 9, 0, Tab): a key map
 * is indexed in this order */
static const uint32_t kPanelBits[24] = {0x1, 0x2, 0x4, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x1000, 0x2000, 0x4000,
	0x8000, 0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000, 0x400000, 0x800000, 0x1000000, 0x2000000,
	0x40000000};

// the (group, index) word of the events
#define PACK(g, i) (((uint32_t)(g) << 16) | (uint32_t)(i))

/* One pass of the panel; returns 1 when the panel is finished (a decision
 * on a class that ends with one, or a hot key), else 0.  In order: the
 * subclass' animation, the hover report (event 0x10000001, rectangle
 * test), the focus (event 0x10000002, mask test), hover selection, click
 * repeat for groups that ask for it, then the one action of the key map
 * that the lowest set input bit selects (bits in `inputMask` are ignored),
 * or - when no mapped bit is set - the hot keys.  The four built-in maps
 * share the mouse buttons (click, right button cancels), decide and cancel
 * keys and Tab (groups) and differ in the direction keys: map 0 moves
 * up / down between groups and left / right between buttons, map 1 up /
 * down between buttons and left / right between groups, map 2 moves in the
 * group's grid, map 3 moves in the grid with the axes swapped (up / down
 * move left / right).  Maps 4 .. 7 are the user maps of "91 BF". */
int ButtonPanel_Update(ButtonPanel_t* p)
{
	// the built-in maps (0..3) by standard-key index; Tab (index 23) is the next group, the previous with Shift
	uint32_t tab = (GetKeyStateEx(0x10) & 0x8000) ? ACT_PREV_GROUP : ACT_NEXT_GROUP; // 0x10 = VK_SHIFT
	uint32_t mapA[24] = {ACT_CLICK, ACT_CANCEL, 0, 0, 0, 0, 0, ACT_DECIDE, ACT_CANCEL_KEY, ACT_PREV_GROUP,
		ACT_NEXT_GROUP, ACT_PREV, ACT_NEXT, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	uint32_t mapB[24] = {ACT_CLICK, ACT_CANCEL, 0, 0, 0, 0, 0, ACT_DECIDE, ACT_CANCEL_KEY, ACT_LEFT, ACT_RIGHT,
		ACT_UP, ACT_DOWN, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	uint32_t mapC[24] = {ACT_CLICK, ACT_CANCEL, 0, 0, 0, 0, 0, ACT_DECIDE, ACT_CANCEL_KEY, ACT_PREV, ACT_NEXT,
		ACT_PREV_GROUP, ACT_NEXT_GROUP, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	uint32_t mapD[24] = {ACT_CLICK, ACT_CANCEL, 0, 0, 0, 0, 0, ACT_DECIDE, ACT_CANCEL_KEY, ACT_UP, ACT_DOWN,
		ACT_LEFT, ACT_RIGHT, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	const uint32_t* maps[8];
	const PanelGroup_t* grp = NULL;
	int result = 0, g = p->currentGroup, sel = -1, hit, k, cols, rows, row, col, n, idx;
	uint32_t act = ACT_NONE, bits;

	PANEL_VT(p)->animate(p);
	if(g >= 0 && g < p->groupCount)
	{
		grp = &p->groups[g];
		sel = grp->selected;
	}
	else
		g = -1;

	// the button under the pointer (rectangle test) is reported when it changes
	hit = ButtonPanel_HitTest(p, NULL, 0);
	if(hit != p->hover)
	{
		p->hover = hit;
		if(hit != -1)
			ButtonPanel_PushEvent(p, 0x10000001, (uint32_t)p->buttons[hit].group, (uint32_t)p->buttons[hit].index);
		else
			ButtonPanel_PushEvent(p, 0x10000001, 0xffffffffu, 0xffffffffu);
	}
	// the focus follows the pointer (mask test)
	if(PANEL_VT(p)->setFocus(p, ButtonPanel_HitTestStore(p), 0))
	{
		if(p->focus != -1)
		{
			const PanelInfo_t* f = &p->buttons[p->focus];
			ButtonPanel_PushEvent(p, 0x10000002, PACK(f->group, f->index), f->button->bmpFocus != -1);
		}
		else
			ButtonPanel_PushEvent(p, 0x10000002, 0xffffffffu, 0);
	}
	// hover selection: once the pointer has moved, the focused button becomes its group's selection
	if(p->inputEnabled && p->focus != -1 && p->groups[p->buttons[p->focus].group].hoverSelects)
	{
		int m[2];
		GetMouseClientPos(m);
		if(m[0] != p->lastMouse[0] || m[1] != p->lastMouse[1])
		{
			const PanelInfo_t* f = &p->buttons[p->focus];
			ButtonPanel_SetCurrentGroup(p, f->group);
			PANEL_VT(p)->setSelection(p, f->group, f->index);
			sel = grp ? grp->selected : sel;
			p->lastMouse[0] = m[0];
			p->lastMouse[1] = m[1];
			ButtonPanel_PushEvent(p, 0x10000004, PACK(f->group, f->index), 0);
		}
	}
	// a held left button repeats as clicks on groups that ask for it
	if(p->inputEnabled && p->focus != -1 && p->groups[p->buttons[p->focus].group].clickDecides &&
		(Input_PollHeld(p->virt->vt->sortKey(p->virt)) & 1))
		p->input |= 1;

	mapA[23] = mapB[23] = mapC[23] = mapD[23] = tab;
	maps[0] = mapA;
	maps[1] = mapC;
	maps[2] = mapD;
	maps[3] = mapB;
	for(k = 4; k < 8; k++)
		maps[k] = Panel_UserKeyMap(k);
	// the lowest input bit that is not masked picks the action
	bits = p->input & ~p->inputMask;
	for(k = 0; k < 24; k++)
	{
		if(bits & kPanelBits[k])
		{
			act = maps[p->keyMap][k];
			break;
		}
	}
	idx = sel;
	switch(act)
	{
		case ACT_CLICK:
			if(p->focus == -1)
			{
				PANEL_VT(p)->clickOutside(p);
				return result;
			}
			{
				const PanelInfo_t* f = &p->buttons[p->focus];
				if(!PANEL_VT(p)->canDecide(p, f->group, f->index))
					return result;
				PANEL_VT(p)->decide(p, p->focus, 1);
				if(p->selectOnDecide)
					PANEL_VT(p)->setSelection(p, f->group, f->index);
				result = PANEL_VT(p)->finishOnDecide(p);
				if(result)
					return result;
				ButtonPanel_SetCurrentGroup(p, f->group); // a panel that goes on moves to the clicked group
				return result;
			}
		case ACT_CANCEL:
			PANEL_VT(p)->decide(p, -1, 1);
			return PANEL_VT(p)->finishOnDecide(p);
		case ACT_DECIDE:
			if(!PANEL_VT(p)->canDecide(p, g, sel) || g == -1 || sel == -1)
				return result;
			PANEL_VT(p)->decideAt(p, g, sel, 0);
			return PANEL_VT(p)->finishOnDecide(p);
		case ACT_CANCEL_KEY:
			PANEL_VT(p)->decide(p, -1, 0);
			return PANEL_VT(p)->finishOnDecide(p);
		case ACT_PREV:
		case ACT_NEXT:
			// the moves try every button (or group, row, column) once, so that a
			// button that cannot be selected is stepped over
			if(!grp || (int)grp->count <= 0)
				return result;
			for(n = 0; n < (int)grp->count; n++)
			{
				if(act == ACT_PREV)
					idx = (idx - 1 < 0 || idx - 1 >= (int)grp->count) ? (int)grp->count - 1 : idx - 1;
				else
					idx = (idx + 1 < 0 || idx + 1 >= (int)grp->count) ? 0 : idx + 1;
				if(PANEL_VT(p)->setSelection(p, g, idx))
				{
					ButtonPanel_PushEvent(p, 0x10000004, PACK(g, idx), act == ACT_PREV ? DIR_PREV : DIR_NEXT);
					return result;
				}
			}
			return result;
		case ACT_PREV_GROUP:
		case ACT_NEXT_GROUP:
			if(p->groupCount <= 0)
				return result;
			for(n = 0; n < p->groupCount; n++)
			{
				if(act == ACT_PREV_GROUP)
					g = (g - 1 < 0 || g - 1 >= p->groupCount) ? p->groupCount - 1 : g - 1;
				else
					g = (g + 1 < 0 || g + 1 >= p->groupCount) ? 0 : g + 1;
				if(ButtonPanel_SetCurrentGroup(p, g))
				{
					ButtonPanel_PushEvent(p, 0x10000003, (uint32_t)g, act == ACT_PREV_GROUP ? DIR_PREV : DIR_NEXT);
					return result;
				}
			}
			return result;
		case ACT_LEFT:
		case ACT_RIGHT:
			if(!grp)
				return result;
			cols = p->grid[g][0];
			if(cols <= 0)
				return result;
			for(n = 0; n < cols; n++)
			{
				row = idx / cols;
				col = idx % cols + (act == ACT_LEFT ? -1 : 1);
				if(act == ACT_LEFT && col < 0)
					col = cols - 1;
				if(act == ACT_RIGHT && col >= cols)
					col = 0;
				idx = row * cols + col;
				if(PANEL_VT(p)->setSelection(p, g, idx))
				{
					ButtonPanel_PushEvent(p, 0x10000004, PACK(g, idx), act == ACT_LEFT ? DIR_LEFT : DIR_RIGHT);
					return result;
				}
			}
			return result;
		case ACT_UP:
		case ACT_DOWN:
			if(!grp)
				return result;
			rows = p->grid[g][1];
			if(rows <= 0)
				return result;
			for(n = 0; n < rows; n++)
			{
				cols = p->grid[g][0];
				row = idx / cols + (act == ACT_UP ? -1 : 1);
				if(act == ACT_UP && row < 0)
					row = rows - 1;
				if(act == ACT_DOWN && row >= rows)
					row = 0;
				idx = row * cols + idx % cols;
				if(PANEL_VT(p)->setSelection(p, g, idx))
				{
					ButtonPanel_PushEvent(p, 0x10000004, PACK(g, idx), act == ACT_UP ? DIR_UP : DIR_DOWN);
					return result;
				}
			}
			return result;
		case ACT_PREV_STOP:
			// the non-wrapping moves report the edge (event 0x10000005) when the
			// selection cannot move any further in that direction
			if(!grp)
				return result;
			if(idx > 0 && PANEL_VT(p)->setSelection(p, g, idx - 1))
				ButtonPanel_PushEvent(p, 0x10000004, PACK(g, idx - 1), DIR_PREV);
			else
				ButtonPanel_PushEvent(p, 0x10000005, PACK(g, idx), DIR_PREV);
			return result;
		case ACT_NEXT_STOP:
			if(!grp)
				return result;
			if(PANEL_VT(p)->setSelection(p, g, idx + 1))
				ButtonPanel_PushEvent(p, 0x10000004, PACK(g, idx + 1), DIR_NEXT);
			else
				ButtonPanel_PushEvent(p, 0x10000005, PACK(g, idx), DIR_NEXT);
			return result;
		case ACT_LEFT_STOP:
			if(!grp)
				return result;
			cols = p->grid[g][0];
			row = idx / cols;
			for(col = idx % cols; col > 0;)
			{
				col--;
				if(PANEL_VT(p)->setSelection(p, g, row * cols + col))
				{
					ButtonPanel_PushEvent(p, 0x10000004, PACK(g, row * cols + col), DIR_LEFT);
					return result;
				}
			}
			ButtonPanel_PushEvent(p, 0x10000005, PACK(g, idx), DIR_LEFT);
			return result;
		case ACT_RIGHT_STOP:
			if(!grp)
				return result;
			cols = p->grid[g][0];
			row = idx / cols;
			for(col = idx % cols + 1; col < cols; col++)
			{
				if(PANEL_VT(p)->setSelection(p, g, row * cols + col))
				{
					ButtonPanel_PushEvent(p, 0x10000004, PACK(g, row * cols + col), DIR_RIGHT);
					return result;
				}
			}
			ButtonPanel_PushEvent(p, 0x10000005, PACK(g, idx), DIR_RIGHT);
			return result;
		case ACT_UP_STOP:
			if(!grp)
				return result;
			cols = p->grid[g][0];
			col = idx % cols;
			for(row = idx / cols; row > 0;)
			{
				row--;
				if(PANEL_VT(p)->setSelection(p, g, row * cols + col))
				{
					ButtonPanel_PushEvent(p, 0x10000004, PACK(g, row * cols + col), DIR_UP);
					return result;
				}
			}
			ButtonPanel_PushEvent(p, 0x10000005, PACK(g, idx), DIR_UP);
			return result;
		case ACT_DOWN_STOP:
			if(!grp)
				return result;
			cols = p->grid[g][0];
			rows = p->grid[g][1];
			col = idx % cols;
			for(row = idx / cols + 1; row < rows; row++)
			{
				if(PANEL_VT(p)->setSelection(p, g, row * cols + col))
				{
					ButtonPanel_PushEvent(p, 0x10000004, PACK(g, row * cols + col), DIR_DOWN);
					return result;
				}
			}
			ButtonPanel_PushEvent(p, 0x10000005, PACK(g, idx), DIR_DOWN);
			return result;
		default: // no mapped key: a pressed hot key decides its button (flag 0) and ends the panel at once
			k = ButtonPanel_CheckHotkeys(p);
			result = k != -1;
			if(result)
				PANEL_VT(p)->decide(p, k, 0);
			return result;
	}
}
