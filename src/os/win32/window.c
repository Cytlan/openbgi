/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.c - the Win32 back end's main window (inc/bgi/os.h): its
 *            creation, the display mode, GDI presentation, and the cursor
 *            and keyboard state
 *
 * This is the original's windowing code (window creation, the display mode
 * part of its mode switch, the DC blit of its presentation) with the
 * engine's decisions removed: wherever the original called into the
 * interpreter, this back end raises the matching OsEventHandlers_t
 * callback (wndproc.c) and the engine (src/sys/window.c) does what the
 * original did there.  The child windows are in child.c, the EDIT control
 * in edit.c, the debugger's window in dbgwin.c.
 *
 * Differences from the original, all deliberate:
 *   * The classes are registered and the windows created through the
 *     wide-character calls, so the windows are Unicode windows: the title
 *     is converted from the engine's Shift-JIS (wide.c) and shows the same
 *     on every system.
 *   * The display mode is switched with ChangeDisplaySettings instead of
 *     IDirectDraw::SetDisplayMode (the original creates a DirectDraw object
 *     only for the mode switch, GetScanLine and GetMonitorFrequency; the
 *     vertical-blank emulation that used the last two is not reproduced).
 *   * The back buffer is plain memory, so presenting uses StretchDIBits
 *     where the original selected its DIB section into a memory DC and
 *     called BitBlt.  The pixels copied are the same.
 *   * The icon (0x65) and the cursor (0x6A) are resources of the original
 *     executable.  They are loaded when a resource script provides them and
 *     silently absent otherwise (LoadIcon / LoadCursor return NULL and the
 *     class falls back to no icon / the arrow).
 */
#include "win32_internal.h"

HWND gWin32MainWnd;               // the main window; NULL before OS_WindowCreate and after WM_DESTROY
OsEventHandlers_t gWin32Handlers; // the engine's callbacks, copied by OS_SetEventHandlers
static int gClassesRegistered;    // RegisterClasses has run (the classes outlive the window)

// the frame the window manager adds around the 800x600 client area (pixels, both sides together)
int gWin32FrameW, gWin32FrameH;

// the cursor table and the shape "80 67" selected (wndproc.c applies it on WM_SETCURSOR)
HCURSOR gWin32Cursors[5];
int gWin32CursorShape;

// the display mode: 1 while ChangeDisplaySettings is in effect
static int gModeChanged;

/* The windowed client area and the picture the engine draws into it.  Up
 * to 1.494 they are the same size; from 1.529 on "81 64" sizes the window
 * on its own (OS_WindowSetPicture tells the picture size) and the picture
 * is stretched into the client area, centred with its aspect kept, the
 * mouse mapped back - the original stretches in its Direct3D present. */
static int gClientW, gClientH; // the client area asked for (OS_WindowSetWindowed)
static int gPicW, gPicH;       // the picture (OS_WindowSetPicture)
static int gFullscreenStyle;   // OS_WindowSetFullscreen is in effect: no mapping (the mode is switched)
static int gMapOn;             // the picture is scaled into the client area
static double gScale = 1.0;    // picture -> client pixels
static int gOffX, gOffY;       // where the scaled picture sits in the client area

static void MapRecompute(void)
{
	double sx, sy;
	gMapOn = 0;
	gScale = 1.0;
	gOffX = gOffY = 0;
	if(gFullscreenStyle || gPicW <= 0 || gPicH <= 0 || gClientW <= 0 || gClientH <= 0 || (gClientW == gPicW && gClientH == gPicH))
		return;
	gMapOn = 1;
	sx = (double)gClientW / gPicW;
	sy = (double)gClientH / gPicH;
	gScale = sx < sy ? sx : sy;
	gOffX = (int)((gClientW - gPicW * gScale) / 2);
	gOffY = (int)((gClientH - gPicH * gScale) / 2);
}

