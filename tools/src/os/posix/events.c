/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * events.c - the event pump of the POSIX back end: X events turned into
 *            the engine's OsEventHandlers_t callbacks, the clipboard, and
 *            the modal loop of the dialogs (inc/bgi/os.h)
 *
 * OS_PumpMessages takes one X event per call and routes it by window:
 * the main window's events become the key, mouse, focus, size, paint and
 * close callbacks; a child window's and the debugger's go to their own
 * handlers (child.c, dbgwin.c).  Keys are tracked for OS_KeyState, X's
 * auto-repeat is turned into the `repeat` flag of key_down, and a second
 * press within 400 ms and 4 pixels becomes a double click.
 */
#include "x11_internal.h"

// ---- events ------------------------------------------------------------------------------------

void OS_SetEventHandlers(const OsEventHandlers_t* h)
{
	gX11Handlers = *h;
}

void OS_PostQuit(void)
{
	gX11QuitPosted = 1; // the next pump raises `quit`
}

// drag and drop is not implemented: nothing to accept
void OS_DragAccept(int accept)
{
}

// record a key or mouse button for OS_KeyState
static void SetKey(int vk, int down)
{
	if(vk > 0 && vk < 256)
		gX11Keys[vk] = (uint8_t)(down ? 1 : 0);
}

// X button number -> the engine's button index (0 L 1 R 2 M 3 X1 4 X2), -1 for the wheel and others
static int ButtonIndex(unsigned xbutton)
{
	switch(xbutton)
	{
		case Button1: return 0;
		case Button3: return 1;
		case Button2: return 2;
		case 8: return 3; // X1 / X2 ("back" / "forward")
		case 9: return 4;
		default: return -1;
	}
}

static const int kButtonVk[5] = {1, 2, 4, 5, 6}; // the virtual key of each button index (VK_LBUTTON ..)

static int gInputDebug = -1; // BGI_INPUT_DEBUG: log the key and button events to stderr (-1 = not yet looked up)

static int InputDebug(void)
{
	if(gInputDebug < 0)
		gInputDebug = getenv("BGI_INPUT_DEBUG") != NULL;
	return gInputDebug;
}

/* a key press or release of the main window: the text entry first when
 * it has the focus, then key_down (with the auto-repeat flag) / syskey
 * / key_up as Windows would deliver them */
static void MainKey(XKeyEvent* ev, int down)
{
	int sys, vk = X11_EventVk(ev, &sys);
	if(InputDebug())
		fprintf(stderr, "x11: key %s keycode %u vk 0x%02x%s\n", down ? "down" : "up", ev->keycode, vk, sys ? " (sys)" : "");
	if(!vk)
		return;
	if(down && X11Edit_Key(ev))
		return;
	SetKey(vk, down);
	if(down)
	{
		/* X repeats a held key as release + press pairs with the same time
		 * stamp; a press whose release is already queued is a repeat */
		int repeat = 0;
		if(XEventsQueued(gX11Dpy, QueuedAfterReading))
		{
			XEvent next;
			XPeekEvent(gX11Dpy, &next);
			if(next.type == KeyRelease && next.xkey.keycode == ev->keycode && next.xkey.time == ev->time)
				repeat = 1;
		}
		if(sys)
		{
			if(gX11Handlers.syskey)
				gX11Handlers.syskey(vk);
		}
		else if(gX11Handlers.key_down)
			gX11Handlers.key_down(vk, repeat);
	}
	else
	{
		// a repeat's release is swallowed
		if(XEventsQueued(gX11Dpy, QueuedAfterReading))
		{
			XEvent next;
			XPeekEvent(gX11Dpy, &next);
			if(next.type == KeyPress && next.xkey.keycode == ev->keycode && next.xkey.time == ev->time)
			{
				SetKey(vk, 1);
				return;
			}
		}
		if(sys)
		{
			if(vk == 0x79 && gX11Handlers.key_up)
				gX11Handlers.key_up(vk); // WM_SYSKEYUP: the original's window procedure only looks at F10
		}
		else if(gX11Handlers.key_up)
			gX11Handlers.key_up(vk);
	}
}

