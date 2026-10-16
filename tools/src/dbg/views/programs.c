/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * programs.c - the Programs view of the debugger (dbg_internal.h)
 *
 * The modules the selected thread (gDbgThreadId) has loaded as a list on
 * the left - name, base in the thread's code area, size; the one holding
 * the instruction pointer accented - and the selected module's whole
 * listing on the right with the current position and the breakpoints
 * marked (F9 or a click on the margin toggles one).  The listing pane
 * starts with the keyboard focus.
 */
#include "../dbg_internal.h"
#include "bgi/vm.h"

static DbgList_t gModuleList;            // the module list
static int gProgramsFocus = 1;           // 0 the module list, 1 the listing
static DbgListingPane_t gProgramListing; // the selected module's listing
static ModuleNode_t* gModuleRows[64];    // the modules behind the rows, rebuilt every draw

// the list, then the selected module's listing
static void ProgramsDraw(int x, int y, int w, int h)
{
	Thread_t* t = Dbg_SelectedThread();
	ModuleNode_t* m;
	int n = 0, i;
	int rx = x + DBG_LEFT_W + DBG_PAD, rw = w - DBG_LEFT_W - 2 * DBG_PAD;
	char title[0x80];
	for(m = t ? t->modules : NULL; m && n < 64; m = m->next)
		gModuleRows[n++] = m;
	gModuleList.count = n;
	snprintf(title, sizeof title, "%-20s %6s %6s   (thread %u)", "module", "base", "size", t ? (unsigned)t->id : 0);
	DbgPane_Frame(x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD, title);
	if(gProgramsFocus == 0)
		DbgPane_Focus(x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD);
	DbgPane_ListArea(&gModuleList, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD);
	if(gModuleList.sel < 0 && n)
		gModuleList.sel = 0;
	for(i = gModuleList.top; i < gModuleList.top + gModuleList.rows && i < n; i++)
	{
		int cur = t && t->opIP >= gModuleRows[i]->base && t->opIP < gModuleRows[i]->base + gModuleRows[i]->size;
		if(i == gModuleList.sel)
			DbgPane_RowBg(&gModuleList, i, DBG_SELECT);
		DbgDraw_TextF(gModuleList.x + 4, DbgPane_RowY(&gModuleList, i), cur ? DBG_ACCENT : DBG_TEXT, "%-20.20s %06x %6u", gModuleRows[i]->name,
			(unsigned)gModuleRows[i]->base, (unsigned)gModuleRows[i]->size);
	}
	if(gModuleList.sel >= 0 && gModuleList.sel < n)
	{
		m = gModuleRows[gModuleList.sel];
		DbgListingPane_Show(&gProgramListing, t, m->name, m->base, m->size);
		snprintf(title, sizeof title, "%s  (click the margin or F9 for a breakpoint)", m->name);
		DbgListingPane_Draw(&gProgramListing, t, rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, title);
		if(gProgramsFocus == 1)
			DbgPane_Focus(rx, y + DBG_PAD, rw, h - 2 * DBG_PAD);
	}
	else
		DbgPane_Frame(rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, "no module");
}

// Left / Right choose the pane; F follows the instruction pointer again; the navigation keys move in the focused pane
static int ProgramsKey(int vk, int ch)
{
	if(DbgPane_IsFocusKey(vk, &gProgramsFocus))
		return 1;
	if(ch == 'f' || ch == 'F')
	{
		gProgramListing.follow = 1;
		return 1;
	}
	if(gProgramsFocus == 0 && DbgPane_IsNavKey(vk))
	{
		DbgList_Key(&gModuleList, vk);
		return 1;
	}
	return DbgListingPane_Key(&gProgramListing, vk);
}

// a click selects a module (and focuses the list) or goes to the listing pane (and focuses it)
static int ProgramsClick(int x, int y, int button)
{
	int row = DbgList_Hit(&gModuleList, x, y);
	if(row >= 0)
	{
		gModuleList.sel = row;
		gProgramsFocus = 0;
		return 1;
	}
	if(DbgListingPane_Click(&gProgramListing, x, y, button))
	{
		gProgramsFocus = 1;
		return 1;
	}
	return 0;
}

// the wheel scrolls the pane under the mouse (three rows at a time)
static int ProgramsWheel(int x, int y, int delta)
{
	if(DbgPane_Contains(x, y, gModuleList.x, gModuleList.y, gModuleList.w, gModuleList.h))
	{
		DbgList_Scroll(&gModuleList, delta > 0 ? -3 : 3);
		return 1;
	}
	return DbgListingPane_Wheel(&gProgramListing, x, y, delta);
}

const DbgViewOps_t kDbgViewPrograms = {"Programs", ProgramsDraw, ProgramsKey, ProgramsClick, ProgramsWheel};
