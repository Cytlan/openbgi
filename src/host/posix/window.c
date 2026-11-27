/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.c - the POSIX back end's main window (src/host/os.h)
 *
 * One top-level X11 window stands in for the engine's main window.  Full
 * screen does not switch the display mode: the window covers the screen
 * (_NET_WM_STATE_FULLSCREEN) and the mode the engine asked for is scaled
 * to fit, centred, with every coordinate the engine sees or hands out
 * translated between the "mode" space and the real screen.  A windowed
 * client area of another size than the picture (OS_WindowSetPicture, the
 * "81 64" window size of 1.529 on) is handled by the same mapping.  The pop-up
 * child windows are in child.c, the event pump in events.c, the
 * debugger's window in dbgwin.c, and the message boxes and dialogs in
 * msgbox.c and dialogs.c on the toolkit of uikit.c; the engine draws the
 * text entry in the frame itself (OS_TextEntry is NULL).
 *
 * Files dropped on the window are xdnd.c's.  Not reproduced: the IME
 * status window.
 *
 * On Haiku the same code runs on Xlibe, which has no full-screen state:
 * there the frame is taken off through _MOTIF_WM_HINTS instead, and the
 * pointer warp and the button mapping come from src/host/haiku/.
 */
#include "x11_internal.h"
#include "host/os_gl.h"

#ifdef BGI_HAIKU
#include "host/os_haiku.h"
#else
#include <dlfcn.h>
#include <time.h>
#endif

// ---- the main window ---------------------------------------------------------------------------

Window gX11Win;                                     // 0 before OS_WindowCreate and after OS_WindowDestroy
XIC gX11Xic;                                        // the input context of the main window, NULL without an input method
int gX11TextInput;                                  // the engine takes typed text (OS_TextInput)
X11Dc_t gX11MainDc = {0, -1};                       // what OS_WindowGetDc hands out
int gX11Alive, gX11Shown;                           // created / mapped
int gX11Active, gX11Minimized, gX11HiddenByUs;      // focus and iconic state (events.c keeps them); hidden by OS_WindowShow(0)
static int gClientW = 800, gClientH = 600;          // the engine's client size (the mode size in full screen)
int gX11FrameL, gX11FrameT, gX11FrameR, gX11FrameB; // _NET_FRAME_EXTENTS once the window manager tells
static int gFrameKnown;                             // it told
static int gFullscreen;                             // the window covers the screen
static int gFsModeW, gFsModeH;                      // the display mode (full screen) or picture (windowed) the engine draws
static int gMapOn;                                  // the mode is scaled into the window: full screen, or a client area of another size
static int gScaleRule;                              // OS_WindowSetScaling: OPENBGI_SCALE_*
static double gFsScale = 1.0;                       // mode -> window pixels
static int gFsOffX, gFsOffY;                        // where the scaled mode sits in the window
static int gScreenW, gScreenH;                      // the screen in pixels (OS_ScreenSize)
int gX11QuitPosted;                                 // a signal asked for the quit: the next pump raises `quit`
int gX11LastClickBtn = -1;                          // double-click detection: the last press's button, time and position
Time gX11LastClickTime;
int gX11LastClickX, gX11LastClickY;
int gX11ModalDepth; // a dialog of uikit.c is up: the engine's windows ignore input

// ---- full-screen mapping -----------------------------------------------------------------------

// the window's pixel size: the screen in full screen, the client area otherwise
static void TargetSize(int* w, int* h)
{
	*w = gFullscreen ? gScreenW : gClientW;
	*h = gFullscreen ? gScreenH : gClientH;
}

/* the scale and offset that fit the mode onto the window: the smaller of
 * the two ratios so that the whole mode fits with its aspect kept (a
 * whole number where OPENBGI_SCALE_WHOLE asks for one and the picture is
 * not made smaller), centred, the rest letterboxed (X11_FillLetterbox).
 * No mapping while a windowed client area is of the mode's size. */
static void FsRecompute(void)
{
	double sx, sy;
	int tw, th;
	TargetSize(&tw, &th);
	gMapOn = 0;
	gFsScale = 1.0;
	gFsOffX = gFsOffY = 0;
	if(gFsModeW <= 0 || gFsModeH <= 0 || (!gFullscreen && tw == gFsModeW && th == gFsModeH))
		return;
	gMapOn = 1;
	sx = (double)tw / gFsModeW;
	sy = (double)th / gFsModeH;
	gFsScale = sx < sy ? sx : sy;
	if((gScaleRule & OPENBGI_SCALE_WHOLE) && gFsScale >= 1.0)
		gFsScale = (double)(int)gFsScale; // by whole numbers: the rest is letterboxed
	gFsOffX = (int)((tw - gFsModeW * gFsScale) / 2);
	gFsOffY = (int)((th - gFsModeH * gFsScale) / 2);
}

// mode (engine) coordinates <-> window pixels
int X11_ToWinX(int x)
{
	return gMapOn ? (int)(x * gFsScale + 0.5) + gFsOffX : x;
}
int X11_ToWinY(int y)
{
	return gMapOn ? (int)(y * gFsScale + 0.5) + gFsOffY : y;
}
int X11_FromWinX(int x)
{
	return gMapOn ? (int)((x - gFsOffX) / gFsScale) : x;
}
int X11_FromWinY(int y)
{
	return gMapOn ? (int)((y - gFsOffY) / gFsScale) : y;
}

static int gGlDirty;         // the picture's texture changed since it was last drawn (OS_WindowPresent)
static int gGlPicW, gGlPicH; // the picture as it was last handed to the graphics processor

/* The main window's picture while it is shown at another size (the full
 * screen, a client area of another size): rows srcY .. srcY + h - 1 of it
 * changed or are to be shown again.  The picture is scaled as a whole by
 * the rule of OS_WindowSetScaling (OsCommon_Scaled: only the rows that
 * changed are worked out) and the rows of the result that came out anew
 * go to the window.  0 when that could not be done. */
static int BlitScaled(const OsSurface_t* s, int srcY, int h)
{
	int pw = (int)(gFsModeW * gFsScale), ph = (int)(gFsModeH * gFsScale), y = 0, rows = 0;
	const uint32_t* pic;
	if(pw < 1 || ph < 1)
		return 0;
	pic = OsCommon_Scaled(s, srcY, h, pw, ph, gScaleRule, &y, &rows);
	if(!pic)
		return 0;
	if(rows > 0)
		X11_PutArgb(gX11Win, gFsOffX, gFsOffY + y, pw, rows, pic + (size_t)y * (size_t)pw, pw);
	return 1;
}