/* a mouse button press or release of the main window: the wheel becomes
 * mouse_wheel, the others record their virtual key, take the focus from
 * the text entry, detect a double click and raise mouse_button in mode
 * coordinates */
static void MainButton(XButtonEvent* ev, int down)
{
	int b = ButtonIndex(ev->button);
	int x = X11_FromWinX(ev->x), y = X11_FromWinY(ev->y);
	if(InputDebug())
		fprintf(stderr, "x11: button %u %s at %d,%d\n", ev->button, down ? "down" : "up", x, y);
	if(ev->button == Button4 || ev->button == Button5)
	{ // the wheel: one notch = WHEEL_DELTA
		if(down && gX11Handlers.mouse_wheel)
			gX11Handlers.mouse_wheel(ev->button == Button4 ? 120 : -120);
		return;
	}
	if(b < 0)
		return;
	SetKey(kButtonVk[b], down);
	if(down)
	{
		// a click takes the focus from the text entry (the original's button handlers call SetFocus)
		if(X11Edit_HasFocus())
			OS_EditFocus(0);
		// WM_?BUTTONDBLCLK: raised in front of the ordinary press, for the left and right buttons only
		if(b <= 1 && gX11LastClickBtn == b && ev->time - gX11LastClickTime < 400 && abs(x - gX11LastClickX) < 4 &&
			abs(y - gX11LastClickY) < 4)
		{
			if(gX11Handlers.double_click)
				gX11Handlers.double_click(b);
			gX11LastClickBtn = -1;
		}
		else
		{
			gX11LastClickBtn = b;
			gX11LastClickTime = ev->time;
			gX11LastClickX = x;
			gX11LastClickY = y;
		}
	}
	if(gX11Handlers.mouse_button)
		gX11Handlers.mouse_button(b, down, x, y);
}

/* an event of the main window.  Input is dropped while a dialog is modal
 * (gX11ModalDepth); the pointer position is noted from every pointer
 * event for OS_CursorGetPos. */
void X11_HandleMainEvent(XEvent* ev)
{
	switch(ev->type)
	{
		case Expose:
			if(ev->xexpose.count == 0)
			{ // the last of a batch: repaint everything once
				X11_FillLetterbox();
				if(gX11Handlers.paint)
					gX11Handlers.paint();
				if(X11Edit_Visible())
					X11Edit_Present();
			}
			break;
		case KeyPress:
			if(!gX11ModalDepth)
				MainKey(&ev->xkey, 1);
			break;
		case KeyRelease:
			if(!gX11ModalDepth)
				MainKey(&ev->xkey, 0);
			break;
		case ButtonPress:
			X11_NotePointer(ev->xbutton.x_root, ev->xbutton.y_root);
			if(!gX11ModalDepth)
				MainButton(&ev->xbutton, 1);
			break;
		case ButtonRelease:
			X11_NotePointer(ev->xbutton.x_root, ev->xbutton.y_root);
			if(!gX11ModalDepth)
				MainButton(&ev->xbutton, 0);
			break;
		case MotionNotify:
			X11_NotePointer(ev->xmotion.x_root, ev->xmotion.y_root);
			if(gX11Handlers.mouse_move && !gX11ModalDepth)
				gX11Handlers.mouse_move(X11_FromWinX(ev->xmotion.x), X11_FromWinY(ev->xmotion.y));
			break;
		case EnterNotify:
		case LeaveNotify:
			X11_NotePointer(ev->xcrossing.x_root, ev->xcrossing.y_root);
			break;
		case FocusIn:
			gX11Active = 1;
			if(gX11Xic && X11Edit_HasFocus())
				XSetICFocus(gX11Xic);
			if(gX11Handlers.activate)
				gX11Handlers.activate(1);
			break;
		case FocusOut:
			gX11Active = 0;
			memset(gX11Keys, 0, sizeof gX11Keys); // nothing is held across a focus loss
			if(gX11Handlers.activate)
				gX11Handlers.activate(0);
			break;
		case MapNotify:
			X11_UpdateClientOrigin();
			/* Without a window manager nothing hands the keyboard focus to a
			 * new window (the X default is "wherever the pointer is", which
			 * sends no FocusIn when the pointer already sits inside), so a
			 * bare display - the headless test runs - gets it set here.
			 * A window manager (_NET_SUPPORTING_WM_CHECK on the root) does
			 * its own focus handling. */
			if(ev->xmap.window == gX11Win && !X11_HasWindowManager())
				XSetInputFocus(gX11Dpy, gX11Win, RevertToParent, CurrentTime);
			if(gX11Minimized)
			{
				gX11Minimized = 0;
				if(gX11Handlers.sized)
					gX11Handlers.sized(0);
			}
			break;
		case UnmapNotify:
			if(!gX11HiddenByUs && !gX11Minimized)
			{ // iconified by the user or the window manager (an unmap of OS_WindowShow(0) is not a minimise)
				gX11Minimized = 1;
				if(gX11Handlers.sized)
					gX11Handlers.sized(1);
			}
			break;
		case ConfigureNotify: // moved or resized: the cached geometry is stale
			X11_ReadFrameExtents();
			X11_UpdateClientOrigin();
			break;
		case PropertyNotify: // the window manager set or changed the frame extents
			if(ev->xproperty.atom == gX11AtomNetFrameExtents)
				X11_ReadFrameExtents();
			break;
		case ClientMessage: // the close box (WM_DELETE_WINDOW)
			if(ev->xclient.message_type == gX11WmProtocols && (Atom)ev->xclient.data.l[0] == gX11WmDelete)
			{
				if(!gX11ModalDepth && gX11Handlers.close_request)
					gX11Handlers.close_request();
			}
			break;
		default:
			break;
	}
}

