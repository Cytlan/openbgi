/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.c - the Win32 back end's main window (src/host/os.h): its
 *            creation, the display mode, GDI presentation, and the cursor
 *            and keyboard state
 *
 * This is the original's windowing code (window creation, the display mode
 * part of its mode switch, the DC blit of its presentation) with the
 * engine's decisions removed: wherever the original called into the
 * interpreter, this back end raises the matching handler of the host's
 * (OsHostHandlers_t, wndproc.c), which hands it to the engine
 * (src/sys/window.c), and the engine does what the original did there.  The child windows are in child.c, the EDIT control
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
 *   * Where the system has a graphics processor to scale with (wgl.c), the
 *     picture is shown through OpenGL, by the rule the player chose, and
 *     the display mode is not switched at all: in full screen the window
 *     covers its monitor without a frame and the mode is scaled onto it.
 *   * The back buffer is plain memory, so presenting uses StretchDIBits
 *     where the original selected its DIB section into a memory DC and
 *     called BitBlt.  The pixels copied are the same.
 *   * The icon (0x65) and the cursor (0x6A) are resources of the original
 *     executable.  They are loaded when a resource script provides them and
 *     silently absent otherwise (LoadIcon / LoadCursor return NULL and the
 *     class falls back to no icon / the arrow).
 */
#include "win32_internal.h"

#include "host/os_gl.h"

HWND gWin32MainWnd;            // the main window; NULL before OS_WindowCreate and after WM_DESTROY
OsHostHandlers_t gWin32Input;  // the host's handlers, copied by OS_SetHostHandlers
static int gClassesRegistered; // RegisterClasses has run (the classes outlive the window)

// the style of the framed window: WS_VISIBLE | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, and with a sizing border its frame and the maximise box
#define STYLE_FIXED     0x90ca0000u
#define STYLE_RESIZABLE (STYLE_FIXED | WS_THICKFRAME | WS_MAXIMIZEBOX)
#define MIN_CLIENT_W    320 // the smallest client area the player can make
#define MIN_CLIENT_H    240

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
static int gClientW, gClientH; // the client area asked for (OS_WindowSetWindowed), or the one the player made (Win32_ClientResized)
static int gResizable;         // OS_WindowSetResizable: the window has a sizing border
static int gUserSized;         // .. and the player gave it a size of his own
static int gOwnSize;           // the back end is setting the window's size itself: a WM_SIZE meanwhile is not the player's
static int gAspectW, gAspectH; // OS_WindowSetAspect: the proportions the client area keeps while the player sizes it (0: any)
static int gPicW, gPicH;       // the picture (OS_WindowSetPicture)
static int gFullscreenStyle;   // OS_WindowSetFullscreen is in effect
static int gMapOn;             // the picture is scaled into the client area
static int gScaleRule;         // OS_WindowSetScaling: OPENBGI_SCALE_*
static double gScale = 1.0;    // picture -> client pixels
static int gOffX, gOffY;       // where the scaled picture sits in the client area
static int gSrcW, gSrcH;       // what is scaled: the picture, or the mode in full screen
static int gDstW, gDstH;       // .. and into what: the client area, or the monitor

/* Full screen without a mode switch, where the graphics processor shows
 * the picture: the window covers its monitor (gFs*: the monitor's place
 * and size) and the mode the engine asked for is scaled onto it. */
static int gBorderless;    // OS_DisplaySetMode chose this way for the full screen asked for
static int gModeW, gModeH; // the mode
static int gFsX, gFsY, gFsW, gFsH;
static DWORD gClip;          // the styles a window drawn on through OpenGL has besides: WS_CLIPCHILDREN | WS_CLIPSIBLINGS
static int gGlDirty;         // the picture's texture changed since it was last drawn (OS_WindowPresent)
static int gGlPicW, gGlPicH; // .. the picture's size then

static void MapRecompute(void)
{
	double sx, sy;
	gMapOn = 0;
	gScale = 1.0;
	gOffX = gOffY = 0;
	gSrcW = gPicW;
	gSrcH = gPicH;
	gDstW = gClientW;
	gDstH = gClientH;
	if(gFullscreenStyle)
	{
		if(!gBorderless)
			return; // (the mode is switched: the picture is the screen)
		gSrcW = gModeW;
		gSrcH = gModeH;
		gDstW = gFsW;
		gDstH = gFsH;
	}
	if(gSrcW <= 0 || gSrcH <= 0 || gDstW <= 0 || gDstH <= 0 || (gDstW == gSrcW && gDstH == gSrcH))
		return;
	gMapOn = 1;
	sx = (double)gDstW / gSrcW;
	sy = (double)gDstH / gSrcH;
	gScale = sx < sy ? sx : sy;
	if((gScaleRule & OPENBGI_SCALE_WHOLE) && gScale >= 1.0)
		gScale = (double)(int)gScale; // by whole numbers: the rest is letterboxed
	gOffX = (int)((gDstW - gSrcW * gScale) / 2);
	gOffY = (int)((gDstH - gSrcH * gScale) / 2);
}

