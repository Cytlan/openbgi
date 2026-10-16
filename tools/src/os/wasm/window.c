/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.c - the WebAssembly back end's window: a canvas, its input, the
 *            cursor, the dialogs, and the stubs of what a page has not got
 *
 * The main window is the page's <canvas id="canvas">, created hidden and
 * shown by OS_WindowShow like the original's.  The engine's back buffer is
 * copied into it rectangle by rectangle (OS_BlitToWindow / OS_StretchToWindow
 * convert the rows to RGBA and putImageData them); the canvas keeps the
 * engine's client size - the display mode's size in full screen - and the
 * page's CSS scales it to the viewport, so nothing here maps coordinates.
 *
 * Input arrives through the DOM handlers installed by WasmJs_InstallInput:
 * they call openbgi_push_event (host.c), which queues the event here, and
 * OS_PumpMessages hands one event per call to the engine's callbacks like
 * the message pump of the original.  Key codes are the browser's keyCode,
 * which is the Windows virtual key for everything the games use.
 *
 * Child windows ("B0 1x") and the text entry ("B0 2x") do not exist on a
 * page: their functions fail or do nothing, which the engine reports the
 * way it reports a window it could not create.  The dialogs are the
 * browser's confirm / alert / prompt.
 *
 * Also here, because it is where the engine returns to the browser:
 * OsWasm_Yield, the suspension point that every sleep and the message pump
 * go through (os_wasm.h).  The interface is the window, display, input,
 * child window, text entry, dialog and clipboard sections of os.h; the
 * debugger's window is not part of this build.  The WasmJs_* functions are
 * the page side (EM_JS); tools/wasm/fakebrowser.c stands in for them on a
 * host.
 */
#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_wasm.h"

#include <emscripten/emscripten.h>

static OsEventHandlers_t gH;                             // the engine's callbacks (OS_SetEventHandlers)
static int gAlive, gShown, gActive = 1, gFullscreen;     // the window exists / is visible / has the focus / covers a mode
static int gClientW = 800, gClientH = 600;               // the windowed client size in pixels
static int gModeW, gModeH;                               // the display mode of full screen, pixels (0 = none set yet)
static int gCursorShape = OS_CURSOR_ARROW, gCursorCount; // OS_CursorSetShape's shape; the ShowCursor counter (visible at >= 0)
static int gCloseRequested, gQuitPosted;                 // OS_WindowClose / OS_PostQuit, delivered by the next pump
static uint8_t gKeys[256];                               // 1 per virtual key held down (OS_KeyState)
static int gMouseX, gMouseY;                             // the pointer in canvas pixels, from the last event
static int gLastClickBtn = -1;                           // the double-click detection: the last press's button (-1 none),
static uint32_t gLastClickTime;                          // its time (OS_TicksMs)
static int gLastClickX, gLastClickY;                     // and its place (canvas pixels)
static uint32_t gLastYield;                              // OS_TicksMs of the last return to the browser
static int gDc;                                          // the one device context: its address is the handle

// ---- the page ----------------------------------------------------------------------------

/* size the canvas to the window's client area (or the display mode in full
 * screen), show or hide it, give it the focus when shown and mark it with
 * the "fullscreen" class the page's CSS scales; Module.onCanvasResized lets
 * the page react */
// clang-format off
EM_JS(void, WasmJs_CanvasSetup, (int w, int h, int visible, int fullscreen), {
	var c = Module['canvas'];
	if(!c)
		return;
	if(c.width !== w || c.height !== h)
	{
		c.width = w;
		c.height = h;
	}
	c.style.visibility = visible ? 'visible' : 'hidden';
	if(visible && document.activeElement !== c)
		c.focus();
	if(fullscreen)
		c.classList.add('fullscreen');
	else
		c.classList.remove('fullscreen');
	if(Module['onCanvasResized'])
		Module['onCanvasResized'](w, h, fullscreen);
});
// clang-format on

// w x h RGBA pixels at `rgba` (4 bytes each, rows packed) to the canvas at (x, y): putImageData
// clang-format off
EM_JS(void, WasmJs_Present, (const void* rgba, int w, int h, int x, int y), {
	var c = Module['canvas'];
	if(!c || w <= 0 || h <= 0)
		return;
	var ctx = Module.bgiCtx || (Module.bgiCtx = c.getContext('2d', { alpha: false }));
	var img = new ImageData(new Uint8ClampedArray(HEAPU8.buffer, rgba, w * h * 4), w, h); // a view of the heap, no copy
	ctx.putImageData(img, x, y);
});
// clang-format on

// the whole canvas black
// clang-format off
EM_JS(void, WasmJs_FillBlack, (void), {
	var c = Module['canvas'];
	if(!c)
		return;
	var ctx = Module.bgiCtx || (Module.bgiCtx = c.getContext('2d', { alpha: false }));
	ctx.fillStyle = '#000';
	ctx.fillRect(0, 0, c.width, c.height);
});
// clang-format on

// the window title is the document's (the browser tab)
// clang-format off
EM_JS(void, WasmJs_SetTitle, (const char* utf8), {
	document.title = UTF8ToString(utf8);
});
// clang-format on

