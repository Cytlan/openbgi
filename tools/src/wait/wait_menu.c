/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wait_menu.c - the text menus of a message window and their starters
 *
 * Interface: waitobj.h (StartMenuSelect, StartMenuSelectBmp, Menu_Draw and
 * the Menu_Set* settings); MenuSelect_SetRequireActive is in gfx.h.
 *
 *   WaitMenuSelect  "90 A0": up to 16 strings laid out in columns over the
 *                   window's text area; a blinking cursor bitmap (window
 *                   item 0) marks the selection
 *   WaitMenuBmp     "90 A2": the same with two decoration bitmaps (items 1
 *                   and 2) beside the selected entry and no keyboard
 *                   navigation
 *   WaitMenuBmp2    "90 A3": no input at all; flashes the initial selection
 *                   twenty times and finishes
 *   Menu_Draw       "90 A1": lay the entries out, no wait
 *
 * The cursor is driven by the mouse (the entry under the pointer becomes the
 * selection), by the arrow keys (wrapping within the grid), by the wheel
 * (which moves the pointer to the entry instead) and by the number keys.
 * Every change of the selection is reported to the thread's message queue as
 * 0x10000001 (selection) / 0x10000002 (entry under the pointer).  The wait
 * ends on a click, the decide key, a number key or - when allowed - a cancel
 * key, and pushes the selected index twice (-1 for cancel; the second copy is
 * -1 when waits are not blocking).  A menu takes input only while its window
 * holds the input focus (InputFocus_IsTop) and, with "90 AF", while the
 * engine window is the active one.
 */
#include <string.h>
#include "bgi/wait.h"
#include "bgi/waitobj.h"
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/sysobj.h"
#include "bgi/sys.h"
#include "bgi/gfx.h"
#include "bgi/gfx/text.h"
#include "bgi/gfx/window.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/bmpmgr.h"

#define MENU_MAX 16 // entries (and columns) at most

typedef struct WaitMenuSelect // 0x194 bytes
{
	Wait_t w;
	Window_t* win;          // the message window the menu lives in
	uint32_t input;         // the (filtered) input bits of the pass
	int msgArrived;         // window message 0x200 (the mouse moved) arrived
	int selected;           // the cursor entry = the result; -1 none
	int count;              // entries
	int hover;              // the entry last reported under the pointer (-1)
	int cols;               // columns
	char* items[MENU_MAX];  // private copies of the strings
	Rect_t rects[MENU_MAX]; // entry rectangles relative to the window
	int centre;             // entries are centred in their column
	uint32_t colour;        // text colour (unless the window has a colour table)
	int allowCancel;        // the cancel keys (bits 1 and 9) end the wait
	int shown;              // the cursor has been drawn since the last change
	int phase;              // blink phase: index into gMenuCursorColour
	int active;             // the window has the input focus
} WaitMenuSelect_t;

typedef struct WaitMenuBmp // 0x1ac bytes
{
	WaitMenuSelect_t m;
	int hasBmp[2];     // the decoration bitmaps are set (items 1 and 2)
	int32_t off[2][2]; // their offsets from the entry's top-left (the
					   // second one is also shifted right by the entry's width)
} WaitMenuBmp_t;

typedef struct WaitMenuBmp2 // 0x1b0 bytes
{
	WaitMenuBmp_t b;
	int flashes; // blink steps done
} WaitMenuBmp2_t;

typedef struct MenuVtbl // the base table plus the menu's own slots
{
	WaitVtbl_t base;
	uint32_t (*filterInput)(WaitMenuSelect_t* w, uint32_t mask); // the input bits the class reacts to
	int (*onMouseMove)(WaitMenuSelect_t* w);                     // 1 when the selection changed
	uint32_t (*blinkInterval)(WaitMenuSelect_t* w);              // ms
	void (*highlight)(WaitMenuSelect_t* w);                      // (re)draw the cursor at the selection
	int (*update)(WaitMenuSelect_t* w);                          // 1 when the wait is over
} MenuVtbl_t;
#define MENU_VT(m) ((const MenuVtbl_t*)(m)->w.vt)

// the window's sort key: the menu's input layer key and the key of its dirty rectangles
#define WIN_KEY(w) ((w)->win->obj.vt->sortKey(&(w)->win->obj))

// ---- the global settings ---------------------------------------------------

static int gMenuRequireActive;           // "90 AF": the menu takes input only while the engine window is active
static uint32_t gMenuBlinkMs = 600;      // "90 A5": the blink interval in ms
static uint32_t gMenuCursorColour[2] = { // "90 A4": the cursor text colour per blink
	0x00ff0000, 0x00ffff00};             //          phase; 0 hides the cursor in that phase
