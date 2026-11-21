/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dbg.c - the debugger's window, run control and the views' plumbing
 * (src/include/bgi/dbg.h for the engine, dbg_internal.h for the views)
 *
 * The window is a surface of DBG_W x DBG_H pixels presented through the
 * host's debugger's window, or over the game's picture when the host has
 * none ("overlay").  Each pass of the main loop calls Dbg_Pass, which
 * redraws at most every 40 ms; the input arrives through the host's calls
 * (OpenBGI_DebugKey and its kin) between passes and is queued, so that
 * the views see a consistent engine state while they draw and act.
 *
 * Keys in the debugger: F5 run / pause, F6 step a pass, F7 step one
 * instruction (per runnable thread), F9 breakpoint on the selected
 * listing line, T trace into the log, 1 .. 8 the views, Tab the next
 * view, Escape or F12 close.  The game's window keeps its keys; F12 there
 * toggles the debugger.  docs/modding/debugger.md describes the tool.
 */
#include "dbg_internal.h"
#include "bgi/display.h"
#include "bgi/vm.h"
#include "bgi/version.h"
#include "bgi/core.h"

#include <stdarg.h>

#define DBG_W    1152 // the window's size in pixels (the overlay takes the game window's)
#define DBG_H    768
#define TAB_H    28 // the tab bar at the top
#define STATUS_H 20 // the status line at the bottom

// ---- the views and the input queue -------------------------------------------------------

static const DbgViewOps_t* const kViews[DBG_VIEW_COUNT] = {
	&kDbgViewThreads, &kDbgViewPrograms, &kDbgViewBitmaps, &kDbgViewObjects, &kDbgViewSound, &kDbgViewMemory, &kDbgViewLog,
	&kDbgViewTargets};

// queued input: the events come from the pump, between passes

// append an event to the queue (dropped when the queue is full)
static void Queue(OpenBGI_t* o, int type, int a, int b, int c, int d)
{
	DbgEvent_t* e;
	if(o->dbg.eventCount == DBG_EVENT_QUEUE)
		return;
	e = &o->dbg.events[(o->dbg.eventHead + o->dbg.eventCount) % DBG_EVENT_QUEUE];
	e->type = type;
	e->a = a;
	e->b = b;
	e->c = c;
	e->d = d;
	o->dbg.eventCount++;
}

// ---- the log ------------------------------------------------------------------------------

// Format a line into the log ring (DBG_LOG_LINES kept, the oldest overwritten); usable while the debugger is closed.
void Dbg_Log(OpenBGI_t* o, const char* fmt, ...)
{
	va_list ap;
	DbgLogLine_t* l = &o->dbg.dbgLog[o->dbg.dbgLogNext];
	va_start(ap, fmt);
	vsnprintf(l->text, sizeof l->text, fmt, ap);
	va_end(ap);
	o->dbg.dbgLogNext = (o->dbg.dbgLogNext + 1) % DBG_LOG_LINES;
	if(o->dbg.dbgLogCount < DBG_LOG_LINES)
		o->dbg.dbgLogCount++;
}

// ---- lists ---------------------------------------------------------------------------------

// Keep the selection and the first shown row inside the list (rows at least 1).
void DbgList_Clamp(DbgList_t* l)
{
	if(l->rows < 1)
		l->rows = 1;
	if(l->sel >= l->count)
		l->sel = l->count - 1;
	if(l->sel < 0 && l->count > 0)
		l->sel = 0;
	if(l->top > l->count - l->rows)
		l->top = l->count - l->rows;
	if(l->top < 0)
		l->top = 0;
}

// Scroll just enough to bring a row into view.
void DbgList_Show(DbgList_t* l, int row)
{
	if(row < l->top)
		l->top = row;
	else if(row >= l->top + l->rows)
		l->top = row - l->rows + 1;
	DbgList_Clamp(l);
}

