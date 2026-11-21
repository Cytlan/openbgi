/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * child.c - the pop-up child windows of the POSIX back end ("B0 1x" of the
 *           scripts, src/sys/child.c), the host's child windows
 *           (OS_ChildWindows, OpenBGI_ChildWindows_t of openbgi.h)
 *
 * Each of the eight slots is a transient top-level window of fixed size
 * owned by the main window (so the window manager keeps it above).  The
 * engine speaks of outer positions: the main window's frame extents stand
 * in for the child's own, which are not known before it is mapped.  The
 * engine hands each window its picture whenever it changed (`present`);
 * the window shows it at once and again on every Expose.  The handle the
 * engine gets is the slot itself; a window the system destroyed leaves
 * its slot taken until the engine closes it.
 */
#include "x11_internal.h"

static X11Child_t gChildren[8]; // indexed by the engine's child index

// the slot of a child window, NULL when `w` is not one (events.c routes by it)
X11Child_t* X11Child_ByWindow(Window w)
{
	int i;
	for(i = 0; i < 8; i++)
		if(gChildren[i].used && w && gChildren[i].win == w)
			return &gChildren[i];
	return NULL;
}

// the picture presented last to the window, unscaled (Expose and `present`)
void X11Child_Show(X11Child_t* c)
{
	if(!c->win || !c->picture.pixels)
		return;
	X11_BlitSurface(&c->dc, 0, 0, c->picture.width, c->picture.height, &c->picture, 0, 0);
	XFlush(gX11Dpy);
}

/* `create`: slot `slot` (0 .. 7) unmapped with a w x h client area, its
 * outer origin at (x, y); NULL without a main window or for a bad slot */
static void* ChildCreate(void* user, int slot, const char* title, int x, int y, int w, int h)
{
	X11Child_t* c;
	XSetWindowAttributes attr;
	BGI_UNUSED(user);
	if(!gX11Dpy || !gX11Win || slot < 0 || slot >= 8)
		return NULL;
	c = &gChildren[slot];
	memset(c, 0, sizeof *c);
	c->clientW = w;
	c->clientH = h;
	memset(&attr, 0, sizeof attr);
	attr.background_pixel = BlackPixel(gX11Dpy, gX11Screen);
	attr.event_mask = KeyPressMask | KeyReleaseMask | ExposureMask | StructureNotifyMask | ButtonPressMask;
	attr.bit_gravity = NorthWestGravity;
	c->win = XCreateWindow(gX11Dpy, gX11Root, x + gX11FrameL, y + gX11FrameT, (unsigned)w, (unsigned)h, 0, gX11Depth, InputOutput, gX11Visual,
		CWBackPixel | CWEventMask | CWBitGravity, &attr);
	if(!c->win)
		return NULL;
	c->used = 1;
	c->dc.win = c->win;
	c->dc.child = slot;
	XSetTransientForHint(gX11Dpy, c->win, gX11Win); // owned pop-up: stays above the main window
	XSetWMProtocols(gX11Dpy, c->win, &gX11WmDelete, 1);
	{
		XSizeHints* hints = XAllocSizeHints();
		if(hints)
		{
			hints->flags = PMinSize | PMaxSize | PWinGravity;
			hints->min_width = hints->max_width = w;
			hints->min_height = hints->max_height = h;
			hints->win_gravity = NorthWestGravity;
			XSetWMNormalHints(gX11Dpy, c->win, hints);
			XFree(hints);
		}
	}
	X11_SetTitle(c->win, title);
	XFlush(gX11Dpy);
	return c;
}

// `close`: the engine's own close (SendMessage(WM_CLOSE)), the only way the player sees a child window go
static void ChildClose(void* user, void* win)
{
	X11Child_t* c = (X11Child_t*)win;
	BGI_UNUSED(user);
	if(c->win)
	{
		XDestroyWindow(gX11Dpy, c->win);
		XFlush(gX11Dpy);
	}
	memset(c, 0, sizeof *c);
}

// `show`: map or unmap the window
static void ChildShow(void* user, void* win, int show)
{
	X11Child_t* c = (X11Child_t*)win;
	BGI_UNUSED(user);
	if(!c->win)
		return;
	if(show)
		XMapWindow(gX11Dpy, c->win); // SW_SHOWNA: no activation; the window manager may still focus it
	else
		XUnmapWindow(gX11Dpy, c->win);
	c->shown = show != 0;
	XFlush(gX11Dpy);
}

static void ChildSetTitle(void* user, void* win, const char* title)
{
	X11Child_t* c = (X11Child_t*)win;
	BGI_UNUSED(user);
	if(c->win)
		X11_SetTitle(c->win, title);
}

// `move`: MoveWindow(.., repaint) to the outer origin (x, y); the size stays
static void ChildMove(void* user, void* win, int x, int y)
{
	X11Child_t* c = (X11Child_t*)win;
	BGI_UNUSED(user);
	if(!c->win)
		return;
	XMoveWindow(gX11Dpy, c->win, x, y);
	X11Child_Show(c);
}

// `getPos`: GetWindowRect's origin: the client origin on the root window less the frame estimate; (0, 0) without a window
static void ChildGetPos(void* user, void* win, int* x, int* y)
{
	X11Child_t* c = (X11Child_t*)win;
	Window child;
	int rx = 0, ry = 0;
	BGI_UNUSED(user);
	*x = *y = 0;
	if(!c->win)
		return;
	XTranslateCoordinates(gX11Dpy, c->win, gX11Root, 0, 0, &rx, &ry, &child);
	*x = rx - gX11FrameL;
	*y = ry - gX11FrameT;
}

// `present`: the window's new picture, shown at once and kept for the repaints
static void ChildPresent(void* user, void* win, const uint8_t* pixels, int pitch, int w, int h, int bpp)
{
	X11Child_t* c = (X11Child_t*)win;
	BGI_UNUSED(user);
	c->picture.pixels = (void*)pixels; // only read
	c->picture.pitch = pitch;
	c->picture.width = w;
	c->picture.height = h;
	c->picture.bpp = bpp;
	X11Child_Show(c);
}

static void ChildSetCopyText(void* user, void* win, const char* text)
{
	BGI_UNUSED(user);
	((X11Child_t*)win)->copyText = text;
}

static const OpenBGI_ChildWindows_t kChildWindows = {sizeof(OpenBGI_ChildWindows_t), ChildCreate, ChildClose, ChildShow, ChildSetTitle, ChildMove, ChildGetPos, ChildPresent,
	ChildSetCopyText};

// the transient windows as the host's child windows
const OpenBGI_ChildWindows_t* OS_ChildWindows(void)
{
	return &kChildWindows;
}