static int gMenuWheelOn;                 // "90 A6": the wheel moves the pointer
static int gMenuWheelCurve;              // to the next entry with this
static int gMenuWheelDuration;           // MouseMove_Start curve, duration
static int gMenuWheelFps;                // and rate

// "90 AF" (through Select_RequireActive)
void MenuSelect_SetRequireActive(int on)
{
	gMenuRequireActive = on;
}

// may a menu react to input right now?
static int MenuAllowed(void)
{
	return gMenuRequireActive ? Window_IsActive() : 1;
}

// "90 A5"
void Menu_SetBlinkInterval(uint32_t ms)
{
	gMenuBlinkMs = ms;
}

// "90 A4": the colours of the two blink phases, 0 hides the cursor in that phase
void Menu_SetCursorColours(uint32_t c0, uint32_t c1)
{
	gMenuCursorColour[0] = c0;
	gMenuCursorColour[1] = c1;
}

// "90 A6": whether the wheel glides the pointer to the next entry, and how (MouseMove_Start)
void Menu_SetWheelMove(int on, int curve, int duration, int fps)
{
	gMenuWheelOn = on;
	gMenuWheelCurve = curve;
	gMenuWheelDuration = duration;
	gMenuWheelFps = fps;
}

// ---- vtables -------------------------------------------------------------------

static void WaitMenuSelect_Destroy(Wait_t* w);
static int WaitMenuSelect_Poll(Wait_t* w);
static uint32_t WaitMenuSelect_FilterInput(WaitMenuSelect_t* w, uint32_t mask);
static int WaitMenuSelect_OnMouseMove(WaitMenuSelect_t* w);
static uint32_t WaitMenuSelect_BlinkInterval(WaitMenuSelect_t* w);
static void WaitMenuSelect_Highlight(WaitMenuSelect_t* w);
static int WaitMenuSelect_Update(WaitMenuSelect_t* w);
static void WaitMenuBmp_Destroy(Wait_t* w);
static uint32_t WaitMenuBmp_FilterInput(WaitMenuSelect_t* w, uint32_t mask);
static int WaitMenuBmp_OnMouseMove(WaitMenuSelect_t* w);
static void WaitMenuBmp_Highlight(WaitMenuSelect_t* w);
static int WaitMenuBmp_Update(WaitMenuSelect_t* w);
static void WaitMenuBmp2_Destroy(Wait_t* w);
static uint32_t WaitMenuBmp2_BlinkInterval(WaitMenuSelect_t* w);
static int WaitMenuBmp2_Update(WaitMenuSelect_t* w);

// isDirty is WaitTextOut's: the window field sits at the same offset in both classes
static const MenuVtbl_t WaitMenuSelect_Vtbl = {
	{WaitMenuSelect_Destroy, WaitMenuSelect_Poll, Wait_SetTimer, WaitTextOut_IsDirty, Wait_OnMessage},
	WaitMenuSelect_FilterInput, WaitMenuSelect_OnMouseMove, WaitMenuSelect_BlinkInterval, WaitMenuSelect_Highlight,
	WaitMenuSelect_Update};
static const MenuVtbl_t WaitMenuBmp_Vtbl = {
	{WaitMenuBmp_Destroy, WaitMenuSelect_Poll, Wait_SetTimer, WaitTextOut_IsDirty, Wait_OnMessage},
	WaitMenuBmp_FilterInput, WaitMenuBmp_OnMouseMove, WaitMenuSelect_BlinkInterval, WaitMenuBmp_Highlight,
	WaitMenuBmp_Update};
static const MenuVtbl_t WaitMenuBmp2_Vtbl = {
	{WaitMenuBmp2_Destroy, WaitMenuSelect_Poll, Wait_SetTimer, WaitTextOut_IsDirty, Wait_OnMessage},
	WaitMenuBmp_FilterInput, WaitMenuBmp_OnMouseMove, WaitMenuBmp2_BlinkInterval, WaitMenuBmp_Highlight,
	WaitMenuBmp2_Update};

// ========================================================================
// WaitMenuSelect
// ========================================================================

static void WaitMenuSelect_SetSelected(WaitMenuSelect_t* w, int idx)
{
	w->selected = idx;
}

static void WaitMenuSelect_SetAllowCancel(WaitMenuSelect_t* w, int on)
{
	w->allowCancel = on;
}

/* the shared constructor: the text layer is reset to opaque and visible
 * and the item layers switched off; the menu owns the window's layer key
 * on both input stacks, a subscription to window message 0x200 (mouse
 * moves) and a place on the input-focus list for its lifetime */
