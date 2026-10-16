/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * threads.c - the Threads view of the debugger (dbg_internal.h)
 *
 * The script threads of the scheduler as a list on the left: id, module
 * and offset of the instruction pointer, frame pointer, stack pointer and
 * state ("runs", or the wait class the thread blocks on).  The selected
 * thread - which becomes gDbgThreadId, the thread the Programs and Memory
 * views look at - has, on the right, its listing following the
 * instruction pointer, its evaluation stack top first with each value
 * interpreted as a tagged pointer, and the dwords below its frame pointer
 * (the locals, as push_local_addr addresses them).
 */
#include "../dbg_internal.h"
#include "bgi/vm.h"
#include "bgi/wait.h"
#include "bgi/sys.h"
#include "bgi/version.h"

static DbgList_t gThreadList;           // the thread list
static DbgListingPane_t gThreadListing; // the selected thread's listing
static Thread_t* gThreadRows[256];      // the threads behind the rows, rebuilt every draw
static int gThreadsFocus;               // 0 the thread list, 1 the listing

static const char* WaitName(const Wait_t* w)
{
	return Wait_ClassName(w);
}

// the list, then for the selected thread the listing (two thirds of the height), the stack beside it and the frame below
static void ThreadsDraw(int x, int y, int w, int h)
{
	Thread_t* t;
	int i, n = 0, ly, rowsH;
	int rx = x + DBG_LEFT_W + DBG_PAD, rw = w - DBG_LEFT_W - 2 * DBG_PAD;
	Thread_t* sel;

	// the list
	for(t = gRootThread ? Thread_Next(gRootThread) : NULL; t && n < 256; t = Thread_Next(t))
		gThreadRows[n++] = t;
	gThreadList.count = n;
	DbgPane_Frame(x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD, " id module+ip           fp  sp state");
	if(gThreadsFocus == 0)
		DbgPane_Focus(x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD);
	DbgPane_ListArea(&gThreadList, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD);
	if(gThreadList.sel < 0 && n)
		gThreadList.sel = 0;
	for(i = 0; i < n; i++)
		if(gThreadRows[i]->id == gDbgThreadId)
			gThreadList.sel = i;
	for(i = gThreadList.top; i < gThreadList.top + gThreadList.rows && i < n; i++)
	{
		const char* module = "?";
		uint32_t base = 0, size = 0;
		t = gThreadRows[i];
		Dbg_ModuleAt(t, t->opIP, &module, &base, &size);
		if(i == gThreadList.sel)
			DbgPane_RowBg(&gThreadList, i, DBG_SELECT);
		DbgDraw_TextF(gThreadList.x + 4, DbgPane_RowY(&gThreadList, i), DBG_TEXT, "%3u %-12.12s+%04x %04x %3u %s", (unsigned)t->id,
			module, (unsigned)(t->opIP - base), (unsigned)t->fp, (unsigned)t->sp,
			(t->flags & THR_FLAG_WAITING) ? WaitName(t->wait) : "runs");
	}
	sel = gThreadList.sel >= 0 && gThreadList.sel < n ? gThreadRows[gThreadList.sel] : NULL;
	if(sel)
		gDbgThreadId = sel->id;

	// the right side: the listing around the IP, the stack and the frame
	rowsH = h - 2 * DBG_PAD;
	if(sel)
	{
		const char* module;
		uint32_t base, size;
		int stackW = 30 * DBG_CELL_W, lh = rowsH * 2 / 3;
		if(Dbg_ModuleAt(sel, sel->opIP, &module, &base, &size))
		{
			DbgListingPane_Show(&gThreadListing, sel, module, base, size);
			gThreadListing.follow = 1;
		}
		DbgListingPane_Draw(&gThreadListing, sel, rx, y + DBG_PAD, rw - stackW - DBG_PAD, lh, "listing (at the instruction pointer)");
		if(gThreadsFocus == 1)
			DbgPane_Focus(rx, y + DBG_PAD, rw - stackW - DBG_PAD, lh);
		// the evaluation stack
		DbgPane_Frame(rx + rw - stackW, y + DBG_PAD, stackW, lh, "evaluation stack (top first)");
		ly = y + DBG_PAD + DBG_CELL_H + 4;
		for(i = 0; i < 24 && (uint32_t)i < sel->sp; i++)
		{
			char d[48];
			Dbg_DescribeValue(sel->stack[sel->sp - 1 - i], d, sizeof d);
			DbgDraw_TextF(rx + rw - stackW + DBG_PAD, ly + i * DBG_CELL_H, i == 0 ? DBG_ACCENT : DBG_TEXT, "%2d  %s", i, d);
		}
		// the frame: dwords below the frame pointer, 32 bytes per row, the row's label the offset of its first dword
		DbgPane_Frame(rx, y + DBG_PAD + lh + DBG_PAD, rw, rowsH - lh - DBG_PAD, "frame (the locals below fp, as dwords; fp-0x.. of push_local_addr)");
		ly = y + DBG_PAD + lh + DBG_PAD + DBG_CELL_H + 4;
		{
			int rows = (rowsH - lh - DBG_PAD - DBG_CELL_H - 8) / DBG_CELL_H, r, c;
			uint32_t fp = sel->fp;
			for(r = 0; r < rows; r++)
			{
				char line[0x200];
				size_t o = 0;
				o += (size_t)snprintf(line + o, sizeof line - o, "fp-%04x:", (unsigned)(r * 32 + 32));
				for(c = 0; c < 8; c++)
				{
					uint32_t off = (uint32_t)(r * 32 + 32 - c * 4);
					uint32_t v;
					if(off > fp || fp - off + 4 > sel->dataSize) // outside the data area: dashes
					{
						o += (size_t)snprintf(line + o, sizeof line - o, "  --------");
						continue;
					}
					memcpy(&v, sel->data + fp - off, 4);
					o += (size_t)snprintf(line + o, sizeof line - o, "  %08x", (unsigned)v);
				}
				DbgDraw_Text(rx + DBG_PAD, ly + r * DBG_CELL_H, DBG_TEXT, line);
			}
		}
	}
	else
		DbgPane_Frame(rx, y + DBG_PAD, rw, rowsH, "no thread");
}

