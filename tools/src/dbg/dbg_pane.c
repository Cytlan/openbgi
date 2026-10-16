/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dbg_pane.c - what the debugger's views share: the framed panes, the
 *              list areas and the keyboard focus, the listing pane that
 *              shows a module's disassembly (Threads and Programs), and
 *              the description of a tagged script value (dbg_internal.h)
 */
#include "dbg_internal.h"
#include "bgi/vm.h"
#include "bgi/sys.h"

// a filled, outlined pane; with a title, a darker bar of one text row at the top
void DbgPane_Frame(int x, int y, int w, int h, const char* title)
{
	DbgDraw_Fill(x, y, w, h, DBG_PANEL);
	DbgDraw_Frame(x, y, w, h, DBG_LINE);
	if(title)
	{
		DbgDraw_Fill(x + 1, y + 1, w - 2, DBG_CELL_H + 2, DBG_LINE);
		DbgDraw_Text(x + DBG_PAD, y + 2, DBG_ACCENT, title);
	}
}

// the pane the arrow keys act on, outlined
void DbgPane_Focus(int x, int y, int w, int h)
{
	DbgDraw_Frame(x, y, w, h, DBG_SELECT);
}

/* the views with two panes give the keyboard to one of them: Left / Right
 * (and a click) choose; 0 is the left list, 1 the listing */
int DbgPane_IsFocusKey(int vk, int* focus)
{
	if(vk == 0x25)
		*focus = 0;
	else if(vk == 0x27)
		*focus = 1;
	else
		return 0;
	return 1;
}

// Up, Down, PgUp, PgDn, Home, End (virtual key codes)
int DbgPane_IsNavKey(int vk)
{
	return vk == 0x26 || vk == 0x28 || vk == 0x21 || vk == 0x22 || vk == 0x24 || vk == 0x23;
}

// the list rows area inside a pane with a title: below the title bar, inside the frame; sets how many rows fit
void DbgPane_ListArea(DbgList_t* l, int x, int y, int w, int h)
{
	l->x = x + 2;
	l->y = y + DBG_CELL_H + 4;
	l->w = w - 4;
	l->h = h - DBG_CELL_H - 6;
	l->rows = l->h / DBG_CELL_H;
	DbgList_Clamp(l);
}

// fill the background of a (visible) row
void DbgPane_RowBg(const DbgList_t* l, int row, uint32_t rgb)
{
	DbgDraw_Fill(l->x, l->y + (row - l->top) * DBG_CELL_H, l->w, DBG_CELL_H, rgb);
}

// the y of a row (meaningful for the visible ones)
int DbgPane_RowY(const DbgList_t* l, int row)
{
	return l->y + (row - l->top) * DBG_CELL_H;
}

// is (x, y) inside the rectangle (px, py, pw, ph)?
int DbgPane_Contains(int x, int y, int px, int py, int pw, int ph)
{
	return x >= px && x < px + pw && y >= py && y < py + ph;
}

// The name of a bitmap mode (RGB16, RGB32, ARGB32, GRAY8, VECTOR, HEIGHT, VECDIST).
const char* Dbg_BmpModeName(int mode)
{
	static const char* const names[] = {"RGB16", "RGB32", "ARGB32", "GRAY8", "VECTOR", "HEIGHT", "VECDIST"};
	return mode >= 0 && mode < 7 ? names[mode] : "?";
}

/* a script value described as "<hex> <meaning>": the tag in its top bits
 * (gEngine->tagShift) names the area - "code+off", "data+off", "heap+off",
 * "dynN+off" for the N-th dynamic area, "gmem+off" for a tagless offset
 * into global memory - else the value as a signed number */
void Dbg_DescribeValue(uint32_t v, char* out, size_t n)
{
	uint32_t tag = v >> gEngine->tagShift, off = v & ((1u << gEngine->tagShift) - 1u);
	if(tag == 0 && off && gGlobalMem && off < gGlobalMemSize)
		snprintf(out, n, "%08x gmem+%x", (unsigned)v, (unsigned)off);
	else if(tag == gEngine->tagCode)
		snprintf(out, n, "%08x code+%x", (unsigned)v, (unsigned)off);
	else if(tag == gEngine->tagData)
		snprintf(out, n, "%08x data+%x", (unsigned)v, (unsigned)off);
	else if(tag == gEngine->tagHeap)
		snprintf(out, n, "%08x heap+%x", (unsigned)v, (unsigned)off);
	else if(tag >= gEngine->dynFirst && tag != 0)
		snprintf(out, n, "%08x dyn%u+%x", (unsigned)v, (unsigned)(tag - gEngine->dynFirst), (unsigned)off);
	else
		snprintf(out, n, "%08x %d", (unsigned)v, (int)v);
}

