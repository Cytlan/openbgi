/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/touch.c - unit test of the touch screen (OpenBGI_Touch and
 *                 OpenBGI_TouchScale of openbgi.h, src/sys/touch.c, and
 *                 where a tap goes: InputMap_TapPoint of bgi/inputmap.h);
 *                 built as test_touch by `make test`
 *
 * An engine with its graphics but no window of the host's: the test says
 * the window is up and gives it a picture of 640 x 480 (so a millimetre is
 * 6.4 pixels without the host's scale: the slop 19, the snap radius 32,
 * a swipe 77, a scroll step 51), makes a sprite registered as a click
 * target ("90 FA") and a knob itself, reports the contacts and runs
 * frames: each calls InputMap_Frame, as OpenBGI_RunFrame does before a pass,
 * and Knob_Update, as the pass does (a knob held follows the pointer),
 * moves the host's clock 16 ms on and notes what the mouse did in it - a
 * move of the pointer ("M x,y"), the left or right button pressed or
 * released ("L+", "R-"), a wheel notch away from the player or towards
 * ("W+", "W-") - as "<frame>:<what>".  The groups, in the order they run:
 *
 *   tap      - a tap on the sprite: the pointer there, a frame later the
 *              press, a frame later the release; the debugger's note of it
 *   snap     - a tap beside the sprite within the snap radius clicks its
 *              centre, one further away where it was; a layer the script
 *              pushed (a click anywhere reaches it) does not keep a tap
 *              from snapping, a higher one that covers the sprite does;
 *              with input off nothing is snapped to; without a window the
 *              tap clicks where it was
 *   disabled - an icon of an icon selection is snapped to, not while
 *              another wait has the input focus (it takes no click)
 *   scale    - the host's scale (OpenBGI_TouchScale) makes the snap radius
 *              and the slop physical; 0 forgets it
 *   slop     - a finger that slipped less than the slop taps where it went
 *              down; one that moved further does not tap
 *   long     - a finger held half a second: the right button where it
 *              went down; lifting it later does nothing; one that moved
 *              first is no long press, the pointer follows it instead
 *   swipe    - a quick vertical flick: a wheel notch away from the player
 *              down, towards up; too slow, too short or too slanted: none
 *   follow   - a pointing finger: the pointer follows once a flick's time
 *              has passed, the moves of one frame make one step
 *   two      - two fingers: tapped, the right button at their middle (as
 *              they went down); held too long, nothing; a second finger
 *              too late is ignored (the first one taps); moved, wheel
 *              notches for every scroll step their middle moves
 *   knob     - a drag from the knob's thumb holds the left button and
 *              moves the thumb; one from beside it too (the thumb taken at
 *              its click point); one from another target beside it points
 *   other    - a cancelled contact does nothing; a third finger ends the
 *              gesture until all have lifted; two taps queue one after the
 *              other
 *   frame    - through OpenBGI_RunFrame, with the picture of the screen
 *              (Screen_Setup) and what the engine draws in it: a child
 *              window or the text entry over the sprite keeps a tap from
 *              it there; a box drawn in the frame takes the tap (nothing
 *              snaps while it is up), and its button answers it
 */
#include "bgi/core.h"
#include "bgi/input.h"
#include "bgi/inputmap.h"
#include "bgi/gfx.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/sysobj.h"
#include "bgi/vm.h"
#include "bgi/waitobj.h"
#include "bgi/sysobj/targets.h"
#include "bgi/sysobj/events.h"
#include "bgi/display.h"
#include "bgi/edit.h"
#include "bgi/inframe.h"
#include "bgi/request.h"
#include "bgi/ui.h"
#include "host/os.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

#define W     640
#define H     480
#define FRAME 16 // ms a frame

static uint32_t gNow = 1000; // the host's clock (ms)
static uint8_t gPicture[W * H * 4];
static char gLog[2048]; // what the mouse did, frame by frame
static int gFrame;      // the next frame's number

static uint32_t TicksMs(void* user)
{
	(void)user;
	return gNow;
}

static OpenBGI_Host_t gHost;

// ---- the engine ---------------------------------------------------------------------------------

