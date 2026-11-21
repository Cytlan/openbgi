/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.c - the WebAssembly back end's window: a canvas, its input, the
 *            cursor, the dialogs, and the stubs of what a page has not got
 *
 * The main window is the page's <canvas id="canvas">, created hidden and
 * shown by OS_WindowShow like the original's.  The frames' pictures are
 * copied into it rectangle by rectangle (OS_BlitToWindow converts the rows
 * to RGBA and putImageData them); the canvas keeps the engine's client
 * size - the display mode's size in full screen - and the page's CSS
 * scales it to the viewport, so nothing here maps coordinates.
 *
 * Input arrives through the DOM handlers installed by WasmJs_InstallInput:
 * they call openbgi_push_event (host.c), which queues the event here, and
 * OS_PumpMessages hands one event per call on like the message pump of
 * the original: keys, the pointer, buttons, the wheel, the focus, the
 * repaints and the page's close to the host's handlers
 * (OS_SetHostHandlers).  Key codes are the browser's keyCode, which is the Windows
 * virtual key for everything the games use.
 *
 * The child windows ("B0 1x"), the text entry ("B0 2x"), the message
 * boxes and the scripts' dialogs are the engine's, drawn in the frame;
 * with --dialogs=host the boxes and dialogs are the browser's confirm /
 * alert / prompt.
 *
 * Also here, because it is where the engine returns to the browser:
 * OsWasm_Yield, the suspension point that every sleep and the message pump
 * go through (os_wasm.h).  The interface is the window, display, input,
 * child window, text entry, dialog and clipboard sections of os.h; the
 * debugger's window is not part of this build.  The WasmJs_* functions are
 * the page side (EM_JS); tools/wasm/fakebrowser.c stands in for them on a
 * host.
 */
#include "host/os.h"
#include "host/os_common.h"
#include "host/os_wasm.h"
#include "bgi/sjis.h"

#include <emscripten/emscripten.h>

static OsHostHandlers_t gIn;                             // the host's handlers (OS_SetHostHandlers)
static int gAlive, gShown, gFullscreen;                  // the window exists / is visible / covers a mode
static int gClientW = 800, gClientH = 600;               // the windowed client size in pixels
static int gModeW, gModeH;                               // the display mode of full screen, pixels (0 = none set yet)
static int gCursorShape = OS_CURSOR_ARROW, gCursorCount; // OS_CursorSetShape's shape; the ShowCursor counter (visible at >= 0)
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
})
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
})
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
})
// clang-format on

// the window title is the document's (the browser tab)
// clang-format off
EM_JS(void, WasmJs_SetTitle, (const char* utf8), {
	document.title = UTF8ToString(utf8);
})
// clang-format on

// the pointer over the canvas: the default arrow or none (the engine's own cursor shapes are not drawn)
// clang-format off
EM_JS(void, WasmJs_SetCursor, (int visible), {
	var c = Module['canvas'];
	if(c)
		c.style.cursor = visible ? 'default' : 'none';
})
// clang-format on

// the screen size in CSS pixels; 1920 x 1080 when the browser does not say
// clang-format off
EM_JS(int, WasmJs_ScreenW, (void), { return (typeof screen !== 'undefined' && screen.width) ? screen.width : 1920; })
// clang-format on
// clang-format off
EM_JS(int, WasmJs_ScreenH, (void), { return (typeof screen !== 'undefined' && screen.height) ? screen.height : 1080; })
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
})
// clang-format on

// window.prompt: the text typed as UTF-8 into out (n bytes); 0 when cancelled
// clang-format off
EM_JS(int, WasmJs_Prompt, (const char* title, const char* initial, char* out, int n), {
	var r = window.prompt(UTF8ToString(title), UTF8ToString(initial));
	if(r === null)
		return 0;
	stringToUTF8(r, out, n);
	return 1;
})
// clang-format on

// navigator.clipboard.writeText, asynchronous and best effort (it needs a secure context and may be refused)
// clang-format off
EM_JS(void, WasmJs_ClipboardWrite, (const char* utf8), {
	var s = UTF8ToString(utf8);
	if(typeof navigator !== 'undefined' && navigator.clipboard && navigator.clipboard.writeText)
		navigator.clipboard.writeText(s).catch(function() {});
})
// clang-format on