static void WaitMenuSelect_Ctor(WaitMenuSelect_t* w, Thread_t* t, Window_t* win)
{
	Wait_Ctor(&w->w, t);
	w->w.vt = &WaitMenuSelect_Vtbl.base;
	w->win = win;
	w->count = 0;
	WaitMenuSelect_SetSelected(w, 0);
	WaitMenuSelect_SetAllowCancel(w, 0);
	w->hover = -1;
	w->shown = 0;
	w->phase = 0;
	Window_SetTextLevel(win, 0);
	Window_ShowText(win, 1);
	Window_RecompositeAll(win);
	Window_ItemsDisableAll(win);
	WinMsgWait_Register(t, 0x200);
	MouseLayer_Push(WIN_KEY(w));
	KeyLayer_Push(WIN_KEY(w));
	Input_Poll(WIN_KEY(w), WIN_KEY(w)); // swallow what is pending
	InputFocus_Add(w->w.serial, WIN_KEY(w));
	w->active = 0;
}

// gives the layers and the subscription back and frees the string copies
static void WaitMenuSelect_Dtor(WaitMenuSelect_t* w)
{
	int i;
	w->w.vt = &WaitMenuSelect_Vtbl.base;
	MouseLayer_Pop(WIN_KEY(w));
	KeyLayer_Pop(WIN_KEY(w));
	WinMsgWait_Unregister(w->w.thread, 0x200);
	for(i = 0; i < w->count; i++)
		BGI_Free(w->items[i]);
	Wait_Dtor(&w->w);
}

static void WaitMenuSelect_Destroy(Wait_t* w)
{
	WaitMenuSelect_Dtor((WaitMenuSelect_t*)w);
	BGI_Free(w);
}

/* render the entries into the window's text layer.  The text area is
 * split into `cols` equal columns; entry i goes to column i % cols, row
 * i / cols, one line advance per row, left-aligned or centred in its
 * column.  Each entry is drawn into a scratch bitmap first so its pixel
 * width is known; the resulting rectangles (window relative, into `rects`)
 * are what the hit test and the cursor use.  A window colour table
 * ("90 A7") overrides `colour` per entry. */
static void Menu_Layout(Window_t* win, Rect_t* rects, int n, const char* const* items, int cols, int centre,
	uint32_t colour)
{
	Rect_t area, r;
	int32_t table[MENU_MAX];
	int colX[MENU_MAX], colMid[MENU_MAX];
	int width, half, font, size, advance, prop, hasTable, i, col;
	Window_GetTextArea(win, &area);
	width = area.r - area.l + 1;
	half = (width / cols) / 2;
	for(col = 0; col < cols; col++)
	{
		colX[col] = area.l + (col * width) / cols;
		colMid[col] = colX[col] + half;
	}
	font = Window_GetFont(win);
	size = Window_GetFontSize(win);
	advance = Window_LineAdvance(win);
	prop = Window_GetProportional(win);
	hasTable = Window_GetTable(win, table);
	Window_ClearText(win);
	col = 0;
	for(i = 0; i < n; i++)
	{
		Bmp_t cell;
		int textW, x, y;
		// generous scratch width: two font sizes per byte of the string
		Bmp_AllocScreen(&cell, (int)strlen(items[i]) * size * 2, size, 1);
		Bmp_Clear(&cell, 0);
		BmpMgr_DrawText(gTextBmpMgr, &cell, &textW, 0, 0, items[i], font, hasTable ? (uint32_t)table[i] : colour,
			0x80, 0, prop);
		x = centre ? colMid[col] - (int)((uint32_t)textW >> 1) : colX[col];
		y = (i / cols) * advance + area.t;
		rects[i].l = x;
		rects[i].t = y;
		rects[i].r = x + textW - 1;
		rects[i].b = y + size - 1;
		Window_DrawBmpToText(win, &r, x, y, &cell, 0x80, 0);
		Bmp_Free(&cell);
		col = (i + 1) % cols;
	}
	Window_TextScreenRect(win, &r);
	Gfx_AddDirty(gTextGfx, win->obj.vt->sortKey(&win->obj), &r);
}

// keep copies of the strings and lay them out
static void WaitMenuSelect_Setup(WaitMenuSelect_t* w, int n, const char* const* items, int cols, int centre,
	uint32_t colour)
{
	int i;
	w->count = n;
	w->cols = cols;
	for(i = 0; i < n; i++)
	{
		w->items[i] = (char*)BGI_Alloc(strlen(items[i]) + 1);
		strcpy(w->items[i], items[i]);
	}
	w->centre = centre;
	w->colour = colour;
	Menu_Layout(w->win, w->rects, n, (const char* const*)w->items, cols, centre, colour);
	Wait_MarkDirty(&w->w);
}

