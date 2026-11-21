/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * glx.c - OpenGL on the game's X11 window (src/host/os_gl.h), when the
 *         system has it
 *
 * Nothing of OpenGL is linked: libGL.so.1 is loaded when the window is
 * made, and where it is missing, or what it offers will not do, the
 * picture is put on the window by the processor as without this file.
 *
 * The context is made for the visual the window already has (the default
 * one), so that the window is the same with and without OpenGL: a frame
 * buffer configuration with that visual, double-buffered, a core context
 * of OpenGL 3.3 where the server makes one, else whatever context it
 * makes.  The X errors a refusal raises are caught for the while, because
 * Xlib would end the program over them.  Buffers are swapped without
 * waiting for the display: the engine paces its frames itself.
 */
#include "x11_internal.h"

#if defined(BGI_GL) && !defined(BGI_HAIKU)

#include "host/os_gl.h"

#include <dlfcn.h>

// GLX as far as it is used (no header of it is included)
typedef struct GlxContextRec* GlxContext_t;
typedef struct GlxConfigRec* GlxConfig_t;

#define GLX_DOUBLEBUFFER                 5
#define GLX_RED_SIZE                     8
#define GLX_GREEN_SIZE                   9
#define GLX_BLUE_SIZE                    10
#define GLX_WINDOW_BIT                   1
#define GLX_RGBA_BIT                     1
#define GLX_DRAWABLE_TYPE                0x8010
#define GLX_RENDER_TYPE                  0x8011
#define GLX_X_RENDERABLE                 0x8012
#define GLX_RGBA_TYPE                    0x8014
#define GLX_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB    0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB     0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 1

static struct
{
	Bool (*QueryExtension)(Display*, int*, int*);
	GlxConfig_t* (*ChooseFBConfig)(Display*, int, const int*, int*);
	XVisualInfo* (*GetVisualFromFBConfig)(Display*, GlxConfig_t);
	GlxContext_t (*CreateNewContext)(Display*, GlxConfig_t, int, GlxContext_t, Bool);
	Bool (*MakeCurrent)(Display*, XID, GlxContext_t);
	void (*SwapBuffers)(Display*, XID);
	void (*DestroyContext)(Display*, GlxContext_t);
	OsGlProc_t (*GetProcAddressARB)(const unsigned char*);
	const char* (*QueryExtensionsString)(Display*, int);
} glx;

static const char* const kNames[] = {"glXQueryExtension", "glXChooseFBConfig", "glXGetVisualFromFBConfig", "glXCreateNewContext",
	"glXMakeCurrent", "glXSwapBuffers", "glXDestroyContext", "glXGetProcAddressARB", "glXQueryExtensionsString"};

static void* gLib;        // libGL, once it was looked for and found
static int gLooked;       // .. it was looked for
static GlxContext_t gCtx; // the context on the main window; NULL: the picture goes the processor's way
static Window gWin;       // the window it is current on
static int gTrapped;      // an X error came while one was expected

static int Trap(Display* dpy, XErrorEvent* e)
{
	BGI_UNUSED(dpy);
	BGI_UNUSED(e);
	gTrapped = 1;
	return 0;
}

// libGL and its functions, looked for once; 1 when they are all there
static int Library(void)
{
	void* found[sizeof kNames / sizeof kNames[0]];
	size_t i;
	if(gLooked)
		return gLib != NULL;
	gLooked = 1;
	if(sizeof glx != sizeof found)
		return 0;
	gLib = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	if(!gLib)
		return 0;
	for(i = 0; i < sizeof kNames / sizeof kNames[0]; i++)
		if(!(found[i] = dlsym(gLib, kNames[i])))
		{
			dlclose(gLib);
			gLib = NULL;
			return 0;
		}
	memcpy(&glx, found, sizeof glx); // (an object pointer is not a function pointer to ISO C; POSIX says dlsym's is)
	return 1;
}

// an OpenGL (or GLX) function by its name
static OsGlProc_t Load(const char* name)
{
	OsGlProc_t p = glx.GetProcAddressARB((const unsigned char*)name);
	if(!p)
	{
		void* sym = dlsym(gLib, name);
		memcpy(&p, &sym, sizeof p);
	}
	return p;
}

// whether the server's GLX names an extension
static int Has(const char* list, const char* name)
{
	size_t n = strlen(name);
	const char* at = list;
	while(at && (at = strstr(at, name)) != NULL)
	{
		if((at == list || at[-1] == ' ') && (at[n] == ' ' || !at[n]))
			return 1;
		at += n;
	}
	return 0;
}