/* the workhorse behind OS_BlitToWindow and the child windows: w x h
 * pixels of `s` from (srcX, srcY) to (dstX, dstY) of the target window
 * (in its engine coordinates), clipped to the window.  On the main window
 * the destination goes through the full-screen mapping: the engine's blit
 * of a part of the mode-sized picture, at its place, lands scaled on the
 * screen (BlitScaled; any other blit there by the nearest pixel).  A
 * child window is never scaled.  Source pixels outside the surface come
 * out black. */
void X11_BlitSurface(const X11Dc_t* dc, int dstX, int dstY, int w, int h, const OsSurface_t* s, int srcX, int srcY)
{
	int cl = 0, ct = 0, cr, cb;   // the visible part in window pixels
	int wx0, wy0, wx1, wy1, x, y; // the destination in window pixels
	int vw, vh;                   // the visible part's size
	uint32_t* d;
	Window win = dc->win;
	double fx, fy; // destination -> source scale

	if(!gX11Dpy || !win || w <= 0 || h <= 0 || !s->pixels)
		return;
	if(dc->child < 0 && X11Gl_On())
	{ // the graphics processor shows the picture: what changed goes into its texture, and OS_WindowPresent draws
		OsGl_Blit(s, dstX, dstY, w, h, srcX, srcY, gFsModeW > 0 ? gFsModeW : s->width, gFsModeH > 0 ? gFsModeH : s->height);
		gGlPicW = gFsModeW > 0 ? gFsModeW : s->width;
		gGlPicH = gFsModeH > 0 ? gFsModeH : s->height;
		gGlDirty = 1;
		return;
	}
	if(dc->child < 0 && gMapOn && dstX == srcX && dstY == srcY && s->width == gFsModeW && s->height == gFsModeH && BlitScaled(s, srcY, h))
		return;
	if(dc->child < 0)
	{ // the main window
		cr = gFullscreen ? gScreenW : gClientW;
		cb = gFullscreen ? gScreenH : gClientH;
		wx0 = X11_ToWinX(dstX);
		wy0 = X11_ToWinY(dstY);
		wx1 = X11_ToWinX(dstX + w);
		wy1 = X11_ToWinY(dstY + h);
	}
	else
	{
		Window root;
		int xx, yy;
		unsigned ww, wh, bw, depth;
		XGetGeometry(gX11Dpy, win, &root, &xx, &yy, &ww, &wh, &bw, &depth);
		cr = (int)ww;
		cb = (int)wh;
		wx0 = dstX;
		wy0 = dstY;
		wx1 = dstX + w;
		wy1 = dstY + h;
	}
	if(wx1 <= wx0 || wy1 <= wy0)
		return;
	fx = (double)w / (wx1 - wx0);
	fy = (double)h / (wy1 - wy0);
	if(cl < wx0)
		cl = wx0;
	if(ct < wy0)
		ct = wy0;
	if(cr > wx1)
		cr = wx1;
	if(cb > wy1)
		cb = wy1;
	vw = cr - cl;
	vh = cb - ct;
	if(vw <= 0 || vh <= 0)
		return;
	d = X11_ConvBuffer((size_t)vw * vh);
	if(!d)
		return;
	if(fx == 1.0 && fy == 1.0)
	{ // plain copy
		int direct = s->bpp == 32 && gX11VisRBits == 8 && gX11VisGBits == 8 && gX11VisBBits == 8 && gX11VisRShift == 16 &&
			gX11VisGShift == 8 && gX11VisBShift == 0; // the surface's own layout: rows go as they are
		for(y = 0; y < vh; y++)
		{
			int sy = srcY + (ct - wy0) + y;
			int sx0 = srcX + (cl - wx0);
			uint32_t* o = d + (size_t)y * vw;
			if(sy < 0 || sy >= s->height)
			{
				memset(o, 0, (size_t)vw * 4);
				continue;
			}
			if(direct && sx0 >= 0 && sx0 + vw <= s->width)
			{
				memcpy(o, (const uint8_t*)s->pixels + (ptrdiff_t)sy * s->pitch + (ptrdiff_t)sx0 * 4, (size_t)vw * 4);
				continue;
			}
			for(x = 0; x < vw; x++)
			{
				int sx = sx0 + x;
				o[x] = (sx >= 0 && sx < s->width) ? X11_PackPixel(X11_SurfacePixel(s, sx, sy)) : 0;
			}
		}
	}
	else
	{ // nearest neighbour
		for(y = 0; y < vh; y++)
		{
			int sy = srcY + (int)((ct - wy0 + y) * fy);
			uint32_t* o = d + (size_t)y * vw;
			if(sy >= srcY + h)
				sy = srcY + h - 1;
			for(x = 0; x < vw; x++)
			{
				int sx = srcX + (int)((cl - wx0 + x) * fx);
				if(sx >= srcX + w)
					sx = srcX + w - 1;
				o[x] = (sx >= 0 && sx < s->width && sy >= 0 && sy < s->height) ? X11_PackPixel(X11_SurfacePixel(s, sx, sy)) : 0;
			}
		}
	}
	X11_PutImage(win, cl, ct, vw, vh, d);
}

// the "device context" is a token naming the window; nothing is acquired, so there is nothing to release
OsDc_t* OS_WindowGetDc(void)
{
	return gX11Alive ? &gX11MainDc : NULL;
}

void OS_WindowReleaseDc(OsDc_t* dc)
{
}

// BitBlt: an unscaled copy
void OS_BlitToWindow(OsDc_t* dc, int dstX, int dstY, int w, int h, const OsSurface_t* src, int srcX, int srcY)
{
	const X11Dc_t* d = dc ? (const X11Dc_t*)dc : &gX11MainDc;
	X11_BlitSurface(d, dstX, dstY, w, h, src, srcX, srcY);
}

// the whole window black (the screen in full screen, the client area otherwise)
void OS_WindowFillBlack(void)
{
	GC gc;
	if(!gX11Alive)
		return;
	if(X11Gl_On())
	{
		int tw, th;
		TargetSize(&tw, &th);
		gGlDirty = 0;
		if(OsGl_Black(tw, th))
			X11Gl_Swap();
		return;
	}
	gc = XCreateGC(gX11Dpy, gX11Win, 0, NULL);
	XSetForeground(gX11Dpy, gc, BlackPixel(gX11Dpy, gX11Screen));
	XFillRectangle(gX11Dpy, gX11Win, gc, 0, 0, (unsigned)(gFullscreen ? gScreenW : gClientW),
		(unsigned)(gFullscreen ? gScreenH : gClientH));
	XFreeGC(gX11Dpy, gc);
	XFlush(gX11Dpy);
}