// the pointer over the canvas: the default arrow or none (the engine's own cursor shapes are not drawn)
// clang-format off
EM_JS(void, WasmJs_SetCursor, (int visible), {
	var c = Module['canvas'];
	if(c)
		c.style.cursor = visible ? 'default' : 'none';
});
// clang-format on

// the screen size in CSS pixels; 1920 x 1080 when the browser does not say
// clang-format off
EM_JS(int, WasmJs_ScreenW, (void), { return (typeof screen !== 'undefined' && screen.width) ? screen.width : 1920; });
// clang-format on
// clang-format off
EM_JS(int, WasmJs_ScreenH, (void), { return (typeof screen !== 'undefined' && screen.height) ? screen.height : 1080; });
// clang-format on

/* the message box: `kind` 0 is window.alert (always 1), anything else
 * window.confirm (1 for OK, 0 for cancel); the caption becomes the first
 * line of the message, since the browser's dialogs have no title */
// clang-format off
EM_JS(int, WasmJs_Confirm, (const char* text, const char* caption, int kind), {
	var msg = UTF8ToString(caption) + '\n\n' + UTF8ToString(text);
	if(kind === 0)
	{
		window.alert(msg);
		return 1;
	}
	return window.confirm(msg) ? 1 : 0;
});
// clang-format on

// window.prompt: the text typed as UTF-8 into out (n bytes); 0 when cancelled
// clang-format off
EM_JS(int, WasmJs_Prompt, (const char* title, const char* initial, char* out, int n), {
	var r = window.prompt(UTF8ToString(title), UTF8ToString(initial));
	if(r === null)
		return 0;
	stringToUTF8(r, out, n);
	return 1;
});
// clang-format on

// navigator.clipboard.writeText, asynchronous and best effort (it needs a secure context and may be refused)
// clang-format off
EM_JS(void, WasmJs_ClipboardWrite, (const char* utf8), {
	var s = UTF8ToString(utf8);
	if(typeof navigator !== 'undefined' && navigator.clipboard && navigator.clipboard.writeText)
		navigator.clipboard.writeText(s).catch(function() {});
});
// clang-format on

/* The DOM handlers, installed once: every event becomes an
 * openbgi_push_event call with a WASM_EV_* type (the numbers below are
 * those of os_wasm.h); the coordinates are scaled from CSS pixels to the
 * canvas's own.  The canvas takes the focus so that it gets the keys; a
 * click or a key also raises Module.onUserGesture, which lets the audio
 * context start (audio.c).  Ctrl / Meta combinations stay with the browser,
 * Alt combinations become WASM_EV_SYSKEY, a key that types a character also
 * queues a WASM_EV_CHAR. */
// clang-format off
EM_JS(void, WasmJs_InstallInput, (void), {
	var c = Module['canvas'];
	if(!c || Module.bgiInputInstalled)
		return;
	Module.bgiInputInstalled = true;
	var push = Module['_openbgi_push_event'];
	var pos = function(e) { // CSS pixels of the canvas's box to canvas pixels
		var r = c.getBoundingClientRect();
		var x = (e.clientX - r.left) * (c.width / (r.width || 1));
		var y = (e.clientY - r.top) * (c.height / (r.height || 1));
		return [Math.floor(x), Math.floor(y)];
	};
	c.tabIndex = 0; // focusable
	c.addEventListener('contextmenu', function(e) { e.preventDefault(); }); // the right button is the game's
	c.addEventListener('mousemove', function(e) {
		var p = pos(e);
		push(3, p[0], p[1], 0, 0);
	});
	var button = function(e) { // DOM button (0 left, 1 middle, 2 right, 3 / 4 the X buttons) to 0 L 1 R 2 M 3 X1 4 X2
		var map = { 0: 0, 2: 1, 1: 2, 3: 3, 4: 4 };
		return e.button in map ? map[e.button] : -1;
	};
	c.addEventListener('mousedown', function(e) {
		c.focus();
		if(Module['onUserGesture'])
			Module['onUserGesture']();
		var b = button(e), p = pos(e);
		if(b >= 0)
			push(4, b, 1, p[0], p[1]);
		e.preventDefault();
	});
	c.addEventListener('mouseup', function(e) {
		var b = button(e), p = pos(e);
		if(b >= 0)
			push(4, b, 0, p[0], p[1]);
		e.preventDefault();
	});
	c.addEventListener('wheel', function(e) {
		if(e.deltaY !== 0)
			push(5, e.deltaY < 0 ? 120 : -120, 0, 0, 0); // one WHEEL_DELTA per event, whatever the browser's unit
		e.preventDefault();
	}, { passive: false });
	c.addEventListener('keydown', function(e) {
		if(Module['onUserGesture'])
			Module['onUserGesture']();
		if(e.ctrlKey || e.metaKey)
			return; // the browser's own shortcuts stay with the browser
		if(e.altKey)
			push(9, e.keyCode, 0, 0, 0);
		else
			push(1, e.keyCode, e.repeat ? 1 : 0, 0, 0);
		if(e.key && e.key.length === 1 && !e.altKey)
			push(8, e.key.codePointAt(0), 0, 0, 0);
		e.preventDefault();
	});
	c.addEventListener('keyup', function(e) {
		if(e.ctrlKey || e.metaKey)
			return;
		push(2, e.keyCode, 0, 0, 0);
		e.preventDefault();
	});
	c.addEventListener('focus', function() { push(6, 1, 0, 0, 0); });
	c.addEventListener('blur', function() { push(6, 0, 0, 0, 0); });
	window.addEventListener('beforeunload', function() { push(7, 0, 0, 0, 0); }); // the tab closes: a close request
});
// clang-format on