// a double-buffered configuration of 8 bits a colour whose visual is the window's own; NULL for none
static GlxConfig_t Config(void)
{
	static const int kWant[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DOUBLEBUFFER, True,
		GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
	GlxConfig_t* list;
	GlxConfig_t pick = NULL;
	int n = 0, i;
	list = glx.ChooseFBConfig(gX11Dpy, gX11Screen, kWant, &n);
	for(i = 0; list && i < n && !pick; i++)
	{
		XVisualInfo* vi = glx.GetVisualFromFBConfig(gX11Dpy, list[i]);
		if(vi && vi->visualid == gX11Visual->visualid)
			pick = list[i];
		if(vi)
			XFree(vi);
	}
	if(list)
		XFree(list);
	return pick;
}

int X11Gl_Attach(Window win)
{
	static const int kCore[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3, GLX_CONTEXT_PROFILE_MASK_ARB,
		GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
	XErrorHandler old;
	GlxConfig_t config;
	const char* ext;
	int a = 0, b = 0, ok;
	X11Gl_Detach();
	if(OsGl_Wanted() == OS_GL_OFF || !win || !Library() || !glx.QueryExtension(gX11Dpy, &a, &b))
		return 0;
	config = Config();
	if(!config)
	{
		fprintf(stderr, "openbgi: OpenGL has no double-buffered configuration for the window's visual: the picture is scaled by the processor\n");
		return 0;
	}
	ext = glx.QueryExtensionsString(gX11Dpy, gX11Screen);
	XSync(gX11Dpy, False);
	gTrapped = 0;
	old = XSetErrorHandler(Trap);
	if(ext && Has(ext, "GLX_ARB_create_context_profile") && OsGl_Wanted() != OS_GL_OLD)
	{
		GlxContext_t (*create)(Display*, GlxConfig_t, GlxContext_t, Bool, const int*) = NULL;
		OsGlProc_t p = Load("glXCreateContextAttribsARB");
		memcpy(&create, &p, sizeof create);
		if(create)
			gCtx = create(gX11Dpy, config, NULL, True, kCore);
		XSync(gX11Dpy, False);
		if(gTrapped)
			gCtx = NULL;
		gTrapped = 0;
	}
	if(!gCtx)
		gCtx = glx.CreateNewContext(gX11Dpy, config, GLX_RGBA_TYPE, NULL, True);
	XSync(gX11Dpy, False);
	ok = gCtx && !gTrapped && glx.MakeCurrent(gX11Dpy, win, gCtx);
	XSync(gX11Dpy, False);
	ok = ok && !gTrapped;
	XSetErrorHandler(old);
	if(ok)
	{
		gWin = win;
		if(ext && Has(ext, "GLX_EXT_swap_control"))
		{
			void (*interval)(Display*, XID, int) = NULL;
			OsGlProc_t p = Load("glXSwapIntervalEXT");
			memcpy(&interval, &p, sizeof interval);
			if(interval)
				interval(gX11Dpy, win, 0);
		}
		else if(ext && Has(ext, "GLX_MESA_swap_control"))
		{
			int (*interval)(unsigned) = NULL;
			OsGlProc_t p = Load("glXSwapIntervalMESA");
			memcpy(&interval, &p, sizeof interval);
			if(interval)
				interval(0);
		}
		ok = OsGl_Start(Load);
	}
	if(!ok)
		X11Gl_Detach();
	return ok;
}

void X11Gl_Detach(void)
{
	if(!gCtx)
		return;
	OsGl_Stop();
	glx.MakeCurrent(gX11Dpy, None, NULL);
	glx.DestroyContext(gX11Dpy, gCtx);
	gCtx = NULL;
	gWin = 0;
}

int X11Gl_On(void)
{
	return gCtx != NULL && OsGl_On();
}

void X11Gl_Swap(void)
{
	if(gCtx)
		glx.SwapBuffers(gX11Dpy, gWin);
}

#else

// a build without OpenGL (Haiku's, or GL=0): the picture always goes the processor's way
int X11Gl_Attach(Window win)
{
	BGI_UNUSED(win);
	return 0;
}

void X11Gl_Detach(void)
{
}

int X11Gl_On(void)
{
	return 0;
}

void X11Gl_Swap(void)
{
}

#endif