static OpenBGI_t* EngineUp(int window)
{
	OpenBGI_t* o;
	gHost = *OS_Host();
	gHost.ticksMs = TicksMs;
	o = OpenBGI_New(&gHost);
	if(!o)
		return NULL;
	Gfx_Create(o, 0xffffffffu, 1);
	Input_ResetLayers(o);
	o->input.inputEnable = 1;
	if(window)
	{ // the window is up and shows a picture of W x H
		o->display.windowAlive = 1;
		memset(&o->screen.pic, 0, sizeof o->screen.pic);
		o->screen.pic.pixels = gPicture;
		o->screen.pic.w = W;
		o->screen.pic.h = H;
		o->screen.pic.pitch = W * 4;
		o->screen.pic.bpp = 4;
		o->screen.pic.mode = PM_RGB32;
	}
	gLog[0] = 0;
	gFrame = 0;
	return o;
}

static void EngineDown(OpenBGI_t* o)
{
	MsgQueue_Clear(o); // (the clicks' events for the game)
	Knob_UnregisterAll(o);
	Target_Clear(o);
	Input_ResetLayers(o);
	memset(&o->screen.pic, 0, sizeof o->screen.pic);
	o->display.windowAlive = 0;
	Gfx_Destroy(o);
	OpenBGI_Delete(o);
}

// bitmap `no` of w x h, opaque
static void MakeBitmap(OpenBGI_t* o, int no, int w, int h)
{
	Bmp_t b;
	int x, y;
	BmpMgr_Create(o->gfx.bmpMgr, no, w, h, PM_ARGB32);
	BmpMgr_GetInfo(o->gfx.bmpMgr, &b, no);
	for(y = 0; y < h; y++)
		for(x = 0; x < w; x++)
			((uint32_t*)(b.pixels + (size_t)y * b.pitch))[x] = 0xff808080u;
}

// a shown sprite of bitmap `bmp` at (x, y) with priority `prio`
static uint32_t MakeSprite(OpenBGI_t* o, int bmp, int x, int y, int prio)
{
	uint32_t h = GfxCall_SpriteCreate(o);
	GfxCall_SpriteSet(o, h, x, y, bmp, 0, 0, prio);
	GfxCall_SpriteShow(o, h, 1);
	return h;
}

// the sprite of 40 x 30 at (100, 100), registered as a click target: centre (120, 115)
static uint32_t Button(OpenBGI_t* o)
{
	uint32_t s;
	MakeBitmap(o, 10, 40, 30);
	s = MakeSprite(o, 10, 100, 100, 3);
	CHECK(Target_Add(o, s));
	return s;
}

// ---- frames -------------------------------------------------------------------------------------

static void Note(const char* fmt, int a, int b)
{
	size_t n = strlen(gLog);
	snprintf(gLog + n, sizeof gLog - n, n ? " %d:" : "%d:", gFrame);
	n = strlen(gLog);
	snprintf(gLog + n, sizeof gLog - n, fmt, a, b);
}

// `count` frames: what the mouse did in each into gLog
static void Frames(OpenBGI_t* o, int count)
{
	while(count-- > 0)
	{
		uint8_t l = o->input.keyboard[1] & 0x80, r = o->input.keyboard[2] & 0x80;
		int32_t away = o->input.keyState[0x0e].counter, towards = o->input.keyState[0x0f].counter;
		o->input.pointerMoved = 0;
		InputMap_Frame(o);
		Knob_Update(o); // (what a pass does with the mouse here: a knob held follows it)
		if(o->input.pointerMoved)
			Note("M%d,%d", o->input.pointerX, o->input.pointerY);
		if((o->input.keyboard[1] & 0x80) != l)
			Note(l ? "L-" : "L+", 0, 0);
		if((o->input.keyboard[2] & 0x80) != r)
			Note(r ? "R-" : "R+", 0, 0);
		if(o->input.keyState[0x0e].counter != away)
			Note("W+", 0, 0);
		if(o->input.keyState[0x0f].counter != towards)
			Note("W-", 0, 0);
		gFrame++;
		gNow += FRAME;
	}
}

// compare the log with `want` (and start a new one); print both when they differ
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

static void Touch(OpenBGI_t* o, int id, int phase, int x, int y)
{
	OpenBGI_Touch(o, id, phase, x, y);
}

// a tap: down, five frames, up, five frames
static void Tap(OpenBGI_t* o, int x, int y)
{
	Touch(o, 1, OPENBGI_TOUCH_DOWN, x, y);
	Frames(o, 5);
	Touch(o, 1, OPENBGI_TOUCH_UP, x, y);
	Frames(o, 5);
}