// ---- the event queue ------------------------------------------------------------------

// one queued input event: a WASM_EV_* type and its arguments as os_wasm.h lists them
typedef struct WasmEvent
{
	int type, a, b, c, d;
} WasmEvent_t;

#define EVENT_QUEUE 256                 // events held at most; the oldest is dropped when full
static WasmEvent_t gQueue[EVENT_QUEUE]; // a ring: gQueueCount events from gQueueHead
static int gQueueHead, gQueueCount;

// queue an event for the pump (the page's handlers reach this through openbgi_push_event)
void OsWasm_PushEvent(int type, int a, int b, int c, int d)
{
	WasmEvent_t* e;
	if(gQueueCount == EVENT_QUEUE)
	{ // full: the oldest event is dropped
		gQueueHead = (gQueueHead + 1) % EVENT_QUEUE;
		gQueueCount--;
	}
	e = &gQueue[(gQueueHead + gQueueCount) % EVENT_QUEUE];
	e->type = type;
	e->a = a;
	e->b = b;
	e->c = c;
	e->d = d;
	gQueueCount++;
}

// Module._openbgi_push_event: the page's handlers (WasmJs_InstallInput) call this export
EMSCRIPTEN_KEEPALIVE void openbgi_push_event(int type, int a, int b, int c, int d)
{
	OsWasm_PushEvent(type, a, b, c, d);
}

// the oldest queued event into *out; 0 when the queue is empty
static int PopEvent(WasmEvent_t* out)
{
	if(!gQueueCount)
		return 0;
	*out = gQueue[gQueueHead];
	gQueueHead = (gQueueHead + 1) % EVENT_QUEUE;
	gQueueCount--;
	return 1;
}

// ---- the window ------------------------------------------------------------------------

// bring the canvas in line with the window state: the mode's size in full screen, else the client size
static void ApplyCanvas(void)
{
	if(gFullscreen)
		WasmJs_CanvasSetup(gModeW > 0 ? gModeW : gClientW, gModeH > 0 ? gModeH : gClientH, gShown, 1);
	else
		WasmJs_CanvasSetup(gClientW, gClientH, gShown, 0);
}

/* the canvas becomes the main window: sized to the client size, hidden,
 * titled, with the input handlers installed; `created` is raised before
 * returning.  0 when the window exists already; the position is ignored
 * (the page lays the canvas out). */
int OS_WindowCreate(const OsWindowDesc_t* d)
{
	char title[0x400], utf[0x800];
	if(gAlive)
		return 0;
	gClientW = d->width;
	gClientH = d->height;
	gAlive = 1;
	gShown = 0;
	gFullscreen = 0;
	memset(gKeys, 0, sizeof gKeys);
	ApplyCanvas();
	OsCommon_MainTitle(title, sizeof title, d->title);
	OsWasm_SjisToUtf8(title, utf, sizeof utf);
	WasmJs_SetTitle(utf);
	WasmJs_InstallInput();
	WasmJs_SetCursor(1);
	if(gH.created)
		gH.created();
	return 1;
}

// hide the canvas and raise `destroyed`; the canvas itself stays in the page
void OS_WindowDestroy(void)
{
	if(!gAlive)
		return;
	gAlive = 0;
	gShown = 0;
	ApplyCanvas();
	if(gH.destroyed)
		gH.destroyed();
}

int OS_WindowAlive(void)
{
	return gAlive;
}

// show or hide the canvas; a show queues the activation and a paint, as the desktop would send them
void OS_WindowShow(int show)
{
	if(!gAlive)
		return;
	gShown = show != 0;
	ApplyCanvas();
	if(show)
	{ // the page is the foreground window: WM_ACTIVATE follows the show as on the desktop
		OsWasm_PushEvent(WASM_EV_ACTIVATE, 1, 0, 0, 0);
		OsWasm_PushEvent(WASM_EV_PAINT, 0, 0, 0, 0);
	}
}

// the document title, spelled by OsCommon_MainTitle (NULL clears the game's part)
void OS_WindowSetTitle(const char* t)
{
	char title[0x400], utf[0x800];
	if(!gAlive)
		return;
	OsCommon_MainTitle(title, sizeof title, t);
	OsWasm_SjisToUtf8(title, utf, sizeof utf);
	WasmJs_SetTitle(utf);
}

void OS_WindowMove(int x, int y)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y); // the page lays the canvas out
}