// the mapping anew, and what lies on the picture with it: the text entry, the system player's video
static void MapChanged(void)
{
	MapRecompute();
	Win32Edit_Remap();
	OsWin32_MovieRemap();
}

double Win32_MapScale(void)
{
	return gMapOn ? gScale : 1.0;
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
	if(!gMapOn || !gWin32MainWnd || Win32Gl_On()) // (through OpenGL the bars are drawn with the picture)
		return;
	if(!hdc)
	{
		hdc = GetDC(gWin32MainWnd);
		own = 1;
	}
	pw = (int)(gSrcW * gScale);
	ph = (int)(gSrcH * gScale);
	if(gOffX > 0)
	{
		SetRect(&r, 0, 0, gOffX, gDstH);
		FillRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
		SetRect(&r, gOffX + pw, 0, gDstW, gDstH);
		FillRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
	}
	if(gOffY > 0)
	{
		SetRect(&r, 0, 0, gDstW, gOffY);
		FillRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
		SetRect(&r, 0, gOffY + ph, gDstW, gDstH);
		FillRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
	}
	if(own)
		ReleaseDC(gWin32MainWnd, hdc);
}

// the DC draws on the main window (the mapping and the graphics processor apply there only)
static int IsMainDc(HDC hdc)
{
	return gWin32MainWnd && WindowFromDC(hdc) == gWin32MainWnd;
}

// ---- window creation ---------------------------------------------------------------------------

void OS_SetHostHandlers(const OsHostHandlers_t* h)
{
	if(h)
		gWin32Input = *h;
	else
		memset(&gWin32Input, 0, sizeof gWin32Input);
}

// ---- the screen's scale ------------------------------------------------------------------------

/* What the system has for a program that follows the scale of each
 * monitor itself is not in every Windows the program runs on, so the
 * functions are looked for at run time: the awareness of Windows 10 1703
 * (per monitor, version 2: the frame follows too), of Windows 8.1 (per
 * monitor) and of Vista (the one scale of the system). */
typedef void (*Proc_t)(void);

static Proc_t Find(const WCHAR* dll, const char* name)
{
	HMODULE m = GetModuleHandleW(dll);
	if(!m)
		m = LoadLibraryW(dll);
	return m ? (Proc_t)GetProcAddress(m, name) : NULL;
}

static UINT(WINAPI* gDpiForWindow)(HWND);                            // GetDpiForWindow (Windows 10 1607)
static HRESULT(WINAPI* gDpiForMonitor)(HMONITOR, int, UINT*, UINT*); // GetDpiForMonitor (Windows 8.1)
static BOOL(WINAPI* gAdjustForDpi)(RECT*, DWORD, BOOL, DWORD, UINT); // AdjustWindowRectExForDpi (Windows 10 1607)
static int gDpiLooked;                                               // the three were looked for

static void DpiFunctions(void)
{
	if(gDpiLooked)
		return;
	gDpiLooked = 1;
	gDpiForWindow = (UINT(WINAPI*)(HWND))Find(L"user32.dll", "GetDpiForWindow");
	gAdjustForDpi = (BOOL(WINAPI*)(RECT*, DWORD, BOOL, DWORD, UINT))Find(L"user32.dll", "AdjustWindowRectExForDpi");
	gDpiForMonitor = (HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*))Find(L"shcore.dll", "GetDpiForMonitor");
}

/* The process says that it follows the screen's scale itself, so that the
 * system does not show its windows enlarged as pictures; before the first
 * window.  BGI_DPI=off leaves it unsaid: the system scales as before, and
 * tells the program of a screen without a scale. */
