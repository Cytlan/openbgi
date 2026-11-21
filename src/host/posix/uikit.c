/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * uikit.c - the dialogs of the POSIX back end as windows of their own
 *           (ui_internal.h)
 *
 * Without a toolkit the back end shows the dialogs of the engine's dialog
 * toolkit (src/include/bgi/ui.h) itself: a top-level window painted from the
 * dialog's buffer, run as a modal loop (X11_RunModal) that keeps
 * repainting the engine's windows but discards their input.  The window's
 * keys, typed text (through the input method when there is one, so
 * Japanese can be entered with an IME) and pointer go to the dialog.
 */
#include "ui_internal.h"

#include "host/os_present.h"

#include <stdlib.h>

// what the modal loop's handler works on
typedef struct X11Ui
{
	UiDialog_t* d;
	Window win;       // the dialog's top-level window
	XIC ic;           // the input context, NULL without an input method or fields
	double scale;     // the window is larger than the dialog's picture by this: the desktop's scale
	int w, h;         // the window's client area
	OsPresent_t* big; // makes the larger picture; NULL at a scale of 1
} X11Ui_t;

void X11Ui_Begin(UiDialog_t* d, UiFont_t* font)
{
	memset(d, 0, sizeof *d);
	OsPosix_UiFont(font, OsPosix_FontUi(UI_FONT_H));
	d->font = font;
}

// paint the whole dialog and put it on the window, enlarged where the desktop has a scale
static void Paint(X11Ui_t* x)
{
	const uint32_t* pic;
	Ui_Draw(x->d);
	if(!x->big)
	{
		X11_PutArgb(x->win, 0, 0, x->d->w, x->d->h, x->d->pix, x->d->w);
		return;
	}
	OsPresent_Changed(x->big, 0, -1);
	pic = OsPresent_Draw(x->big, (const uint8_t*)x->d->pix, x->d->w, x->d->h, x->d->w * 4, 32, NULL);
	if(pic)
		X11_PutArgb(x->win, 0, 0, x->w, x->h, pic, x->w);
}

// a place in the window as a place in the dialog's picture
static int Unscaled(const X11Ui_t* x, int v)
{
	return x->big ? (int)(v / x->scale) : v;
}

// a key press: the dialog's keys, then the text it typed when a field may take it
static void Key(X11Ui_t* x, XKeyEvent* ev)
{
	int sys, vk = X11_EventVk(ev, &sys);
	int r = Ui_Key(x->d, vk, (ev->state & ShiftMask) != 0, (ev->state & ControlMask) != 0, (ev->state & Mod1Mask) != 0);
	if(r == UI_KEY_TYPE)
	{
		char utf8[64];
		if(X11_EventUtf8(ev, x->ic, utf8, (int)sizeof utf8) <= 0)
			utf8[0] = 0;
		Ui_Type(x->d, vk, utf8);
	}
}

// the dialog window's events for X11_RunModal; 0 once a button ended the dialog
static int Handler(XEvent* ev, void* ctx)
{
	X11Ui_t* x = (X11Ui_t*)ctx;
	UiDialog_t* d = x->d;
	switch(ev->type)
	{
		case Expose:
			if(ev->xexpose.count == 0)
				Paint(x);
			break;
		case KeyPress:
			if(x->ic && XFilterEvent(ev, None))
				break; // the input method took it (composition in progress)
			Key(x, &ev->xkey);
			break;
		case ButtonPress: // a left click, the wheel over a list
			if(ev->xbutton.button == Button1)
				Ui_Press(d, Unscaled(x, ev->xbutton.x), Unscaled(x, ev->xbutton.y), (uint32_t)ev->xbutton.time);
			else if(ev->xbutton.button == Button4 || ev->xbutton.button == Button5)
				Ui_Wheel(d, Unscaled(x, ev->xbutton.x), Unscaled(x, ev->xbutton.y), ev->xbutton.button == Button4 ? 1 : -1);
			break;
		case ButtonRelease:
			if(ev->xbutton.button == Button1)
				Ui_Release(d, Unscaled(x, ev->xbutton.x), Unscaled(x, ev->xbutton.y));
			break;
		case ClientMessage: // the close box acts as the cancel button; nothing without one
			if(ev->xclient.message_type == gX11WmProtocols && (Atom)ev->xclient.data.l[0] == gX11WmDelete)
				Ui_Close(d);
			break;
		case FocusIn:
			if(x->ic)
				XSetICFocus(x->ic);
			break;
		case FocusOut:
			if(x->ic)
				XUnsetICFocus(x->ic);
			break;
		default:
			break;
	}
	if(d->complaint[0] && !d->done)
	{ // a button handler refused: its message in a box over the dialog
		char text[sizeof d->complaint], caption[sizeof d->complaintCaption];
		memcpy(text, d->complaint, sizeof text);
		memcpy(caption, d->complaintCaption, sizeof caption);
		d->complaint[0] = 0;
		Paint(x);
		OS_MessageBox(text, caption, d->complaintFlags);
	}
	if(d->redraw && !d->done)
	{
		d->redraw = 0;
		Paint(x);
	}
	return !d->done;
}

