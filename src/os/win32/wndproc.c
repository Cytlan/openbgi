/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wndproc.c - the main window procedure of the Win32 back end and the
 *             message pump (inc/bgi/os.h)
 *
 * The original's window procedure with the interpreter calls replaced by
 * the OsEventHandlers_t callbacks: every message the engine reacts to
 * (keys, the mouse, activation, painting, the close box, the IPC message
 * of a second instance, the movie graph's and MCI's notifications)
 * becomes a callback into src/sys/winmsg.c; what the original answered
 * itself (the screen saver, the menu beep, the EDIT control's colours)
 * is answered here.  OS_PumpMessages dispatches one message per call and
 * turns WM_QUIT into the `quit` callback.
 */
#include "win32_internal.h"

// ---- the main window procedure -----------------------------------------------------------------

/* The original hands every message to its message-wait dispatcher before
 * anything else.  The engine's key and button handlers dispatch their own
 * message numbers, so those are left out here to avoid a double delivery;
 * every other message goes through `raw_message`.  (A double click
 * therefore reaches the waits as 0x203 from here and as 0x201 from the
 * button handler, where the original delivered 0x203 alone.) */
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

// a button press: the original gives the window the focus, then runs the engine's handler (button 0 L 1 R 2 M 3 X1 4 X2, client pixels from lParam)
static void ButtonDown(HWND hwnd, int button, LPARAM lp)
{
	SetFocus(hwnd);
	if(gWin32Handlers.mouse_button)
		gWin32Handlers.mouse_button(button, 1, Win32_FromWinX((int)(short)LOWORD(lp)), Win32_FromWinY((int)(short)HIWORD(lp)));
}

// a button release to the engine's handler
static void ButtonUp(int button, LPARAM lp)
{
	if(gWin32Handlers.mouse_button)
		gWin32Handlers.mouse_button(button, 0, Win32_FromWinX((int)(short)LOWORD(lp)), Win32_FromWinY((int)(short)HIWORD(lp)));
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
 * goes to `raw_message` first; then each message the engine or the
 * original cares about is handled as the case says, and the rest reaches
 * DefWindowProc.  A `return` skips DefWindowProc, a `break` falls through
 * to it. */
LRESULT CALLBACK Win32_MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if(gWin32Handlers.raw_message && !EngineDispatchesItself(msg))
		gWin32Handlers.raw_message((uint32_t)msg, (uint32_t)wp, (uint32_t)lp);

	switch(msg)
	{
		case WM_CREATE:
			if(gWin32Handlers.created)
				gWin32Handlers.created();
			return 0;

		case WM_DESTROY:
			gWin32MainWnd = NULL;
			if(gWin32Handlers.destroyed)
				gWin32Handlers.destroyed(); // the engine posts the quit message from here
			return 0;

		case WM_SIZE:
			// only "restored" and "minimized" are of interest
			if(wp == SIZE_RESTORED || wp == SIZE_MINIMIZED)
			{
				if(gWin32Handlers.sized)
					gWin32Handlers.sized(wp == SIZE_MINIMIZED);
			}
			break;

		case WM_ACTIVATE:
			if(gWin32Handlers.activate)
				gWin32Handlers.activate(LOWORD(wp) != 0);
			break;

		case WM_PAINT:
		{
			// validate first, then let the engine repaint: it presents from its own back buffer, not into the paint DC
			PAINTSTRUCT ps;
			BeginPaint(hwnd, &ps);
			EndPaint(hwnd, &ps);
			Win32_FillLetterbox(NULL); // the bars beside a scaled picture
			if(gWin32Handlers.paint)
				gWin32Handlers.paint();
			return 0;
		}

		case WM_CLOSE:
			// the engine either asks the script (close mode 0) or tears the window down itself
			if(gWin32Handlers.close_request)
				gWin32Handlers.close_request();
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
			if(gWin32Handlers.display_change)
				gWin32Handlers.display_change();
			break;

		case WM_KEYDOWN:
			if(gWin32Handlers.key_down)
				gWin32Handlers.key_down((int)wp, (int)((lp >> 30) & 1)); // bit 30: the key was already down (auto-repeat)
			break;

		case WM_KEYUP:
			if(gWin32Handlers.key_up)
				gWin32Handlers.key_up((int)wp);
			break;

		case WM_SYSKEYDOWN:
			// Alt+key: F10, Enter and F4 mean something to the engine; nothing reaches DefWindowProc (no menu)
			if(gWin32Handlers.syskey)
				gWin32Handlers.syskey((int)wp);
			return 0;

		case WM_SYSKEYUP:
			// F10 arrives as a system key: its release reaches the engine as an ordinary key up
			if(wp == VK_F10 && gWin32Handlers.key_up)
				gWin32Handlers.key_up(VK_F10);
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
			// a double click is delivered as a press as well, after `double_click`
			if(gWin32Handlers.double_click)
				gWin32Handlers.double_click(0);
			ButtonDown(hwnd, 0, lp);
			break;
		case WM_LBUTTONDOWN:
			ButtonDown(hwnd, 0, lp);
			break;
		case WM_LBUTTONUP:
			ButtonUp(0, lp);
			break;

		case WM_RBUTTONDBLCLK:
			if(gWin32Handlers.double_click)
				gWin32Handlers.double_click(1);
			ButtonDown(hwnd, 1, lp);
			break;
		case WM_RBUTTONDOWN:
			ButtonDown(hwnd, 1, lp);
			break;
		case WM_RBUTTONUP:
			ButtonUp(1, lp);
			break;

		case WM_MBUTTONDBLCLK:
		case WM_MBUTTONDOWN:
			ButtonDown(hwnd, 2, lp);
			break;
		case WM_MBUTTONUP:
			ButtonUp(2, lp);
			break;

		case WM_XBUTTONDOWN:
			ButtonDown(hwnd, (HIWORD(wp) & XBUTTON1) ? 3 : 4, lp);
			break;
		case WM_XBUTTONUP:
			ButtonUp((HIWORD(wp) & XBUTTON1) ? 3 : 4, lp);
			break;

		case WM_MOUSEWHEEL:
			if(gWin32Handlers.mouse_wheel)
				gWin32Handlers.mouse_wheel((int)(short)HIWORD(wp)); // WHEEL_DELTA units, signed
			break;

		case WM_DROPFILES:
		{
			// the first dropped file's path as the engine's (an empty string when the query fails)
			WCHAR wide[WIN32_WPATH];
			char path[WIN32_WPATH];
			HDROP drop = (HDROP)wp;
			wide[0] = 0;
			DragQueryFileW(drop, 0, wide, (UINT)BGI_COUNTOF(wide));
			DragFinish(drop);
			Win32_PathFromWide(wide, path, (int)sizeof path);
			if(gWin32Handlers.drop_file)
				gWin32Handlers.drop_file(path);
			return 0;
		}

		case MM_MCINOTIFY:
			if(gWin32Handlers.mci_notify)
				gWin32Handlers.mci_notify((int)wp); // MCI_NOTIFY_SUCCESSFUL = 1
			break;

		case WM_APP:
		{
			// the movie graph's events
			int completed = OsWin32_MovieDrainEvents();
			if(gWin32Handlers.movie_event)
				gWin32Handlers.movie_event(completed);
			break;
		}

		case BGI_WM_IPC:
		{
			// another instance hands over its command line
			char buf[0x200];
			if(OsWin32_IpcReceive((uint32_t)wp, (uint32_t)lp, buf, sizeof buf) && gWin32Handlers.ipc_string)
				gWin32Handlers.ipc_string(buf);
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
		if(gWin32Handlers.quit)
			gWin32Handlers.quit(); // throws 0 in the engine
		return 1;
	}
	TranslateMessage(&m);
	DispatchMessageW(&m);
	return 1;
}

void OS_PostQuit(void)
{
	PostQuitMessage(0);
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