void OsWin32_DeclareDpiAware(void)
{
	const char* e = getenv("BGI_DPI");
	BOOL(WINAPI * context)
	(HANDLE);
	HRESULT(WINAPI * awareness)
	(int);
	BOOL(WINAPI * aware)
	(void);
	if(e && strcmp(e, "off") == 0)
		return;
	context = (BOOL(WINAPI*)(HANDLE))Find(L"user32.dll", "SetProcessDpiAwarenessContext");
	if(context && context((HANDLE)(LONG_PTR)-4)) // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
		return;
	awareness = (HRESULT(WINAPI*)(int))Find(L"shcore.dll", "SetProcessDpiAwareness");
	if(awareness && SUCCEEDED(awareness(2))) // PROCESS_PER_MONITOR_DPI_AWARE
		return;
	aware = (BOOL(WINAPI*)(void))Find(L"user32.dll", "SetProcessDPIAware");
	if(aware)
		aware();
}

// the monitor the window is on; before there is a window the primary one, where the scripts will centre it
static HMONITOR WindowMonitor(void)
{
	POINT origin = {0, 0};
	return gWin32MainWnd ? MonitorFromWindow(gWin32MainWnd, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

int OS_WindowFits(void)
{
	return 1;
}

// the scale of the window's monitor in dots per inch (96: none)
int OS_WindowDpi(void)
{
	UINT x = 0, y = 0;
	HDC dc;
	int dpi;
	DpiFunctions();
	if(gWin32MainWnd && gDpiForWindow && (x = gDpiForWindow(gWin32MainWnd)) != 0)
		return (int)x;
	if(gDpiForMonitor && SUCCEEDED(gDpiForMonitor(WindowMonitor(), 0, &x, &y)) && x != 0) // MDT_EFFECTIVE_DPI
		return (int)x;
	dc = GetDC(NULL);
	dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
	if(dc)
		ReleaseDC(NULL, dc);
	return dpi > 0 ? dpi : 96;
}

// what the window's monitor has room for beside the task bar
void OS_WindowWorkArea(int* x, int* y, int* w, int* h)
{
	MONITORINFO mi;
	memset(&mi, 0, sizeof mi);
	mi.cbSize = sizeof mi;
	*x = *y = 0;
	OS_ScreenSize(w, h);
	if(!GetMonitorInfoW(WindowMonitor(), &mi) || mi.rcWork.right <= mi.rcWork.left || mi.rcWork.bottom <= mi.rcWork.top)
		return;
	*x = mi.rcWork.left;
	*y = mi.rcWork.top;
	*w = mi.rcWork.right - mi.rcWork.left;
	*h = mi.rcWork.bottom - mi.rcWork.top;
}

/* The smallest window the player can make (WM_GETMINMAXINFO): the
 * smallest client area, in the proportions of OS_WindowSetAspect when
 * there are any, with the frame. */
void Win32_MinTrack(POINT* min)
{
	int fw, fh, h = MIN_CLIENT_H;
	if(!gResizable || gFullscreenStyle)
		return;
	OS_FrameMetrics(&fw, &fh);
	if(gAspectW > 0 && gAspectH > 0)
		h = MulDiv(MIN_CLIENT_W, gAspectH, gAspectW);
	min->x = MIN_CLIENT_W + fw;
	min->y = (h > 0 ? h : 1) + fh;
}

/* The frame of the window with its sizing border, which it always has
 * (the engine fits the window to its screen and lets the player size it:
 * OS_WindowFits), at the scale of its monitor: the width and the height
 * it adds to the client area, the caption with the top border. */
static void Frame(int* fw, int* fh, int* top)
{
	RECT r = {0, 0, 0, 0};
	DpiFunctions();
	if(!gAdjustForDpi || !gAdjustForDpi(&r, STYLE_RESIZABLE, FALSE, 0, (UINT)OS_WindowDpi()))
	{
		SetRect(&r, 0, 0, 0, 0);
		AdjustWindowRectEx(&r, STYLE_RESIZABLE, FALSE, 0);
	}
	*fw = r.right - r.left;
	*fh = r.bottom - r.top;
	*top = -r.top;
}

void OS_FrameMetrics(int* fw, int* fh)
{
	int top;
	Frame(fw, fh, &top);
}

int OS_FrameTop(void) // the caption and the top border
{
	int fw, fh, top;
	Frame(&fw, &fh, &top);
	return top;
}

// the primary monitor, in its own pixels
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

	// CS_OWNDC | CS_BYTEALIGNCLIENT | CS_BYTEALIGNWINDOW
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

static void ApplyIcon(void); // the icon OS_WindowSetIcon keeps, onto the window

/* The main window, hidden, with its sizing border and the frame of
 * OS_FrameMetrics added around the client size, and the title as
 * OsCommon_MainTitle spells it; the EDIT control is created with it.  `created` is raised from inside
 * CreateWindowEx (WM_CREATE).  0 when a class or the window cannot be
 * created. */
int OS_WindowCreate(const OsWindowDesc_t* d)
{
	HWND hwnd;
	char title[0x400];
	WCHAR wide[0x400];
	int fw, fh, top;
	if(!RegisterClasses())
		return 0;
	OsCommon_MainTitle(title, sizeof title, d->title);
	Win32_ToWide(title, wide, (int)BGI_COUNTOF(wide));
	gResizable = 1; // (the engine asks for the border at once: OS_WindowFits)
	gUserSized = 0;
	gAspectW = gAspectH = 0;
	gFullscreenStyle = 0;
	gClientW = d->width;
	gClientH = d->height;
	gClip = Win32Gl_Usable() ? WS_CLIPCHILDREN | WS_CLIPSIBLINGS : 0;
	gGlDirty = 0;
	Frame(&fw, &fh, &top);
	// the framed window's style, created hidden
	gOwnSize++;
	hwnd = CreateWindowExW(0, BGI_MAIN_CLASS, wide, (STYLE_RESIZABLE & ~(DWORD)WS_VISIBLE) | gClip, d->x, d->y, d->width + fw, d->height + fh, NULL,
		NULL, gWin32Instance, NULL);
	gOwnSize--;
	if(!hwnd)
		return 0;
	gWin32MainWnd = hwnd;
	ApplyIcon();
	Win32Gl_Attach(hwnd);

	if(!Win32Gl_Usable())
		Win32Edit_Create(hwnd); // (with the graphics processor the engine draws the entry itself: OS_TextEntry)
	return 1;
}

void OS_WindowDestroy(void)
{
	if(gWin32MainWnd)
		DestroyWindow(gWin32MainWnd); // WM_DESTROY clears gWin32MainWnd
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

// GetWindowRect: the outer rectangle in screen pixels; all zero without a window.  Over a monitor whose mode was not switched, the mode at (0, 0): what it would be with the mode switched
void OS_WindowGetRect(int* x, int* y, int* w, int* h)
{
	RECT r = {0, 0, 0, 0};
	if(gFullscreenStyle && gBorderless)
	{
		*x = *y = 0;
		*w = gModeW;
		*h = gModeH;
		return;
	}
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

void OS_WindowMinimize(void) // "80 65": CloseWindow, which minimises despite its name
{
	if(gWin32MainWnd)
		CloseWindow(gWin32MainWnd);
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

/* The engine takes typed text: the IME's status window shown and its
 * composition window put at the field (CFS_POINT, in client pixels)
 * while it does, hidden again after */
void OS_TextInput(int on, int x, int y, int w, int h)
{
	HIMC imc;
	OS_ImeStatus(on);
	if(!on || !gWin32MainWnd || !(imc = ImmGetContext(gWin32MainWnd)))
		return;
	{
		COMPOSITIONFORM cf;
		memset(&cf, 0, sizeof cf);
		cf.dwStyle = CFS_POINT;
		cf.ptCurrentPos.x = Win32_ToWinX(x);
		cf.ptCurrentPos.y = Win32_ToWinY(y);
		ImmSetCompositionWindow(imc, &cf);
	}
	ImmReleaseContext(gWin32MainWnd, imc);
}

// the message boxes and the dialogs are the system's (dialogs.c)
uint32_t OS_HostCaps(void)
{
	return OPENBGI_CAP_DIALOGS | OPENBGI_CAP_POINTER | OPENBGI_CAP_KEYBOARD;
}

// ---- display mode ------------------------------------------------------------------------------

/* ChangeDisplaySettings(CDS_FULLSCREEN) to w x h at bpp bits when
 * `fullscreen` is set, the desktop mode again otherwise (always 1).  0
 * when the mode is refused; the engine then tries the next depth and
 * falls back to windowed mode when every depth fails, as the original did
 * with IDirectDraw::SetDisplayMode.  While the graphics processor shows
 * the picture nothing is switched, and every mode will do. */
int OS_DisplaySetMode(int fullscreen, int w, int h, int bpp)
{
	DEVMODEW dm;
	if(!fullscreen)
	{
		OS_DisplayRestore();
		return 1;
	}
	gBorderless = gWin32MainWnd ? Win32Gl_On() : Win32Gl_Usable();
	if(gBorderless)
	{ // no switch: the mode is remembered, and scaled onto the monitor by the graphics processor (OS_WindowSetFullscreen)
		if(w <= 0 || h <= 0)
			return 0;
		OS_DisplayRestore();
		gModeW = w;
		gModeH = h;
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

// a borderless WS_POPUP window covering the mode from (0, 0) - or, the mode not switched, the monitor the window is on, with the mode scaled onto it
void OS_WindowSetFullscreen(int modeW, int modeH)
{
	int x = 0, y = 0, w = modeW, h = modeH;
	if(!gWin32MainWnd)
		return;
	if(gBorderless)
	{
		MONITORINFO mi;
		memset(&mi, 0, sizeof mi);
		mi.cbSize = sizeof mi;
		gModeW = modeW;
		gModeH = modeH;
		if(GetMonitorInfoW(WindowMonitor(), &mi) && mi.rcMonitor.right > mi.rcMonitor.left && mi.rcMonitor.bottom > mi.rcMonitor.top)
		{
			x = mi.rcMonitor.left;
			y = mi.rcMonitor.top;
			w = mi.rcMonitor.right - mi.rcMonitor.left;
			h = mi.rcMonitor.bottom - mi.rcMonitor.top;
		}
		gFsX = x;
		gFsY = y;
		gFsW = w;
		gFsH = h;
	}
	gFullscreenStyle = 1;
	MapChanged();
	SetWindowLongW(gWin32MainWnd, GWL_STYLE, (LONG)(WS_POPUP | WS_VISIBLE | gClip));
	SetWindowPos(gWin32MainWnd, NULL, x, y, w, h, SWP_FRAMECHANGED);
}

// WM_DISPLAYCHANGE: a window that covers its monitor without a mode switch covers it as it is now
void Win32_DisplayChanged(void)
{
	if(gWin32MainWnd && gFullscreenStyle && gBorderless)
	{
		OS_WindowSetFullscreen(gModeW, gModeH);
		InvalidateRect(gWin32MainWnd, NULL, FALSE);
	}
}

// the outer size of the window around a client area, at the scale of its monitor
static void Outer(int clientW, int clientH, int* w, int* h)
{
	int fw, fh, top;
	Frame(&fw, &fh, &top);
	*w = clientW + fw;
	*h = clientH + fh;
}

// the framed window again at the outer origin (x, y), sized for the client area
void OS_WindowSetWindowed(int x, int y, int clientW, int clientH)
{
	int w, h;
	RECT rc;
	if(!gWin32MainWnd)
		return;
	if(gResizable && gUserSized && !gFullscreenStyle && clientW == gClientW && clientH == gClientH)
		return; // the size the player gave it
	gFullscreenStyle = 0;
	gUserSized = 0;
	gClientW = clientW;
	gClientH = clientH;
	Outer(clientW, clientH, &w, &h);
	gOwnSize++;
	SetWindowLongW(gWin32MainWnd, GWL_STYLE, (LONG)((gResizable ? STYLE_RESIZABLE : STYLE_FIXED) | gClip));
	SetWindowPos(gWin32MainWnd, NULL, x, y, w, h, SWP_FRAMECHANGED);
	if(GetClientRect(gWin32MainWnd, &rc) && rc.right > 0 && rc.bottom > 0)
	{ // (what the system made of it: on a monitor of another scale the frame is another)
		gClientW = rc.right;
		gClientH = rc.bottom;
	}
	gOwnSize--;
	MapChanged();
}

// "81 66": the sizing border comes or goes around the client area as it is
void OS_WindowSetResizable(int on)
{
	RECT rc;
	int w, h;
	LONG shown;
	if(gResizable == (on != 0) && gWin32MainWnd)
		return;
	gResizable = on != 0;
	if(!gWin32MainWnd || gFullscreenStyle || !GetClientRect(gWin32MainWnd, &rc))
		return;
	shown = GetWindowLongW(gWin32MainWnd, GWL_STYLE) & WS_VISIBLE;
	if(rc.right > 0 && rc.bottom > 0)
	{
		gClientW = rc.right;
		gClientH = rc.bottom;
	}
	Outer(gClientW, gClientH, &w, &h);
	gOwnSize++;
	SetWindowLongW(gWin32MainWnd, GWL_STYLE, (LONG)(((gResizable ? STYLE_RESIZABLE : STYLE_FIXED) & ~(DWORD)WS_VISIBLE) | (DWORD)shown | gClip));
	SetWindowPos(gWin32MainWnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
	gOwnSize--;
}

/* WM_SIZE of the framed window: 1 when a window with a sizing border got
 * another client size from the player (the picture is scaled into it).
 * A size that comes of the back end's own call is taken as it is and is
 * nobody's news. */
int Win32_ClientResized(int w, int h)
{
	if(!gResizable || gFullscreenStyle || w <= 0 || h <= 0 || (w == gClientW && h == gClientH))
		return 0;
	gClientW = w;
	gClientH = h;
	MapChanged();
	Win32_FillLetterbox(NULL);
	if(gOwnSize)
		return 0;
	gUserSized = 1;
	return 1;
}

// more (+1) or less (-1) of the back end's own sizing: wndproc.c counts the rest of a drag in which the monitor's scale changed as such
void Win32_OwnSize(int by)
{
	gOwnSize += by;
}

void OS_WindowSetAspect(int w, int h)
{
	if(w <= 0 || h <= 0)
		w = h = 0;
	gAspectW = w;
	gAspectH = h;
}

/* WM_SIZING: the rectangle the player is dragging the window to, made to
 * keep the client area in the proportions of OS_WindowSetAspect - the
 * height after the width, but the width after the height when the top or
 * the bottom edge alone is dragged - and no smaller than the smallest
 * client area. */
void Win32_KeepAspect(int edge, RECT* r)
{
	RECT wr, cr;
	int fw, fh, cw, ch;
	if(!gWin32MainWnd || gFullscreenStyle || !GetWindowRect(gWin32MainWnd, &wr) || !GetClientRect(gWin32MainWnd, &cr))
		return;
	fw = (wr.right - wr.left) - cr.right;
	fh = (wr.bottom - wr.top) - cr.bottom;
	cw = (r->right - r->left) - fw;
	ch = (r->bottom - r->top) - fh;
	if(cw < MIN_CLIENT_W)
		cw = MIN_CLIENT_W;
	if(ch < 1)
		ch = 1;
	if(gAspectW > 0 && gAspectH > 0)
	{
		if(edge == WMSZ_TOP || edge == WMSZ_BOTTOM)
			cw = MulDiv(ch, gAspectW, gAspectH) > MIN_CLIENT_W ? MulDiv(ch, gAspectW, gAspectH) : MIN_CLIENT_W;
		ch = MulDiv(cw, gAspectH, gAspectW);
	}
	else if(ch < MIN_CLIENT_H)
		ch = MIN_CLIENT_H;
	// the edges that are not dragged stay where they are
	if(edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
		r->left = r->right - cw - fw;
	else
		r->right = r->left + cw + fw;
	if(edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
		r->top = r->bottom - ch - fh;
	else
		r->bottom = r->top + ch + fh;
}

// the rule of the scaling; the picture is shown again by it at its next frame or repaint
void OS_WindowSetScaling(int rule)
{
	int was = gScaleRule;
	gScaleRule = rule;
	if(rule == was || !gWin32MainWnd)
		return;
	if((rule ^ was) & OPENBGI_SCALE_WHOLE)
		MapChanged(); // (the steps decide how large the picture is)
	InvalidateRect(gWin32MainWnd, NULL, TRUE);
}

int OS_WindowScales(void)
{
	return 1;
}

// the filters of the graphics processor's own (none without one)
const char* OS_WindowFilterName(int n)
{
	return Win32Gl_On() ? OsGl_FilterName(n) : NULL;
}

/* The picture through the graphics processor, once for all that a frame
 * changed: into the place the mapping gives it (the whole client area
 * when it is not scaled), by the rule of OS_WindowSetScaling, black
 * around it.  Without one every blit is shown as it is made, and there is
 * nothing to do here. */
void OS_WindowPresent(void)
{
	RECT rc;
	int x = 0, y = 0, w, h;
	if(!gGlDirty || !Win32Gl_On() || !gWin32MainWnd || !GetClientRect(gWin32MainWnd, &rc) || rc.right <= 0 || rc.bottom <= 0)
		return;
	gGlDirty = 0;
	w = rc.right;
	h = rc.bottom;
	if(gMapOn)
	{
		x = gOffX;
		y = gOffY;
		w = (int)(gSrcW * gScale);
		h = (int)(gSrcH * gScale);
	}
	else if(gGlPicW > 0 && gGlPicH > 0)
	{
		w = gGlPicW;
		h = gGlPicH;
	}
	if(OsGl_Draw(rc.right, rc.bottom, x, y, w, h, gScaleRule))
		Win32Gl_Swap();
}

/* An icon from a picture of B G R A rows: a colour bitmap with the alpha
 * in it, and a mask that is set where the picture is clear (what is drawn
 * where alpha is not understood). */
static HICON MakeIcon(const OpenBGI_Icon_t* p)
{
	BITMAPINFO bi;
	ICONINFO info;
	HICON icon = NULL;
	HDC dc;
	void* bits = NULL;
	uint8_t* mask;
	int x, y, stride = ((p->width + 15) / 16) * 2; // mask rows are whole words
	memset(&bi, 0, sizeof bi);
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = p->width;
	bi.bmiHeader.biHeight = -p->height; // top row first
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32; // (the fourth byte is the alpha: an icon of 32 bits has one)
	bi.bmiHeader.biCompression = BI_RGB;
	memset(&info, 0, sizeof info);
	info.fIcon = TRUE;
	dc = GetDC(NULL);
	info.hbmColor = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	ReleaseDC(NULL, dc);
	mask = (uint8_t*)calloc((size_t)stride, (size_t)p->height);
	if(info.hbmColor && bits && mask)
	{
		memcpy(bits, p->pixels, (size_t)p->width * (size_t)p->height * 4);
		for(y = 0; y < p->height; y++)
			for(x = 0; x < p->width; x++)
				if(p->pixels[((size_t)y * (size_t)p->width + (size_t)x) * 4 + 3] < 0x80)
					mask[y * stride + x / 8] |= (uint8_t)(0x80 >> (x & 7));
		info.hbmMask = CreateBitmap(p->width, p->height, 1, 1, mask);
		if(info.hbmMask)
			icon = CreateIconIndirect(&info);
	}
	if(info.hbmMask)
		DeleteObject(info.hbmMask);
	if(info.hbmColor)
		DeleteObject(info.hbmColor);
	free(mask);
	return icon;
}

/* The picture for a place that shows icons `want` pixels wide: the
 * smallest that is at least as large, else the largest there is. */
static const OpenBGI_Icon_t* IconFor(const OpenBGI_Icon_t* icons, int count, int want)
{
	const OpenBGI_Icon_t* best = &icons[0];
	int i;
	for(i = 1; i < count; i++)
	{
		int fits = icons[i].width >= want, bestFits = best->width >= want;
		if(fits ? (!bestFits || icons[i].width < best->width) : (!bestFits && icons[i].width > best->width))
			best = &icons[i];
	}
	return best;
}

// the icons OS_WindowSetIcon made: the large one (task bar, Alt+Tab) and the small one (title bar)
static HICON gIconBig, gIconSmall;

static void ApplyIcon(void)
{
	if(!gWin32MainWnd || (!gIconBig && !gIconSmall))
		return;
	SendMessageW(gWin32MainWnd, WM_SETICON, ICON_BIG, (LPARAM)gIconBig);
	SendMessageW(gWin32MainWnd, WM_SETICON, ICON_SMALL, (LPARAM)gIconSmall);
}

void OS_WindowSetIcon(const OpenBGI_Icon_t* icons, int count)
{
	HICON oldBig = gIconBig, oldSmall = gIconSmall;
	gIconBig = gIconSmall = NULL;
	if(count > 0)
	{
		gIconBig = MakeIcon(IconFor(icons, count, GetSystemMetrics(SM_CXICON)));
		gIconSmall = MakeIcon(IconFor(icons, count, GetSystemMetrics(SM_CXSMICON)));
	}
	if(gWin32MainWnd)
	{ // (without pictures: back to the icon of the window's class)
		SendMessageW(gWin32MainWnd, WM_SETICON, ICON_BIG, (LPARAM)gIconBig);
		SendMessageW(gWin32MainWnd, WM_SETICON, ICON_SMALL, (LPARAM)gIconSmall);
	}
	if(oldBig)
		DestroyIcon(oldBig);
	if(oldSmall)
		DestroyIcon(oldSmall);
}

int OS_WindowHasIcons(void)
{
	return 1;
}

// the picture the engine draws: scaled into a windowed client area of another size (MapRecompute)
void OS_WindowSetPicture(int w, int h)
{
	if(w <= 0 || h <= 0)
		return;
	gPicW = w;
	gPicH = h;
	MapChanged();
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

/* The main window's picture while the client area is of another size:
 * rows srcY .. srcY + h - 1 of it changed or are to be shown again.  The
 * picture is scaled as a whole by the rule of OS_WindowSetScaling
 * (OsCommon_Scaled: only the rows that changed are worked out) and the
 * rows of the result that came out anew go to the window, 1:1.  0 when
 * that could not be done. */
static int BlitScaled(HDC hdc, const OsSurface_t* s, int srcY, int h)
{
	int pw = (int)(gSrcW * gScale), ph = (int)(gSrcH * gScale), y = 0, rows = 0;
	const uint32_t* pic;
	BITMAPINFOHEADER bi;
	if(pw < 1 || ph < 1)
		return 0;
	pic = OsCommon_Scaled(s, srcY, h, pw, ph, gScaleRule, &y, &rows);
	if(!pic)
		return 0;
	if(rows <= 0)
		return 1;
	memset(&bi, 0, sizeof bi);
	bi.biSize = sizeof bi;
	bi.biWidth = pw;
	bi.biHeight = -rows; // top-down
	bi.biPlanes = 1;
	bi.biBitCount = 32;
	bi.biCompression = BI_RGB;
	StretchDIBits(hdc, gOffX, gOffY + y, pw, rows, 0, 0, pw, rows, pic + (size_t)y * (size_t)pw, (const BITMAPINFO*)&bi, DIB_RGB_COLORS, SRCCOPY);
	return 1;
}

/* w x h pixels of the surface from (srcX, srcY) to (dstX, dstY) of the
 * window: the original's BitBlt from its memory DC, done here as a 1:1
 * StretchDIBits; on a main window that shows the picture at another size
 * a part of the picture at its place goes through BlitScaled.  `dc` NULL
 * takes and releases the main window's own; nothing happens without a
 * window, an empty rectangle or a surface without pixels. */
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
	if(Win32Gl_On() && (own || IsMainDc(hdc)))
	{ // the graphics processor shows the picture: what changed goes into its texture, and OS_WindowPresent draws
		gGlPicW = gSrcW > 0 ? gSrcW : src->width;
		gGlPicH = gSrcH > 0 ? gSrcH : src->height;
		OsGl_Blit(src, dstX, dstY, w, h, srcX, srcY, gGlPicW, gGlPicH);
		gGlDirty = 1;
		if(own)
			ReleaseDC(gWin32MainWnd, hdc);
		return;
	}
	if(gMapOn && (own || IsMainDc(hdc)))
	{ // a scaled window: the destination goes through the mapping
		int x0 = Win32_ToWinX(dstX), y0 = Win32_ToWinY(dstY);
		int x1 = Win32_ToWinX(dstX + w), y1 = Win32_ToWinY(dstY + h);
		if(dstX == srcX && dstY == srcY && src->width == gSrcW && src->height == gSrcH && BlitScaled(hdc, src, srcY, h))
		{
			if(own)
				ReleaseDC(gWin32MainWnd, hdc);
			return;
		}
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

void OS_WindowFillBlack(void) // the whole client area black: the host's repaint while there is no picture
{
	RECT rc;
	HDC hdc;
	if(!gWin32MainWnd)
		return;
	GetClientRect(gWin32MainWnd, &rc);
	if(Win32Gl_On())
	{
		gGlDirty = 0;
		if(rc.right > 0 && rc.bottom > 0 && OsGl_Black(rc.right, rc.bottom))
			Win32Gl_Swap();
		return;
	}
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

/* The pointer on the screen.  Over a monitor whose mode was not switched
 * (gBorderless) the engine's screen is the mode it asked for, as it would
 * be with the mode switched: the pointer is told and put in the mode's
 * pixels, and screen and client coordinates are the same there. */
void OS_CursorGetPos(int* x, int* y)
{
	POINT p;
	GetCursorPos(&p);
	if(gFullscreenStyle && gBorderless)
	{
		p.x = Win32_FromWinX(p.x - gFsX);
		p.y = Win32_FromWinY(p.y - gFsY);
	}
	*x = p.x;
	*y = p.y;
}

void OS_CursorSetPos(int x, int y)
{
	if(gFullscreenStyle && gBorderless)
	{
		x = Win32_ToWinX(x) + gFsX;
		y = Win32_ToWinY(y) + gFsY;
	}
	SetCursorPos(x, y);
}

void OS_ScreenToClient(int* x, int* y) // unchanged without a window; client coordinates are picture pixels (the mapping)
{
	POINT p;
	if(gFullscreenStyle && gBorderless)
		return; // (mode space is client space)
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
	if(gFullscreenStyle && gBorderless)
		return;
	p.x = Win32_ToWinX(*x);
	p.y = Win32_ToWinY(*y);
	if(gWin32MainWnd)
		ClientToScreen(gWin32MainWnd, &p);
	*x = p.x;
	*y = p.y;
}