// ---- the groups ---------------------------------------------------------------------------------

static void TestTap(void)
{
	OpenBGI_t* o = EngineUp(1);
	uint32_t s;
	printf("tap\n");
	if(!o)
		return;
	s = Button(o);
	CHECK(!o->input.touch.tapValid);
	Tap(o, 120, 115);
	LOG("5:M120,115 6:L+ 7:L-");
	CHECK(o->input.touch.tapValid && o->input.touch.tapX == 120 && o->input.touch.tapY == 115);
	CHECK(o->input.touch.tapToX == 120 && o->input.touch.tapToY == 115);
	CHECK(o->input.touch.tapKind == OPENBGI_TARGET_SPRITE && o->input.touch.tapOwner == s);
	CHECK(o->input.keyState[1].counter == 1);
	Tap(o, 105, 102); // anywhere on it: where the finger is
	LOG("5:M105,102 6:L+ 7:L-");
	EngineDown(o);
}

static void TestSnap(void)
{
	OpenBGI_t* o = EngineUp(1);
	OpenBGI_Target_t t[4];
	uint32_t s, low = (1u << 20) | 0xfffff, high = (7u << 20) | 0xfffff;
	int out[2], kind;
	uint32_t owner;
	printf("snap\n");
	if(!o)
		return;
	s = Button(o);
	Tap(o, 150, 115); // 11 pixels right of it
	LOG("5:M120,115 6:L+ 7:L-");
	CHECK(o->input.touch.tapX == 150 && o->input.touch.tapToX == 120 && o->input.touch.tapKind == OPENBGI_TARGET_SPRITE);
	Tap(o, 171, 115); // 32: the snap radius
	LOG("5:M120,115 6:L+ 7:L-");
	Tap(o, 120, 161);
	LOG("5:M120,115 6:L+ 7:L-");
	Tap(o, 172, 115); // 33: too far
	LOG("5:M172,115 6:L+ 7:L-");
	CHECK(o->input.touch.tapKind == 0 && o->input.touch.tapToX == 172);
	CHECK(InputMap_TapPoint(o, 172, 115, 32, out, &kind, &owner) == 0 && out[0] == 172 && out[1] == 115 && kind == 0);
	CHECK(InputMap_TapPoint(o, 150, 115, 32, out, &kind, &owner) == 2 && out[0] == 120 && out[1] == 115 && owner == s);
	CHECK(InputMap_TapPoint(o, 110, 110, 32, out, &kind, &owner) == 1 && out[0] == 110 && out[1] == 110);
	// a layer of the script's below the sprite: the buttons first
	MouseLayer_Push(o, low);
	CHECK(OpenBGI_Targets(o, t, 4, sizeof(OpenBGI_Target_t)) == 2 && t[0].kind == OPENBGI_TARGET_SPRITE && t[1].kind == OPENBGI_TARGET_LAYER && t[0].layer > low);
	Tap(o, 150, 115);
	LOG("5:M120,115 6:L+ 7:L-");
	CHECK(o->input.touch.tapKind == OPENBGI_TARGET_SPRITE);
	Tap(o, 300, 300); // nothing near: the layer, where the finger was
	LOG("5:M300,300 6:L+ 7:L-");
	CHECK(o->input.touch.tapKind == OPENBGI_TARGET_LAYER && o->input.touch.tapOwner == 1);
	// a higher one covers the sprite: not snapped to
	MouseLayer_Push(o, high);
	Tap(o, 150, 115);
	LOG("5:M150,115 6:L+ 7:L-");
	CHECK(o->input.touch.tapKind == OPENBGI_TARGET_LAYER && o->input.touch.tapOwner == 7);
	Tap(o, 110, 110); // on it: the layer's too
	LOG("5:M110,110 6:L+ 7:L-");
	CHECK(o->input.touch.tapKind == OPENBGI_TARGET_LAYER && o->input.touch.tapOwner == 7);
	MouseLayer_Pop(o, high);
	MouseLayer_Pop(o, low);
	// input off: nothing takes a click, nothing is snapped to
	o->input.inputEnable = 0;
	Tap(o, 150, 115);
	LOG("5:M150,115 6:L+ 7:L-");
	CHECK(o->input.touch.tapKind == 0);
	o->input.inputEnable = 1;
	EngineDown(o);
	// no window: the tap clicks where it was
	o = EngineUp(0);
	if(!o)
		return;
	Button(o);
	Tap(o, 150, 115);
	LOG("5:M150,115 6:L+ 7:L-");
	EngineDown(o);
}