// an event of a child window: repaints are its own, keys go to the main window, the close box is refused
void X11_HandleChildEvent(X11Child_t* c, XEvent* ev)
{
	switch(ev->type)
	{
		case Expose:
			if(ev->xexpose.count == 0 && gX11Handlers.child_paint)
				gX11Handlers.child_paint(c->dc.child, &c->dc);
			break;
		case KeyPress: // forwarded to the main window
		case KeyRelease:
			if(!gX11ModalDepth)
				MainKey(&ev->xkey, ev->type == KeyPress);
			break;
		case ClientMessage:
			// WM_CLOSE is refused unless the engine closes the window itself: the close box does nothing
			break;
		case DestroyNotify: // destroyed from outside (OS_ChildClose frees the slot itself)
			if(c->used)
			{
				c->used = 0;
				c->win = 0;
				if(gX11Handlers.child_destroyed)
					gX11Handlers.child_destroyed(c->dc.child);
			}
			break;
		default:
			break;
	}
}

// ---- the clipboard ("7E" of 1.69 build 472 on) --------------------------------

static char* gClipText;                  // UTF-8, owned; NULL when another client owns CLIPBOARD
static Atom aClipboard, aTargets, aText; // interned on the first use

/* SetClipboardData(CF_TEXT): take ownership of CLIPBOARD and answer the
 * requests for it from the event loop (AnswerSelection); 1 when the
 * selection was taken */
int OS_ClipboardSetText(const char* sjis)
{
	char utf[0x2000];
	if(!gX11Dpy || !gX11Win)
		return 0;
	OsPosix_SjisToUtf8(sjis, utf, sizeof utf);
	free(gClipText);
	gClipText = strdup(utf);
	if(!aClipboard)
	{
		aClipboard = XInternAtom(gX11Dpy, "CLIPBOARD", False);
		aTargets = XInternAtom(gX11Dpy, "TARGETS", False);
		aText = XInternAtom(gX11Dpy, "TEXT", False);
	}
	XSetSelectionOwner(gX11Dpy, aClipboard, gX11Win, CurrentTime);
	return XGetSelectionOwner(gX11Dpy, aClipboard) == gX11Win;
}

/* another client asks for the clipboard: hand it the text as UTF8_STRING,
 * TEXT or STRING (or the list of those for TARGETS), or refuse with
 * property None when the target is unknown or the text is gone */