// the black bars beside a scaled picture (full screen, or a window of another size than the picture)
void X11_FillLetterbox(void)
{
	GC gc;
	int pw, ph, tw, th;
	if(!gMapOn || !gX11Win || X11Gl_On())
		return; // (the graphics processor draws the bars with the picture)
	TargetSize(&tw, &th);
	pw = (int)(gFsModeW * gFsScale);
	ph = (int)(gFsModeH * gFsScale);
	gc = XCreateGC(gX11Dpy, gX11Win, 0, NULL);
	XSetForeground(gX11Dpy, gc, BlackPixel(gX11Dpy, gX11Screen));
	if(gFsOffX > 0)
	{
		XFillRectangle(gX11Dpy, gX11Win, gc, 0, 0, (unsigned)gFsOffX, (unsigned)th);
		XFillRectangle(gX11Dpy, gX11Win, gc, gFsOffX + pw, 0, (unsigned)(tw - gFsOffX - pw), (unsigned)th);
	}
	if(gFsOffY > 0)
	{
		XFillRectangle(gX11Dpy, gX11Win, gc, 0, 0, (unsigned)tw, (unsigned)gFsOffY);
		XFillRectangle(gX11Dpy, gX11Win, gc, 0, gFsOffY + ph, (unsigned)tw, (unsigned)(th - gFsOffY - ph));
	}
	XFreeGC(gX11Dpy, gc);
}

/* The surface the picture is laid on, said to the engine whenever it is
 * another (OsHostHandlers_t.surface): the window's pixels, the picture's
 * place in them and the pixels an inch - while the picture is drawn
 * through the graphics processor, which draws the layer over it.  Drawn
 * any other way there is no surface, and so no layer: the engine keeps
 * its display settings box then.
 *
 * The engine's measures are a phone's, 160 to the inch where a pixel is
 * a unit; a monitor is looked at from further away, and its 96 to the
 * inch are the same unit (the desktop's scale counts as it does for
 * everything else). */
void X11_SurfaceTell(void)
{
	static OpenBGI_Surface_t told;
	OpenBGI_Surface_t s;
	if(!gX11Input.surface)
		return;
	memset(&s, 0, sizeof s);
	s.size = sizeof s;
	if(gX11Win && X11Gl_On() && gGlPicW > 0 && gGlPicH > 0)
	{
		TargetSize(&s.width, &s.height);
		s.picture.w = gGlPicW;
		s.picture.h = gGlPicH;
		if(gMapOn)
		{
			s.picture.x = gFsOffX;
			s.picture.y = gFsOffY;
			s.picture.w = (int)(gFsModeW * gFsScale);
			s.picture.h = (int)(gFsModeH * gFsScale);
		}
		s.dpi = OS_WindowDpi() * 160 / 96;
	}
	if(memcmp(&s, &told, sizeof s) == 0)
		return;
	told = s;
	gX11Input.surface(gX11Input.user, &s);
}

// the layer, into the texture the picture's draw puts over everything
void OS_WindowLayer(const unsigned char* pixels, int w, int h, int pitch, int x, int y, int cw, int ch)
{
	if(!gX11Win || !X11Gl_On())
		return;
	OsGl_Layer(pixels, w, h, pitch, x, y, cw, ch);
	gGlDirty = 1;
}

/* The picture through the graphics processor, once for all that a frame
 * changed: into the place the mapping gives it (the whole client area
 * when it is not scaled), by the rule of OS_WindowSetScaling, black
 * around it. */
void OS_WindowPresent(void)
{
	int tw, th, x = 0, y = 0, w = gGlPicW, h = gGlPicH;
	if(!gGlDirty || !X11Gl_On() || !gX11Win)
		return;
	gGlDirty = 0;
	TargetSize(&tw, &th);
	if(gMapOn)
	{
		x = gFsOffX;
		y = gFsOffY;
		w = (int)(gFsModeW * gFsScale);
		h = (int)(gFsModeH * gFsScale);
	}
	if(OsGl_Draw(tw, th, x, y, w, h, gScaleRule))
		X11Gl_Swap();
}

// the filters of the graphics processor's own (none without one)
const char* OS_WindowFilterName(int n)
{
	return X11Gl_On() ? OsGl_FilterName(n) : NULL;
}

// ---- geometry ----------------------------------------------------------------------------------

// 1 when an EWMH window manager runs (_NET_SUPPORTING_WM_CHECK on the root window)
int X11_HasWindowManager(void)
{
	Atom type;
	int fmt;
	unsigned long n = 0, after;
	unsigned char* data = NULL;
	int has = 0;
	if(XGetWindowProperty(gX11Dpy, gX11Root, gX11AtomSupportingWmCheck, 0, 1, False, XA_WINDOW, &type, &fmt, &n, &after,
		   &data) == Success)
	{
		has = n > 0;
		if(data)
			XFree(data);
	}
	return has;
}

/* the frame the window manager put around the main window
 * (_NET_FRAME_EXTENTS: left, right, top, bottom) into gX11Frame*; left
 * as they are when the property is not there yet */
int X11_ReadFrameExtents(void)
{
	Atom type;
	int fmt, changed = 0;
	unsigned long n, after;
	unsigned char* data = NULL;
	if(!gX11Win)
		return 0;
	if(XGetWindowProperty(gX11Dpy, gX11Win, gX11AtomNetFrameExtents, 0, 4, False, XA_CARDINAL, &type, &fmt, &n, &after, &data) ==
			Success &&
		data)
	{
		if(n == 4 && fmt == 32)
		{
			const long* v = (const long*)data;
			changed = gX11FrameL != (int)v[0] || gX11FrameR != (int)v[1] || gX11FrameT != (int)v[2] || gX11FrameB != (int)v[3];
			gX11FrameL = (int)v[0];
			gX11FrameR = (int)v[1];
			gX11FrameT = (int)v[2];
			gX11FrameB = (int)v[3];
			gFrameKnown = 1;
		}
		XFree(data);
	}
	return changed;
}

#ifndef BGI_HAIKU
/* The frame before the window is shown: the window manager is asked what
 * it would put around it (_NET_REQUEST_FRAME_EXTENTS) and given a tenth
 * of a second to say, so that a window made to fit the screen is of the
 * right size when it first appears.  Once known the frame is not asked
 * for again: the next window's is taken to be the same until the window
 * manager says otherwise. */
