/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.c - the POSIX back end's main window (inc/bgi/os.h)
 *
 * One top-level X11 window stands in for the engine's main window.  Full
 * screen does not switch the display mode: the window covers the screen
 * (_NET_WM_STATE_FULLSCREEN) and the mode the engine asked for is scaled
 * to fit, centred, with every coordinate the engine sees or hands out
 * translated between the "mode" space and the real screen.  A windowed
 * client area of another size than the picture (OS_WindowSetPicture, the
 * "81 64" window size of 1.529 on) is handled by the same mapping.  The pop-up
 * child windows are in child.c, the text entry in edit.c, the event pump
 * in events.c, the debugger's window in dbgwin.c, and the message boxes
 * and dialogs in msgbox.c and dialogs.c on the toolkit of uikit.c.
 *
 * Not reproduced: drag and drop of files onto the window (XDND) - the
 * engine's drop_file event is never raised - and the IME status window.
 */
#include "x11_internal.h"

// ---- the main window ---------------------------------------------------------------------------

Window gX11Win;                                     // 0 before OS_WindowCreate and after OS_WindowDestroy
XIC gX11Xic;                                        // the input context of the main window, NULL without an input method
X11Dc_t gX11MainDc = {0, -1};                       // what OS_WindowGetDc hands out
int gX11Alive, gX11Shown;                           // created / mapped
int gX11Active, gX11Minimized, gX11HiddenByUs;      // focus and iconic state (events.c keeps them); hidden by OS_WindowShow(0)
static int gClientW = 800, gClientH = 600;          // the engine's client size (the mode size in full screen)
int gX11FrameL, gX11FrameT, gX11FrameR, gX11FrameB; // _NET_FRAME_EXTENTS once the window manager tells
static int gFullscreen;                             // the window covers the screen
static int gFsModeW, gFsModeH;                      // the display mode (full screen) or picture (windowed) the engine draws
static int gMapOn;                                  // the mode is scaled into the window: full screen, or a client area of another size
static double gFsScale = 1.0;                       // mode -> window pixels
static int gFsOffX, gFsOffY;                        // where the scaled mode sits in the window
static int gScreenW, gScreenH;                      // the screen in pixels (OS_ScreenSize)
int gX11CloseRequested, gX11QuitPosted;             // OS_WindowClose / OS_PostQuit, delivered by the next pump
uint8_t gX11Keys[256];                              // GetAsyncKeyState emulation, indexed by virtual key
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
 * the two ratios so that the whole mode fits with its aspect kept,
 * centred, the rest letterboxed (X11_FillLetterbox).  No mapping while a
 * windowed client area is of the mode's size. */
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

/* the workhorse behind OS_BlitToWindow / OS_StretchToWindow: the source
 * rectangle of `s` stretched to the destination rectangle (in the target
 * window's engine coordinates), clipped to `clip` ({l, t, r, b}, exclusive)
 * and to the window; `smooth` filters bilinearly when the sizes differ.
 * On the main window the destination goes through the full-screen
 * mapping (so the engine's blit of the mode-sized picture lands scaled
 * on the screen); a child window is never scaled.  Source pixels outside
 * the surface come out black. */
