/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * xdnd.c - files dropped on the main window: the receiving side of the
 *          XDND protocol, version 5 (src/host/os.h, OS_DragAccept)
 *
 * While the engine accepts drops the window carries the property
 * XdndAware.  A program a file is dragged from then sends XdndEnter with
 * the kinds of data it offers, XdndPosition as the pointer moves (each
 * answered with XdndStatus: taken, as a copy, when a list of URIs is
 * among the kinds), and XdndDrop.  The list is fetched as the selection
 * XdndSelection in the form "text/uri-list" and arrives with a
 * SelectionNotify; every "file:" entry of it is raised as the host's
 * `dropFile`, by its path, in the list's order, and XdndFinished ends the
 * exchange.  Nothing is taken while a dialog is modal, and a list too
 * large to arrive in one piece is refused.
 */
#include "x11_internal.h"

static Atom aAware, aEnter, aPosition, aStatus, aLeave, aDrop, aFinished, aSelection, aTypeList, aActionCopy, aUriList, aData;
static int gAccept;    // OS_DragAccept
static Window gSource; // the window a drag over this one comes from, 0 for none
static int gOffered;   // .. and it offers a list of URIs

static void Atoms(void)
{
	if(aAware)
		return;
	aAware = XInternAtom(gX11Dpy, "XdndAware", False);
	aEnter = XInternAtom(gX11Dpy, "XdndEnter", False);
	aPosition = XInternAtom(gX11Dpy, "XdndPosition", False);
	aStatus = XInternAtom(gX11Dpy, "XdndStatus", False);
	aLeave = XInternAtom(gX11Dpy, "XdndLeave", False);
	aDrop = XInternAtom(gX11Dpy, "XdndDrop", False);
	aFinished = XInternAtom(gX11Dpy, "XdndFinished", False);
	aSelection = XInternAtom(gX11Dpy, "XdndSelection", False);
	aTypeList = XInternAtom(gX11Dpy, "XdndTypeList", False);
	aActionCopy = XInternAtom(gX11Dpy, "XdndActionCopy", False);
	aUriList = XInternAtom(gX11Dpy, "text/uri-list", False);
	aData = XInternAtom(gX11Dpy, "OPENBGI_DROP", False);
}

// the property that says the window takes drops, on it or off it (also for a window made after OS_DragAccept)
void X11_DndApply(void)
{
	if(!gX11Dpy || !gX11Win)
		return;
	Atoms();
	if(gAccept)
	{
		Atom version = 5;
		XChangeProperty(gX11Dpy, gX11Win, aAware, XA_ATOM, 32, PropModeReplace, (unsigned char*)&version, 1);
	}
	else
		XDeleteProperty(gX11Dpy, gX11Win, aAware);
}

// DragAcceptFiles: whether `dropFile` is raised
void OS_DragAccept(int accept)
{
	gAccept = accept != 0;
	X11_DndApply();
}

// the main window is gone: the next one takes no drops until it is told to
void X11_DndReset(void)
{
	gAccept = 0;
	gSource = 0;
	gOffered = 0;
}

static int Taking(void)
{
	return gAccept && gOffered && !gX11ModalDepth;
}

// a message of the protocol to the window the drag comes from
static void Send(Atom type, long a, long b)
{
	XEvent ev;
	if(!gSource)
		return;
	memset(&ev, 0, sizeof ev);
	ev.xclient.type = ClientMessage;
	ev.xclient.display = gX11Dpy;
	ev.xclient.window = gSource;
	ev.xclient.message_type = type;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = (long)gX11Win;
	ev.xclient.data.l[1] = a;
	ev.xclient.data.l[4] = type == aStatus ? b : 0;
	ev.xclient.data.l[2] = type == aFinished ? b : 0;
	XSendEvent(gX11Dpy, gSource, False, NoEventMask, &ev);
	XFlush(gX11Dpy);
}

