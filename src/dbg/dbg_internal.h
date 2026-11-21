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
 * eight views, each exported as a DbgViewOps_t.
 */
#ifndef BGI_DBG_INTERNAL_H_
#define BGI_DBG_INTERNAL_H_

#include "bgi/dbg.h"
#include "bgi/gfx/bitmap.h"

// ---- the surface and the drawing primitives (dbgdraw.c) ----------------------------------

/* the text cell: DBG_CELL_H (16) pixels high; 8 wide with a half-width
 * font (MS Gothic, the fixed-pitch Japanese fonts), else the font's own
 * advance (o->dbg.dbgCellW, measured when the font opens) */
#define DBG_CELL_W(o) o->dbg.dbgCellW

int DbgDraw_Init(OpenBGI_t* o, int w, int h); // the surface and the glyph cache; 1 ok
void DbgDraw_Shutdown(OpenBGI_t* o);
void DbgDraw_Clip(OpenBGI_t* o, int x, int y, int w, int h); // (0, 0, 0, 0) resets to the whole surface
void DbgDraw_Fill(OpenBGI_t* o, int x, int y, int w, int h, uint32_t rgb);
void DbgDraw_Frame(OpenBGI_t* o, int x, int y, int w, int h, uint32_t rgb); // a one-pixel outline
void DbgDraw_HLine(OpenBGI_t* o, int x, int y, int w, uint32_t rgb);
void DbgDraw_VLine(OpenBGI_t* o, int x, int y, int h, uint32_t rgb);
/* text in the engine's text encoding (Shift-JIS; the debugger's own labels
 * are ASCII), drawn with the glyph cache; returns the width in pixels */
int DbgDraw_Text(OpenBGI_t* o, int x, int y, uint32_t rgb, const char* text);
int DbgDraw_TextF(OpenBGI_t* o, int x, int y, uint32_t rgb, const char* fmt, ...);
int DbgDraw_TextWidth(OpenBGI_t* o, const char* text);
// a bitmap (16- or 32-bit) scaled by nearest neighbour into the rectangle, keeping its aspect
void DbgDraw_Bitmap(OpenBGI_t* o, int x, int y, int w, int h, const Bmp_t* b);

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

void DbgList_Clamp(DbgList_t* l);                  // keep sel and top inside the list
void DbgList_Key(DbgList_t* l, int vk);            // up / down / page up / page down / home / end
int DbgList_Hit(const DbgList_t* l, int x, int y); // the row under a point, -1 none
void DbgList_Scroll(DbgList_t* l, int rows);       // scroll by rows (negative: up), the selection stays
void DbgList_Show(DbgList_t* l, int row);          // bring a row into view

// ---- the views ----------------------------------------------------------------------------

enum DbgView // the tabs, in order (keys 1 .. 8)
{
	DBG_VIEW_THREADS,
	DBG_VIEW_PROGRAMS,
	DBG_VIEW_BITMAPS,
	DBG_VIEW_OBJECTS,
	DBG_VIEW_SOUND,
	DBG_VIEW_MEMORY,
	DBG_VIEW_LOG,
	DBG_VIEW_TARGETS,
	DBG_VIEW_COUNT
};

/* the view functions: draw the panes into the area (x, y, w, h) between
 * the tab bar and the status line; handle a key (virtual key and
 * character) / a click (window coordinates, button 0 left) / the wheel
 * at the mouse position (1 when consumed) */
typedef struct DbgViewOps
{
	const char* name; // the tab's label
	void (*draw)(OpenBGI_t* o, int x, int y, int w, int h);
	int (*key)(OpenBGI_t* o, int vk, int ch);
	int (*click)(OpenBGI_t* o, int x, int y, int button);
	int (*wheel)(OpenBGI_t* o, int x, int y, int delta);
} DbgViewOps_t;

extern const DbgViewOps_t kDbgViewThreads, kDbgViewPrograms, kDbgViewBitmaps, kDbgViewObjects, kDbgViewSound,
	kDbgViewMemory, kDbgViewLog, kDbgViewTargets;

