/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dbg.c - the debugger's window, run control and the views' plumbing
 * (inc/bgi/dbg.h for the engine, dbg_internal.h for the views)
 *
 * The window is a surface of DBG_W x DBG_H pixels presented through the
 * OS layer's debug window, or over the game's picture when the back end
 * has none ("overlay").  Each pass of the main loop calls Dbg_Pass, which
 * redraws at most every 40 ms; the input arrives through the OS event
 * handlers between passes and is queued, so that the views see a
 * consistent engine state while they draw and act.
 *
 * Keys in the debugger: F5 run / pause, F6 step a pass, F7 step one
 * instruction (per runnable thread), F9 breakpoint on the selected
 * listing line, T trace into the log, 1 .. 7 the views, Tab the next
 * view, Escape or F12 close.  The game's window keeps its keys; F12 there
 * toggles the debugger.  docs/debugger.md describes the tool.
 */
#include "dbg_internal.h"
#include "bgi/display.h"
#include "bgi/vm.h"
#include "bgi/version.h"

#include <stdarg.h>

#define DBG_W    1152 // the window's size in pixels (the overlay takes the game window's)
#define DBG_H    768
#define TAB_H    28 // the tab bar at the top
#define STATUS_H 20 // the status line at the bottom

// ---- shared state ------------------------------------------------------------------------

DbgLogLine_t gDbgLog[DBG_LOG_LINES];
int gDbgLogCount, gDbgLogNext;
uint32_t gDbgThreadId = 1;
int gDbgPaused;
int gDbgTrace;
int gDbgHooked;
uint32_t gDbgInsnLimit = 0x100000;
DbgBreak_t gDbgBreaks[DBG_MAX_BREAKS];
int gDbgBreakCount;

static int gOpen, gOverlay, gWantOpen;       // the window is open / draws over the game / --debug asked for it
static int gView = DBG_VIEW_THREADS;         // the current tab
static int gStep;                            // 0 none, 1 a pass, 2 an instruction
static uint32_t gLastDraw;                   // OS_TicksMs of the last redraw
static int gMouseX = -1, gMouseY = -1;       // the last mouse position seen (for the wheel)
static uint32_t gPassCount;                  // passes of the main loop (the status line)
static uint32_t gSkipThread = ~0u, gSkipOff; // the breakpoint to pass over once (the one just hit)

static const DbgViewOps_t* const kViews[DBG_VIEW_COUNT] = {
	&kDbgViewThreads, &kDbgViewPrograms, &kDbgViewBitmaps, &kDbgViewObjects, &kDbgViewSound, &kDbgViewMemory, &kDbgViewLog};

// queued input: the events come from the pump, between passes
typedef struct DbgEvent
{
	int type;       // 1 key, 2 mouse, 3 wheel, 4 close
	int a, b, c, d; // key: vk, down, ch; mouse: x, y, button, down; wheel: delta
} DbgEvent_t;
#define EVENT_QUEUE 128 // events kept; further ones are dropped until the next pass
static DbgEvent_t gEvents[EVENT_QUEUE];
static int gEventHead, gEventCount;

// append an event to the queue (dropped when the queue is full)
static void Queue(int type, int a, int b, int c, int d)
{
	DbgEvent_t* e;
	if(gEventCount == EVENT_QUEUE)
		return;
	e = &gEvents[(gEventHead + gEventCount) % EVENT_QUEUE];
	e->type = type;
	e->a = a;
	e->b = b;
	e->c = c;
	e->d = d;
	gEventCount++;
}

// ---- the log ------------------------------------------------------------------------------

