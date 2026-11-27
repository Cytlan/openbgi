/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/overlay.c - unit test of the screen layer and the on-screen
 *                   controls (OpenBGI_SetSurface, OpenBGI_SurfaceTouch and
 *                   OpenBGI_Frame_t.layer of openbgi.h; src/sys/overlay.c);
 *                   built as test_overlay by `make test`
 *
 * An engine with its graphics but no window of the host's, as in
 * tests/touch.c: the test says the window is up with a picture of
 * 640 x 480.  The surface is a phone's, 2400 x 1080 at 400 pixels an
 * inch, the picture shown at (480, 0) as 1440 x 1080 - so a pixel of the
 * picture is 2.25 of the surface.  Each frame takes the layer
 * (Overlay_Frame, as OpenBGI_RunFrame does) and notes what the mouse did
 * as tests/touch.c notes it.  The groups, in the order they run:
 *
 *   none    - no surface: no layer, and a touch "in the surface's pixels"
 *             is one in the picture's
 *   layer   - a surface: a layer of its size, all of it at first and then
 *             unchanged; the pad's button and the settings button drawn
 *             in it and nothing else;
 *             the gestures' scale told from the surface; no surface any
 *             more: no layer
 *   places  - the controls lie on the surface, apart from each other, on
 *             a phone held either way, with a cutout, and on a surface
 *             too small for them at full size
 *   show    - a tap on the pad's button turns the pad on - the controls
 *             are drawn, the frame says the part that changed, the
 *             setting is kept - and another off; a finger that slides off
 *             it before lifting does nothing
 *   pad     - the pad's controls play as a pad's: Confirm, Cancel, Skip,
 *             Backlog, Forward the buttons, the D-pad the direction under
 *             the finger, the stick by how far the finger is out; no pad
 *             "is there" for it; a control drawn anew when held
 *   claim   - a finger that went down on a control stays the control's
 *             wherever it goes; a second one on the picture taps there;
 *             a control held by one finger takes no second
 *   through - with the pad on a touch beside the controls reaches the
 *             picture, unless the settings say only the pad plays; with
 *             the pad off it always does
 *   kept    - the settings turn the pad on at a start; an engine that
 *             starts again keeps its surface and makes the layer anew
 */
#include "bgi/core.h"
#include "bgi/engine.h"
#include "bgi/input.h"
#include "bgi/inputmap.h"
#include "bgi/overlay.h"
#include "bgi/version.h"
#include "bgi/gfx.h"
#include "bgi/display.h"
#include "bgi/sysobj/events.h"
#include "host/os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

#define W     640
#define H     480
#define FRAME 16 // ms a frame

#define SW    2400 // the surface,
#define SH    1080
#define DPI   400
#define PX    480 // .. and the picture on it
#define PW    1440

static uint32_t gNow = 1000;
static uint8_t gPicture[W * H * 4];
static char gLog[2048];
static int gFrame;
static OpenBGI_Frame_t gLast; // the frame taken last

static uint32_t TicksMs(void* user)
{
	(void)user;
	return gNow;
}

static OpenBGI_Host_t gHost;

static OpenBGI_t* EngineUp(void)
{
	OpenBGI_t* o;
	gHost = *OS_Host();
	if(gHost.window)
	{ // a phone's host fits no window to a screen (one that does gets no on-screen controls: tests/settings.c)
		static OpenBGI_Window_t window;
		window = *gHost.window;
		window.workArea = NULL;
		gHost.window = &window;
	}
	gHost.ticksMs = TicksMs;
	o = OpenBGI_New(&gHost);
	if(!o)
		return NULL;
	Gfx_Create(o, 0xffffffffu, 1);
	Input_ResetLayers(o);
	o->input.inputEnable = 1;
	o->display.windowAlive = 1;
	memset(&o->screen.pic, 0, sizeof o->screen.pic);
	o->screen.pic.pixels = gPicture;
	o->screen.pic.w = W;
	o->screen.pic.h = H;
	o->screen.pic.pitch = W * 4;
	o->screen.pic.bpp = 4;
	o->screen.pic.mode = PM_RGB32;
	gLog[0] = 0;
	gFrame = 0;
	return o;
}

