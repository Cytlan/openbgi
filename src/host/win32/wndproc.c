/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wndproc.c - the main window procedure of the Win32 back end and the
 *             message pump (src/host/os.h)
 *
 * The original's window procedure with the interpreter calls replaced by
 * the host's handlers (OsHostHandlers_t), which hand everything to the
 * engine: the input (keys, the buttons, the wheel, the pointer, the
 * focus), every other message the engine reacts to (the close box, a
 * minimise, the IPC message of a second instance, the movie graph's and
 * MCI's notifications, and the rest for the scripts' message waits); the
 * host paints from the last frame.  What the original answered itself
 * (the screen saver, the menu beep, the EDIT control's colours) is
 * answered here.  OS_PumpMessages dispatches one message per call and
 * turns WM_QUIT into the `quit` handler.
 */
#include "win32_internal.h"

// ---- the main window procedure -----------------------------------------------------------------

/* The original hands every message to its message-wait dispatcher before
 * anything else.  The engine's key and button handling (reached through
 * the host's input handlers) dispatches their own message numbers, so
 * those are left out here to avoid a double delivery; every other message
 * goes through `message`.  (A double click therefore reaches the
 * waits as 0x203 from here and as 0x201 from the button handling, where
 * the original delivered 0x203 alone.) */
static int EngineDispatchesItself(UINT msg)
{
	switch(msg)
	{
		case WM_CLOSE:
		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		case WM_MOUSEWHEEL:
		case WM_XBUTTONDOWN:
		case WM_XBUTTONUP:
			return 1;
		default:
			return 0;
	}
}

/* a button press: the original gives the window the focus, then handles
 * it; here the host's `button` (0 L 1 R 2 M 3 X1 4 X2, the picture's
 * pixels from lParam; `flags` OPENBGI_CLICK_DOUBLE for a double click) */
static void ButtonDown(HWND hwnd, int button, LPARAM lp, int flags)
{
	SetFocus(hwnd);
	if(gWin32Input.button)
		gWin32Input.button(gWin32Input.user, button, 1, Win32_FromWinX((int)(short)LOWORD(lp)), Win32_FromWinY((int)(short)HIWORD(lp)),
			flags);
}

// a button release to the host's `button`
static void ButtonUp(int button, LPARAM lp)
{
	if(gWin32Input.button)
		gWin32Input.button(gWin32Input.user, button, 0, Win32_FromWinX((int)(short)LOWORD(lp)), Win32_FromWinY((int)(short)HIWORD(lp)), 0);
}

// a key to the host's `key`
static void Key(int vk, int down, int flags)
{
	if(gWin32Input.key)
		gWin32Input.key(gWin32Input.user, vk, down, flags);
}

/* WM_CHAR: a UTF-16 unit typed (the input method's results come as such
 * too) to the host's `text` as UTF-8; the first half of a surrogate pair
 * waits for the second, control characters are keys */
static void Char(unsigned unit)
{
	static unsigned high;
	uint32_t cp = unit;
	char u[5];
	if(unit >= 0xd800 && unit < 0xdc00)
	{
		high = unit;
		return;
	}
	if(unit >= 0xdc00 && unit < 0xe000)
	{
		if(!high)
			return;
		cp = 0x10000 + ((high - 0xd800) << 10) + (unit - 0xdc00);
	}
	high = 0;
	if(cp < 0x20 || cp == 0x7f || !gWin32Input.text)
		return;
	if(cp < 0x80)
		u[0] = (char)cp, u[1] = 0;
	else if(cp < 0x800)
		u[0] = (char)(0xc0 | (cp >> 6)), u[1] = (char)(0x80 | (cp & 0x3f)), u[2] = 0;
	else if(cp < 0x10000)
		u[0] = (char)(0xe0 | (cp >> 12)), u[1] = (char)(0x80 | ((cp >> 6) & 0x3f)), u[2] = (char)(0x80 | (cp & 0x3f)), u[3] = 0;
	else
		u[0] = (char)(0xf0 | (cp >> 18)), u[1] = (char)(0x80 | ((cp >> 12) & 0x3f)), u[2] = (char)(0x80 | ((cp >> 6) & 0x3f)),
		u[3] = (char)(0x80 | (cp & 0x3f)), u[4] = 0;
	gWin32Input.text(gWin32Input.user, u);
}

/* WM_MOUSEACTIVATE while the movie's video window covers the picture: the
 * click that activated the window is replayed as a button down / up pair
 * so the script sees it.  The original uses the engine's client position
 * of the mouse (picture coordinates); client coordinates of the main
 * window are used here, identical except under display scaling. */
static void MovieClick(HWND hwnd, LPARAM lp)
{
	POINT pt;
	LPARAM pos;
	UINT down;
	if(LOWORD(lp) != HTCLIENT || !OsWin32_MovieWindowShown())
		return;
	switch(HIWORD(lp))
	{
		case WM_LBUTTONDOWN: down = WM_LBUTTONDOWN; break;
		case WM_RBUTTONDOWN: down = WM_RBUTTONDOWN; break;
		case WM_MBUTTONDOWN: down = WM_MBUTTONDOWN; break;
		default: return;
	}
	GetCursorPos(&pt);
	ScreenToClient(hwnd, &pt);
	pos = MAKELPARAM(pt.x & 0xffff, pt.y & 0xffff);
	SendMessageW(hwnd, down, 0, pos);
	SendMessageW(hwnd, down + 1, 0, pos); // the matching WM_?BUTTONUP
}

/* The main window's procedure.  Every message not in EngineDispatchesItself
 * goes to `message` first; then each message the engine or the
 * original cares about is handled as the case says, and the rest reaches
 * DefWindowProc.  A `return` skips DefWindowProc, a `break` falls through
 * to it. */
#define DRAG_TIMER 0x0b61 // the main window's timer while the player drags it (WM_ENTERSIZEMOVE)

static int gDrag;      // the player drags the window or its border (WM_ENTERSIZEMOVE .. WM_EXITSIZEMOVE)
static int gDragScale; // .. and the monitor's scale became another meanwhile

LRESULT CALLBACK Win32_MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if(msg == WM_TIMER && wp == DRAG_TIMER)
	{ // (the back end's own: not a message of the game's window)
		if(gWin32Input.modal)
			gWin32Input.modal(gWin32Input.user);
		return 0;
	}
	if(gWin32Input.message && !EngineDispatchesItself(msg))
		gWin32Input.message(gWin32Input.user, (uint32_t)msg, (uint32_t)wp, (uint32_t)lp);

	switch(msg)
	{
		case WM_CREATE:
			return 0;

		case WM_DESTROY:
			Win32Gl_Detach();
			gWin32MainWnd = NULL; // (the engine posts its quit itself)
			return 0;

		case WM_SIZE:
			// only "restored" and "minimized" are of interest
			if(wp == SIZE_RESTORED || wp == SIZE_MINIMIZED)
			{
				if(gWin32Input.minimized)
					gWin32Input.minimized(gWin32Input.user, wp == SIZE_MINIMIZED);
			}
			if((wp == SIZE_RESTORED || wp == SIZE_MAXIMIZED) && Win32_ClientResized((int)LOWORD(lp), (int)HIWORD(lp)))
			{ // a window with a sizing border got another size: the engine hears of it, the picture is shown again
				if(gWin32Input.resized)
					gWin32Input.resized(gWin32Input.user, (int)LOWORD(lp), (int)HIWORD(lp));
				InvalidateRect(hwnd, NULL, FALSE);
			}
			break;

		case WM_ACTIVATE:
			if(gWin32Input.focus)
				gWin32Input.focus(gWin32Input.user, LOWORD(wp) != 0);
			break;

		case WM_PAINT:
		{
			// validate first, then let the host repaint: it shows the last frame's picture, not into the paint DC
			PAINTSTRUCT ps;
			BeginPaint(hwnd, &ps);
			EndPaint(hwnd, &ps);
			Win32_FillLetterbox(NULL); // the bars beside a scaled picture
			if(gWin32Input.paint)
				gWin32Input.paint(gWin32Input.user);
			return 0;
		}

		case WM_CLOSE:
			// the engine either asks the script (close mode 0) or tears the window down itself
			if(gWin32Input.close)
				gWin32Input.close(gWin32Input.user);
			return 0;

		case WM_SETCURSOR:
			/* the table entry for the shape, then DefWindowProc.
			 * Over the client area DefWindowProc re-applies the class
			 * cursor (the arrow), exactly as in the original. */
			if(gWin32Cursors[gWin32CursorShape])
				SetCursor(gWin32Cursors[gWin32CursorShape]);
			break;

		case WM_MOUSEACTIVATE:
			MovieClick(hwnd, lp);
			break;

		case WM_DISPLAYCHANGE:
			Win32_DisplayChanged();
			if(gWin32Input.displayChanged)
				gWin32Input.displayChanged(gWin32Input.user);
			break;

		/* the system's own loop has the thread while the player drags the
		 * window or its border: a timer keeps the engine's sound going */
		case WM_ENTERSIZEMOVE:
			SetTimer(hwnd, DRAG_TIMER, 20, NULL);
			gDrag = 1;
			gDragScale = 0;
			break;

		case WM_EXITSIZEMOVE:
			KillTimer(hwnd, DRAG_TIMER);
			gDrag = 0;
			if(gDragScale)
			{ // dragged to a monitor of another scale: fitted once more where it was let go
				gDragScale = 0;
				Win32_OwnSize(-1);
				if(gWin32Input.screenChanged)
					gWin32Input.screenChanged(gWin32Input.user);
			}
			break;

		case WM_SIZING: // the player drags the border: in proportion
			Win32_KeepAspect((int)wp, (RECT*)lp);
			return TRUE;

		case WM_GETMINMAXINFO:
			Win32_MinTrack(&((MINMAXINFO*)lp)->ptMinTrackSize);
			break;

		/* the scale of the window's monitor is another (the window was
		 * moved to another monitor, or the desktop's setting changed), or
		 * the task bar's place or size is: the engine fits the window
		 * again.  The rectangle the system proposes is not taken: the
		 * engine has its own idea of the size.  During a drag the system's
		 * loop may go on giving the window the size it had: such a size is
		 * not one the player gave it, and the window is fitted once more
		 * when the drag ends. */
		case WM_DPICHANGED:
			if(gDrag && !gDragScale)
			{
				gDragScale = 1;
				Win32_OwnSize(1);
			}
			if(gWin32Input.screenChanged)
				gWin32Input.screenChanged(gWin32Input.user);
			return 0;

		case WM_SETTINGCHANGE:
			if(wp == SPI_SETWORKAREA && gWin32Input.screenChanged)
				gWin32Input.screenChanged(gWin32Input.user);
			break;

		case WM_KEYDOWN:
			Key((int)wp, 1, ((lp >> 30) & 1) ? OPENBGI_KEY_REPEAT : 0); // bit 30: the key was already down (auto-repeat)
			break;

		case WM_KEYUP:
			Key((int)wp, 0, 0);
			break;

		case WM_CHAR:
			Char((unsigned)wp);
			break;

		case WM_SYSKEYDOWN:
			// Alt+key: F10, Enter and F4 mean something to the engine; nothing reaches DefWindowProc (no menu)
			Key((int)wp, 1, OPENBGI_KEY_SYSTEM | (((lp >> 30) & 1) ? OPENBGI_KEY_REPEAT : 0));
			return 0;

		case WM_SYSKEYUP:
			// (the engine counts F10's release as a key's)
			Key((int)wp, 0, OPENBGI_KEY_SYSTEM);
			return 0;

		case WM_SYSCOMMAND:
			// the screen saver and monitor power-down are refused while the game runs
			if(wp == SC_SCREENSAVE || wp == SC_MONITORPOWER)
				return 1;
			break;

		case WM_MENUCHAR:
			return MAKELRESULT(0, MNC_CLOSE); // no beep for Alt+letter

		case WM_CTLCOLOREDIT:
			// the text entry draws transparently in its own colour
			SetBkMode((HDC)wp, TRANSPARENT);
			SetTextColor((HDC)wp, gWin32EditColour);
			return (LRESULT)GetStockObject(NULL_BRUSH);

		case WM_LBUTTONDBLCLK:
			// a double click is a press as well, marked as the second of the pair
			ButtonDown(hwnd, 0, lp, OPENBGI_CLICK_DOUBLE);
			break;
		case WM_LBUTTONDOWN:
			ButtonDown(hwnd, 0, lp, 0);
			break;
		case WM_LBUTTONUP:
			ButtonUp(0, lp);
			break;

		case WM_RBUTTONDBLCLK:
			ButtonDown(hwnd, 1, lp, OPENBGI_CLICK_DOUBLE);
			break;
		case WM_RBUTTONDOWN:
			ButtonDown(hwnd, 1, lp, 0);
			break;
		case WM_RBUTTONUP:
			ButtonUp(1, lp);
			break;

		case WM_MBUTTONDBLCLK:
		case WM_MBUTTONDOWN:
			ButtonDown(hwnd, 2, lp, 0);
			break;
		case WM_MBUTTONUP:
			ButtonUp(2, lp);
			break;

		case WM_XBUTTONDOWN:
			ButtonDown(hwnd, (HIWORD(wp) & XBUTTON1) ? 3 : 4, lp, 0);
			break;
		case WM_XBUTTONUP:
			ButtonUp((HIWORD(wp) & XBUTTON1) ? 3 : 4, lp);
			break;

		case WM_MOUSEWHEEL:
			if(gWin32Input.wheel)
				gWin32Input.wheel(gWin32Input.user, (int)(short)HIWORD(wp)); // WHEEL_DELTA units, signed
			break;

		case WM_MOUSEMOVE:
			if(gWin32Input.pointer)
				gWin32Input.pointer(gWin32Input.user, Win32_FromWinX((int)(short)LOWORD(lp)), Win32_FromWinY((int)(short)HIWORD(lp)));
			break;

		case WM_DROPFILES:
		{
			/* every dropped file's path as the engine's, the first one last: the
			 * original takes the first alone, and so that is the one a game's
			 * scripts are left with ("80 6D"); the launcher takes them all.  (An
			 * empty string when the query of the only one fails.) */
			WCHAR wide[WIN32_WPATH];
			char path[WIN32_WPATH];
			HDROP drop = (HDROP)wp;
			UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, NULL, 0), i;
			for(i = count ? count : 1; i-- > 0;)
			{
				wide[0] = 0;
				DragQueryFileW(drop, i, wide, (UINT)BGI_COUNTOF(wide));
				Win32_PathFromWide(wide, path, (int)sizeof path);
				if(gWin32Input.dropFile && (path[0] || count <= 1))
					gWin32Input.dropFile(gWin32Input.user, path);
			}
			DragFinish(drop);
			return 0;
		}

		case MM_MCINOTIFY:
			if(gWin32Input.cdNotify)
				gWin32Input.cdNotify(gWin32Input.user, (int)wp); // MCI_NOTIFY_SUCCESSFUL = 1
			break;

		case WM_APP:
		{
			// the movie graph's events
			int completed = OsWin32_MovieDrainEvents();
			if(gWin32Input.movieEvent)
				gWin32Input.movieEvent(gWin32Input.user, completed);
			break;
		}

		case BGI_WM_IPC:
		{
			// another instance hands over its command line
			char buf[0x200];
			if(OsWin32_IpcReceive((uint32_t)wp, (uint32_t)lp, buf, sizeof buf) && gWin32Input.dropFile)
				gWin32Input.dropFile(gWin32Input.user, buf); // (the engine takes it as a dropped file)
			return 0;
		}

		default:
			break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- the message pump --------------------------------------------------------------------------

// PeekMessage(PM_REMOVE) of one message: 0 when the queue was empty; WM_QUIT raises `quit` instead of being dispatched
int OS_PumpMessages(void)
{
	MSG m;
	if(!PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
		return 0;
	if(m.message == WM_QUIT)
	{
		if(gWin32Input.quit)
			gWin32Input.quit(gWin32Input.user);
		return 1;
	}
	TranslateMessage(&m);
	DispatchMessageW(&m);
	return 1;
}

/* The back end's own modal windows (the installer's progress dialog) run
 * the queue through this: `dialog` gets IsDialogMessage first.  1 to go
 * on (also when the queue was empty), 0 on WM_QUIT, which is put back so
 * the engine's pump ends the program as usual. */
int OsWin32_PumpOnce(HWND dialog)
{
	MSG m;
	if(!PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
		return 1;
	if(m.message == WM_QUIT)
	{
		PostQuitMessage((int)m.wParam);
		return 0;
	}
	if(dialog && IsDialogMessageW(dialog, &m))
		return 1;
	TranslateMessage(&m);
	DispatchMessageW(&m);
	return 1;
}

void OS_DragAccept(int accept) // DragAcceptFiles: whether WM_DROPFILES (and so `drop_file`) arrives
{
	if(gWin32MainWnd)
		DragAcceptFiles(gWin32MainWnd, accept != 0);
}