/* The DOM handlers, installed once: every event becomes an
 * openbgi_push_event call with a WASM_EV_* type (the numbers below are
 * those of os_wasm.h); the coordinates are scaled from CSS pixels to the
 * canvas's own.  The canvas takes the focus so that it gets the keys; a
 * click or a key also raises Module.onUserGesture, which lets the audio
 * context start (audio.c).  Ctrl / Meta combinations stay with the browser,
 * Alt combinations become WASM_EV_SYSKEY, a key that types a character also
 * queues a WASM_EV_CHAR.  A finger on the canvas becomes WASM_EV_TOUCH
 * events, one for each contact that changed, and the mouse events the
 * browser would make of a touch are not wanted.
 *
 * While the engine takes text (WasmJs_TextInput), the keys go to a text
 * field of the page instead (Module.bgiTextField), placed over the engine's
 * field, because the browser's input method only composes into one.  It
 * is invisible but for the text being composed; what it is given -
 * typed, committed by the input method or pasted - becomes WASM_EV_CHAR
 * events and is cleared at once, and its keys queue the canvas's key
 * events, except those the input method processes.  The canvas and the
 * field pass the focus between them without a WASM_EV_ACTIVATE. */
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
	var f = document.createElement('input'); // the text field (see above)
	f.type = 'text';
	f.tabIndex = -1;
	f.autocomplete = 'off';
	f.spellcheck = false;
	f.setAttribute('autocapitalize', 'off');
	f.setAttribute('aria-hidden', 'true');
	f.style.cssText = 'position:fixed;left:0;top:0;width:1px;height:1px;opacity:0;pointer-events:none;'
		+ 'margin:0;border:0;padding:0 2px;box-sizing:border-box;outline:none;background:#fff;color:#000;z-index:2147483647';
	document.body.appendChild(f);
	Module.bgiTextField = f;
	Module.bgiTextOn = false;
	var ours = function(el) { return el === c || el === f; };
	var chars = function(s) { // the code points of s as WASM_EV_CHAR events
		for(var i = 0; i < s.length; ) {
			var cp = s.codePointAt(i);
			push(8, cp, 0, 0, 0);
			i += cp > 0xffff ? 2 : 1;
		}
	};
	var composing = false;
	var take = function() { // the field's text to the engine; the field stays empty
		var s = f.value;
		f.value = "";
		chars(s);
	};
	f.addEventListener('compositionstart', function() {
		composing = true;
		Module.bgiTextPlace();
		f.style.opacity = '1';
	});
	f.addEventListener('compositionend', function() {
		composing = false;
		f.style.opacity = '0';
		take();
	});
	f.addEventListener('input', function() {
		if(!composing)
			take();
	});
	f.addEventListener('beforeinput', function(e) { // a virtual keyboard's BackSpace comes as an edit of the empty field
		if(!composing && e.inputType === 'deleteContentBackward' && f.value === "") {
			push(1, 8, 0, 0, 0);
			push(2, 8, 0, 0, 0);
		}
	});
	f.addEventListener('keydown', function(e) {
		if(Module['onUserGesture'])
			Module['onUserGesture']();
		if(e.isComposing || e.keyCode === 229 || composing)
			return; // the input method's
		if(e.ctrlKey || e.metaKey)
			return; // the browser's shortcuts, pasting among them
		if(e.altKey)
			push(9, e.keyCode, 0, 0, 0);
		else
			push(1, e.keyCode, e.repeat ? 1 : 0, 0, 0);
		if(e.altKey || !(e.key && (e.key.length === 1 || e.key === 'Dead' || e.key === 'Unidentified'
			|| (e.key.length === 2 && e.key.codePointAt(0) > 0xffff))))
			e.preventDefault(); // a key that types nothing does nothing else either (Tab keeps the focus)
	});
	f.addEventListener('keyup', function(e) {
		if(e.ctrlKey || e.metaKey || e.isComposing)
			return;
		push(2, e.keyCode, 0, 0, 0);
		e.preventDefault();
	});
	f.addEventListener('focus', function(e) { if(!ours(e.relatedTarget)) push(6, 1, 0, 0, 0); });
	f.addEventListener('blur', function(e) {
		if(composing) { // still composing (browsers commit first, as a rule): the text goes with the focus
			composing = false;
			f.style.opacity = '0';
			f.value = "";
		}
		if(!ours(e.relatedTarget))
			push(6, 0, 0, 0, 0);
	});
	Module.bgiTextPlace = function() { // the field over the engine's field (canvas pixels in Module.bgiTextRect)
		var t = Module.bgiTextRect || [0, 0, 1, 1];
		var r = c.getBoundingClientRect(), k = r.width / (c.width || 1);
		f.style.left = (r.left + t[0] * k) + 'px';
		f.style.top = (r.top + t[1] * k) + 'px';
		f.style.width = Math.max(1, t[2] * k) + 'px';
		f.style.height = Math.max(1, t[3] * k) + 'px';
		f.style.fontSize = Math.max(8, Math.round(t[3] * k * 0.75)) + 'px';
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
		(Module.bgiTextOn ? f : c).focus({ preventScroll: true });
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
	var touch = function(phase) { // 0 down, 1 moved, 2 up, 3 cancelled
		return function(e) {
			if(phase === 0)
				(Module.bgiTextOn ? f : c).focus({ preventScroll: true });
			if((phase === 0 || phase === 2) && Module['onUserGesture'])
				Module['onUserGesture']();
			for(var i = 0; i < e.changedTouches.length; i++) {
				var t = e.changedTouches[i], p = pos(t);
				push(11, t.identifier | 0, phase, p[0], p[1]);
			}
			e.preventDefault();
		};
	};
	c.addEventListener('touchstart', touch(0), { passive: false });
	c.addEventListener('touchmove', touch(1), { passive: false });
	c.addEventListener('touchend', touch(2), { passive: false });
	c.addEventListener('touchcancel', touch(3), { passive: false });
	c.addEventListener('keydown', function(e) {
		if(Module['onUserGesture'])
			Module['onUserGesture']();
		if(e.ctrlKey || e.metaKey)
			return; // the browser's own shortcuts stay with the browser
		if(e.altKey)
			push(9, e.keyCode, 0, 0, 0);
		else
			push(1, e.keyCode, e.repeat ? 1 : 0, 0, 0);
		if(e.key && !e.altKey && (e.key.length === 1 || (e.key.length === 2 && e.key.codePointAt(0) > 0xffff))) // one character
			push(8, e.key.codePointAt(0), 0, 0, 0);
		e.preventDefault();
	});
	c.addEventListener('keyup', function(e) {
		if(e.ctrlKey || e.metaKey)
			return;
		push(2, e.keyCode, 0, 0, 0);
		e.preventDefault();
	});
	c.addEventListener('focus', function(e) {
		if(!ours(e.relatedTarget))
			push(6, 1, 0, 0, 0);
		if(Module.bgiTextOn)
			f.focus({ preventScroll: true });
	});
	c.addEventListener('blur', function(e) { if(!ours(e.relatedTarget)) push(6, 0, 0, 0, 0); });
	window.addEventListener('beforeunload', function() { push(7, 0, 0, 0, 0); }); // the tab closes: a close request
})
// clang-format on