int X11Ui_Run(UiDialog_t* d)
{
	X11Ui_t x;
	XSetWindowAttributes attr;
	XSizeHints* hints;
	int ax = 0, ay = 0, aw = 0, ah = 0, px, py;
	struct FtFont* font = d->font ? (struct FtFont*)d->font->ctx : NULL;
#ifndef BGI_HAIKU
	Atom typeDialog = XInternAtom(gX11Dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	Atom typeAtom = XInternAtom(gX11Dpy, "_NET_WM_WINDOW_TYPE", False);
#endif

	if(!font || !Ui_Alloc(d))
	{
		Ui_Free(d);
		OsPosix_FontClose(font);
		return d->escButton >= 0 ? d->buttons[d->escButton].id : 0;
	}
	memset(&x, 0, sizeof x);
	x.d = d;
	/* the window in the middle of what its screen has room for, and larger
	 * than the dialog's picture by the desktop's scale: the picture is
	 * enlarged as a whole, its letters with it */
	x.scale = OS_WindowFits() ? OsCommon_Dpi() / 96.0 : 1.0;
	x.w = (int)(d->w * x.scale + 0.5);
	x.h = (int)(d->h * x.scale + 0.5);
	if((x.w != d->w || x.h != d->h) && (x.big = OsPresent_New()) != NULL)
		OsPresent_Setup(x.big, x.w, x.h, OS_PRESENT_STRETCH, OS_PRESENT_BICUBIC);
	if(!x.big)
	{
		x.scale = 1.0;
		x.w = d->w;
		x.h = d->h;
	}
	OS_WindowWorkArea(&ax, &ay, &aw, &ah);
	px = ax + (aw - x.w) / 2;
	py = ay + (ah - x.h) / 2;
	memset(&attr, 0, sizeof attr);
	attr.background_pixel = UI_FACE;
	attr.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | ExposureMask |
		StructureNotifyMask | FocusChangeMask;
	x.win = XCreateWindow(gX11Dpy, gX11Root, px, py, (unsigned)x.w, (unsigned)x.h, 0,
		gX11Depth, InputOutput, gX11Visual, CWBackPixel | CWEventMask, &attr);
	hints = XAllocSizeHints();
	if(hints)
	{
		hints->flags = PMinSize | PMaxSize | PPosition;
		hints->min_width = hints->max_width = x.w;
		hints->min_height = hints->max_height = x.h;
		hints->x = px;
		hints->y = py;
		XSetWMNormalHints(gX11Dpy, x.win, hints);
		XFree(hints);
	}
#ifndef BGI_HAIKU // Xlibe knows no dialog type (it says so on stderr) and gives the window a title tab anyway
	XChangeProperty(gX11Dpy, x.win, typeAtom, XA_ATOM, 32, PropModeReplace, (unsigned char*)&typeDialog, 1);
#endif
	XSetWMProtocols(gX11Dpy, x.win, &gX11WmDelete, 1);
	X11_SetTitle(x.win, d->title);
	if(gX11Im && d->nFields) // an input context only when there is something to type into
		x.ic = XCreateIC(gX11Im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, x.win, XNFocusWindow,
			x.win, NULL);
	XMapRaised(gX11Dpy, x.win);
	XFlush(gX11Dpy);
	Paint(&x);
	X11_RunModal(x.win, Handler, &x);
	if(x.ic)
		XDestroyIC(x.ic);
	XDestroyWindow(gX11Dpy, x.win);
	XFlush(gX11Dpy);
	OsPresent_Delete(x.big);
	OsPosix_FontClose(font);
	Ui_Free(d);
	return d->result;
}