static void EngineDown(OpenBGI_t* o)
{
	MsgQueue_Clear(o);
	Input_ResetLayers(o);
	memset(&o->screen.pic, 0, sizeof o->screen.pic);
	o->display.windowAlive = 0;
	Gfx_Destroy(o);
	OpenBGI_Delete(o);
}

// the phone's surface, the picture in its middle
static void Surface(OpenBGI_t* o, int w, int h, int dpi, int px, int py, int pw, int ph)
{
	OpenBGI_Surface_t s;
	memset(&s, 0, sizeof s);
	s.size = sizeof s;
	s.width = w;
	s.height = h;
	s.picture.x = px;
	s.picture.y = py;
	s.picture.w = pw;
	s.picture.h = ph;
	s.dpi = dpi;
	OpenBGI_SetSurface(o, &s);
}

static void Phone(OpenBGI_t* o)
{
	Surface(o, SW, SH, DPI, PX, 0, PW, SH);
}

static void Note(const char* fmt, int a, int b)
{
	size_t n = strlen(gLog);
	snprintf(gLog + n, sizeof gLog - n, n ? " %d:" : "%d:", gFrame);
	n = strlen(gLog);
	snprintf(gLog + n, sizeof gLog - n, fmt, a, b);
}

// `count` frames: what the mouse did in each into gLog, the layer of the last into gLast
static void Frames(OpenBGI_t* o, int count)
{
	while(count-- > 0)
	{
		uint8_t l = o->input.keyboard[1] & 0x80, r = o->input.keyboard[2] & 0x80;
		o->input.pointerMoved = 0;
		InputMap_Frame(o);
		if(o->input.pointerMoved)
			Note("M%d,%d", o->input.pointerX, o->input.pointerY);
		if((o->input.keyboard[1] & 0x80) != l)
			Note(l ? "L-" : "L+", 0, 0);
		if((o->input.keyboard[2] & 0x80) != r)
			Note(r ? "R-" : "R+", 0, 0);
		memset(&gLast, 0, sizeof gLast);
		gLast.size = sizeof gLast;
		Overlay_Frame(o, &gLast);
		gFrame++;
		gNow += FRAME;
	}
}

static int Log(const char* want, int line)
{
	int same = strcmp(gLog, want) == 0;
	if(!same)
	{
		printf("  FAIL %s:%d: the mouse did \"%s\", not \"%s\"\n", __FILE__, line, gLog, want);
		gFails++;
	}
	gLog[0] = 0;
	gFrame = 0;
	return same;
}
#define LOG(want) Log(want, __LINE__)

// the alpha of the layer at a place of the surface; -1 without a layer
static int Alpha(int x, int y)
{
	if(!gLast.layer || x < 0 || y < 0 || x >= gLast.layerWidth || y >= gLast.layerHeight)
		return -1;
	return gLast.layer[(size_t)y * (size_t)gLast.layerPitch + (size_t)x * 4 + 3];
}

// whether anything is drawn in control k's square
static int Drawn(OpenBGI_t* o, int k)
{
	const OverlayControl_t* c = &o->overlay.c[k];
	int x, y;
	for(y = c->y; y < c->y + c->side; y++)
		for(x = c->x; x < c->x + c->side; x++)
			if(Alpha(x, y) > 0)
				return 1;
	return 0;
}

// a finger down at a control's centre, moved by (dx, dy)
static void Down(OpenBGI_t* o, int id, int k, int dx, int dy)
{
	OpenBGI_SurfaceTouch(o, id, OPENBGI_TOUCH_DOWN, o->overlay.c[k].cx + dx, o->overlay.c[k].cy + dy);
}

static void Up(OpenBGI_t* o, int id, int k, int dx, int dy)
{
	OpenBGI_SurfaceTouch(o, id, OPENBGI_TOUCH_UP, o->overlay.c[k].cx + dx, o->overlay.c[k].cy + dy);
}

// the pad's button tapped
static void Show(OpenBGI_t* o)
{
	Down(o, 9, OVERLAY_SHOW, 0, 0);
	Frames(o, 2);
	Up(o, 9, OVERLAY_SHOW, 0, 0);
	Frames(o, 2);
}

static const OpenBGI_PadState_t* Own(OpenBGI_t* o)
{
	return &o->input.pad.port[PAD_PORT_OWN];
}

// ---- the groups ---------------------------------------------------------------------------------