// Left / Right choose the pane; F follows the instruction pointer again; the navigation keys move in the focused pane
static int ThreadsKey(int vk, int ch)
{
	if(DbgPane_IsFocusKey(vk, &gThreadsFocus))
		return 1;
	if(ch == 'f' || ch == 'F')
	{
		gThreadListing.follow = 1;
		return 1;
	}
	if(gThreadsFocus == 0 && DbgPane_IsNavKey(vk))
	{
		DbgList_Key(&gThreadList, vk);
		if(gThreadList.sel >= 0 && gThreadList.sel < gThreadList.count)
			gDbgThreadId = gThreadRows[gThreadList.sel]->id;
		gThreadListing.follow = 1;
		return 1;
	}
	return DbgListingPane_Key(&gThreadListing, vk);
}

// a click selects a thread (and focuses the list) or goes to the listing pane (and focuses it)
static int ThreadsClick(int x, int y, int button)
{
	int row = DbgList_Hit(&gThreadList, x, y);
	if(row >= 0)
	{
		gThreadList.sel = row;
		gDbgThreadId = gThreadRows[row]->id;
		gThreadListing.follow = 1;
		gThreadsFocus = 0;
		return 1;
	}
	if(DbgListingPane_Click(&gThreadListing, x, y, button))
	{
		gThreadsFocus = 1;
		return 1;
	}
	return 0;
}

// the wheel scrolls the pane under the mouse (three rows at a time)
static int ThreadsWheel(int x, int y, int delta)
{
	if(DbgPane_Contains(x, y, gThreadList.x, gThreadList.y, gThreadList.w, gThreadList.h))
	{
		DbgList_Scroll(&gThreadList, delta > 0 ? -3 : 3);
		return 1;
	}
	return DbgListingPane_Wheel(&gThreadListing, x, y, delta);
}

const DbgViewOps_t kDbgViewThreads = {"Threads", ThreadsDraw, ThreadsKey, ThreadsClick, ThreadsWheel};
