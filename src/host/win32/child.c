/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * child.c - the pop-up child windows of the Win32 back end ("B0 1x" of
 *           the scripts, src/sys/child.c), the host's child windows
 *           (OS_ChildWindows, OpenBGI_ChildWindows_t of openbgi.h): up
 *           to eight windows of class BGI_CHILD_CLASS owned by the main
 *           window, and their window procedure
 *
 * The engine hands each window its picture whenever it changed
 * (`present`); the window shows it at once and paints itself from it when
 * it is uncovered.  A child window cannot be closed by the player:
 * WM_CLOSE is refused unless the engine is closing the window itself.
 * Ctrl+C copies the window's copy text ("B0 1C") to the clipboard; the
 * other keys typed into it are posted to the main window, as typed
 * there.
 */
#include "win32_internal.h"

// one child window slot (the original keeps a 32-byte record per slot)
typedef struct Win32Child
{
	HWND hwnd;            // NULL while there is no window (the slot free, or the system destroyed it)
	int closing;          // set before the engine's WM_CLOSE; the procedure refuses it otherwise
	int w, h;             // the client size it was made with
	OsSurface_t picture;  // what was presented last (the engine's pixels, valid until the next present or the close)
	const char* copyText; // what Ctrl+C copies (the engine's, valid as long); NULL nothing
} Win32Child_t;
static Win32Child_t gChildren[8];

// the slot of a window handle; NULL for a window that is not (or no longer) in the table
static Win32Child_t* ChildFind(HWND hwnd)
{
	int i;
	for(i = 0; i < 8; i++)
		if(hwnd && gChildren[i].hwnd == hwnd)
			return &gChildren[i];
	return NULL;
}

// the picture to the window's client area at its top left, unscaled
static void ShowPicture(Win32Child_t* c, HDC hdc)
{
	BITMAPINFOHEADER bi;
	if(!c->picture.pixels)
		return;
	Win32_SurfaceHeader(&bi, &c->picture, c->picture.height);
	StretchDIBits(hdc, 0, 0, c->picture.width, c->picture.height, 0, 0, c->picture.width, c->picture.height, c->picture.pixels,
		(const BITMAPINFO*)&bi, DIB_RGB_COLORS, SRCCOPY);
}

/* The child windows' procedure: WM_PAINT shows the picture presented
 * last, WM_DESTROY drops the window from its slot, WM_CLOSE is swallowed
 * unless the engine asked for it, Ctrl+C copies the copy text, the other
 * keys are posted to the main window; everything else is
 * DefWindowProc. */
LRESULT CALLBACK Win32_ChildWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	Win32Child_t* c = ChildFind(hwnd);
	switch(msg)
	{
		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			if(c)
				ShowPicture(c, hdc);
			EndPaint(hwnd, &ps);
			return 0;
		}
		case WM_DESTROY:
			if(c)
				c->hwnd = NULL; // (the engine closes the slot itself)
			return 0;
		case WM_CLOSE:
			// the player cannot close a child window; the engine sets `closing` first
			if(c && !c->closing)
				return 0;
			break;
		case WM_KEYDOWN:
			if(wp == 'C' && GetKeyState(VK_CONTROL) < 0)
			{ // Ctrl+C: the copy text, and the key goes no further
				if(c && c->copyText)
					OS_ClipboardSetText(c->copyText);
				return 0;
			}
			// fall through
		case WM_KEYUP:
			// keys typed into a child window count as typed into the main window
			if(gWin32MainWnd)
				PostMessageW(gWin32MainWnd, msg, wp, lp);
			return 0;
		default:
			break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

/* `create`: a hidden pop-up window in slot `slot` (0..7), owned by the
 * main window, its outer rectangle at screen position (x, y) around a
 * w x h client area.  NULL when the slot is out of range or
 * CreateWindowEx fails. */