/* icons of an icon selection ("90 B0") that take no click while another
 * wait has the input focus (OPENBGI_TARGET_DISABLED): not snapped to */
static void TestDisabled(void)
{
	OpenBGI_t* o = EngineUp(1);
	int32_t icons[2][4] = {{10, 20, 10, -1}, {60, 20, 10, -1}}; // x, y, bitmap, focus bitmap
	Rect_t area;
	Thread_t* th;
	uint32_t hw = 0;
	int x, y;
	char want[64];
	printf("disabled\n");
	if(!o)
		return;
	o->vm.rootThread = Thread_New(o, 0, 0, 0);
	th = Thread_SpawnChild(o, o->vm.rootThread, 0x100, 0x100, 0x400);
	MakeBitmap(o, 10, 40, 30);
	CHECK(GfxCall_WindowCreate(o, &hw, 300, 200) == 0);
	GfxCall_WindowSet(o, hw, 50, 60, 0, 0, 0, 4);
	GfxCall_WindowShow(o, hw, 1);
	CHECK(GfxCall_WindowGetTextArea(o, &area, hw) == 0);
	CHECK(StartIconSelect(o, th, hw, 2, icons, NULL, 3, 0, 0) == 0);
	x = 50 + area.l + 10 + 20; // the first icon's centre
	y = 60 + area.t + 20 + 15;
	Tap(o, x, y + 25); // 10 below it
	snprintf(want, sizeof want, "5:M%d,%d 6:L+ 7:L-", x, y);
	LOG(want);
	CHECK(o->input.touch.tapToX == x && o->input.touch.tapToY == y && o->input.touch.tapKind == OPENBGI_TARGET_ICON);
	InputFocus_Add(o, 0x7777, 0xfffffff0u); // a wait above has the focus
	Tap(o, x, y + 25);
	snprintf(want, sizeof want, "5:M%d,%d 6:L+ 7:L-", x, y + 25);
	LOG(want);
	CHECK(o->input.touch.tapToX == x && o->input.touch.tapToY == y + 25 && o->input.touch.tapKind == 0);
	InputFocus_Remove(o, 0x7777);
	Thread_DropWait(o, th);
	Thread_KillAllChildren(o, o->vm.rootThread);
	Thread_Delete(o, o->vm.rootThread);
	o->vm.rootThread = NULL;
	EngineDown(o);
}

static void TestScale(void)
{
	OpenBGI_t* o = EngineUp(1);
	printf("scale\n");
	if(!o)
		return;
	Button(o);
	OpenBGI_TouchScale(o, 96); // 3.78 pixels a millimetre: the snap radius 19, the slop 11
	Tap(o, 158, 115);          // 19
	LOG("5:M120,115 6:L+ 7:L-");
	Tap(o, 160, 115); // 21
	LOG("5:M160,115 6:L+ 7:L-");
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 300, 300);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 312, 300); // beyond the slop: no tap
	Touch(o, 1, OPENBGI_TOUCH_UP, 312, 300);
	Frames(o, 5);
	LOG("");
	OpenBGI_TouchScale(o, 0); // forgotten: the picture's height
	Tap(o, 160, 115);
	LOG("5:M120,115 6:L+ 7:L-");
	OpenBGI_TouchScale(o, -5);
	CHECK(o->input.touch.ppi == 0);
	EngineDown(o);
}

static void TestSlop(void)
{
	OpenBGI_t* o = EngineUp(1);
	printf("slop\n");
	if(!o)
		return;
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 300, 300);
	Frames(o, 2);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 313, 313); // 18.4 away
	Frames(o, 2);
	Touch(o, 1, OPENBGI_TOUCH_UP, 313, 313);
	Frames(o, 4);
	LOG("4:M300,300 5:L+ 6:L-"); // where it went down
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 300, 300);
	Frames(o, 2);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 320, 300); // 20
	Frames(o, 2);
	Touch(o, 1, OPENBGI_TOUCH_UP, 320, 300);
	Frames(o, 4);
	LOG("");
	EngineDown(o);
}

