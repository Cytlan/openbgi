/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dbgwin.c - the debugger's own window of the Win32 back end
 *            (OS_DebugWindowOpen and friends, inc/bgi/os.h): a second
 *            top-level window the debugger (src/dbg) draws its panes into
 *
 * None of this exists in the original.  The window is a plain overlapped
 * window of its own class whose input goes to the debug_* callbacks; the
 * debugger presents a whole 32-bit surface into it at its own rate with
 * OS_DebugWindowPresent, so WM_PAINT only validates and the background is
 * never erased.
 */
#include "win32_internal.h"

#define BGI_DEBUG_CLASS L"OpenBGI - Debugger" // the window class, registered on the first open
static HWND gDbgWnd;                          // the window; NULL while closed
static int gDbgClassRegistered;               // the class exists (it outlives the window)

/* The printable ASCII character a virtual key produces on the current
 * layout, lower-cased unless Shift is down, for the debugger's letter and
 * digit commands; 0 for a key without one. */
static int DebugKeyChar(WPARAM vk)
{
	UINT ch = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR) & 0xffff;
	if(ch < 0x20 || ch >= 0x7f)
		return 0;
	if(ch >= 'A' && ch <= 'Z' && !(GetKeyState(VK_SHIFT) & 0x8000))
		ch += 'a' - 'A';
	return (int)ch;
}

/* The window procedure: keys (system keys included) to `debug_key`, the
 * mouse to `debug_mouse` (button -1 for a move, 0 L 1 R 2 M; a press
 * takes the focus), the wheel to `debug_wheel`, the close box to
 * `debug_close` (the debugger then calls OS_DebugWindowClose itself). */
static LRESULT CALLBACK DebugWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch(msg)
	{
		case WM_PAINT:
		{ // the debugger presents at its own rate; the paint only validates
			PAINTSTRUCT ps;
			BeginPaint(hwnd, &ps);
			EndPaint(hwnd, &ps);
			return 0;
		}
		case WM_ERASEBKGND:
			return 1; // nothing to erase: the next present covers everything
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			if(gWin32Handlers.debug_key)
				gWin32Handlers.debug_key((int)wp, 1, DebugKeyChar(wp));
			return 0;
		case WM_KEYUP:
		case WM_SYSKEYUP:
			if(gWin32Handlers.debug_key)
				gWin32Handlers.debug_key((int)wp, 0, 0);
			return 0;
		case WM_MOUSEMOVE:
			if(gWin32Handlers.debug_mouse)
				gWin32Handlers.debug_mouse((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), -1, 0);
			return 0;
		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
		case WM_MBUTTONDOWN:
			SetFocus(hwnd);
			if(gWin32Handlers.debug_mouse)
				gWin32Handlers.debug_mouse((int)(short)LOWORD(lp), (int)(short)HIWORD(lp),
					msg == WM_LBUTTONDOWN ? 0 : msg == WM_RBUTTONDOWN ? 1
																	  : 2,
					1);
			return 0;
		case WM_LBUTTONUP:
		case WM_RBUTTONUP:
		case WM_MBUTTONUP:
			if(gWin32Handlers.debug_mouse)
				gWin32Handlers.debug_mouse((int)(short)LOWORD(lp), (int)(short)HIWORD(lp),
					msg == WM_LBUTTONUP ? 0 : msg == WM_RBUTTONUP ? 1
																  : 2,
					0);
			return 0;
		case WM_MOUSEWHEEL:
			if(gWin32Handlers.debug_wheel)
				gWin32Handlers.debug_wheel((int)(short)HIWORD(wp));
			return 0;
		case WM_CLOSE:
			if(gWin32Handlers.debug_close)
				gWin32Handlers.debug_close(); // the debugger closes itself (OS_DebugWindowClose)
			return 0;
		case WM_DESTROY:
			gDbgWnd = NULL;
			return 0;
		default:
			break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Open the window with a w x h client area at (40, 40), with the main
 * window's icon and a black background; 1 when it is up, 0 when one is
 * open already or the class or the window cannot be created. */
int OS_DebugWindowOpen(int w, int h, const char* title)
{
	RECT r;
	WCHAR wide[0x100];
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	if(gDbgWnd)
		return 0;
	if(!gDbgClassRegistered)
	{
		WNDCLASSEXW wc;
		memset(&wc, 0, sizeof wc);
		wc.cbSize = sizeof wc;
		wc.style = CS_OWNDC;
		wc.lpfnWndProc = DebugWndProc;
		wc.hInstance = gWin32Instance;
		wc.hIcon = LoadIconW(gWin32Instance, MAKEINTRESOURCEW(0x65));
		wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
		wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
		wc.lpszClassName = BGI_DEBUG_CLASS;
		if(!RegisterClassExW(&wc))
			return 0;
		gDbgClassRegistered = 1;
	}
	r.left = 0;
	r.top = 0;
	r.right = w;
	r.bottom = h;
	AdjustWindowRect(&r, style, FALSE); // the outer size that gives a w x h client area
	Win32_ToWide(title, wide, (int)BGI_COUNTOF(wide));
	gDbgWnd = CreateWindowExW(0, BGI_DEBUG_CLASS, wide, style, 40, 40, r.right - r.left, r.bottom - r.top, NULL, NULL,
		gWin32Instance, NULL);
	if(!gDbgWnd)
		return 0;
	ShowWindow(gDbgWnd, SW_SHOWNORMAL);
	return 1;
}

void OS_DebugWindowClose(void) // DestroyWindow; nothing without a window
{
	HWND hwnd = gDbgWnd;
	gDbgWnd = NULL;
	if(hwnd)
		DestroyWindow(hwnd);
}

int OS_DebugWindowAlive(void)
{
	return gDbgWnd != NULL;
}

// the whole surface at 1:1 from the window's top-left corner (StretchDIBits, SRCCOPY); nothing without a window or pixels
void OS_DebugWindowPresent(const OsSurface_t* s)
{
	BITMAPINFOHEADER bi;
	HDC hdc;
	if(!gDbgWnd || !s->pixels)
		return;
	hdc = GetDC(gDbgWnd);
	if(!hdc)
		return;
	Win32_SurfaceHeader(&bi, s, s->height);
	StretchDIBits(hdc, 0, 0, s->width, s->height, 0, 0, s->width, s->height, s->pixels, (const BITMAPINFO*)&bi,
		DIB_RGB_COLORS, SRCCOPY);
	ReleaseDC(gDbgWnd, hdc);
}
