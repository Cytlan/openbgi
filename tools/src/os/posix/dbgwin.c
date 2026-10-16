/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dbgwin.c - the debugger's own window of the POSIX back end
 *            (OS_DebugWindowOpen and friends, inc/bgi/os.h): a second
 *            top-level window placed beside the game's that the debugger
 *            (src/dbg) draws its panes into and whose input it gets
 *            through the debug_* handlers
 */
#include "x11_internal.h"

static Window gDbgWin;   // 0 while there is none
static int gDbgW, gDbgH; // its client size in pixels

int X11_IsDebugWindow(Window w)
{
	return gDbgWin && w == gDbgWin;
}

/* open the window (w x h, fixed size, mapped at once) next to the main
 * window - to its right when the screen has room, else to its left, else
 * at (40, 40); 0 without a display or when it exists already */
int OS_DebugWindowOpen(int w, int h, const char* title)
{
	XSetWindowAttributes attr;
	XSizeHints* hints;
	int x = 40, y = 40;
	if(!X11_Open() || gDbgWin)
		return 0;
	if(gX11Win)
	{ // beside the game's window where the screen has room: to its right, else to its left
		XWindowAttributes a;
		Window child;
		int mx = 0, my = 0, sw = DisplayWidth(gX11Dpy, gX11Screen);
		if(XGetWindowAttributes(gX11Dpy, gX11Win, &a) &&
			XTranslateCoordinates(gX11Dpy, gX11Win, gX11Root, 0, 0, &mx, &my, &child))
		{
			if(mx + a.width + 8 + w <= sw)
			{
				x = mx + a.width + 8;
				y = my;
			}
			else if(mx - 8 - w >= 0)
			{
				x = mx - 8 - w;
				y = my;
			}
		}
	}
	memset(&attr, 0, sizeof attr);
	attr.background_pixel = BlackPixel(gX11Dpy, gX11Screen);
	attr.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
		ExposureMask | StructureNotifyMask;
	attr.bit_gravity = NorthWestGravity;
	gDbgWin = XCreateWindow(gX11Dpy, gX11Root, x, y, (unsigned)w, (unsigned)h, 0, gX11Depth, InputOutput, gX11Visual,
		CWBackPixel | CWEventMask | CWBitGravity, &attr);
	if(!gDbgWin)
		return 0;
	gDbgW = w;
	gDbgH = h;
	hints = XAllocSizeHints();
	if(hints)
	{
		hints->flags = PMinSize | PMaxSize | PPosition | USPosition;
		hints->min_width = hints->max_width = w;
		hints->min_height = hints->max_height = h;
		hints->x = x;
		hints->y = y;
		XSetWMNormalHints(gX11Dpy, gDbgWin, hints);
		XFree(hints);
	}
	XSetWMProtocols(gX11Dpy, gDbgWin, &gX11WmDelete, 1);
	{
		XClassHint* ch = XAllocClassHint();
		if(ch)
		{
			ch->res_name = (char*)"bgi-debug";
			ch->res_class = (char*)"BGI";
			XSetClassHint(gX11Dpy, gDbgWin, ch);
			XFree(ch);
		}
	}
	X11_SetTitle(gDbgWin, title);
	XMapRaised(gX11Dpy, gDbgWin);
	XFlush(gX11Dpy);
	return 1;
}

void OS_DebugWindowClose(void)
{
	if(!gDbgWin)
		return;
	XDestroyWindow(gX11Dpy, gDbgWin);
	XFlush(gX11Dpy);
	gDbgWin = 0;
}

int OS_DebugWindowAlive(void)
{
	return gDbgWin != 0;
}

// the surface at 1:1 from the window's top-left corner, cut to the window; rows copied straight when the layouts agree
void OS_DebugWindowPresent(const OsSurface_t* s)
{
	uint32_t* d;
	int x, y, w, h;
	if(!gDbgWin || !s->pixels)
		return;
	w = s->width < gDbgW ? s->width : gDbgW;
	h = s->height < gDbgH ? s->height : gDbgH;
	d = X11_ConvBuffer((size_t)w * h);
	if(!d)
		return;
	for(y = 0; y < h; y++)
	{
		uint32_t* o = d + (size_t)y * w;
		if(s->bpp == 32 && gX11VisRBits == 8 && gX11VisGBits == 8 && gX11VisBBits == 8 && gX11VisRShift == 16 && gX11VisGShift == 8 &&
			gX11VisBShift == 0)
			memcpy(o, (const uint8_t*)s->pixels + (ptrdiff_t)y * s->pitch, (size_t)w * 4);
		else
			for(x = 0; x < w; x++)
				o[x] = X11_PackPixel(X11_SurfacePixel(s, x, y));
	}
	X11_PutImage(gDbgWin, 0, 0, w, h, d);
}

/* the events of the debugger's window: keys with their characters, the
 * mouse, the close box.  The debugger's input is ASCII: the character of
 * a press is the printable Latin-1 byte XLookupString gives, 0 for any
 * other key. */
void X11_HandleDebugEvent(XEvent* ev)
{
	switch(ev->type)
	{
		case KeyPress:
		case KeyRelease:
		{
			int sys, vk = X11_EventVk(&ev->xkey, &sys);
			int ch = 0;
			if(ev->type == KeyPress)
			{
				char buf[16];
				KeySym ks = 0;
				int n = XLookupString(&ev->xkey, buf, sizeof buf - 1, &ks, NULL);
				if(n == 1 && (uint8_t)buf[0] >= 0x20 && (uint8_t)buf[0] < 0x7f)
					ch = (uint8_t)buf[0];
			}
			if(gX11Handlers.debug_key && (vk || ch))
				gX11Handlers.debug_key(vk, ev->type == KeyPress, ch);
			break;
		}
		case ButtonPress:
		case ButtonRelease:
			if(ev->xbutton.button == Button4 || ev->xbutton.button == Button5)
			{
				if(ev->type == ButtonPress && gX11Handlers.debug_wheel)
					gX11Handlers.debug_wheel(ev->xbutton.button == Button4 ? 120 : -120);
			}
			else if(gX11Handlers.debug_mouse)
			{
				int b = ev->xbutton.button == Button1 ? 0 : ev->xbutton.button == Button3 ? 1
					: ev->xbutton.button == Button2                                       ? 2
																						  : -1;
				if(b >= 0)
					gX11Handlers.debug_mouse(ev->xbutton.x, ev->xbutton.y, b, ev->type == ButtonPress);
				if(ev->type == ButtonPress && !X11_HasWindowManager())
					XSetInputFocus(gX11Dpy, gDbgWin, RevertToParent, CurrentTime); // click to focus, as the main window
			}
			break;
		case MapNotify:
			if(!X11_HasWindowManager()) // as the main window: a bare display hands the focus to nobody
				XSetInputFocus(gX11Dpy, gDbgWin, RevertToParent, CurrentTime);
			break;
		case MotionNotify:
			if(gX11Handlers.debug_mouse)
				gX11Handlers.debug_mouse(ev->xmotion.x, ev->xmotion.y, -1, 0);
			break;
		case ClientMessage:
			if(ev->xclient.message_type == gX11WmProtocols && (Atom)ev->xclient.data.l[0] == gX11WmDelete && gX11Handlers.debug_close)
				gX11Handlers.debug_close();
			break;
		case Expose:
			if(ev->xexpose.count == 0 && gX11Handlers.debug_mouse)
				gX11Handlers.debug_mouse(-1, -1, -1, 0); // a nudge: the debugger redraws on its next pass anyway
			break;
		default:
			break;
	}
}