// the entry under the pointer (inclusive rectangles), -1 for none
static int WaitMenuSelect_HitTest(WaitMenuSelect_t* w)
{
	int mouse[2], pos[2], x, y, i;
	GetMouseClientPos(mouse);
	w->win->obj.vt->getPos(&w->win->obj, pos);
	x = mouse[0] - pos[0];
	y = mouse[1] - pos[1];
	for(i = 0; i < w->count; i++)
	{
		const Rect_t* r = &w->rects[i];
		if(r->l <= x && x <= r->r && r->t <= y && y <= r->b)
			return i;
	}
	return -1;
}

// does grid cell (col, row) hold an entry?
static int WaitMenuSelect_IsValidCell(WaitMenuSelect_t* w, int col, int row)
{
	return w->cols * row + col < w->count;
}

// glide the pointer to the centre of an entry (the wheel's way of selecting)
static void WaitMenuSelect_MoveMouseToItem(WaitMenuSelect_t* w, int idx)
{
	const Rect_t* r = &w->rects[idx];
	int pos[2];
	w->win->obj.vt->getPos(&w->win->obj, pos);
	MouseMove_Start(pos[0] + r->l + ((r->r - r->l + 1) >> 1), pos[1] + r->t + ((r->b - r->t + 1) >> 1),
		gMenuWheelCurve, gMenuWheelDuration, gMenuWheelFps, 0);
}

/* react to the input bits of the pass.  0 nothing happened, 1 the wait is
 * over (a selection or a cancel), 2 the cursor moved.  The priorities are:
 * number keys, then click / decide / cancel, then the direction keys and
 * the wheel.  Direction keys walk the grid in the pressed direction,
 * wrapping and skipping the empty cells of the last row; the wheel (when
 * "90 A6" enabled it) moves the pointer instead of the cursor so the mouse
 * tracking selects the entry. */
static int WaitMenuSelect_ProcessInput(WaitMenuSelect_t* w)
{
	static const uint32_t kNumberKeys[] = {0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000, 0x400000,
		0x800000, 0x1000000, 0x2000000, 0};
	static const uint32_t kArrowKeys[] = {0x1000, 0x2000, 0x4000, 0x8000, 0};
	static const uint32_t kArrowWheelKeys[] = {0x40, 0x80, 0x1000, 0x2000, 0x4000, 0x8000, 0};
	uint32_t mask = w->input, key = 0;
	const uint32_t* keys;
	int i, row, col, rows, n;
	if(mask & 0x3ff0000) // the number keys: '1' .. '9', '0' pick entries 0 .. 9 (a key beyond the count does nothing)
	{
		for(i = 0; kNumberKeys[i]; i++)
		{
			if((mask & kNumberKeys[i]) && i < w->count)
			{
				WaitMenuSelect_SetSelected(w, i);
				return 1;
			}
		}
		return 0;
	}
	if(mask & 0x303)
	{
		if(mask & 0x202) // cancel key or right button
		{
			WaitMenuSelect_SetSelected(w, -1);
			return 1;
		}
		if(mask & 1) // a click selects what is under the pointer; the decide key the cursor entry
		{
			int hit = WaitMenuSelect_HitTest(w);
			if(hit < 0)
				return 0;
			WaitMenuSelect_SetSelected(w, hit);
		}
		return 1;
	}
	if(!(mask & 0xf0c0))
		return 0;
	keys = gMenuWheelOn ? kArrowWheelKeys : kArrowKeys;
	for(i = 0; keys[i]; i++)
	{
		if(mask & keys[i])
		{
			key = keys[i];
			break;
		}
	}
	row = w->selected / w->cols;
	col = w->selected % w->cols;
	rows = (w->count + w->cols - 1) / w->cols;
	switch(key)
	{
		case 0x40:   // wheel up
		case 0x1000: // up
			for(n = 0; n < rows; n++)
			{
				if(--row < 0)
					row = rows - 1;
				if(WaitMenuSelect_IsValidCell(w, col, row))
				{
					if(key == 0x40)
					{
						WaitMenuSelect_MoveMouseToItem(w, row * w->cols + col);
						return 2;
					}
					WaitMenuSelect_SetSelected(w, row * w->cols + col);
					break;
				}
			}
			break;
		case 0x80:   // wheel down
		case 0x2000: // down
			for(n = 0; n < rows; n++)
			{
				if(++row >= rows)
					row = 0;
				if(WaitMenuSelect_IsValidCell(w, col, row))
				{
					if(key == 0x80)
					{
						WaitMenuSelect_MoveMouseToItem(w, row * w->cols + col);
						return 2;
					}
					WaitMenuSelect_SetSelected(w, row * w->cols + col);
					break;
				}
			}
			break;
		case 0x4000: // left
			for(n = 0; n < w->cols; n++)
			{
				if(--col < 0)
					col = w->cols - 1;
				if(WaitMenuSelect_IsValidCell(w, col, row))
				{
					WaitMenuSelect_SetSelected(w, row * w->cols + col);
					break;
				}
			}
			return 2;
		case 0x8000: // right
			for(n = 0; n < w->cols; n++)
			{
				if(++col >= w->cols)
					col = 0;
				if(WaitMenuSelect_IsValidCell(w, col, row))
				{
					WaitMenuSelect_SetSelected(w, row * w->cols + col);
					break;
				}
			}
			return 2;
		default: break;
	}
	return key ? 2 : 0;
}