// XdndEnter: who drags, and whether a list of URIs is among what is offered
static void Enter(const XClientMessageEvent* m)
{
	int i;
	gSource = (Window)m->data.l[0];
	gOffered = 0;
	if(m->data.l[1] & 1)
	{ // more than three kinds: the list is a property of the source's window
		Atom type = None;
		int format = 0;
		unsigned long count = 0, left = 0, k;
		unsigned char* data = NULL;
		if(XGetWindowProperty(gX11Dpy, gSource, aTypeList, 0, 1024, False, XA_ATOM, &type, &format, &count, &left, &data) == Success && data)
		{
			if(type == XA_ATOM && format == 32)
				for(k = 0; k < count; k++)
					if(((Atom*)data)[k] == aUriList)
						gOffered = 1;
			XFree(data);
		}
	}
	else
		for(i = 2; i <= 4; i++)
			if((Atom)m->data.l[i] == aUriList)
				gOffered = 1;
}

static int Hex(int c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	c |= 0x20;
	return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* One line of the list: "file://<host>/<path>" (or "file:/<path>") with
 * the bytes a URI cannot hold written "%xx".  The path is raised as a
 * dropped file; anything else - a comment, another scheme, a path that
 * does not fit - is passed over.  1 when a path was raised. */
static int DropUri(const char* s, size_t n)
{
	char path[0x1000];
	size_t i = 0, k = 0;
	if(n < 6 || strncmp(s, "file:", 5) != 0)
		return 0;
	i = 5;
	if(i + 1 < n && s[i] == '/' && s[i + 1] == '/')
	{ // an authority: the host's name, mostly none
		i += 2;
		while(i < n && s[i] != '/')
			i++;
	}
	if(i >= n || s[i] != '/')
		return 0;
	for(; i < n; i++)
	{
		int c = (unsigned char)s[i];
		if(c == '%' && i + 2 < n && Hex(s[i + 1]) >= 0 && Hex(s[i + 2]) >= 0)
		{
			c = Hex(s[i + 1]) * 16 + Hex(s[i + 2]);
			i += 2;
		}
		if(!c || k + 1 >= sizeof path)
			return 0;
		path[k++] = (char)c;
	}
	path[k] = 0;
	if(gX11Input.dropFile)
		gX11Input.dropFile(gX11Input.user, path);
	return 1;
}

// the list arrived (or was refused: no property): its files are raised; 1 when one was
static int Arrived(const XSelectionEvent* sel)
{
	Atom type = None;
	int format = 0, raised = 0;
	unsigned long count = 0, left = 0;
	unsigned char* data = NULL;
	size_t at = 0;
	if(sel->property == None)
		return 0;
	if(XGetWindowProperty(gX11Dpy, gX11Win, sel->property, 0, 1 << 20, True, AnyPropertyType, &type, &format, &count, &left, &data) != Success || !data)
		return 0;
	if(format == 8 && !left) // (one piece: an INCR transfer says so with another type and is refused)
		while(at < count)
		{
			size_t end = at;
			while(end < count && data[end] != '\r' && data[end] != '\n')
				end++;
			if(end > at && data[at] != '#')
				raised |= DropUri((const char*)data + at, end - at);
			at = end + 1;
		}
	XFree(data);
	return raised;
}

// an event of the protocol for the main window: 1 when it was one
int X11_DndEvent(XEvent* ev)
{
	if(!gX11Win || !aAware)
		return 0;
	if(ev->type == SelectionNotify && ev->xselection.selection == aSelection)
	{
		int ok = Arrived(&ev->xselection);
		Send(aFinished, ok, ok ? (long)aActionCopy : 0);
		gSource = 0;
		gOffered = 0;
		return 1;
	}
	if(ev->type != ClientMessage || ev->xclient.window != gX11Win || ev->xclient.format != 32)
		return 0;
	if(ev->xclient.message_type == aEnter)
		Enter(&ev->xclient);
	else if(ev->xclient.message_type == aPosition)
	{
		gSource = (Window)ev->xclient.data.l[0];
		Send(aStatus, Taking() ? 1 : 0, Taking() ? (long)aActionCopy : 0);
	}
	else if(ev->xclient.message_type == aLeave)
	{
		gSource = 0;
		gOffered = 0;
	}
	else if(ev->xclient.message_type == aDrop)
	{
		gSource = (Window)ev->xclient.data.l[0];
		if(Taking())
			XConvertSelection(gX11Dpy, aSelection, aUriList, aData, gX11Win, (Time)ev->xclient.data.l[2]);
		else
		{
			Send(aFinished, 0, 0);
			gSource = 0;
			gOffered = 0;
		}
	}
	else
		return 0;
	return 1;
}