static void TestNone(void)
{
	OpenBGI_t* o = EngineUp();
	printf("none\n");
	if(!o)
		return;
	Frames(o, 2);
	CHECK(!gLast.layer && gLast.layerChanges == 0 && gLast.layerWidth == 0);
	gFrame = 0;
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_DOWN, 300, 200);
	Frames(o, 5);
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_UP, 300, 200);
	Frames(o, 5);
	LOG("5:M300,200 6:L+ 7:L-");
	// a surface that says no size is none
	Surface(o, 0, 0, DPI, 0, 0, 0, 0);
	Frames(o, 1);
	CHECK(!gLast.layer);
	EngineDown(o);
}

static void TestLayer(void)
{
	OpenBGI_t* o = EngineUp();
	const OverlayControl_t* m;
	int k;
	printf("layer\n");
	if(!o)
		return;
	Phone(o);
	Frames(o, 1);
	CHECK(gLast.layer && gLast.layerWidth == SW && gLast.layerHeight == SH && gLast.layerPitch == SW * 4);
	CHECK(gLast.layerChanges == -1);
	Frames(o, 1);
	CHECK(gLast.layer && gLast.layerChanges == 0);
	// the pad's button: in the top right corner, 7 mm, drawn
	m = &o->overlay.c[OVERLAY_SHOW];
	CHECK(m->cx > SW - 200 && m->cy < 200 && m->r == 7 * DPI * 10 / 254 / 2);
	CHECK(Alpha(m->cx, m->cy) > 0 && Alpha(m->cx + m->r * 7 / 10, m->cy) > 0);
	CHECK(Alpha(m->cx - m->r - 8, m->cy) == 0 && Alpha(SW / 2, SH / 2) == 0 && Alpha(0, 0) == 0);
	// the settings button: in the top left corner, as large; nothing else is drawn
	m = &o->overlay.c[OVERLAY_MENU];
	CHECK(m->cx < 200 && m->cy < 200 && m->r == 7 * DPI * 10 / 254 / 2 && Drawn(o, OVERLAY_MENU));
	CHECK(Alpha(m->cx + m->r * 8 / 10, m->cy) > 0 && Alpha(m->cx + m->r + 8, m->cy) == 0);
	for(k = OVERLAY_MENU + 1; k < OVERLAY_CONTROLS; k++)
		CHECK(!Drawn(o, k));
	m = &o->overlay.c[OVERLAY_SHOW];
	CHECK(!Overlay_PadOn(o));
	// the gestures go by the picture's pixels an inch as it lies on the surface
	CHECK(o->input.touch.ppi == (DPI * W + PW / 2) / PW);
	Surface(o, SW, SH, DPI, PX + 240, 0, PW / 2, SH / 2); // the picture shown smaller: finer still, and no control moved
	Frames(o, 1);
	CHECK(o->input.touch.ppi == (DPI * W + PW / 4) / (PW / 2));
	CHECK(gLast.layer && gLast.layerChanges == 0);
	// the surface without its pixels an inch: 160
	Surface(o, SW, SH, 0, PX, 0, PW, SH);
	Frames(o, 1);
	CHECK(gLast.layerChanges == -1 && o->overlay.c[OVERLAY_SHOW].r == 7 * 160 * 10 / 254 / 2);
	// no surface any more
	OpenBGI_SetSurface(o, NULL);
	Frames(o, 1);
	CHECK(!gLast.layer && !o->overlay.pix);
	EngineDown(o);
}

// the controls of the surface as it is: all on it, none on another
static void Apart(OpenBGI_t* o, int line)
{
	const OverlayState_t* v = &o->overlay;
	int a, b, bad = 0;
	for(a = 0; a < OVERLAY_CONTROLS; a++)
	{
		const OverlayControl_t* c = &v->c[a];
		if(c->x < 0 || c->y < 0 || c->x + c->side > v->surface.width || c->y + c->side > v->surface.height)
			bad++;
		for(b = a + 1; b < OVERLAY_CONTROLS; b++)
		{
			const OverlayControl_t* d = &v->c[b];
			if(c->x < d->x + d->side && d->x < c->x + c->side && c->y < d->y + d->side && d->y < c->y + c->side)
				bad++;
		}
	}
	if(bad)
	{
		printf("  FAIL %s:%d: %d controls lie off the surface or on another\n", __FILE__, line, bad);
		gFails++;
	}
}

