/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * edit.c - the text entry of the Win32 back end ("B0 2x" of the scripts,
 *          src/sys/edit.c) (inc/bgi/os.h): the original's 1 x 1 EDIT
 *          control over the picture, subclassed to hand its keys to the
 *          engine, shown and moved where the script asks
 *
 * The control is created hidden together with the main window
 * (Win32Edit_Create from OS_WindowCreate) and lives as long as it; the
 * OS_Edit* calls are no-ops without it.  Its colours come from the main
 * window's WM_CTLCOLOREDIT (wndproc.c, gWin32EditColour): a transparent
 * background, so the subclass raises `edit_paint` before each paint for
 * the engine to put the picture under it.
 *
 * The control is a Unicode window (CreateWindowExW): its text is converted
 * from and to the engine's encoding (wide.c), and since it counts UTF-16
 * units where the original's ANSI control counted bytes, the subclass
 * also keeps the text within the script's limit in bytes
 * (Win32_EditClamp) after every message that may have added to it.
 */
#include "win32_internal.h"

// the EDIT control, its original procedure, font and colour
static HWND gEdit;            // NULL before the main window exists and after OS_EditDestroy
static WNDPROC gEditOrigProc; // the class procedure the subclass chains to
static HFONT gEditFont;       // the font of OS_EditSetFont; NULL until the first call
COLORREF gWin32EditColour;    // the text colour (OS_EditSetColour), applied by WM_CTLCOLOREDIT
static int gEditShown;        // OS_EditShow's last value: OS_EditRefresh only repaints a shown control
static int gEditRejectAscii;  // "B0 29" of 1.69/472 on: single-byte printable characters are swallowed
static int gEditLimit;        // OS_EditLimitText: the most the text may take, in bytes of the text encoding; 0 for no limit

/* Create the text entry of "B0 2x" as a child of the main window: a 1 x 1
 * EDIT with ES_CENTER | ES_AUTOHSCROLL | ES_NOHIDESEL and the text
 * MSG_EDIT_DEFAULT_TEXT, subclassed and hidden until the script shows it.
 * The colour starts black. */
void Win32Edit_Create(HWND parent)
{
	WCHAR text[0x100];
	Win32_ToWide(MSG_EDIT_DEFAULT_TEXT, text, (int)BGI_COUNTOF(text));
	gEdit = CreateWindowExW(0, L"EDIT", text, 0x40000181, 0, 0, 1, 1, parent, NULL, gWin32Instance, NULL);
	if(gEdit)
	{
		gEditOrigProc = (WNDPROC)SetWindowLongPtrW(gEdit, GWLP_WNDPROC, (LONG_PTR)Win32Edit_SubclassProc);
		ShowWindow(gEdit, SW_HIDE);
	}
	gEditLimit = 0;
	gEditShown = 0;
	gWin32EditColour = RGB(0, 0, 0);
}

/* The subclass procedure: Tab (WM_KEYDOWN) and Enter (WM_CHAR) select the
 * whole text and give the focus back to the main window; every key and
 * IME notification invalidates the control so the picture under it is
 * redrawn; WM_PAINT first raises `edit_paint`.  Everything then goes on
 * to the EDIT class procedure, except Enter (no beep for a character the
 * single-line control cannot use) and, with OS_EditRejectAscii on,
 * single-byte printable characters (WM_CHAR carries a UTF-16 unit here:
 * 0x20 .. 0x7f are the ASCII characters).  Once the control has taken a
 * character, a pasted text or an input method's result, the byte limit
 * is applied. */
LRESULT CALLBACK Win32Edit_SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	LRESULT r;
	switch(msg)
	{
		case WM_KEYDOWN:
			if(wp == VK_TAB)
			{
				SendMessageW(hwnd, EM_SETSEL, 0, (LPARAM)-1);
				SetFocus(gWin32MainWnd);
			}
			InvalidateRect(hwnd, NULL, FALSE);
			break;
		case WM_CHAR:
			if(gEditRejectAscii && wp >= 0x20 && wp < 0x80)
				return 0; // 1.69/472 on ("B0 29"): single-byte characters are swallowed
			if(wp == '\r')
			{
				SendMessageW(hwnd, EM_SETSEL, 0, (LPARAM)-1);
				SetFocus(gWin32MainWnd);
				return 0; // not passed on: no beep for the unused character
			}
			break;
		case WM_PAINT:
			if(gWin32Handlers.edit_paint)
				gWin32Handlers.edit_paint();
			break;
		case WM_IME_NOTIFY:
			InvalidateRect(hwnd, NULL, FALSE);
			break;
		default:
			break;
	}
	r = CallWindowProcW(gEditOrigProc, hwnd, msg, wp, lp);
	if(gEditLimit > 0 && (msg == WM_CHAR || msg == WM_PASTE || msg == WM_IME_CHAR || msg == WM_IME_COMPOSITION))
		Win32_EditClamp(hwnd, gEditLimit);
	return r;
}