// Move the selection by a navigation key (virtual key codes) and keep it in view; other keys do nothing.
void DbgList_Key(DbgList_t* l, int vk)
{
	switch(vk)
	{
		case 0x26: l->sel--; break;              // up
		case 0x28: l->sel++; break;              // down
		case 0x21: l->sel -= l->rows - 1; break; // page up
		case 0x22: l->sel += l->rows - 1; break; // page down
		case 0x24: l->sel = 0; break;            // home
		case 0x23: l->sel = l->count - 1; break; // end
		default: return;
	}
	DbgList_Clamp(l);
	DbgList_Show(l, l->sel);
}

// Scroll by `rows` (negative: up) without moving the selection.
void DbgList_Scroll(DbgList_t* l, int rows)
{
	l->top += rows;
	DbgList_Clamp(l);
}

// The row under (x, y), -1 outside the pane or below the last row.
int DbgList_Hit(const DbgList_t* l, int x, int y)
{
	int row;
	if(x < l->x || x >= l->x + l->w || y < l->y || y >= l->y + l->h)
		return -1;
	row = l->top + (y - l->y) / DBG_CELL_H;
	return row < l->count ? row : -1;
}

// ---- threads and breakpoints ------------------------------------------------------------------

/* The thread with id o->dbg.dbgThreadId; when it is gone, the first
 * thread of the scheduler becomes the selected one.  NULL before the VM
 * exists. */
Thread_t* Dbg_SelectedThread(OpenBGI_t* o)
{
	Thread_t* t;
	if(!o->vm.rootThread)
		return NULL;
	for(t = Thread_Next(o->vm.rootThread); t; t = Thread_Next(t))
		if(t->id == o->dbg.dbgThreadId)
			return t;
	t = Thread_Next(o->vm.rootThread);
	if(t)
		o->dbg.dbgThreadId = t->id;
	return t;
}

// the scheduler calls Dbg_OnInsn only while there is something for it to do
static void Rehook(OpenBGI_t* o)
{
	o->vm.dbgHooked = o->dbg.dbgBreakCount > 0 || o->dbg.dbgTrace;
}

// Is there a breakpoint at offset `off` of module `module`?
int Dbg_HasBreak(OpenBGI_t* o, const char* module, uint32_t off)
{
	int i;
	for(i = 0; i < o->dbg.dbgBreakCount; i++)
		if(o->dbg.dbgBreaks[i].off == off && strcmp(o->dbg.dbgBreaks[i].module, module) == 0)
			return 1;
	return 0;
}

/* Clear the breakpoint at (module, off) when there is one, else set it:
 * 1 set, 0 cleared (or not set because DBG_MAX_BREAKS exist already).
 * Either way the log gets a line. */
int Dbg_ToggleBreak(OpenBGI_t* o, const char* module, uint32_t off)
{
	int i;
	for(i = 0; i < o->dbg.dbgBreakCount; i++)
	{
		if(o->dbg.dbgBreaks[i].off == off && strcmp(o->dbg.dbgBreaks[i].module, module) == 0)
		{
			o->dbg.dbgBreaks[i] = o->dbg.dbgBreaks[--o->dbg.dbgBreakCount];
			Dbg_Log(o, "breakpoint cleared: %s+0x%x", module, (unsigned)off);
			Rehook(o);
			return 0;
		}
	}
	if(o->dbg.dbgBreakCount == DBG_MAX_BREAKS)
		return 0;
	snprintf(o->dbg.dbgBreaks[o->dbg.dbgBreakCount].module, sizeof o->dbg.dbgBreaks[0].module, "%s", module);
	o->dbg.dbgBreaks[o->dbg.dbgBreakCount].off = off;
	o->dbg.dbgBreakCount++;
	Dbg_Log(o, "breakpoint set: %s+0x%x", module, (unsigned)off);
	Rehook(o);
	return 1;
}

// ---- the scheduler's hooks -----------------------------------------------------------------------

/* May the threads run this pass?  Yes while running; while paused only
 * when a step was asked for, and then with the instruction limit the
 * step needs (1 per turn for a step by instruction). */