void X11_BlitSurface(const X11Dc_t* dc, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* s, int srcX,
	int srcY, int srcW, int srcH, int smooth, const int* clip)
{
	int cl = 0, ct = 0, cr, cb;         // destination clip in window pixels
	int wx0, wy0, wx1, wy1, w, h, x, y; // the destination in window pixels, the visible part's size
	uint32_t* d;
	int main = dc->child < 0;
	Window win = dc->win;
	double fx, fy; // destination -> source scale

	if(!gX11Dpy || !win || dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0 || !s->pixels)
		return;
	if(main)
	{
		int ww = gFullscreen ? gScreenW : gClientW, wh = gFullscreen ? gScreenH : gClientH;
		cr = ww;
		cb = wh;
		wx0 = X11_ToWinX(dstX);
		wy0 = X11_ToWinY(dstY);
		wx1 = X11_ToWinX(dstX + dstW);
		wy1 = X11_ToWinY(dstY + dstH);
		if(clip)
		{
			int l = X11_ToWinX(clip[0]), t = X11_ToWinY(clip[1]), r = X11_ToWinX(clip[2]), b = X11_ToWinY(clip[3]);
			if(l > cl)
				cl = l;
			if(t > ct)
				ct = t;
			if(r < cr)
				cr = r;
			if(b < cb)
				cb = b;
		}
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
		wx1 = dstX + dstW;
		wy1 = dstY + dstH;
		if(clip)
		{
			if(clip[0] > cl)
				cl = clip[0];
			if(clip[1] > ct)
				ct = clip[1];
			if(clip[2] < cr)
				cr = clip[2];
			if(clip[3] < cb)
				cb = clip[3];
		}
	}
	if(wx1 <= wx0 || wy1 <= wy0)
		return;
	fx = (double)srcW / (wx1 - wx0);
	fy = (double)srcH / (wy1 - wy0);
	// the visible part
	if(cl < wx0)
		cl = wx0;
	if(ct < wy0)
		ct = wy0;
	if(cr > wx1)
		cr = wx1;
	if(cb > wy1)
		cb = wy1;
	w = cr - cl;
	h = cb - ct;
	if(w <= 0 || h <= 0)
		return;
	d = X11_ConvBuffer((size_t)w * h);
	if(!d)
		return;
	if(fx == 1.0 && fy == 1.0)
	{ // plain copy
		int direct = s->bpp == 32 && gX11VisRBits == 8 && gX11VisGBits == 8 && gX11VisBBits == 8 && gX11VisRShift == 16 &&
			gX11VisGShift == 8 && gX11VisBShift == 0; // the surface's own layout: rows go as they are
		for(y = 0; y < h; y++)
		{
			int sy = srcY + (ct - wy0) + y;
			int sx0 = srcX + (cl - wx0);
			uint32_t* o = d + (size_t)y * w;
			if(sy < 0 || sy >= s->height)
			{
				memset(o, 0, (size_t)w * 4);
				continue;
			}
			if(direct && sx0 >= 0 && sx0 + w <= s->width)
			{
				memcpy(o, (const uint8_t*)s->pixels + (ptrdiff_t)sy * s->pitch + (ptrdiff_t)sx0 * 4, (size_t)w * 4);
				continue;
			}
			for(x = 0; x < w; x++)
			{
				int sx = sx0 + x;
				o[x] = (sx >= 0 && sx < s->width) ? X11_PackPixel(X11_SurfacePixel(s, sx, sy)) : 0;
			}
		}
	}
	else if(!smooth)
	{ // nearest neighbour
		for(y = 0; y < h; y++)
		{
			int sy = srcY + (int)((ct - wy0 + y) * fy);
			uint32_t* o = d + (size_t)y * w;
			if(sy >= srcY + srcH)
				sy = srcY + srcH - 1;
			for(x = 0; x < w; x++)
			{
				int sx = srcX + (int)((cl - wx0 + x) * fx);
				if(sx >= srcX + srcW)
					sx = srcX + srcW - 1;
				o[x] = (sx >= 0 && sx < s->width && sy >= 0 && sy < s->height) ? X11_PackPixel(X11_SurfacePixel(s, sx, sy)) : 0;
			}
		}
	}
	else
	{ // bilinear (HALFTONE): each output pixel from the four source pixels around its centre, weights in 1/256
		for(y = 0; y < h; y++)
		{
			double syf = srcY + ((ct - wy0 + y) + 0.5) * fy - 0.5;
			int sy0 = (int)syf, sy1;
			int wy = (int)((syf - sy0) * 256);
			uint32_t* o = d + (size_t)y * w;
			if(syf < 0)
				sy0 = 0, wy = 0;
			sy1 = sy0 + 1 < srcY + srcH ? sy0 + 1 : sy0;
			if(sy0 >= s->height)
				sy0 = s->height - 1;
			if(sy1 >= s->height)
				sy1 = s->height - 1;
			for(x = 0; x < w; x++)
			{
				double sxf = srcX + ((cl - wx0 + x) + 0.5) * fx - 0.5;
				int sx0 = (int)sxf, sx1;
				int wx = (int)((sxf - sx0) * 256);
				uint32_t p00, p01, p10, p11, r, g, b;
				if(sxf < 0)
					sx0 = 0, wx = 0;
				sx1 = sx0 + 1 < srcX + srcW ? sx0 + 1 : sx0;
				if(sx0 >= s->width)
					sx0 = s->width - 1;
				if(sx1 >= s->width)
					sx1 = s->width - 1;
				p00 = X11_SurfacePixel(s, sx0, sy0);
				p01 = X11_SurfacePixel(s, sx1, sy0);
				p10 = X11_SurfacePixel(s, sx0, sy1);
				p11 = X11_SurfacePixel(s, sx1, sy1);
#define CH(p, sh)     (((p) >> (sh)) & 0xff)
#define LERP(a, b, t) ((a) + ((((b) - (a)) * (t)) >> 8))
				r = LERP(LERP(CH(p00, 16), CH(p01, 16), wx), LERP(CH(p10, 16), CH(p11, 16), wx), wy);
				g = LERP(LERP(CH(p00, 8), CH(p01, 8), wx), LERP(CH(p10, 8), CH(p11, 8), wx), wy);
				b = LERP(LERP(CH(p00, 0), CH(p01, 0), wx), LERP(CH(p10, 0), CH(p11, 0), wx), wy);
#undef CH
#undef LERP
				o[x] = X11_PackPixel((r << 16) | (g << 8) | b);
			}
		}
	}
	X11_PutImage(win, cl, ct, w, h, d);
}

