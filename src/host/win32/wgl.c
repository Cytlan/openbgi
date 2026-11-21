/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wgl.c - OpenGL on the game's window (src/host/os_gl.h), when the system
 *         has a graphics processor for it
 *
 * Nothing of OpenGL is linked: opengl32.dll is loaded when it is first
 * asked about, and where it is missing, or what it offers will not do, the
 * picture is put on the window by the processor as without this file.
 *
 * Whether it will do has to be known before the game's window is there -
 * the window's styles, the text entry and the full screen go by it - and a
 * window takes a pixel format once for good, so the question is asked on a
 * small hidden window made for it (Win32Gl_Usable), once.  The game's
 * window then gets the same: a double-buffered format, a core context of
 * OpenGL 3.3 where the driver makes one, else whatever context it makes.
 * Buffers are swapped without waiting for the display: the engine paces
 * its frames itself.
 */
#include "win32_internal.h"

#ifdef BGI_GL

#include "host/os_gl.h"

#define WGL_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB    0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB     0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 1

typedef void (*Proc_t)(void);

static HMODULE gLib;                           // opengl32.dll, once it was looked for and found
static int gLooked;                            // .. it was looked for
static int gUsable = -1;                       // what the hidden window's trial said; -1: not asked yet
static HGLRC(WINAPI* gCreate)(HDC);            // wglCreateContext
static BOOL(WINAPI* gDelete)(HGLRC);           // wglDeleteContext
static BOOL(WINAPI* gMakeCurrent)(HDC, HGLRC); // wglMakeCurrent
static PROC(WINAPI* gGetProc)(LPCSTR);         // wglGetProcAddress
static HGLRC gCtx;                             // the context on the main window; NULL: the picture goes the processor's way
static HDC gDc;                                // the window's own DC (its class has CS_OWNDC) the context is current on

// opengl32.dll and the four functions around a context, looked for once; 1 when they are all there
static int Library(void)
{
	if(gLooked)
		return gLib != NULL;
	gLooked = 1;
	gLib = LoadLibraryW(L"opengl32.dll");
	if(!gLib)
		return 0;
	gCreate = (HGLRC(WINAPI*)(HDC))(Proc_t)GetProcAddress(gLib, "wglCreateContext");
	gDelete = (BOOL(WINAPI*)(HGLRC))(Proc_t)GetProcAddress(gLib, "wglDeleteContext");
	gMakeCurrent = (BOOL(WINAPI*)(HDC, HGLRC))(Proc_t)GetProcAddress(gLib, "wglMakeCurrent");
	gGetProc = (PROC(WINAPI*)(LPCSTR))(Proc_t)GetProcAddress(gLib, "wglGetProcAddress");
	if(!gCreate || !gDelete || !gMakeCurrent || !gGetProc)
	{
		FreeLibrary(gLib);
		gLib = NULL;
	}
	return gLib != NULL;
}

// an OpenGL function by its name: the driver's, else the library's own (the functions of OpenGL 1.1 are only there)
static OsGlProc_t Load(const char* name)
{
	Proc_t p = (Proc_t)gGetProc(name);
	uintptr_t v = 0;
	memcpy(&v, &p, sizeof p < sizeof v ? sizeof p : sizeof v);
	if(v <= 3 || v == (uintptr_t)-1) // (some drivers answer a small number for a name they do not have)
		p = (Proc_t)GetProcAddress(gLib, name);
	return p;
}

// a double-buffered format of 8 bits a colour for the window of the DC; 1 when it has it
static int Format(HDC dc)
{
	PIXELFORMATDESCRIPTOR pfd;
	int n;
	memset(&pfd, 0, sizeof pfd);
	pfd.nSize = sizeof pfd;
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 24;
	pfd.iLayerType = PFD_MAIN_PLANE;
	n = ChoosePixelFormat(dc, &pfd);
	return n > 0 && SetPixelFormat(dc, n, &pfd);
}

/* A context on the DC, current when it comes back: of OpenGL 3.3's core
 * where the driver makes one (the function for it is the driver's, and is
 * found only with a context current: the plain one is made first), else
 * the plain one.  NULL for none. */
static HGLRC Context(HDC dc)
{
	static const int kCore[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3, WGL_CONTEXT_PROFILE_MASK_ARB,
		WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
	HGLRC(WINAPI * attribs)
	(HDC, HGLRC, const int*);
	HGLRC ctx = gCreate(dc), core;
	if(!ctx)
		return NULL;
	if(!gMakeCurrent(dc, ctx))
	{
		gDelete(ctx);
		return NULL;
	}
	if(OsGl_Wanted() == OS_GL_OLD)
		return ctx;
	attribs = (HGLRC(WINAPI*)(HDC, HGLRC, const int*))Load("wglCreateContextAttribsARB");
	core = attribs ? attribs(dc, NULL, kCore) : NULL;
	if(!core)
		return ctx;
	if(!gMakeCurrent(dc, core))
	{
		gDelete(core);
		gMakeCurrent(dc, ctx);
		return ctx;
	}
	gDelete(ctx);
	return core;
}

// the trial: a hidden window, a context on it, and the question whether that one will do
static int Trial(void)
{
	HWND hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 16, 16, NULL, NULL, gWin32Instance, NULL);
	HDC dc;
	HGLRC ctx;
	int ok = 0;
	if(!hwnd)
		return 0;
	dc = GetDC(hwnd);
	if(dc && Format(dc) && (ctx = Context(dc)) != NULL)
	{
		ok = OsGl_Usable(Load);
		gMakeCurrent(NULL, NULL);
		gDelete(ctx);
	}
	if(dc)
		ReleaseDC(hwnd, dc);
	DestroyWindow(hwnd);
	return ok;
}

int Win32Gl_Usable(void)
{
	if(gUsable < 0)
		gUsable = OsGl_Wanted() != OS_GL_OFF && Library() && Trial();
	return gUsable;
}

int Win32Gl_Attach(HWND hwnd)
{
	BOOL(WINAPI * interval)
	(int);
	Win32Gl_Detach();
	if(!hwnd || !Win32Gl_Usable())
		return 0;
	gDc = GetDC(hwnd); // (the window's one DC: kept, never released)
	if(!gDc || !Format(gDc) || !(gCtx = Context(gDc)))
	{
		gDc = NULL;
		return 0;
	}
	interval = (BOOL(WINAPI*)(int))Load("wglSwapIntervalEXT");
	if(interval)
		interval(0);
	if(!OsGl_Start(Load))
	{
		Win32Gl_Detach();
		return 0;
	}
	return 1;
}

void Win32Gl_Detach(void)
{
	if(!gCtx)
		return;
	OsGl_Stop();
	gMakeCurrent(NULL, NULL);
	gDelete(gCtx);
	gCtx = NULL;
	gDc = NULL;
}

int Win32Gl_On(void)
{
	return gCtx != NULL && OsGl_On();
}

void Win32Gl_Swap(void)
{
	if(gCtx)
		SwapBuffers(gDc);
}

#else

// a build without OpenGL (GL=0): the picture always goes the processor's way
int Win32Gl_Usable(void)
{
	return 0;
}

int Win32Gl_Attach(HWND hwnd)
{
	BGI_UNUSED(hwnd);
	return 0;
}

void Win32Gl_Detach(void)
{
}

int Win32Gl_On(void)
{
	return 0;
}

void Win32Gl_Swap(void)
{
}

#endif