int Dbg_GateRun(OpenBGI_t* o)
{
	if(!o->dbg.dbgPaused)
		return 1;
	if(o->dbg.step)
	{
		o->vm.dbgInsnLimit = o->dbg.step == 2 ? 1 : 0x100000;
		return 1;
	}
	return 0;
}

/* Before an instruction of thread t at code offset opIP (op is its first
 * byte): log it when tracing; at a breakpoint pause the VM, select the
 * thread, open the window when it is closed and return 1 so that the turn
 * ends before the instruction.  The breakpoint the pause was resumed from
 * is passed once by the same thread. */
int Dbg_OnInsn(OpenBGI_t* o, Thread_t* t, uint32_t opIP, int op)
{
	const char* module;
	uint32_t base, size;
	if(o->dbg.dbgTrace)
	{
		if(Dbg_ModuleAt(t, opIP, &module, &base, &size))
			Dbg_Log(o, "t%u %s+%04x: %02x %02x", (unsigned)t->id, module, (unsigned)(opIP - base), op,
				opIP + 1 < t->codeSize ? t->code[opIP + 1] : 0);
	}
	if(o->dbg.dbgBreakCount && Dbg_ModuleAt(t, opIP, &module, &base, &size) && Dbg_HasBreak(o, module, opIP - base))
	{
		if(o->dbg.skipThread == t->id && o->dbg.skipOff == opIP)
		{ // resumed from this very breakpoint: it is passed once
			o->dbg.skipThread = ~0u;
			return 0;
		}
		o->dbg.dbgPaused = 1;
		o->dbg.step = 0;
		o->dbg.dbgThreadId = t->id;
		o->dbg.skipThread = t->id;
		o->dbg.skipOff = opIP;
		Dbg_Log(o, "breakpoint: thread %u at %s+0x%x", (unsigned)t->id, module, (unsigned)(opIP - base));
		if(!o->dbg.open)
			Dbg_Toggle(o);
		return 1;
	}
	return 0;
}

// let the threads run again, any pending step forgotten
static void Resume(OpenBGI_t* o)
{
	o->dbg.dbgPaused = 0;
	o->dbg.step = 0;
	o->vm.dbgInsnLimit = 0x100000;
}

// hold the threads from the next pass on
static void Pause(OpenBGI_t* o)
{
	o->dbg.dbgPaused = 1;
	o->dbg.step = 0;
	o->vm.dbgInsnLimit = 0x100000;
}

// ---- opening and closing ----------------------------------------------------------------------------

void Dbg_Init(OpenBGI_t* o)
{
	o->dbg.open = 0;
	o->dbg.overlay = 0;
	o->dbg.dbgLogCount = o->dbg.dbgLogNext = 0;
}

void Dbg_Shutdown(OpenBGI_t* o)
{
	if(o->dbg.open)
		Dbg_Toggle(o);
	DbgListing_FreeAll(o);
}

// --debug: the next Dbg_Pass opens the window (the main window exists by then); --debug=pause holds the threads
void Dbg_RequestOpen(OpenBGI_t* o, int paused)
{
	o->dbg.wantOpen = 1;
	if(paused)
		Pause(o);
}

/* Open the window: the host's debugger window sized DBG_W x DBG_H, or the
 * overlay over the game's picture when the host has none (or
 * BGI_DEBUG_OVERLAY is set); the surface is created to match.  Nothing
 * happens when the surface cannot be made. */