static void AnswerSelection(XSelectionRequestEvent* req)
{
	XEvent reply;
	Atom target = req->target;
	memset(&reply, 0, sizeof reply);
	reply.xselection.type = SelectionNotify;
	reply.xselection.display = req->display;
	reply.xselection.requestor = req->requestor;
	reply.xselection.selection = req->selection;
	reply.xselection.target = target;
	reply.xselection.time = req->time;
	reply.xselection.property = None;
	if(gClipText && req->property != None)
	{
		if(target == aTargets)
		{
			Atom list[4] = {aTargets, gX11Utf8String, aText, XA_STRING};
			XChangeProperty(req->display, req->requestor, req->property, XA_ATOM, 32, PropModeReplace,
				(unsigned char*)list, 4);
			reply.xselection.property = req->property;
		}
		else if(target == gX11Utf8String || target == aText || target == XA_STRING)
		{
			XChangeProperty(req->display, req->requestor, req->property, target == XA_STRING ? XA_STRING : gX11Utf8String,
				8, PropModeReplace, (unsigned char*)gClipText, (int)strlen(gClipText));
			reply.xselection.property = req->property;
		}
	}
	XSendEvent(req->display, req->requestor, False, 0, &reply);
	XFlush(req->display);
}

// ---- dispatch and the pump -----------------------------------------------------------------------

/* route one event to its window's handler (the debugger's window, the
 * input method's filter, the clipboard requests, the main window, a
 * child); returns 0 for an event of a window nobody owns */
static int DispatchEvent(XEvent* ev)
{
	X11Child_t* c;
	if(X11_IsDebugWindow(ev->xany.window))
	{
		X11_HandleDebugEvent(ev);
		return 1;
	}
	if(gX11Xic && XFilterEvent(ev, None))
		return 1; // consumed by the input method
	if(ev->type == SelectionRequest && ev->xselectionrequest.selection == aClipboard)
	{
		AnswerSelection(&ev->xselectionrequest);
		return 1;
	}
	if(ev->type == SelectionClear && ev->xselectionclear.selection == aClipboard)
	{
		free(gClipText);
		gClipText = NULL;
		return 1;
	}
	if(ev->xany.window == gX11Win && gX11Win)
	{
		X11_HandleMainEvent(ev);
		return 1;
	}
	if((c = X11Child_ByWindow(ev->xany.window)) != NULL)
	{
		X11_HandleChildEvent(c, ev);
		return 1;
	}
	return 0;
}

/* one message: a pending quit (OS_PostQuit, a signal) or close request
 * (OS_WindowClose) first - they are the back end's "posted messages" -
 * then one X event when there is one; 1 when something was delivered.
 * `quit` may not return (the engine throws out of its loop). */
int OS_PumpMessages(void)
{
	XEvent ev;
	if(gX11QuitPosted)
	{
		gX11QuitPosted = 0;
		if(gX11Handlers.quit)
			gX11Handlers.quit();
		return 1;
	}
	if(gX11CloseRequested)
	{
		gX11CloseRequested = 0;
		if(gX11Handlers.close_request)
			gX11Handlers.close_request();
		return 1;
	}
	if(!gX11Dpy)
		return 0;
	if(XPending(gX11Dpy) <= 0)
		return 0;
	XNextEvent(gX11Dpy, &ev);
	DispatchEvent(&ev);
	return 1;
}

/* the modal loop of a dialog: blocks on XNextEvent, hands the dialog's
 * own events to `handler` until it returns 0, and keeps dispatching the
 * other windows' events (so they repaint) with their input ignored
 * through gX11ModalDepth; nests */
void X11_RunModal(Window modal, int (*handler)(XEvent* ev, void* ctx), void* ctx)
{
	gX11ModalDepth++;
	for(;;)
	{
		XEvent ev;
		XNextEvent(gX11Dpy, &ev);
		if(ev.xany.window == modal)
		{
			if(!handler(&ev, ctx))
				break;
			continue;
		}
		if(gX11Xic && XFilterEvent(&ev, None))
			continue;
		DispatchEvent(&ev); // repaints of the engine's windows; their input is ignored while modal
	}
	gX11ModalDepth--;
}
