/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_window.c - the OS layer's main window and debugger's window as the
 *               host's tables (OpenBGI_Window_t and OpenBGI_DebugWindow_t
 *               of openbgi.h, for OS_Host; declared in host/os_common.h)
 *
 * The back ends keep their window code (OS_Window*, OS_Display*, the
 * cursor's, OS_DebugWindow*); the tables hand it to the engine as it is.
 * The headless back end (the test host's and Android's) has no window of
 * its own: its engines keep the window nobody sees, and the table is
 * NULL.  The page has no debugger's window.
 */
#include "host/os.h"
#include "host/os_common.h"
#include "host/os_present.h"

#include <stdlib.h>

#if defined(BGI_HEADLESS) || defined(BGI_ANDROID)

const OpenBGI_Window_t* OsCommon_Window(void)
{
	return NULL;
}

const OpenBGI_DebugWindow_t* OsCommon_DebugWindow(void)
{
	return NULL;
}

#else

// ---- the main window ------------------------------------------------------------------------

static int WindowCreate(void* user, const char* title, int x, int y, int w, int h)
{
	OsWindowDesc_t d;
	BGI_UNUSED(user);
	d.title = title;
	d.x = x;
	d.y = y;
	d.width = w;
	d.height = h;
	return OS_WindowCreate(&d);
}

static void WindowDestroy(void* user)
{
	BGI_UNUSED(user);
	OS_WindowDestroy();
}

static void WindowShow(void* user, int show)
{
	BGI_UNUSED(user);
	OS_WindowShow(show);
}

static void WindowSetTitle(void* user, const char* title)
{
	BGI_UNUSED(user);
	OS_WindowSetTitle(title);
}

static void WindowMove(void* user, int x, int y)
{
	BGI_UNUSED(user);
	OS_WindowMove(x, y);
}

static void WindowGetRect(void* user, int* x, int* y, int* w, int* h)
{
	BGI_UNUSED(user);
	OS_WindowGetRect(x, y, w, h);
}

static int WindowMinimized(void* user)
{
	BGI_UNUSED(user);
	return OS_WindowMinimized();
}

static void WindowMinimize(void* user)
{
	BGI_UNUSED(user);
	OS_WindowMinimize();
}

static void WindowScreenSize(void* user, int* w, int* h)
{
	BGI_UNUSED(user);
	OS_ScreenSize(w, h);
}

static void WindowFrame(void* user, int* fw, int* fh, int* top)
{
	BGI_UNUSED(user);
	OS_FrameMetrics(fw, fh);
	*top = OS_FrameTop();
}

static int WindowSetMode(void* user, int w, int h, int bpp)
{
	BGI_UNUSED(user);
	return OS_DisplaySetMode(1, w, h, bpp);
}

static void WindowRestoreMode(void* user)
{
	BGI_UNUSED(user);
	OS_DisplayRestore();
}

static void WindowSetFullscreen(void* user, int w, int h)
{
	BGI_UNUSED(user);
	OS_WindowSetFullscreen(w, h);
}

static void WindowSetWindowed(void* user, int x, int y, int w, int h)
{
	BGI_UNUSED(user);
	OS_WindowSetWindowed(x, y, w, h);
}

static void WindowSetPicture(void* user, int w, int h)
{
	BGI_UNUSED(user);
	OS_WindowSetPicture(w, h);
}

static void WindowCursorShape(void* user, int shape)
{
	BGI_UNUSED(user);
	OS_CursorSetShape(shape);
}

static void WindowCursorShow(void* user, int show)
{
	BGI_UNUSED(user);
	OS_CursorShow(show);
}

static void WindowImeStatus(void* user, int open)
{
	BGI_UNUSED(user);
	OS_ImeStatus(open);
}

static void WindowDragAccept(void* user, int accept)
{
	BGI_UNUSED(user);
	OS_DragAccept(accept);
}

static void WindowSetResizable(void* user, int on)
{
	BGI_UNUSED(user);
	OS_WindowSetResizable(on);
}

// ---- the picture at another size ----------------------------------------------------------------

static OsPresent_t* gScaler; // made at the first use, kept for the process

const uint32_t* OsCommon_Scaled(const OsSurface_t* s, int y, int h, int w, int hOut, int rule, int* outY, int* outH)
{
	const uint32_t* out;
	*outY = *outH = 0;
	if(!gScaler && !(gScaler = OsPresent_New()))
		return NULL;
	rule &= OPENBGI_SCALE_RULE; // (without OPENBGI_SCALE_WHOLE, which is the caller's)
	OsPresent_Setup(gScaler, w, hOut, OS_PRESENT_STRETCH,
		rule == OPENBGI_SCALE_BICUBIC ? OS_PRESENT_BICUBIC : rule == OPENBGI_SCALE_NEAREST ? OS_PRESENT_NEAREST
																						   : OS_PRESENT_BILINEAR);
	OsPresent_Changed(gScaler, y, h);
	out = OsPresent_Draw(gScaler, (const uint8_t*)s->pixels, s->width, s->height, s->pitch, s->bpp, NULL);
	if(out)
		OsPresent_Drawn(gScaler, outY, outH);
	return out;
}

static void WindowSetScaling(void* user, int rule)
{
	BGI_UNUSED(user);
	OS_WindowSetScaling(rule);
}

static void WindowSetIcon(void* user, const OpenBGI_Icon_t* icons, int count)
{
	BGI_UNUSED(user);
	OS_WindowSetIcon(icons, count);
}

int OsCommon_Dpi(void)
{
	const char* e = getenv("BGI_DPI");
	int dpi = e && atoi(e) > 0 ? atoi(e) : OS_WindowDpi();
	return dpi < 48 ? 48 : dpi > 480 ? 480
									 : dpi;
}

static int WindowDpi(void* user)
{
	BGI_UNUSED(user);
	return OS_WindowDpi();
}

static void WindowWorkArea(void* user, int* x, int* y, int* w, int* h)
{
	BGI_UNUSED(user);
	OS_WindowWorkArea(x, y, w, h);
}

static void WindowSetAspect(void* user, int w, int h)
{
	BGI_UNUSED(user);
	OS_WindowSetAspect(w, h);
}

static const char* WindowFilterName(void* user, int n)
{
	BGI_UNUSED(user);
	return OS_WindowFilterName(n);
}

static const OpenBGI_Window_t kWindow = {
	sizeof(OpenBGI_Window_t),
	WindowCreate,
	WindowDestroy,
	WindowShow,
	WindowSetTitle,
	WindowMove,
	WindowGetRect,
	WindowMinimized,
	WindowMinimize,
	WindowScreenSize,
	WindowFrame,
	WindowSetMode,
	WindowRestoreMode,
	WindowSetFullscreen,
	WindowSetWindowed,
	WindowSetPicture,
	WindowCursorShape,
	WindowCursorShow,
	WindowImeStatus,
	WindowDragAccept,
	WindowSetResizable,
	WindowSetScaling,
	WindowSetIcon,
	WindowDpi,
	WindowWorkArea,
	WindowSetAspect,
	WindowFilterName,
};

// (a back end that scales by one rule alone leaves the member out: the engine tells the scripts so)
const OpenBGI_Window_t* OsCommon_Window(void)
{
	static OpenBGI_Window_t table;
	table = kWindow;
	if(!OS_WindowScales())
		table.setScaling = NULL;
	if(!OS_WindowHasIcons())
		table.setIcon = NULL;
	if(!OS_WindowFits())
	{
		table.dpi = NULL;
		table.workArea = NULL;
		table.setAspect = NULL;
	}
	return &table;
}

// ---- the debugger's window ------------------------------------------------------------------

#if defined(BGI_WASM)

const OpenBGI_DebugWindow_t* OsCommon_DebugWindow(void)
{
	return NULL;
}

#else

static int DebugOpen(void* user, int w, int h, const char* title)
{
	BGI_UNUSED(user);
	return OS_DebugWindowOpen(w, h, title);
}

static void DebugClose(void* user)
{
	BGI_UNUSED(user);
	OS_DebugWindowClose();
}

static void DebugPresent(void* user, const uint8_t* pixels, int pitch, int w, int h)
{
	OsSurface_t s;
	BGI_UNUSED(user);
	s.pixels = (void*)pixels;
	s.pitch = pitch;
	s.width = w;
	s.height = h;
	s.bpp = 32;
	OS_DebugWindowPresent(&s);
}

static const OpenBGI_DebugWindow_t kDebugWindow = {sizeof(OpenBGI_DebugWindow_t), DebugOpen, DebugClose, DebugPresent};

const OpenBGI_DebugWindow_t* OsCommon_DebugWindow(void)
{
	return &kDebugWindow;
}

#endif // !BGI_WASM
#endif // !BGI_HEADLESS && !BGI_ANDROID