static void Open(OpenBGI_t* o)
{
	if(o->dbg.open)
		return;
	// BGI_DEBUG_OVERLAY=1 forces the overlay, to try that path on a platform with a window
	o->dbg.overlay = (getenv("BGI_DEBUG_OVERLAY") && *getenv("BGI_DEBUG_OVERLAY") != '0') ||
		!MainWin_DebugOpen(o, DBG_W, DBG_H, "OpenBGI debugger");
	if(o->dbg.overlay)
	{ // over the game's picture: the surface is the window's client area
		int w = 0, h = 0;
		Display_GetWindowSize(o, &w, &h);
		if(w < 320 || h < 240)
		{
			w = 800;
			h = 600;
		}
		if(!DbgDraw_Init(o, w, h))
			return;
	}
	else if(!DbgDraw_Init(o, DBG_W, DBG_H))
	{
		MainWin_DebugClose(o);
		return;
	}
	o->dbg.open = 1;
	o->dbg.lastDraw = 0;
	Dbg_Log(o, "debugger opened%s", o->dbg.overlay ? " (over the game's window: no second window on this platform)" : "");
}

// Close the window, drop the queued input and the listings, and resume the VM when it was paused.
static void Close(OpenBGI_t* o)
{
	if(!o->dbg.open)
		return;
	if(!o->dbg.overlay)
		MainWin_DebugClose(o);
	else
		Present_RequestFull(o); // the game's picture back over the overlay, whether or not anything changed
	DbgDraw_Shutdown(o);
	DbgListing_FreeAll(o);
	o->dbg.open = 0;
	o->dbg.eventCount = 0;
	if(o->dbg.dbgPaused)
		Resume(o); // nothing would show the pause any more
}

void Dbg_Toggle(OpenBGI_t* o)
{
	if(o->dbg.open)
		Close(o);
	else
		Open(o);
}

// ---- the window's events (queued) --------------------------------------------------------------------

// a key of the debug window: the virtual key, down / up, and the character it produces (0 none)
void Dbg_OnKey(OpenBGI_t* o, int vk, int down, int ch)
{
	Queue(o, 1, vk, down, ch, 0);
}

// a mouse button (down / up) at (x, y), or a move when button is -1
void Dbg_OnMouse(OpenBGI_t* o, int x, int y, int button, int down)
{
	if(button < 0)
	{ // a move: only the position matters, and only the last one
		o->dbg.mouseX = x;
		o->dbg.mouseY = y;
		return;
	}
	Queue(o, 2, x, y, button, down);
}

void Dbg_OnWheel(OpenBGI_t* o, int delta)
{
	Queue(o, 3, delta, 0, 0, 0);
}

// the window's close box
void Dbg_OnClose(OpenBGI_t* o)
{
	Queue(o, 4, 0, 0, 0, 0);
}

// The game window's keys while the overlay is up: queued as the debugger's; 1 when consumed.
int Dbg_OverlayKey(OpenBGI_t* o, int vk, int down)
{
	int ch = 0;
	if(!o->dbg.open || !o->dbg.overlay)
		return 0;
	// the game window reports virtual keys only: the letters and digits the debugger uses map directly
	if(vk >= '0' && vk <= '9')
		ch = vk;
	else if(vk >= 'A' && vk <= 'Z')
		ch = vk - 'A' + 'a';
	Queue(o, 1, vk, down, ch, 0);
	return 1;
}

int Dbg_OverlayMouse(OpenBGI_t* o, int x, int y, int button, int down)
{
	if(!o->dbg.open || !o->dbg.overlay)
		return 0;
	Dbg_OnMouse(o, x, y, button, down);
	return 1;
}

int Dbg_OverlayWheel(OpenBGI_t* o, int delta)
{
	if(!o->dbg.open || !o->dbg.overlay)
		return 0;
	Dbg_OnWheel(o, delta);
	return 1;
}

// ---- drawing -------------------------------------------------------------------------------------