int Win32_ToWinX(int x)
{
	return gMapOn ? (int)(x * gScale + 0.5) + gOffX : x;
}
int Win32_ToWinY(int y)
{
	return gMapOn ? (int)(y * gScale + 0.5) + gOffY : y;
}
int Win32_FromWinX(int x)
{
	return gMapOn ? (int)((x - gOffX) / gScale) : x;
}
int Win32_FromWinY(int y)
{
	return gMapOn ? (int)((y - gOffY) / gScale) : y;
}

void Win32_FillLetterbox(HDC hdc)
{
	RECT r;
	int pw, ph, own = 0;
	if(!gMapOn || !gWin32MainWnd)
		return;
	if(!hdc)
	{
		hdc = GetDC(gWin32MainWnd);
		own = 1;
	}
	pw = (int)(gPicW * gScale);
	ph = (int)(gPicH * gScale);
	if(gOffX > 0)
	{
		SetRect(&r, 0, 0, gOffX, gClientH);
		FillRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
		SetRect(&r, gOffX + pw, 0, gClientW, gClientH);
		FillRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
	}
	if(gOffY > 0)
	{
		SetRect(&r, 0, 0, gClientW, gOffY);
		FillRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
		SetRect(&r, 0, gOffY + ph, gClientW, gClientH);
		FillRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
	}
	if(own)
		ReleaseDC(gWin32MainWnd, hdc);
}

// the DC draws on the main window (the mapping applies there only)
static int IsMainDc(HDC hdc)
{
	return gMapOn && gWin32MainWnd && WindowFromDC(hdc) == gWin32MainWnd;
}

// ---- window creation ---------------------------------------------------------------------------

void OS_SetEventHandlers(const OsEventHandlers_t* h)
{
	gWin32Handlers = *h;
}

// the fixed frame of a WS_CAPTION pop-up: twice the fixed border in width, the caption plus twice the border in height; also stored for OS_WindowCreate
void OS_FrameMetrics(int* fw, int* fh)
{
	gWin32FrameW = GetSystemMetrics(SM_CXFIXEDFRAME) * 2;
	gWin32FrameH = GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFIXEDFRAME) * 2;
	*fw = gWin32FrameW;
	*fh = gWin32FrameH;
}

int OS_FrameTop(void) // the caption and the top border
{
	return GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFIXEDFRAME);
}

void OS_ScreenSize(int* w, int* h)
{
	*w = GetSystemMetrics(SM_CXSCREEN);
	*h = GetSystemMetrics(SM_CYSCREEN);
}

/* The two window classes of the original (BGI_MAIN_CLASS with its icon
 * and the arrow cursor, BGI_CHILD_CLASS sharing them) and the cursor
 * table; done once, 1 when both classes exist, 0 when one cannot be
 * registered. */
static int RegisterClasses(void)
{
	WNDCLASSEXW wc;
	if(gClassesRegistered)
		return 1;
	memset(&wc, 0, sizeof wc);
	wc.cbSize = sizeof wc;
	wc.style = CS_OWNDC | CS_DBLCLKS;
	wc.lpfnWndProc = Win32_MainWndProc;
	wc.hInstance = gWin32Instance;
	wc.hIcon = LoadIconW(gWin32Instance, MAKEINTRESOURCEW(0x65));
	wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	wc.hbrBackground = NULL;
	wc.lpszClassName = BGI_MAIN_CLASS;
	if(!RegisterClassExW(&wc))
		return 0;

	// CS_OWNDC | CS_NOCLOSE | CS_SAVEBITS | CS_BYTEALIGNCLIENT | CS_BYTEALIGNWINDOW
	wc.style = 0x3020;
	wc.lpfnWndProc = Win32_ChildWndProc;
	wc.lpszClassName = BGI_CHILD_CLASS;
	if(!RegisterClassExW(&wc))
		return 0;

	// the cursor table; entries 2..4 stay NULL
	gWin32Cursors[0] = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	gWin32Cursors[1] = LoadCursorW(gWin32Instance, MAKEINTRESOURCEW(0x6a));
	gWin32CursorShape = 0;
	gClassesRegistered = 1;
	return 1;
}

