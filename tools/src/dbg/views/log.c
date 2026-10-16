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

static DbgList_t gLogList; // the lines
static int gLogFollow = 1; // keep the newest line in view

// the visible lines, mapped from the ring (gDbgLogNext is the slot after the newest)
static void LogDraw(int x, int y, int w, int h)
{
	int i;
	DbgPane_Frame(x + DBG_PAD, y + DBG_PAD, w - 2 * DBG_PAD, h - 2 * DBG_PAD, "log   (newest last; the wheel scrolls, End follows)");
	DbgPane_ListArea(&gLogList, x + DBG_PAD, y + DBG_PAD, w - 2 * DBG_PAD, h - 2 * DBG_PAD);
	gLogList.count = gDbgLogCount;
	if(gLogFollow)
		gLogList.top = gDbgLogCount - gLogList.rows;
	DbgList_Clamp(&gLogList);
	for(i = gLogList.top; i < gLogList.top + gLogList.rows && i < gDbgLogCount; i++)
	{
		int idx = (gDbgLogNext - gDbgLogCount + i + DBG_LOG_LINES) % DBG_LOG_LINES;
		DbgDraw_Text(gLogList.x + 4, DbgPane_RowY(&gLogList, i), DBG_TEXT, gDbgLog[idx].text);
	}
}

// End follows again; Up / Down / PgUp / PgDn / Home scroll and stop following
static int LogKey(int vk, int ch)
{
	if(vk == 0x23)
	{
		gLogFollow = 1;
		return 1;
	}
	if(vk == 0x21 || vk == 0x22 || vk == 0x26 || vk == 0x28 || vk == 0x24)
	{
		gLogFollow = 0;
		DbgList_Scroll(&gLogList, vk == 0x21 ? -gLogList.rows : vk == 0x22 ? gLogList.rows
				: vk == 0x26                                               ? -1
				: vk == 0x28                                               ? 1
																		   : -gDbgLogCount);
		return 1;
	}
	return 0;
}

static int LogClick(int x, int y, int button)
{
	return 0;
}

// the wheel scrolls three lines; reaching the end follows again
static int LogWheel(int x, int y, int delta)
{
	gLogFollow = 0;
	DbgList_Scroll(&gLogList, delta > 0 ? -3 : 3);
	if(gLogList.top >= gDbgLogCount - gLogList.rows)
		gLogFollow = 1;
	return 1;
}

const DbgViewOps_t kDbgViewLog = {"Log", LogDraw, LogKey, LogClick, LogWheel};