// show or hide, and give the control its font when shown and none when hidden (posted, as the original does)
void OS_EditShow(int show)
{
	if(!gEdit)
		return;
	ShowWindow(gEdit, show ? SW_SHOW : SW_HIDE);
	PostMessageW(gEdit, WM_SETFONT, show ? (WPARAM)gEditFont : 0, 0);
	gEditShown = show != 0;
}

void OS_EditMove(int x, int y, int w, int h) // client coordinates of the main window, with a repaint
{
	if(gEdit)
		MoveWindow(gEdit, x, y, w, h, TRUE);
}

// replace the font: CreateFont(size, size / 2, .., FW_THIN, SHIFTJIS_CHARSET, OUT_TT_PRECIS, PROOF_QUALITY, FIXED_PITCH, face), WM_SETFONT with redraw
void OS_EditSetFont(const char* face, int size)
{
	WCHAR wface[0x100];
	if(!gEdit)
		return;
	if(gEditFont)
		DeleteObject(gEditFont);
	Win32_ToWide(face, wface, (int)BGI_COUNTOF(wface));
	gEditFont = CreateFontW(size, size / 2, 0, 0, FW_THIN, 0, 0, 0, SHIFTJIS_CHARSET, OUT_TT_PRECIS, 0,
		PROOF_QUALITY, FIXED_PITCH, wface);
	SendMessageW(gEdit, WM_SETFONT, (WPARAM)gEditFont, 1);
}

/* EM_LIMITTEXT: the most the user may type.  The original's ANSI control
 * counts bytes; this one counts UTF-16 units, of which a text never has
 * more than it has bytes, so the control's own limit is a first bound and
 * the subclass cuts the text to the bytes (gEditLimit). */
void OS_EditLimitText(int maxChars)
{
	gEditLimit = maxChars > 0 ? maxChars : 0;
	if(gEdit)
		SendMessageW(gEdit, EM_LIMITTEXT, (WPARAM)maxChars, 0);
}

void OS_EditSetText(const char* t)
{
	WCHAR wide[WIN32_WPATH];
	if(!gEdit)
		return;
	Win32_ToWide(t, wide, (int)BGI_COUNTOF(wide));
	SetWindowTextW(gEdit, wide);
}

void OS_EditSelectAll(void) // EM_SETSEL 0, -1
{
	if(gEdit)
		SendMessageW(gEdit, EM_SETSEL, 0, (LPARAM)-1);
}

void OS_EditFocus(int toEdit) // SetFocus to the control (1) or back to the main window (0, or without a control)
{
	SetFocus(toEdit && gEdit ? gEdit : gWin32MainWnd);
}

// GetWindowText into buf (n bytes) in the text encoding; the length copied, 0 (and an empty buf) without a control
int OS_EditGetText(char* buf, size_t n)
{
	WCHAR wide[WIN32_WPATH];
	if(!n)
		return 0;
	if(!gEdit)
	{
		buf[0] = 0;
		return 0;
	}
	wide[0] = 0;
	GetWindowTextW(gEdit, wide, (int)BGI_COUNTOF(wide));
	return Win32_TextFromWide(wide, buf, (int)n);
}

void OS_EditSetColour(uint32_t rgb) // 0x00RRGGBB -> COLORREF; applied at the next WM_CTLCOLOREDIT
{
	gWin32EditColour = RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

void OS_EditRefresh(void) // repaint a shown control at once (InvalidateRect + UpdateWindow)
{
	if(gEdit && gEditShown)
	{
		InvalidateRect(gEdit, NULL, FALSE);
		UpdateWindow(gEdit);
	}
}

void OS_EditRejectAscii(int reject)
{
	gEditRejectAscii = reject;
}

// "B0 23" of 1.69/472 on: the IME open state of the control's input context (ImmGetOpenStatus); 0 without a control or context
int OS_EditImeOpen(void)
{
	HIMC imc = gEdit ? ImmGetContext(gEdit) : NULL;
	int open = imc ? ImmGetOpenStatus(imc) != 0 : 0;
	if(imc)
		ImmReleaseContext(gEdit, imc);
	return open;
}

/* Called when the engine tears down (its WM_CLOSE path in src/sys/window.c
 * and the shutdown in src/sys/engine.c): remove the subclass, delete the
 * font and forget the control, which the main window destroys with
 * itself.  The OS_Edit* calls are no-ops from here on. */
void OS_EditDestroy(void)
{
	if(gEdit && gEditOrigProc)
	{
		SetWindowLongPtrW(gEdit, GWLP_WNDPROC, (LONG_PTR)gEditOrigProc);
		gEditOrigProc = NULL;
	}
	if(gEditFont)
	{
		DeleteObject(gEditFont);
		gEditFont = NULL;
	}
	gEdit = NULL; // destroyed with its parent
}