/* The engine takes text in the canvas rectangle (x, y, w, h), or no more
 * (on 0): the text field of WasmJs_InstallInput moves there and takes the
 * focus from the canvas, or gives it back.  The focus is only moved
 * between the two: while the page has it elsewhere, the next press on the
 * canvas gives it to the field. */
// clang-format off
EM_JS(void, WasmJs_TextInput, (int on, int x, int y, int w, int h), {
	var f = Module.bgiTextField, c = Module['canvas'];
	if(!f)
		return;
	Module.bgiTextOn = !!on;
	Module.bgiTextRect = [x, y, w, h];
	Module.bgiTextPlace();
	if(on && document.activeElement === c)
		f.focus({ preventScroll: true });
	else if(!on && document.activeElement === f)
		c.focus({ preventScroll: true });
})
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
	ApplyCanvas();
	OsCommon_MainTitle(title, sizeof title, d->title);
	OsCommon_SjisToUtf8(title, utf, sizeof utf);
	WasmJs_SetTitle(utf);
	WasmJs_InstallInput();
	WasmJs_SetCursor(1);
	return 1;
}

// hide the canvas; the canvas itself stays in the page
void OS_WindowDestroy(void)
{
	if(!gAlive)
		return;
	gAlive = 0;
	gShown = 0;
	ApplyCanvas();
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
	OsCommon_SjisToUtf8(title, utf, sizeof utf);
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

void OS_WindowMinimize(void)
{
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

// the page's text field takes the keys over (on, at the rectangle) or gives them back to the canvas
void OS_TextInput(int on, int x, int y, int w, int h)
{
	WasmJs_TextInput(on, x, y, w, h);
}

// no dialogs of the page's own: the engine draws them in the frame (the page's confirm and prompt only with --dialogs=host)
uint32_t OS_HostCaps(void)
{
	return OPENBGI_CAP_POINTER | OPENBGI_CAP_KEYBOARD;
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

// the canvas is the page's to size: no sizing border
void OS_WindowSetResizable(int on)
{
	BGI_UNUSED(on);
}

// the page scales its canvas, by the browser's rule
void OS_WindowSetScaling(int rule)
{
	BGI_UNUSED(rule);
}

int OS_WindowScales(void)
{
	return 0;
}

// no filters beyond the three rules, and every blit is shown as it is made
const char* OS_WindowFilterName(int n)
{
	BGI_UNUSED(n);
	return NULL;
}

void OS_WindowPresent(void)
{
}

// no part in the fitting of the window to its screen: the canvas's size on the page is the page's matter
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

void OS_WindowSetAspect(int w, int h)
{
	BGI_UNUSED(w);
	BGI_UNUSED(h);
}

// the page's icon is the page's own matter
void OS_WindowSetIcon(const OpenBGI_Icon_t* icons, int count)
{
	BGI_UNUSED(icons);
	BGI_UNUSED(count);
}

int OS_WindowHasIcons(void)
{
	return 0;
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

/* w x h pixels of the surface from (srcX, srcY) to (dstX, dstY) of the
 * canvas, clipped to it, converted to RGBA and presented.  Source pixels
 * outside the surface read as black. */
static void BlitSurface(int dstX, int dstY, int w, int h, const OsSurface_t* s, int srcX, int srcY)
{
	int cl = dstX, ct = dstY, cr = dstX + w, cb = dstY + h, x, y; // the output rectangle: the destination, cut by the canvas
	int cw, ch;
	uint8_t* d;
	if(!gAlive || w <= 0 || h <= 0 || !s->pixels)
		return;
	cw = gFullscreen ? gModeW : gClientW;
	ch = gFullscreen ? gModeH : gClientH;
	if(cl < 0)
		cl = 0;
	if(ct < 0)
		ct = 0;
	if(cr > cw)
		cr = cw;
	if(cb > ch)
		cb = ch;
	if(cr <= cl || cb <= ct)
		return;
	d = ConvBuffer((size_t)(cr - cl) * (size_t)(cb - ct));
	if(!d)
		return;
	for(y = 0; y < cb - ct; y++)
	{
		int sy = srcY + (ct - dstY) + y;
		int sx0 = srcX + (cl - dstX);
		uint8_t* o = d + (size_t)y * (size_t)(cr - cl) * 4;
		if(sy < 0 || sy >= s->height)
		{
			memset(o, 0, (size_t)(cr - cl) * 4);
			continue;
		}
		for(x = 0; x < cr - cl; x++)
		{
			int sx = sx0 + x;
			PutRgb(o + x * 4, (sx >= 0 && sx < s->width) ? SurfacePixel(s, sx, sy) : 0);
		}
	}
	WasmJs_Present(d, cr - cl, cb - ct, cl, ct);
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
	BGI_UNUSED(dc); // (the one context)
	BlitSurface(dstX, dstY, w, h, src, srcX, srcY);
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


// ---- child windows and the text entry: the engine's -------------------------------------

// no windows of the page's own beside the canvas: the engine draws the child windows in the frame
const OpenBGI_ChildWindows_t* OS_ChildWindows(void)
{
	return NULL;
}

// no edit control of the page's own: the engine draws the text entry in the frame
const OpenBGI_TextEntry_t* OS_TextEntry(void)
{
	return NULL;
}

// ---- dialogs ----------------------------------------------------------------------------

/* MessageBox as the browser's alert (OPENBGI_MB_OK) or confirm: the two buttons
 * of confirm stand for the pair the flags ask for - yes / no, retry /
 * cancel, else OK / cancel - and the icon and default-button flags are
 * ignored.  The dialog blocks the page; no event reaches the engine while
 * it is up. */
/* The boxes are the page's confirm and prompt, which stop the page's
 * script until they are answered: nothing can tick meanwhile (what the
 * output had queued, about 200 ms, plays on). */
void OS_SetModalTick(void (*tick)(void* user), void* user)
{
	BGI_UNUSED(tick);
	BGI_UNUSED(user);
}

int OS_MessageBox(const char* text, const char* caption, uint32_t flags)
{
	char t[0x1000], c[0x400];
	uint32_t type = flags & 0xf;
	int yes;
	OsCommon_SjisToUtf8(text ? text : "", t, sizeof t);
	OsCommon_SjisToUtf8(caption ? caption : "", c, sizeof c);
	if(type == OPENBGI_MB_OK)
	{
		WasmJs_Confirm(t, c, 0);
		return OPENBGI_IDOK;
	}
	yes = WasmJs_Confirm(t, c, 1);
	switch(type)
	{
		case OPENBGI_MB_YESNO: return yes ? OPENBGI_IDYES : OPENBGI_IDNO;
		case OPENBGI_MB_RETRYCANCEL: return yes ? OPENBGI_IDRETRY : OPENBGI_IDCANCEL;
		default: return yes ? OPENBGI_IDOK : OPENBGI_IDCANCEL;
	}
}

/* the one-field dialog as window.prompt: the answer comes back converted to
 * the engine's encoding into `out` (0x100 bytes), cut to `maxLen` bytes
 * when that is positive - which may split a two-byte character, as the
 * EDIT control's limit cannot; 0 when cancelled */
int OS_DialogInput1(const char* title, const char* initial, int maxLen, int digits, char* out)
{
	char t[0x400], i[0x400], r[0x400], sj[0x100];
	BGI_UNUSED(digits); // (a prompt cannot filter what is typed: a digits-only field takes the answer as it is)
	OsCommon_SjisToUtf8(title ? title : "", t, sizeof t);
	OsCommon_SjisToUtf8(initial ? initial : "", i, sizeof i);
	if(!WasmJs_Prompt(t, i, r, sizeof r))
		return 0;
	Sjis_FromUtf8(r, sj, sizeof sj);
	if(maxLen > 0 && (int)strlen(sj) > maxLen)
		sj[maxLen] = 0;
	snprintf(out, 0x100, "%s", sj);
	return 1;
}

/* the two-field dialog as two prompts in a row, each titled "<title>\n<label>";
 * cancelling the first skips the second (0), the result is the second's */
int OS_DialogInput2(const char* title, const char* label1, const char* initial1, int maxLen1, int digits1, char* out1,
	const char* label2, const char* initial2, int maxLen2, int digits2, char* out2, int compact)
{
	char t[0x400], l[0x400];
	OsCommon_SjisToUtf8(title ? title : "", t, sizeof t);
	OsCommon_SjisToUtf8(label1 ? label1 : "", l, sizeof l);
	snprintf(t + strlen(t), sizeof t - strlen(t), "\n%s", l);
	BGI_UNUSED(compact);
	if(!OS_DialogInput1(t, initial1, maxLen1, digits1, out1))
		return 0;
	OsCommon_SjisToUtf8(title ? title : "", t, sizeof t);
	OsCommon_SjisToUtf8(label2 ? label2 : "", l, sizeof l);
	snprintf(t + strlen(t), sizeof t - strlen(t), "\n%s", l);
	return OS_DialogInput1(t, initial2, maxLen2, digits2, out2);
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

int OS_FileDialogN(int save, char* path, size_t n, const char* const* descs, const char* const* pats, int count,
	const char* defExt, const char* initialDir, const char* title)
{
	BGI_UNUSED(save);
	BGI_UNUSED(path);
	BGI_UNUSED(n);
	BGI_UNUSED(descs);
	BGI_UNUSED(pats);
	BGI_UNUSED(count);
	BGI_UNUSED(defExt);
	BGI_UNUSED(initialDir);
	BGI_UNUSED(title);
	return 0;
}

// no game chooser on a page: the game is what the server serves
int OS_GameChooserDialog(OpenBGI_GameChooser_t* l)
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
	OsCommon_SjisToUtf8(sjis, utf, sizeof utf);
	WasmJs_ClipboardWrite(utf);
	return 1;
}

// ---- the pump --------------------------------------------------------------------------------

void OS_SetHostHandlers(const OsHostHandlers_t* h)
{
	if(h)
		gIn = *h;
	else
		memset(&gIn, 0, sizeof gIn);
}

// no file drops on the canvas: `drop_file` is never raised
void OS_DragAccept(int accept)
{
	BGI_UNUSED(accept);
}

/* a typed code point to the host's `text` as UTF-8; a UTF-16 surrogate
 * half waits for its other half, control characters are keys */
static void Text(uint32_t cp)
{
	static uint32_t high; // the first half of a surrogate pair
	char u[5];
	if(cp >= 0xd800 && cp < 0xdc00)
	{
		high = cp;
		return;
	}
	if(cp >= 0xdc00 && cp < 0xe000)
	{
		if(!high)
			return;
		cp = 0x10000 + ((high - 0xd800) << 10) + (cp - 0xdc00);
	}
	high = 0;
	if(cp < 0x20 || cp == 0x7f || cp > 0x10ffff || !gIn.text)
		return;
	if(cp < 0x80)
		u[0] = (char)cp, u[1] = 0;
	else if(cp < 0x800)
		u[0] = (char)(0xc0 | (cp >> 6)), u[1] = (char)(0x80 | (cp & 0x3f)), u[2] = 0;
	else if(cp < 0x10000)
		u[0] = (char)(0xe0 | (cp >> 12)), u[1] = (char)(0x80 | ((cp >> 6) & 0x3f)), u[2] = (char)(0x80 | (cp & 0x3f)), u[3] = 0;
	else
		u[0] = (char)(0xf0 | (cp >> 18)), u[1] = (char)(0x80 | ((cp >> 12) & 0x3f)), u[2] = (char)(0x80 | ((cp >> 6) & 0x3f)),
		u[3] = (char)(0x80 | (cp & 0x3f)), u[4] = 0;
	gIn.text(gIn.user, u);
}

/* Hand one queued event on: the input to the host's handlers, keeping the
 * pointer up to date on the way, the rest to the engine's callbacks.  A
 * press of the left or right button within 400 ms and 4 pixels of the
 * previous press of the same button is marked as a double click, as the
 * desktop sends WM_?BUTTONDBLCLK for it (the browser's dblclick event is
 * not used). */
static void Deliver(const WasmEvent_t* e)
{
	switch(e->type)
	{
		case WASM_EV_KEY_DOWN:
			if(gIn.key)
				gIn.key(gIn.user, e->a, 1, e->b ? OPENBGI_KEY_REPEAT : 0);
			break;
		case WASM_EV_KEY_UP:
			if(gIn.key)
				gIn.key(gIn.user, e->a, 0, 0);
			break;
		case WASM_EV_SYSKEY:
			if(gIn.key)
				gIn.key(gIn.user, e->a, 1, OPENBGI_KEY_SYSTEM); // released by the ordinary key-up
			break;
		case WASM_EV_MOUSE_MOVE:
			gMouseX = e->a;
			gMouseY = e->b;
			if(gIn.pointer)
				gIn.pointer(gIn.user, e->a, e->b);
			break;
		case WASM_EV_BUTTON:
		{
			int flags = 0;
			gMouseX = e->c;
			gMouseY = e->d;
			if(e->b)
			{ // WM_?BUTTONDBLCLK in place of the ordinary press
				uint32_t now = OS_TicksMs();
				if(e->a <= 1 && gLastClickBtn == e->a && now - gLastClickTime < 400 && BGI_ABS(e->c - gLastClickX) < 4 &&
					BGI_ABS(e->d - gLastClickY) < 4)
				{
					flags = OPENBGI_CLICK_DOUBLE;
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
			if(gIn.button)
				gIn.button(gIn.user, e->a, e->b, e->c, e->d, flags);
			break;
		}
		case WASM_EV_WHEEL:
			if(gIn.wheel)
				gIn.wheel(gIn.user, e->a);
			break;
		case WASM_EV_TOUCH:
			if(gIn.touch)
				gIn.touch(gIn.user, e->a, e->b, e->c, e->d);
			break;
		case WASM_EV_ACTIVATE:
			if(gIn.focus)
				gIn.focus(gIn.user, e->a);
			break;
		case WASM_EV_CLOSE:
			if(gIn.close)
				gIn.close(gIn.user);
			break;
		case WASM_EV_PAINT:
			if(gIn.paint)
				gIn.paint(gIn.user);
			break;
		case WASM_EV_CHAR:
			Text((uint32_t)e->a);
			break;
		default:
			break;
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
})
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

/* one message: the oldest queued event; 1 when something was delivered.  When the engine has
 * not returned to the browser for 20 ms (a pass that presented a frame
 * skips the idle step) the pump yields first, so that input keeps arriving
 * and the canvas keeps painting. */
int OS_PumpMessages(void)
{
	WasmEvent_t e;
	if(OS_TicksMs() - gLastYield > 20)
		OsWasm_Yield(0);
	if(!PopEvent(&e))
		return 0;
	Deliver(&e);
	return 1;
}