// the "outer rectangle" is the canvas at the origin: no frame, no position
void OS_WindowGetRect(int* x, int* y, int* w, int* h)
{
	*x = 0;
	*y = 0;
	*w = gFullscreen ? gModeW : gClientW;
	*h = gFullscreen ? gModeH : gClientH;
}

// a canvas is never minimised
int OS_WindowMinimized(void)
{
	return 0;
}

// the canvas has the focus (the focus / blur events)
int OS_WindowActive(void)
{
	return gActive;
}

void OS_WindowMinimize(void)
{
}

// `close_request` from the next pump
void OS_WindowClose(void)
{
	gCloseRequested = 1;
}

// the browser's screen in CSS pixels
void OS_ScreenSize(int* w, int* h)
{
	*w = WasmJs_ScreenW();
	*h = WasmJs_ScreenH();
}

// no frame: the engine places the client area where it likes, nothing moves
void OS_FrameMetrics(int* fw, int* fh)
{
	*fw = *fh = 0;
}

int OS_FrameTop(void)
{
	return 0;
}

void OS_ImeStatus(int open)
{
	BGI_UNUSED(open);
}

/* no mode is switched: the size is remembered and the canvas takes it in
 * full screen (the page's CSS scales it to the viewport), so every size and
 * depth "succeeds"; 0 only for an empty size */
int OS_DisplaySetMode(int fullscreen, int w, int h, int bpp)
{
	BGI_UNUSED(fullscreen);
	BGI_UNUSED(bpp);
	if(w <= 0 || h <= 0)
		return 0;
	gModeW = w;
	gModeH = h;
	return 1;
}

void OS_DisplayRestore(void)
{
}

// the canvas takes the mode's size and the "fullscreen" class, and starts black
void OS_WindowSetFullscreen(int modeW, int modeH)
{
	gModeW = modeW;
	gModeH = modeH;
	gFullscreen = 1;
	ApplyCanvas();
	WasmJs_FillBlack();
}

// back to the client size in the page's layout; the position is ignored
void OS_WindowSetWindowed(int x, int y, int clientW, int clientH)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	gFullscreen = 0;
	gClientW = clientW;
	gClientH = clientH;
	ApplyCanvas();
}

// the canvas shows the picture at its own size: no mapping to keep
void OS_WindowSetPicture(int w, int h)
{
	BGI_UNUSED(w);
	BGI_UNUSED(h);
}

// ---- presenting ----------------------------------------------------------------------------

static uint8_t* gConv;  // the RGBA rows handed to the canvas, grown to the largest blit so far
static size_t gConvCap; // its size in bytes

// the conversion buffer with room for `pixels` RGBA pixels; NULL when out of memory
static uint8_t* ConvBuffer(size_t pixels)
{
	if(pixels * 4 > gConvCap)
	{
		free(gConv);
		gConv = (uint8_t*)malloc(pixels * 4);
		gConvCap = gConv ? pixels * 4 : 0;
	}
	return gConv;
}

// one source pixel as 0x00RRGGBB (16-bit surfaces are RGB555, each 5-bit channel widened to 8)
static uint32_t SurfacePixel(const OsSurface_t* s, int x, int y)
{
	const uint8_t* row = (const uint8_t*)s->pixels + (ptrdiff_t)y * s->pitch;
	if(s->bpp == 16)
	{
		uint32_t v = ((const uint16_t*)row)[x];
		uint32_t r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, b = v & 0x1f;
		return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
	}
	return ((const uint32_t*)row)[x] & 0xffffff;
}

// write 0x00RRGGBB as the four bytes R, G, B, 255 of an RGBA pixel
static void PutRgb(uint8_t* o, uint32_t rgb)
{
	o[0] = (uint8_t)(rgb >> 16);
	o[1] = (uint8_t)(rgb >> 8);
	o[2] = (uint8_t)rgb;
	o[3] = 0xff;
}

/* The source rectangle (srcX, srcY, srcW x srcH of the surface) stretched to
 * the destination rectangle (dstX, dstY, dstW x dstH in canvas pixels),
 * clipped to `clip` ({l, t, r, b}, right / bottom exclusive; NULL for none)
 * and to the canvas, converted to RGBA and presented.  Three paths: a plain
 * copy when the sizes match, nearest-neighbour when they differ, and with
 * `smooth` a bilinear filter (the HALFTONE mode of OS_StretchToWindow's
 * clipped stretch).  Source pixels outside the surface read as black. */
