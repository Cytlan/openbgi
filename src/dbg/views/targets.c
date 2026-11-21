/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * targets.c - the Targets view of the debugger (dbg_internal.h)
 *
 * What a click reaches now (OpenBGI_Targets of openbgi.h), as a list on
 * the left - kind, owner, group and index, rectangle, flags; the topmost
 * first - and on the right the game's picture (the compositor's back
 * buffer, which the debugger drawn over the game's frames does not
 * touch; the targets' pixels are mapped to it) with the targets' outlines:
 * the selected one in the accent colour with its click point marked (and
 * a knob's travel), those that take no click now dimmed; the targets a
 * click anywhere reaches are outlined only while selected.  Below the
 * picture the selected target's layer key, click point and owner, and
 * the last tap of the touch screen (OpenBGI_Touch): where the finger was
 * (a small square on the picture) and where it clicked (a cross), with
 * the target there; then which device the player uses, and for a pad what
 * it drives (the target its focus is on).  A click on the picture selects
 * the topmost target outlined there.
 */
#include "../dbg_internal.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/inputmap.h"
#include "bgi/core.h"

#define TARGETS_LIST_W (DBG_LEFT_W + 140) // the list pane is wider than the usual left pane
#define OUTLINE        0x50a0f0u          // a target's outline on the picture
#define OUTLINE_OFF    0x707070u          // ... when it takes no click now

static const char* KindName(int kind)
{
	static const char* const names[] = {"?", "text", "wait", "layer", "raw", "button", "panel", "item", "icon", "scroll", "knob", "sprite",
		"probe", "entry", "hotspot"};
	return kind > 0 && kind < (int)(sizeof names / sizeof names[0]) ? names[kind] : "?";
}

// the flags as letters: Anywhere, Disabled, Covered, Hover, Selected, Grabbed (held), Probed
static void FlagLetters(uint32_t f, char out[8])
{
	static const char letters[] = "ADCHSGP";
	int i, n = 0;
	for(i = 0; i < 7; i++)
		if(f & (1u << i))
			out[n++] = letters[i];
	out[n] = 0;
}

/* where the picture is drawn in the right pane: (x, y) and the scale
 * DbgDraw_Bitmap uses (never enlarged) */
static double PictureScale(const Bmp_t* b, int w, int h)
{
	double sx, sy, s;
	if(!b->pixels || b->w <= 0 || b->h <= 0 || w <= 0 || h <= 0)
		return 0;
	sx = (double)w / b->w;
	sy = (double)h / b->h;
	s = sx < sy ? sx : sy;
	return s > 1.0 ? 1.0 : s;
}

// the picture pane's place: the area below the title, above the six detail lines
static void PicturePane(int x, int y, int w, int h, int* px, int* py, int* pw, int* ph)
{
	int rx = x + TARGETS_LIST_W + 2 * DBG_PAD;
	*px = rx + 4;
	*py = y + DBG_PAD + DBG_CELL_H + 6;
	*pw = w - TARGETS_LIST_W - 3 * DBG_PAD - 8;
	*ph = h - 2 * DBG_PAD - DBG_CELL_H - 12 - 6 * DBG_CELL_H;
}

// a pixel of the frame's picture in the back buffer, and back (Gfx_MapPoint)
static void ToBack(OpenBGI_t* o, int x, int y, int out[2])
{
	int32_t p[2] = {x, y};
	Gfx_MapPoint(o->gfx.gfx, p, x, y, 1);
	out[0] = p[0];
	out[1] = p[1];
}

static void ToPicture(OpenBGI_t* o, int x, int y, int out[2])
{
	int32_t p[2] = {x, y};
	Gfx_MapPoint(o->gfx.gfx, p, x, y, 0);
	out[0] = p[0];
	out[1] = p[1];
}

// a rectangle of the picture's pixels as an outline over the scaled back buffer drawn at (px, py)
static void Outline(OpenBGI_t* o, int px, int py, double s, const OpenBGI_Rect_t* r, uint32_t rgb)
{
	int a[2], b[2], w, h;
	ToBack(o, r->x, r->y, a);
	ToBack(o, r->x + r->w - 1, r->y + r->h - 1, b);
	w = (int)((b[0] - a[0] + 1) * s);
	h = (int)((b[1] - a[1] + 1) * s);
	DbgDraw_Frame(o, px + (int)(a[0] * s), py + (int)(a[1] * s), w > 1 ? w : 1, h > 1 ? h : 1, rgb);
}

// a pixel of the frame's picture on the scaled back buffer drawn at (px, py)
static void OnPane(OpenBGI_t* o, int px, int py, double s, int x, int y, int out[2])
{
	ToBack(o, x, y, out);
	out[0] = px + (int)(out[0] * s);
	out[1] = py + (int)(out[1] * s);
}

// the last tap: a square where the finger was, a cross where it clicked; a line of text below the picture at ly
static void LastTap(OpenBGI_t* o, int px, int py, double s, int tx, int ly)
{
	const TouchState_t* t = &o->input.touch;
	int a[2], b[2];
	if(!t->tapValid)
		return;
	OnPane(o, px, py, s, t->tapX, t->tapY, a);
	OnPane(o, px, py, s, t->tapToX, t->tapToY, b);
	DbgDraw_Frame(o, a[0] - 3, a[1] - 3, 7, 7, DBG_GREEN);
	DbgDraw_HLine(o, b[0] - 5, b[1], 11, DBG_GREEN);
	DbgDraw_VLine(o, b[0], b[1] - 5, 11, DBG_GREEN);
	DbgDraw_Clip(o, 0, 0, 0, 0);
	if(!t->tapKind)
		DbgDraw_TextF(o, tx, ly, DBG_DIM, "last tap at %d,%d: nothing there", t->tapX, t->tapY);
	else
		DbgDraw_TextF(o, tx, ly, DBG_GREEN, "last tap at %d,%d: clicked %d,%d, %s owner %u%s", t->tapX, t->tapY, t->tapToX, t->tapToY,
			KindName(t->tapKind), (unsigned)t->tapOwner, t->tapToX != t->tapX || t->tapToY != t->tapY ? " (snapped)" : "");
}

// what a pad drives now, and the on-screen keyboard: a line of text at ly
static void PadLine(OpenBGI_t* o, int tx, int ly)
{
	static const char* const kDevice[] = {"none yet", "mouse", "keyboard", "touch", "pad"};
	const PadMap_t* p = &o->input.pad;
	const char* osk = Osk_Up(o) ? ", the on-screen keyboard up" : "";
	if(o->input.device != INPUT_DEV_PAD)
		DbgDraw_TextF(o, tx, ly, DBG_DIM, "device: %s%s", kDevice[o->input.device <= INPUT_DEV_PAD ? o->input.device : 0], osk);
	else if(p->mode == PAD_MODE_FOCUS)
		DbgDraw_TextF(o, tx, ly, DBG_TEXT, "device: pad, focus on %s owner %u %d/%d%s", KindName(p->focusKind), (unsigned)p->focusOwner, p->focusGroup,
			p->focusIndex, osk);
	else
		DbgDraw_TextF(o, tx, ly, DBG_TEXT, "device: pad, %s%s", Pad_ModeName(o), osk);
}

// gather, draw the list, the picture with the outlines and the selected target's details
static void TargetsDraw(OpenBGI_t* o, int x, int y, int w, int h)
{
	const Bmp_t* pic = o->gfx.gfx ? &o->gfx.gfx->back : NULL;
	int n = InputMap_Targets(o, o->dbg.targetRows, 256), i, px, py, pw, ph, rx, rw;
	char title[0x80], fl[8];
	double s;
	if(n > 256)
		n = 256;
	o->dbg.targetList.count = n;
	snprintf(title, sizeof title, "kind     owner  grp/idx        x,y      size  flags  (%d targets)", n);
	DbgPane_Frame(o, x + DBG_PAD, y + DBG_PAD, TARGETS_LIST_W, h - 2 * DBG_PAD, title);
	DbgPane_ListArea(&o->dbg.targetList, x + DBG_PAD, y + DBG_PAD, TARGETS_LIST_W, h - 2 * DBG_PAD);
	DbgList_Clamp(&o->dbg.targetList);
	if(o->dbg.targetList.sel < 0 && n)
		o->dbg.targetList.sel = 0;
	for(i = o->dbg.targetList.top; i < o->dbg.targetList.top + o->dbg.targetList.rows && i < n; i++)
	{
		const OpenBGI_Target_t* t = &o->dbg.targetRows[i];
		uint32_t rgb = t->flags & (OPENBGI_TARGET_DISABLED | OPENBGI_TARGET_COVERED) ? DBG_DIM : DBG_TEXT;
		if(i == o->dbg.targetList.sel)
			DbgPane_RowBg(o, &o->dbg.targetList, i, DBG_SELECT);
		FlagLetters(t->flags, fl);
		DbgDraw_TextF(o, o->dbg.targetList.x + 4, DbgPane_RowY(&o->dbg.targetList, i), rgb, "%-7s %6u %4d/%-4d %5d,%-5d %4dx%-4d %s",
			KindName(t->kind), (unsigned)t->owner, t->group, t->index, t->rect.x, t->rect.y, t->rect.w, t->rect.h, fl);
	}

	// the picture with the outlines
	rx = x + TARGETS_LIST_W + 2 * DBG_PAD;
	rw = w - TARGETS_LIST_W - 3 * DBG_PAD;
	DbgPane_Frame(o, rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, "the picture");
	PicturePane(x, y, w, h, &px, &py, &pw, &ph);
	DbgDraw_TextF(o, rx + DBG_PAD, py + ph + 4 + 5 * DBG_CELL_H, DBG_DIM, "flags: A anywhere D disabled C covered H hover S selected G held P probed");
	PadLine(o, rx + DBG_PAD, py + ph + 4 + 4 * DBG_CELL_H);
	s = pic ? PictureScale(pic, pw, ph) : 0;
	o->dbg.targetPicX = px; // (for the clicks)
	o->dbg.targetPicY = py;
	o->dbg.targetPicScale = s;
	if(s <= 0)
		return;
	DbgDraw_Clip(o, px, py, pw, ph);
	DbgDraw_Bitmap(o, px, py, pw, ph, pic);
	for(i = n - 1; i >= 0; i--) // the topmost drawn last
	{
		const OpenBGI_Target_t* t = &o->dbg.targetRows[i];
		if(i == o->dbg.targetList.sel || (t->flags & OPENBGI_TARGET_ANYWHERE))
			continue;
		Outline(o, px, py, s, &t->rect, t->flags & (OPENBGI_TARGET_DISABLED | OPENBGI_TARGET_COVERED) ? OUTLINE_OFF : OUTLINE);
	}
	if(o->dbg.targetList.sel >= 0 && o->dbg.targetList.sel < n)
	{
		const OpenBGI_Target_t* t = &o->dbg.targetRows[o->dbg.targetList.sel];
		int ly = py + ph + 4;
		if(t->kind == OPENBGI_TARGET_KNOB)
			Outline(o, px, py, s, &t->drag, DBG_GREEN);
		Outline(o, px, py, s, &t->rect, DBG_ACCENT);
		if(t->clickX >= 0)
		{
			int c[2], cx, cy;
			ToBack(o, t->clickX, t->clickY, c);
			cx = px + (int)(c[0] * s);
			cy = py + (int)(c[1] * s);
			DbgDraw_HLine(o, cx - 5, cy, 11, DBG_RED);
			DbgDraw_VLine(o, cx, cy - 5, 11, DBG_RED);
		}
		DbgDraw_Clip(o, 0, 0, 0, 0);
		DbgDraw_TextF(o, rx + DBG_PAD, ly, DBG_TEXT, "%s  owner %u  group %d  index %d  layer 0x%08x", KindName(t->kind), (unsigned)t->owner,
			t->group, t->index, (unsigned)t->layer);
		if(t->clickX >= 0)
			DbgDraw_TextF(o, rx + DBG_PAD, ly + DBG_CELL_H, DBG_TEXT, "rectangle %d,%d %dx%d  click at %d,%d", t->rect.x, t->rect.y, t->rect.w,
				t->rect.h, t->clickX, t->clickY);
		else
			DbgDraw_TextF(o, rx + DBG_PAD, ly + DBG_CELL_H, DBG_DIM, "rectangle %d,%d %dx%d  covered: no click reaches it", t->rect.x, t->rect.y,
				t->rect.w, t->rect.h);
		if(t->kind == OPENBGI_TARGET_KNOB)
			DbgDraw_TextF(o, rx + DBG_PAD, ly + 2 * DBG_CELL_H, DBG_TEXT, "travel %d,%d %dx%d", t->drag.x, t->drag.y, t->drag.w, t->drag.h);
		DbgDraw_Clip(o, px, py, pw, ph);
	}
	LastTap(o, px, py, s, rx + DBG_PAD, py + ph + 4 + 3 * DBG_CELL_H);
	DbgDraw_Clip(o, 0, 0, 0, 0);
}

// the navigation keys move the selection
static int TargetsKey(OpenBGI_t* o, int vk, int ch)
{
	DbgList_Key(&o->dbg.targetList, vk);
	return 1;
}

/* a click on a row selects it; one on the picture the topmost target
 * outlined under it (the ones a click anywhere reaches last) */
static int TargetsClick(OpenBGI_t* o, int x, int y, int button)
{
	const Bmp_t* pic = o->gfx.gfx ? &o->gfx.gfx->back : NULL;
	int row = DbgList_Hit(&o->dbg.targetList, x, y), px = o->dbg.targetPicX, py = o->dbg.targetPicY, i, pass, p[2];
	double s = o->dbg.targetPicScale;
	if(row >= 0)
	{
		o->dbg.targetList.sel = row;
		return 1;
	}
	if(!pic || s <= 0 || x < px || y < py || x >= px + (int)(pic->w * s) || y >= py + (int)(pic->h * s))
		return 0;
	ToPicture(o, (int)((x - px) / s), (int)((y - py) / s), p);
	x = p[0];
	y = p[1];
	for(pass = 0; pass < 2; pass++)
		for(i = 0; i < o->dbg.targetList.count; i++)
		{
			const OpenBGI_Target_t* t = &o->dbg.targetRows[i];
			if(!(t->flags & OPENBGI_TARGET_ANYWHERE) != !pass && x >= t->rect.x && x < t->rect.x + t->rect.w && y >= t->rect.y &&
				y < t->rect.y + t->rect.h)
			{
				o->dbg.targetList.sel = i;
				DbgList_Show(&o->dbg.targetList, i);
				return 1;
			}
		}
	return 1;
}

// the wheel scrolls the list, wherever the mouse is
static int TargetsWheel(OpenBGI_t* o, int x, int y, int delta)
{
	DbgList_Scroll(&o->dbg.targetList, delta > 0 ? -3 : 3);
	return 1;
}

const DbgViewOps_t kDbgViewTargets = {"Targets", TargetsDraw, TargetsKey, TargetsClick, TargetsWheel};
