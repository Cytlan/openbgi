/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sprpanel.c - the sprite panel (the "91 BA" controller) and the "90 B7"
 *              painter; the interface is src/include/bgi/panel.h
 *
 * A ButtonPanel (panel.c) whose buttons are the window's sub-sprites
 * instead of pixels in the text layer (the text layer is hidden).  The
 * button-panel tables are derived from the sprite records so that the
 * whole input logic is inherited; this file only overrides, through the
 * PanelVtbl_t hooks, how a button is shown: the sub-sprite's bitmap, the
 * per-button frame animation and the "swing" (a sequence of timed rotations
 * about the sprite's pivot).  Decisions are additionally reported as event
 * 0x10000006 (group<<16|index, or 0xffffffff for a cancel; flag), from
 * 1.69/472 preceded by event 0x10000007 with the click position inside
 * the button for a mouse decision (from 1.492 also sent, with 0xffffffff
 * and the position inside the window, for a click beside the buttons),
 * and a decision does not end the panel.
 */
#include <string.h>
#include "bgi/panel.h"
#include "bgi/gfx.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/window.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/input.h"
#include "bgi/sys.h"
#include "bgi/vm.h"
#include "bgi/version.h" // the `flags` of SpriteButton_t depend on the engine generation
#include "bgi/core.h"

// the window's sort key: the layer key the panel uses on the input layer stacks
#define WIN_KEY(o, p) ((p)->win->obj.vt->sortKey(o, &(p)->win->obj))

typedef struct SpriteState // the animation state of one button
{
	int hasBitmap;       // the button is drawn (a sub-sprite exists)
	SpriteButton_t* btn; // its record in the panel's own copy of the tables
	int sub;             // the window's sub-sprite index (= the flat button index)
	int frame;           // the current animation frame, added to the bitmap number
	uint32_t nextFrame;  // tick (ms) of the next frame; 0 = the animation has not started
	int32_t angle;       // the angle at the start of the current swing phase (16.16 degrees)
	int phase;           // the current swing phase (index into `swing` of the record)
	int step, steps;     // 4 ms steps done in the phase / steps the phase has
	uint32_t nextSwing;  // tick (ms) of the next swing step; 0 = at rest
	int prio;            // the sub-sprite's priority at rest (SpritePriority)
} SpriteState_t;

typedef struct SpritePanel
{
	ButtonPanel_t b;        // the base class; `b.groups` are the derived button-panel tables
	SpriteDesc_t desc;      // the copied descriptor (the panel's own sprite group and button tables)
	SpriteState_t** states; // per group: one state per button
} SpritePanel_t;

// the tables of a copied descriptor / of the panel / of a group
#define DGROUPS(d)      ((d)->groups)
#define SGROUPS(sp)     DGROUPS(&(sp)->desc)
#define SBUTTONS(g)     ((g)->buttons)
#define SWING_FULL_TURN 23592960 // 360 degrees in 16.16 (360 << 16)

static void SpritePanel_Destroy(OpenBGI_t* o, ObjProc_t* p);
static void SpritePanel_Animate(OpenBGI_t* o, ButtonPanel_t* p);
static int SpritePanel_FinishOnDecide(OpenBGI_t* o, ButtonPanel_t* p);
static int SpritePanel_SetFocus(OpenBGI_t* o, ButtonPanel_t* p, int btn, int force);
static int SpritePanel_GetBitmap(OpenBGI_t* o, ButtonPanel_t* p, int32_t* out, int g, int i, int focus, int sel);
static int SpritePanel_SetSelection(OpenBGI_t* o, ButtonPanel_t* p, int g, int i);
static int SpritePanel_CanDecide(OpenBGI_t* o, ButtonPanel_t* p, int g, int i);
static int SpritePanel_DecideAt(OpenBGI_t* o, ButtonPanel_t* p, int g, int i, int flag);
static int SpritePanel_Decide(OpenBGI_t* o, ButtonPanel_t* p, int btn, int flag);
static void SpritePanel_Stop(OpenBGI_t* o, ButtonPanel_t* p);
static void SpritePanel_ClickOutside(OpenBGI_t* o, ButtonPanel_t* p);

static const PanelVtbl_t SpritePanel_Vtbl = {
	{SpritePanel_Destroy, ButtonPanel_Poll, ButtonPanel_OnMessage}, SpritePanel_Animate, SpritePanel_FinishOnDecide,
	SpritePanel_SetFocus, SpritePanel_GetBitmap, SpritePanel_SetSelection, SpritePanel_CanDecide,
	SpritePanel_DecideAt, SpritePanel_Decide, SpritePanel_Stop, SpritePanel_ClickOutside};

// ========================================================================
// construction
// ========================================================================

// a button panel on `win` with the sprite hooks and no tables yet
static void SpritePanel_Ctor(OpenBGI_t* o, SpritePanel_t* sp, Window_t* win)
{
	ButtonPanel_Ctor(o, &sp->b, win);
	sp->b.p.vt = &SpritePanel_Vtbl.base;
	sp->b.isSprite = 1;
	memset(&sp->desc, 0, sizeof sp->desc);
	sp->states = NULL;
}

// allocate a sprite panel on `win`; the result is its ButtonPanel_t base
ButtonPanel_t* SpritePanel_New(OpenBGI_t* o, Window_t* win)
{
	SpritePanel_t* sp = (SpritePanel_t*)BGI_Alloc(sizeof *sp);
	SpritePanel_Ctor(o, sp, win);
	return &sp->b;
}

static void SpritePanel_Dtor(OpenBGI_t* o, SpritePanel_t* sp)
{
	sp->b.p.vt = &SpritePanel_Vtbl.base; // this class' hooks for the stop, whatever a subclass set
	SpritePanel_Stop(o, &sp->b);
	ButtonPanel_Dtor(o, &sp->b);
}

static void SpritePanel_Destroy(OpenBGI_t* o, ObjProc_t* p)
{
	SpritePanel_Dtor(o, (SpritePanel_t*)p);
	BGI_Free(p);
}

// ========================================================================
// the record copies
// ========================================================================

// free a copy made by SpritePanel_CopyDesc (also a partial one)
void SpritePanel_FreeDesc(SpriteDesc_t* d)
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

/* Panel_CopyDesc for the sprite records: a private copy of the descriptor,
 * its groups and their buttons with the tagged pointers resolved through
 * thread `t`; the copy is the host-side type of panel.h.  Returns 0 and
 * the copy in *out, 2 for an unusable group table (null, or a group count
 * outside 1..0x100), 3 for an unusable button table (null, or a count
 * whose low 16 bits are outside 1..0x100 - only those are validated, the
 * full 32-bit count is copied).  The caller frees the copy with
 * SpritePanel_FreeDesc. */
int SpritePanel_CopyDesc(OpenBGI_t* o, SpriteDesc_t** out, const SpriteDescScript_t* d, Thread_t* t)
{
	SpriteDesc_t* c = (SpriteDesc_t*)BGI_Alloc(sizeof *c);
	const SpriteGroupScript_t* src;
	SpriteGroup_t* groups;
	int status = 0, g;
	c->groupCount = d->groupCount;
	c->groups = NULL;
	c->currentGroup = d->currentGroup;
	c->modal = d->modal;
	c->mouse = d->mouse;
	c->inputMask = d->inputMask;
	c->keyMap = d->keyMap;
	c->selectOnDecide = d->selectOnDecide;
	c->inputDisabled = d->inputDisabled;
	c->unused24 = d->unused24;
	src = (const SpriteGroupScript_t*)ResolvePtr(o, d->groups, t);
	if(!src || d->groupCount <= 0 || d->groupCount > 0x100)
	{
		SpritePanel_FreeDesc(c);
		*out = NULL;
		return 2;
	}
	groups = (SpriteGroup_t*)BGI_Calloc((size_t)d->groupCount * sizeof *groups);
	c->groups = groups;
	for(g = 0; g < d->groupCount; g++)
	{
		const SpriteButton_t* buttons;
		int count;
		if(status) // after a failure the remaining groups keep their NULL button tables, for SpritePanel_FreeDesc
			continue;
		groups[g].count = src[g].count;
		groups[g].columns = src[g].columns;
		groups[g].selected = src[g].selected;
		groups[g].unused10 = src[g].unused10;
		groups[g].selectable = src[g].selectable;
		groups[g].hoverSelects = src[g].hoverSelects;
		groups[g].clickDecides = src[g].clickDecides;
		groups[g].linkId = src[g].linkId;
		memcpy(groups[g].ov, src[g].ov, sizeof groups[g].ov);
		groups[g].flags = src[g].flags;
		buttons = (const SpriteButton_t*)ResolvePtr(o, src[g].buttons, t);
		count = src[g].count & 0xffff;
		if(!buttons || count <= 0 || count > 0x100)
		{
			status = 3;
			continue;
		}
		groups[g].buttons = (SpriteButton_t*)BGI_Alloc((size_t)count * sizeof *buttons);
		memcpy(groups[g].buttons, buttons, (size_t)count * sizeof *buttons);
	}
	if(status)
	{
		SpritePanel_FreeDesc(c);
		*out = NULL;
		return status;
	}
	*out = c;
	return 0;
}

// ========================================================================
// painting and start
// ========================================================================

/* the bitmap number button `i` shows at start: its selected picture when
 * it is the group's selection (`selected`) and has one, else the normal one
 * (-1 when it has none) */
static int SpriteStartBitmap(const SpriteButton_t* b, int i, int selected)
{
	int no = b->bmpNormal;
	if(no != -1 && i == selected && b->bmpSelected != -1)
		no = b->bmpSelected;
	return no;
}

/* Whether a button gets a sub-sprite: from 1.69/451 on only the buttons
 * with `shown` set, whatever `enabled` says - a game hides the buttons of
 * the pages it does not show that way (build 444 draws them all in
 * SpritePanel_Paint, and the enabled or shown ones in SpritePanel_Start);
 * from 1.640 on also not those with flag 0x40. */
static int SpriteIsDrawn(OpenBGI_t* o, const SpriteButton_t* b)
{
	if(o->engine.profile->gen >= GEN_1_69_451 && !b->shown)
		return 0;
	if(o->engine.profile->gen >= GEN_1_640 && (b->flags & 0x40))
		return 0;
	return 1;
}

/* the draw priority of a button's sub-sprite: `dflt`, unless the `flags` of
 * the record ask for its `shown` value (0x10, 1.588 on) or its y (0x02,
 * 1.492 on) */
static int SpritePriority(OpenBGI_t* o, const SpriteButton_t* b, int dflt)
{
	if(o->engine.profile->gen >= GEN_1_588 && (b->flags & 0x10))
		return b->shown;
	if(o->engine.profile->gen >= GEN_1_492 && (b->flags & 0x2))
		return b->y;
	return dflt;
}

// every group has 1..0x100 buttons; *total receives the sum of the counts
static int SpriteGroupsValid(const SpriteDesc_t* d, int* total)
{
	const SpriteGroup_t* groups = DGROUPS(d);
	int g, ok = 1, sum = 0;
	for(g = 0; g < d->groupCount && ok; g++)
	{
		sum += groups[g].count;
		ok = groups[g].count > 0 && groups[g].count <= 0x100;
	}
	*total = sum;
	return ok;
}

/* "90 B7": show the buttons of a copied sprite table on window `w` without
 * creating a panel.  The window is reset (items off, text layer cleared
 * and hidden), then every drawn button (SpriteIsDrawn; the `enabled` flag
 * is not consulted here) becomes a sub-sprite with its start picture at
 * (x + ox, y + oy), pivot (ox, oy) and its flat index as the priority
 * (unless the flags say otherwise).  Returns 0, or 0x80000001 for a group
 * count outside 1..0x100, 0x80000002 for a button count outside 1..0x100.
 * The window is invalidated in every case. */
uint32_t SpritePanel_Paint(OpenBGI_t* o, Window_t* w, const SpriteDesc_t* d)
{
	uint32_t status = 0x80000001;
	int total, g, k = 0;
	Window_ItemsDisableAll(o, w);
	Window_SubAlloc(o, w, 0);
	Window_ClearText(o, w);
	Window_SetTextLevel(w, 0);
	Window_ShowText(w, 0);
	Window_RecompositeAll(o, w);
	if(d->groupCount > 0 && d->groupCount <= 0x100)
	{
		if(!SpriteGroupsValid(d, &total))
			status = 0x80000002;
		else
		{
			const SpriteGroup_t* groups = DGROUPS(d);
			Window_SubAlloc(o, w, total);
			for(g = 0; g < d->groupCount; g++)
			{
				const SpriteButton_t* b = SBUTTONS(&groups[g]);
				int i;
				for(i = 0; i < groups[g].count; i++, k++)
				{
					Bmp_t bmp;
					Rect_t r;
					int no = SpriteStartBitmap(&b[i], i, groups[g].selected);
					if(!SpriteIsDrawn(o, &b[i]) || !BmpMgr_GetInfo(o->panel.panelBmpMgr, &bmp, no))
						continue;
					Window_SubCreate(o, w, k, no, b[i].x + b[i].ox, b[i].y + b[i].oy, 0, b[i].ox, b[i].oy, 0,
						SpritePriority(o, &b[i], k));
					Window_SubRefresh(o, w, &r, k);
				}
			}
			status = 0;
		}
	}
	w->obj.vt->invalidate(o, &w->obj);
	return status;
}

/* "91 BA": start (or restart) the panel from a copied descriptor.  A
 * running panel is stopped first and the window reset with its text layer
 * hidden.  The panel keeps its own copy of the sprite records (for the
 * animation) and derives the button-panel tables from them (position,
 * pictures, hit mask and overlays; no hot keys).  Each drawn button
 * becomes a sub-sprite plus a hit object; a button that is merely `shown`
 * but not enabled cannot be hit.  Returns 0, or 0x80000001 for a group
 * count outside 1..0x100, 0x80000002 for a button count outside 1..0x100.
 * The copy `d` is not kept; the caller frees it. */
uint32_t SpritePanel_Start(OpenBGI_t* o, ButtonPanel_t* p, const SpriteDesc_t* d)
{
	SpritePanel_t* sp = (SpritePanel_t*)p;
	uint32_t status = 0x80000001;
	SpriteGroup_t* src;
	PanelInfo_t* info;
	int g, total, k = 0, startSel = 0;
	PANEL_VT(p)->stop(o, p);
	Window_ItemsDisableAll(o, p->win);
	Window_SubAlloc(o, p->win, 0);
	Window_ClearText(o, p->win);
	Window_SetTextLevel(p->win, 0);
	Window_ShowText(p->win, 0);
	Window_RecompositeAll(o, p->win);
	if(d->groupCount <= 0 || d->groupCount > 0x100)
		goto done;
	// the {columns, rows} grid of every group, as Panel_BuildGrid makes it, from
	// the 32-bit count and column fields of the sprite records
	p->grid = (int32_t(*)[2])BGI_Alloc((size_t)d->groupCount * 8);
	src = DGROUPS(d);
	{
		int ok = 1;
		total = 0;
		for(g = 0; g < d->groupCount && ok; g++)
		{
			int count = src[g].count, cols = src[g].columns;
			if(cols > count)
				cols = count;
			else if(cols < 1)
				cols = 1;
			p->grid[g][0] = cols;
			p->grid[g][1] = (count + cols - 1) / cols;
			total += count;
			ok = count > 0 && count <= 0x100;
		}
		if(!ok)
		{
			BGI_Free(p->grid);
			status = 0x80000002;
			goto done;
		}
	}
	Window_SubAlloc(o, p->win, total);
	sp->desc = *d;
	sp->desc.groups = (SpriteGroup_t*)BGI_Alloc((size_t)d->groupCount * sizeof(SpriteGroup_t));
	p->groupCount = d->groupCount;
	p->groups = (PanelGroup_t*)BGI_Alloc((size_t)d->groupCount * sizeof *p->groups);
	p->currentGroup = (d->currentGroup >= 0 && d->currentGroup < d->groupCount) ? d->currentGroup : -1;
	p->modal = d->modal;
	p->mouse = d->mouse;
	p->inputMask = d->inputMask;
	p->keyMap = d->keyMap & 7; // 0..3 the built-in maps, 4..7 the user maps of "91 BF"
	p->selectOnDecide = d->selectOnDecide;
	p->inputEnabled = d->inputDisabled == 0;
	p->total = total;
	p->buttons = (PanelInfo_t*)BGI_Alloc((size_t)total * sizeof *p->buttons);
	sp->states = (SpriteState_t**)BGI_Alloc((size_t)d->groupCount * sizeof *sp->states);
	info = p->buttons;
	for(g = 0; g < d->groupCount; g++)
	{
		const SpriteGroup_t* sg = &src[g];
		const SpriteButton_t* sb = SBUTTONS(sg);
		SpriteGroup_t* og = &SGROUPS(sp)[g]; // the panel's own sprite group
		SpriteButton_t* ob;
		PanelGroup_t* dg = &p->groups[g];
		PanelButton_t* db;
		SpriteState_t* st;
		int i;
		*og = *sg;
		ob = (SpriteButton_t*)BGI_Alloc((size_t)sg->count * sizeof *ob);
		og->buttons = ob;
		dg->count = sg->count;
		dg->columns = sg->columns;
		db = (PanelButton_t*)BGI_Alloc((size_t)sg->count * sizeof *db);
		dg->buttons = db;
		dg->selected = (sg->selected >= 0 && sg->selected < sg->count) ? sg->selected : -1;
		dg->selectable = sg->selectable;
		dg->hoverSelects = sg->hoverSelects;
		dg->clickDecides = sg->clickDecides;
		dg->linkId = sg->linkId;
		memcpy(dg->ov, sg->ov, sizeof dg->ov);
		st = (SpriteState_t*)BGI_Alloc((size_t)sg->count * sizeof *st);
		sp->states[g] = st;
		for(i = 0; i < sg->count; i++, k++, info++)
		{
			Bmp_t bmp;
			Rect_t r;
			int no = SpriteStartBitmap(&sb[i], i, dg->selected);
			int ok;
			memset(&st[i], 0, sizeof st[i]);
			ob[i] = sb[i];
			// build 444 draws a button when it is enabled or shown, from 451 on SpriteIsDrawn decides
			ok = (o->engine.profile->gen >= GEN_1_69_451 ? SpriteIsDrawn(o, &sb[i]) : (sb[i].enabled || sb[i].shown)) &&
				BmpMgr_GetInfo(o->panel.panelBmpMgr, &bmp, no);
			info->hasBitmap = ok;
			info->group = g;
			info->index = i;
			info->button = &db[i];
			info->virt = NULL;
			if(!ok)
			{
				// a button that is not drawn gets a blank record (as PanelButton_Blank in
				// panel.c): position 0, every bitmap number -1, no hot key; `enabled` and
				// `unused34` are left as allocated, nothing reads them for such a button
				db[i].x = db[i].y = 0;
				db[i].bmpNormal = db[i].bmpSelected = db[i].bmpFocus = db[i].hitMaskBmp = -1;
				db[i].ov[0][0] = db[i].ov[0][1] = 0;
				db[i].ov[0][2] = -1;
				db[i].ov[1][0] = db[i].ov[1][1] = 0;
				db[i].ov[1][2] = -1;
				db[i].hotkey = 0;
				continue;
			}
			st[i].hasBitmap = 1;
			st[i].btn = &ob[i];
			st[i].sub = k;
			db[i].enabled = sb[i].enabled;
			db[i].x = sb[i].x;
			db[i].y = sb[i].y;
			db[i].bmpNormal = sb[i].bmpNormal;
			db[i].bmpSelected = sb[i].bmpSelected;
			db[i].bmpFocus = sb[i].bmpFocus;
			db[i].hitMaskBmp = sb[i].hitMaskBmp;
			memcpy(db[i].ov, sb[i].ov, sizeof db[i].ov);
			db[i].unused34 = 0;
			db[i].hotkey = 0;
			st[i].prio = SpritePriority(o, &sb[i], k);
			Window_SubCreate(o, p->win, k, no, sb[i].x + sb[i].ox, sb[i].y + sb[i].oy, 0, sb[i].ox, sb[i].oy, 0, st[i].prio);
			Window_SubRefresh(o, p->win, &r, k);
			// the hit object sits at (x, y); a disabled (merely shown) button cannot be hit
			info->virt = Panel_MakeHitObject(o, p, sb[i].x, sb[i].y, &bmp, sb[i].enabled ? sb[i].hitMaskBmp : -2);
			if(getenv("BGI_PANEL_DEBUG"))
				fprintf(stderr, "  btn g%d i%d en=%d shown=%d x=%d y=%d bmp=%d mask=%d flags=%x size=%dx%d frames=%d/%dms pictures=%d,%d,%d,%d swing=%d\n", g, i,
					sb[i].enabled, sb[i].shown, sb[i].x, sb[i].y, no, sb[i].hitMaskBmp, sb[i].flags, bmp.w, bmp.h, sb[i].frames, sb[i].frameMs,
					sb[i].bmpNormal, sb[i].bmpFocus, sb[i].bmpSelected, sb[i].bmpSelFocus, sb[i].swingPhases);
		}
	}
	if(d->currentGroup != -1 && src[d->currentGroup].selected != -1)
		startSel = 1;
	ButtonPanel_FinishStart(o, p, startSel);
	status = 0;
done:
	p->win->obj.vt->invalidate(o, &p->win->obj);
	ObjProc_MarkDirty(&p->p);
	return status;
}

// the `stop` hook: free the sprite tables and states, then stop the button panel
static void SpritePanel_Stop(OpenBGI_t* o, ButtonPanel_t* p)
{
	SpritePanel_t* sp = (SpritePanel_t*)p;
	int g;
	// the group count of `desc` is not cleared below, as in the original, where a
	// stop after a Start() that failed its validation dereferences the NULL
	// tables; the NULL test of the loop avoids that here
	for(g = 0; SGROUPS(sp) && g < sp->desc.groupCount; g++)
	{
		BGI_Free(SBUTTONS(&SGROUPS(sp)[g]));
		BGI_Free(sp->states[g]);
	}
	BGI_Free(SGROUPS(sp));
	BGI_Free(sp->states);
	sp->desc.groups = NULL;
	sp->states = NULL;
	ButtonPanel_Stop(o, p);
}

// ========================================================================
// showing a button
// ========================================================================

// give sub-sprite `sub` bitmap `no` and mark its rectangle dirty
static void SpritePanel_ShowSub(OpenBGI_t* o, SpritePanel_t* sp, int sub, int no)
{
	Rect_t r;
	Window_SubSetBitmap(o, sp->b.win, sub, no);
	Window_SubRefresh(o, sp->b.win, &r, sub);
	Gfx_AddDirty(o->panel.panelGfx, WIN_KEY(o, &sp->b), &r);
	ObjProc_MarkDirty(&sp->b.p);
}

// is button (g, i) the focused one (the one under the pointer)?
static int SpriteIsFocused(ButtonPanel_t* p, int g, int i)
{
	return p->focus != -1 && p->buttons[p->focus].group == g && p->buttons[p->focus].index == i;
}

/* Whether the frames of button (g, i) run now: always, but from 1.553 on
 * only while the pointer is on the button when its flags say so (0x01), or
 * while it is on the button or the button is its group's selection (0x08). */
static int SpriteAnimates(OpenBGI_t* o, ButtonPanel_t* p, int g, int i)
{
	const SpritePanel_t* sp = (const SpritePanel_t*)p;
	int flags = sp->states[g][i].btn->flags;
	if(o->engine.profile->gen < GEN_1_553)
		return 1;
	if(flags & 0x1)
		return SpriteIsFocused(p, g, i);
	if(flags & 0x8)
		return SpriteIsFocused(p, g, i) || i == SGROUPS(sp)[g].selected;
	return 1;
}

/* a button that stops being what its frames run for starts them anew the
 * next time (1.553 on) */
static void SpriteRewind(SpriteState_t* st)
{
	st->frame = 0;
	st->nextFrame = 0;
}

/* The `getBitmap` hook: the bitmap number button (g, i) shows into *out:
 * normal, the focus picture when `focus` is set, the selected picture when
 * `sel` is set and it is the group's selection, the selected-and-focused
 * picture when both apply (each only when the button has it; the later
 * ones win), plus the current animation frame.  From 1.553 on the frames
 * belong to one of the pictures when the button's flags say so: with 0x01
 * to the focus picture - an unfocused button, one without a focus picture
 * and a selected one that shows its selected picture stay still - and
 * with 0x08 to the focused or selected button, so that an unfocused,
 * unselected one stays still.  Returns 0 (and leaves *out) when (g, i) is
 * out of range or the button is not drawn. */
static int SpritePanel_GetBitmap(OpenBGI_t* o, ButtonPanel_t* p, int32_t* out, int g, int i, int focus, int sel)
{
	SpritePanel_t* sp = (SpritePanel_t*)p;
	const SpriteGroup_t* grp;
	const SpriteState_t* st;
	const SpriteButton_t* b;
	if(g >= sp->desc.groupCount)
		return 0;
	grp = &SGROUPS(sp)[g];
	if(i >= grp->count)
		return 0;
	st = &sp->states[g][i];
	if(!st->hasBitmap)
		return 0;
	b = st->btn;
	*out = b->bmpNormal;
	if(o->engine.profile->gen >= GEN_1_553)
	{
		int forFocus = b->flags & 0x1, forSelected = (b->flags >> 3) & 1; // whose the frames are
		int isSel = sel && i == grp->selected, still = 0;
		int atRest = isSel ? !forFocus : !forFocus && !forSelected; // the frames count for a button that shows no focus picture
		int addFrame = focus ? 1 : atRest;
		if(focus)
		{
			if(b->bmpFocus != -1)
				*out = b->bmpFocus;
			else
				addFrame = atRest;
		}
		if(isSel)
		{
			if(b->bmpSelected != -1)
			{
				*out = b->bmpSelected;
				still = forFocus; // the selected picture is not the one the frames are for
			}
			if(focus)
			{
				if(b->bmpSelFocus != -1)
					*out = b->bmpSelFocus;
				else if(still)
					addFrame = 0;
			}
		}
		if(addFrame)
			*out += st->frame;
		return 1;
	}
	if(focus && b->bmpFocus != -1)
		*out = b->bmpFocus;
	if(sel && i == grp->selected)
	{
		if(b->bmpSelected != -1)
			*out = b->bmpSelected;
		if(focus && b->bmpSelFocus != -1)
			*out = b->bmpSelFocus;
	}
	*out += st->frame;
	return 1;
}

/* The `setFocus` hook, like ButtonPanel_SetFocus with the sub-sprites'
 * bitmaps swapped instead of the text layer redrawn: the old focus goes
 * back to its unfocused picture, the new one (flat index `btn`, -1 none)
 * gets its focused picture and its overlays in window items 2 and 3.
 * From 1.492 on a group can ask (flag 0x01) for its focused button to be
 * drawn over the others: its sub-sprite's priority is 0x100 higher while
 * it has the focus.  From 1.553 on the frames of the button that loses
 * the focus start anew when they were running for the focus.
 * Returns 0 when `btn` is the focus already and `force` is clear, else 1. */
static int SpritePanel_SetFocus(OpenBGI_t* o, ButtonPanel_t* p, int btn, int force)
{
	SpritePanel_t* sp = (SpritePanel_t*)p;
	Rect_t area;
	int32_t no;
	if(p->focus == btn && !force)
		return 0;
	Window_GetTextArea(p->win, &area);
	ButtonPanel_DirtyItem(o, p, 2);
	ButtonPanel_DirtyItem(o, p, 3);
	if(p->focus != -1)
	{
		const PanelInfo_t* f = &p->buttons[p->focus];
		SpriteState_t* st = &sp->states[f->group][f->index];
		if(o->engine.profile->gen >= GEN_1_553)
		{
			int flags = st->btn->flags;
			if((flags & 0x1) || ((flags & 0x8) && f->index != SGROUPS(sp)[f->group].selected))
				SpriteRewind(st);
		}
		if(o->engine.profile->gen >= GEN_1_492 && (SGROUPS(sp)[f->group].flags & 0x1))
			Window_SubSetPriority(o, p->win, st->sub, st->prio);
		PANEL_VT(p)->getBitmap(o, p, &no, f->group, f->index, 0, 1);
		SpritePanel_ShowSub(o, sp, sp->states[f->group][f->index].sub, no);
	}
	if(btn != -1)
	{
		const PanelInfo_t* f = &p->buttons[btn];
		if(o->engine.profile->gen >= GEN_1_492 && (SGROUPS(sp)[f->group].flags & 0x1)) // the focused button over its neighbours
			Window_SubSetPriority(o, p->win, sp->states[f->group][f->index].sub, sp->states[f->group][f->index].prio + 0x100);
		PANEL_VT(p)->getBitmap(o, p, &no, f->group, f->index, 1, 1);
		SpritePanel_ShowSub(o, sp, sp->states[f->group][f->index].sub, no);
		ButtonPanel_DrawOverlays(o, p, 2, f->button->ov, &area);
	}
	else
	{
		Window_ItemEnable(o, p->win, 2, 0);
		Window_ItemEnable(o, p->win, 3, 0);
		ObjProc_MarkDirty(&p->p);
	}
	p->focus = btn;
	return 1;
}

/* The `setSelection` hook: select button `i` of group `g` (-1 clears),
 * swapping the sub-sprites' bitmaps of the old and the new selection and
 * applying the group's link.  Unlike the button panel's, a disabled button
 * cannot be selected.  The selection is recorded in both the panel group
 * and the sprite group.  Returns 1 when the selection changed, 0 when `g`
 * or `i` is out of range, the button is disabled, or `i` is the selection
 * already. */
static int SpritePanel_SetSelection(OpenBGI_t* o, ButtonPanel_t* p, int g, int i)
{
	SpritePanel_t* sp = (SpritePanel_t*)p;
	PanelGroup_t* grp;
	int old;
	int32_t no;
	if(g < 0 || g >= p->groupCount)
		return 0;
	grp = &p->groups[g];
	old = grp->selected;
	if(i >= 0 && i < (int)grp->count)
	{
		if(!SBUTTONS(&SGROUPS(sp)[g])[i].enabled)
			return 0;
	}
	else if(i != -1)
		return 0;
	if(i == old)
		return 0;
	if(old != -1)
	{
		if(o->engine.profile->gen >= GEN_1_553 && (sp->states[g][old].btn->flags & 0x8)) // its frames ran for the selection
			SpriteRewind(&sp->states[g][old]);
		PANEL_VT(p)->getBitmap(o, p, &no, g, old, SpriteIsFocused(p, g, old), 0);
		SpritePanel_ShowSub(o, sp, sp->states[g][old].sub, no);
	}
	grp->selected = i;
	SGROUPS(sp)
	[g].selected = i;
	if(i == -1)
		return 1;
	PANEL_VT(p)->getBitmap(o, p, &no, g, i, SpriteIsFocused(p, g, i), 1);
	Window_SubSetBitmap(o, p->win, sp->states[g][i].sub, no);
	{
		Rect_t r;
		Window_SubRefresh(o, p->win, &r, sp->states[g][i].sub);
		Gfx_AddDirty(o->panel.panelGfx, WIN_KEY(o, p), &r);
	}
	ButtonPanel_ApplyLink(o, p, g);
	ObjProc_MarkDirty(&p->p);
	return 1;
}

/* the `canDecide` hook: a button out of range cannot be decided, nor one
 * marked `noRedecide` while it is its group's selection */
static int SpritePanel_CanDecide(OpenBGI_t* o, ButtonPanel_t* p, int g, int i)
{
	SpritePanel_t* sp = (SpritePanel_t*)p;
	if(g < 0 || g >= p->groupCount || i < 0 || i >= (int)p->groups[g].count)
		return 0;
	if(SBUTTONS(&SGROUPS(sp)[g])[i].noRedecide && i == p->groups[g].selected)
		return 0;
	return 1;
}

/* 1.69/472 on: event 0x10000007 (group<<16|index, y<<16|x) ahead of a
 * mouse decision's 0x10000006 - the pointer position relative to the
 * decided button's top left, (0, 0) when the pointer is not inside its
 * rectangle (a decision by message) */
static void SpritePanel_PushClickEvent(OpenBGI_t* o, ButtonPanel_t* p, int btn, uint32_t who)
{
	int m[2], x = 0, y = 0;
	Rect_t r;
	if(o->engine.profile->gen < GEN_1_69_472)
		return;
	GetMouseClientPos(o, m);
	if(!p->buttons[btn].virt)
		return;
	p->buttons[btn].virt->vt->screenRect(o, p->buttons[btn].virt, &r);
	if(m[0] >= r.l && m[0] <= r.r && m[1] >= r.t && m[1] <= r.b)
	{
		x = m[0] - r.l;
		y = m[1] - r.t;
	}
	ButtonPanel_PushEvent(p, 0x10000007, who, ((uint32_t)y << 16) | ((uint32_t)x & 0xffffu));
}

// the `decide` hook: ButtonPanel_Decide plus event 0x10000006 (and the click position) when it was recorded
static int SpritePanel_Decide(OpenBGI_t* o, ButtonPanel_t* p, int btn, int flag)
{
	int r = ButtonPanel_Decide(o, p, btn, flag);
	if(r)
	{
		uint32_t who = 0xffffffffu;
		if(btn != -1)
		{
			who = ((uint32_t)p->buttons[btn].group << 16) | (uint32_t)p->buttons[btn].index;
			if(flag)
				SpritePanel_PushClickEvent(o, p, btn, who);
		}
		ButtonPanel_PushEvent(p, 0x10000006, who, (uint32_t)flag);
	}
	return r;
}

// the `decideAt` hook: ButtonPanel_DecideAt plus event 0x10000006 (and the click position) when it was recorded
static int SpritePanel_DecideAt(OpenBGI_t* o, ButtonPanel_t* p, int g, int i, int flag)
{
	int r = ButtonPanel_DecideAt(o, p, g, i, flag);
	if(r)
	{
		uint32_t who = (g != -1 && i != -1) ? (((uint32_t)g << 16) | (uint32_t)i) : 0xffffffffu;
		if(who != 0xffffffffu && flag)
		{
			int btn;
			for(btn = 0; btn < p->total; btn++)
				if(p->buttons[btn].group == g && p->buttons[btn].index == i)
					break;
			if(btn < p->total && p->buttons[btn].virt)
				SpritePanel_PushClickEvent(o, p, btn, who);
		}
		ButtonPanel_PushEvent(p, 0x10000006, who, (uint32_t)flag);
	}
	return r;
}

/* the `clickOutside` hook (1.492 on): a click beside the buttons is
 * reported as event 0x10000007 (0xffffffff, y<<16|x) with the pointer
 * position relative to the window */
static void SpritePanel_ClickOutside(OpenBGI_t* o, ButtonPanel_t* p)
{
	int m[2];
	Rect_t r;
	if(o->engine.profile->gen < GEN_1_492)
		return;
	GetMouseClientPos(o, m);
	p->win->obj.vt->screenRect(o, &p->win->obj, &r);
	ButtonPanel_PushEvent(p, 0x10000007, 0xffffffffu,
		((uint32_t)(m[1] - r.t) << 16) | ((uint32_t)(m[0] - r.l) & 0xffffu));
}

// the `finishOnDecide` hook: a decision does not end a sprite panel
static int SpritePanel_FinishOnDecide(OpenBGI_t* o, ButtonPanel_t* p)
{
	return 0;
}

// ========================================================================
// animation
// ========================================================================

// turn sub-sprite `sub` to `angle` (16.16 degrees): erase, rotate, refresh, both rectangles dirty
static void SpritePanel_Rotate(OpenBGI_t* o, SpritePanel_t* sp, int sub, int32_t angle)
{
	Rect_t r;
	Window_SubErase(o, sp->b.win, &r, sub);
	Gfx_AddDirty(o->panel.panelGfx, WIN_KEY(o, &sp->b), &r);
	Window_SubSetAngle(o, sp->b.win, sub, angle);
	Window_SubRefresh(o, sp->b.win, &r, sub);
	Gfx_AddDirty(o->panel.panelGfx, WIN_KEY(o, &sp->b), &r);
	ObjProc_MarkDirty(&sp->b.p);
}

/* The `animate` hook, run at the start of every update: advance the frame
 * animations (the bitmap number steps through `frames` consecutive slots,
 * one every `frameMs` milliseconds; SpriteAnimates says for which buttons)
 * and the swings of the drawn buttons.
 * A swing runs through its `swingPhases` phases in 4 ms steps, each phase
 * turning the sprite by its angle over its duration, starting from
 * `swingAngle0`; a swing that is for the focused button only rests at
 * angle 0 (or, with `swingPause`, pauses where it is) while the pointer is
 * elsewhere or the button cannot be decided. */
static void SpritePanel_Animate(OpenBGI_t* o, ButtonPanel_t* p)
{
	SpritePanel_t* sp = (SpritePanel_t*)p;
	int g;
	for(g = 0; g < sp->desc.groupCount; g++)
	{
		const SpriteGroup_t* grp = &SGROUPS(sp)[g];
		int i;
		for(i = 0; i < grp->count; i++)
		{
			SpriteState_t* st = &sp->states[g][i];
			const SpriteButton_t* b = st->btn;
			uint32_t now;
			if(!st->hasBitmap)
				continue;
			// the frame animation: the first pass only schedules the second frame
			if(b->frames > 1 && b->frameMs > 0 && SpriteAnimates(o, p, g, i))
			{
				if(st->nextFrame == 0)
				{
					st->frame = 0;
					st->nextFrame = GetTicks(o) + (uint32_t)b->frameMs;
				}
				else if(GetTicks(o) >= st->nextFrame)
				{
					int32_t no;
					if(++st->frame == b->frames)
						st->frame = 0;
					st->nextFrame += (uint32_t)b->frameMs; // from the due time, not from now: no drift
					PANEL_VT(p)->getBitmap(o, p, &no, g, i, SpriteIsFocused(p, g, i), 1);
					SpritePanel_ShowSub(o, sp, st->sub, no);
				}
			}
			if(b->swingPhases <= 0)
				continue;
			if(b->swingFocusOnly)
			{
				int on = p->focus != -1 && PANEL_VT(p)->canDecide(o, p, g, i) && SpriteIsFocused(p, g, i);
				if(!on)
				{
					if(st->nextSwing == 0) // at rest already
						continue;
					if(b->swingPause) // keep the state, push the next step ahead
					{
						st->nextSwing = GetTicks(o) + 4;
						continue;
					}
					SpritePanel_Rotate(o, sp, st->sub, 0); // back to rest
					st->nextSwing = 0;
					continue;
				}
			}
			if(st->nextSwing == 0) // (re)start the swing from its first phase
			{
				st->phase = 0;
				st->angle = b->swingAngle0;
				st->step = 0;
				st->nextSwing = GetTicks(o) + 4;
				continue;
			}
			now = GetTicks(o);
			if(st->nextSwing > now)
				continue;
			if(st->step == 0) // entering a phase: its duration in 4 ms steps, at least one
			{
				int steps = b->swing[st->phase][1] / 4;
				st->steps = steps > 0 ? steps : 1;
			}
			// one step: the phase's angle, interpolated linearly, on top of the phase's start angle
			st->step++;
			SpritePanel_Rotate(o, sp, st->sub,
				st->angle + (int32_t)(((int64_t)b->swing[st->phase][0] * st->step) / st->steps));
			if(st->step == st->steps) // the phase is complete: the next one starts from its end angle
			{
				st->angle = (st->angle + b->swing[st->phase][0]) % SWING_FULL_TURN;
				if(++st->phase == b->swingPhases)
					st->phase = 0;
				st->step = 0;
			}
			st->nextSwing += 4; // at most one step per pass; a swing that fell behind catches up a step per pass
		}
	}
}