// the input bits the full menu reacts to
static uint32_t WaitMenuSelect_FilterInput(WaitMenuSelect_t* w, uint32_t mask)
{
	return mask & 0x3fff3c3; // left / right button, wheel, decide / cancel, directions, numbers
}

/* the pointer moved: the entry under it becomes the selection (nothing
 * happens while the menu is inactive).  A change of the entry under the
 * pointer (also to none) is reported as message 0x10000002.  1 when the
 * selection changed. */
static int WaitMenuSelect_OnMouseMove(WaitMenuSelect_t* w)
{
	int changed = 0, hit;
	if(!w->active)
		return 0;
	hit = WaitMenuSelect_HitTest(w);
	if(hit >= 0 && hit != w->selected)
	{
		WaitMenuSelect_SetSelected(w, hit);
		changed = 1;
	}
	if(hit != w->hover)
	{
		MsgQueue_Post(0x10000002, Thread_GetId(w->w.thread), (uint32_t)hit);
		w->hover = hit;
	}
	return changed;
}

// the blink interval of the full menu: the "90 A5" setting
static uint32_t WaitMenuSelect_BlinkInterval(WaitMenuSelect_t* w)
{
	return gMenuBlinkMs;
}

/* the cursor is the selected entry's text redrawn in the cursor colour into
 * window item 0.  A colour of 0 draws nothing (the blink's "off" phase);
 * returns whether the item is to be shown. */
static int Menu_DrawCursor(WaitMenuSelect_t* w, uint32_t colour)
{
	const Rect_t* r = &w->rects[w->selected];
	Bmp_t cell;
	int textW;
	if(colour == 0)
		return 0;
	Bmp_AllocScreen(&cell, r->r - r->l + 1, Window_GetFontSize(w->win), 1);
	Bmp_Clear(&cell, 0);
	BmpMgr_DrawText(gTextBmpMgr, &cell, &textW, 0, 0, w->items[w->selected], Window_GetFont(w->win), colour, 0x80,
		0, Window_GetProportional(w->win));
	Window_ItemSetBitmap(w->win, 0, &cell);
	Bmp_Free(&cell);
	return 1;
}

// a window item's screen rectangle becomes dirty (when it has one)
static void Menu_DirtyItem(WaitMenuSelect_t* w, int item)
{
	Rect_t r;
	if(Window_ItemRect(w->win, &r, item))
		Gfx_AddDirty(gTextGfx, WIN_KEY(w), &r);
}

// move the cursor to the selection (or hide it when there is none) and restart the blink in phase 0
static void WaitMenuSelect_Highlight(WaitMenuSelect_t* w)
{
	int on;
	Menu_DirtyItem(w, 0); // where the cursor was
	if(w->selected == -1)
	{
		w->w.vt->setTimer(&w->w, 0);
		Window_ItemEnable(w->win, 0, 0);
		Wait_MarkDirty(&w->w);
		return;
	}
	w->w.vt->setTimer(&w->w, MENU_VT(w)->blinkInterval(w));
	on = Menu_DrawCursor(w, gMenuCursorColour[0]);
	Window_ItemSet(w->win, 0, w->rects[w->selected].l, w->rects[w->selected].t, 0);
	Window_ItemEnable(w->win, 0, on);
	Menu_DirtyItem(w, 0);
	w->phase = 0;
	Wait_MarkDirty(&w->w);
}

// the blink timer fired: the other phase's colour
static void WaitMenuSelect_Blink(WaitMenuSelect_t* w)
{
	w->w.vt->setTimer(&w->w, MENU_VT(w)->blinkInterval(w));
	Menu_DirtyItem(w, 0);
	w->phase ^= 1;
	Window_ItemEnable(w->win, 0, Menu_DrawCursor(w, gMenuCursorColour[w->phase])); // the item keeps its position
	Menu_DirtyItem(w, 0);
	Wait_MarkDirty(&w->w);
}

/* one pass with something to do (the update slot of the full menu).  The
 * first call draws the cursor; input goes to ProcessInput, a mouse move to
 * the class's onMouseMove.  A cursor move redraws the cursor and reports
 * message 0x10000001; otherwise the blink timer is served.  1 when the wait
 * is over. */