static void RequestFrameExtents(void)
{
	XEvent e;
	int i;
	if(gFrameKnown || !X11_HasWindowManager())
		return;
	memset(&e, 0, sizeof e);
	e.xclient.type = ClientMessage;
	e.xclient.window = gX11Win;
	e.xclient.message_type = XInternAtom(gX11Dpy, "_NET_REQUEST_FRAME_EXTENTS", False);
	e.xclient.format = 32;
	XSendEvent(gX11Dpy, gX11Root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
	for(i = 0; i < 20 && !gFrameKnown; i++)
	{
		struct timespec ts = {0, 5000000};
		XSync(gX11Dpy, False);
		X11_ReadFrameExtents();
		if(!gFrameKnown)
			nanosleep(&ts, NULL);
	}
}
#endif

// the frame's total width and height: what the window manager reported, 0 before it did
void OS_FrameMetrics(int* fw, int* fh)
{
	*fw = gX11FrameL + gX11FrameR;
	*fh = gX11FrameT + gX11FrameB;
}

int OS_FrameTop(void)
{
	return gX11FrameT;
}

// the screen in pixels (also refreshes the cached size); 1024 x 768 without a display
void OS_ScreenSize(int* w, int* h)
{
	if(!X11_Open())
	{
		*w = 1024;
		*h = 768;
		return;
	}
	gScreenW = DisplayWidth(gX11Dpy, gX11Screen);
	gScreenH = DisplayHeight(gX11Dpy, gX11Screen);
	*w = gScreenW;
	*h = gScreenH;
}

/* The client origin on the screen and the pointer position are asked for
 * several times per frame (every mouse layer, the cursor, the window
 * rectangle); both are X round trips, so they are cached from the events
 * that change them (ConfigureNotify / MapNotify for the origin, the
 * pointer events for the position) and queried only when no event has
 * supplied them yet. */
static int gOriginKnown, gOriginX, gOriginY; // the client area's top-left in root coordinates
static int gAsked, gAskedX, gAskedY;         // where the engine last put the frame's corner of a window not shown yet
static int gPtrKnown, gPtrX, gPtrY;          // the pointer in root coordinates

// re-read where the client area sits on the root window (XTranslateCoordinates)
void X11_UpdateClientOrigin(void)
{
	Window child;
	int rx = 0, ry = 0;
	if(!gX11Win)
		return;
	XTranslateCoordinates(gX11Dpy, gX11Win, gX11Root, 0, 0, &rx, &ry, &child);
	gOriginX = rx;
	gOriginY = ry;
	gOriginKnown = 1;
}

// the cached client origin, queried when nothing has supplied it yet; (0, 0) without a window
static void ClientOrigin(int* x, int* y)
{
	*x = *y = 0;
	if(!gX11Win)
		return;
	if(!gOriginKnown)
		X11_UpdateClientOrigin();
	*x = gOriginX;
	*y = gOriginY;
}

// the pointer position an event reported (root coordinates), so that OS_CursorGetPos need not ask
void X11_NotePointer(int rootX, int rootY)
{
	gPtrX = rootX;
	gPtrY = rootY;
	gPtrKnown = 1;
}

// GetWindowRect: the frame's rectangle on the screen; in full screen the display mode at (0, 0)
void OS_WindowGetRect(int* x, int* y, int* w, int* h)
{
	int cx, cy;
	if(gFullscreen)
	{ // the window covers the display mode
		*x = *y = 0;
		*w = gFsModeW;
		*h = gFsModeH;
		return;
	}
	ClientOrigin(&cx, &cy);
	X11_ReadFrameExtents();
	*x = cx - gX11FrameL;
	*y = cy - gX11FrameT;
	if(gAsked && !gX11Shown)
	{ // (a window not shown yet has no frame around it: it is where the engine put it)
		*x = gAskedX;
		*y = gAskedY;
	}
	*w = gClientW + gX11FrameL + gX11FrameR;
	*h = gClientH + gX11FrameT + gX11FrameB;
}

// screen -> client coordinates; in full screen the engine's "screen" is the display mode, which is the client area
void OS_ScreenToClient(int* x, int* y)
{
	int cx, cy;
	if(gFullscreen)
		return; // mode space is client space
	ClientOrigin(&cx, &cy);
	*x -= cx;
	*y -= cy;
}

// client -> screen coordinates (the identity in full screen, as above)
void OS_ClientToScreen(int* x, int* y)
{
	int cx, cy;
	if(gFullscreen)
		return;
	ClientOrigin(&cx, &cy);
	*x += cx;
	*y += cy;
}

static int gResizable; // OS_WindowSetResizable: the player may give the window another size
static int gUserSized; // and did: the client area is of a size the window manager set
#ifndef BGI_HAIKU
static unsigned long gSizeSerial; // the request the engine last set the window's size with
#endif
static int gAspectW, gAspectH; // OS_WindowSetAspect: the proportions the client area keeps meanwhile (0: any)

/* the engine owns the size: min == max keeps the window manager from
 * resizing the window - unless the window has a sizing border, which here
 * is no largest size and a smallest that still shows something, with the
 * proportions of OS_WindowSetAspect when there are any (a window manager
 * that does not keep them leaves bars beside the picture) */
static void SetSizeHints(int w, int h)
{
	XSizeHints* hints = XAllocSizeHints();
	if(!hints)
		return;
	hints->flags = PMinSize | PMaxSize | PWinGravity;
	hints->min_width = hints->max_width = w;
	hints->min_height = hints->max_height = h;
	if(gResizable && !gFullscreen)
	{
		hints->flags = PMinSize | PWinGravity;
		hints->min_width = 320;
		hints->min_height = 240;
		if(gAspectW > 0 && gAspectH > 0)
		{
			hints->flags |= PAspect;
			hints->min_aspect.x = hints->max_aspect.x = gAspectW;
			hints->min_aspect.y = hints->max_aspect.y = gAspectH;
			hints->min_height = hints->min_width * gAspectH / gAspectW; // (a smallest size of the same proportions)
			if(hints->min_height < 1)
				hints->min_height = 1;
		}
	}
	hints->win_gravity = NorthWestGravity;
	XSetWMNormalHints(gX11Dpy, gX11Win, hints);
	XFree(hints);
}

void OS_WindowSetResizable(int on)
{
	gResizable = on != 0;
	if(!gX11Win || gFullscreen)
		return;
	SetSizeHints(gClientW, gClientH);
	XFlush(gX11Dpy);
}

/* the window manager gave the window another size (ConfigureNotify): the
 * picture is scaled into the new client area from now on; 1 when the
 * size is news.  An event from before the engine last set the size (its
 * serial is older than that request's) tells of a size that is no more. */
int X11_ClientResized(int w, int h, unsigned long serial)
{
	if(!gResizable || gFullscreen || w <= 0 || h <= 0 || (w == gClientW && h == gClientH))
		return 0;
#ifndef BGI_HAIKU
	if(serial < gSizeSerial)
		return 0;
#endif
	gClientW = w;
	gClientH = h;
	gUserSized = 1;
	FsRecompute();
	X11_FillLetterbox();
	return 1;
}

void OS_WindowSetAspect(int w, int h)
{
	if(w <= 0 || h <= 0)
		w = h = 0;
	if(w == gAspectW && h == gAspectH)
		return;
	gAspectW = w;
	gAspectH = h;
	if(!gX11Win || gFullscreen)
		return;
	SetSizeHints(gClientW, gClientH);
	XFlush(gX11Dpy);
}

// ---- the window's screen -------------------------------------------------------------------------

#ifdef BGI_HAIKU

int OS_WindowFits(void)
{
	return 0;
}

int OS_WindowDpi(void)
{
	return 96;
}

void OS_WindowWorkArea(int* x, int* y, int* w, int* h)
{
	*x = *y = 0;
	OS_ScreenSize(w, h);
}

#else

int OS_WindowFits(void)
{
	return 1;
}

// a property of the root window as the server has it now: n items, to be freed with XFree; NULL when there is none
static unsigned char* RootProperty(Atom name, Atom type, long longs, unsigned long* n)
{
	Atom gotType;
	int fmt;
	unsigned long after;
	unsigned char* data = NULL;
	*n = 0;
	if(XGetWindowProperty(gX11Dpy, gX11Root, name, 0, longs, False, type, &gotType, &fmt, n, &after, &data) != Success)
		return NULL;
	if(data && (gotType != type || *n == 0))
	{
		XFree(data);
		data = NULL;
		*n = 0;
	}
	return data;
}

/* The scale the desktop is set to, in dots per inch: the "Xft.dpi" line
 * of the resources on the root window, which the desktops keep at 96
 * times their scale; 96 without one. */
int OS_WindowDpi(void)
{
	unsigned long n = 0;
	unsigned char* data;
	const char* p;
	double dpi = 0;
	if(!X11_Open())
		return 96;
	data = RootProperty(XA_RESOURCE_MANAGER, XA_STRING, 0x40000, &n);
	if(!data)
		return 96;
	for(p = (const char*)data; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL)
	{
		if(strncmp(p, "Xft.dpi:", 8) == 0)
		{
			dpi = atof(p + 8);
			break;
		}
	}
	XFree(data);
	return dpi >= 48.0 && dpi <= 960.0 ? (int)(dpi + 0.5) : 96;
}

/* The monitors, through libXrandr when the system has it (it is looked
 * for at run time: nothing is linked).  XRRMonitorInfo as Xrandr.h of
 * RandR 1.5 declares it. */
typedef struct
{
	Atom name;
	Bool primary, automatic;
	int noutput;
	int x, y, width, height;
	int mwidth, mheight;
	XID* outputs;
} RrMonitor_t;

static int gRrState; // 0: not looked for yet, 1: there, -1: not there
static RrMonitor_t* (*gRrGetMonitors)(Display* dpy, Window win, Bool active, int* count);
static void (*gRrFreeMonitors)(RrMonitor_t* list);

static int RrOpen(void)
{
	Bool (*queryExtension)(Display* dpy, int* eventBase, int* errorBase) = NULL;
	Status (*queryVersion)(Display* dpy, int* major, int* minor) = NULL;
	void* lib;
	void* sym[4];
	int a = 0, b = 0;
	if(gRrState)
		return gRrState > 0;
	gRrState = -1;
	lib = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL);
	if(!lib)
		return 0;
	sym[0] = dlsym(lib, "XRRQueryExtension");
	sym[1] = dlsym(lib, "XRRQueryVersion");
	sym[2] = dlsym(lib, "XRRGetMonitors");
	sym[3] = dlsym(lib, "XRRFreeMonitors");
	if(!sym[0] || !sym[1] || !sym[2] || !sym[3])
	{
		dlclose(lib);
		return 0;
	}
	// (an object pointer is not a function pointer to ISO C; POSIX says dlsym's is)
	memcpy(&queryExtension, &sym[0], sizeof queryExtension);
	memcpy(&queryVersion, &sym[1], sizeof queryVersion);
	memcpy(&gRrGetMonitors, &sym[2], sizeof gRrGetMonitors);
	memcpy(&gRrFreeMonitors, &sym[3], sizeof gRrFreeMonitors);
	if(!queryExtension(gX11Dpy, &a, &b) || !queryVersion(gX11Dpy, &a, &b) || a < 1 || (a == 1 && b < 5))
		return 0; // (the library stays loaded: the display knows its extension now)
	gRrState = 1;
	return 1;
}

