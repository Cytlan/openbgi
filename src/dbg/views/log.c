/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * log.c - the Log view of the debugger (dbg_internal.h)
 *
 * The log ring (Dbg_Log) as one pane, oldest line first: module loads,
 * message boxes, breakpoints set and hit, pauses, the instruction trace
 * while it is on.  The view follows the newest line until the user
 * scrolls up; End (or scrolling back down to the end) follows again.
 */
#include "../dbg_internal.h"
#include "bgi/core.h"

// the visible lines, mapped from the ring (o->dbg.dbgLogNext is the slot after the newest)
static void LogDraw(OpenBGI_t* o, int x, int y, int w, int h)
{
	int i;
	DbgPane_Frame(o, x + DBG_PAD, y + DBG_PAD, w - 2 * DBG_PAD, h - 2 * DBG_PAD, "log   (newest last; the wheel scrolls, End follows)");
	DbgPane_ListArea(&o->dbg.logList, x + DBG_PAD, y + DBG_PAD, w - 2 * DBG_PAD, h - 2 * DBG_PAD);
	o->dbg.logList.count = o->dbg.dbgLogCount;
	if(o->dbg.logFollow)
		o->dbg.logList.top = o->dbg.dbgLogCount - o->dbg.logList.rows;
	DbgList_Clamp(&o->dbg.logList);
	for(i = o->dbg.logList.top; i < o->dbg.logList.top + o->dbg.logList.rows && i < o->dbg.dbgLogCount; i++)
	{
		int idx = (o->dbg.dbgLogNext - o->dbg.dbgLogCount + i + DBG_LOG_LINES) % DBG_LOG_LINES;
		DbgDraw_Text(o, o->dbg.logList.x + 4, DbgPane_RowY(&o->dbg.logList, i), DBG_TEXT, o->dbg.dbgLog[idx].text);
	}
}

// End follows again; Up / Down / PgUp / PgDn / Home scroll and stop following
static int LogKey(OpenBGI_t* o, int vk, int ch)
{
	if(vk == 0x23)
	{
		o->dbg.logFollow = 1;
		return 1;
	}
	if(vk == 0x21 || vk == 0x22 || vk == 0x26 || vk == 0x28 || vk == 0x24)
	{
		o->dbg.logFollow = 0;
		DbgList_Scroll(&o->dbg.logList, vk == 0x21 ? -o->dbg.logList.rows : vk == 0x22 ? o->dbg.logList.rows
				: vk == 0x26                                                           ? -1
				: vk == 0x28                                                           ? 1
																					   : -o->dbg.dbgLogCount);
		return 1;
	}
	return 0;
}

static int LogClick(OpenBGI_t* o, int x, int y, int button)
{
	return 0;
}

// the wheel scrolls three lines; reaching the end follows again
static int LogWheel(OpenBGI_t* o, int x, int y, int delta)
{
	o->dbg.logFollow = 0;
	DbgList_Scroll(&o->dbg.logList, delta > 0 ? -3 : 3);
	if(o->dbg.logList.top >= o->dbg.dbgLogCount - o->dbg.logList.rows)
		o->dbg.logFollow = 1;
	return 1;
}

const DbgViewOps_t kDbgViewLog = {"Log", LogDraw, LogKey, LogClick, LogWheel};