static void TestPlaces(void)
{
	OpenBGI_t* o = EngineUp();
	const OverlayControl_t* c;
	OpenBGI_Surface_t s;
	printf("places\n");
	if(!o)
		return;
	Phone(o);
	Frames(o, 1);
	c = o->overlay.c;
	Apart(o, __LINE__);
	CHECK(o->overlay.scale == 256);
	// the D-pad low on the left with the stick over it; the buttons a column on the right, Confirm lowest
	CHECK(c[OVERLAY_DPAD].cx < SW / 4 && c[OVERLAY_DPAD].cy > SH / 2 && c[OVERLAY_STICK].cx == c[OVERLAY_DPAD].cx && c[OVERLAY_STICK].cy < c[OVERLAY_DPAD].cy);
	CHECK(c[OVERLAY_CONFIRM].cy > c[OVERLAY_CANCEL].cy && c[OVERLAY_CANCEL].cy > c[OVERLAY_SKIP].cy);
	CHECK(c[OVERLAY_SKIP].cy > c[OVERLAY_FORWARD].cy && c[OVERLAY_FORWARD].cy > c[OVERLAY_BACKLOG].cy && c[OVERLAY_BACKLOG].cy > c[OVERLAY_SHOW].cy);
	// the settings button opposite the pad's, over the stick
	CHECK(c[OVERLAY_MENU].cy == c[OVERLAY_SHOW].cy && c[OVERLAY_MENU].cx == SW - 1 - c[OVERLAY_SHOW].cx && c[OVERLAY_MENU].cy < c[OVERLAY_STICK].cy);
	CHECK(c[OVERLAY_DPAD].r == 24 * DPI * 10 / 254 / 2 && c[OVERLAY_CONFIRM].r == 13 * DPI * 10 / 254 / 2);
	// the column lies within 16 mm of the edge: beside a 16:9 picture on this phone
	CHECK(c[OVERLAY_CONFIRM].x >= SW - 16 * DPI * 10 / 254 && c[OVERLAY_BACKLOG].x >= SW - 16 * DPI * 10 / 254);
	// the phone held upright
	Surface(o, SH, SW, DPI, 0, 795, SH, 810);
	Frames(o, 1);
	Apart(o, __LINE__);
	CHECK(o->overlay.scale == 256 && gLast.layerWidth == SH && gLast.layerChanges == -1);
	CHECK(c[OVERLAY_DPAD].cy > SW - 500 && c[OVERLAY_CONFIRM].cx > SH - 300);
	// a cutout on the left and a rounded corner's worth on the right: the controls keep off them
	memset(&s, 0, sizeof s);
	s.size = sizeof s;
	s.width = SW;
	s.height = SH;
	s.picture.x = PX;
	s.picture.w = PW;
	s.picture.h = SH;
	s.safe.x = 90;
	s.safe.y = 0;
	s.safe.w = SW - 90 - 40;
	s.safe.h = SH;
	s.dpi = DPI;
	OpenBGI_SetSurface(o, &s);
	Frames(o, 1);
	Apart(o, __LINE__);
	CHECK(c[OVERLAY_DPAD].x >= 90 && c[OVERLAY_CONFIRM].x + c[OVERLAY_CONFIRM].side <= SW - 40 && c[OVERLAY_SHOW].x + c[OVERLAY_SHOW].side <= SW - 40);
	CHECK(c[OVERLAY_MENU].x >= 90);
	// a small screen: the pad smaller, the two buttons as they were
	Surface(o, 800, 480, 240, 80, 0, 640, 480);
	Frames(o, 1);
	Apart(o, __LINE__);
	CHECK(o->overlay.scale < 256 && o->overlay.scale >= 128);
	CHECK(c[OVERLAY_SHOW].r == 7 * 240 * 10 / 254 / 2 && c[OVERLAY_MENU].r == c[OVERLAY_SHOW].r && c[OVERLAY_DPAD].r < 24 * 240 * 10 / 254 / 2);
	EngineDown(o);
}

