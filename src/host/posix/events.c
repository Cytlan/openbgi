/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * events.c - the event pump of the POSIX back end: X events turned into
 *            the host's and the engine's callbacks, the clipboard, and
 *            the modal loop of the dialogs with its tick (src/host/os.h)
 *
 * OS_PumpMessages takes one X event per call and routes it by window:
 * the main window's input (keys, buttons, the wheel, motion, the focus),
 * its repaints and its minimise, restore and close go to the host's
 * handlers (OS_SetHostHandlers); a child window's and the debugger's
 * events to their own code (child.c, dbgwin.c).  X's
 * auto-repeat is turned into OPENBGI_KEY_REPEAT, and a second press
 * within 400 ms and 4 pixels becomes a double click.
 */
#include "x11_internal.h"
#include "bgi/sjis.h"

// ---- events ------------------------------------------------------------------------------------

void OS_SetHostHandlers(const OsHostHandlers_t* h)
{
	if(h)
		gX11Input = *h;
	else
		memset(&gX11Input, 0, sizeof gX11Input);
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

static int gInputDebug = -1; // BGI_INPUT_DEBUG: log the key and button events to stderr (-1 = not yet looked up)

static int InputDebug(void)
{
	if(gInputDebug < 0)
		gInputDebug = getenv("BGI_INPUT_DEBUG") != NULL;
	return gInputDebug;
}

static unsigned gRepeatKey; // the keycode whose auto-repeat release was swallowed (0: none): its next press is a repeat

// the text a key press typed to the host's `text`, after the key itself (as WM_CHAR follows WM_KEYDOWN)
static void MainText(XKeyEvent* ev)
{
	char utf8[64];
	if(gX11Input.text && X11_EventUtf8(ev, gX11Xic, utf8, (int)sizeof utf8) > 0)
		gX11Input.text(gX11Input.user, utf8);
}

/* a key press or release of the main window: the host's `key` as Windows
 * would deliver it (OPENBGI_KEY_SYSTEM for WM_SYSKEYDOWN / UP,
 * OPENBGI_KEY_REPEAT for the auto-repeat), and the text it typed to
 * `text` */
static void MainKey(XKeyEvent* ev, int down)
{
	int sys, vk = X11_EventVk(ev, &sys), flags;
	if(!vk)
	{
		if(InputDebug())
			fprintf(stderr, "x11: key %s keycode %u: no virtual key\n", down ? "down" : "up", ev->keycode);
		if(down)
			MainText(ev); // what the input method composed comes without a key
		return;
	}
	flags = sys ? OPENBGI_KEY_SYSTEM : 0;
	if(!down && XEventsQueued(gX11Dpy, QueuedAfterReading))
	{
		/* X repeats a held key as release + press pairs with the same time
		 * stamp: such a release is swallowed, and the press after it is a
		 * repeat */
		XEvent next;
		XPeekEvent(gX11Dpy, &next);
		if(next.type == KeyPress && next.xkey.keycode == ev->keycode && next.xkey.time == ev->time)
		{
			gRepeatKey = ev->keycode;
			return;
		}
	}
	if(down && ev->keycode == gRepeatKey)
		flags |= OPENBGI_KEY_REPEAT;
	gRepeatKey = 0;
	if(InputDebug())
		fprintf(stderr, "x11: key %s keycode %u vk 0x%02x%s%s\n", down ? "down" : "up", ev->keycode, vk, sys ? " (sys)" : "",
			flags & OPENBGI_KEY_REPEAT ? " (repeat)" : "");
	if(gX11Input.key)
		gX11Input.key(gX11Input.user, vk, down, flags);
	if(down)
		MainText(ev);
}

/* a mouse button press or release of the main window: the wheel becomes
 * the host's `wheel`, the others take the focus from the text entry,
 * detect a double click and raise `button` in the picture's pixels */
static void MainButton(XButtonEvent* ev, int down)
{
	int b = ButtonIndex(ev->button), flags = 0;
	int x = X11_FromWinX(ev->x), y = X11_FromWinY(ev->y);
	if(InputDebug())
		fprintf(stderr, "x11: button %u %s at %d,%d\n", ev->button, down ? "down" : "up", x, y);
	if(ev->button == Button4 || ev->button == Button5)
	{ // the wheel: one notch = WHEEL_DELTA
		if(down && gX11Input.wheel)
			gX11Input.wheel(gX11Input.user, ev->button == Button4 ? 120 : -120);
		return;
	}
	if(b < 0)
		return;
	if(down)
	{
		// WM_?BUTTONDBLCLK: the second press, for the left and right buttons only
		if(b <= 1 && gX11LastClickBtn == b && ev->time - gX11LastClickTime < 400 && abs(x - gX11LastClickX) < 4 &&
			abs(y - gX11LastClickY) < 4)
		{
			flags = OPENBGI_CLICK_DOUBLE;
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
	if(gX11Input.button)
		gX11Input.button(gX11Input.user, b, down, x, y, flags);
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
				if(gX11Input.paint)
					gX11Input.paint(gX11Input.user);
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
			if(gX11Input.pointer && !gX11ModalDepth)
				gX11Input.pointer(gX11Input.user, X11_FromWinX(ev->xmotion.x), X11_FromWinY(ev->xmotion.y));
			break;
		case EnterNotify:
		case LeaveNotify:
			X11_NotePointer(ev->xcrossing.x_root, ev->xcrossing.y_root);
			break;
		case FocusIn:
			gX11Active = 1;
			if(gX11Xic && gX11TextInput)
				XSetICFocus(gX11Xic);
			if(gX11Input.focus)
				gX11Input.focus(gX11Input.user, 1);
			break;
		case FocusOut:
			gX11Active = 0;
			if(gX11Input.focus)
				gX11Input.focus(gX11Input.user, 0);
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
				if(gX11Input.minimized)
					gX11Input.minimized(gX11Input.user, 0);
			}
			break;
		case UnmapNotify:
			if(!gX11HiddenByUs && !gX11Minimized)
			{ // iconified by the user or the window manager (an unmap of OS_WindowShow(0) is not a minimise)
				gX11Minimized = 1;
				if(gX11Input.minimized)
					gX11Input.minimized(gX11Input.user, 1);
			}
			break;
		case ConfigureNotify: // moved or resized: the cached geometry is stale
		{
			int frame;
			X11_UpdateClientOrigin();
			frame = X11_ReadFrameExtents();
			if(X11_ClientResized(ev->xconfigure.width, ev->xconfigure.height, ev->xconfigure.serial))
			{ // a window with a sizing border got another size: the engine hears of it, the picture is shown again
				if(gX11Input.resized)
					gX11Input.resized(gX11Input.user, ev->xconfigure.width, ev->xconfigure.height);
				if(gX11Input.paint)
					gX11Input.paint(gX11Input.user);
			}
			if(frame && gX11Input.screenChanged)
				gX11Input.screenChanged(gX11Input.user); // (a window fitted to the screen is fitted again)
			break;
		}
		case PropertyNotify: // the window manager set or changed the frame extents
			if(ev->xproperty.atom == gX11AtomNetFrameExtents && X11_ReadFrameExtents() && gX11Input.screenChanged)
				gX11Input.screenChanged(gX11Input.user); // (a window fitted to the screen is fitted again)
			break;
		case ClientMessage: // the close box (WM_DELETE_WINDOW)
			if(ev->xclient.message_type == gX11WmProtocols && (Atom)ev->xclient.data.l[0] == gX11WmDelete)
			{
				if(!gX11ModalDepth && gX11Input.close)
					gX11Input.close(gX11Input.user);
			}
			break;
		default:
			break;
	}
}

/* an event of a child window: repaints are its own (the picture presented
 * last), Ctrl+C copies its copy text, the other keys go to the main
 * window, the close box is refused */
void X11_HandleChildEvent(X11Child_t* c, XEvent* ev)
{
	switch(ev->type)
	{
		case Expose:
			if(ev->xexpose.count == 0)
				X11Child_Show(c);
			break;
		case KeyPress:
		{
			int sys;
			if(X11_EventVk(&ev->xkey, &sys) == 'C' && (ev->xkey.state & ControlMask))
			{ // Ctrl+C: the copy text, and the key goes no further
				if(c->copyText)
					OS_ClipboardSetText(c->copyText);
				break;
			}
		}
			// fall through
		case KeyRelease: // forwarded to the main window
			if(!gX11ModalDepth)
				MainKey(&ev->xkey, ev->type == KeyPress);
			break;
		case ClientMessage:
			// WM_CLOSE is refused unless the engine closes the window itself: the close box does nothing
			break;
		case DestroyNotify: // destroyed from outside: the slot stays the engine's until it closes it
			c->win = 0;
			c->dc.win = 0;
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
	OsCommon_SjisToUtf8(sjis, utf, sizeof utf);
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
	if(X11_DndEvent(ev))
		return 1; // a file dragged over the main window, or dropped on it (xdnd.c)
	if(ev->xany.window == gX11Root)
	{ // the root window's: the desktop's scale or its work area is another, or the screen's size
		if(gX11Input.screenChanged &&
			(ev->type == ConfigureNotify ||
				(ev->type == PropertyNotify && (ev->xproperty.atom == XA_RESOURCE_MANAGER || ev->xproperty.atom == gX11AtomNetWorkarea))))
			gX11Input.screenChanged(gX11Input.user);
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

/* one message: the quit a signal asked for first (the back end's posted
 * message), then one X event when there is one; 1 when something was
 * delivered */
int OS_PumpMessages(void)
{
	XEvent ev;
	if(gX11QuitPosted)
	{
		gX11QuitPosted = 0;
		if(gX11Input.quit)
			gX11Input.quit(gX11Input.user);
		return 1;
	}
	if(!gX11Dpy)
		return 0;
	if(XPending(gX11Dpy) <= 0)
	{
		X11_SurfaceTell(); // (once a round: the window's size or the picture's place may be another)
		return 0;
	}
	XNextEvent(gX11Dpy, &ev);
	DispatchEvent(&ev);
	return 1;
}

// ---- the modal loop -------------------------------------------------------------------------------

static void (*gModalTick)(void* user); // OS_SetModalTick's function and its argument
static void* gModalTickUser;

void OS_SetModalTick(void (*tick)(void* user), void* user)
{
	gModalTick = tick;
	gModalTickUser = user;
}

/* The next event of a modal loop into *ev.  With a modal tick set, the
 * wait for it is a look every 5 ms (XPending; Xlibe has no descriptor to
 * wait on), with the tick every 20 ms meanwhile and between events that
 * come quicker; `lastTick` is the loop's time of the last one. */
static void ModalNextEvent(XEvent* ev, uint32_t* lastTick)
{
	while(gModalTick)
	{
		uint32_t now = OS_TicksMs();
		if(now - *lastTick >= 20)
		{
			*lastTick = now;
			gModalTick(gModalTickUser);
		}
		if(XPending(gX11Dpy))
			break;
		OS_SleepMs(5);
	}
	XNextEvent(gX11Dpy, ev);
}

/* the modal loop of a dialog: waits for the next event (ModalNextEvent),
 * hands the dialog's own events to `handler` until it returns 0, and
 * keeps dispatching the other windows' events (so they repaint) with
 * their input ignored through gX11ModalDepth; nests */
void X11_RunModal(Window modal, int (*handler)(XEvent* ev, void* ctx), void* ctx)
{
	uint32_t lastTick = OS_TicksMs();
	gX11ModalDepth++;
	for(;;)
	{
		XEvent ev;
		ModalNextEvent(&ev, &lastTick);
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
