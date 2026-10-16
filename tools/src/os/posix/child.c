/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * child.c - the pop-up child windows of the POSIX back end ("B0 1x" of the
 *           scripts: small top-level windows the engine draws into like
 *           the main one) (inc/bgi/os.h)
 *
 * Each of the eight slots is a transient top-level window of fixed size
 * owned by the main window (so the window manager keeps it above).  The
 * engine speaks of outer sizes and positions: the main window's frame
 * extents stand in for the child's own, which are not known before it is
 * mapped.  The handle the engine gets is the slot itself.
 */
#include "x11_internal.h"

static X11Child_t gChildren[8]; // indexed by the engine's child index

// the OsChild_t handle is the slot
static X11Child_t* ChildBySlot(OsChild_t* c)
{
	return (X11Child_t*)c;
}

// the slot of a child window, NULL when `w` is not one (events.c routes by it)
X11Child_t* X11Child_ByWindow(Window w)
{
	int i;
	for(i = 0; i < 8; i++)
		if(gChildren[i].used && gChildren[i].win == w)
			return &gChildren[i];
	return NULL;
}

/* create slot `index` (0 .. 7) unmapped at the outer origin (x, y) with
 * the outer size outerW x outerH; NULL without a main window, for a bad
 * index or a slot in use */
OsChild_t* OS_ChildCreate(int index, const char* title, int x, int y, int outerW, int outerH)
{
	X11Child_t* c;
	XSetWindowAttributes attr;
	if(!gX11Dpy || !gX11Win || index < 0 || index >= 8 || gChildren[index].used)
		return NULL;
	c = &gChildren[index];
	memset(c, 0, sizeof *c);
	c->clientW = outerW - gX11FrameL - gX11FrameR; // the main window's frame as the estimate of this one's
	c->clientH = outerH - gX11FrameT - gX11FrameB;
	if(c->clientW < 1)
		c->clientW = 1;
	if(c->clientH < 1)
		c->clientH = 1;
	memset(&attr, 0, sizeof attr);
	attr.background_pixel = BlackPixel(gX11Dpy, gX11Screen);
	attr.event_mask = KeyPressMask | KeyReleaseMask | ExposureMask | StructureNotifyMask | ButtonPressMask;
	attr.bit_gravity = NorthWestGravity;
	c->win = XCreateWindow(gX11Dpy, gX11Root, x + gX11FrameL, y + gX11FrameT, (unsigned)c->clientW, (unsigned)c->clientH, 0,
		gX11Depth, InputOutput, gX11Visual, CWBackPixel | CWEventMask | CWBitGravity, &attr);
	if(!c->win)
		return NULL;
	c->used = 1;
	c->dc.win = c->win;
	c->dc.child = index;
	XSetTransientForHint(gX11Dpy, c->win, gX11Win); // owned pop-up: stays above the main window
	XSetWMProtocols(gX11Dpy, c->win, &gX11WmDelete, 1);
	{
		XSizeHints* hints = XAllocSizeHints();
		if(hints)
		{
			hints->flags = PMinSize | PMaxSize | PWinGravity;
			hints->min_width = hints->max_width = c->clientW;
			hints->min_height = hints->max_height = c->clientH;
			hints->win_gravity = NorthWestGravity;
			XSetWMNormalHints(gX11Dpy, c->win, hints);
			XFree(hints);
		}
	}
	X11_SetTitle(c->win, title);
	XFlush(gX11Dpy);
	return c;
}

// destroy the window, free the slot and tell the engine (child_destroyed, as WM_DESTROY would)
static void ChildDestroy(X11Child_t* c)
{
	int index = c->dc.child;
	if(!c->used)
		return;
	XDestroyWindow(gX11Dpy, c->win);
	XFlush(gX11Dpy);
	c->used = 0;
	c->win = 0;
	if(gX11Handlers.child_destroyed)
		gX11Handlers.child_destroyed(index);
}

// the engine's own close (SendMessage(WM_CLOSE)): the only way a child window goes
void OS_ChildClose(OsChild_t* c)
{
	X11Child_t* w = ChildBySlot(c);
	if(w && w->used)
	{
		w->closing = 1;
		ChildDestroy(w);
	}
}

// map or unmap the window
void OS_ChildShow(OsChild_t* c, int show)
{
	X11Child_t* w = ChildBySlot(c);
	if(!w || !w->used)
		return;
	if(show)
		XMapWindow(gX11Dpy, w->win); // SW_SHOWNA: no activation; the window manager may still focus it
	else
		XUnmapWindow(gX11Dpy, w->win);
	w->shown = show != 0;
	XFlush(gX11Dpy);
}

void OS_ChildSetTitle(OsChild_t* c, const char* title)
{
	X11Child_t* w = ChildBySlot(c);
	if(w && w->used)
		X11_SetTitle(w->win, title);
}

// MoveWindow(.., repaint): the new outer rectangle (frame estimate as in OS_ChildCreate), then a repaint
void OS_ChildMove(OsChild_t* c, int x, int y, int outerW, int outerH)
{
	X11Child_t* w = ChildBySlot(c);
	if(!w || !w->used)
		return;
	w->clientW = outerW - gX11FrameL - gX11FrameR;
	w->clientH = outerH - gX11FrameT - gX11FrameB;
	if(w->clientW < 1)
		w->clientW = 1;
	if(w->clientH < 1)
		w->clientH = 1;
	{
		XSizeHints* hints = XAllocSizeHints();
		if(hints)
		{
			hints->flags = PMinSize | PMaxSize;
			hints->min_width = hints->max_width = w->clientW;
			hints->min_height = hints->max_height = w->clientH;
			XSetWMNormalHints(gX11Dpy, w->win, hints);
			XFree(hints);
		}
	}
	XMoveResizeWindow(gX11Dpy, w->win, x, y, (unsigned)w->clientW, (unsigned)w->clientH);
	XFlush(gX11Dpy);
	if(gX11Handlers.child_paint)
		gX11Handlers.child_paint(w->dc.child, &w->dc); // MoveWindow(.., repaint)
}

// GetWindowRect's origin: the client origin on the root window less the frame estimate; (0, 0) for a dead slot
void OS_ChildGetPos(OsChild_t* c, int* x, int* y)
{
	X11Child_t* w = ChildBySlot(c);
	Window child;
	int rx = 0, ry = 0;
	*x = *y = 0;
	if(!w || !w->used)
		return;
	XTranslateCoordinates(gX11Dpy, w->win, gX11Root, 0, 0, &rx, &ry, &child);
	*x = rx - gX11FrameL;
	*y = ry - gX11FrameT;
}

// the slot's X11Dc_t names the window for X11_BlitSurface; nothing is acquired, nothing to release
OsDc_t* OS_ChildGetDc(OsChild_t* c)
{
	X11Child_t* w = ChildBySlot(c);
	return w && w->used ? &w->dc : NULL;
}

void OS_ChildReleaseDc(OsChild_t* c, OsDc_t* dc)
{
}