static void BlitSurface(int dstX, int dstY, int dstW, int dstH, const OsSurface_t* s, int srcX, int srcY, int srcW,
	int srcH, int smooth, const int* clip)
{
	int cl = 0, ct = 0, cr, cb, w, h, x, y; // the output rectangle: the canvas, cut by `clip` and the destination
	int wx1 = dstX + dstW, wy1 = dstY + dstH;
	uint8_t* d;
	double fx, fy; // source pixels per destination pixel
	if(!gAlive || dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0 || !s->pixels)
		return;
	cr = gFullscreen ? gModeW : gClientW;
	cb = gFullscreen ? gModeH : gClientH;
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
	if(cl < dstX)
		cl = dstX;
	if(ct < dstY)
		ct = dstY;
	if(cr > wx1)
		cr = wx1;
	if(cb > wy1)
		cb = wy1;
	w = cr - cl;
	h = cb - ct;
	if(w <= 0 || h <= 0)
		return;
	fx = (double)srcW / dstW;
	fy = (double)srcH / dstH;
	d = ConvBuffer((size_t)w * h);
	if(!d)
		return;
	if(fx == 1.0 && fy == 1.0)
	{ // 1:1: a straight copy
		for(y = 0; y < h; y++)
		{
			int sy = srcY + (ct - dstY) + y;
			int sx0 = srcX + (cl - dstX);
			uint8_t* o = d + (size_t)y * w * 4;
			if(sy < 0 || sy >= s->height)
			{
				memset(o, 0, (size_t)w * 4);
				continue;
			}
			for(x = 0; x < w; x++)
			{
				int sx = sx0 + x;
				PutRgb(o + x * 4, (sx >= 0 && sx < s->width) ? SurfacePixel(s, sx, sy) : 0);
			}
		}
	}
	else if(!smooth)
	{ // nearest neighbour, the last source row / column repeated at the edge
		for(y = 0; y < h; y++)
		{
			int sy = srcY + (int)((ct - dstY + y) * fy);
			uint8_t* o = d + (size_t)y * w * 4;
			if(sy >= srcY + srcH)
				sy = srcY + srcH - 1;
			for(x = 0; x < w; x++)
			{
				int sx = srcX + (int)((cl - dstX + x) * fx);
				if(sx >= srcX + srcW)
					sx = srcX + srcW - 1;
				PutRgb(o + x * 4, (sx >= 0 && sx < s->width && sy >= 0 && sy < s->height) ? SurfacePixel(s, sx, sy) : 0);
			}
		}
	}
	else
	{ // bilinear: sample at pixel centres, weights as 8-bit fractions, clamped at the edges
		for(y = 0; y < h; y++)
		{
			double syf = srcY + ((ct - dstY + y) + 0.5) * fy - 0.5;
			int sy0 = (int)syf, sy1;
			int wy = (int)((syf - sy0) * 256);
			uint8_t* o = d + (size_t)y * w * 4;
			if(syf < 0)
				sy0 = 0, wy = 0;
			sy1 = sy0 + 1 < srcY + srcH ? sy0 + 1 : sy0;
			if(sy0 >= s->height)
				sy0 = s->height - 1;
			if(sy1 >= s->height)
				sy1 = s->height - 1;
			for(x = 0; x < w; x++)
			{
				double sxf = srcX + ((cl - dstX + x) + 0.5) * fx - 0.5;
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
				p00 = SurfacePixel(s, sx0, sy0);
				p01 = SurfacePixel(s, sx1, sy0);
				p10 = SurfacePixel(s, sx0, sy1);
				p11 = SurfacePixel(s, sx1, sy1);
#define CH(p, sh)     (((p) >> (sh)) & 0xff)             // one channel of 0x00RRGGBB
#define LERP(a, b, t) ((a) + ((((b) - (a)) * (t)) >> 8)) // a + (b - a) * t / 256
				r = LERP(LERP(CH(p00, 16), CH(p01, 16), wx), LERP(CH(p10, 16), CH(p11, 16), wx), wy);
				g = LERP(LERP(CH(p00, 8), CH(p01, 8), wx), LERP(CH(p10, 8), CH(p11, 8), wx), wy);
				b = LERP(LERP(CH(p00, 0), CH(p01, 0), wx), LERP(CH(p10, 0), CH(p11, 0), wx), wy);
#undef CH
#undef LERP
				PutRgb(o + x * 4, (r << 16) | (g << 8) | b);
			}
		}
	}
	WasmJs_Present(d, w, h, cl, ct);
}

// the one context (the address of gDc); NULL without a window
OsDc_t* OS_WindowGetDc(void)
{
	return gAlive ? &gDc : NULL;
}

void OS_WindowReleaseDc(OsDc_t* dc)
{
	BGI_UNUSED(dc);
}

// BitBlt: w x h pixels of the surface from (srcX, srcY) to (dstX, dstY) of the canvas, unscaled
void OS_BlitToWindow(OsDc_t* dc, int dstX, int dstY, int w, int h, const OsSurface_t* src, int srcX, int srcY)
{
	if(dc && dc != (OsDc_t*)&gDc)
		return; // a child window's context: none exist
	BlitSurface(dstX, dstY, w, h, src, srcX, srcY, w, h, 0, NULL);
}

/* StretchDIBits of the whole surface; with `clip` the output is clipped and
 * filtered (HALFTONE), without it nearest neighbour */
void OS_StretchToWindow(OsDc_t* dc, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* src, const int* clip)
{
	if(dc && dc != (OsDc_t*)&gDc)
		return;
	BlitSurface(dstX, dstY, dstW, dstH, src, 0, 0, src->width, src->height, clip != NULL, clip);
}

void OS_WindowFillBlack(void)
{
	if(gAlive)
		WasmJs_FillBlack();
}

// ---- cursor and keys ----------------------------------------------------------------------

