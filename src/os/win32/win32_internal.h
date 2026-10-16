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

#include "bgi/os_win32.h"
#include <imm.h>
#include <mmsystem.h>
#include <shellapi.h>

#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/msg.h"

// ---- window.c ---------------------------------------------------------------------------------

extern OsEventHandlers_t gWin32Handlers; // the engine's callbacks (OS_SetEventHandlers); every pointer may be NULL
extern int gWin32FrameW, gWin32FrameH;   // the frame the window manager adds around the client area (pixels, both sides together)
extern HCURSOR gWin32Cursors[5];         // the cursor table "80 67" selects from; entries 2..4 stay NULL
extern int gWin32CursorShape;            // the shape "80 67" selected (0..4), applied on WM_SETCURSOR

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
void Win32_FillLetterbox(HDC hdc); // the black bars beside the scaled picture (nothing when it is not scaled)

// ---- the window procedures (wndproc.c, child.c, edit.c) --------------------------------------

LRESULT CALLBACK Win32_MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK Win32_ChildWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK Win32Edit_SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// ---- edit.c -----------------------------------------------------------------------------------

void Win32Edit_Create(HWND parent); // the hidden EDIT control, created with the main window (OS_WindowCreate)
extern COLORREF gWin32EditColour;   // the text colour WM_CTLCOLOREDIT applies (OS_EditSetColour)

#endif
