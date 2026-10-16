/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_x11.h - what the X11 files of the POSIX back end share
 *            (src/os/posix/x11.c, window.c, events.c, field.c, the dialogs);
 *            private to them.  The connection and its atoms, the title and
 *            key helpers, the modal loop of the dialogs, and the
 *            single-line text field the text entry and the dialogs draw.
 */
#ifndef BGI_OS_X11_H_
#define BGI_OS_X11_H_

#include "bgi/os.h"
#include "bgi/os_posix.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

// the connection (x11.c); X11_Open() connects lazily and returns 0 without a display
extern Display* gX11Dpy;
extern int gX11Screen;
extern Window gX11Root;
extern Visual* gX11Visual; // the default visual; TrueColor is assumed
extern int gX11Depth;
extern Atom gX11WmDelete, gX11WmProtocols, gX11NetWmName, gX11Utf8String;
extern XIM gX11Im; // the input method; NULL without one

int X11_Open(void); // connect once (idempotent); 1 when a display is available
// a 32-bit 0x00RRGGBB buffer (pitch in pixels) copied to (x, y) of `win`
void X11_PutArgb(Window win, int x, int y, int w, int h, const uint32_t* pix, int pitch);
void X11_SetTitle(Window win, const char* sjis); // _NET_WM_NAME in UTF-8 and the ASCII part as WM_NAME
// Windows virtual-key code of a key event (0 when none); `sys` = Alt held or F10
int X11_EventVk(XKeyEvent* ev, int* sys);
// the Shift-JIS bytes typed by a key event (through the input method when there is one) into out (n bytes); the count
int X11_EventText(XKeyEvent* ev, XIC ic, char* out, int n);
/* run `handler` for every event of `modal` until it returns 0 (the dialog
 * is done); events for the engine's windows are still dispatched, with
 * their input discarded while the modal window is up */
void X11_RunModal(Window modal, int (*handler)(XEvent* ev, void* ctx), void* ctx);
int X11_FrameLeft(void); // the main window's frame extents in pixels, as far as the window manager told
int X11_FrameTop(void);

// a single-line text field (field.c): the edit control (edit.c) and the dialogs share it
typedef struct X11Field
{
	char text[0x100]; // Shift-JIS, NUL-terminated
	int limit;        // bytes, 0 = unlimited (the buffer size still applies)
	int caret;        // byte offset of the caret in `text`
	int selAll;       // the whole text is selected: typing replaces it
	int noAscii;      // reject single-byte printable characters (the edit control's "B0 29")
} X11Field_t;
void X11Field_SetText(X11Field_t* f, const char* text); // replace the text (cut to the limit), caret at the end, nothing selected
void X11Field_SelectAll(X11Field_t* f);                 // select everything (nothing when empty)
/* 1 when the field changed, 0 when the key was handled without a change,
 * -1 when the key is not a text-entry key (Tab, Enter, Escape, ...) */
int X11Field_Key(X11Field_t* f, XKeyEvent* ev, XIC ic);
/* draw the field into a 32-bit buffer (pitch in pixels, bufW x bufH) at
 * (x, y, w, h): the text (centred when `centred`, else left-aligned and
 * scrolled to keep the caret in view) with the selection and caret when
 * `focused`; `frame` adds the sunken white box of a dialog field */
void X11Field_Draw(const X11Field_t* f, uint32_t* pix, int pitch, int bufW, int bufH, int x, int y, int w, int h,
	struct OsFont* font, uint32_t rgb, int centred, int focused, int frame);

#endif // BGI_OS_X11_H_