/* the monitor the point (px, py) of the screen is on, else the primary
 * one, else the first; the whole screen when the monitors are not known */
static void MonitorAt(int px, int py, int* x, int* y, int* w, int* h)
{
	RrMonitor_t* list;
	int n = 0, i, pick = -1;
	*x = *y = 0;
	OS_ScreenSize(w, h);
	if(!RrOpen())
		return;
	list = gRrGetMonitors(gX11Dpy, gX11Root, True, &n);
	if(!list)
		return;
	for(i = 0; i < n && pick < 0; i++)
	{
		if(px >= list[i].x && px < list[i].x + list[i].width && py >= list[i].y && py < list[i].y + list[i].height)
			pick = i;
	}
	for(i = 0; i < n && pick < 0; i++)
	{
		if(list[i].primary)
			pick = i;
	}
	if(pick < 0 && n > 0)
		pick = 0;
	if(pick >= 0 && list[pick].width > 0 && list[pick].height > 0)
	{
		*x = list[pick].x;
		*y = list[pick].y;
		*w = list[pick].width;
		*h = list[pick].height;
	}
	gRrFreeMonitors(list);
}

/* What a window may take of its screen: the monitor the window's middle
 * is on (before there is a window: the one the pointer is on), without
 * what the desktop keeps for its panels (_NET_WORKAREA of the current
 * desktop, which is one rectangle for all monitors: a panel on one of
 * several monitors is not always left out). */