static int WaitMenuSelect_Update(WaitMenuSelect_t* w)
{
	int done = 0, moved;
	if(!w->shown)
	{
		MENU_VT(w)->highlight(w);
		w->shown++;
	}
	if(w->input)
	{
		switch(WaitMenuSelect_ProcessInput(w))
		{
			case 1:
				done = 1;
				w->shown = 0;
				moved = 1;
				break;
			case 2: moved = 1; break;
			default: moved = 0; break;
		}
	}
	else
		moved = w->msgArrived && MENU_VT(w)->onMouseMove(w);
	if(!moved)
	{
		if(Wait_TimerExpired(&w->w))
			WaitMenuSelect_Blink(w);
		return 0;
	}
	MENU_VT(w)->highlight(w);
	if(done)
		return 1;
	MsgQueue_Post(0x10000001, Thread_GetId(w->w.thread), (uint32_t)w->selected);
	return 0;
}

/* the shared poll: gather the pass's input (the class's filter, the cancel
 * keys only with allowCancel), the mouse-move message and the focus state,
 * run the class's update when any of them or the timer calls for it, and
 * push the results when it is over or waits stopped blocking */
static int WaitMenuSelect_Poll(Wait_t* base)
{
	WaitMenuSelect_t* w = (WaitMenuSelect_t*)base;
	WinMsgRec_t rec;
	int done = 0, wasActive;
	Wait_Tick(base);
	w->input = MENU_VT(w)->filterInput(w, Input_Poll(WIN_KEY(w), WIN_KEY(w)));
	if(!w->allowCancel)
		w->input &= ~0x202u;
	WinMsgWait_Poll(&rec, base->thread, 0x200);
	w->msgArrived = rec.arrived;
	wasActive = w->active;
	w->active = InputFocus_IsTop(WIN_KEY(w)) && MenuAllowed();
	if(Wait_TimerExpired(base) || w->input || w->msgArrived || w->active != wasActive)
		done = MENU_VT(w)->update(w);
	Wait_Flush(base);
	if(!done && Wait_IsBlocking(base))
		return 0;
	Thread_Push(base->thread, (uint32_t)w->selected);
	Thread_Push(base->thread, Wait_IsBlocking(base) ? (uint32_t)w->selected : 0xffffffffu);
	return 1;
}

// ========================================================================
// WaitMenuBmp: decoration bitmaps beside the selection, mouse only
// ========================================================================

static void WaitMenuBmp_Ctor(WaitMenuBmp_t* w, Thread_t* t, Window_t* win)
{
	WaitMenuSelect_Ctor(&w->m, t, win);
	w->m.w.vt = &WaitMenuBmp_Vtbl.base;
	w->hasBmp[0] = 0;
	w->hasBmp[1] = 0;
}

static void WaitMenuBmp_Dtor(WaitMenuBmp_t* w)
{
	w->m.w.vt = &WaitMenuBmp_Vtbl.base;
	WaitMenuSelect_Dtor(&w->m);
}

static void WaitMenuBmp_Destroy(Wait_t* w)
{
	WaitMenuBmp_Dtor((WaitMenuBmp_t*)w);
	BGI_Free(w);
}

// the input bits the bitmap menu reacts to
static uint32_t WaitMenuBmp_FilterInput(WaitMenuSelect_t* w, uint32_t mask)
{
	return mask & 0x3ff0203; // left / right button, cancel key, numbers: no keyboard navigation
}

// the entry under the pointer (none while inactive) becomes the selection; 1 when it changed
static int WaitMenuBmp_OnMouseMove(WaitMenuSelect_t* w)
{
	int hit = w->active ? WaitMenuSelect_HitTest(w) : -1;
	if(hit == w->selected)
		return 0;
	MsgQueue_Post(0x10000002, Thread_GetId(w->w.thread), (uint32_t)hit);
	WaitMenuSelect_SetSelected(w, hit);
	return 1;
}

/* the decoration bitmaps go into window items 1 and 2, with their offsets
 * from the selected entry.  -1 skips one.  0 ok, 0x8001 / 0x8003 unknown
 * bitmap, 0x8002 / 0x8004 not usable in the window (the starter maps these
 * to 0x80000005 .. 8). */
static int WaitMenuBmp_SetBitmaps(WaitMenuBmp_t* w, int bmp1, int dx1, int dy1, int bmp2, int dx2, int dy2)
{
	Bmp_t bmp;
	w->hasBmp[0] = 0;
	if(bmp1 != -1)
	{
		if(!BmpMgr_GetInfo(gTextBmpMgr, &bmp, bmp1))
			return 0x8001;
		if(Window_ItemSetBitmap(w->m.win, 1, &bmp))
			return 0x8002;
		w->hasBmp[0] = 1;
	}
	w->hasBmp[1] = 0;
	if(bmp2 != -1)
	{
		if(!BmpMgr_GetInfo(gTextBmpMgr, &bmp, bmp2))
			return 0x8003;
		if(Window_ItemSetBitmap(w->m.win, 2, &bmp))
			return 0x8004;
		w->hasBmp[1] = 1;
	}
	w->off[0][0] = dx1;
	w->off[0][1] = dy1;
	w->off[1][0] = dx2;
	w->off[1][1] = dy2;
	return 0;
}

