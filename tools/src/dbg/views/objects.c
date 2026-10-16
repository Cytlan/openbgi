/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * objects.c - the Objects view of the debugger (dbg_internal.h)
 *
 * The display objects of the graphics manager, class by class (the
 * background, sprites, windows, maps, filters, effectors, particle and
 * rain screens, knobs, groups), as a list on the left - class, slot id,
 * visibility, priority, position, size; invisible ones dimmed - and the
 * selected object's fields on the right: enabled, visible, priority,
 * level, fade, effect, position, offsets, the 16.16 position and depth,
 * progress, mask, owner, and its bitmap drawn below (DbgDraw_Bitmap).
 * The list pane is 120 pixels wider than the usual left pane.
 */
#include "../dbg_internal.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/dispobj.h"

static DbgList_t gObjList;          // the object list
static DispObj_t* gObjRows[1024];   // the objects behind the rows, rebuilt every draw
static const char* gObjClass[1024]; // the class each row was collected from

// the name of a DispObj class order (dispobj.h), for the title of the selected object
static const char* ClassName(int order)
{
	static const char* const names[] = {"background", "map", "sprite", "window", "particle", "rain", "effector", "filter",
		"virtual", "group", "knob"};
	return order >= 0 && order < 11 ? names[order] : "?";
}

// append the live objects of one manager array to the rows, labelled `cls`
static void Collect(DispObj_t* const* arr, int count, const char* cls, int* n)
{
	int i;
	for(i = 0; i < count && *n < 1024; i++)
		if(arr[i])
		{
			gObjRows[*n] = arr[i];
			gObjClass[*n] = cls;
			(*n)++;
		}
}

// collect the objects, draw the list, then the selected object's fields and bitmap
static void ObjectsDraw(int x, int y, int w, int h)
{
	int n = 0, i;
	int rx = x + DBG_LEFT_W + DBG_PAD, rw = w - DBG_LEFT_W - 2 * DBG_PAD;
	char title[0x80];
	if(gGfx)
	{
		if(gGfx->background)
		{
			gObjRows[n] = (DispObj_t*)gGfx->background;
			gObjClass[n++] = "background";
		}
		Collect(gGfx->sprites, GFX_SPRITES, "sprite", &n);
		Collect(gGfx->windows, GFX_WINDOWS, "window", &n);
		Collect(gGfx->maps, GFX_MAPS, "map", &n);
		Collect(gGfx->filters, GFX_FILTERS, "filter", &n);
		Collect(gGfx->effectors, GFX_EFFECTORS, "effector", &n);
		Collect(gGfx->particles, GFX_PARTICLES, "particle", &n);
		Collect(gGfx->rains, GFX_RAINS, "rain", &n);
		Collect(gGfx->knobs, GFX_KNOBS, "knob", &n);
		Collect(gGfx->groups, GFX_GROUPS, "group", &n);
	}
	gObjList.count = n;
	snprintf(title, sizeof title, "class        id vis  pri      x,y      size   (%d objects)", n);
	DbgPane_Frame(x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W + 120, h - 2 * DBG_PAD, title);
	DbgPane_ListArea(&gObjList, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W + 120, h - 2 * DBG_PAD);
	rx = x + DBG_LEFT_W + 120 + 2 * DBG_PAD;
	rw = w - DBG_LEFT_W - 120 - 3 * DBG_PAD;
	if(gObjList.sel < 0 && n)
		gObjList.sel = 0;
	for(i = gObjList.top; i < gObjList.top + gObjList.rows && i < n; i++)
	{
		DispObj_t* o = gObjRows[i];
		if(i == gObjList.sel)
			DbgPane_RowBg(&gObjList, i, DBG_SELECT);
		DbgDraw_TextF(gObjList.x + 4, DbgPane_RowY(&gObjList, i), o->visible ? DBG_TEXT : DBG_DIM, "%-10s %4d %s %4d %5d,%-5d %4dx%-4d",
			gObjClass[i], o->slotId, o->visible ? "on " : "off", o->priority, o->x, o->y, o->bmp.w, o->bmp.h);
	}
	if(gObjList.sel >= 0 && gObjList.sel < n)
	{
		DispObj_t* o = gObjRows[gObjList.sel];
		int ly = y + DBG_PAD + DBG_CELL_H + 6;
		snprintf(title, sizeof title, "%s %d (%s)", gObjClass[gObjList.sel], o->slotId, ClassName(o->classOrder));
		DbgPane_Frame(rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, title);
		DbgDraw_TextF(rx + DBG_PAD, ly, DBG_TEXT, "enabled %d  visible %d  priority %d  level %d  fade %d  effect 0x%x", o->enabled,
			o->visible, o->priority, o->level, o->fade, (unsigned)o->effect);
		DbgDraw_TextF(rx + DBG_PAD, ly + DBG_CELL_H, DBG_TEXT, "position %d,%d  offset %d,%d  fixed %d.%04x,%d.%04x depth %d.%04x",
			o->x, o->y, o->offX, o->offY, o->fx >> 16, (unsigned)(o->fx & 0xffff), o->fy >> 16, (unsigned)(o->fy & 0xffff),
			o->fz >> 16, (unsigned)(o->fz & 0xffff));
		DbgDraw_TextF(rx + DBG_PAD, ly + 2 * DBG_CELL_H, DBG_TEXT, "size %dx%d %s  progress %d.%02x  mask %s  owner %s", o->bmp.w,
			o->bmp.h, Dbg_BmpModeName(o->bmp.mode), o->progress >> 16, (unsigned)((o->progress >> 8) & 0xff),
			o->maskBits ? "yes" : "rect", o->owner ? "yes" : "-");
		if(o->bmp.pixels && (o->bmp.bpp == 4 || o->bmp.bpp == 2 || o->bmp.bpp == 1))
		{
			DbgDraw_Clip(rx + 2, ly + 3 * DBG_CELL_H + 4, rw - 4, h - 2 * DBG_PAD - 4 * DBG_CELL_H - 12);
			DbgDraw_Bitmap(rx + 4, ly + 3 * DBG_CELL_H + 6, rw - 8, h - 2 * DBG_PAD - 4 * DBG_CELL_H - 16, &o->bmp);
			DbgDraw_Clip(0, 0, 0, 0);
		}
	}
	else
		DbgPane_Frame(rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, "no object");
}

// the navigation keys move the selection
static int ObjectsKey(int vk, int ch)
{
	DbgList_Key(&gObjList, vk);
	return 1;
}

// a click on a row selects it
static int ObjectsClick(int x, int y, int button)
{
	int row = DbgList_Hit(&gObjList, x, y);
	if(row >= 0)
		gObjList.sel = row;
	return row >= 0;
}

// the wheel scrolls the list, wherever the mouse is
static int ObjectsWheel(int x, int y, int delta)
{
	DbgList_Scroll(&gObjList, delta > 0 ? -3 : 3);
	return 1;
}

const DbgViewOps_t kDbgViewObjects = {"Objects", ObjectsDraw, ObjectsKey, ObjectsClick, ObjectsWheel};