void OS_WindowWorkArea(int* x, int* y, int* w, int* h)
{
	int px = 0, py = 0, wx, wy, ww, wh, x1, y1;
	unsigned long n = 0, desk = 0;
	unsigned char* data;
	*x = *y = 0;
	*w = 1024;
	*h = 768;
	if(!X11_Open())
		return;
	if(gX11Win && !gFullscreen)
	{
		ClientOrigin(&px, &py);
		px += gClientW / 2;
		py += gClientH / 2;
	}
	else
	{
		Window root, child;
		int cx, cy;
		unsigned mask;
		XQueryPointer(gX11Dpy, gX11Root, &root, &child, &px, &py, &cx, &cy, &mask);
	}
	MonitorAt(px, py, x, y, w, h);
	data = RootProperty(XInternAtom(gX11Dpy, "_NET_CURRENT_DESKTOP", False), XA_CARDINAL, 1, &n);
	if(data)
	{
		desk = (unsigned long)*(const long*)data;
		XFree(data);
	}
	data = RootProperty(gX11AtomNetWorkarea, XA_CARDINAL, 4 * 64, &n);
	if(!data)
		return;
	if(n >= 4)
	{
		const long* v = (const long*)data + (n >= (desk + 1) * 4 ? desk * 4 : 0);
		wx = (int)v[0];
		wy = (int)v[1];
		ww = (int)v[2];
		wh = (int)v[3];
		x1 = *x + *w < wx + ww ? *x + *w : wx + ww;
		y1 = *y + *h < wy + wh ? *y + *h : wy + wh;
		wx = *x > wx ? *x : wx;
		wy = *y > wy ? *y : wy;
		if(x1 - wx >= 160 && y1 - wy >= 120)
		{ // (a work area that leaves next to nothing of the monitor is not believed)
			*x = wx;
			*y = wy;
			*w = x1 - wx;
			*h = y1 - wy;
		}
	}
	XFree(data);
}

#endif

// ---- creation ----------------------------------------------------------------------------------

static Cursor gCursorBlank, gCursorArrow, gCursorCustom; // the three cursors of ApplyCursor
static int gCursorShape = OS_CURSOR_ARROW;               // OS_CursorSetShape
static int gCursorCount;                                 // ShowCursor: visible while >= 0

// the cursors: an invisible 1 x 1 one, the arrow, and a hand for the original's custom cursor
static void MakeCursors(void)
{
	Pixmap pm;
	XColor black;
	static char zero[1] = {0};
	memset(&black, 0, sizeof black);
	pm = XCreateBitmapFromData(gX11Dpy, gX11Root, zero, 1, 1);
	gCursorBlank = XCreatePixmapCursor(gX11Dpy, pm, pm, &black, &black, 0, 0);
	XFreePixmap(gX11Dpy, pm);
	gCursorArrow = XCreateFontCursor(gX11Dpy, XC_left_ptr);
	gCursorCustom = XCreateFontCursor(gX11Dpy, XC_hand2); // the original's resource cursor 0x6A is not available
}

// the window's cursor from the shape and the show count: blank while hidden, else the shape's cursor
static void ApplyCursor(void)
{
	Cursor c;
	if(!gX11Win)
		return;
	if(gCursorCount < 0 || gCursorShape == OS_CURSOR_NONE)
		c = gCursorBlank;
	else
		c = gCursorShape == OS_CURSOR_CUSTOM ? gCursorCustom : gCursorArrow;
	XDefineCursor(gX11Dpy, gX11Win, c);
	XFlush(gX11Dpy);
}

static void ApplyIcon(void); // the icon OS_WindowSetIcon keeps, onto the window

/* Create the main window, unmapped: fixed size, WM_DELETE_WINDOW for the
 * close box, the process id and class hints for the window manager, the
 * title as OsCommon_MainTitle spells it, an input context for the text
 * entry, the cursors.  Raises `created` and creates the (hidden) text
 * entry.  0 without a display or when a window exists already. */
int OS_WindowCreate(const OsWindowDesc_t* d)
{
	XSetWindowAttributes attr;
	long pid;
	if(!X11_Open())
		return 0;
	if(gX11Win)
		return 0;
	OS_ScreenSize(&gScreenW, &gScreenH);
	gResizable = 0; // (a new window is of the engine's size until a script says otherwise)
	gUserSized = 0;
	gAspectW = gAspectH = 0;
	gClientW = d->width;
	gClientH = d->height;
	memset(&attr, 0, sizeof attr);
	attr.background_pixel = BlackPixel(gX11Dpy, gX11Screen);
	attr.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
		EnterWindowMask | LeaveWindowMask | ExposureMask | StructureNotifyMask | FocusChangeMask | PropertyChangeMask;
	attr.bit_gravity = NorthWestGravity;
	// (x, y) is the frame's corner: the client area goes inside the frame extents known so far
	gX11Win = XCreateWindow(gX11Dpy, gX11Root, d->x + gX11FrameL, d->y + gX11FrameT, (unsigned)d->width, (unsigned)d->height, 0,
		gX11Depth, InputOutput, gX11Visual, CWBackPixel | CWEventMask | CWBitGravity, &attr);
	if(!gX11Win)
		return 0;
	gX11MainDc.win = gX11Win;
	gAsked = 1;
	gAskedX = d->x;
	gAskedY = d->y;
	SetSizeHints(d->width, d->height);
	XSetWMProtocols(gX11Dpy, gX11Win, &gX11WmDelete, 1);
	pid = (long)getpid();
	XChangeProperty(gX11Dpy, gX11Win, gX11AtomNetWmPid, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&pid, 1);
	ApplyIcon();
	{
		XClassHint* ch = XAllocClassHint();
		if(ch)
		{
			ch->res_name = (char*)"openbgi";
			ch->res_class = (char*)"OpenBGI"; // the original's class is "Ethornell"
			XSetClassHint(gX11Dpy, gX11Win, ch);
			XFree(ch);
		}
	}
	{
		char title[0x400];
		OsCommon_MainTitle(title, sizeof title, d->title);
		X11_SetTitle(gX11Win, title);
	}
	// an input context for the text entry: typed text arrives through the input method when there is one
	if(gX11Im)
		gX11Xic = XCreateIC(gX11Im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, gX11Win, XNFocusWindow,
			gX11Win, NULL);
	MakeCursors();
	gGlDirty = 0;
	X11Gl_Attach(gX11Win); // (the picture through the graphics processor where there is one)
#ifndef BGI_HAIKU
	RequestFrameExtents();
#endif
	gX11Alive = 1;
	gX11Shown = 0;
	gX11Minimized = 0;
	gCursorCount = 0;
	gCursorShape = OS_CURSOR_ARROW;
	ApplyCursor();
	return 1;
}

// DestroyWindow: the input context and the window go
void OS_WindowDestroy(void)
{
	if(!gX11Win)
		return;
	if(gX11Xic)
	{
		XDestroyIC(gX11Xic);
		gX11Xic = NULL;
	}
	X11Gl_Detach();
	XDestroyWindow(gX11Dpy, gX11Win);
	XFlush(gX11Dpy);
	gX11Win = 0;
	X11_DndReset();
	gOriginKnown = 0;
	gX11MainDc.win = 0;
	gX11Alive = 0;
}

// map (raised) or unmap the window; gX11HiddenByUs tells the pump's UnmapNotify from an iconification
void OS_WindowShow(int show)
{
	if(!gX11Win)
		return;
	if(show)
	{
		gX11HiddenByUs = 0;
		XMapRaised(gX11Dpy, gX11Win);
		gX11Shown = 1;
	}
	else
	{
		gX11HiddenByUs = 1;
		XUnmapWindow(gX11Dpy, gX11Win);
		gX11Shown = 0;
	}
	XFlush(gX11Dpy);
}