/* the text cursor plus the two decorations: the first at its offset from
 * the entry's top-left, the second additionally past the entry's right
 * edge.  Both are keyed on the first bitmap's presence (the original tests
 * hasBmp[0] for either item, so a second bitmap alone is never shown). */
static void WaitMenuBmp_Highlight(WaitMenuSelect_t* base)
{
	WaitMenuBmp_t* w = (WaitMenuBmp_t*)base;
	const Rect_t* r = &base->rects[base->selected];
	int xoff[2], k;
	Rect_t dirty;
	WaitMenuSelect_Highlight(base);
	xoff[0] = 0;
	xoff[1] = r->r - r->l + 1;
	for(k = 0; k < 2; k++)
	{
		int item = k + 1;
		Menu_DirtyItem(base, item); // where it was
		if(base->selected == -1)
		{
			Window_ItemEnable(base->win, item, 0);
			continue;
		}
		if(!w->hasBmp[0])
			continue;
		Window_ItemSet(base->win, item, r->l + w->off[k][0] + xoff[k], r->t + w->off[k][1], 0);
		Window_ItemEnable(base->win, item, 1);
		Window_ItemRect(base->win, &dirty, item); // (added even when the item has no pixels)
		Gfx_AddDirty(gTextGfx, WIN_KEY(base), &dirty);
	}
}

// like the base's update without the blink timer; 1 when the wait is over
static int WaitMenuBmp_Update(WaitMenuSelect_t* w)
{
	if(!w->shown)
	{
		MENU_VT(w)->highlight(w);
		w->shown++;
	}
	if(w->input)
	{
		switch(WaitMenuSelect_ProcessInput(w))
		{
			case 1: return 1;
			case 2: break;
			default: return 0;
		}
	}
	else if(!MENU_VT(w)->onMouseMove(w))
		return 0;
	MENU_VT(w)->highlight(w);
	MsgQueue_Post(0x10000001, Thread_GetId(w->w.thread), (uint32_t)w->selected);
	return 0;
}

// ========================================================================
// WaitMenuBmp2: flash the preselected entry and finish
// ========================================================================

static void WaitMenuBmp2_Ctor(WaitMenuBmp2_t* w, Thread_t* t, Window_t* win)
{
	WaitMenuBmp_Ctor(&w->b, t, win);
	w->b.m.w.vt = &WaitMenuBmp2_Vtbl.base;
	w->flashes = 0;
}

static void WaitMenuBmp2_Destroy(Wait_t* w)
{
	w->vt = &WaitMenuBmp2_Vtbl.base;
	WaitMenuBmp_Dtor((WaitMenuBmp_t*)w);
	BGI_Free(w);
}

// 50 ms: the twenty flashes take a second
static uint32_t WaitMenuBmp2_BlinkInterval(WaitMenuSelect_t* w)
{
	return 50;
}

// twenty blink steps (one second) on the selection, then done; input is ignored
static int WaitMenuBmp2_Update(WaitMenuSelect_t* base)
{
	WaitMenuBmp2_t* w = (WaitMenuBmp2_t*)base;
	if(w->flashes >= 20 || base->selected == -1)
		return 1;
	if(Wait_TimerExpired(&base->w))
	{
		if(w->flashes == 0)
			MENU_VT(base)->highlight(base);
		else
			WaitMenuSelect_Blink(base);
		w->flashes++;
	}
	return 0;
}

// ========================================================================
// the starters
// ========================================================================

/* the argument checks shared by the three entry points: 0 ok, 0x80000001
 * n outside 1 .. 16, 0x80000002 init outside 0 .. n - 1, 0x80000003 cols
 * outside 0 .. 16.  (A column count of 0 passes and divides by zero in the
 * layout, as in the original.) */
static int Menu_CheckArgs(int n, int cols, int init)
{
	if(n < 1 || n > MENU_MAX)
		return (int)0x80000001;
	if(init < 0 || init >= n)
		return (int)0x80000002;
	if(cols < 0 || cols > MENU_MAX)
		return (int)0x80000003;
	return 0;
}

/* "90 A0": the full menu.  `items` is the script's array of n tagged
 * string pointers, resolved through t.  0 ok, the Menu_CheckArgs codes,
 * -1 no such window, 0x80000004 no font. */
