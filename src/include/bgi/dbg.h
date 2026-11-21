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
 * rasteriser (the monochrome typeface), presented in a second window
 * where the host has one (OpenBGI_DebugWindow_t) and over the game's
 * picture where it has not.  Nothing of the engine's behaviour
 * changes while it is closed: the hooks below are a flag test each.
 *
 * None of this exists in the original.
 */
#ifndef BGI_DBG_H_
#define BGI_DBG_H_

#include "bgi/common.h"

struct Thread;

#if defined(BGI_NO_DEBUGGER)
/* A build without the debugger (BGI_NO_DEBUGGER: the WebAssembly one,
 * which leaves out src/dbg/): the hooks are empty functions the compiler
 * removes, and the scheduler's flag and limit keep their starting values
 * (o->vm.dbgHooked 0, o->vm.dbgInsnLimit 0x100000). */
static inline void Dbg_Init(OpenBGI_t* o)
{
	(void)o;
}
static inline void Dbg_Shutdown(OpenBGI_t* o)
{
	(void)o;
}
static inline void Dbg_RequestOpen(OpenBGI_t* o, int paused)
{
	(void)o;
	(void)paused;
}
static inline void Dbg_Toggle(OpenBGI_t* o)
{
	(void)o;
}
static inline void Dbg_Pass(OpenBGI_t* o)
{
	(void)o;
}
static inline int Dbg_GateRun(OpenBGI_t* o)
{
	(void)o;
	return 1;
}
static inline int Dbg_OnInsn(OpenBGI_t* o, struct Thread* t, uint32_t opIP, int op)
{
	(void)o;
	(void)t;
	(void)opIP;
	(void)op;
	return 0;
}
static inline int Dbg_OverlayKey(OpenBGI_t* o, int vk, int down)
{
	(void)o;
	(void)vk;
	(void)down;
	return 0;
}
static inline int Dbg_OverlayMouse(OpenBGI_t* o, int x, int y, int button, int down)
{
	(void)o;
	(void)x;
	(void)y;
	(void)button;
	(void)down;
	return 0;
}
static inline int Dbg_OverlayWheel(OpenBGI_t* o, int delta)
{
	(void)o;
	(void)delta;
	return 0;
}
static inline void Dbg_Log(OpenBGI_t* o, const char* fmt, ...)
{
	(void)o;
	(void)fmt;
}
static inline void Dbg_OverlayPresent(OpenBGI_t* o)
{
	(void)o;
}
static inline void Dbg_OnKey(OpenBGI_t* o, int vk, int down, int ch)
{
	(void)o;
	(void)vk;
	(void)down;
	(void)ch;
}
static inline void Dbg_OnMouse(OpenBGI_t* o, int x, int y, int button, int down)
{
	(void)o;
	(void)x;
	(void)y;
	(void)button;
	(void)down;
}
static inline void Dbg_OnWheel(OpenBGI_t* o, int delta)
{
	(void)o;
	(void)delta;
}
static inline void Dbg_OnClose(OpenBGI_t* o)
{
	(void)o;
}
#else

void Dbg_Init(OpenBGI_t* o);                    // once, at engine start
void Dbg_Shutdown(OpenBGI_t* o);                // at engine end (closes the window)
void Dbg_RequestOpen(OpenBGI_t* o, int paused); // --debug: open as soon as the main window exists (paused: hold the threads)
void Dbg_Toggle(OpenBGI_t* o);                  // F12: open / close

/* The scheduler's hooks (src/vm/sched.c).  Dbg_Pass runs once per pass of
 * the main loop, after the threads: it redraws and presents the window at
 * its own rate and handles the queued input.  Dbg_GateRun says whether the
 * threads may run this pass (0 while paused, unless a step was asked
 * for).  The number of instructions a turn may execute is
 * o->vm.dbgInsnLimit (the usual 0x100000, or 1 while stepping by
 * instruction).  Dbg_OnInsn is called before every instruction while
 * breakpoints exist or the debugger traces; it returns 1 when the turn
 * must end at once (a breakpoint). */
void Dbg_Pass(OpenBGI_t* o);
int Dbg_GateRun(OpenBGI_t* o);
int Dbg_OnInsn(OpenBGI_t* o, struct Thread* t, uint32_t opIP, int op);

/* the debugger's window's events (OpenBGI_DebugKey and its kin,
 * openbgi.h), queued and applied in the next Dbg_Pass: a key (virtual
 * key code, down / up, the character it produces or 0), a mouse button
 * (down / up at x, y; button -1 is a move), the wheel, the close box */