// ---- the text entry's capture (see X11Edit_Present) --------------------------------------------

X11Capture_t gX11Capture;

/* a blit to the main window while the capture is active: the part that
 * falls into the captured rectangle goes into the capture buffer (as
 * 0x00RRGGBB) instead of the window; the rest is dropped */
static void CaptureBlit(int dstX, int dstY, int dstW, int dstH, const OsSurface_t* s, int srcX, int srcY)
{
	int x, y;
	for(y = 0; y < dstH; y++)
	{
		int cy = dstY + y - gX11Capture.y, sy = srcY + y;
		if(cy < 0 || cy >= gX11Capture.h || sy < 0 || sy >= s->height)
			continue;
		for(x = 0; x < dstW; x++)
		{
			int cx = dstX + x - gX11Capture.x, sx = srcX + x;
			if(cx < 0 || cx >= gX11Capture.w || sx < 0 || sx >= s->width)
				continue;
			gX11Capture.pix[(size_t)cy * gX11Capture.w + cx] = X11_SurfacePixel(s, sx, sy);
		}
	}
}

// the "device context" is a token naming the window; nothing is acquired, so there is nothing to release
OsDc_t* OS_WindowGetDc(void)
{
	return gX11Alive ? &gX11MainDc : NULL;
}

void OS_WindowReleaseDc(OsDc_t* dc)
{
}

// BitBlt: an unscaled copy; while the text entry captures, a blit to the main window feeds its buffer instead
void OS_BlitToWindow(OsDc_t* dc, int dstX, int dstY, int w, int h, const OsSurface_t* src, int srcX, int srcY)
{
	const X11Dc_t* d = dc ? (const X11Dc_t*)dc : &gX11MainDc;
	if(gX11Capture.active && d->child < 0)
	{
		CaptureBlit(dstX, dstY, w, h, src, srcX, srcY);
		return;
	}
	X11_BlitSurface(d, dstX, dstY, w, h, src, srcX, srcY, w, h, 0, NULL);
}

// StretchDIBits of the whole surface; a clip rectangle also selects the bilinear (HALFTONE) filter
void OS_StretchToWindow(OsDc_t* dc, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* src, const int* clip)
{
	const X11Dc_t* d = dc ? (const X11Dc_t*)dc : &gX11MainDc;
	X11_BlitSurface(d, dstX, dstY, dstW, dstH, src, 0, 0, src->width, src->height, clip != NULL, clip);
}