static void TestLong(void)
{
	OpenBGI_t* o = EngineUp(1);
	printf("long\n");
	if(!o)
		return;
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 300, 300);
	Frames(o, 40); // frame 32 is 512 ms after
	LOG("32:M300,300 33:R+ 34:R-");
	Touch(o, 1, OPENBGI_TOUCH_UP, 300, 300);
	Frames(o, 5);
	LOG("");
	CHECK(o->input.keyState[1].counter == 0);
	// moved first: pointing, no long press
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 300, 300);
	Frames(o, 6);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 330, 300);
	Frames(o, 34);
	LOG("19:M330,300"); // 304 ms after it went down
	Touch(o, 1, OPENBGI_TOUCH_UP, 330, 300);
	Frames(o, 5);
	LOG("");
	EngineDown(o);
}

// a finger from (x0, y0) to (x1, y1): down, a frame, a move to the middle, a frame, a move to the end, `wait` frames, up, 3 frames
static void Stroke(OpenBGI_t* o, int x0, int y0, int x1, int y1, int wait)
{
	Touch(o, 1, OPENBGI_TOUCH_DOWN, x0, y0);
	Frames(o, 1);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, (x0 + x1) / 2, (y0 + y1) / 2);
	Frames(o, 1);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, x1, y1);
	Frames(o, wait);
	Touch(o, 1, OPENBGI_TOUCH_UP, x1, y1);
	Frames(o, 3);
}

static void TestSwipe(void)
{
	OpenBGI_t* o = EngineUp(1);
	printf("swipe\n");
	if(!o)
		return;
	Stroke(o, 300, 100, 300, 200, 1); // down: away from the player
	LOG("3:W+");
	CHECK(o->input.keyState[0x0e].counter == 1);
	Stroke(o, 300, 300, 300, 200, 1); // up: towards
	LOG("3:W-");
	Stroke(o, 300, 100, 300, 177, 1); // 77: just long enough
	LOG("3:W+");
	Stroke(o, 300, 100, 300, 176, 1); // too short
	LOG("");
	Stroke(o, 300, 100, 351, 200, 1); // too slanted
	LOG("");
	Stroke(o, 300, 100, 350, 200, 1); // twice as long as wide
	LOG("3:W+");
	Stroke(o, 300, 100, 300, 200, 18); // too slow (up 320 ms after): the pointer followed at 304
	LOG("19:M300,200");
	EngineDown(o);
}

static void TestFollow(void)
{
	OpenBGI_t* o = EngineUp(1);
	printf("follow\n");
	if(!o)
		return;
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 300, 300);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 340, 300);
	Frames(o, 20);
	LOG("19:M340,300");
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 350, 300);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 360, 310);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 370, 320); // the moves of one frame: one step
	Frames(o, 2);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 380, 320);
	Frames(o, 1);
	Frames(o, 2); // standing still: nothing more
	Touch(o, 1, OPENBGI_TOUCH_UP, 380, 320);
	Frames(o, 2);
	LOG("0:M370,320 2:M380,320");
	CHECK(o->input.keyState[1].counter == 0 && !(o->input.keyboard[1] & 0x80));
	EngineDown(o);
}

