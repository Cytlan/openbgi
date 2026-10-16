/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * child.c - the pop-up child windows of the Win32 back end ("B0 1x" of
 *           the scripts, src/sys/child.c) (inc/bgi/os.h): up to eight
 *           windows of class BGI_CHILD_CLASS owned by the main window,
 *           and their window procedure
 *
 * The engine keeps a back buffer per window and presents it from the
 * `child_paint` callback; the procedure here only forwards.  A child
 * window cannot be closed by the user: WM_CLOSE is refused unless the
 * engine is closing the window itself (OS_ChildClose), and keys typed
 * into it are passed on to the main window.
 */
#include "win32_internal.h"

// one child window slot (the original keeps a 32-byte record per slot)
typedef struct Win32Child
{
	HWND hwnd;   // NULL while the slot is free
	int closing; // set before the engine sends WM_CLOSE; the procedure refuses it otherwise
	int index;   // the slot number (0..7) the engine knows the window by
} Win32Child_t;
static Win32Child_t gChildren[8];

// the slot of a window handle; NULL for a window that is not (or no longer) in the table
static Win32Child_t* ChildFind(HWND hwnd)
{
	int i;
	for(i = 0; i < 8; i++)
		if(gChildren[i].hwnd == hwnd)
			return &gChildren[i];
	return NULL;
}

/* The child windows' procedure: WM_PAINT hands the paint DC to
 * `child_paint`, WM_DESTROY frees the slot and raises `child_destroyed`,
 * WM_CLOSE is swallowed unless the engine asked for it, keys go to the
 * main window; everything else is DefWindowProc. */
LRESULT CALLBACK Win32_ChildWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	Win32Child_t* c = ChildFind(hwnd);
	switch(msg)
	{
		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			if(c && gWin32Handlers.child_paint)
				gWin32Handlers.child_paint(c->index, (OsDc_t*)hdc); // the engine presents the window's back buffer
			EndPaint(hwnd, &ps);
			return 0;
		}
		case WM_DESTROY:
			if(c)
			{
				int index = c->index;
				memset(c, 0, sizeof *c); // the slot is free before the engine hears of it
				if(gWin32Handlers.child_destroyed)
					gWin32Handlers.child_destroyed(index);
			}
			return 0;
		case WM_CLOSE:
			// the user cannot close a child window; the engine sets `closing` first
			if(c && !c->closing)
				return 0;
			break;
		case WM_KEYDOWN:
		case WM_KEYUP:
			// keys typed into a child window count as typed into the main window
			if(gWin32MainWnd)
				SendMessageW(gWin32MainWnd, msg, wp, lp);
			break;
		default:
			break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

/* A hidden pop-up window in slot `index` (0..7), owned by the main window,
 * with its outer rectangle at (x, y) of size outerW x outerH in screen
 * pixels.  NULL when the index is out of range, the slot is taken or
 * CreateWindowEx fails. */
OsChild_t* OS_ChildCreate(int index, const char* title, int x, int y, int outerW, int outerH)
{
	Win32Child_t* c;
	HWND hwnd;
	WCHAR wide[0x400];
	if(index < 0 || index >= 8 || gChildren[index].hwnd)
		return NULL;
	Win32_ToWide(title, wide, (int)BGI_COUNTOF(wide));
	// WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, owned by the main window
	hwnd = CreateWindowExW(0, BGI_CHILD_CLASS, wide, 0x80ca0000, x, y, outerW, outerH,
		gWin32MainWnd, NULL, gWin32Instance, NULL);
	if(!hwnd)
		return NULL;
	c = &gChildren[index];
	c->hwnd = hwnd;
	c->closing = 0;
	c->index = index;
	return (OsChild_t*)c;
}

// close the window for good: WM_CLOSE with `closing` set, so the procedure lets it through; `child_destroyed` is raised before this returns
void OS_ChildClose(OsChild_t* ch)
{
	Win32Child_t* c = (Win32Child_t*)ch;
	if(!c || !c->hwnd)
		return;
	c->closing = 1;
	SendMessageW(c->hwnd, WM_CLOSE, 0, 0); // WM_DESTROY follows and clears the record
}

void OS_ChildShow(OsChild_t* ch, int show) // SW_SHOWNA: shown without taking the focus from the main window
{
	Win32Child_t* c = (Win32Child_t*)ch;
	if(c && c->hwnd)
		ShowWindow(c->hwnd, show ? SW_SHOWNA : SW_HIDE);
}

void OS_ChildSetTitle(OsChild_t* ch, const char* title)
{
	Win32Child_t* c = (Win32Child_t*)ch;
	WCHAR wide[0x400];
	if(!c || !c->hwnd)
		return;
	Win32_ToWide(title, wide, (int)BGI_COUNTOF(wide));
	SetWindowTextW(c->hwnd, wide);
}

void OS_ChildMove(OsChild_t* ch, int x, int y, int outerW, int outerH) // MoveWindow of the outer rectangle, with a repaint
{
	Win32Child_t* c = (Win32Child_t*)ch;
	if(c && c->hwnd)
		MoveWindow(c->hwnd, x, y, outerW, outerH, TRUE);
}

void OS_ChildGetPos(OsChild_t* ch, int* x, int* y) // the outer origin in screen pixels; (0, 0) without a window
{
	Win32Child_t* c = (Win32Child_t*)ch;
	RECT r = {0, 0, 0, 0};
	if(c && c->hwnd)
		GetWindowRect(c->hwnd, &r);
	*x = r.left;
	*y = r.top;
}

OsDc_t* OS_ChildGetDc(OsChild_t* ch)
{
	Win32Child_t* c = (Win32Child_t*)ch;
	return (c && c->hwnd) ? (OsDc_t*)GetDC(c->hwnd) : NULL;
}

void OS_ChildReleaseDc(OsChild_t* ch, OsDc_t* dc)
{
	Win32Child_t* c = (Win32Child_t*)ch;
	if(c && c->hwnd && dc)
		ReleaseDC(c->hwnd, (HDC)dc);
}
