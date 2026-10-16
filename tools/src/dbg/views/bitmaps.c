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

static DbgList_t gBmpList; // the slot list
static int gBmpRows[4096]; // the slot numbers behind the rows, rebuilt every draw

// the list of the slots in use, then the selected slot's picture (when it has pixels of a drawable depth)
static void BitmapsDraw(int x, int y, int w, int h)
{
	int i, n = 0;
	int rx = x + DBG_LEFT_W + DBG_PAD, rw = w - DBG_LEFT_W - 2 * DBG_PAD;
	char title[0x80];
	if(gBmpMgr)
		for(i = 0; i < gBmpMgr->count && n < 4096; i++)
			if(gBmpMgr->slots[i].buf)
				gBmpRows[n++] = i;
	gBmpList.count = n;
	snprintf(title, sizeof title, "  no  size       mode    gen   (%d of %d slots)", n, gBmpMgr ? gBmpMgr->count : 0);
	DbgPane_Frame(x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD, title);
	DbgPane_ListArea(&gBmpList, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD);
	if(gBmpList.sel < 0 && n)
		gBmpList.sel = 0;
	for(i = gBmpList.top; i < gBmpList.top + gBmpList.rows && i < n; i++)
	{
		const BmpSlot_t* s = &gBmpMgr->slots[gBmpRows[i]];
		if(i == gBmpList.sel)
			DbgPane_RowBg(&gBmpList, i, DBG_SELECT);
		DbgDraw_TextF(gBmpList.x + 4, DbgPane_RowY(&gBmpList, i), DBG_TEXT, "%4d %5dx%-5d %-7s %d%s", gBmpRows[i], s->bmp.w, s->bmp.h,
			Dbg_BmpModeName(s->bmp.mode), s->generation, s->bmp.pixels ? "" : " (virtual)");
	}
	if(gBmpList.sel >= 0 && gBmpList.sel < n)
	{
		const BmpSlot_t* s = &gBmpMgr->slots[gBmpRows[gBmpList.sel]];
		snprintf(title, sizeof title, "slot %d: %dx%d %s, pitch %d, %d bytes per pixel%s", gBmpRows[gBmpList.sel], s->bmp.w,
			s->bmp.h, Dbg_BmpModeName(s->bmp.mode), s->bmp.pitch, s->bmp.bpp, s->bmp.pixels ? "" : ", no pixels");
		DbgPane_Frame(rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, title);
		if(s->bmp.pixels && (s->bmp.bpp == 4 || s->bmp.bpp == 2 || s->bmp.bpp == 1))
		{
			DbgDraw_Clip(rx + 2, y + DBG_PAD + DBG_CELL_H + 4, rw - 4, h - 2 * DBG_PAD - DBG_CELL_H - 6);
			DbgDraw_Bitmap(rx + 4, y + DBG_PAD + DBG_CELL_H + 6, rw - 8, h - 2 * DBG_PAD - DBG_CELL_H - 10, &s->bmp);
			DbgDraw_Clip(0, 0, 0, 0);
		}
	}
	else
		DbgPane_Frame(rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, "no bitmap");
}

// the navigation keys move the selection
static int BitmapsKey(int vk, int ch)
{
	DbgList_Key(&gBmpList, vk);
	return 1;
}

// a click on a row selects it
static int BitmapsClick(int x, int y, int button)
{
	int row = DbgList_Hit(&gBmpList, x, y);
	if(row >= 0)
		gBmpList.sel = row;
	return row >= 0;
}

// the wheel scrolls the list, wherever the mouse is
static int BitmapsWheel(int x, int y, int delta)
{
	DbgList_Scroll(&gBmpList, delta > 0 ? -3 : 3);
	return 1;
}

const DbgViewOps_t kDbgViewBitmaps = {"Bitmaps", BitmapsDraw, BitmapsKey, BitmapsClick, BitmapsWheel};