static void TestTwo(void)
{
	OpenBGI_t* o = EngineUp(1);
	printf("two\n");
	if(!o)
		return;
	// tapped: the right button at the middle
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 200, 200);
	Frames(o, 1);
	Touch(o, 2, OPENBGI_TOUCH_DOWN, 240, 204);
	Frames(o, 3);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 210, 200); // (within the slop: where they went down counts)
	Touch(o, 1, OPENBGI_TOUCH_UP, 210, 200);
	Frames(o, 1);
	Touch(o, 2, OPENBGI_TOUCH_UP, 240, 204);
	Frames(o, 4);
	LOG("4:M220,202 5:R+ 6:R-");
	// held too long
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 200, 200);
	Touch(o, 2, OPENBGI_TOUCH_DOWN, 240, 200);
	Frames(o, 26); // 416 ms
	Touch(o, 2, OPENBGI_TOUCH_UP, 240, 200);
	Touch(o, 1, OPENBGI_TOUCH_UP, 200, 200);
	Frames(o, 4);
	LOG("");
	// the second finger too late: ignored, the first one taps
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 200, 200);
	Frames(o, 13); // 208 ms
	Touch(o, 2, OPENBGI_TOUCH_DOWN, 240, 200);
	Frames(o, 1);
	Touch(o, 2, OPENBGI_TOUCH_UP, 240, 200);
	Touch(o, 1, OPENBGI_TOUCH_UP, 200, 200);
	Frames(o, 4);
	LOG("14:M200,200 15:L+ 16:L-");
	// moved: a notch for every 51 pixels the middle moves
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 200, 100);
	Touch(o, 2, OPENBGI_TOUCH_DOWN, 240, 100);
	Frames(o, 1);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 200, 160); // the middle 130: nothing yet
	Frames(o, 1);
	Touch(o, 2, OPENBGI_TOUCH_MOVE, 240, 160); // 160: one notch
	Frames(o, 2);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 200, 310);
	Touch(o, 2, OPENBGI_TOUCH_MOVE, 240, 310); // 310: three more (151 -> 304)
	Frames(o, 4);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 200, 200);
	Touch(o, 2, OPENBGI_TOUCH_MOVE, 240, 200); // back to 200: two the other way
	Frames(o, 3);
	Touch(o, 1, OPENBGI_TOUCH_UP, 200, 200);
	Touch(o, 2, OPENBGI_TOUCH_UP, 240, 200);
	Frames(o, 3);
	LOG("2:W+ 4:W+ 5:W+ 6:W+ 8:W- 9:W-");
	CHECK(o->input.keyState[0x0e].counter == 4 && o->input.keyState[0x0f].counter == 2);
	EngineDown(o);
}

static void TestKnob(void)
{
	OpenBGI_t* o = EngineUp(1);
	OpenBGI_Target_t t[4];
	uint32_t thumb, knob, s;
	int n;
	printf("knob\n");
	if(!o)
		return;
	MakeBitmap(o, 10, 20, 30);
	MakeBitmap(o, 11, 40, 30);
	thumb = MakeSprite(o, 10, 200, 300, 2);
	CHECK(GfxCall_KnobCreate(o, &knob, thumb) == 0);
	CHECK(GfxCall_KnobSetRange(o, knob, 220, 30) == 0);
	GfxCall_KnobShow(o, knob, 1);
	CHECK(Knob_Register(o, knob));
	// from the thumb
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 205, 310);
	Frames(o, 1);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 300, 312);
	Frames(o, 3);
	n = OpenBGI_Targets(o, t, 4, sizeof(OpenBGI_Target_t));
	CHECK(n == 1 && t[0].kind == OPENBGI_TARGET_KNOB && (t[0].flags & OPENBGI_TARGET_HELD));
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 320, 330);
	Frames(o, 1);
	Touch(o, 1, OPENBGI_TOUCH_UP, 320, 330);
	Frames(o, 3);
	LOG("1:M205,310 2:L+ 3:M300,312 4:M320,330 5:L-");
	n = OpenBGI_Targets(o, t, 4, sizeof(OpenBGI_Target_t));
	CHECK(n == 1 && !(t[0].flags & OPENBGI_TARGET_HELD));
	CHECK(t[0].rect.x == 315 && t[0].rect.y == 300 && t[0].clickX == 325 && t[0].clickY == 315); // taken 5 right of its edge
	// from beside it (16 below the thumb): taken at its click point
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 325, 345);
	Frames(o, 1);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 225, 345);
	Frames(o, 3);
	Touch(o, 1, OPENBGI_TOUCH_UP, 225, 345);
	Frames(o, 2);
	LOG("1:M325,315 2:L+ 3:M225,315 4:L-");
	CHECK(OpenBGI_Targets(o, t, 4, sizeof(OpenBGI_Target_t)) == 1 && t[0].rect.x == 215);
	// from another target, 16 below the thumb: pointing
	s = MakeSprite(o, 11, 200, 340, 1);
	CHECK(Target_Add(o, s));
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 225, 345);
	Frames(o, 1);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 325, 345);
	Frames(o, 2);
	Touch(o, 1, OPENBGI_TOUCH_UP, 325, 345);
	Frames(o, 2);
	LOG("");
	CHECK(OpenBGI_Targets(o, t, 4, sizeof(OpenBGI_Target_t)) == 2 && t[0].kind == OPENBGI_TARGET_KNOB && t[0].rect.x == 215);
	// beside both, nearer the sprite: the knob
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 245, 335);
	Frames(o, 1);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 145, 335);
	Frames(o, 3);
	Touch(o, 1, OPENBGI_TOUCH_UP, 145, 335);
	Frames(o, 2);
	LOG("1:M225,315 2:L+ 3:M125,315 4:L-");
	EngineDown(o);
}