// the tab bar: one tab per view, numbered, the current one raised; the run state at the right
static void DrawTabs(OpenBGI_t* o)
{
	int i, x = 8;
	DbgDraw_Fill(o, 0, 0, o->dbg.dbgSurf.w, TAB_H, DBG_PANEL);
	DbgDraw_HLine(o, 0, TAB_H - 1, o->dbg.dbgSurf.w, DBG_LINE);
	for(i = 0; i < DBG_VIEW_COUNT; i++)
	{
		int w = DbgDraw_TextWidth(o, kViews[i]->name) + 24;
		if(i == o->dbg.view)
		{
			DbgDraw_Fill(o, x, 2, w, TAB_H - 3, DBG_BG);
			DbgDraw_Frame(o, x, 2, w, TAB_H - 2, DBG_LINE);
		}
		DbgDraw_TextF(o, x + 12, 6, i == o->dbg.view ? DBG_ACCENT : DBG_TEXT, "%d %s", i + 1, kViews[i]->name);
		x += w + 4;
	}
	DbgDraw_TextF(o, o->dbg.dbgSurf.w - 8 - DbgDraw_TextWidth(o, o->dbg.dbgPaused ? "PAUSED" : "running"), 6, o->dbg.dbgPaused ? DBG_RED : DBG_GREEN,
		o->dbg.dbgPaused ? "PAUSED" : "running");
}

// the tab under (x, y), -1 none (the layout of DrawTabs)
static int TabHit(OpenBGI_t* o, int x, int y)
{
	int i, tx = 8;
	if(y >= TAB_H)
		return -1;
	for(i = 0; i < DBG_VIEW_COUNT; i++)
	{
		int w = DbgDraw_TextWidth(o, kViews[i]->name) + 24;
		if(x >= tx && x < tx + w)
			return i;
		tx += w + 4;
	}
	return -1;
}

// the status line: the key help, the pass counter, the engine generation, whether breakpoints exist
static void DrawStatus(OpenBGI_t* o)
{
	int y = o->dbg.dbgSurf.h - STATUS_H;
	DbgDraw_Fill(o, 0, y, o->dbg.dbgSurf.w, STATUS_H, DBG_PANEL);
	DbgDraw_HLine(o, 0, y, o->dbg.dbgSurf.w, DBG_LINE);
	DbgDraw_TextF(o, 8, y + 2, DBG_DIM,
		"F5 run/pause  F6 step pass  F7 step insn  F9 breakpoint  T trace%s  Tab/1-8 views  Left/Right pane  Esc close   pass %u  %s%s",
		o->dbg.dbgTrace ? " [on]" : "", (unsigned)o->dbg.passCount, o->engine.profile ? o->engine.profile->name : "?",
		o->dbg.dbgBreakCount ? "  (breakpoints set)" : "");
}

// the whole surface: tabs, the current view clipped to the area between the bars, the status line
static void Draw(OpenBGI_t* o)
{
	DbgDraw_Clip(o, 0, 0, 0, 0);
	DbgDraw_Fill(o, 0, 0, o->dbg.dbgSurf.w, o->dbg.dbgSurf.h, DBG_BG);
	DrawTabs(o);
	DbgDraw_Clip(o, 0, TAB_H, o->dbg.dbgSurf.w, o->dbg.dbgSurf.h - TAB_H - STATUS_H);
	kViews[o->dbg.view]->draw(o, 0, TAB_H, o->dbg.dbgSurf.w, o->dbg.dbgSurf.h - TAB_H - STATUS_H);
	DbgDraw_Clip(o, 0, 0, 0, 0);
	DrawStatus(o);
}

// the surface to the screen: into the game's window (overlay) or the debug window
static void Present(OpenBGI_t* o)
{
	if(o->dbg.overlay)
	{
		Bmp_t b;
		b.pixels = (uint8_t*)o->dbg.dbgSurf.px;
		b.pitch = o->dbg.dbgSurf.w * 4;
		b.w = o->dbg.dbgSurf.w;
		b.h = o->dbg.dbgSurf.h;
		b.mode = PM_RGB32;
		b.bpp = 4;
		Screen_Copy(o, 0, 0, &b, 0, 0, b.w, b.h);
	}
	else
	{
		MainWin_DebugPresent(o, (const uint8_t*)o->dbg.dbgSurf.px, o->dbg.dbgSurf.w * 4, o->dbg.dbgSurf.w, o->dbg.dbgSurf.h);
	}
}