int StartMenuSelect(Thread_t* t, uint32_t h, int n, const void* items, int cols, int centre, uint32_t colour,
	int init, int allowCancel)
{
	const char* strs[MENU_MAX];
	Window_t* win;
	WaitMenuSelect_t* w;
	int err = Menu_CheckArgs(n, cols, init);
	if(err)
		return err;
	ResolvePtrArray((void**)strs, t, (const uint32_t*)items, n);
	win = (Window_t*)Gfx_FindWindow(gGfx, h);
	if(!win)
		return -1;
	if(!Window_GetFont(win))
		return (int)0x80000004;
	w = (WaitMenuSelect_t*)BGI_Alloc(sizeof *w);
	WaitMenuSelect_Ctor(w, t, win);
	WaitMenuSelect_Setup(w, n, strs, cols, centre, colour);
	WaitMenuSelect_SetSelected(w, init);
	WaitMenuSelect_SetAllowCancel(w, allowCancel);
	Thread_SetWait(t, &w->w);
	return 0;
}

/* "90 A1": lay the entries out as a menu would and leave them on the text
 * layer (opaque, visible, items off) without waiting.  0 ok, 0x80000001 /
 * 0x80000003 bad counts, -1 no such window. */
int Menu_Draw(uint32_t h, int n, const void* items, Thread_t* t, int cols, int centre, uint32_t colour)
{
	const char* strs[MENU_MAX];
	Rect_t rects[MENU_MAX];
	Window_t* win;
	int err = Menu_CheckArgs(n, cols, 0);
	if(err)
		return err;
	ResolvePtrArray((void**)strs, t, (const uint32_t*)items, n);
	win = (Window_t*)Gfx_FindWindow(gGfx, h);
	if(!win)
		return -1;
	Menu_Layout(win, rects, n, strs, cols, centre, colour);
	Window_SetTextLevel(win, 0);
	Window_ShowText(win, 1);
	Window_RecompositeAll(win);
	Window_ItemsDisableAll(win);
	return 0;
}

/* "90 A2" (mode 1: WaitMenuBmp) / "90 A3" (mode 0: WaitMenuBmp2): the
 * menu with decoration bitmaps.  Codes as StartMenuSelect, plus
 * 0x80000005 / 7 an unknown bitmap and 0x80000006 / 8 one the window
 * cannot hold, for bmp1 / bmp2. */
int StartMenuSelectBmp(Thread_t* t, uint32_t h, int n, const void* items, int cols, int centre, uint32_t colour,
	int init, int allowCancel, int bmp1, int dx1, int dy1, int bmp2, int dx2, int dy2, int mode)
{
	const char* strs[MENU_MAX];
	Window_t* win;
	WaitMenuBmp_t* w;
	int err = Menu_CheckArgs(n, cols, init);
	if(err)
		return err;
	ResolvePtrArray((void**)strs, t, (const uint32_t*)items, n);
	win = (Window_t*)Gfx_FindWindow(gGfx, h);
	if(!win)
		return -1;
	if(!Window_GetFont(win))
		return (int)0x80000004;
	if(mode)
	{
		w = (WaitMenuBmp_t*)BGI_Alloc(sizeof *w);
		WaitMenuBmp_Ctor(w, t, win);
	}
	else
	{
		WaitMenuBmp2_t* w2 = (WaitMenuBmp2_t*)BGI_Alloc(sizeof *w2);
		WaitMenuBmp2_Ctor(w2, t, win);
		w = &w2->b;
	}
	WaitMenuSelect_Setup(&w->m, n, strs, cols, centre, colour);
	WaitMenuSelect_SetSelected(&w->m, init);
	WaitMenuSelect_SetAllowCancel(&w->m, allowCancel);
	err = WaitMenuBmp_SetBitmaps(w, bmp1, dx1, dy1, bmp2, dx2, dy2);
	if(err)
	{
		Wait_Release(&w->m.w);
		switch(err)
		{
			case 0x8001: return (int)0x80000005;
			case 0x8002: return (int)0x80000006;
			case 0x8003: return (int)0x80000007;
			case 0x8004: return (int)0x80000008;
			default: return 0; // unreachable; the original would hand the freed object to the thread
		}
	}
	Thread_SetWait(t, &w->m.w);
	return 0;
}

// the name of a wait class of this file, for the debugger (NULL: not one of these)
const char* WaitMenu_ClassName(const WaitVtbl_t* vt)
{
	if(vt == &WaitMenuSelect_Vtbl.base)
		return "WaitMenuSelect";
	if(vt == &WaitMenuBmp_Vtbl.base)
		return "WaitMenuBmp";
	if(vt == &WaitMenuBmp2_Vtbl.base)
		return "WaitMenuBmp2";
	return NULL;
}