// the whole window black (the screen in full screen, the client area otherwise)
void OS_WindowFillBlack(void)
{
	GC gc;
	if(!gX11Alive)
		return;
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
	if(!gMapOn || !gX11Win)
		return;
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
void X11_ReadFrameExtents(void)
{
	Atom type;
	int fmt;
	unsigned long n, after;
	unsigned char* data = NULL;
	if(!gX11Win)
		return;
	if(XGetWindowProperty(gX11Dpy, gX11Win, gX11AtomNetFrameExtents, 0, 4, False, XA_CARDINAL, &type, &fmt, &n, &after, &data) ==
			Success &&
		data)
	{
		if(n == 4 && fmt == 32)
		{
			const long* v = (const long*)data;
			gX11FrameL = (int)v[0];
			gX11FrameR = (int)v[1];
			gX11FrameT = (int)v[2];
			gX11FrameB = (int)v[3];
		}
		XFree(data);
	}
}

int X11_FrameLeft(void)
{
	return gX11FrameL;
}
int X11_FrameTop(void)
{
	return gX11FrameT;
}

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

// the engine owns the size: min == max keeps the window manager from resizing the window
static void SetSizeHints(int w, int h)
{
	XSizeHints* hints = XAllocSizeHints();
	if(!hints)
		return;
	hints->flags = PMinSize | PMaxSize | PWinGravity;
	hints->min_width = hints->max_width = w;
	hints->min_height = hints->max_height = h;
	hints->win_gravity = NorthWestGravity;
	XSetWMNormalHints(gX11Dpy, gX11Win, hints);
	XFree(hints);
}

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
	SetSizeHints(d->width, d->height);
	XSetWMProtocols(gX11Dpy, gX11Win, &gX11WmDelete, 1);
	pid = (long)getpid();
	XChangeProperty(gX11Dpy, gX11Win, gX11AtomNetWmPid, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&pid, 1);
	{
		XClassHint* ch = XAllocClassHint();
		if(ch)
		{
			ch->res_name = (char*)"bgi";
			ch->res_class = (char*)"BGI"; // the original's class is "Ethornell"
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
	gX11Alive = 1;
	gX11Shown = 0;
	gX11Minimized = 0;
	gCursorCount = 0;
	gCursorShape = OS_CURSOR_ARROW;
	ApplyCursor();
	memset(gX11Keys, 0, sizeof gX11Keys);
	if(gX11Handlers.created)
		gX11Handlers.created();
	X11Edit_Create();
	return 1;
}

// DestroyWindow: the input context and the window go; `destroyed` is raised before the return, as WM_DESTROY is
void OS_WindowDestroy(void)
{
	if(!gX11Win)
		return;
	if(gX11Xic)
	{
		XDestroyIC(gX11Xic);
		gX11Xic = NULL;
	}
	XDestroyWindow(gX11Dpy, gX11Win);
	XFlush(gX11Dpy);
	gX11Win = 0;
	gOriginKnown = 0;
	gX11MainDc.win = 0;
	gX11Alive = 0;
	if(gX11Handlers.destroyed)
		gX11Handlers.destroyed(); // WM_DESTROY is synchronous in DestroyWindow
}

int OS_WindowAlive(void)
{
	return gX11Alive;
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

// the title with " - OpenBGI" appended; NULL leaves "OpenBGI" alone, as SetWindowTextA(NULL) clears the text
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
	gOriginKnown = 0;
	XFlush(gX11Dpy);
}

int OS_WindowMinimized(void)
{
	return gX11Minimized;
}

int OS_WindowActive(void)
{
	return gX11Active;
}

// iconify through the window manager; the pump's UnmapNotify then reports the window minimised
void OS_WindowMinimize(void)
{
	if(gX11Win)
		XIconifyWindow(gX11Dpy, gX11Win, gX11Screen);
}

void OS_WindowClose(void)
{
	gX11CloseRequested = 1; // delivered from the pump like a posted WM_CLOSE
}

/* SIGTERM / SIGINT (process.c): the quit the window manager's close would
 * post once the engine agreed to it (gX11Handlers.quit), so that an
 * unattended run ends through the engine's own shutdown (and writes
 * BGI_OPSTAT).  Only a flag is set: the pump raises the event. */
void OsPosix_RequestClose(void)
{
	gX11QuitPosted = 1;
}

// no IME status window to show or hide: the input method has none of its own here
void OS_ImeStatus(int open)
{
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

/* the window over the whole screen with the mode scaled onto it: the
 * full-screen state goes through the window manager when the window is
 * mapped and is set as a property on the unmapped window otherwise (the
 * window manager reads it when mapping) */
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
	if(gX11Shown)
		SetNetWmFullscreen(1);
	else
		XChangeProperty(gX11Dpy, gX11Win, gX11AtomNetWmState, XA_ATOM, 32, PropModeReplace, (unsigned char*)&gX11AtomNetWmStateFs, 1);
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
		if(gX11Shown)
			SetNetWmFullscreen(0);
		else
			XDeleteProperty(gX11Dpy, gX11Win, gX11AtomNetWmState);
	}
	gClientW = clientW;
	gClientH = clientH;
	FsRecompute();
	SetSizeHints(clientW, clientH);
	XMoveResizeWindow(gX11Dpy, gX11Win, x, y, (unsigned)clientW, (unsigned)clientH);
	gOriginKnown = 0;
	XFlush(gX11Dpy);
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
	XWarpPointer(gX11Dpy, None, gX11Root, 0, 0, 0, 0, x, y);
	XFlush(gX11Dpy);
	X11_NotePointer(x, y);
}

// the "swap mouse buttons" setting: a pointer mapping that exchanges buttons 1 and 3 (a left-handed setup)
int OS_MouseButtonsSwapped(void)
{
	unsigned char map[8];
	int n;
	if(!gX11Dpy)
		return 0;
	n = XGetPointerMapping(gX11Dpy, map, 8);
	return n >= 3 && map[0] == 3 && map[2] == 1; // buttons 1 and 3 exchanged
}

// GetAsyncKeyState & 0x8000: the key is down, from the presses and releases the pump has seen
int OS_KeyState(int vk)
{
	if(vk < 0 || vk > 255)
		return 0;
	return gX11Keys[vk];
}

// GetKeyboardState: bit 7 of each entry from the same record; the toggle bits (Caps Lock etc.) are not tracked
void OS_KeyboardState(uint8_t out[256])
{
	int i;
	for(i = 0; i < 256; i++)
		out[i] = gX11Keys[i] ? 0x80 : 0; // no toggle bits (Caps Lock etc.) are tracked
}
