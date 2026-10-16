/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dbg_internal.h - what the debugger's files share (src/dbg/)
 *
 * dbg.c owns the window, the run control, the log and the breakpoints;
 * dbgdraw.c the surface and the drawing primitives; dbg_listing.c the
 * cached disassembly of the modules; dbg_pane.c the pieces the views
 * build on (panes, lists, the listing pane); the files under views/ the
 * seven views, each exported as a DbgViewOps_t.
 */
#ifndef BGI_DBG_INTERNAL_H_
#define BGI_DBG_INTERNAL_H_

#include "bgi/dbg.h"
#include "bgi/os.h"
#include "bgi/gfx/bitmap.h"

// ---- the surface and the drawing primitives (dbgdraw.c) ----------------------------------

/* the text cell: 16 pixels high; 8 wide with a half-width font (MS Gothic,
 * the fixed-pitch Japanese fonts), else the font's own advance (gDbgCellW,
 * measured when the font opens) */
#define DBG_CELL_W gDbgCellW
#define DBG_CELL_H 16
extern int gDbgCellW;

typedef struct DbgSurf // the debugger's picture
{
	uint32_t* px;                   // 0x00RRGGBB rows, w * h pixels
	int w, h;                       // size in pixels
	int clipX, clipY, clipW, clipH; // the clip rectangle every primitive honours
} DbgSurf_t;

extern DbgSurf_t gDbgSurf;

int DbgDraw_Init(int w, int h); // the surface and the glyph cache; 1 ok
void DbgDraw_Shutdown(void);
void DbgDraw_Clip(int x, int y, int w, int h); // (0, 0, 0, 0) resets to the whole surface
void DbgDraw_Fill(int x, int y, int w, int h, uint32_t rgb);
void DbgDraw_Frame(int x, int y, int w, int h, uint32_t rgb); // a one-pixel outline
void DbgDraw_HLine(int x, int y, int w, uint32_t rgb);
void DbgDraw_VLine(int x, int y, int h, uint32_t rgb);
/* text in the engine's text encoding (Shift-JIS; the debugger's own labels
 * are ASCII), drawn with the glyph cache; returns the width in pixels */
int DbgDraw_Text(int x, int y, uint32_t rgb, const char* text);
int DbgDraw_TextF(int x, int y, uint32_t rgb, const char* fmt, ...);
int DbgDraw_TextWidth(const char* text);
// a bitmap (16- or 32-bit) scaled by nearest neighbour into the rectangle, keeping its aspect
void DbgDraw_Bitmap(int x, int y, int w, int h, const Bmp_t* b);

// colours (0x00RRGGBB)
#define DBG_BG      0x1e1e22u // the window background
#define DBG_PANEL   0x26262cu // the tab bar, the status line, pane titles
#define DBG_LINE    0x3c3c46u // frames and separators
#define DBG_TEXT    0xd8d8d8u // ordinary text
#define DBG_DIM     0x8a8a94u // secondary text
#define DBG_HILITE  0x3a4a6au // the selected line of a listing
#define DBG_SELECT  0x2f5f9fu // the selected row of a list, the outline of the focused pane
#define DBG_ACCENT  0xe0c060u // what deserves attention (the current tab, pane titles, labels, the current module)
#define DBG_GREEN   0x70d070u // "running"
#define DBG_RED     0xe06060u // "PAUSED"
#define DBG_CURRENT 0x60402au // the listing line at the instruction pointer
#define DBG_BREAK   0xa02020u // a breakpoint marker

// ---- lists (dbg.c) ----------------------------------------------------------------------

typedef struct DbgList // a scrollable list of rows of DBG_CELL_H pixels with one selected
{
	int count;      // rows
	int sel;        // the selected row, -1 none
	int top;        // the first row shown
	int rows;       // rows that fit
	int x, y, w, h; // the pane, for the mouse
} DbgList_t;

void DbgList_Clamp(DbgList_t* l);                  // keep sel and top inside the list
void DbgList_Key(DbgList_t* l, int vk);            // up / down / page up / page down / home / end
int DbgList_Hit(const DbgList_t* l, int x, int y); // the row under a point, -1 none
void DbgList_Scroll(DbgList_t* l, int rows);       // scroll by rows (negative: up), the selection stays
void DbgList_Show(DbgList_t* l, int row);          // bring a row into view

// ---- the views ----------------------------------------------------------------------------

enum DbgView // the tabs, in order (keys 1 .. 7)
{
	DBG_VIEW_THREADS,
	DBG_VIEW_PROGRAMS,
	DBG_VIEW_BITMAPS,
	DBG_VIEW_OBJECTS,
	DBG_VIEW_SOUND,
	DBG_VIEW_MEMORY,
	DBG_VIEW_LOG,
	DBG_VIEW_COUNT
};

/* the view functions: draw the panes into the area (x, y, w, h) between
 * the tab bar and the status line; handle a key (virtual key and
 * character) / a click (window coordinates, button 0 left) / the wheel
 * at the mouse position (1 when consumed) */
typedef struct DbgViewOps
{
	const char* name; // the tab's label
	void (*draw)(int x, int y, int w, int h);
	int (*key)(int vk, int ch);
	int (*click)(int x, int y, int button);
	int (*wheel)(int x, int y, int delta);
} DbgViewOps_t;

