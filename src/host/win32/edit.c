/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * edit.c - the text entry of the Win32 back end ("B0 2x" of the scripts,
 *          src/sys/edit.c), the host's (OS_TextEntry, src/host/os.h): the
 *          original's 1 x 1 EDIT control over the picture, subclassed as
 *          the original's, shown and moved where the script asks
 *
 * The control is created hidden together with the main window
 * (Win32Edit_Create from OS_WindowCreate) and lives as long as it, in the
 * style of the builds up to 1.69/451; a setup of a later build's style
 * (setStyle) makes it again in that style, as those builds make the
 * control at each setup.  The entry's calls are no-ops without it.  Its
 * colours come from the main window's WM_CTLCOLOREDIT (wndproc.c,
 * gWin32EditColour): a transparent background, so the subclass raises
 * the input handlers' `entryPaint` before each paint for the host to put
 * the picture under it.  The keys it takes are reported to the host's input handlers as
 * taken (OPENBGI_KEY_TAKEN), so that the engine's key state, which the
 * scripts read, follows them as the original's GetAsyncKeyState did.
 *
 * The control is a Unicode window (CreateWindowExW): its text is converted
 * from and to the engine's encoding (wide.c), and since it counts UTF-16
 * units where the original's ANSI control counted bytes, the subclass
 * also keeps the text within the script's limit in bytes
 * (Win32_EditClamp) after every message that may have added to it.
 */
#include "win32_internal.h"

// the EDIT control, its original procedure, font and colour
static HWND gEdit;            // NULL before the main window exists and after the entry was destroyed
static WNDPROC gEditOrigProc; // the class procedure the subclass chains to
static HFONT gEditFont;       // the font of the last setFont; NULL until the first
/* The control lies on the picture: where the picture is shown at another
 * size (a client area of another size than it, a screen with a scale), the
 * control's place, its size and its font's are the engine's times that. */
static int gEditX, gEditY, gEditW = 1, gEditH = 1; // the rectangle of the last move, in the picture's pixels
static WCHAR gEditFace[0x100];                     // the face, the height and the width of the last setFont
static int gEditFontH, gEditFontW, gEditFontSet;
static double gEditFontScale = 1.0; // the scale gEditFont was made for
COLORREF gWin32EditColour;          // the text colour (setColour), applied by WM_CTLCOLOREDIT
static int gEditShown;              // the last show: a refresh only repaints a shown control
static int gEditRejectAscii;        // "B0 29" of 1.69/472 on: single-byte printable characters are swallowed
static int gEditLimit;              // the limit: the most the text may take, in bytes of the text encoding; 0 for no limit
static DWORD gEditStyle;            // the control's window style
static int gEditFlags;              // OPENBGI_ENTRY_* of the last setStyle: what Tab and Enter do, the IME status window

// the subclassed EDIT in `style`, hidden, with `text`
static void Make(HWND parent, DWORD style, const WCHAR* text)
{
	gEdit = CreateWindowExW(0, L"EDIT", text, style, 0, 0, 1, 1, parent, NULL, gWin32Instance, NULL);
	gEditStyle = style;
	if(gEdit)
	{
		gEditOrigProc = (WNDPROC)SetWindowLongPtrW(gEdit, GWLP_WNDPROC, (LONG_PTR)Win32Edit_SubclassProc);
		ShowWindow(gEdit, SW_HIDE);
	}
	gEditShown = 0;
	gEditX = gEditY = 0;
	gEditW = gEditH = 1;
}

/* Create the text entry of "B0 2x" as a child of the main window: a 1 x 1
 * EDIT with ES_CENTER | ES_AUTOHSCROLL | ES_NOHIDESEL and the text
 * MSG_EDIT_DEFAULT_TEXT, subclassed and hidden until the script shows it.
 * The colour starts black. */
void Win32Edit_Create(HWND parent)
{
	WCHAR text[0x100];
	Win32_ToWide(MSG_EDIT_DEFAULT_TEXT, text, (int)BGI_COUNTOF(text));
	Make(parent, 0x40000181, text); // WS_CHILD | ES_NOHIDESEL | ES_AUTOHSCROLL | ES_CENTER
	gEditLimit = 0;
	gEditFlags = OPENBGI_ENTRY_CENTRE;
	gWin32EditColour = RGB(0, 0, 0);
}

static void EditShow(void* user, int show);