/* The main window, hidden, with the frame of OS_FrameMetrics added around
 * the client size and the title as OsCommon_MainTitle spells it; the EDIT
 * control is created with it.  `created` is raised from inside
 * CreateWindowEx (WM_CREATE).  0 when a class or the window cannot be
 * created. */
int OS_WindowCreate(const OsWindowDesc_t* d)
{
	HWND hwnd;
	char title[0x400];
	WCHAR wide[0x400];
	if(!RegisterClasses())
		return 0;
	OsCommon_MainTitle(title, sizeof title, d->title);
	Win32_ToWide(title, wide, (int)BGI_COUNTOF(wide));
	// WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, created hidden
	hwnd = CreateWindowExW(0, BGI_MAIN_CLASS, wide, 0x80ca0000, d->x, d->y,
		d->width + gWin32FrameW, d->height + gWin32FrameH, NULL, NULL, gWin32Instance, NULL);
	if(!hwnd)
		return 0;
	gWin32MainWnd = hwnd;

	Win32Edit_Create(hwnd);
	return 1;
}

void OS_WindowDestroy(void)
{
	if(gWin32MainWnd)
		DestroyWindow(gWin32MainWnd); // WM_DESTROY clears gWin32MainWnd
}

int OS_WindowAlive(void)
{
	return gWin32MainWnd != NULL && IsWindow(gWin32MainWnd);
}

// "80 64": show (SW_SHOWNORMAL, paint at once, and a frame refresh so the style set before showing takes) or hide
void OS_WindowShow(int show)
{
	if(!gWin32MainWnd)
		return;
	if(show)
	{
		ShowWindow(gWin32MainWnd, SW_SHOWNORMAL);
		UpdateWindow(gWin32MainWnd);
		SetWindowPos(gWin32MainWnd, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_FRAMECHANGED);
	}
	else
	{
		ShowWindow(gWin32MainWnd, SW_HIDE);
	}
}

// "80 66": the title as OsCommon_MainTitle spells it (NULL clears the game's part)
void OS_WindowSetTitle(const char* t)
{
	char title[0x400];
	WCHAR wide[0x400];
	if(!gWin32MainWnd)
		return;
	OsCommon_MainTitle(title, sizeof title, t);
	Win32_ToWide(title, wide, (int)BGI_COUNTOF(wide));
	SetWindowTextW(gWin32MainWnd, wide);
}

