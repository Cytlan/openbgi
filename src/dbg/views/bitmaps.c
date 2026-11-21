/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bitmaps.c - the Bitmaps view of the debugger (dbg_internal.h)
 *
 * The bitmap manager's slots in use as a list on the left - slot number,
 * size, pixel mode, generation (the counter of the slot's last
 * allocation), virtual slots marked - and the selected slot's picture on
 * the right, scaled to fit and never enlarged (DbgDraw_Bitmap).
 */
#include "../dbg_internal.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/core.h"

// the list of the slots in use, then the selected slot's picture (when it has pixels of a drawable depth)
static void BitmapsDraw(OpenBGI_t* o, int x, int y, int w, int h)
{
	int i, n = 0;
	int rx = x + DBG_LEFT_W + DBG_PAD, rw = w - DBG_LEFT_W - 2 * DBG_PAD;
	char title[0x80];
	if(o->gfx.bmpMgr)
		for(i = 0; i < o->gfx.bmpMgr->count && n < 4096; i++)
			if(o->gfx.bmpMgr->slots[i].buf)
				o->dbg.bmpRows[n++] = i;
	o->dbg.bmpList.count = n;
	snprintf(title, sizeof title, "  no  size       mode    gen   (%d of %d slots)", n, o->gfx.bmpMgr ? o->gfx.bmpMgr->count : 0);
	DbgPane_Frame(o, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD, title);
	DbgPane_ListArea(&o->dbg.bmpList, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD);
	if(o->dbg.bmpList.sel < 0 && n)
		o->dbg.bmpList.sel = 0;
	for(i = o->dbg.bmpList.top; i < o->dbg.bmpList.top + o->dbg.bmpList.rows && i < n; i++)
	{
		const BmpSlot_t* s = &o->gfx.bmpMgr->slots[o->dbg.bmpRows[i]];
		if(i == o->dbg.bmpList.sel)
			DbgPane_RowBg(o, &o->dbg.bmpList, i, DBG_SELECT);
		DbgDraw_TextF(o, o->dbg.bmpList.x + 4, DbgPane_RowY(&o->dbg.bmpList, i), DBG_TEXT, "%4d %5dx%-5d %-7s %d%s", o->dbg.bmpRows[i], s->bmp.w, s->bmp.h,
			Dbg_BmpModeName(s->bmp.mode), s->generation, s->bmp.pixels ? "" : " (virtual)");
	}
	if(o->dbg.bmpList.sel >= 0 && o->dbg.bmpList.sel < n)
	{
		const BmpSlot_t* s = &o->gfx.bmpMgr->slots[o->dbg.bmpRows[o->dbg.bmpList.sel]];
		snprintf(title, sizeof title, "slot %d: %dx%d %s, pitch %d, %d bytes per pixel%s", o->dbg.bmpRows[o->dbg.bmpList.sel], s->bmp.w,
			s->bmp.h, Dbg_BmpModeName(s->bmp.mode), s->bmp.pitch, s->bmp.bpp, s->bmp.pixels ? "" : ", no pixels");
		DbgPane_Frame(o, rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, title);
		if(s->bmp.pixels && (s->bmp.bpp == 4 || s->bmp.bpp == 2 || s->bmp.bpp == 1))
		{
			DbgDraw_Clip(o, rx + 2, y + DBG_PAD + DBG_CELL_H + 4, rw - 4, h - 2 * DBG_PAD - DBG_CELL_H - 6);
			DbgDraw_Bitmap(o, rx + 4, y + DBG_PAD + DBG_CELL_H + 6, rw - 8, h - 2 * DBG_PAD - DBG_CELL_H - 10, &s->bmp);
			DbgDraw_Clip(o, 0, 0, 0, 0);
		}
	}
	else
		DbgPane_Frame(o, rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, "no bitmap");
}

// the navigation keys move the selection
static int BitmapsKey(OpenBGI_t* o, int vk, int ch)
{
	DbgList_Key(&o->dbg.bmpList, vk);
	return 1;
}

// a click on a row selects it
static int BitmapsClick(OpenBGI_t* o, int x, int y, int button)
{
	int row = DbgList_Hit(&o->dbg.bmpList, x, y);
	if(row >= 0)
		o->dbg.bmpList.sel = row;
	return row >= 0;
}

// the wheel scrolls the list, wherever the mouse is
static int BitmapsWheel(OpenBGI_t* o, int x, int y, int delta)
{
	DbgList_Scroll(&o->dbg.bmpList, delta > 0 ? -3 : 3);
	return 1;
}

const DbgViewOps_t kDbgViewBitmaps = {"Bitmaps", BitmapsDraw, BitmapsKey, BitmapsClick, BitmapsWheel};