// a key the control takes, to the engine's key state: OPENBGI_KEY_TAKEN with the repeat and system flags of the message
static void TakenKey(UINT msg, WPARAM wp, LPARAM lp)
{
	int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, flags = OPENBGI_KEY_TAKEN;
	if(msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
		flags |= OPENBGI_KEY_SYSTEM;
	if(down && ((lp >> 30) & 1))
		flags |= OPENBGI_KEY_REPEAT;
	if(gWin32Input.key)
		gWin32Input.key(gWin32Input.user, (int)wp, down, flags);
}

/* The subclass procedure: every key the control gets goes to the engine's
 * key state first (TakenKey); Tab (WM_KEYDOWN) selects the whole text and
 * gives the focus back to the main window, or hides the control
 * (OPENBGI_ENTRY_TAB_HIDES); Enter (WM_CHAR) selects the whole text and
 * gives the focus back, and hides the control too with
 * OPENBGI_ENTRY_ENTER_HIDES; every key, a click and an IME notification
 * invalidate the control so the picture under it is redrawn; WM_PAINT
 * first raises `entryPaint` with the control's rectangle.  Everything then goes on to the EDIT class
 * procedure, except Enter (no beep, no new line), Tab's character when
 * Tab hides, and with rejectAscii on single-byte printable characters
 * (WM_CHAR carries a UTF-16 unit here: 0x20 .. 0x7f are the ASCII
 * characters).  Once the control has taken a character, a pasted text or
 * an input method's result, the byte limit is applied. */
LRESULT CALLBACK Win32Edit_SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	LRESULT r;
	switch(msg)
	{
		case WM_KEYUP:
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
			TakenKey(msg, wp, lp);
			break;
		case WM_KEYDOWN:
			TakenKey(msg, wp, lp);
			if(wp == VK_TAB && (gEditFlags & OPENBGI_ENTRY_TAB_HIDES))
				EditShow(NULL, 0);
			else if(wp == VK_TAB)
			{
				SendMessageW(hwnd, EM_SETSEL, 0, (LPARAM)-1);
				SetFocus(gWin32MainWnd);
			}
			InvalidateRect(hwnd, NULL, FALSE);
			break;
		case WM_CHAR:
			if(wp == '\t' && (gEditFlags & OPENBGI_ENTRY_TAB_HIDES))
				return 0;
			if(gEditRejectAscii && wp >= 0x20 && wp < 0x80)
				return 0; // 1.69/472 on ("B0 29"): single-byte characters are swallowed
			if(wp == '\r')
			{
				SendMessageW(hwnd, EM_SETSEL, 0, (LPARAM)-1);
				SetFocus(gWin32MainWnd);
				if(gEditFlags & OPENBGI_ENTRY_ENTER_HIDES)
					EditShow(NULL, 0);
				return 0; // not passed on: no beep, no new line
			}
			break;
		case WM_LBUTTONDOWN:
			InvalidateRect(hwnd, NULL, FALSE);
			break;
		case WM_PAINT:
			if(gWin32Input.entryPaint)
			{ // the control's rectangle in the picture's pixels
				RECT r;
				GetWindowRect(hwnd, &r);
				MapWindowPoints(NULL, GetParent(hwnd), (POINT*)&r, 2);
				r.left = Win32_FromWinX(r.left);
				r.top = Win32_FromWinY(r.top);
				r.right = Win32_FromWinX(r.right);
				r.bottom = Win32_FromWinY(r.bottom);
				gWin32Input.entryPaint(gWin32Input.user, r.left, r.top, r.right - r.left, r.bottom - r.top);
			}
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

/* show or hide, and give the control its font when shown and none when
 * hidden (sent, without a redraw, as the original does); with
 * OPENBGI_ENTRY_IME_STATUS the input method's status window opens and
 * closes with it */
static void EditShow(void* user, int show)
{
	if(!gEdit)
		return;
	ShowWindow(gEdit, show ? SW_SHOW : SW_HIDE);
	SendMessageW(gEdit, WM_SETFONT, show ? (WPARAM)gEditFont : 0, 0);
	if(gEditFlags & OPENBGI_ENTRY_IME_STATUS)
	{
		HWND ime = ImmGetDefaultIMEWnd(gWin32MainWnd);
		if(ime)
			SendMessageW(ime, WM_IME_CONTROL, show ? IMC_OPENSTATUSWINDOW : IMC_CLOSESTATUSWINDOW, 0);
	}
	gEditShown = show != 0;
}

// a length of the picture as it is shown
static int Scaled(int v)
{
	double s = Win32_MapScale();
	int out = (int)(v * s + (v < 0 ? -0.5 : 0.5));
	return out || !v ? out : (v < 0 ? -1 : 1);
}

// the control where and as large as the picture's mapping puts its rectangle, with a repaint
static void Place(void)
{
	if(gEdit)
		MoveWindow(gEdit, Win32_ToWinX(gEditX), Win32_ToWinY(gEditY), Scaled(gEditW), Scaled(gEditH), TRUE);
}

/* the font of the last setFont at the picture's scale: CreateFont(height,
 * width, .., FW_THIN, SHIFTJIS_CHARSET, OUT_TT_PRECIS, PROOF_QUALITY,
 * FIXED_PITCH, face); given to the control (WM_SETFONT with redraw) when
 * `send` */
static void MakeFont(int send)
{
	HFONT old = gEditFont;
	gEditFontScale = Win32_MapScale();
	gEditFont = CreateFontW(Scaled(gEditFontH), Scaled(gEditFontW), 0, 0, FW_THIN, 0, 0, 0, SHIFTJIS_CHARSET, OUT_TT_PRECIS, 0, PROOF_QUALITY,
		FIXED_PITCH, gEditFace);
	if(send)
		SendMessageW(gEdit, WM_SETFONT, (WPARAM)gEditFont, 1);
	if(old)
		DeleteObject(old);
}

static void EditMove(void* user, int x, int y, int w, int h) // the picture's coordinates, with a repaint
{
	gEditX = x;
	gEditY = y;
	gEditW = w;
	gEditH = h;
	Place();
}

void Win32Edit_Remap(void)
{
	if(!gEdit)
		return;
	Place();
	if(gEditFontSet && gEditFontScale != Win32_MapScale())
		MakeFont(gEditShown); // (a hidden control has no font: EditShow)
}

// replace the font (MakeFont), with a redraw
static void EditSetFont(void* user, const char* face, int height, int width)
{
	if(!gEdit)
		return;
	Win32_ToWide(face, gEditFace, (int)BGI_COUNTOF(gEditFace));
	gEditFontH = height;
	gEditFontW = width;
	gEditFontSet = 1;
	MakeFont(1);
}

/* EM_LIMITTEXT: the most the user may type.  The original's ANSI control
 * counts bytes; this one counts UTF-16 units, of which a text never has
 * more than it has bytes, so the control's own limit is a first bound and
 * the subclass cuts the text to the bytes (gEditLimit). */
static void EditLimitText(void* user, int maxChars)
{
	gEditLimit = maxChars > 0 ? maxChars : 0;
	if(gEdit)
		SendMessageW(gEdit, EM_LIMITTEXT, (WPARAM)maxChars, 0);
}

static void EditSetText(void* user, const char* t)
{
	WCHAR wide[WIN32_WPATH];
	if(!gEdit)
		return;
	Win32_ToWide(t, wide, (int)BGI_COUNTOF(wide));
	SetWindowTextW(gEdit, wide);
}

static void EditSelectAll(void* user) // EM_SETSEL 0, -1
{
	if(gEdit)
		SendMessageW(gEdit, EM_SETSEL, 0, (LPARAM)-1);
}

static void EditFocus(void* user, int toEdit) // SetFocus to the control (1) or back to the main window (0, or without a control)
{
	SetFocus(toEdit && gEdit ? gEdit : gWin32MainWnd);
}

// GetWindowText into buf (n bytes) in the text encoding; the length copied, 0 (and an empty buf) without a control
static int EditGetText(void* user, char* buf, size_t n)
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

static void EditSetColour(void* user, uint32_t rgb) // 0x00RRGGBB -> COLORREF; applied at the next WM_CTLCOLOREDIT
{
	gWin32EditColour = RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

static void EditRefresh(void* user) // repaint a shown control at once (InvalidateRect + UpdateWindow)
{
	if(gEdit && gEditShown)
	{
		InvalidateRect(gEdit, NULL, FALSE);
		UpdateWindow(gEdit);
	}
}

static void EditRejectAscii(void* user, int reject)
{
	gEditRejectAscii = reject;
}

/* The style of the next setup: a control of another alignment or another
 * kind (one line or several) is made again in that style, empty and
 * hidden, as the builds from 1.69/472 on make one at each setup; Tab and
 * Enter work as the flags say from now on. */
static void EditSetStyle(void* user, int flags)
{
	DWORD style = WS_CHILD | ES_NOHIDESEL | ((flags & OPENBGI_ENTRY_MULTILINE) ? ES_MULTILINE : ES_AUTOHSCROLL) |
		((flags & OPENBGI_ENTRY_CENTRE) ? ES_CENTER : (flags & OPENBGI_ENTRY_RIGHT) ? ES_RIGHT
																					: ES_LEFT);
	gEditFlags = flags;
	if(!gEdit || style == gEditStyle)
		return;
	SetWindowLongPtrW(gEdit, GWLP_WNDPROC, (LONG_PTR)gEditOrigProc);
	DestroyWindow(gEdit);
	gEdit = NULL;
	Make(gWin32MainWnd, style, L"");
}

// "B0 23" of 1.69/472 on: shown - what the IME status window was last told; Tab and Enter may have hidden the control
static int EditShown(void* user)
{
	return gEdit && gEditShown;
}

/* Called when the engine tears down (its WM_CLOSE path in src/sys/window.c
 * and the shutdown in src/sys/engine.c): remove the subclass, delete the
 * font and forget the control, which the main window destroys with
 * itself.  The entry's calls are no-ops from here on. */
static void EditDestroy(void* user)
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
	gEditFontSet = 0;
	gEdit = NULL; // destroyed with its parent
}

static const OpenBGI_TextEntry_t kEntry = {sizeof(OpenBGI_TextEntry_t), EditShow, EditMove, EditSetFont, EditLimitText, EditSetText, EditSelectAll, EditFocus,
	EditGetText, EditSetColour, EditRefresh, EditDestroy, EditRejectAscii, EditSetStyle, EditShown};

// the EDIT control as the host's text entry
const OpenBGI_TextEntry_t* OS_TextEntry(void)
{
	/* a window drawn on through OpenGL has no control of the system's on
	 * its picture: the engine draws the entry in the frame itself there */
	return Win32Gl_Usable() ? NULL : &kEntry;
}
