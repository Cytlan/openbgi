/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * memory.c - the Memory view of the debugger (dbg_internal.h)
 *
 * The memory areas as a list on the left - the global memory, the system
 * area and the selected thread's (o->dbg.dbgThreadId) code, data and
 * local-heap areas - and the selected area as a hex dump with an ASCII
 * column on the right, 16 bytes per row; PgUp / PgDn move by 0x100
 * bytes, the wheel by 48, Home / End to the ends.  Choosing another area
 * returns to its start.
 */
#include "../dbg_internal.h"
#include "bgi/vm.h"
#include "bgi/sys.h"
#include "bgi/core.h"

typedef struct MemArea // one dumpable area
{
	const char* name;
	const uint8_t* base; // NULL when the area does not exist yet (dimmed, not dumpable)
	uint32_t size;       // bytes
} MemArea_t;

// the areas into out (room for 8): the two global ones always, the thread's three when there is a thread; their count
static int MemAreas(OpenBGI_t* o, MemArea_t* out)
{
	Thread_t* t = Dbg_SelectedThread(o);
	int n = 0;
	out[n].name = "global memory";
	out[n].base = o->mem.globalMem;
	out[n].size = o->mem.globalMem ? o->mem.globalMemSize : 0;
	n++;
	out[n].name = "system area";
	out[n].base = o->mem.sysArea;
	out[n].size = o->mem.sysArea ? o->mem.sysAreaSize : 0;
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

// the list, then the dump of the selected area from o->dbg.memTop on
static void MemoryDraw(OpenBGI_t* o, int x, int y, int w, int h)
{
	MemArea_t areas[8];
	int n = MemAreas(o, areas), i;
	int rx = x + DBG_LEFT_W + DBG_PAD, rw = w - DBG_LEFT_W - 2 * DBG_PAD;
	o->dbg.areaList.count = n;
	DbgPane_Frame(o, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD, "areas   (PgUp / PgDn and the wheel move the dump)");
	DbgPane_ListArea(&o->dbg.areaList, x + DBG_PAD, y + DBG_PAD, DBG_LEFT_W - DBG_PAD, h - 2 * DBG_PAD);
	if(o->dbg.areaList.sel < 0)
		o->dbg.areaList.sel = 0;
	for(i = 0; i < n; i++)
	{
		if(i == o->dbg.areaList.sel)
			DbgPane_RowBg(o, &o->dbg.areaList, i, DBG_SELECT);
		DbgDraw_TextF(o, o->dbg.areaList.x + 4, DbgPane_RowY(&o->dbg.areaList, i), areas[i].base ? DBG_TEXT : DBG_DIM, "%-20s %8u bytes", areas[i].name,
			(unsigned)areas[i].size);
	}
	if(o->dbg.areaList.sel < n && areas[o->dbg.areaList.sel].base)
	{
		const MemArea_t* a = &areas[o->dbg.areaList.sel];
		int rows = (h - 2 * DBG_PAD - DBG_CELL_H - 8) / DBG_CELL_H, r;
		char title[0x80];
		if(o->dbg.memTop + 16 > a->size)
			o->dbg.memTop = a->size > 16 ? (a->size - 16) & ~15u : 0;
		snprintf(title, sizeof title, "%s  0x%x of 0x%x", a->name, (unsigned)o->dbg.memTop, (unsigned)a->size);
		DbgPane_Frame(o, rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, title);
		for(r = 0; r < rows; r++)
		{
			uint32_t off = o->dbg.memTop + (uint32_t)r * 16;
			char line[0x100], chars[20];
			size_t ofs;
			int c;
			if(off >= a->size)
				break;
			ofs = (size_t)snprintf(line, sizeof line, "%06x ", (unsigned)off);
			for(c = 0; c < 16; c++)
			{
				if(off + c < a->size)
				{
					uint8_t b = a->base[off + c];
					ofs += (size_t)snprintf(line + ofs, sizeof line - ofs, "%s%02x", c == 8 ? "  " : " ", b);
					chars[c] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
				}
				else
				{
					ofs += (size_t)snprintf(line + ofs, sizeof line - ofs, "%s  ", c == 8 ? "  " : " ");
					chars[c] = ' ';
				}
			}
			chars[16] = 0;
			snprintf(line + ofs, sizeof line - ofs, "  %s", chars);
			DbgDraw_Text(o, rx + DBG_PAD, y + DBG_PAD + DBG_CELL_H + 6 + r * DBG_CELL_H, DBG_TEXT, line);
		}
	}
	else
		DbgPane_Frame(o, rx, y + DBG_PAD, rw, h - 2 * DBG_PAD, "nothing here");
}

// Up / Down choose the area (and rewind the dump); PgUp / PgDn, Home, End move the dump (End: the last row, once clamped)
static int MemoryKey(OpenBGI_t* o, int vk, int ch)
{
	switch(vk)
	{
		case 0x26:
			o->dbg.areaList.sel--;
			o->dbg.memTop = 0;
			break;
		case 0x28:
			o->dbg.areaList.sel++;
			o->dbg.memTop = 0;
			break;
		case 0x21: o->dbg.memTop = o->dbg.memTop > 0x100 ? o->dbg.memTop - 0x100 : 0; break;
		case 0x22: o->dbg.memTop += 0x100; break;
		case 0x24: o->dbg.memTop = 0; break;
		case 0x23: o->dbg.memTop = 0xffffffffu; break;
		default: return 0;
	}
	DbgList_Clamp(&o->dbg.areaList);
	return 1;
}

// a click on a row chooses the area and rewinds the dump
static int MemoryClick(OpenBGI_t* o, int x, int y, int button)
{
	int row = DbgList_Hit(&o->dbg.areaList, x, y);
	if(row >= 0)
	{
		o->dbg.areaList.sel = row;
		o->dbg.memTop = 0;
	}
	return row >= 0;
}

// the wheel moves the dump by three rows, wherever the mouse is
static int MemoryWheel(OpenBGI_t* o, int x, int y, int delta)
{
	if(delta > 0)
		o->dbg.memTop = o->dbg.memTop > 48 ? o->dbg.memTop - 48 : 0;
	else
		o->dbg.memTop += 48;
	return 1;
}

const DbgViewOps_t kDbgViewMemory = {"Memory", MemoryDraw, MemoryKey, MemoryClick, MemoryWheel};