// Format a line into the log ring (DBG_LOG_LINES kept, the oldest overwritten); usable while the debugger is closed.
void Dbg_Log(const char* fmt, ...)
{
	va_list ap;
	DbgLogLine_t* l = &gDbgLog[gDbgLogNext];
	va_start(ap, fmt);
	vsnprintf(l->text, sizeof l->text, fmt, ap);
	va_end(ap);
	gDbgLogNext = (gDbgLogNext + 1) % DBG_LOG_LINES;
	if(gDbgLogCount < DBG_LOG_LINES)
		gDbgLogCount++;
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

/* The thread with id gDbgThreadId; when it is gone, the first thread of
 * the scheduler becomes the selected one.  NULL before the VM exists. */
Thread_t* Dbg_SelectedThread(void)
{
	Thread_t* t;
	if(!gRootThread)
		return NULL;
	for(t = Thread_Next(gRootThread); t; t = Thread_Next(t))
		if(t->id == gDbgThreadId)
			return t;
	t = Thread_Next(gRootThread);
	if(t)
		gDbgThreadId = t->id;
	return t;
}

// the scheduler calls Dbg_OnInsn only while there is something for it to do
static void Rehook(void)
{
	gDbgHooked = gDbgBreakCount > 0 || gDbgTrace;
}

// Is there a breakpoint at offset `off` of module `module`?
int Dbg_HasBreak(const char* module, uint32_t off)
{
	int i;
	for(i = 0; i < gDbgBreakCount; i++)
		if(gDbgBreaks[i].off == off && strcmp(gDbgBreaks[i].module, module) == 0)
			return 1;
	return 0;
}

/* Clear the breakpoint at (module, off) when there is one, else set it:
 * 1 set, 0 cleared (or not set because DBG_MAX_BREAKS exist already).
 * Either way the log gets a line. */
int Dbg_ToggleBreak(const char* module, uint32_t off)
{
	int i;
	for(i = 0; i < gDbgBreakCount; i++)
	{
		if(gDbgBreaks[i].off == off && strcmp(gDbgBreaks[i].module, module) == 0)
		{
			gDbgBreaks[i] = gDbgBreaks[--gDbgBreakCount];
			Dbg_Log("breakpoint cleared: %s+0x%x", module, (unsigned)off);
			Rehook();
			return 0;
		}
	}
	if(gDbgBreakCount == DBG_MAX_BREAKS)
		return 0;
	snprintf(gDbgBreaks[gDbgBreakCount].module, sizeof gDbgBreaks[0].module, "%s", module);
	gDbgBreaks[gDbgBreakCount].off = off;
	gDbgBreakCount++;
	Dbg_Log("breakpoint set: %s+0x%x", module, (unsigned)off);
	Rehook();
	return 1;
}

// ---- the scheduler's hooks -----------------------------------------------------------------------

/* May the threads run this pass?  Yes while running; while paused only
 * when a step was asked for, and then with the instruction limit the
 * step needs (1 per turn for a step by instruction). */
int Dbg_GateRun(void)
{
	if(!gDbgPaused)
		return 1;
	if(gStep)
	{
		gDbgInsnLimit = gStep == 2 ? 1 : 0x100000;
		return 1;
	}
	return 0;
}

uint32_t Dbg_InsnLimit(void)
{
	return gDbgInsnLimit;
}

/* Before an instruction of thread t at code offset opIP (op is its first
 * byte): log it when tracing; at a breakpoint pause the VM, select the
 * thread, open the window when it is closed and return 1 so that the turn
 * ends before the instruction.  The breakpoint the pause was resumed from
 * is passed once by the same thread. */
int Dbg_OnInsn(Thread_t* t, uint32_t opIP, int op)
{
	const char* module;
	uint32_t base, size;
	if(gDbgTrace)
	{
		if(Dbg_ModuleAt(t, opIP, &module, &base, &size))
			Dbg_Log("t%u %s+%04x: %02x %02x", (unsigned)t->id, module, (unsigned)(opIP - base), op,
				opIP + 1 < t->codeSize ? t->code[opIP + 1] : 0);
	}
	if(gDbgBreakCount && Dbg_ModuleAt(t, opIP, &module, &base, &size) && Dbg_HasBreak(module, opIP - base))
	{
		if(gSkipThread == t->id && gSkipOff == opIP)
		{ // resumed from this very breakpoint: it is passed once
			gSkipThread = ~0u;
			return 0;
		}
		gDbgPaused = 1;
		gStep = 0;
		gDbgThreadId = t->id;
		gSkipThread = t->id;
		gSkipOff = opIP;
		Dbg_Log("breakpoint: thread %u at %s+0x%x", (unsigned)t->id, module, (unsigned)(opIP - base));
		if(!gOpen)
			Dbg_Toggle();
		return 1;
	}
	return 0;
}

// let the threads run again, any pending step forgotten
static void Resume(void)
{
	gDbgPaused = 0;
	gStep = 0;
	gDbgInsnLimit = 0x100000;
}

// hold the threads from the next pass on
static void Pause(void)
{
	gDbgPaused = 1;
	gStep = 0;
	gDbgInsnLimit = 0x100000;
}

// ---- opening and closing ----------------------------------------------------------------------------

void Dbg_Init(void)
{
	gOpen = 0;
	gOverlay = 0;
	gDbgLogCount = gDbgLogNext = 0;
}

void Dbg_Shutdown(void)
{
	if(gOpen)
		Dbg_Toggle();
	DbgListing_FreeAll();
}

// --debug: the next Dbg_Pass opens the window (the main window exists by then); --debug=pause holds the threads
void Dbg_RequestOpen(int paused)
{
	gWantOpen = 1;
	if(paused)
		Pause();
}

int Dbg_IsOpen(void)
{
	return gOpen;
}

/* Open the window: a debug window of the OS layer sized DBG_W x DBG_H, or
 * the overlay over the game's picture when the OS layer has none (or
 * BGI_DEBUG_OVERLAY is set); the surface is created to match.  Nothing
 * happens when the surface cannot be made. */
static void Open(void)
{
	if(gOpen)
		return;
	// BGI_DEBUG_OVERLAY=1 forces the overlay, to try that path on a platform with a window
	gOverlay = (getenv("BGI_DEBUG_OVERLAY") && *getenv("BGI_DEBUG_OVERLAY") != '0') ||
		!OS_DebugWindowOpen(DBG_W, DBG_H, "OpenBGI debugger");
	if(gOverlay)
	{ // over the game's picture: the surface is the window's client area
		int w = 0, h = 0;
		Display_GetWindowSize(&w, &h);
		if(w < 320 || h < 240)
		{
			w = 800;
			h = 600;
		}
		if(!DbgDraw_Init(w, h))
			return;
	}
	else if(!DbgDraw_Init(DBG_W, DBG_H))
	{
		OS_DebugWindowClose();
		return;
	}
	gOpen = 1;
	gLastDraw = 0;
	Dbg_Log("debugger opened%s", gOverlay ? " (over the game's window: no second window on this platform)" : "");
}

// Close the window, drop the queued input and the listings, and resume the VM when it was paused.
static void Close(void)
{
	if(!gOpen)
		return;
	if(!gOverlay)
		OS_DebugWindowClose();
	else
		Present_RequestFull(); // the game's picture back over the overlay, whether or not anything changed
	DbgDraw_Shutdown();
	DbgListing_FreeAll();
	gOpen = 0;
	gEventCount = 0;
	if(gDbgPaused)
		Resume(); // nothing would show the pause any more
}

void Dbg_Toggle(void)
{
	if(gOpen)
		Close();
	else
		Open();
}

// ---- the window's events (queued) --------------------------------------------------------------------

// a key of the debug window: the virtual key, down / up, and the character it produces (0 none)
void Dbg_OnKey(int vk, int down, int ch)
{
	Queue(1, vk, down, ch, 0);
}

// a mouse button (down / up) at (x, y), or a move when button is -1
void Dbg_OnMouse(int x, int y, int button, int down)
{
	if(button < 0)
	{ // a move: only the position matters, and only the last one
		gMouseX = x;
		gMouseY = y;
		return;
	}
	Queue(2, x, y, button, down);
}

void Dbg_OnWheel(int delta)
{
	Queue(3, delta, 0, 0, 0);
}

// the window's close box
void Dbg_OnClose(void)
{
	Queue(4, 0, 0, 0, 0);
}

// The game window's keys while the overlay is up: queued as the debugger's; 1 when consumed.
int Dbg_OverlayKey(int vk, int down)
{
	int ch = 0;
	if(!gOpen || !gOverlay)
		return 0;
	// the game window reports virtual keys only: the letters and digits the debugger uses map directly
	if(vk >= '0' && vk <= '9')
		ch = vk;
	else if(vk >= 'A' && vk <= 'Z')
		ch = vk - 'A' + 'a';
	Queue(1, vk, down, ch, 0);
	return 1;
}

int Dbg_OverlayMouse(int x, int y, int button, int down)
{
	if(!gOpen || !gOverlay)
		return 0;
	Dbg_OnMouse(x, y, button, down);
	return 1;
}

int Dbg_OverlayWheel(int delta)
{
	if(!gOpen || !gOverlay)
		return 0;
	Dbg_OnWheel(delta);
	return 1;
}

// ---- drawing -------------------------------------------------------------------------------------

// the tab bar: one tab per view, numbered, the current one raised; the run state at the right
static void DrawTabs(void)
{
	int i, x = 8;
	DbgDraw_Fill(0, 0, gDbgSurf.w, TAB_H, DBG_PANEL);
	DbgDraw_HLine(0, TAB_H - 1, gDbgSurf.w, DBG_LINE);
	for(i = 0; i < DBG_VIEW_COUNT; i++)
	{
		int w = DbgDraw_TextWidth(kViews[i]->name) + 24;
		if(i == gView)
		{
			DbgDraw_Fill(x, 2, w, TAB_H - 3, DBG_BG);
			DbgDraw_Frame(x, 2, w, TAB_H - 2, DBG_LINE);
		}
		DbgDraw_TextF(x + 12, 6, i == gView ? DBG_ACCENT : DBG_TEXT, "%d %s", i + 1, kViews[i]->name);
		x += w + 4;
	}
	DbgDraw_TextF(gDbgSurf.w - 8 - DbgDraw_TextWidth(gDbgPaused ? "PAUSED" : "running"), 6, gDbgPaused ? DBG_RED : DBG_GREEN,
		gDbgPaused ? "PAUSED" : "running");
}

// the tab under (x, y), -1 none (the layout of DrawTabs)
static int TabHit(int x, int y)
{
	int i, tx = 8;
	if(y >= TAB_H)
		return -1;
	for(i = 0; i < DBG_VIEW_COUNT; i++)
	{
		int w = DbgDraw_TextWidth(kViews[i]->name) + 24;
		if(x >= tx && x < tx + w)
			return i;
		tx += w + 4;
	}
	return -1;
}

// the status line: the key help, the pass counter, the engine generation, whether breakpoints exist
static void DrawStatus(void)
{
	int y = gDbgSurf.h - STATUS_H;
	DbgDraw_Fill(0, y, gDbgSurf.w, STATUS_H, DBG_PANEL);
	DbgDraw_HLine(0, y, gDbgSurf.w, DBG_LINE);
	DbgDraw_TextF(8, y + 2, DBG_DIM,
		"F5 run/pause  F6 step pass  F7 step insn  F9 breakpoint  T trace%s  Tab/1-7 views  Left/Right pane  Esc close   pass %u  %s%s",
		gDbgTrace ? " [on]" : "", (unsigned)gPassCount, gEngine ? gEngine->name : "?",
		gDbgBreakCount ? "  (breakpoints set)" : "");
}

// the whole surface: tabs, the current view clipped to the area between the bars, the status line
static void Draw(void)
{
	DbgDraw_Clip(0, 0, 0, 0);
	DbgDraw_Fill(0, 0, gDbgSurf.w, gDbgSurf.h, DBG_BG);
	DrawTabs();
	DbgDraw_Clip(0, TAB_H, gDbgSurf.w, gDbgSurf.h - TAB_H - STATUS_H);
	kViews[gView]->draw(0, TAB_H, gDbgSurf.w, gDbgSurf.h - TAB_H - STATUS_H);
	DbgDraw_Clip(0, 0, 0, 0);
	DrawStatus();
}

// the surface to the screen: into the game's window (overlay) or the debug window
static void Present(void)
{
	OsSurface_t s;
	s.pixels = gDbgSurf.px;
	s.pitch = gDbgSurf.w * 4;
	s.width = gDbgSurf.w;
	s.height = gDbgSurf.h;
	s.bpp = 32;
	if(gOverlay)
	{
		OsDc_t* dc = OS_WindowGetDc();
		if(dc)
		{
			OS_BlitToWindow(dc, 0, 0, s.width, s.height, &s, 0, 0);
			OS_WindowReleaseDc(dc);
		}
	}
	else
		OS_DebugWindowPresent(&s);
}

// After the engine presented its frame: the overlay goes over it (the debug window presents from Dbg_Pass).
void Dbg_OverlayPresent(void)
{
	if(gOpen && gOverlay)
		Present();
}

// ---- the events, applied between passes ------------------------------------------------------------

/* a key press: the global keys (close, run control, the view tabs, the
 * trace) here, everything else to the current view */
static void HandleKey(int vk, int down, int ch)
{
	if(!down)
		return;
	switch(vk)
	{
		case 0x1b: // Escape
		case 0x7b: // F12
			Close();
			return;
		case 0x74: // F5
			if(gDbgPaused)
				Resume();
			else
				Pause();
			Dbg_Log(gDbgPaused ? "paused" : "running");
			return;
		case 0x75: // F6: a pass
			gDbgPaused = 1;
			gStep = 1;
			return;
		case 0x76: // F7: an instruction
			gDbgPaused = 1;
			gStep = 2;
			return;
		case 0x09: // Tab
			gView = (gView + 1) % DBG_VIEW_COUNT;
			return;
		default:
			break;
	}
	if(ch >= '1' && ch < '1' + DBG_VIEW_COUNT)
	{
		gView = ch - '1';
		return;
	}
	if(ch == 't' || ch == 'T')
	{
		gDbgTrace = !gDbgTrace;
		Rehook();
		Dbg_Log("trace %s", gDbgTrace ? "on" : "off");
		return;
	}
	kViews[gView]->key(vk, ch);
}

// drain the queue: keys to HandleKey, a button press to the tabs or the view, the wheel to the view at the last mouse position
static void HandleEvents(void)
{
	while(gEventCount)
	{
		DbgEvent_t e = gEvents[gEventHead];
		gEventHead = (gEventHead + 1) % EVENT_QUEUE;
		gEventCount--;
		switch(e.type)
		{
			case 1: HandleKey(e.a, e.b, e.c); break;
			case 2:
				if(e.d)
				{
					int tab = TabHit(e.a, e.b);
					if(tab >= 0)
						gView = tab;
					else
						kViews[gView]->click(e.a, e.b, e.c);
				}
				break;
			case 3: kViews[gView]->wheel(gMouseX, gMouseY, e.a); break;
			case 4: Close(); break;
		}
		if(!gOpen)
			return;
	}
}

/* Once per pass of the main loop, after the threads: end a step, open the
 * window when --debug asked for it, close it when the OS window went
 * away, apply the queued input, and redraw at most every 40 ms while the
 * VM runs (16 ms while paused, when nothing but the debugger changes). */
void Dbg_Pass(void)
{
	uint32_t now;
	gPassCount++;
	if(gStep)
	{ // the step ran: paused again
		gStep = 0;
		gDbgInsnLimit = 0x100000;
	}
	if(gWantOpen)
	{
		gWantOpen = 0;
		Open();
	}
	if(!gOpen)
		return;
	if(!gOverlay && !OS_DebugWindowAlive())
	{
		Close();
		return;
	}
	HandleEvents();
	if(!gOpen)
		return;
	now = OS_TicksMs();
	if(now - gLastDraw < 40 && !gDbgPaused)
		return;
	if(now - gLastDraw < 16)
		return;
	gLastDraw = now;
	Draw();
	if(!gOverlay)
		Present();
}