// the pointer is shown while the ShowCursor counter is not negative and a shape is selected
static void ApplyCursor(void)
{
	WasmJs_SetCursor(gCursorCount >= 0 && gCursorShape != OS_CURSOR_NONE);
}

// "80 67": every shape but OS_CURSOR_NONE is the browser's default arrow
void OS_CursorSetShape(int shape)
{
	gCursorShape = shape;
	ApplyCursor();
}

// ShowCursor counter semantics: visible while the counter is >= 0
void OS_CursorShow(int show)
{
	gCursorCount += show ? 1 : -1;
	ApplyCursor();
}

// the position of the last mouse event (canvas pixels, which are the "screen" here)
void OS_CursorGetPos(int* x, int* y)
{
	*x = gMouseX;
	*y = gMouseY;
}

void OS_CursorSetPos(int x, int y)
{
	gMouseX = x; // a page cannot move the pointer; the engine's idea of it follows
	gMouseY = y;
}

// screen and client coordinates are the same: the canvas is the screen here
void OS_ScreenToClient(int* x, int* y)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y); // the canvas is the screen here
}

void OS_ClientToScreen(int* x, int* y)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
}

int OS_MouseButtonsSwapped(void)
{
	return 0;
}

// 1 while the virtual key is held, from the key and button events delivered so far
int OS_KeyState(int vk)
{
	if(vk < 0 || vk > 255)
		return 0;
	return gKeys[vk];
}

// GetKeyboardState: bit 7 set for every key held
void OS_KeyboardState(uint8_t out[256])
{
	int i;
	for(i = 0; i < 256; i++)
		out[i] = gKeys[i] ? 0x80 : 0;
}

// record a key's state as its event is delivered (virtual key 0 is nothing)
static void SetKey(int vk, int down)
{
	if(vk > 0 && vk < 256)
		gKeys[vk] = (uint8_t)(down ? 1 : 0);
}

// the virtual keys of the buttons 0 L 1 R 2 M 3 X1 4 X2 (VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2)
static const int kButtonVk[5] = {1, 2, 4, 5, 6};

// ---- child windows and the text entry: not on a page -----------------------------------------

// no child windows: OS_ChildCreate fails (NULL) and the engine reports the window as not created
OsChild_t* OS_ChildCreate(int index, const char* title, int x, int y, int outerW, int outerH)
{
	BGI_UNUSED(index);
	BGI_UNUSED(title);
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(outerW);
	BGI_UNUSED(outerH);
	return NULL;
}

void OS_ChildClose(OsChild_t* c)
{
	BGI_UNUSED(c);
}

void OS_ChildShow(OsChild_t* c, int show)
{
	BGI_UNUSED(c);
	BGI_UNUSED(show);
}

void OS_ChildSetTitle(OsChild_t* c, const char* title)
{
	BGI_UNUSED(c);
	BGI_UNUSED(title);
}

void OS_ChildMove(OsChild_t* c, int x, int y, int outerW, int outerH)
{
	BGI_UNUSED(c);
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(outerW);
	BGI_UNUSED(outerH);
}

void OS_ChildGetPos(OsChild_t* c, int* x, int* y)
{
	BGI_UNUSED(c);
	*x = *y = 0;
}

OsDc_t* OS_ChildGetDc(OsChild_t* c)
{
	BGI_UNUSED(c);
	return NULL;
}

void OS_ChildReleaseDc(OsChild_t* c, OsDc_t* dc)
{
	BGI_UNUSED(c);
	BGI_UNUSED(dc);
}

/* No text entry either: the control's functions do nothing, except that the
 * text set is kept so that OS_EditGetText reads back what the script put in
 * (the field is never shown and nothing can be typed into it). */
static char gEditText[0x100]; // the text of OS_EditSetText

void OS_EditShow(int show)
{
	BGI_UNUSED(show);
}

void OS_EditMove(int x, int y, int w, int h)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(w);
	BGI_UNUSED(h);
}

void OS_EditSetFont(const char* face, int size)
{
	BGI_UNUSED(face);
	BGI_UNUSED(size);
}

void OS_EditLimitText(int maxChars)
{
	BGI_UNUSED(maxChars);
}

void OS_EditSetText(const char* t)
{
	snprintf(gEditText, sizeof gEditText, "%s", t ? t : "");
}

void OS_EditSelectAll(void)
{
}

void OS_EditFocus(int toEdit)
{
	BGI_UNUSED(toEdit);
}

int OS_EditGetText(char* buf, size_t n)
{
	snprintf(buf, n, "%s", gEditText);
	return (int)strlen(buf);
}

void OS_EditSetColour(uint32_t rgb)
{
	BGI_UNUSED(rgb);
}

void OS_EditRefresh(void)
{
}

void OS_EditDestroy(void)
{
}

void OS_EditRejectAscii(int reject)
{
	BGI_UNUSED(reject);
}

int OS_EditImeOpen(void)
{
	return 0;
}

// ---- dialogs ----------------------------------------------------------------------------

