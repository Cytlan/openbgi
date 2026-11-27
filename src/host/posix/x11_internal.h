/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * x11_internal.h - shared between the X11 window files of the POSIX back
 *                  end (x11.c, window.c, child.c, events.c, dbgwin.c);
 *                  the dialog files use only src/host/os_x11.h and
 *                  ui_internal.h
 *
 * The state of the main window lives in window.c and is read by the event
 * pump (events.c), which keeps the focus, iconic and pointer bookkeeping
 * up to date; the child windows and the debugger's window each own their
 * part and hand the pump a handler for their events.
 */
#ifndef BGI_OS_POSIX_X11_INTERNAL_H
#define BGI_OS_POSIX_X11_INTERNAL_H

#include "host/os_x11.h"
#include "host/os_common.h"

#include <locale.h>
#include <unistd.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>

// ---- x11.c: the connection and the pixel conversions ---------------------------------------

// the EWMH atoms the window files use (interned by X11_Open)
extern Atom gX11AtomNetWmState, gX11AtomNetWmStateFs, gX11AtomNetFrameExtents, gX11AtomNetWmPid,
	gX11AtomSupportingWmCheck;
extern Atom gX11AtomNetWorkarea;
// the visual's channels: the bit position and width of red, green and blue in a pixel
extern int gX11VisRShift, gX11VisGShift, gX11VisBShift, gX11VisRBits, gX11VisGBits, gX11VisBBits;
// the host's handlers (OS_SetHostHandlers); every pointer is checked before the call
extern OsHostHandlers_t gX11Input;

// a scratch buffer of that many 32-bit pixels (grown as needed, never freed)
uint32_t* X11_ConvBuffer(size_t pixels);
uint32_t X11_PackPixel(uint32_t rgb);                                      // 0x00RRGGBB -> the visual's pixel
uint32_t X11_SurfacePixel(const OsSurface_t* s, int x, int y);             // one pixel of a surface as 0x00RRGGBB
void X11_PutImage(Window win, int x, int y, int w, int h, uint32_t* data); // rows of the visual's pixels (pitch w) to the window

// ---- window.c: the main window ----------------------------------------------------------------

// What OS_WindowGetDc hands out: the window to draw into (a child window's too, child.c).
typedef struct X11Dc
{
	Window win;
	int child; // slot index, -1 = the main window
} X11Dc_t;

extern Window gX11Win;    // 0 before OS_WindowCreate
extern XIC gX11Xic;       // the input context of the main window, NULL without an input method
extern int gX11TextInput; // the engine takes typed text (OS_TextInput): the input context has the focus with the window
extern X11Dc_t gX11MainDc;
extern int gX11Alive, gX11Shown;                           // created / mapped
extern int gX11Active, gX11Minimized, gX11HiddenByUs;      // focus and iconic state (the pump keeps them); hidden by OS_WindowShow(0)
extern int gX11FrameL, gX11FrameT, gX11FrameR, gX11FrameB; // _NET_FRAME_EXTENTS once the window manager tells
extern int gX11QuitPosted;                                 // a signal asked for the quit: the next pump raises `quit`
extern int gX11LastClickBtn;                               // double-click detection: the last press's button (-1 none), time and position
extern Time gX11LastClickTime;
extern int gX11LastClickX, gX11LastClickY;
extern int gX11ModalDepth; // a dialog of uikit.c is up: the engine's windows ignore input

// mode (engine) coordinates <-> window pixels (the identity outside full screen)
int X11_ToWinX(int x);
int X11_ToWinY(int y);
int X11_FromWinX(int x);
int X11_FromWinY(int y);

/* w x h pixels of `s` from (srcX, srcY) to (dstX, dstY) of the window
 * (its engine coordinates: the main window's through the full-screen
 * mapping), clipped to the window */
void X11_BlitSurface(const X11Dc_t* dc, int dstX, int dstY, int w, int h, const OsSurface_t* s, int srcX, int srcY);
void X11_FillLetterbox(void); // the black bars beside a scaled full-screen picture

int X11_HasWindowManager(void);                            // _NET_SUPPORTING_WM_CHECK is set
int X11_ReadFrameExtents(void);                            // read _NET_FRAME_EXTENTS into gX11Frame*: 1 when they are others than before
int X11_ClientResized(int w, int h, unsigned long serial); // a ConfigureNotify's size: 1 when a window with a sizing border got another one
void X11_UpdateClientOrigin(void);                         // re-read where the client area sits on the root window
void X11_NotePointer(int rootX, int rootY);                // the last known pointer position (root coordinates)

// ---- glx.c: OpenGL on the main window, where the system has it ------------------------------------

int X11Gl_Attach(Window win); // a context on the window: 1 when its picture goes through the graphics processor from now on
void X11Gl_Detach(void);      // before the window goes
int X11Gl_On(void);
void X11Gl_Swap(void); // what was drawn is shown

// ---- xdnd.c: files dropped on the main window ---------------------------------------------------

void X11_SurfaceTell(void);   // the window as the surface the picture is laid on, to the engine when it is another (window.c)
void X11_DndApply(void);      // the window says whether it takes drops (OS_DragAccept); for a new window too
void X11_DndReset(void);      // the main window is gone
int X11_DndEvent(XEvent* ev); // 1 when the event was the drop protocol's

// ---- child.c: the pop-up child windows ----------------------------------------------------------

typedef struct X11Child
{
	Window win;           // 0 when there is none (the slot free, or the window destroyed from outside)
	X11Dc_t dc;           // the window for X11_BlitSurface; dc.child is the slot index
	int used, shown;      // the engine has the slot / mapped
	int clientW, clientH; // client size in pixels
	OsSurface_t picture;  // what was presented last (the engine's pixels, valid until the next present or the close)
	const char* copyText; // what Ctrl+C copies (the engine's, valid as long); NULL nothing
} X11Child_t;

X11Child_t* X11Child_ByWindow(Window w); // the slot of a child window, NULL when it is not one
void X11Child_Show(X11Child_t* c);       // the picture presented last to the window again

// ---- events.c / dbgwin.c: the event handlers DispatchEvent routes to ------------------------------

void X11_HandleMainEvent(XEvent* ev);                 // an event of the main window
void X11_HandleChildEvent(X11Child_t* c, XEvent* ev); // an event of a child window
void X11_HandleDebugEvent(XEvent* ev);                // an event of the debugger's window
int X11_IsDebugWindow(Window w);                      // the debugger's window exists and is w

#endif