static void TestShow(void)
{
	OpenBGI_t* o = EngineUp();
	const OverlayControl_t* m;
	char value[16];
	int k, rest;
	printf("show\n");
	if(!o)
		return;
	Phone(o);
	Frames(o, 2);
	m = &o->overlay.c[OVERLAY_SHOW];
	// held: drawn anew, lighter; the pad comes when the finger lifts
	rest = Alpha(m->cx + m->r / 2, m->cy + m->r * 6 / 10); // its body, beside the sign
	Down(o, 9, OVERLAY_SHOW, 0, 0);
	Frames(o, 1);
	CHECK(gLast.layerChanges == 1 && gLast.layerChange.x == m->x && gLast.layerChange.y == m->y && gLast.layerChange.w == m->side);
	CHECK(Alpha(m->cx + m->r / 2, m->cy + m->r * 6 / 10) > rest);
	CHECK(!Overlay_PadOn(o));
	Up(o, 9, OVERLAY_SHOW, 0, 0);
	Frames(o, 1);
	CHECK(Overlay_PadOn(o));
	// all the controls drawn; the frame says the part that holds them
	CHECK(gLast.layerChanges == 1);
	for(k = 0; k < OVERLAY_CONTROLS; k++)
	{
		const OverlayControl_t* c = &o->overlay.c[k];
		CHECK(Drawn(o, k));
		CHECK(gLast.layerChange.x <= c->x && gLast.layerChange.y <= c->y);
		CHECK(gLast.layerChange.x + gLast.layerChange.w >= c->x + c->side && gLast.layerChange.y + gLast.layerChange.h >= c->y + c->side);
	}
	CHECK(Engine_CfgGet(o, "", "touchPad", 0, value, sizeof value) == 2 && strcmp(value, "on") == 0);
	CHECK(Alpha(m->cx + m->r * 3 / 10, m->cy - m->r / 4) > rest); // the button's sign is filled while the pad is shown
	Frames(o, 1);
	CHECK(gLast.layerChanges == 0);
	LOG(""); // (nothing of it was a touch on the picture)
	// and off again
	Show(o);
	CHECK(!Overlay_PadOn(o) && !Drawn(o, OVERLAY_DPAD) && Drawn(o, OVERLAY_SHOW));
	CHECK(Alpha(m->cx + m->r * 3 / 10, m->cy - m->r / 4) == rest); // .. and an outline again
	CHECK(Engine_CfgGet(o, "", "touchPad", 0, value, sizeof value) < 0);
	// a finger that slides off the button before it lifts
	Down(o, 9, OVERLAY_SHOW, 0, 0);
	OpenBGI_SurfaceTouch(o, 9, OPENBGI_TOUCH_MOVE, m->cx - 400, m->cy + 300);
	OpenBGI_SurfaceTouch(o, 9, OPENBGI_TOUCH_UP, m->cx - 400, m->cy + 300);
	Frames(o, 8);
	CHECK(!Overlay_PadOn(o));
	// .. or is taken away
	Down(o, 9, OVERLAY_SHOW, 0, 0);
	OpenBGI_SurfaceTouch(o, 9, OPENBGI_TOUCH_CANCEL, m->cx, m->cy);
	Frames(o, 2);
	CHECK(!Overlay_PadOn(o) && !o->overlay.held);
	LOG("");
	EngineDown(o);
}

