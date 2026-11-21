/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * win32_internal.h - what the window files of the Win32 back end share
 *                    (window.c, wndproc.c, child.c, edit.c, dbgwin.c): the
 *                    engine's callbacks, the frame metrics, the cursor
 *                    table, the three window procedures and the EDIT
 *                    control's creation
 *
 * window.c owns the main window and the globals below; wndproc.c is its
 * window procedure and the message pump; child.c and edit.c hold the
 * procedures of the pop-up windows and of the text entry; dbgwin.c is the
 * debugger's own window.  The system files include sys_internal.h instead
 * and the dialog files ui_internal.h; os_win32.h carries what every file
 * of the back end sees.
 */
#ifndef BGI_OS_WIN32_INTERNAL_H
#define BGI_OS_WIN32_INTERNAL_H

#include "host/os_win32.h"
#include <imm.h>
#include <mmsystem.h>
#include <shellapi.h>

#include "host/os.h"
#include "host/os_common.h"
#include "bgi/msg.h"

// ---- window.c ---------------------------------------------------------------------------------

extern OsHostHandlers_t gWin32Input; // the host's handlers (OS_SetHostHandlers); every pointer may be NULL
extern HCURSOR gWin32Cursors[5];     // the cursor table "80 67" selects from; entries 2..4 stay NULL
extern int gWin32CursorShape;        // the shape "80 67" selected (0..4), applied on WM_SETCURSOR

/* A BITMAPINFOHEADER for `rows` rows of a surface as the original builds
 * one: top-down (negative height), BI_RGB at the surface depth, so a
 * 16-bit surface is RGB 5-5-5 and a 32-bit one 0x00RRGGBB.  biWidth is
 * taken from the pitch so any 4-byte padding is honoured. */
void Win32_SurfaceHeader(BITMAPINFOHEADER* bi, const OsSurface_t* s, int rows);
/* the mapping between the engine's picture and a windowed client area of
 * another size ("81 64"; OS_WindowSetPicture): picture pixels <-> client
 * pixels; the identity while the two are of one size or in full screen */
int Win32_ToWinX(int x);
int Win32_ToWinY(int y);
int Win32_FromWinX(int x);
int Win32_FromWinY(int y);
void Win32_FillLetterbox(HDC hdc);     // the black bars beside the scaled picture (nothing when it is not scaled)
int Win32_ClientResized(int w, int h); // WM_SIZE: 1 when a window with a sizing border (OS_WindowSetResizable) got another client size from the player
double Win32_MapScale(void);           // how much larger the picture is shown than it is (1 while it is not scaled)
/* the window's size while the player drags it: WM_SIZING's rectangle kept
 * in the proportions of OS_WindowSetAspect (`edge` is its WMSZ_*), and the
 * smallest size of WM_GETMINMAXINFO */
void Win32_KeepAspect(int edge, RECT* r);
void Win32_MinTrack(POINT* min);
void Win32_OwnSize(int by);      // +1 .. -1 around a time in which a size the window gets is not one the player gave it
void Win32_DisplayChanged(void); // WM_DISPLAYCHANGE: a full screen without a mode switch follows its monitor
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0 // the scale of the window's monitor is another (Windows 8.1 on)
#endif

// ---- wgl.c: OpenGL on the main window --------------------------------------------------------

/* Whether the picture will be shown through OpenGL: the system has it,
 * with a graphics processor behind it (asked once, on a hidden window of
 * its own).  The window's styles, the text entry and the full screen go
 * by the answer. */
int Win32Gl_Usable(void);
int Win32Gl_Attach(HWND hwnd); // a context on the main window (once per window: with it made); 1 when the picture goes through it
void Win32Gl_Detach(void);     // before the window goes
int Win32Gl_On(void);
void Win32Gl_Swap(void); // what was drawn onto the window

// ---- the window procedures (wndproc.c, child.c, edit.c) --------------------------------------

LRESULT CALLBACK Win32_MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK Win32_ChildWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK Win32Edit_SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// ---- edit.c -----------------------------------------------------------------------------------

void Win32Edit_Create(HWND parent); // the hidden EDIT control, created with the main window (OS_WindowCreate)
void Win32Edit_Remap(void);         // the control's place, size and font again after the picture's mapping changed
extern COLORREF gWin32EditColour;   // the text colour WM_CTLCOLOREDIT applies (the entry's setColour)

#endif
