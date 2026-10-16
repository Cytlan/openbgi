/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * x11_internal.h - shared between the X11 window files of the POSIX back
 *                  end (x11.c, window.c, child.c, edit.c, events.c,
 *                  dbgwin.c); the dialog files use only inc/bgi/os_x11.h
 *                  and ui_internal.h
 *
 * The state of the main window lives in window.c and is read by the event
 * pump (events.c), which keeps the focus, iconic and pointer bookkeeping
 * up to date; the child windows, the text entry and the debugger's window
 * each own their part and hand the pump a handler for their events.
 */
#ifndef BGI_OS_POSIX_X11_INTERNAL_H
#define BGI_OS_POSIX_X11_INTERNAL_H

#include "bgi/os_x11.h"
#include "bgi/os_common.h"

#include <locale.h>
#include <unistd.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>

// ---- x11.c: the connection and the pixel conversions ---------------------------------------

// the EWMH atoms the window files use (interned by X11_Open)
extern Atom gX11AtomNetWmState, gX11AtomNetWmStateFs, gX11AtomNetFrameExtents, gX11AtomNetWmPid,
	gX11AtomSupportingWmCheck;
// the visual's channels: the bit position and width of red, green and blue in a pixel
extern int gX11VisRShift, gX11VisGShift, gX11VisBShift, gX11VisRBits, gX11VisGBits, gX11VisBBits;
// the engine's callbacks (OS_SetEventHandlers); every pointer is checked before the call
extern OsEventHandlers_t gX11Handlers;

// a scratch buffer of that many 32-bit pixels (grown as needed, never freed)
uint32_t* X11_ConvBuffer(size_t pixels);
uint32_t X11_PackPixel(uint32_t rgb);                                      // 0x00RRGGBB -> the visual's pixel
uint32_t X11_SurfacePixel(const OsSurface_t* s, int x, int y);             // one pixel of a surface as 0x00RRGGBB
void X11_PutImage(Window win, int x, int y, int w, int h, uint32_t* data); // rows of the visual's pixels (pitch w) to the window

// ---- window.c: the main window ----------------------------------------------------------------

// What OS_WindowGetDc hands out: the window to draw into.
typedef struct X11Dc
{
	Window win;
	int child; // slot index, -1 = the main window
} X11Dc_t;

// The picture under the text entry, captured when it is shown and kept up to date by the blits.
typedef struct X11Capture
{
	int active;     // the engine's blits go into `pix` instead of the window (during edit_paint)
	int x, y, w, h; // client rectangle captured
	uint32_t* pix;  // w * h, 0x00RRGGBB
} X11Capture_t;

extern Window gX11Win; // 0 before OS_WindowCreate
extern XIC gX11Xic;    // the input context of the main window, NULL without an input method
extern X11Dc_t gX11MainDc;
extern X11Capture_t gX11Capture;
extern int gX11Alive, gX11Shown;                           // created / mapped
extern int gX11Active, gX11Minimized, gX11HiddenByUs;      // focus and iconic state (the pump keeps them); hidden by OS_WindowShow(0)
extern int gX11FrameL, gX11FrameT, gX11FrameR, gX11FrameB; // _NET_FRAME_EXTENTS once the window manager tells
extern int gX11CloseRequested, gX11QuitPosted;             // OS_WindowClose / OS_PostQuit, delivered by the next pump
extern uint8_t gX11Keys[256];                              // GetAsyncKeyState emulation, indexed by virtual key
extern int gX11LastClickBtn;                               // double-click detection: the last press's button (-1 none), time and position
extern Time gX11LastClickTime;
extern int gX11LastClickX, gX11LastClickY;
extern int gX11ModalDepth; // a dialog of uikit.c is up: the engine's windows ignore input

// mode (engine) coordinates <-> window pixels (the identity outside full screen)
int X11_ToWinX(int x);
int X11_ToWinY(int y);
int X11_FromWinX(int x);
int X11_FromWinY(int y);

/* The (srcX, srcY, srcW, srcH) rectangle of `s` stretched to the
 * destination rectangle (in the target window's engine coordinates),
 * clipped to `clip` ({l, t, r, b}, exclusive, or NULL) and to the window;
 * `smooth` filters bilinearly when the sizes differ. */
void X11_BlitSurface(const X11Dc_t* dc, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* s, int srcX,
	int srcY, int srcW, int srcH, int smooth, const int* clip);
void X11_FillLetterbox(void); // the black bars beside a scaled full-screen picture

int X11_HasWindowManager(void);             // _NET_SUPPORTING_WM_CHECK is set
void X11_ReadFrameExtents(void);            // read _NET_FRAME_EXTENTS into gX11Frame*
void X11_UpdateClientOrigin(void);          // re-read where the client area sits on the root window
void X11_NotePointer(int rootX, int rootY); // the last known pointer position (root coordinates)

// ---- child.c: the pop-up child windows ----------------------------------------------------------

typedef struct X11Child
{
	Window win;               // 0 when the slot is free
	X11Dc_t dc;               // what OS_ChildGetDc hands out; dc.child is the slot index
	int used, closing, shown; // the slot is taken / OS_ChildClose is destroying it / mapped
	int clientW, clientH;     // client size in pixels (the outer size less the main window's frame)
} X11Child_t;

X11Child_t* X11Child_ByWindow(Window w); // the slot of a child window, NULL when it is not one

// ---- edit.c: the text entry -------------------------------------------------------------------

void X11Edit_Create(void);      // called once with the main window (the entry starts hidden)
int X11Edit_Visible(void);      // shown over the picture
int X11Edit_HasFocus(void);     // takes the keys
void X11Edit_Present(void);     // repaint the entry: the picture under it, then the text
int X11Edit_Key(XKeyEvent* ev); // a key while the entry has the focus; 1 when consumed

// ---- events.c / dbgwin.c: the event handlers DispatchEvent routes to ------------------------------

void X11_HandleMainEvent(XEvent* ev);                 // an event of the main window
void X11_HandleChildEvent(X11Child_t* c, XEvent* ev); // an event of a child window
void X11_HandleDebugEvent(XEvent* ev);                // an event of the debugger's window
int X11_IsDebugWindow(Window w);                      // the debugger's window exists and is w

#endif