// ---- shared state (dbg.c; the fields are o->dbg's) ----------------------------------------

// the thread the views look at (o->dbg.dbgThreadId; the pointer is looked up each frame)
struct Thread* Dbg_SelectedThread(OpenBGI_t* o);

// breakpoints: a module name and an offset inside it
int Dbg_ToggleBreak(OpenBGI_t* o, const char* module, uint32_t off); // 1 set, 0 cleared (or no room)

int Dbg_HasBreak(OpenBGI_t* o, const char* module, uint32_t off);

// the listing of a module as thread t has it loaded, from the cache or made now; NULL when it cannot be made
const DbgListing_t* DbgListing_Get(OpenBGI_t* o, struct Thread* t, const char* module, uint32_t base, uint32_t size);
int DbgListing_LineOf(const DbgListing_t* l, uint32_t off); // the line of an offset, -1 none
void DbgListing_FreeAll(OpenBGI_t* o);                      // drop the cache (closing, shutdown)
// whether a listing of the cache still is that module's (none is once the cache was dropped or its slot reused)
int DbgListing_Holds(const DbgListing_t* l, const char* module, uint32_t base, uint32_t size);

// the module of a thread containing a code offset: its name, base and size (1 found)
int Dbg_ModuleAt(struct Thread* t, uint32_t off, const char** name, uint32_t* base, uint32_t* size);

// ---- what the views share (dbg_pane.c) -----------------------------------------------------

#define DBG_LEFT_W 380 // the width of a view's left list
#define DBG_PAD    6   // the text inset of a pane

// A framed pane with an optional title bar.
void DbgPane_Frame(OpenBGI_t* o, int x, int y, int w, int h, const char* title);
// Outline the pane the arrow keys act on.
void DbgPane_Focus(OpenBGI_t* o, int x, int y, int w, int h);
/* The views with two panes give the keyboard to one of them: Left / Right
 * choose (0 the left list, 1 the right pane); 1 when vk was one of them. */
int DbgPane_IsFocusKey(int vk, int* focus);
// Up / Down / PgUp / PgDn / Home / End
int DbgPane_IsNavKey(int vk);
// Place the rows of a list inside a titled pane.
void DbgPane_ListArea(DbgList_t* l, int x, int y, int w, int h);
// Paint the background of a row / the y of a row.
void DbgPane_RowBg(OpenBGI_t* o, const DbgList_t* l, int row, uint32_t rgb);
int DbgPane_RowY(const DbgList_t* l, int row);
// Is (x, y) inside the rectangle?
int DbgPane_Contains(int x, int y, int px, int py, int pw, int ph);
/* A tagged script value described into out: "code+0x12", "data+0x40",
 * "heap+0x8", "dyn3+0x10", "gmem+0x100" or the plain number. */
void Dbg_DescribeValue(OpenBGI_t* o, uint32_t v, char* out, size_t n);
// The name of a bitmap mode ("RGB16", "ARGB32", ...).
const char* Dbg_BmpModeName(int mode);

// The listing pane: a module's disassembly with the current line and the breakpoints marked.
// Switch the pane to a module of thread t (nothing happens when it shows it already).
void DbgListingPane_Show(OpenBGI_t* o, DbgListingPane_t* p, struct Thread* t, const char* module, uint32_t base, uint32_t size);
// Draw it at (x, y, w, h) with t's instruction pointer highlighted.
void DbgListingPane_Draw(OpenBGI_t* o, DbgListingPane_t* p, struct Thread* t, int x, int y, int w, int h, const char* title);
// The key, click and wheel handlers; 1 when consumed (F9 toggles a breakpoint on the selected line).
int DbgListingPane_Key(OpenBGI_t* o, DbgListingPane_t* p, int vk);
int DbgListingPane_Click(OpenBGI_t* o, DbgListingPane_t* p, int x, int y, int button);
int DbgListingPane_Wheel(DbgListingPane_t* p, int x, int y, int delta);

#endif // BGI_DBG_INTERNAL_H_