// After the engine presented its frame: the overlay goes over it (the debug window presents from Dbg_Pass).
void Dbg_OverlayPresent(OpenBGI_t* o)
{
	if(o->dbg.open && o->dbg.overlay)
		Present(o);
}

// ---- the events, applied between passes ------------------------------------------------------------

/* a key press: the global keys (close, run control, the view tabs, the
 * trace) here, everything else to the current view */
static void HandleKey(OpenBGI_t* o, int vk, int down, int ch)
{
	if(!down)
		return;
	switch(vk)
	{
		case 0x1b: // Escape
		case 0x7b: // F12
			Close(o);
			return;
		case 0x74: // F5
			if(o->dbg.dbgPaused)
				Resume(o);
			else
				Pause(o);
			Dbg_Log(o, o->dbg.dbgPaused ? "paused" : "running");
			return;
		case 0x75: // F6: a pass
			o->dbg.dbgPaused = 1;
			o->dbg.step = 1;
			return;
		case 0x76: // F7: an instruction
			o->dbg.dbgPaused = 1;
			o->dbg.step = 2;
			return;
		case 0x09: // Tab
			o->dbg.view = (o->dbg.view + 1) % DBG_VIEW_COUNT;
			return;
		default:
			break;
	}
	if(ch >= '1' && ch < '1' + DBG_VIEW_COUNT)
	{
		o->dbg.view = ch - '1';
		return;
	}
	if(ch == 't' || ch == 'T')
	{
		o->dbg.dbgTrace = !o->dbg.dbgTrace;
		Rehook(o);
		Dbg_Log(o, "trace %s", o->dbg.dbgTrace ? "on" : "off");
		return;
	}
	kViews[o->dbg.view]->key(o, vk, ch);
}

// drain the queue: keys to HandleKey, a button press to the tabs or the view, the wheel to the view at the last mouse position
static void HandleEvents(OpenBGI_t* o)
{
	while(o->dbg.eventCount)
	{
		DbgEvent_t e = o->dbg.events[o->dbg.eventHead];
		o->dbg.eventHead = (o->dbg.eventHead + 1) % DBG_EVENT_QUEUE;
		o->dbg.eventCount--;
		switch(e.type)
		{
			case 1: HandleKey(o, e.a, e.b, e.c); break;
			case 2:
				if(e.d)
				{
					int tab = TabHit(o, e.a, e.b);
					if(tab >= 0)
						o->dbg.view = tab;
					else
						kViews[o->dbg.view]->click(o, e.a, e.b, e.c);
				}
				break;
			case 3: kViews[o->dbg.view]->wheel(o, o->dbg.mouseX, o->dbg.mouseY, e.a); break;
			case 4: Close(o); break;
		}
		if(!o->dbg.open)
			return;
	}
}

/* Once per pass of the main loop, after the threads: end a step, open the
 * window when --debug asked for it, close it when the OS window went
 * away, apply the queued input, and redraw at most every 40 ms while the
 * VM runs (16 ms while paused, when nothing but the debugger changes). */
void Dbg_Pass(OpenBGI_t* o)
{
	uint32_t now;
	o->dbg.passCount++;
	if(o->dbg.step)
	{ // the step ran: paused again
		o->dbg.step = 0;
		o->vm.dbgInsnLimit = 0x100000;
	}
	if(o->dbg.wantOpen)
	{
		o->dbg.wantOpen = 0;
		Open(o);
	}
	if(!o->dbg.open)
		return;
	HandleEvents(o);
	if(!o->dbg.open)
		return;
	now = Clock_HostMs(o);
	if(now - o->dbg.lastDraw < 40 && !o->dbg.dbgPaused)
		return;
	if(now - o->dbg.lastDraw < 16)
		return;
	o->dbg.lastDraw = now;
	Draw(o);
	if(!o->dbg.overlay)
		Present(o);
}