// the title with " - OpenBGI v1.0" appended; NULL leaves "OpenBGI v1.0" alone, as SetWindowTextA(NULL) clears the text
void OS_WindowSetTitle(const char* t)
{
	char title[0x400];
	if(!gX11Win)
		return;
	OsCommon_MainTitle(title, sizeof title, t); // SetWindowTextA(NULL) clears the title
	X11_SetTitle(gX11Win, title);
}

// move the frame's corner to (x, y); ignored in full screen
void OS_WindowMove(int x, int y)
{
	if(!gX11Win || gFullscreen)
		return;
	XMoveWindow(gX11Dpy, gX11Win, x, y); // NorthWest gravity: the frame's corner goes here
	gAskedX = x;
	gAskedY = y;
	gOriginKnown = 0;
	XFlush(gX11Dpy);
}

int OS_WindowMinimized(void)
{
	return gX11Minimized;
}

// iconify through the window manager; the pump's UnmapNotify then reports the window minimised
void OS_WindowMinimize(void)
{
	if(gX11Win)
		XIconifyWindow(gX11Dpy, gX11Win, gX11Screen);
}

/* SIGTERM / SIGINT (process.c): the quit the window manager's close would
 * post once the engine agreed to it (the host's `quit`, OpenBGI_Quit), so
 * that an unattended run ends through the engine's own shutdown (and
 * writes BGI_OPSTAT).  Only a flag is set: the pump raises the event. */
void OsPosix_RequestClose(void)
{
	gX11QuitPosted = 1;
}

// no IME status window to show or hide: the input method has none of its own here
void OS_ImeStatus(int open)
{
}

/* the input method composes for the main window while the engine takes
 * text (its text entry or a dialog's field has the focus); where the
 * field is does not matter to an input method that shows its own window */
void OS_TextInput(int on, int x, int y, int w, int h)
{
	gX11TextInput = on != 0;
	if(!gX11Xic)
		return;
	if(on)
		XSetICFocus(gX11Xic);
	else
		XUnsetICFocus(gX11Xic);
}

// no edit control of the system's: the engine draws the text entry in the frame
const OpenBGI_TextEntry_t* OS_TextEntry(void)
{
	return NULL;
}

// the message boxes and the dialogs are windows of their own (uikit.c)
uint32_t OS_HostCaps(void)
{
	return OPENBGI_CAP_DIALOGS | OPENBGI_CAP_POINTER | OPENBGI_CAP_KEYBOARD;
}

// ---- display modes -----------------------------------------------------------------------------

// no mode switch: the requested mode is remembered and scaled onto the screen (see FsRecompute); bpp is ignored
int OS_DisplaySetMode(int fullscreen, int w, int h, int bpp)
{
	// no mode switch: the requested mode is scaled onto the screen (see FsRecompute)
	if(w <= 0 || h <= 0)
		return 0;
	gFsModeW = w;
	gFsModeH = h;
	return 1;
}

// nothing was switched, so there is nothing to restore
void OS_DisplayRestore(void)
{
}

#ifdef BGI_HAIKU
/* Xlibe: no full-screen state, but _MOTIF_WM_HINTS sets the window's look -
 * none (no tab, no border) in full screen, the titled window otherwise;
 * the caller moves the window over the screen or back */