static void TestPad(void)
{
	static const struct
	{
		int control;
		uint32_t button;
	} kButtons[] = {{OVERLAY_CONFIRM, OPENBGI_PAD_SOUTH}, {OVERLAY_CANCEL, OPENBGI_PAD_EAST}, {OVERLAY_SKIP, OPENBGI_PAD_R2},
		{OVERLAY_BACKLOG, OPENBGI_PAD_L1}, {OVERLAY_FORWARD, OPENBGI_PAD_R1}};
	OpenBGI_t* o = EngineUp();
	const OverlayControl_t* c;
	int i, r, before;
	printf("pad\n");
	if(!o)
		return;
	Phone(o);
	Frames(o, 1);
	// off: a touch where a control would be is one on the surface
	Down(o, 1, OVERLAY_CONFIRM, 0, 0);
	CHECK(!o->overlay.held && Own(o)->buttons == 0);
	Up(o, 1, OVERLAY_CONFIRM, 0, 0);
	Frames(o, 10);
	gLog[0] = 0;
	Show(o);
	CHECK(Overlay_PadOn(o));
	c = o->overlay.c;
	for(i = 0; i < (int)(sizeof kButtons / sizeof kButtons[0]); i++)
	{
		int k = kButtons[i].control;
		before = Alpha(c[k].cx + c[k].r * 7 / 10, c[k].cy);
		Down(o, 1, k, 3, -2);
		CHECK(Own(o)->buttons == kButtons[i].button && o->input.pad.buttons == kButtons[i].button);
		Frames(o, 1);
		CHECK(gLast.layerChanges == 1 && gLast.layerChange.x == c[k].x && gLast.layerChange.w == c[k].side);
		CHECK(Alpha(c[k].cx + c[k].r * 7 / 10, c[k].cy) > before);
		Up(o, 1, k, 3, -2);
		CHECK(Own(o)->buttons == 0 && o->input.pad.buttons == 0);
		Frames(o, 1);
		CHECK(Alpha(c[k].cx + c[k].r * 7 / 10, c[k].cy) == before);
	}
	CHECK(o->input.pad.attached == 0); // no pad "is there" for it
	// two at once
	Down(o, 1, OVERLAY_SKIP, 0, 0);
	Down(o, 2, OVERLAY_CONFIRM, 0, 0);
	CHECK(Own(o)->buttons == (OPENBGI_PAD_R2 | OPENBGI_PAD_SOUTH));
	Up(o, 1, OVERLAY_SKIP, 0, 0);
	CHECK(Own(o)->buttons == OPENBGI_PAD_SOUTH);
	Up(o, 2, OVERLAY_CONFIRM, 0, 0);
	// the D-pad: the arm under the finger, none in its middle; the finger slides from one to the next
	r = c[OVERLAY_DPAD].r;
	Down(o, 1, OVERLAY_DPAD, 0, -r * 2 / 3);
	CHECK(Own(o)->dpad == OPENBGI_DPAD_UP);
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_MOVE, c[OVERLAY_DPAD].cx + r * 2 / 3, c[OVERLAY_DPAD].cy - r / 4);
	CHECK(Own(o)->dpad == OPENBGI_DPAD_RIGHT);
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_MOVE, c[OVERLAY_DPAD].cx + r / 10, c[OVERLAY_DPAD].cy);
	CHECK(Own(o)->dpad == 0);
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_MOVE, c[OVERLAY_DPAD].cx - r / 2, c[OVERLAY_DPAD].cy + r / 3);
	CHECK(Own(o)->dpad == OPENBGI_DPAD_LEFT);
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_MOVE, c[OVERLAY_DPAD].cx - r * 3, c[OVERLAY_DPAD].cy + r * 4); // far off it: still its own
	CHECK(Own(o)->dpad == OPENBGI_DPAD_DOWN);
	Up(o, 1, OVERLAY_DPAD, -r * 3, r * 4);
	CHECK(Own(o)->dpad == 0 && !o->overlay.held);
	Down(o, 1, OVERLAY_DPAD, r * 2 / 3, 0);
	CHECK(Own(o)->dpad == OPENBGI_DPAD_RIGHT);
	Down(o, 2, OVERLAY_DPAD, 0, r * 2 / 3);
	CHECK(Own(o)->dpad == OPENBGI_DPAD_RIGHT); // (one finger has it)
	Up(o, 2, OVERLAY_DPAD, 0, r * 2 / 3);
	CHECK(Own(o)->dpad == OPENBGI_DPAD_RIGHT);
	Up(o, 1, OVERLAY_DPAD, r * 2 / 3, 0);
	// the stick: at rest in its middle, at its end three quarters of the way out and beyond
	r = c[OVERLAY_STICK].r;
	Frames(o, 1);
	Down(o, 1, OVERLAY_STICK, 0, 0);
	CHECK(Own(o)->leftX == 0 && Own(o)->leftY == 0 && (o->overlay.held & 1u << OVERLAY_STICK));
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_MOVE, c[OVERLAY_STICK].cx + r * 3 / 8, c[OVERLAY_STICK].cy);
	CHECK(Own(o)->leftX > 15000 && Own(o)->leftX < 18000 && Own(o)->leftY == 0);
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_MOVE, c[OVERLAY_STICK].cx, c[OVERLAY_STICK].cy - r * 3 / 4);
	CHECK(Own(o)->leftX == 0 && Own(o)->leftY < -32000);
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_MOVE, c[OVERLAY_STICK].cx - r * 5, c[OVERLAY_STICK].cy + r * 5);
	CHECK(Own(o)->leftX < -23000 && Own(o)->leftX > -23300 && Own(o)->leftY == -Own(o)->leftX);
	Frames(o, 1);
	CHECK(gLast.layerChanges == 1 && gLast.layerChange.x == c[OVERLAY_STICK].x && gLast.layerChange.w == c[OVERLAY_STICK].side);
	Up(o, 1, OVERLAY_STICK, -r * 5, r * 5);
	CHECK(Own(o)->leftX == 0 && Own(o)->leftY == 0);
	// the host's pads have their four ports, and nothing of them reaches the engine's own
	{
		OpenBGI_PadState_t s;
		memset(&s, 0, sizeof s);
		s.size = sizeof s;
		s.buttons = OPENBGI_PAD_NORTH;
		OpenBGI_Pad(o, PAD_PORT_OWN, &s);
		CHECK(Own(o)->buttons == 0 && o->input.pad.attached == 0);
		OpenBGI_Pad(o, PAD_HOST_PORTS - 1, &s);
		CHECK(o->input.pad.attached == 1u << (PAD_HOST_PORTS - 1) && o->input.pad.buttons == OPENBGI_PAD_NORTH);
		Down(o, 1, OVERLAY_CANCEL, 0, 0);
		CHECK(o->input.pad.buttons == (OPENBGI_PAD_NORTH | OPENBGI_PAD_EAST));
		OpenBGI_Pad(o, PAD_HOST_PORTS - 1, NULL);
		CHECK(o->input.pad.attached == 0 && o->input.pad.buttons == OPENBGI_PAD_EAST);
		Up(o, 1, OVERLAY_CANCEL, 0, 0);
	}
	// the pad switched off under a finger: nothing stays held
	Down(o, 1, OVERLAY_CONFIRM, 0, 0);
	Overlay_SetPad(o, 0, 0);
	CHECK(Own(o)->buttons == 0 && !(o->overlay.held & 1u << OVERLAY_CONFIRM));
	Up(o, 1, OVERLAY_CONFIRM, 0, 0);
	CHECK(Own(o)->buttons == 0);
	EngineDown(o);
}