extern const DbgViewOps_t kDbgViewThreads, kDbgViewPrograms, kDbgViewBitmaps, kDbgViewObjects, kDbgViewSound,
	kDbgViewMemory, kDbgViewLog;

// ---- shared state (dbg.c) ------------------------------------------------------------------

// the log: a ring of DBG_LOG_LINES lines, gDbgLogNext the slot the next line takes
typedef struct DbgLogLine
{
	char text[160]; // one line, truncated to fit
} DbgLogLine_t;
#define DBG_LOG_LINES 400
extern DbgLogLine_t gDbgLog[DBG_LOG_LINES];
extern int gDbgLogCount, gDbgLogNext;

// the thread the views look at (its id; the pointer is looked up each frame)
extern uint32_t gDbgThreadId;
struct Thread* Dbg_SelectedThread(void);

// run control
extern int gDbgPaused; // the threads are held (Dbg_GateRun)
extern int gDbgTrace;  // instruction trace into the log

// breakpoints: a module name and an offset inside it
typedef struct DbgBreak
{
	char module[0x40]; // the module's name as the thread loaded it
	uint32_t off;      // the instruction's offset in the module
} DbgBreak_t;
#define DBG_MAX_BREAKS 64
extern DbgBreak_t gDbgBreaks[DBG_MAX_BREAKS];
extern int gDbgBreakCount;
int Dbg_ToggleBreak(const char* module, uint32_t off); // 1 set, 0 cleared (or no room)
int Dbg_HasBreak(const char* module, uint32_t off);

// the listing of a module (dbg_listing.c): lines of a Dis_Program listing with their offsets
typedef struct DbgListing
{
	char key[0x60]; // "module name:base:size" of the thread it was made for
	char* text;     // the whole listing
	char** lines;   // pointers into `text` (NUL-terminated)
	uint32_t* offs; // the offset of each line (the next instruction's for a label line); ~0u none
	int count;      // lines
	int problems;   // the disassembler noted flow problems (shown in the pane's title)
} DbgListing_t;
// the listing of a module as thread t has it loaded, from the cache or made now; NULL when it cannot be made
const DbgListing_t* DbgListing_Get(struct Thread* t, const char* module, uint32_t base, uint32_t size);
int DbgListing_LineOf(const DbgListing_t* l, uint32_t off); // the line of an offset, -1 none
void DbgListing_FreeAll(void);                              // drop the cache (closing, shutdown)

// the module of a thread containing a code offset: its name, base and size (1 found)
int Dbg_ModuleAt(struct Thread* t, uint32_t off, const char** name, uint32_t* base, uint32_t* size);

// ---- what the views share (dbg_pane.c) -----------------------------------------------------

#define DBG_LEFT_W 380 // the width of a view's left list
#define DBG_PAD    6   // the text inset of a pane

// A framed pane with an optional title bar.
void DbgPane_Frame(int x, int y, int w, int h, const char* title);
// Outline the pane the arrow keys act on.
void DbgPane_Focus(int x, int y, int w, int h);
/* The views with two panes give the keyboard to one of them: Left / Right
 * choose (0 the left list, 1 the right pane); 1 when vk was one of them. */
int DbgPane_IsFocusKey(int vk, int* focus);
// Up / Down / PgUp / PgDn / Home / End
int DbgPane_IsNavKey(int vk);
// Place the rows of a list inside a titled pane.
void DbgPane_ListArea(DbgList_t* l, int x, int y, int w, int h);
// Paint the background of a row / the y of a row.
void DbgPane_RowBg(const DbgList_t* l, int row, uint32_t rgb);
int DbgPane_RowY(const DbgList_t* l, int row);
// Is (x, y) inside the rectangle?
int DbgPane_Contains(int x, int y, int px, int py, int pw, int ph);
/* A tagged script value described into out: "code+0x12", "data+0x40",
 * "heap+0x8", "dyn3+0x10", "gmem+0x100" or the plain number. */
void Dbg_DescribeValue(uint32_t v, char* out, size_t n);
// The name of a bitmap mode ("RGB16", "ARGB32", ...).
const char* Dbg_BmpModeName(int mode);

// The listing pane: a module's disassembly with the current line and the breakpoints marked.
typedef struct DbgListingPane
{
	DbgList_t list;              // the lines
	const char* module;          // what it shows (for the breakpoints)
	uint32_t base, size;         // the module's place in the thread's code area
	const DbgListing_t* listing; // the cached listing, NULL when none could be made
	int follow;                  // keep the current instruction in view (off once the user scrolls; F restores it)
} DbgListingPane_t;
// Switch the pane to a module of thread t (nothing happens when it shows it already).
void DbgListingPane_Show(DbgListingPane_t* p, struct Thread* t, const char* module, uint32_t base, uint32_t size);
// Draw it at (x, y, w, h) with t's instruction pointer highlighted.
void DbgListingPane_Draw(DbgListingPane_t* p, struct Thread* t, int x, int y, int w, int h, const char* title);
// The key, click and wheel handlers; 1 when consumed (F9 toggles a breakpoint on the selected line).
int DbgListingPane_Key(DbgListingPane_t* p, int vk);
int DbgListingPane_Click(DbgListingPane_t* p, int x, int y, int button);
int DbgListingPane_Wheel(DbgListingPane_t* p, int x, int y, int delta);

#endif // BGI_DBG_INTERNAL_H_