/* MessageBox as the browser's alert (OS_MB_OK) or confirm: the two buttons
 * of confirm stand for the pair the flags ask for - yes / no, retry /
 * cancel, else OK / cancel - and the icon and default-button flags are
 * ignored.  The dialog blocks the page; no event reaches the engine while
 * it is up. */
int OS_MessageBox(const char* text, const char* caption, uint32_t flags)
{
	char t[0x1000], c[0x400];
	uint32_t type = flags & 0xf;
	int yes;
	OsWasm_SjisToUtf8(text ? text : "", t, sizeof t);
	OsWasm_SjisToUtf8(caption ? caption : "", c, sizeof c);
	if(type == OS_MB_OK)
	{
		WasmJs_Confirm(t, c, 0);
		return OS_IDOK;
	}
	yes = WasmJs_Confirm(t, c, 1);
	switch(type)
	{
		case OS_MB_YESNO: return yes ? OS_IDYES : OS_IDNO;
		case OS_MB_RETRYCANCEL: return yes ? OS_IDRETRY : OS_IDCANCEL;
		default: return yes ? OS_IDOK : OS_IDCANCEL;
	}
}

/* the one-field dialog as window.prompt: the answer comes back converted to
 * the engine's encoding into `out` (0x100 bytes), cut to `maxLen` bytes
 * when that is positive - which may split a two-byte character, as the
 * EDIT control's limit cannot; 0 when cancelled */
int OS_DialogInput1(const char* title, const char* initial, int maxLen, char* out)
{
	char t[0x400], i[0x400], r[0x400], sj[0x100];
	OsWasm_SjisToUtf8(title ? title : "", t, sizeof t);
	OsWasm_SjisToUtf8(initial ? initial : "", i, sizeof i);
	if(!WasmJs_Prompt(t, i, r, sizeof r))
		return 0;
	OS_Utf8ToSjis(r, sj, sizeof sj);
	if(maxLen > 0 && (int)strlen(sj) > maxLen)
		sj[maxLen] = 0;
	snprintf(out, 0x100, "%s", sj);
	return 1;
}

/* the two-field dialog as two prompts in a row, each titled "<title>\n<label>";
 * cancelling the first skips the second (0), the result is the second's */
int OS_DialogInput2(const char* title, const char* label1, const char* initial1, int maxLen1, char* out1,
	const char* label2, const char* initial2, int maxLen2, char* out2)
{
	char t[0x400], l[0x400];
	OsWasm_SjisToUtf8(title ? title : "", t, sizeof t);
	OsWasm_SjisToUtf8(label1 ? label1 : "", l, sizeof l);
	snprintf(t + strlen(t), sizeof t - strlen(t), "\n%s", l);
	if(!OS_DialogInput1(t, initial1, maxLen1, out1))
		return 0;
	OsWasm_SjisToUtf8(title ? title : "", t, sizeof t);
	OsWasm_SjisToUtf8(label2 ? label2 : "", l, sizeof l);
	snprintf(t + strlen(t), sizeof t - strlen(t), "\n%s", l);
	return OS_DialogInput1(t, initial2, maxLen2, out2);
}

int OS_DialogProfile(char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month, int32_t* day)
{
	BGI_UNUSED(surname);
	BGI_UNUSED(givenName);
	BGI_UNUSED(nickname);
	BGI_UNUSED(pronoun);
	BGI_UNUSED(month);
	BGI_UNUSED(day);
	return 0; // the profile dialog is beyond a prompt; "cancelled"
}

// no file system to browse on a page: "no file chosen"
int OS_FileDialog(int save, char* path, size_t n, const char* filter, const char* defExt, const char* initialDir, const char* title)
{
	BGI_UNUSED(save);
	BGI_UNUSED(path);
	BGI_UNUSED(n);
	BGI_UNUSED(filter);
	BGI_UNUSED(defExt);
	BGI_UNUSED(initialDir);
	BGI_UNUSED(title);
	return 0;
}

// no game chooser on a page: the game is what the server serves
int OS_LauncherDialog(OsLauncher_t* l)
{
	BGI_UNUSED(l);
	return -1;
}

int OS_BrowseFolder(char* path, size_t n, const char* title, const char* initial)
{
	BGI_UNUSED(path);
	BGI_UNUSED(n);
	BGI_UNUSED(title);
	BGI_UNUSED(initial);
	return 0;
}

// "7E": the text as UTF-8 to the browser's clipboard; 1 always, since the write is asynchronous and cannot be checked
int OS_ClipboardSetText(const char* sjis)
{
	char utf[0x2000];
	OsWasm_SjisToUtf8(sjis, utf, sizeof utf);
	WasmJs_ClipboardWrite(utf);
	return 1;
}

// ---- the pump --------------------------------------------------------------------------------

void OS_SetEventHandlers(const OsEventHandlers_t* h)
{
	gH = *h;
}

// `quit` from the next pump
void OS_PostQuit(void)
{
	gQuitPosted = 1;
}

// no file drops on the canvas: `drop_file` is never raised
void OS_DragAccept(int accept)
{
	BGI_UNUSED(accept);
}

