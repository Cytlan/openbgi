/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * memory.c - the Memory view of the debugger (dbg_internal.h)
 *
 * The memory areas as a list on the left - the global memory, the system
 * area and the selected thread's (gDbgThreadId) code, data and local-heap
 * areas - and the selected area as a hex dump with an ASCII column on
 * the right, 16 bytes per row; PgUp / PgDn move by 0x100 bytes, the
 * wheel by 48, Home / End to the ends.  Choosing another area returns to
 * its start.
 */
#include "../dbg_internal.h"
#include "bgi/vm.h"
#include "bgi/sys.h"

static DbgList_t gAreaList; // the area list
static uint32_t gMemTop;    // the first byte shown (clamped to the area when drawing)

typedef struct MemArea // one dumpable area
{
	const char* name;
	const uint8_t* base; // NULL when the area does not exist yet (dimmed, not dumpable)
	uint32_t size;       // bytes
} MemArea_t;

// the areas into out (room for 8): the two global ones always, the thread's three when there is a thread; their count
static int MemAreas(MemArea_t* out)
{
	Thread_t* t = Dbg_SelectedThread();
	int n = 0;
	out[n].name = "global memory";
	out[n].base = gGlobalMem;
	out[n].size = gGlobalMem ? gGlobalMemSize : 0;
	n++;
	out[n].name = "system area";
	out[n].base = gSysArea;
	out[n].size = gSysArea ? gSysAreaSize : 0;
	n++;
	if(t)
	{
		out[n].name = "thread code area";
		out[n].base = t->code;
		out[n].size = t->codeUsed;
		n++;
		out[n].name = "thread data area";
		out[n].base = t->data;
		out[n].size = t->dataSize;
		n++;
		out[n].name = "thread local heap";
		out[n].base = t->heap ? Heap_Ptr(t->heap, 0) : NULL;
		out[n].size = t->heap ? 0x8000 : 0; // the arena's initial size; a grown arena shows its first 0x8000 bytes
		n++;
	}
	return n;
}

// the list, then the dump of the selected area from gMemTop on
static void MemoryDraw(int x, int y, int w, int h)
{
	MemArea_t areas[8];
	int n = MemAreas(areas), i;
	int rx = x + DBG_LEFT_W + DBG_PAD, rw = w - DBG_LEFT_W - 2 * DBG_PAD;
	gAreaList.count = n;
	DbgPane_Frame(x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD, "areas   (PgUp / PgDn and the wheel move the dump)");
	DbgPane_ListArea(&gAreaList, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD);
	if(gAreaList.sel < 0)
		gAreaList.sel = 0;
	for(i = 0; i < n; i++)
	{
		if(i == gAreaList.sel)
			DbgPane_RowBg(&gAreaList, i, DBG_SELECT);
		DbgDraw_TextF(gAreaList.x + 4, DbgPane_RowY(&gAreaList, i), areas[i].base ? DBG_TEXT : DBG_DIM, "%-20s %8u bytes", areas[i].name,
			(unsigned)areas[i].size);
	}
	if(gAreaList.sel < n && areas[gAreaList.sel].base)
	{
		const MemArea_t* a = &areas[gAreaList.sel];
		int rows = (h - 2 * DBG_PAD - DBG_CELL_H - 8) / DBG_CELL_H, r;
		char title[0x80];
		if(gMemTop + 16 > a->size)
			gMemTop = a->size > 16 ? (a->size - 16) & ~15u : 0;
		snprintf(title, sizeof title, "%s  0x%x of 0x%x", a->name, (unsigned)gMemTop, (unsigned)a->size);
		DbgPane_Frame(rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, title);
		for(r = 0; r < rows; r++)
		{
			uint32_t off = gMemTop + (uint32_t)r * 16;
			char line[0x100], chars[20];
			size_t o;
			int c;
			if(off >= a->size)
				break;
			o = (size_t)snprintf(line, sizeof line, "%06x ", (unsigned)off);
			for(c = 0; c < 16; c++)
			{
				if(off + c < a->size)
				{
					uint8_t b = a->base[off + c];
					o += (size_t)snprintf(line + o, sizeof line - o, "%s%02x", c == 8 ? "  " : " ", b);
					chars[c] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
				}
				else
				{
					o += (size_t)snprintf(line + o, sizeof line - o, "%s  ", c == 8 ? "  " : " ");
					chars[c] = ' ';
				}
			}
			chars[16] = 0;
			snprintf(line + o, sizeof line - o, "  %s", chars);
			DbgDraw_Text(rx + DBG_PAD, y + DBG_PAD + DBG_CELL_H + 6 + r * DBG_CELL_H, DBG_TEXT, line);
		}
	}
	else
		DbgPane_Frame(rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, "nothing here");
}

// Up / Down choose the area (and rewind the dump); PgUp / PgDn, Home, End move the dump (End: the last row, once clamped)
static int MemoryKey(int vk, int ch)
{
	switch(vk)
	{
		case 0x26:
			gAreaList.sel--;
			gMemTop = 0;
			break;
		case 0x28:
			gAreaList.sel++;
			gMemTop = 0;
			break;
		case 0x21: gMemTop = gMemTop > 0x100 ? gMemTop - 0x100 : 0; break;
		case 0x22: gMemTop += 0x100; break;
		case 0x24: gMemTop = 0; break;
		case 0x23: gMemTop = 0xffffffffu; break;
		default: return 0;
	}
	DbgList_Clamp(&gAreaList);
	return 1;
}

// a click on a row chooses the area and rewinds the dump
static int MemoryClick(int x, int y, int button)
{
	int row = DbgList_Hit(&gAreaList, x, y);
	if(row >= 0)
	{
		gAreaList.sel = row;
		gMemTop = 0;
	}
	return row >= 0;
}

// the wheel moves the dump by three rows, wherever the mouse is
static int MemoryWheel(int x, int y, int delta)
{
	if(delta > 0)
		gMemTop = gMemTop > 48 ? gMemTop - 48 : 0;
	else
		gMemTop += 48;
	return 1;
}

const DbgViewOps_t kDbgViewMemory = {"Memory", MemoryDraw, MemoryKey, MemoryClick, MemoryWheel};