// move the outer origin without resizing or re-ordering; the engine re-centres the window with it after WM_DISPLAYCHANGE
void OS_WindowMove(int x, int y)
{
	if(gWin32MainWnd)
		SetWindowPos(gWin32MainWnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

// GetWindowRect: the outer rectangle in screen pixels; all zero without a window
void OS_WindowGetRect(int* x, int* y, int* w, int* h)
{
	RECT r = {0, 0, 0, 0};
	if(gWin32MainWnd)
		GetWindowRect(gWin32MainWnd, &r);
	*x = r.left;
	*y = r.top;
	*w = r.right - r.left;
	*h = r.bottom - r.top;
}

int OS_WindowMinimized(void)
{
	return gWin32MainWnd ? IsIconic(gWin32MainWnd) != 0 : 0;
}

int OS_WindowActive(void) // 1 when the main window is the foreground window
{
	return gWin32MainWnd && GetForegroundWindow() == gWin32MainWnd;
}

void OS_WindowMinimize(void) // "80 65": CloseWindow, which minimises despite its name
{
	if(gWin32MainWnd)
		CloseWindow(gWin32MainWnd);
}

void OS_WindowClose(void) // SendMessage(WM_CLOSE), as Alt+F4 does; `close_request` is raised from inside the call
{
	if(gWin32MainWnd)
		SendMessageW(gWin32MainWnd, WM_CLOSE, 0, 0);
}

/* The status window of the main window's default IME window: closed (0)
 * before the interpreter runs and opened (1) again afterwards
 * (src/sys/engine.c brackets the run with the two calls, as WinMain does
 * in the original).  Posted, so it takes effect on a later pump. */
void OS_ImeStatus(int open)
{
	HWND ime;
	if(!gWin32MainWnd)
		return;
	ime = ImmGetDefaultIMEWnd(gWin32MainWnd);
	if(ime)
		PostMessageW(ime, WM_IME_CONTROL, open ? IMC_OPENSTATUSWINDOW : IMC_CLOSESTATUSWINDOW, 0);
}

// ---- display mode ------------------------------------------------------------------------------

/* ChangeDisplaySettings(CDS_FULLSCREEN) to w x h at bpp bits when
 * `fullscreen` is set, the desktop mode again otherwise (always 1).  0
 * when the mode is refused; the engine then tries the next depth and
 * falls back to windowed mode when every depth fails, as the original did
 * with IDirectDraw::SetDisplayMode. */
int OS_DisplaySetMode(int fullscreen, int w, int h, int bpp)
{
	DEVMODEW dm;
	if(!fullscreen)
	{
		OS_DisplayRestore();
		return 1;
	}
	memset(&dm, 0, sizeof dm);
	dm.dmSize = sizeof dm;
	dm.dmPelsWidth = (DWORD)w;
	dm.dmPelsHeight = (DWORD)h;
	dm.dmBitsPerPel = (DWORD)bpp;
	dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
	if(ChangeDisplaySettingsW(&dm, CDS_FULLSCREEN) != DISP_CHANGE_SUCCESSFUL)
		return 0;
	gModeChanged = 1;
	return 1;
}

void OS_DisplayRestore(void) // back to the desktop mode (ChangeDisplaySettings(NULL, 0)) when a mode switch is in effect
{
	if(gModeChanged)
		ChangeDisplaySettingsW(NULL, 0);
	gModeChanged = 0;
}

// a borderless WS_POPUP window covering the mode from (0, 0)
void OS_WindowSetFullscreen(int modeW, int modeH)
{
	if(!gWin32MainWnd)
		return;
	gFullscreenStyle = 1;
	MapRecompute();
	SetWindowLongW(gWin32MainWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
	SetWindowPos(gWin32MainWnd, NULL, 0, 0, modeW, modeH, SWP_FRAMECHANGED);
}

// the framed window again at the outer origin (x, y), sized for the client area
void OS_WindowSetWindowed(int x, int y, int clientW, int clientH)
{
	if(!gWin32MainWnd)
		return;
	gFullscreenStyle = 0;
	gClientW = clientW;
	gClientH = clientH;
	MapRecompute();
	// WS_VISIBLE | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX
	SetWindowLongW(gWin32MainWnd, GWL_STYLE, 0x90ca0000);
	SetWindowPos(gWin32MainWnd, NULL, x, y, clientW + gWin32FrameW, clientH + gWin32FrameH, SWP_FRAMECHANGED);
}

// the picture the engine draws: scaled into a windowed client area of another size (MapRecompute)
void OS_WindowSetPicture(int w, int h)
{
	if(w <= 0 || h <= 0)
		return;
	gPicW = w;
	gPicH = h;
	MapRecompute();
	Win32_FillLetterbox(NULL);
}

// ---- presentation ------------------------------------------------------------------------------

OsDc_t* OS_WindowGetDc(void)
{
	return gWin32MainWnd ? (OsDc_t*)GetDC(gWin32MainWnd) : NULL;
}

void OS_WindowReleaseDc(OsDc_t* dc)
{
	if(dc && gWin32MainWnd)
		ReleaseDC(gWin32MainWnd, (HDC)dc);
}

/* A BITMAPINFOHEADER for `rows` rows of a surface as the original builds
 * one: top-down (negative height), BI_RGB at the surface depth,
 * so a 16-bit surface is RGB 5-5-5 and a 32-bit one 0x00RRGGBB.  biWidth
 * is taken from the pitch so any 4-byte padding is honoured. */
void Win32_SurfaceHeader(BITMAPINFOHEADER* bi, const OsSurface_t* s, int rows)
{
	int bytesPP = s->bpp / 8;
	memset(bi, 0, sizeof *bi);
	bi->biSize = sizeof *bi;
	bi->biWidth = bytesPP ? s->pitch / bytesPP : s->width;
	bi->biHeight = -rows;
	bi->biPlanes = 1;
	bi->biBitCount = (WORD)s->bpp;
	bi->biCompression = BI_RGB;
	bi->biSizeImage = (DWORD)(s->pitch * rows);
}

/* w x h pixels of the surface from (srcX, srcY) to (dstX, dstY) of the
 * window, unscaled: the original's BitBlt from its memory DC, done here
 * as a 1:1 StretchDIBits.  `dc` NULL takes and releases the main
 * window's own; nothing happens without a window, an empty rectangle or
 * a surface without pixels. */
void OS_BlitToWindow(OsDc_t* dc, int dstX, int dstY, int w, int h, const OsSurface_t* src, int srcX, int srcY)
{
	BITMAPINFOHEADER bi;
	HDC hdc = (HDC)dc;
	int own = 0;
	const uint8_t* bits;
	if(w <= 0 || h <= 0 || !src->pixels)
		return;
	if(!hdc)
	{
		if(!gWin32MainWnd)
			return;
		hdc = GetDC(gWin32MainWnd);
		own = 1;
	}
	/* the source rectangle starts at row srcY: pointing the DIB at that row
	 * keeps ySrc at 0, which is unambiguous for a top-down DIB */
	bits = (const uint8_t*)src->pixels + (size_t)srcY * (size_t)src->pitch;
	Win32_SurfaceHeader(&bi, src, h);
	if(own || IsMainDc(hdc))
	{ // a scaled window: the destination goes through the mapping
		int x0 = Win32_ToWinX(dstX), y0 = Win32_ToWinY(dstY);
		int x1 = Win32_ToWinX(dstX + w), y1 = Win32_ToWinY(dstY + h);
		if(x1 - x0 != w || y1 - y0 != h)
		{
			int oldMode = SetStretchBltMode(hdc, HALFTONE);
			POINT oldOrg = {0, 0};
			SetBrushOrgEx(hdc, 0, 0, &oldOrg);
			StretchDIBits(hdc, x0, y0, x1 - x0, y1 - y0, srcX, 0, w, h, bits, (const BITMAPINFO*)&bi, DIB_RGB_COLORS, SRCCOPY);
			SetStretchBltMode(hdc, oldMode);
			SetBrushOrgEx(hdc, oldOrg.x, oldOrg.y, NULL);
			if(own)
				ReleaseDC(gWin32MainWnd, hdc);
			return;
		}
		dstX = x0;
		dstY = y0;
	}
	StretchDIBits(hdc, dstX, dstY, w, h, srcX, 0, w, h, bits, (const BITMAPINFO*)&bi, DIB_RGB_COLORS, SRCCOPY);
	if(own)
		ReleaseDC(gWin32MainWnd, hdc);
}

/* The whole surface stretched to dstW x dstH at (dstX, dstY).  With `clip`
 * ({left, top, right, bottom}, right / bottom exclusive) the output is
 * clipped to that rectangle and filtered (HALFTONE), and the context is
 * put back as it was; without one the context is used as it is.  `dc`
 * NULL takes the main window's own. */
void OS_StretchToWindow(OsDc_t* dc, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* src, const int* clip)
{
	BITMAPINFOHEADER bi;
	HDC hdc = (HDC)dc;
	HRGN rgn = NULL;
	POINT oldOrg = {0, 0};
	int oldMode = 0, own = 0;
	int mapped[4];
	if(!src->pixels || dstW <= 0 || dstH <= 0)
		return;
	if(!hdc)
	{
		if(!gWin32MainWnd)
			return;
		hdc = GetDC(gWin32MainWnd);
		own = 1;
	}
	Win32_SurfaceHeader(&bi, src, src->height);
	if(own || IsMainDc(hdc))
	{ // a scaled window: the destination and the clip go through the mapping
		int x0 = Win32_ToWinX(dstX), y0 = Win32_ToWinY(dstY);
		int x1 = Win32_ToWinX(dstX + dstW), y1 = Win32_ToWinY(dstY + dstH);
		dstX = x0;
		dstY = y0;
		dstW = x1 - x0;
		dstH = y1 - y0;
		if(clip)
		{
			mapped[0] = Win32_ToWinX(clip[0]);
			mapped[1] = Win32_ToWinY(clip[1]);
			mapped[2] = Win32_ToWinX(clip[2]);
			mapped[3] = Win32_ToWinY(clip[3]);
			clip = mapped;
		}
	}
	if(clip)
	{
		rgn = CreateRectRgn(clip[0], clip[1], clip[2], clip[3]);
		SelectClipRgn(hdc, rgn);
		oldMode = SetStretchBltMode(hdc, HALFTONE);
		SetBrushOrgEx(hdc, 0, 0, &oldOrg); // HALFTONE needs the brush origin re-asserted
	}
	StretchDIBits(hdc, dstX, dstY, dstW, dstH, 0, 0, src->width, src->height, src->pixels,
		(const BITMAPINFO*)&bi, DIB_RGB_COLORS, SRCCOPY);
	if(clip)
	{
		SetStretchBltMode(hdc, oldMode);
		SetBrushOrgEx(hdc, oldOrg.x, oldOrg.y, NULL);
		SelectClipRgn(hdc, NULL);
		DeleteObject(rgn);
	}
	if(own)
		ReleaseDC(gWin32MainWnd, hdc);
}

void OS_WindowFillBlack(void) // the whole client area black: the `paint` handler's answer while there is no picture yet
{
	RECT rc;
	HDC hdc;
	if(!gWin32MainWnd)
		return;
	GetClientRect(gWin32MainWnd, &rc);
	hdc = GetDC(gWin32MainWnd);
	FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
	ReleaseDC(gWin32MainWnd, hdc);
}

// ---- cursor and keyboard state -----------------------------------------------------------------

// "80 67": remember the shape (0..4; anything else is the arrow); WM_SETCURSOR applies it
void OS_CursorSetShape(int shape)
{
	gWin32CursorShape = (shape >= 0 && shape < 5) ? shape : 0;
}

// ShowCursor(TRUE / FALSE): the system counts the calls; src/sys/cursor.c keeps its own shown flag and calls this when it flips (and again after a mode switch, which resets the count)
void OS_CursorShow(int show)
{
	ShowCursor(show != 0);
}

void OS_CursorGetPos(int* x, int* y)
{
	POINT p;
	GetCursorPos(&p);
	*x = p.x;
	*y = p.y;
}

void OS_CursorSetPos(int x, int y)
{
	SetCursorPos(x, y);
}

void OS_ScreenToClient(int* x, int* y) // unchanged without a window; client coordinates are picture pixels (the mapping)
{
	POINT p;
	p.x = *x;
	p.y = *y;
	if(gWin32MainWnd)
		ScreenToClient(gWin32MainWnd, &p);
	*x = Win32_FromWinX(p.x);
	*y = Win32_FromWinY(p.y);
}

void OS_ClientToScreen(int* x, int* y) // unchanged without a window
{
	POINT p;
	p.x = Win32_ToWinX(*x);
	p.y = Win32_ToWinY(*y);
	if(gWin32MainWnd)
		ClientToScreen(gWin32MainWnd, &p);
	*x = p.x;
	*y = p.y;
}

int OS_MouseButtonsSwapped(void) // GetSystemMetrics(SM_SWAPBUTTON): the user swapped the left and right buttons
{
	return GetSystemMetrics(SM_SWAPBUTTON) != 0;
}

int OS_KeyState(int vk) // GetAsyncKeyState: the key is down now
{
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void OS_KeyboardState(uint8_t out[256])
{
	GetKeyboardState(out);
}