static void* ChildCreate(void* user, int slot, const char* title, int x, int y, int w, int h)
{
	Win32Child_t* c;
	HWND hwnd;
	WCHAR wide[0x400];
	int fw, fh;
	BGI_UNUSED(user);
	if(slot < 0 || slot >= 8)
		return NULL;
	Win32_ToWide(title, wide, (int)BGI_COUNTOF(wide));
	OS_FrameMetrics(&fw, &fh);
	// WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, owned by the main window
	hwnd = CreateWindowExW(0, BGI_CHILD_CLASS, wide, 0x80ca0000, x, y, w + fw, h + fh, gWin32MainWnd, NULL, gWin32Instance, NULL);
	if(!hwnd)
		return NULL;
	c = &gChildren[slot];
	memset(c, 0, sizeof *c);
	c->hwnd = hwnd;
	c->w = w;
	c->h = h;
	return c;
}

// `close`: the window goes for good (WM_CLOSE with `closing` set, so the procedure lets it through)
static void ChildClose(void* user, void* win)
{
	Win32Child_t* c = (Win32Child_t*)win;
	BGI_UNUSED(user);
	if(c->hwnd)
	{
		c->closing = 1;
		SendMessageW(c->hwnd, WM_CLOSE, 0, 0); // WM_DESTROY follows
	}
	memset(c, 0, sizeof *c);
}

static void ChildShow(void* user, void* win, int show) // SW_SHOWNA: shown without taking the focus from the main window
{
	Win32Child_t* c = (Win32Child_t*)win;
	BGI_UNUSED(user);
	if(c->hwnd)
		ShowWindow(c->hwnd, show ? SW_SHOWNA : SW_HIDE);
}

static void ChildSetTitle(void* user, void* win, const char* title)
{
	Win32Child_t* c = (Win32Child_t*)win;
	WCHAR wide[0x400];
	BGI_UNUSED(user);
	if(!c->hwnd)
		return;
	Win32_ToWide(title, wide, (int)BGI_COUNTOF(wide));
	SetWindowTextW(c->hwnd, wide);
}

// `move`: MoveWindow of the outer rectangle (the client size and the frame as made), with a repaint
static void ChildMove(void* user, void* win, int x, int y)
{
	Win32Child_t* c = (Win32Child_t*)win;
	int fw, fh;
	BGI_UNUSED(user);
	if(!c->hwnd)
		return;
	OS_FrameMetrics(&fw, &fh);
	MoveWindow(c->hwnd, x, y, c->w + fw, c->h + fh, TRUE);
}

// `getPos`: the outer origin in screen pixels; (0, 0) without a window
static void ChildGetPos(void* user, void* win, int* x, int* y)
{
	Win32Child_t* c = (Win32Child_t*)win;
	RECT r = {0, 0, 0, 0};
	BGI_UNUSED(user);
	if(c->hwnd)
		GetWindowRect(c->hwnd, &r);
	*x = r.left;
	*y = r.top;
}

// `present`: the window's new picture, shown at once and kept for its repaints
static void ChildPresent(void* user, void* win, const uint8_t* pixels, int pitch, int w, int h, int bpp)
{
	Win32Child_t* c = (Win32Child_t*)win;
	HDC hdc;
	BGI_UNUSED(user);
	c->picture.pixels = (void*)pixels; // only read
	c->picture.pitch = pitch;
	c->picture.width = w;
	c->picture.height = h;
	c->picture.bpp = bpp;
	if(!c->hwnd || !(hdc = GetDC(c->hwnd)))
		return;
	ShowPicture(c, hdc);
	ReleaseDC(c->hwnd, hdc);
}

static void ChildSetCopyText(void* user, void* win, const char* text)
{
	BGI_UNUSED(user);
	((Win32Child_t*)win)->copyText = text;
}

static const OpenBGI_ChildWindows_t kChildWindows = {sizeof(OpenBGI_ChildWindows_t), ChildCreate, ChildClose, ChildShow, ChildSetTitle, ChildMove, ChildGetPos, ChildPresent,
	ChildSetCopyText};

// the pop-up windows as the host's child windows
const OpenBGI_ChildWindows_t* OS_ChildWindows(void)
{
	return &kChildWindows;
}