static void TestOther(void)
{
	OpenBGI_t* o = EngineUp(1);
	printf("other\n");
	if(!o)
		return;
	Button(o);
	// cancelled: nothing, and the next tap is one
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 120, 115);
	Frames(o, 2);
	Touch(o, 1, OPENBGI_TOUCH_CANCEL, 120, 115);
	Frames(o, 2);
	LOG("");
	CHECK(o->input.touch.mode == 0 && !o->input.touch.tapValid);
	Tap(o, 120, 115);
	LOG("5:M120,115 6:L+ 7:L-");
	// a third finger: nothing until all have lifted
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 200, 200);
	Touch(o, 2, OPENBGI_TOUCH_DOWN, 240, 200);
	Touch(o, 3, OPENBGI_TOUCH_DOWN, 280, 200);
	Frames(o, 2);
	Touch(o, 3, OPENBGI_TOUCH_UP, 280, 200);
	Touch(o, 2, OPENBGI_TOUCH_UP, 240, 200);
	Frames(o, 2);
	Touch(o, 2, OPENBGI_TOUCH_DOWN, 240, 200); // (while one is still down)
	Touch(o, 2, OPENBGI_TOUCH_UP, 240, 200);
	Touch(o, 1, OPENBGI_TOUCH_MOVE, 200, 400);
	Touch(o, 1, OPENBGI_TOUCH_UP, 200, 400);
	Frames(o, 40);
	LOG("");
	// an unknown contact, a phase that is none: nothing
	Touch(o, 9, OPENBGI_TOUCH_UP, 10, 10);
	Touch(o, 9, OPENBGI_TOUCH_MOVE, 10, 10);
	Touch(o, 1, 7, 10, 10);
	Frames(o, 2);
	LOG("");
	// two taps between two frames: one after the other
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 120, 115);
	Touch(o, 1, OPENBGI_TOUCH_UP, 120, 115);
	Touch(o, 1, OPENBGI_TOUCH_DOWN, 300, 300);
	Touch(o, 1, OPENBGI_TOUCH_UP, 300, 300);
	Frames(o, 8);
	LOG("0:M120,115 1:L+ 2:L- 3:M300,300 4:L+ 5:L-");
	EngineDown(o);
}

// the frame's picture: the bounds of what is not black, 0 for nothing
static int Drawn(const OpenBGI_Frame_t* f, OpenBGI_Rect_t* r)
{
	int x, y, l = f->width, t = f->height, rr = -1, b = -1;
	for(y = 0; y < f->height; y++)
		for(x = 0; x < f->width; x++)
			if(((const uint32_t*)(f->pixels + (size_t)y * f->pitch))[x] & 0xffffff)
			{
				l = x < l ? x : l;
				t = y < t ? y : t;
				rr = x > rr ? x : rr;
				b = y > b ? y : b;
			}
	r->x = l;
	r->y = t;
	r->w = rr - l + 1;
	r->h = b - t + 1;
	return rr >= 0;
}

// `count` frames of the engine (OpenBGI_RunFrame), 16 ms apart
static void RunFrames(OpenBGI_t* o, OpenBGI_Frame_t* f, int count)
{
	while(count-- > 0)
	{
		OpenBGI_RunFrame(o, f);
		gNow += FRAME;
	}
}

