/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * threads.c - the Threads view of the debugger (dbg_internal.h)
 *
 * The script threads of the scheduler as a list on the left: id, module
 * and offset of the instruction pointer, frame pointer, stack pointer and
 * state ("runs", or the wait class the thread blocks on).  The selected
 * thread - which becomes o->dbg.dbgThreadId, the thread the Programs and
 * Memory views look at - has, on the right, its listing following the
 * instruction pointer, its evaluation stack top first with each value
 * interpreted as a tagged pointer, and the dwords below its frame pointer
 * (the locals, as push_local_addr addresses them).
 */
#include "../dbg_internal.h"
#include "bgi/vm.h"
#include "bgi/wait.h"
#include "bgi/sys.h"
#include "bgi/version.h"
#include "bgi/core.h"

static const char* WaitName(const Wait_t* w)
{
	return Wait_ClassName(w);
}

// the list, then for the selected thread the listing (two thirds of the height), the stack beside it and the frame below
static void ThreadsDraw(OpenBGI_t* o, int x, int y, int w, int h)
{
	Thread_t* t;
	int i, n = 0, ly, rowsH;
	int rx = x + DBG_LEFT_W + DBG_PAD, rw = w - DBG_LEFT_W - 2 * DBG_PAD;
	Thread_t* sel;

	// the list
	for(t = o->vm.rootThread ? Thread_Next(o->vm.rootThread) : NULL; t && n < 256; t = Thread_Next(t))
		o->dbg.threadRows[n++] = t;
	o->dbg.threadList.count = n;
	DbgPane_Frame(o, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD, " id module+ip           fp  sp state");
	if(o->dbg.threadsFocus == 0)
		DbgPane_Focus(o, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD);
	DbgPane_ListArea(&o->dbg.threadList, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD);
	if(o->dbg.threadList.sel < 0 && n)
		o->dbg.threadList.sel = 0;
	for(i = 0; i < n; i++)
		if(o->dbg.threadRows[i]->id == o->dbg.dbgThreadId)
			o->dbg.threadList.sel = i;
	for(i = o->dbg.threadList.top; i < o->dbg.threadList.top + o->dbg.threadList.rows && i < n; i++)
	{
		const char* module = "?";
		uint32_t base = 0, size = 0;
		t = o->dbg.threadRows[i];
		Dbg_ModuleAt(t, t->opIP, &module, &base, &size);
		if(i == o->dbg.threadList.sel)
			DbgPane_RowBg(o, &o->dbg.threadList, i, DBG_SELECT);
		DbgDraw_TextF(o, o->dbg.threadList.x + 4, DbgPane_RowY(&o->dbg.threadList, i), DBG_TEXT, "%3u %-12.12s+%04x %04x %3u %s", (unsigned)t->id,
			module, (unsigned)(t->opIP - base), (unsigned)t->fp, (unsigned)t->sp,
			(t->flags & THR_FLAG_WAITING) ? WaitName(t->wait) : "runs");
	}
	sel = o->dbg.threadList.sel >= 0 && o->dbg.threadList.sel < n ? o->dbg.threadRows[o->dbg.threadList.sel] : NULL;
	if(sel)
		o->dbg.dbgThreadId = sel->id;

	// the right side: the listing around the IP, the stack and the frame
	rowsH = h - 2 * DBG_PAD;
	if(sel)
	{
		const char* module;
		uint32_t base, size;
		int stackW = 30 * DBG_CELL_W(o), lh = rowsH * 2 / 3;
		if(Dbg_ModuleAt(sel, sel->opIP, &module, &base, &size))
		{
			DbgListingPane_Show(o, &o->dbg.threadListing, sel, module, base, size);
			o->dbg.threadListing.follow = 1;
		}
		DbgListingPane_Draw(o, &o->dbg.threadListing, sel, rx, y + DBG_PAD, rw - stackW - DBG_PAD, lh, "listing (at the instruction pointer)");
		if(o->dbg.threadsFocus == 1)
			DbgPane_Focus(o, rx, y + DBG_PAD, rw - stackW - DBG_PAD, lh);
		// the evaluation stack
		DbgPane_Frame(o, rx + rw - stackW, y + DBG_PAD, stackW, lh, "evaluation stack (top first)");
		ly = y + DBG_PAD + DBG_CELL_H + 4;
		for(i = 0; i < 24 && (uint32_t)i < sel->sp; i++)
		{
			char d[48];
			Dbg_DescribeValue(o, sel->stack[sel->sp - 1 - i], d, sizeof d);
			DbgDraw_TextF(o, rx + rw - stackW + DBG_PAD, ly + i * DBG_CELL_H, i == 0 ? DBG_ACCENT : DBG_TEXT, "%2d  %s", i, d);
		}
		// the frame: dwords below the frame pointer, 32 bytes per row, the row's label the offset of its first dword
		DbgPane_Frame(o, rx, y + DBG_PAD + lh + DBG_PAD, rw, rowsH - lh - DBG_PAD, "frame (the locals below fp, as dwords; fp-0x.. of push_local_addr)");
		ly = y + DBG_PAD + lh + DBG_PAD + DBG_CELL_H + 4;
		{
			int rows = (rowsH - lh - DBG_PAD - DBG_CELL_H - 8) / DBG_CELL_H, r, c;
			uint32_t fp = sel->fp;
			for(r = 0; r < rows; r++)
			{
				char line[0x200];
				size_t ofs = 0;
				ofs += (size_t)snprintf(line + ofs, sizeof line - ofs, "fp-%04x:", (unsigned)(r * 32 + 32));
				for(c = 0; c < 8; c++)
				{
					uint32_t off = (uint32_t)(r * 32 + 32 - c * 4);
					uint32_t v;
					if(off > fp || fp - off + 4 > sel->dataSize) // outside the data area: dashes
					{
						ofs += (size_t)snprintf(line + ofs, sizeof line - ofs, "  --------");
						continue;
					}
					memcpy(&v, sel->data + fp - off, 4);
					ofs += (size_t)snprintf(line + ofs, sizeof line - ofs, "  %08x", (unsigned)v);
				}
				DbgDraw_Text(o, rx + DBG_PAD, ly + r * DBG_CELL_H, DBG_TEXT, line);
			}
		}
	}
	else
		DbgPane_Frame(o, rx, y + DBG_PAD, rw, rowsH, "no thread");
}