static void TestClaim(void)
{
	OpenBGI_t* o = EngineUp();
	const OverlayControl_t* c;
	printf("claim\n");
	if(!o)
		return;
	Phone(o);
	Frames(o, 1);
	Overlay_SetPad(o, 1, 0);
	Frames(o, 12);
	gLog[0] = 0;
	gFrame = 0;
	c = o->overlay.c;
	// down on Confirm, off onto the picture, up there: Confirm's all the way, no touch on the picture
	Down(o, 1, OVERLAY_CONFIRM, 0, 0);
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_MOVE, SW / 2, SH / 2);
	CHECK(Own(o)->buttons == OPENBGI_PAD_SOUTH);
	// .. while a second finger taps the picture: surface (1200, 540) is the picture's (320, 240)
	OpenBGI_SurfaceTouch(o, 2, OPENBGI_TOUCH_DOWN, 1200, 540);
	Frames(o, 5);
	OpenBGI_SurfaceTouch(o, 2, OPENBGI_TOUCH_UP, 1200, 540);
	Frames(o, 1);
	CHECK(Own(o)->buttons == OPENBGI_PAD_SOUTH);
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_UP, SW / 2, SH / 2);
	CHECK(Own(o)->buttons == 0);
	// (Confirm with nothing focused goes on: the left button, with the tap's own after it)
	Frames(o, 12);
	CHECK(strstr(gLog, "M320,240") != NULL);
	gLog[0] = 0;
	gFrame = 0;
	// a control that a finger holds takes no second one, and the second does nothing when it lifts
	Down(o, 1, OVERLAY_CANCEL, 0, 0);
	Down(o, 2, OVERLAY_CANCEL, 2, 2);
	Up(o, 2, OVERLAY_CANCEL, 2, 2);
	CHECK(Own(o)->buttons == OPENBGI_PAD_EAST);
	Up(o, 1, OVERLAY_CANCEL, 0, 0);
	CHECK(Own(o)->buttons == 0);
	// a finger that went down on the picture and slides onto a control stays the picture's
	OpenBGI_SurfaceTouch(o, 3, OPENBGI_TOUCH_DOWN, 1200, 540);
	OpenBGI_SurfaceTouch(o, 3, OPENBGI_TOUCH_MOVE, c[OVERLAY_SKIP].cx, c[OVERLAY_SKIP].cy);
	CHECK(Own(o)->buttons == 0 && !o->overlay.held);
	OpenBGI_SurfaceTouch(o, 3, OPENBGI_TOUCH_UP, c[OVERLAY_SKIP].cx, c[OVERLAY_SKIP].cy);
	CHECK(Own(o)->buttons == 0);
	Frames(o, 12);
	EngineDown(o);
}

