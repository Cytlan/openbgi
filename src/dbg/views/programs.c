/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * programs.c - the Programs view of the debugger (dbg_internal.h)
 *
 * The modules the selected thread (o->dbg.dbgThreadId) has loaded as a
 * list on the left - name, base in the thread's code area, size; the one
 * holding the instruction pointer accented - and the selected module's
 * whole listing on the right with the current position and the
 * breakpoints marked (F9 or a click on the margin toggles one).  The
 * listing pane starts with the keyboard focus.
 */
#include "../dbg_internal.h"
#include "bgi/vm.h"
#include "bgi/core.h"

// the list, then the selected module's listing
static void ProgramsDraw(OpenBGI_t* o, int x, int y, int w, int h)
{
	Thread_t* t = Dbg_SelectedThread(o);
	ModuleNode_t* m;
	int n = 0, i;
	int rx = x + DBG_LEFT_W + DBG_PAD, rw = w - DBG_LEFT_W - 2 * DBG_PAD;
	char title[0x80];
	for(m = t ? t->modules : NULL; m && n < 64; m = m->next)
		o->dbg.moduleRows[n++] = m;
	o->dbg.moduleList.count = n;
	snprintf(title, sizeof title, "%-20s %6s %6s   (thread %u)", "module", "base", "size", t ? (unsigned)t->id : 0);
	DbgPane_Frame(o, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD, title);
	if(o->dbg.programsFocus == 0)
		DbgPane_Focus(o, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD);
	DbgPane_ListArea(&o->dbg.moduleList, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD);
	if(o->dbg.moduleList.sel < 0 && n)
		o->dbg.moduleList.sel = 0;
	for(i = o->dbg.moduleList.top; i < o->dbg.moduleList.top + o->dbg.moduleList.rows && i < n; i++)
	{
		int cur = t && t->opIP >= o->dbg.moduleRows[i]->base && t->opIP < o->dbg.moduleRows[i]->base + o->dbg.moduleRows[i]->size;
		if(i == o->dbg.moduleList.sel)
			DbgPane_RowBg(o, &o->dbg.moduleList, i, DBG_SELECT);
		DbgDraw_TextF(o, o->dbg.moduleList.x + 4, DbgPane_RowY(&o->dbg.moduleList, i), cur ? DBG_ACCENT : DBG_TEXT, "%-20.20s %06x %6u", o->dbg.moduleRows[i]->name,
			(unsigned)o->dbg.moduleRows[i]->base, (unsigned)o->dbg.moduleRows[i]->size);
	}
	if(o->dbg.moduleList.sel >= 0 && o->dbg.moduleList.sel < n)
	{
		m = o->dbg.moduleRows[o->dbg.moduleList.sel];
		DbgListingPane_Show(o, &o->dbg.programListing, t, m->name, m->base, m->size);
		snprintf(title, sizeof title, "%s  (click the margin or F9 for a breakpoint)", m->name);
		DbgListingPane_Draw(o, &o->dbg.programListing, t, rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, title);
		if(o->dbg.programsFocus == 1)
			DbgPane_Focus(o, rx, y + DBG_PAD, rw, h - 2 * DBG_PAD);
	}
	else
		DbgPane_Frame(o, rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, "no module");
}

// Left / Right choose the pane; F follows the instruction pointer again; the navigation keys move in the focused pane
static int ProgramsKey(OpenBGI_t* o, int vk, int ch)
{
	if(DbgPane_IsFocusKey(vk, &o->dbg.programsFocus))
		return 1;
	if(ch == 'f' || ch == 'F')
	{
		o->dbg.programListing.follow = 1;
		return 1;
	}
	if(o->dbg.programsFocus == 0 && DbgPane_IsNavKey(vk))
	{
		DbgList_Key(&o->dbg.moduleList, vk);
		return 1;
	}
	return DbgListingPane_Key(o, &o->dbg.programListing, vk);
}

// a click selects a module (and focuses the list) or goes to the listing pane (and focuses it)
static int ProgramsClick(OpenBGI_t* o, int x, int y, int button)
{
	int row = DbgList_Hit(&o->dbg.moduleList, x, y);
	if(row >= 0)
	{
		o->dbg.moduleList.sel = row;
		o->dbg.programsFocus = 0;
		return 1;
	}
	if(DbgListingPane_Click(o, &o->dbg.programListing, x, y, button))
	{
		o->dbg.programsFocus = 1;
		return 1;
	}
	return 0;
}

// the wheel scrolls the pane under the mouse (three rows at a time)
static int ProgramsWheel(OpenBGI_t* o, int x, int y, int delta)
{
	if(DbgPane_Contains(x, y, o->dbg.moduleList.x, o->dbg.moduleList.y, o->dbg.moduleList.w, o->dbg.moduleList.h))
	{
		DbgList_Scroll(&o->dbg.moduleList, delta > 0 ? -3 : 3);
		return 1;
	}
	return DbgListingPane_Wheel(&o->dbg.programListing, x, y, delta);
}

const DbgViewOps_t kDbgViewPrograms = {"Programs", ProgramsDraw, ProgramsKey, ProgramsClick, ProgramsWheel};