// Left / Right choose the pane; F follows the instruction pointer again; the navigation keys move in the focused pane
static int ThreadsKey(OpenBGI_t* o, int vk, int ch)
{
	if(DbgPane_IsFocusKey(vk, &o->dbg.threadsFocus))
		return 1;
	if(ch == 'f' || ch == 'F')
	{
		o->dbg.threadListing.follow = 1;
		return 1;
	}
	if(o->dbg.threadsFocus == 0 && DbgPane_IsNavKey(vk))
	{
		DbgList_Key(&o->dbg.threadList, vk);
		if(o->dbg.threadList.sel >= 0 && o->dbg.threadList.sel < o->dbg.threadList.count)
			o->dbg.dbgThreadId = o->dbg.threadRows[o->dbg.threadList.sel]->id;
		o->dbg.threadListing.follow = 1;
		return 1;
	}
	return DbgListingPane_Key(o, &o->dbg.threadListing, vk);
}

// a click selects a thread (and focuses the list) or goes to the listing pane (and focuses it)
static int ThreadsClick(OpenBGI_t* o, int x, int y, int button)
{
	int row = DbgList_Hit(&o->dbg.threadList, x, y);
	if(row >= 0)
	{
		o->dbg.threadList.sel = row;
		o->dbg.dbgThreadId = o->dbg.threadRows[row]->id;
		o->dbg.threadListing.follow = 1;
		o->dbg.threadsFocus = 0;
		return 1;
	}
	if(DbgListingPane_Click(o, &o->dbg.threadListing, x, y, button))
	{
		o->dbg.threadsFocus = 1;
		return 1;
	}
	return 0;
}

// the wheel scrolls the pane under the mouse (three rows at a time)
static int ThreadsWheel(OpenBGI_t* o, int x, int y, int delta)
{
	if(DbgPane_Contains(x, y, o->dbg.threadList.x, o->dbg.threadList.y, o->dbg.threadList.w, o->dbg.threadList.h))
	{
		DbgList_Scroll(&o->dbg.threadList, delta > 0 ? -3 : 3);
		return 1;
	}
	return DbgListingPane_Wheel(&o->dbg.threadListing, x, y, delta);
}

const DbgViewOps_t kDbgViewThreads = {"Threads", ThreadsDraw, ThreadsKey, ThreadsClick, ThreadsWheel};
