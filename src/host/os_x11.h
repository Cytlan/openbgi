/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_x11.h - what the X11 files of the POSIX back end share
 *            (src/host/posix/x11.c, window.c, events.c, the dialogs);
 *            private to them.  The connection and its atoms, the
 *            title and key helpers, and the modal loop of the dialogs.
 */
#ifndef BGI_OS_X11_H_
#define BGI_OS_X11_H_

#include "host/os.h"
#include "host/os_posix.h"

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
/* the text typed by a key event (through the input method when there is
 * one) as UTF-8 into out (n bytes, NUL-terminated); control characters
 * are left out.  The length. */
int X11_EventUtf8(XKeyEvent* ev, XIC ic, char* out, int n);
/* run `handler` for every event of `modal` until it returns 0 (the dialog
 * is done); events for the engine's windows are still dispatched, with
 * their input discarded while the modal window is up */
void X11_RunModal(Window modal, int (*handler)(XEvent* ev, void* ctx), void* ctx);

#endif // BGI_OS_X11_H_
