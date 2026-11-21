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
#include "bgi/core.h"

// the name of a DispObj class order (dispobj.h), for the title of the selected object
static const char* ClassName(int order)
{
	static const char* const names[] = {"background", "map", "sprite", "window", "particle", "rain", "effector", "filter",
		"virtual", "group", "knob"};
	return order >= 0 && order < 11 ? names[order] : "?";
}

// append the live objects of one manager array to the rows, labelled `cls`
static void Collect(OpenBGI_t* o, DispObj_t* const* arr, int count, const char* cls, int* n)
{
	int i;
	for(i = 0; i < count && *n < 1024; i++)
		if(arr[i])
		{
			o->dbg.objRows[*n] = arr[i];
			o->dbg.objClass[*n] = cls;
			(*n)++;
		}
}

// collect the objects, draw the list, then the selected object's fields and bitmap
static void ObjectsDraw(OpenBGI_t* o, int x, int y, int w, int h)
{
	int n = 0, i;
	int rx = x + DBG_LEFT_W + DBG_PAD, rw = w - DBG_LEFT_W - 2 * DBG_PAD;
	char title[0x80];
	if(o->gfx.gfx)
	{
		if(o->gfx.gfx->background)
		{
			o->dbg.objRows[n] = (DispObj_t*)o->gfx.gfx->background;
			o->dbg.objClass[n++] = "background";
		}
		Collect(o, o->gfx.gfx->sprites, GFX_SPRITES, "sprite", &n);
		Collect(o, o->gfx.gfx->windows, GFX_WINDOWS, "window", &n);
		Collect(o, o->gfx.gfx->maps, GFX_MAPS, "map", &n);
		Collect(o, o->gfx.gfx->filters, GFX_FILTERS, "filter", &n);
		Collect(o, o->gfx.gfx->effectors, GFX_EFFECTORS, "effector", &n);
		Collect(o, o->gfx.gfx->particles, GFX_PARTICLES, "particle", &n);
		Collect(o, o->gfx.gfx->rains, GFX_RAINS, "rain", &n);
		Collect(o, o->gfx.gfx->knobs, GFX_KNOBS, "knob", &n);
		Collect(o, o->gfx.gfx->groups, GFX_GROUPS, "group", &n);
	}
	o->dbg.objList.count = n;
	snprintf(title, sizeof title, "class        id vis  pri      x,y      size   (%d objects)", n);
	DbgPane_Frame(o, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W + 120, h - 2 * DBG_PAD, title);
	DbgPane_ListArea(&o->dbg.objList, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W + 120, h - 2 * DBG_PAD);
	rx = x + DBG_LEFT_W + 120 + 2 * DBG_PAD;
	rw = w - DBG_LEFT_W - 120 - 3 * DBG_PAD;
	if(o->dbg.objList.sel < 0 && n)
		o->dbg.objList.sel = 0;
	for(i = o->dbg.objList.top; i < o->dbg.objList.top + o->dbg.objList.rows && i < n; i++)
	{
		DispObj_t* obj = o->dbg.objRows[i];
		if(i == o->dbg.objList.sel)
			DbgPane_RowBg(o, &o->dbg.objList, i, DBG_SELECT);
		DbgDraw_TextF(o, o->dbg.objList.x + 4, DbgPane_RowY(&o->dbg.objList, i), obj->visible ? DBG_TEXT : DBG_DIM, "%-10s %4d %s %4d %5d,%-5d %4dx%-4d",
			o->dbg.objClass[i], obj->slotId, obj->visible ? "on " : "off", obj->priority, obj->x, obj->y, obj->bmp.w, obj->bmp.h);
	}
	if(o->dbg.objList.sel >= 0 && o->dbg.objList.sel < n)
	{
		DispObj_t* obj = o->dbg.objRows[o->dbg.objList.sel];
		int ly = y + DBG_PAD + DBG_CELL_H + 6;
		snprintf(title, sizeof title, "%s %d (%s)", o->dbg.objClass[o->dbg.objList.sel], obj->slotId, ClassName(obj->classOrder));
		DbgPane_Frame(o, rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, title);
		DbgDraw_TextF(o, rx + DBG_PAD, ly, DBG_TEXT, "enabled %d  visible %d  priority %d  level %d  fade %d  effect 0x%x", obj->enabled,
			obj->visible, obj->priority, obj->level, obj->fade, (unsigned)obj->effect);
		DbgDraw_TextF(o, rx + DBG_PAD, ly + DBG_CELL_H, DBG_TEXT, "position %d,%d  offset %d,%d  fixed %d.%04x,%d.%04x depth %d.%04x",
			obj->x, obj->y, obj->offX, obj->offY, obj->fx >> 16, (unsigned)(obj->fx & 0xffff), obj->fy >> 16, (unsigned)(obj->fy & 0xffff),
			obj->fz >> 16, (unsigned)(obj->fz & 0xffff));
		DbgDraw_TextF(o, rx + DBG_PAD, ly + 2 * DBG_CELL_H, DBG_TEXT, "size %dx%d %s  progress %d.%02x  mask %s  owner %s", obj->bmp.w,
			obj->bmp.h, Dbg_BmpModeName(obj->bmp.mode), obj->progress >> 16, (unsigned)((obj->progress >> 8) & 0xff),
			obj->maskBits ? "yes" : "rect", obj->owner ? "yes" : "-");
		if(obj->bmp.pixels && (obj->bmp.bpp == 4 || obj->bmp.bpp == 2 || obj->bmp.bpp == 1))
		{
			DbgDraw_Clip(o, rx + 2, ly + 3 * DBG_CELL_H + 4, rw - 4, h - 2 * DBG_PAD - 4 * DBG_CELL_H - 12);
			DbgDraw_Bitmap(o, rx + 4, ly + 3 * DBG_CELL_H + 6, rw - 8, h - 2 * DBG_PAD - 4 * DBG_CELL_H - 16, &obj->bmp);
			DbgDraw_Clip(o, 0, 0, 0, 0);
		}
	}
	else
		DbgPane_Frame(o, rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, "no object");
}

// the navigation keys move the selection
static int ObjectsKey(OpenBGI_t* o, int vk, int ch)
{
	DbgList_Key(&o->dbg.objList, vk);
	return 1;
}

// a click on a row selects it
static int ObjectsClick(OpenBGI_t* o, int x, int y, int button)
{
	int row = DbgList_Hit(&o->dbg.objList, x, y);
	if(row >= 0)
		o->dbg.objList.sel = row;
	return row >= 0;
}

// the wheel scrolls the list, wherever the mouse is
static int ObjectsWheel(OpenBGI_t* o, int x, int y, int delta)
{
	DbgList_Scroll(&o->dbg.objList, delta > 0 ? -3 : 3);
	return 1;
}

const DbgViewOps_t kDbgViewObjects = {"Objects", ObjectsDraw, ObjectsKey, ObjectsClick, ObjectsWheel};