/* Hand one queued event to the engine's callback, keeping the key and
 * pointer state up to date on the way.  A press of the left or right button
 * within 400 ms and 4 pixels of the previous press of the same button
 * raises `double_click` first, as the desktop sends WM_?BUTTONDBLCLK before
 * the press (the browser's dblclick event is not used); a focus loss
 * releases every key, since the key-up events go to whoever has the focus
 * then. */
static void Deliver(const WasmEvent_t* e)
{
	switch(e->type)
	{
		case WASM_EV_KEY_DOWN:
			SetKey(e->a, 1);
			if(gH.key_down)
				gH.key_down(e->a, e->b);
			break;
		case WASM_EV_KEY_UP:
			SetKey(e->a, 0);
			if(gH.key_up)
				gH.key_up(e->a);
			break;
		case WASM_EV_SYSKEY:
			SetKey(e->a, 1); // released by the ordinary key-up
			if(gH.syskey)
				gH.syskey(e->a);
			break;
		case WASM_EV_MOUSE_MOVE:
			gMouseX = e->a;
			gMouseY = e->b;
			if(gH.mouse_move)
				gH.mouse_move(e->a, e->b);
			break;
		case WASM_EV_BUTTON:
			gMouseX = e->c;
			gMouseY = e->d;
			if(e->a >= 0 && e->a < 5)
				SetKey(kButtonVk[e->a], e->b);
			if(e->b)
			{ // WM_?BUTTONDBLCLK before the ordinary press
				uint32_t now = OS_TicksMs();
				if(e->a <= 1 && gLastClickBtn == e->a && now - gLastClickTime < 400 && BGI_ABS(e->c - gLastClickX) < 4 &&
					BGI_ABS(e->d - gLastClickY) < 4)
				{
					if(gH.double_click)
						gH.double_click(e->a);
					gLastClickBtn = -1; // a third press starts over, it is not another double click
				}
				else
				{
					gLastClickBtn = e->a;
					gLastClickTime = now;
					gLastClickX = e->c;
					gLastClickY = e->d;
				}
			}
			if(gH.mouse_button)
				gH.mouse_button(e->a, e->b, e->c, e->d);
			break;
		case WASM_EV_WHEEL:
			if(gH.mouse_wheel)
				gH.mouse_wheel(e->a);
			break;
		case WASM_EV_ACTIVATE:
			gActive = e->a;
			if(!e->a)
				memset(gKeys, 0, sizeof gKeys); // nothing is held across a focus loss
			if(gH.activate)
				gH.activate(e->a);
			break;
		case WASM_EV_CLOSE:
			if(gH.close_request)
				gH.close_request();
			break;
		case WASM_EV_PAINT:
			if(gH.paint)
				gH.paint();
			break;
		case WASM_EV_CHAR:
		default:
			break; // typed text would go to the text entry, which does not exist here
	}
}

/* The browser only paints the canvas and runs the websocket when the
 * engine has returned to it; Asyncify's suspension is that return.  A
 * timed sleep goes through emscripten_sleep (setTimeout, which browsers
 * clamp to a few milliseconds when it is nested); the idle step of every
 * pass (OS_IdleSleep, the original's half millisecond) instead posts a
 * message to itself and resumes when it arrives, which lets the browser
 * run its pending tasks and paint without the clamp.  A pass that
 * presented a frame skips the idle step, so the pump also yields when
 * nothing has for a while. */
// the fast yield: a MessageChannel round trip (made on first use), awaited through Asyncify
// clang-format off
EM_ASYNC_JS(void, WasmJs_YieldFast, (void), {
	if(!Module.bgiYieldChannel)
		Module.bgiYieldChannel = new MessageChannel();
	await new Promise(function(resolve) {
		Module.bgiYieldChannel.port1.onmessage = function() { resolve(); };
		Module.bgiYieldChannel.port2.postMessage(0);
	});
});
// clang-format on

/* return to the browser: the audio is pumped first (audio.c), then the
 * engine suspends for `ms` milliseconds (emscripten_sleep) or, for 0, for
 * one message round trip; the input handlers and the websocket run in the
 * meantime */
void OsWasm_Yield(int ms)
{
	OsWasm_AudioPump();
	if(ms > 0)
		emscripten_sleep((unsigned)ms);
	else
		WasmJs_YieldFast();
	gLastYield = OS_TicksMs();
}

/* one message: a pending quit or close request first, then the oldest
 * queued input event; 1 when something was delivered.  When the engine has
 * not returned to the browser for 20 ms (a pass that presented a frame
 * skips the idle step) the pump yields first, so that input keeps arriving
 * and the canvas keeps painting. */
int OS_PumpMessages(void)
{
	WasmEvent_t e;
	if(gQuitPosted)
	{
		gQuitPosted = 0;
		if(gH.quit)
			gH.quit();
		return 1;
	}
	if(gCloseRequested)
	{
		gCloseRequested = 0;
		if(gH.close_request)
			gH.close_request();
		return 1;
	}
	if(OS_TicksMs() - gLastYield > 20)
		OsWasm_Yield(0);
	if(!PopEvent(&e))
		return 0;
	Deliver(&e);
	return 1;
}