/* Draw the pane: the title (with a note when the listing has flow
 * problems or is missing), then the visible lines - offset in the margin,
 * a red square for a breakpoint, labels accented, comments dimmed; the
 * line at t's instruction pointer gets the DBG_CURRENT background and,
 * while following, becomes the selection and is kept in the middle
 * third of the pane. */
void DbgListingPane_Draw(DbgListingPane_t* p, Thread_t* t, int x, int y, int w, int h, const char* title)
{
	int i, cur = -1;
	char head[0x100];
	snprintf(head, sizeof head, "%s%s%s", title, p->listing && p->listing->problems ? "  (flow problems noted)" : "",
		p->listing ? "" : "  (no listing)");
	DbgPane_Frame(x, y, w, h, head);
	DbgPane_ListArea(&p->list, x, y, w, h);
	if(!p->listing)
		return;
	p->list.count = p->listing->count;
	DbgList_Clamp(&p->list);
	if(t && t->opIP >= p->base && t->opIP < p->base + p->size)
		cur = DbgListing_LineOf(p->listing, t->opIP - p->base);
	if(p->follow && cur >= 0)
	{ // the current instruction is the selected line (F9 acts on it) and stays in view
		p->list.sel = cur;
		if(cur < p->list.top + 2 || cur >= p->list.top + p->list.rows - 2)
		{
			p->list.top = cur - p->list.rows / 3;
			DbgList_Clamp(&p->list);
		}
	}
	for(i = p->list.top; i < p->list.top + p->list.rows && i < p->listing->count; i++)
	{
		const char* text = p->listing->lines[i];
		uint32_t off = p->listing->offs[i];
		int ry = DbgPane_RowY(&p->list, i);
		int label = text[0] && text[strlen(text) - 1] == ':';
		int isBreak = off != ~0u && !label && text[0] != ';' && Dbg_HasBreak(p->module, off);
		if(i == cur)
			DbgPane_RowBg(&p->list, i, DBG_CURRENT);
		else if(i == p->list.sel)
			DbgPane_RowBg(&p->list, i, DBG_HILITE);
		if(isBreak)
			DbgDraw_Fill(p->list.x + 2, ry + 4, 8, 8, DBG_BREAK);
		if(off != ~0u && !label && text[0] != ';')
			DbgDraw_TextF(p->list.x + 14, ry, DBG_DIM, "%04x", (unsigned)off);
		DbgDraw_Text(p->list.x + 14 + 5 * DBG_CELL_W, ry, label ? DBG_ACCENT : text[0] == ';' ? DBG_DIM
																							  : DBG_TEXT,
			text);
	}
}

// Point the pane at a module (the name is compared by address: it is the thread's own string); following resumes.
void DbgListingPane_Show(DbgListingPane_t* p, Thread_t* t, const char* module, uint32_t base, uint32_t size)
{
	if(p->module != module || p->base != base || p->size != size || !p->listing)
	{
		p->module = module;
		p->base = base;
		p->size = size;
		p->listing = DbgListing_Get(t, module, base, size);
		p->list.count = p->listing ? p->listing->count : 0;
		p->list.sel = -1;
		p->follow = 1;
	}
}

// F9 toggles a breakpoint on the selected line; the navigation keys move the selection and stop following.
int DbgListingPane_Key(DbgListingPane_t* p, int vk)
{
	if(vk == 0x78)
	{ // F9: a breakpoint on the selected line
		if(p->listing && p->list.sel >= 0 && p->list.sel < p->listing->count && p->listing->offs[p->list.sel] != ~0u)
			Dbg_ToggleBreak(p->module, p->listing->offs[p->list.sel]);
		return 1;
	}
	if(vk == 0x26 || vk == 0x28 || vk == 0x21 || vk == 0x22 || vk == 0x24 || vk == 0x23)
	{
		if(p->list.sel < 0)
			p->list.sel = p->list.top;
		DbgList_Key(&p->list, vk);
		p->follow = 0;
		return 1;
	}
	return 0;
}

// A left click in the 14-pixel margin toggles a breakpoint on that line; any other click selects it.
int DbgListingPane_Click(DbgListingPane_t* p, int x, int y, int button)
{
	int row = DbgList_Hit(&p->list, x, y);
	if(row < 0 || !p->listing)
		return 0;
	if(button == 0 && x < p->list.x + 14 && p->listing->offs[row] != ~0u)
		Dbg_ToggleBreak(p->module, p->listing->offs[row]);
	else
		p->list.sel = row;
	p->follow = 0;
	return 1;
}

// The wheel over the pane scrolls three lines and stops following.
int DbgListingPane_Wheel(DbgListingPane_t* p, int x, int y, int delta)
{
	if(!DbgPane_Contains(x, y, p->list.x, p->list.y, p->list.w, p->list.h))
		return 0;
	DbgList_Scroll(&p->list, delta > 0 ? -3 : 3);
	p->follow = 0;
	return 1;
}