static void TestFrame(void)
{
	OpenBGI_t* o;
	OpenBGI_Frame_t f = {.size = sizeof f};
	OpenBGI_Rect_t r;
	uint32_t s, child, id;
	int out[2], kind, answer = -1;
	printf("frame\n");
	gHost = *OS_Host();
	gHost.ticksMs = TicksMs;
	gHost.caps = 0; // the boxes drawn in the frame
	gHost.childWindows = NULL;
	gHost.textEntry = NULL;
	o = OpenBGI_New(&gHost);
	if(!o)
		return;
	Screen_Setup(o, W, H, PM_RGB32);
	Gfx_Create(o, 0xffffffffu, 1);
	Input_ResetLayers(o);
	o->input.inputEnable = 1;
	o->display.windowAlive = 1;
	s = Button(o);
	RunFrames(o, &f, 1);
	CHECK(InputMap_TapPoint(o, 110, 110, 32, out, &kind, &s) == 1 && kind == OPENBGI_TARGET_SPRITE);
	// a child window over all of the sprite (where its frame lies is the system's: the client at 90, 90)
	Child_InitTable(o);
	CHECK(Child_Create(o, &child, "c", 90, 90, 80, 60) == 0 && Child_Show(o, child, 1) == 1);
	RunFrames(o, &f, 1);
	CHECK(Child_Over(o, 110, 110) && Drawn(&f, &r) && r.x <= 100 && r.y <= 100 && r.x + r.w >= 140 && r.y + r.h >= 130);
	CHECK(InputMap_TapPoint(o, 110, 110, 32, out, &kind, &s) == 0 && kind == 0 && out[0] == 110);
	Child_Show(o, child, 0);
	RunFrames(o, &f, 1);
	CHECK(InputMap_TapPoint(o, 110, 110, 32, out, &kind, &s) == 1 && kind == OPENBGI_TARGET_SPRITE);
	// the text entry over the right half of it: a tap there is the entry's, the sprite is clicked on its left half
	CHECK(Edit_Setup(o, 120, 90, 100, 50, 0, 20, 10, 0) == 0);
	RunFrames(o, &f, 1);
	CHECK(Edit_Over(o, 125, 115) && !Edit_Over(o, 119, 115));
	CHECK(InputMap_TapPoint(o, 125, 115, 32, out, &kind, &s) == 1 && kind == OPENBGI_TARGET_ENTRY && out[0] == 125);
	CHECK(InputMap_TapPoint(o, 119, 115, 32, out, &kind, &s) == 1 && kind == OPENBGI_TARGET_SPRITE);
	CHECK(InputMap_TapPoint(o, 125, 150, 32, out, &kind, &s) == 2 && kind == OPENBGI_TARGET_ENTRY && out[1] < 140); // below both: the entry is nearer
	Edit_Show(o, 0);
	RunFrames(o, &f, 1);
	CHECK(InputMap_TapPoint(o, 125, 115, 32, out, &kind, &s) == 1);
	// a box: nothing snaps, the tap is the box's
	id = Request_Box(o, "t", "c", OPENBGI_MB_OK, REQUEST_TREAT_NONE);
	RunFrames(o, &f, 1);
	CHECK(Inframe_Up(o) && Drawn(&f, &r));
	CHECK(InputMap_TapPoint(o, 110, 110, 32, out, &kind, &s) == 0 && kind == 0);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_DOWN, r.x + r.w / 2, r.y + r.h - UI_BORDER - UI_MARGIN - UI_BTN_H / 2); // OK, centred at the bottom
	RunFrames(o, &f, 2);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_UP, r.x + r.w / 2, r.y + r.h - UI_BORDER - UI_MARGIN - UI_BTN_H / 2);
	CHECK(Inframe_Up(o));
	RunFrames(o, &f, 2); // the pointer there, the press
	CHECK(Inframe_Up(o) && !Request_Take(o, id, &answer));
	RunFrames(o, &f, 1); // the release
	CHECK(!Inframe_Up(o) && Request_Take(o, id, &answer) && answer == OPENBGI_IDOK);
	Edit_Destroy(o);
	MsgQueue_Clear(o);
	Knob_UnregisterAll(o);
	Target_Clear(o);
	Input_ResetLayers(o);
	o->display.windowAlive = 0;
	Gfx_Destroy(o);
	OpenBGI_Delete(o);
}

int main(void)
{
	OS_Init();
	TestTap();
	TestSnap();
	TestDisabled();
	TestScale();
	TestSlop();
	TestLong();
	TestSwipe();
	TestFollow();
	TestTwo();
	TestKnob();
	TestOther();
	TestFrame();
	OS_Shutdown();
	if(gFails)
	{
		printf("touch: %d checks failed\n", gFails);
		return 1;
	}
	printf("touch: all tests passed\n");
	return 0;
}