static void SetFullscreenState(int on)
{
	Atom hints = XInternAtom(gX11Dpy, "_MOTIF_WM_HINTS", False);
	long v[5] = {2, 0, 0, 0, 0}; // MWM_HINTS_DECORATIONS, then the decorations
	v[2] = on ? 0 : 1;           // none / MWM_DECOR_ALL
	XChangeProperty(gX11Dpy, gX11Win, hints, hints, 32, PropModeReplace, (unsigned char*)v, 5);
}
#else
// ask the window manager to add or remove _NET_WM_STATE_FULLSCREEN on the mapped window (the EWMH client message)
static void SetNetWmFullscreen(int on)
{
	XEvent e;
	memset(&e, 0, sizeof e);
	e.xclient.type = ClientMessage;
	e.xclient.window = gX11Win;
	e.xclient.message_type = gX11AtomNetWmState;
	e.xclient.format = 32;
	e.xclient.data.l[0] = on ? 1 : 0; // _NET_WM_STATE_ADD / REMOVE
	e.xclient.data.l[1] = (long)gX11AtomNetWmStateFs;
	e.xclient.data.l[2] = 0;
	e.xclient.data.l[3] = 1; // normal application
	XSendEvent(gX11Dpy, gX11Root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

/* the full-screen state goes through the window manager when the window
 * is mapped and is set as a property on the unmapped window otherwise (the
 * window manager reads it when mapping) */
static void SetFullscreenState(int on)
{
	if(gX11Shown)
		SetNetWmFullscreen(on);
	else if(on)
		XChangeProperty(gX11Dpy, gX11Win, gX11AtomNetWmState, XA_ATOM, 32, PropModeReplace, (unsigned char*)&gX11AtomNetWmStateFs, 1);
	else
		XDeleteProperty(gX11Dpy, gX11Win, gX11AtomNetWmState);
}
#endif

// the window over the whole screen with the mode scaled onto it
void OS_WindowSetFullscreen(int modeW, int modeH)
{
	if(!gX11Win)
		return;
	OS_ScreenSize(&gScreenW, &gScreenH);
	gFsModeW = modeW;
	gFsModeH = modeH;
	gFullscreen = 1;
	FsRecompute();
	SetSizeHints(gScreenW, gScreenH);
	SetFullscreenState(1);
	XMoveResizeWindow(gX11Dpy, gX11Win, 0, 0, (unsigned)gScreenW, (unsigned)gScreenH);
	gOriginKnown = 0;
	XFlush(gX11Dpy);
	X11_FillLetterbox();
}

// back to (or still) a framed window of clientW x clientH at the outer origin (x, y)
void OS_WindowSetWindowed(int x, int y, int clientW, int clientH)
{
	if(!gX11Win)
		return;
	if(gFullscreen)
	{
		gFullscreen = 0;
		FsRecompute();
		SetFullscreenState(0);
	}
	if(gResizable && gUserSized && clientW == gClientW && clientH == gClientH)
		return; // the size the player gave it: nothing to do, and the window manager may still be moving it
	gUserSized = 0;
	gClientW = clientW;
	gClientH = clientH;
	FsRecompute();
	SetSizeHints(clientW, clientH);
#ifndef BGI_HAIKU
	gSizeSerial = NextRequest(gX11Dpy);
#endif
	XMoveResizeWindow(gX11Dpy, gX11Win, x, y, (unsigned)clientW, (unsigned)clientH);
	gAskedX = x;
	gAskedY = y;
	gOriginKnown = 0;
	XFlush(gX11Dpy);
}

// the rule of the scaling; the picture is shown again by it at its next frame or repaint
void OS_WindowSetScaling(int rule)
{
	int was = gScaleRule;
	gScaleRule = rule;
	if(rule == was || !gX11Win)
		return;
	if((rule ^ was) & OPENBGI_SCALE_WHOLE)
	{ // (the steps decide how large the picture is)
		FsRecompute();
		if(!X11Gl_On())
			XClearWindow(gX11Dpy, gX11Win);
	}
	if(gX11Input.paint)
		gX11Input.paint(gX11Input.user);
}

int OS_WindowScales(void)
{
	return 1;
}

/* The icon as the window managers take it (_NET_WM_ICON): for every
 * picture its width, its height and its pixels as A R G B in one word
 * each.  A picture of more than 128 pixels a side is halved until it is
 * not: the property has to fit a request.  The words are kept, so that a
 * window made later gets them too. */
static unsigned long* gIconData;
static int gIconWords;

static void ApplyIcon(void)
{
	Atom icon;
	if(!gX11Win || !gIconData)
		return;
	icon = XInternAtom(gX11Dpy, "_NET_WM_ICON", False);
	XChangeProperty(gX11Dpy, gX11Win, icon, XA_CARDINAL, 32, PropModeReplace, (const unsigned char*)gIconData, gIconWords);
}

void OS_WindowSetIcon(const OpenBGI_Icon_t* icons, int count)
{
	int i, x, y, words = 0, at = 0;
	free(gIconData);
	gIconData = NULL;
	gIconWords = 0;
	for(i = 0; i < count; i++)
	{
		int step = 1;
		while(icons[i].width / step > 128 || icons[i].height / step > 128)
			step *= 2;
		words += 2 + (icons[i].width / step) * (icons[i].height / step);
	}
	if(count < 1 || (gIconData = (unsigned long*)malloc((size_t)words * sizeof(unsigned long))) == NULL)
		return;
	for(i = 0; i < count; i++)
	{
		int step = 1, w, h;
		while(icons[i].width / step > 128 || icons[i].height / step > 128)
			step *= 2;
		w = icons[i].width / step;
		h = icons[i].height / step;
		gIconData[at++] = (unsigned long)w;
		gIconData[at++] = (unsigned long)h;
		for(y = 0; y < h; y++)
			for(x = 0; x < w; x++)
			{ // the mean of step x step pixels, weighted by their alpha
				unsigned long sum[4] = {0, 0, 0, 0};
				int sx, sy;
				for(sy = 0; sy < step; sy++)
					for(sx = 0; sx < step; sx++)
					{
						const uint8_t* p = icons[i].pixels + ((size_t)(y * step + sy) * (size_t)icons[i].width + (size_t)(x * step + sx)) * 4;
						sum[0] += (unsigned long)p[0] * p[3];
						sum[1] += (unsigned long)p[1] * p[3];
						sum[2] += (unsigned long)p[2] * p[3];
						sum[3] += p[3];
					}
				if(sum[3])
					gIconData[at++] = ((sum[3] / (unsigned long)(step * step)) << 24) | ((sum[2] / sum[3]) << 16) | ((sum[1] / sum[3]) << 8) | (sum[0] / sum[3]);
				else
					gIconData[at++] = 0;
			}
	}
	gIconWords = at;
	ApplyIcon();
}

int OS_WindowHasIcons(void)
{
	return 1;
}

// the picture the engine draws: scaled into a windowed client area of another size (see FsRecompute)
void OS_WindowSetPicture(int w, int h)
{
	if(gFullscreen || w <= 0 || h <= 0)
		return;
	gFsModeW = w;
	gFsModeH = h;
	FsRecompute();
	if(gX11Win)
		X11_FillLetterbox();
}

// ---- cursor ------------------------------------------------------------------------------------

// the shape applies at once (the original waits for WM_SETCURSOR; the effect is the same)
void OS_CursorSetShape(int shape)
{
	gCursorShape = shape;
	ApplyCursor();
}

// ShowCursor's counter: +1 / -1, the cursor is visible while the count is not negative
void OS_CursorShow(int show)
{
	gCursorCount += show ? 1 : -1;
	ApplyCursor();
}

/* the pointer in screen coordinates (in full screen: mode coordinates,
 * since that is the engine's screen), from the position the last event
 * reported, with one query when no event has reported it yet */
void OS_CursorGetPos(int* x, int* y)
{
	int rx = 0, ry = 0;
	*x = *y = 0;
	if(!gX11Dpy)
		return;
	if(gPtrKnown)
	{
		rx = gPtrX;
		ry = gPtrY;
	}
	else
	{
		Window root, child;
		int wx, wy;
		unsigned mask;
		XQueryPointer(gX11Dpy, gX11Root, &root, &child, &rx, &ry, &wx, &wy, &mask);
		X11_NotePointer(rx, ry);
	}
	if(gFullscreen)
	{ // mode coordinates
		int cx, cy;
		ClientOrigin(&cx, &cy);
		*x = X11_FromWinX(rx - cx);
		*y = X11_FromWinY(ry - cy);
	}
	else if(gMapOn)
	{ // screen coordinates whose client part is in picture pixels (OS_ScreenToClient subtracts the origin)
		int cx, cy;
		ClientOrigin(&cx, &cy);
		*x = cx + X11_FromWinX(rx - cx);
		*y = cy + X11_FromWinY(ry - cy);
	}
	else
	{
		*x = rx;
		*y = ry;
	}
}

// SetCursorPos: warp the pointer; the coordinates go through the full-screen mapping like OS_CursorGetPos
void OS_CursorSetPos(int x, int y)
{
	if(!gX11Dpy)
		return;
	if(gFullscreen)
	{
		int cx, cy;
		ClientOrigin(&cx, &cy);
		x = X11_ToWinX(x) + cx;
		y = X11_ToWinY(y) + cy;
	}
	else if(gMapOn)
	{
		int cx, cy;
		ClientOrigin(&cx, &cy);
		x = X11_ToWinX(x - cx) + cx;
		y = X11_ToWinY(y - cy) + cy;
	}
#ifdef BGI_HAIKU
	OsHaiku_SetMousePosition(x, y);
#else
	XWarpPointer(gX11Dpy, None, gX11Root, 0, 0, 0, 0, x, y);
	XFlush(gX11Dpy);
#endif
	X11_NotePointer(x, y);
}