// a tap on the picture's middle through the surface
static void TapPicture(OpenBGI_t* o)
{
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_DOWN, PX + 450, 225);
	Frames(o, 5);
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_UP, PX + 450, 225);
	Frames(o, 5);
}

static void TestThrough(void)
{
	OpenBGI_t* o = EngineUp();
	printf("through\n");
	if(!o)
		return;
	Phone(o);
	Frames(o, 1);
	gFrame = 0;
	// the pad off: surface (480 + 450, 225) is the picture's (200, 100)
	TapPicture(o);
	LOG("5:M200,100 6:L+ 7:L-");
	// a touch in a bar is one beside the picture, as the host's own mapping has it
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_DOWN, 30, 600);
	Frames(o, 5);
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_UP, 30, 600);
	Frames(o, 5);
	CHECK(o->input.touch.tapValid && o->input.touch.tapX == -200 && o->input.touch.tapY == 266);
	gLog[0] = 0;
	gFrame = 0;
	// the pad on: the same
	Overlay_SetPad(o, 1, 0);
	CHECK(!o->overlay.padOnly);
	TapPicture(o);
	LOG("5:M200,100 6:L+ 7:L-");
	// only the pad plays
	o->overlay.padOnly = 1;
	TapPicture(o);
	LOG("");
	Down(o, 1, OVERLAY_CONFIRM, 0, 0);
	CHECK(Own(o)->buttons == OPENBGI_PAD_SOUTH);
	Up(o, 1, OVERLAY_CONFIRM, 0, 0);
	Frames(o, 12);
	gLog[0] = 0;
	gFrame = 0;
	// .. which holds only while the pad is on, or nothing could be touched
	Overlay_SetPad(o, 0, 0);
	TapPicture(o);
	LOG("5:M200,100 6:L+ 7:L-");
	EngineDown(o);
}

static void TestKept(void)
{
	OpenBGI_t* o = EngineUp();
	printf("kept\n");
	if(!o)
		return;
	Phone(o);
	// the settings are read once the game runs: the pad on, only the pad playing
	CHECK(Engine_CfgSet(o, "", "touchPad", 0, "on"));
	CHECK(Engine_CfgSet(o, "", "touchThrough", 0, "off"));
	Frames(o, 1);
	CHECK(!Overlay_PadOn(o)); // (not yet)
	o->engine.stage = ENGINE_STAGE_VM;
	Frames(o, 1);
	CHECK(Overlay_PadOn(o) && o->overlay.padOnly && Drawn(o, OVERLAY_DPAD));
	// what the player does after that stands, whatever the settings said
	Overlay_SetPad(o, 0, 0);
	Frames(o, 2);
	CHECK(!Overlay_PadOn(o));
	// the engine starts again (the launcher's game, or back from it): the surface is as the host said it
	o->engine.stage = ENGINE_STAGE_NONE;
	memset(&o->screen.pic, 0, sizeof o->screen.pic);
	o->display.windowAlive = 0;
	Gfx_Destroy(o);
	Engine_Renew(o);
	CHECK(o->overlay.surface.width == SW && o->overlay.surface.dpi == DPI && o->overlay.surface.picture.x == PX && !o->overlay.pix);
	Frames(o, 1);
	CHECK(gLast.layer && gLast.layerChanges == -1 && gLast.layerWidth == SW && Drawn(o, OVERLAY_SHOW));
	OpenBGI_Delete(o);
}

int main(void)
{
	OS_Init();
	TestNone();
	TestLayer();
	TestPlaces();
	TestShow();
	TestPad();
	TestClaim();
	TestThrough();
	TestKept();
	OS_Shutdown();
	if(gFails)
	{
		printf("overlay: %d checks failed\n", gFails);
		return 1;
	}
	printf("overlay: all tests passed\n");
	return 0;
}
