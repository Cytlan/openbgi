/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dbg.h - the debugger: a window into the running engine (src/dbg/)
 *
 * Opened with F12 in the game's window or --debug on the command line,
 * the debugger shows, live, what the engine is doing: the script threads
 * with their stacks, frames and the disassembly around their instruction
 * pointers; the loaded programs as listings (the assembler's, with the
 * current position marked, and breakpoints set by clicking a line); the
 * bitmap slots with their pictures; the display objects; the sound
 * channels; the memory areas as hex dumps; a log of what happened.  The
 * VM can be paused and stepped by passes or by instructions.
 *
 * It draws into a 32-bit surface of its own with the engine's glyph
 * rasteriser (through the OS layer's mono font), presented in a second
 * window where the OS layer has one (OS_DebugWindowOpen) and over the
 * game's picture where it has not.  Nothing of the engine's behaviour
 * changes while it is closed: the hooks below are a flag test each.
 *
 * None of this exists in the original.
 */
#ifndef BGI_DBG_H_
#define BGI_DBG_H_

#include "bgi/common.h"

struct Thread;

#if defined(BGI_WASM)
/* The WebAssembly build has no debugger: the hooks are empty functions the
 * compiler removes, and the scheduler's flag and limit are constants. */
#define gDbgHooked    0
#define gDbgInsnLimit 0x100000u
static inline void Dbg_Init(void)
{
}
static inline void Dbg_Shutdown(void)
{
}
static inline void Dbg_RequestOpen(int paused)
{
	(void)paused;
}
static inline void Dbg_Toggle(void)
{
}
static inline int Dbg_IsOpen(void)
{
	return 0;
}
static inline void Dbg_Pass(void)
{
}
static inline int Dbg_GateRun(void)
{
	return 1;
}
static inline uint32_t Dbg_InsnLimit(void)
{
	return gDbgInsnLimit;
}
static inline int Dbg_OnInsn(struct Thread* t, uint32_t opIP, int op)
{
	(void)t;
	(void)opIP;
	(void)op;
	return 0;
}
static inline int Dbg_OverlayKey(int vk, int down)
{
	(void)vk;
	(void)down;
	return 0;
}
static inline int Dbg_OverlayMouse(int x, int y, int button, int down)
{
	(void)x;
	(void)y;
	(void)button;
	(void)down;
	return 0;
}
static inline int Dbg_OverlayWheel(int delta)
{
	(void)delta;
	return 0;
}
static inline void Dbg_Log(const char* fmt, ...)
{
	(void)fmt;
}
static inline void Dbg_OverlayPresent(void)
{
}
#define DBG_EVENT_HANDLERS NULL, NULL, NULL, NULL // the OsEventHandlers_t.debug_* entries
#else

void Dbg_Init(void);              // once, at engine start
void Dbg_Shutdown(void);          // at engine end (closes the window)
void Dbg_RequestOpen(int paused); // --debug: open as soon as the main window exists (paused: hold the threads)
void Dbg_Toggle(void);            // F12: open / close
int Dbg_IsOpen(void);

/* The scheduler's hooks (src/vm/sched.c).  Dbg_Pass runs once per pass of
 * the main loop, after the threads: it redraws and presents the window at
 * its own rate and handles the queued input.  Dbg_GateRun says whether the
 * threads may run this pass (0 while paused, unless a step was asked
 * for).  Dbg_InsnLimit is the number of instructions a turn may execute
 * (the usual 0x100000, or 1 while stepping by instruction).  Dbg_OnInsn is
 * called before every instruction while breakpoints exist or the debugger
 * traces; it returns 1 when the turn must end at once (a breakpoint). */
void Dbg_Pass(void);
int Dbg_GateRun(void);
uint32_t Dbg_InsnLimit(void);
int Dbg_OnInsn(struct Thread* t, uint32_t opIP, int op);
extern int gDbgHooked;         // 1 while Dbg_OnInsn must be called (breakpoints or tracing)
extern uint32_t gDbgInsnLimit; // what Dbg_InsnLimit returns, for the scheduler's loop condition

/* the window's events (OsEventHandlers_t.debug_*), queued and applied in
 * the next Dbg_Pass: a key (virtual key code, down / up, the character it
 * produces or 0), a mouse button (down / up at x, y; button -1 is a move),
 * the wheel, the close box */
void Dbg_OnKey(int vk, int down, int ch);
void Dbg_OnMouse(int x, int y, int button, int down);
void Dbg_OnWheel(int delta);
void Dbg_OnClose(void);
// the game window's input while the debugger draws over it (no second window): 1 when consumed
int Dbg_OverlayKey(int vk, int down);
int Dbg_OverlayMouse(int x, int y, int button, int down); // button -1: a move
int Dbg_OverlayWheel(int delta);
// a printf-style line for the debugger's log (script errors, message boxes, loads ...); kept while it is closed
void Dbg_Log(const char* fmt, ...);
/* the overlay: after the engine presented its frame, draw the debugger
 * over it (into the game's window) when it has no window of its own */
void Dbg_OverlayPresent(void);
#define DBG_EVENT_HANDLERS Dbg_OnKey, Dbg_OnMouse, Dbg_OnWheel, Dbg_OnClose // the OsEventHandlers_t.debug_* entries

#endif // BGI_WASM

#endif // BGI_DBG_H_