void Dbg_OnKey(OpenBGI_t* o, int vk, int down, int ch);
void Dbg_OnMouse(OpenBGI_t* o, int x, int y, int button, int down);
void Dbg_OnWheel(OpenBGI_t* o, int delta);
void Dbg_OnClose(OpenBGI_t* o);
// the game window's input while the debugger draws over it (no second window): 1 when consumed
int Dbg_OverlayKey(OpenBGI_t* o, int vk, int down);
int Dbg_OverlayMouse(OpenBGI_t* o, int x, int y, int button, int down); // button -1: a move
int Dbg_OverlayWheel(OpenBGI_t* o, int delta);
// a printf-style line for the debugger's log (script errors, message boxes, loads ...); kept while it is closed
void Dbg_Log(OpenBGI_t* o, const char* fmt, ...);
/* the overlay: after the engine presented its frame, draw the debugger
 * over it (into the game's window) when it has no window of its own */
void Dbg_OverlayPresent(OpenBGI_t* o);

/* ---- the types of the debugger's state (o->dbg, bgi/core.h) ---------------------------- */

#define DBG_CELL_H       16  // the text cell's height in pixels (the width: DBG_CELL_W, dbg_internal.h)
#define DBG_CELL_PITCH   4   // bytes per glyph row in the glyph caches: up to 32 pixels wide
#define DBG_WIDE_CACHE   256 // two-byte characters cached; the oldest is replaced
#define DBG_LOG_LINES    400 // the log's ring of lines
#define DBG_MAX_BREAKS   64  // breakpoints at once
#define DBG_MAX_LISTINGS 16  // listings cached at once; the oldest is replaced
#define DBG_EVENT_QUEUE  128 // events kept; further ones are dropped until the next pass

typedef struct DbgSurf // the debugger's picture
{
	uint32_t* px;                   // 0x00RRGGBB rows, w * h pixels
	int w, h;                       // size in pixels
	int clipX, clipY, clipW, clipH; // the clip rectangle every primitive honours
} DbgSurf_t;

// a glyph of the two-byte characters' cache
typedef struct DbgWideGlyph
{
	uint16_t code;                             // the Shift-JIS code (lead byte first)
	uint8_t bits[DBG_CELL_H * DBG_CELL_PITCH]; // two cells wide
	int used;                                  // the slot holds a glyph
} DbgWideGlyph_t;

typedef struct DbgList // a scrollable list of rows of DBG_CELL_H pixels with one selected
{
	int count;      // rows
	int sel;        // the selected row, -1 none
	int top;        // the first row shown
	int rows;       // rows that fit
	int x, y, w, h; // the pane, for the mouse
} DbgList_t;

// a line of the log
typedef struct DbgLogLine
{
	char text[160]; // one line, truncated to fit
} DbgLogLine_t;

// a breakpoint: a module name and an offset inside it
typedef struct DbgBreak
{
	char module[0x40]; // the module's name as the thread loaded it
	uint32_t off;      // the instruction's offset in the module
} DbgBreak_t;

// the listing of a module: lines of a Dis_Program listing with their offsets
typedef struct DbgListing
{
	char key[0x60]; // "module name:base:size" of the thread it was made for
	char* text;     // the whole listing
	char** lines;   // pointers into `text` (NUL-terminated)
	uint32_t* offs; // the offset of each line (the next instruction's for a label line); ~0u none
	int count;      // lines
	int problems;   // the disassembler noted flow problems (shown in the pane's title)
} DbgListing_t;

// the listing pane: a module's disassembly with the current line and the breakpoints marked
typedef struct DbgListingPane
{
	DbgList_t list;              // the lines
	const char* module;          // what it shows (for the breakpoints)
	uint32_t base, size;         // the module's place in the thread's code area
	const DbgListing_t* listing; // the cached listing, NULL when none could be made
	int follow;                  // keep the current instruction in view (off once the user scrolls; F restores it)
} DbgListingPane_t;

// queued input: the events come from the pump, between passes
typedef struct DbgEvent
{
	int type;       // 1 key, 2 mouse, 3 wheel, 4 close
	int a, b, c, d; // key: vk, down, ch; mouse: x, y, button, down; wheel: delta
} DbgEvent_t;

#endif // BGI_NO_DEBUGGER

#endif // BGI_DBG_H_
