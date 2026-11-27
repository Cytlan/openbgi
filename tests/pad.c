/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pad.c - tests for the input mapper's pads, actions and on-screen
 *         keyboard (src/sys/pad.c, src/sys/osk.c, src/sys/steps.c; the
 *         focus and the place a screen goes on at of src/sys/inputmap.c):
 *         what OpenBGI_Pad and OpenBGI_Action do to the mouse and the keys
 *         the game knows
 *
 *     make test
 *
 * An engine with its graphics but no window of the host's, as in
 * tests/touch.c: the test says the window is up and gives it a picture of
 * 640 x 480, makes sprites registered as click targets ("90 FA"), layers
 * of the scripts' ("80 18") and a knob, hands over the pad's state and
 * runs frames: each calls InputMap_Frame, as OpenBGI_RunFrame does before
 * a pass, and Knob_Update, as the pass does, moves the host's clock 16 ms
 * on and notes what the mouse did in it - a move of the pointer
 * ("M x,y"), the left or right button pressed or released ("L+", "R-"), a
 * wheel notch away from the player or towards ("W+", "W-") - as
 * "<frame>:<what>".  The keys are counted by their presses.  The groups,
 * in the order they run:
 *
 *   confirm  - CONFIRM with nothing driven: Enter where nothing takes a
 *              click; a click where the pointer is on a screen that goes
 *              on with one, at its click point when the pointer is on
 *              something else; on a screen with only targets the focus
 *              shows first, and the next CONFIRM clicks it
 *   focus    - the directions: the first shows the focus (the target
 *              under the pointer, else the nearest), the next move it to
 *              the target that way, none at an edge; a target off to the
 *              side is taken when none lies ahead; without a target they
 *              are the arrow keys
 *   pick     - which target a direction takes: one within a cone ahead
 *              before a nearer one off to the side; one that overlaps the
 *              focused target across the way as straight ahead; one
 *              outside the cone when nothing else lies that way
 *   repeat   - a direction held moves again after 400 ms and then every
 *              120 ms, not once it is let go
 *   cancel   - CANCEL is the right button while it is held; with a focus
 *              on a screen that goes on with a click it puts the focus
 *              away instead
 *   upkeep   - the focus follows its target when that moves; when it goes
 *              the nearest target gets it, or - on a screen that goes on
 *              with a click - nobody
 *   stick    - the left stick moves the pointer, faster further out, not
 *              within its dead zone; CONFIRM is the left button then; the
 *              right stick turns the wheel
 *   actions  - BACKLOG and FORWARD are wheel notches, SKIP holds the skip
 *              key; OpenBGI_Action does the same as the controls; a
 *              second pad's buttons count too, and a control held on one
 *              pad is not let go by the other
 *   own      - the game's own pad keys ("81 1B"): a control with a key
 *              taps it and does nothing else; the left stick is the
 *              directions when one of those has a key; not with the keys
 *              switched off
 *   knob     - a direction along a focused knob's travel moves its thumb
 *              a sixteenth of the way
 *   device   - a mouse, a key or a finger takes the pad's turn: the focus
 *              goes; the host's pointer reported where the pad put it is
 *              no mouse
 *   settings - the "input" objects of openbgi.json (bgi/inputcfg.h): the
 *              names read and written, the file's and a game's objects
 *              kept when the file is rewritten; applied, a control holds
 *              a key or a mouse button or turns the wheel, the game's own
 *              keys can be switched off, a click waits the tap delay, and
 *              a hotspot is a target while its program is loaded and the
 *              screen has no button of its own
 *   arrow    - through OpenBGI_RunFrame: the pointer drawn over the
 *              picture while a pad drives it on a host that shows none,
 *              not on one that does, not while the game hides the cursor
 *   keyboard - through OpenBGI_RunFrame: the on-screen keyboard comes up
 *              when the text entry gets the keys under a pad, types with
 *              CONFIRM and with the pointer, deletes, turns its tables,
 *              goes with "done" (and Enter), with a real key, and when
 *              the entry loses the keys; a touch screen's tap in the
 *              entry brings it up on a host without a keyboard only; the
 *              entry is a target (ENTRY) the focus can take
 *   dialog   - a dialog drawn in the frame under a pad: the directions
 *              are Tab and the arrows, CONFIRM on a field brings the
 *              keyboard up, on a button presses it; CANCEL is Escape
 *   windows  - a host with dialogs and a text entry of its own: while a
 *              pad has the turn (it was used last, or one is there and
 *              nothing was used yet) the engine draws the dialogs, and a
 *              setup's entry is the engine's; an entry of the host's
 *              that is shown when the pad is taken up moves into the
 *              picture with its text; the next setup under a mouse or a
 *              keyboard is the host's again, told what it missed
 *   display  - the settings key (F9): nothing on a host that does not fit
 *              the window to its screen, where the key is the game's; on
 *              one that does it puts the display settings up and down
 *              again and is never the game's; the pickers step by the
 *              arrows, a click and the wheel, take no text, and what they
 *              say holds at once; "settingsKey" names another key or
 *              none; not over another dialog
 *   restart  - an engine made new for another run (Engine_Renew): the
 *              pads that are there and the touch screen's scale stay,
 *              and the host's services in the engine's own copies; the
 *              device used last and the pointer do not
 */
#include "bgi/core.h"
#include "bgi/engine.h"
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
#include "bgi/json.h"
#include "bgi/version.h"
#include "host/os.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "check.h"

#define W     640
#define H     480
#define FRAME 16 // ms a frame

#define LOW   ((1u << 20) | 0xfffff) // a layer of the scripts' below the sprites: a click anywhere reaches it
#define FULL  32767                  // a stick at the end of its travel

static uint32_t gNow = 1000; // the host's clock (ms)
static uint8_t gPicture[W * H * 4];
static char gLog[2048];         // what the mouse did, frame by frame
static int gFrame;              // the next frame's number
static OpenBGI_PadState_t gPad; // the pad of port 0 as the test holds it
static OpenBGI_Host_t gHost;

static uint32_t TicksMs(void* user)
{
	(void)user;
	return gNow;
}

// ---- the engine ---------------------------------------------------------------------------------

static OpenBGI_t* EngineUp(void)
{
	OpenBGI_t* o;
	gHost = *OS_Host();
	gHost.ticksMs = TicksMs;
	gHost.caps = 0;
	o = OpenBGI_New(&gHost);
	if(!o)
		return NULL;
	Gfx_Create(o, 0xffffffffu, 1);
	Input_ResetLayers(o);
	o->input.inputEnable = 1;
	o->display.windowAlive = 1; // the window is up and shows a picture of W x H
	memset(&o->screen.pic, 0, sizeof o->screen.pic);
	o->screen.pic.pixels = gPicture;
	o->screen.pic.w = W;
	o->screen.pic.h = H;
	o->screen.pic.pitch = W * 4;
	o->screen.pic.bpp = 4;
	o->screen.pic.mode = PM_RGB32;
	memset(&gPad, 0, sizeof gPad);
	gPad.size = sizeof gPad;
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

// bitmap `no` of w x h, opaque grey
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

// a sprite of 40 x 30 at (x, y), registered as a click target: its centre (x + 20, y + 15)
static uint32_t Button(OpenBGI_t* o, int x, int y)
{
	uint32_t s;
	Bmp_t b;
	if(!BmpMgr_GetInfo(o->gfx.bmpMgr, &b, 10) || !b.pixels)
		MakeBitmap(o, 10, 40, 30);
	s = MakeSprite(o, 10, x, y, 3);
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

// the first frame (the sticks' clock starts with it), and a new log
static void Settle(OpenBGI_t* o)
{
	Frames(o, 1);
	gLog[0] = 0;
	gFrame = 0;
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

// how often key `vk` was pressed since the engine came up
static int Presses(OpenBGI_t* o, int vk)
{
	return o->input.keyState[vk].counter;
}

// ---- the pad ------------------------------------------------------------------------------------

// the pad's buttons and directions as given, its sticks as they are
static void Hold(OpenBGI_t* o, uint32_t buttons, uint32_t dpad)
{
	gPad.buttons = buttons;
	gPad.dpad = dpad;
	OpenBGI_Pad(o, 0, &gPad);
}

// a button pressed for two frames, then `after` frames more
static void Press(OpenBGI_t* o, uint32_t button, int after)
{
	Hold(o, button, 0);
	Frames(o, 2);
	Hold(o, 0, 0);
	Frames(o, after);
}

// a direction the same
static void Dir(OpenBGI_t* o, uint32_t dpad, int after)
{
	Hold(o, 0, dpad);
	Frames(o, 2);
	Hold(o, 0, 0);
	Frames(o, after);
}

static void Sticks(OpenBGI_t* o, int lx, int ly, int rx, int ry)
{
	gPad.leftX = (int16_t)lx;
	gPad.leftY = (int16_t)ly;
	gPad.rightX = (int16_t)rx;
	gPad.rightY = (int16_t)ry;
	OpenBGI_Pad(o, 0, &gPad);
}

// the focus is on the target of `owner`
static int Focused(OpenBGI_t* o, uint32_t owner)
{
	return o->input.pad.mode == PAD_MODE_FOCUS && o->input.pad.focusOwner == owner;
}

// ---- the groups ---------------------------------------------------------------------------------

static void TestConfirm(void)
{
	OpenBGI_t* o = EngineUp();
	uint32_t s, mid;
	int at[2];
	printf("confirm\n");
	if(!o)
		return;
	Settle(o);
	// nothing takes a click: Enter
	CHECK(!InputMap_AdvancePoint(o, at));
	Press(o, OPENBGI_PAD_SOUTH, 3);
	LOG("");
	CHECK(Presses(o, 0x0d) == 1 && o->input.device == INPUT_DEV_PAD && o->input.pad.mode == PAD_MODE_ADVANCE);
	// a screen that goes on with a click: one where the pointer is
	MouseLayer_Push(o, LOW);
	OpenBGI_Pointer(o, 300, 200);
	CHECK(InputMap_AdvancePoint(o, at) && at[0] == 300 && at[1] == 200);
	Press(o, OPENBGI_PAD_SOUTH, 3);
	LOG("0:L+ 1:L-");
	CHECK(Presses(o, 0x0d) == 1 && Presses(o, 1) == 1);
	// .. and a button on it under the pointer: the click goes on where one does, not on the button
	s = Button(o, 280, 190);
	CHECK(InputMap_AdvancePoint(o, at) && at[0] == 320 && at[1] == 240);
	Press(o, OPENBGI_PAD_SOUTH, 3);
	LOG("0:M320,240 1:L+ 2:L-");
	CHECK(o->input.pad.mode == PAD_MODE_ADVANCE);
	Press(o, OPENBGI_PAD_SOUTH, 3); // there already
	LOG("0:L+ 1:L-");
	// a script that reads the buttons itself goes on with a click anywhere: where the pointer is, unless that is on a button
	MouseLayer_Pop(o, LOW);
	Input_NoteProbe(o, PROBE_BUTTONS, 1);
	OpenBGI_Pointer(o, 50, 60);
	CHECK(InputMap_AdvancePoint(o, at) && at[0] == 50 && at[1] == 60);
	OpenBGI_Pointer(o, 300, 200);
	CHECK(InputMap_AdvancePoint(o, at) && at[0] == 320 && at[1] == 240);
	mid = Button(o, 300, 230); // .. and not in the middle of the picture when a button lies there: the first place free of them
	CHECK(InputMap_AdvancePoint(o, at) && at[0] == 35 && at[1] == 26);
	GfxCall_SpriteShow(o, mid, 0);
	CHECK(InputMap_AdvancePoint(o, at) && at[0] == 320 && at[1] == 240);
	memset(o->input.probes, 0, sizeof o->input.probes);
	// a screen with only targets: the focus shows first, then it is clicked
	CHECK(!InputMap_AdvancePoint(o, at));
	Press(o, OPENBGI_PAD_SOUTH, 3);
	LOG("0:M300,205");
	CHECK(Focused(o, s) && Presses(o, 1) == 3);
	Press(o, OPENBGI_PAD_SOUTH, 3);
	LOG("0:L+ 1:L-");
	CHECK(Focused(o, s) && Presses(o, 1) == 4 && Presses(o, 0x0d) == 1);
	EngineDown(o);
}

static void TestFocus(void)
{
	OpenBGI_t* o = EngineUp();
	OpenBGI_Target_t t[8];
	uint32_t a, b, c, d, e, a2;
	printf("focus\n");
	if(!o)
		return;
	Settle(o);
	// without a target the directions are the arrow keys
	Dir(o, OPENBGI_DPAD_UP, 1);
	Dir(o, OPENBGI_DPAD_DOWN, 1);
	Dir(o, OPENBGI_DPAD_DOWN, 1);
	Dir(o, OPENBGI_DPAD_LEFT, 1);
	Dir(o, OPENBGI_DPAD_RIGHT, 1);
	LOG("");
	CHECK(Presses(o, 0x26) == 1 && Presses(o, 0x28) == 2 && Presses(o, 0x25) == 1 && Presses(o, 0x27) == 1);
	CHECK(o->input.pad.mode == PAD_MODE_ADVANCE);
	// four in a square, one far off to the right and a little down
	a = Button(o, 100, 100);
	b = Button(o, 200, 100);
	c = Button(o, 100, 200);
	d = Button(o, 200, 200);
	e = Button(o, 500, 240);
	CHECK(InputMap_Focusables(o, t, 8) == 5);
	// the first press only shows the focus: the nearest to the pointer (0, 0)
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	LOG("0:M120,115");
	CHECK(Focused(o, a) && o->input.pad.focusKind == OPENBGI_TARGET_SPRITE);
	Dir(o, OPENBGI_DPAD_RIGHT, 2);
	LOG("0:M220,115");
	CHECK(Focused(o, b));
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	LOG("0:M220,215");
	CHECK(Focused(o, d));
	Dir(o, OPENBGI_DPAD_LEFT, 2);
	LOG("0:M120,215");
	CHECK(Focused(o, c));
	Dir(o, OPENBGI_DPAD_UP, 2);
	LOG("0:M120,115");
	CHECK(Focused(o, a));
	// nothing that way: it stays, and no key is pressed
	Dir(o, OPENBGI_DPAD_UP, 2);
	Dir(o, OPENBGI_DPAD_LEFT, 2);
	LOG("");
	CHECK(Focused(o, a) && Presses(o, 0x26) == 1 && Presses(o, 0x25) == 1);
	// right of b there is only the one off to the side: taken, though not straight ahead
	Dir(o, OPENBGI_DPAD_RIGHT, 2);
	Dir(o, OPENBGI_DPAD_RIGHT, 2);
	LOG("0:M220,115 4:M520,255");
	CHECK(Focused(o, e));
	// back: the one in line with it (d overlaps it across), not the nearer row above
	Dir(o, OPENBGI_DPAD_LEFT, 2);
	LOG("0:M220,215");
	CHECK(Focused(o, d));
	// what a click anywhere reaches is no place for the focus, and two targets in one place are one
	MouseLayer_Push(o, LOW);
	CHECK(OpenBGI_Targets(o, t, 8, sizeof(OpenBGI_Target_t)) == 6 && InputMap_Focusables(o, t, 8) == 5);
	MouseLayer_Pop(o, LOW);
	a2 = Button(o, 100, 100);
	CHECK(OpenBGI_Targets(o, t, 8, sizeof(OpenBGI_Target_t)) == 6 && InputMap_Focusables(o, t, 8) == 5);
	GfxCall_SpriteShow(o, a2, 0);
	// the focus first shows on what the pointer is on
	OpenBGI_Pointer(o, 505, 245);
	CHECK(o->input.pad.mode == PAD_MODE_ADVANCE);
	Dir(o, OPENBGI_DPAD_LEFT, 2);
	LOG("0:M520,255");
	CHECK(Focused(o, e));
	// a target that takes no click is none: with input off the directions are the arrow keys again
	o->input.inputEnable = 0;
	CHECK(InputMap_Focusables(o, t, 8) == 0);
	Dir(o, OPENBGI_DPAD_LEFT, 8);
	LOG("");
	CHECK(o->input.pad.mode == PAD_MODE_ADVANCE && Presses(o, 0x25) == 2);
	o->input.inputEnable = 1;
	EngineDown(o);
}

/* how a direction picks: within a cone of about 60 degrees first, where
 * a target that overlaps the focused one across the way counts as
 * straight ahead; then anything that lies that way at all */
static void TestPick(void)
{
	OpenBGI_t* o = EngineUp();
	uint32_t a, p, q, wide, under, far;
	printf("pick\n");
	if(!o)
		return;
	Settle(o);
	// p right beside a and a little lower, q straight below it: down goes to q, though p is nearer
	a = Button(o, 100, 100);
	p = Button(o, 140, 105);
	q = Button(o, 100, 200);
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	CHECK(Focused(o, a));
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	CHECK(Focused(o, q));
	Dir(o, OPENBGI_DPAD_UP, 2);
	CHECK(Focused(o, a));
	Dir(o, OPENBGI_DPAD_RIGHT, 2);
	CHECK(Focused(o, p));
	Dir(o, OPENBGI_DPAD_DOWN, 2); // from p: q is within the cone
	CHECK(Focused(o, q));
	// below q only one far off to the right (60 down, 300 across): outside the cone, taken all the same
	far = Button(o, 400, 260);
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	CHECK(Focused(o, far));
	GfxCall_SpriteShow(o, a, 0);
	GfxCall_SpriteShow(o, p, 0);
	GfxCall_SpriteShow(o, q, 0);
	GfxCall_SpriteShow(o, far, 0);
	Target_Clear(o);
	// a wide one: what lies under any part of it is straight below - the nearer row, though its middle is far off
	MakeBitmap(o, 12, 300, 30);
	wide = MakeSprite(o, 12, 100, 300, 3);
	CHECK(Target_Add(o, wide));
	under = Button(o, 100, 350); // under its left end
	far = Button(o, 230, 400);   // under its middle, a row further
	OpenBGI_Pointer(o, 250, 310);
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	CHECK(Focused(o, wide));
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	CHECK(Focused(o, under));
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	CHECK(Focused(o, far));
	EngineDown(o);
}

static void TestRepeat(void)
{
	OpenBGI_t* o = EngineUp();
	uint32_t s[6];
	int i;
	printf("repeat\n");
	if(!o)
		return;
	Settle(o);
	for(i = 0; i < 6; i++)
		s[i] = Button(o, 50 + 80 * i, 100);
	Dir(o, OPENBGI_DPAD_RIGHT, 2); // shown
	LOG("0:M70,115");
	Hold(o, 0, OPENBGI_DPAD_RIGHT);
	Frames(o, 25); // 400 ms: the step of the press
	LOG("0:M150,115");
	Frames(o, 1); // .. and the first repeat
	LOG("0:M230,115");
	Frames(o, 7);
	LOG("");
	Frames(o, 1); // 120 ms later the next
	LOG("0:M310,115");
	Hold(o, 0, 0);
	Frames(o, 30);
	LOG("");
	CHECK(Focused(o, s[3]));
	// a direction taken over by another repeats that one
	Hold(o, 0, OPENBGI_DPAD_RIGHT);
	Frames(o, 2);
	Hold(o, 0, OPENBGI_DPAD_RIGHT | OPENBGI_DPAD_LEFT);
	Frames(o, 30);
	Hold(o, 0, 0);
	Frames(o, 2);
	LOG("0:M390,115 2:M310,115 27:M230,115");
	CHECK(Focused(o, s[2]));
	EngineDown(o);
}

static void TestCancel(void)
{
	OpenBGI_t* o = EngineUp();
	uint32_t s;
	printf("cancel\n");
	if(!o)
		return;
	Settle(o);
	// the right button, as long as it is held
	Hold(o, OPENBGI_PAD_EAST, 0);
	Frames(o, 5);
	Hold(o, 0, 0);
	Frames(o, 3);
	LOG("0:R+ 5:R-");
	// START is CANCEL too
	Press(o, OPENBGI_PAD_START, 3);
	LOG("0:R+ 2:R-");
	// a focus on a screen with only targets: still the right button
	s = Button(o, 100, 100);
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	LOG("0:M120,115");
	Press(o, OPENBGI_PAD_EAST, 3);
	LOG("0:R+ 2:R-");
	CHECK(Focused(o, s));
	// .. on one that goes on with a click: the focus is put away, the pointer off its target
	MouseLayer_Push(o, LOW);
	Press(o, OPENBGI_PAD_EAST, 3);
	LOG("0:M320,240");
	CHECK(o->input.pad.mode == PAD_MODE_ADVANCE && Presses(o, 2) == 3);
	Press(o, OPENBGI_PAD_EAST, 3); // .. and then it is the right button again
	LOG("0:R+ 2:R-");
	MouseLayer_Pop(o, LOW);
	EngineDown(o);
}

static void TestUpkeep(void)
{
	OpenBGI_t* o = EngineUp();
	uint32_t a, b;
	printf("upkeep\n");
	if(!o)
		return;
	Settle(o);
	a = Button(o, 100, 100);
	b = Button(o, 300, 100);
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	LOG("0:M120,115");
	// the target moves: the pointer with it, within a tenth of a second
	GfxCall_SpriteSet(o, a, 100, 160, 10, 0, 0, 3);
	Frames(o, 8);
	CHECK(Focused(o, a) && o->input.pointerX == 120 && o->input.pointerY == 175);
	gLog[0] = 0;
	gFrame = 0;
	// it goes: the nearest gets the focus
	GfxCall_SpriteShow(o, a, 0);
	Frames(o, 8);
	CHECK(Focused(o, b) && o->input.pointerX == 320 && o->input.pointerY == 115);
	// CONFIRM right after the target went: the focus shows where it is now, and is not clicked yet
	GfxCall_SpriteShow(o, a, 1);
	GfxCall_SpriteShow(o, b, 0);
	gLog[0] = 0;
	gFrame = 0;
	Press(o, OPENBGI_PAD_SOUTH, 3);
	LOG("0:M120,175");
	CHECK(Focused(o, a));
	GfxCall_SpriteShow(o, b, 1);
	GfxCall_SpriteShow(o, a, 0);
	Frames(o, 8);
	CHECK(Focused(o, b));
	// on a screen that goes on with a click nobody does: CONFIRM goes on again
	MouseLayer_Push(o, LOW);
	GfxCall_SpriteShow(o, a, 1);
	GfxCall_SpriteShow(o, b, 0);
	Frames(o, 8);
	CHECK(o->input.pad.mode == PAD_MODE_ADVANCE);
	gLog[0] = 0;
	gFrame = 0;
	Press(o, OPENBGI_PAD_SOUTH, 3);
	LOG("0:L+ 1:L-");
	MouseLayer_Pop(o, LOW);
	EngineDown(o);
}

static void TestStick(void)
{
	OpenBGI_t* o = EngineUp();
	int x;
	printf("stick\n");
	if(!o)
		return;
	OpenBGI_Pointer(o, 100, 100);
	Settle(o);
	// within the dead zone (a fifth of the travel): nothing, and the mouse keeps its turn
	Sticks(o, 6000, -6000, 0, 6000);
	Frames(o, 10);
	LOG("");
	CHECK(o->input.device == INPUT_DEV_MOUSE);
	// at its end: the picture's height in 1.1 s, 7 pixels a frame of 16 ms
	Sticks(o, FULL, 0, 0, 0);
	Frames(o, 10);
	CHECK(o->input.device == INPUT_DEV_PAD && o->input.pad.mode == PAD_MODE_POINTER);
	CHECK(o->input.pointerX >= 168 && o->input.pointerX <= 171 && o->input.pointerY == 100);
	CHECK(Presses(o, 0x27) == 0); // (the stick is no direction)
	// a frame that took a second is not a second's way: 50 ms of it
	x = o->input.pointerX;
	gNow += 1000;
	Frames(o, 1);
	CHECK(o->input.pointerX >= x + 20 && o->input.pointerX <= x + 23);
	OpenBGI_Pointer(o, x, 100);
	Frames(o, 1);
	// half way out: (0.5 - 0.2) / 0.8 of the travel, squared - 0.14 of that
	x = o->input.pointerX;
	Sticks(o, 0, FULL / 2, 0, 0);
	Frames(o, 30);
	CHECK(o->input.pointerX == x && o->input.pointerY >= 128 && o->input.pointerY <= 131);
	// it stops at the picture's edge
	Sticks(o, -FULL, -FULL, 0, 0);
	Frames(o, 40);
	CHECK(o->input.pointerX == 0 && o->input.pointerY == 0);
	Sticks(o, 0, 0, 0, 0);
	Frames(o, 5);
	gLog[0] = 0;
	gFrame = 0;
	// CONFIRM is the left button there, as long as it is held
	Hold(o, OPENBGI_PAD_SOUTH, 0);
	Frames(o, 6);
	Hold(o, 0, 0);
	Frames(o, 3);
	LOG("0:L+ 6:L-");
	// the right stick: eight notches a second at its end, up away from the player
	Sticks(o, 0, 0, 0, -FULL);
	Frames(o, 32);
	Sticks(o, 0, 0, 0, 0);
	Frames(o, 4);
	CHECK(Presses(o, 0x0e) == 4 && Presses(o, 0x0f) == 0);
	Sticks(o, 0, 0, 0, FULL);
	Frames(o, 16);
	Sticks(o, 0, 0, 0, 0);
	Frames(o, 4);
	CHECK(Presses(o, 0x0e) == 4 && Presses(o, 0x0f) == 2);
	EngineDown(o);
}

static void TestActions(void)
{
	OpenBGI_t* o = EngineUp();
	OpenBGI_PadState_t two;
	printf("actions\n");
	if(!o)
		return;
	Settle(o);
	Press(o, OPENBGI_PAD_L1, 2);
	Press(o, OPENBGI_PAD_R1, 2);
	LOG("0:W+ 4:W-");
	// SKIP holds the skip key (Ctrl) at once
	Hold(o, OPENBGI_PAD_R2, 0);
	CHECK(o->input.keyboard[0x11] & 0x80);
	Frames(o, 3);
	CHECK(o->input.keyboard[0x11] & 0x80);
	Hold(o, 0, 0);
	CHECK(!(o->input.keyboard[0x11] & 0x80));
	// .. the key the game made its skip key
	o->input.skipKeys[0] = 'S';
	Hold(o, OPENBGI_PAD_R2, 0);
	CHECK((o->input.keyboard['S'] & 0x80) && !(o->input.keyboard[0x11] & 0x80));
	Hold(o, 0, 0);
	CHECK(!(o->input.keyboard['S'] & 0x80));
	o->input.skipKeys[0] = 0x11;
	// buttons nothing is bound to do nothing
	Press(o, OPENBGI_PAD_WEST | OPENBGI_PAD_SELECT | OPENBGI_PAD_L3 | OPENBGI_PAD_R3 | OPENBGI_PAD_L2 | OPENBGI_PAD_GUIDE, 2);
	LOG("");
	// the actions themselves
	OpenBGI_Action(o, OPENBGI_ACTION_BACKLOG, 1);
	OpenBGI_Action(o, OPENBGI_ACTION_BACKLOG, 0);
	OpenBGI_Action(o, OPENBGI_ACTION_CANCEL, 1);
	Frames(o, 3);
	OpenBGI_Action(o, OPENBGI_ACTION_CANCEL, 0);
	OpenBGI_Action(o, OPENBGI_ACTION_SKIP, 1);
	CHECK(o->input.keyboard[0x11] & 0x80);
	OpenBGI_Action(o, OPENBGI_ACTION_SKIP, 0);
	CHECK(!(o->input.keyboard[0x11] & 0x80));
	OpenBGI_Action(o, OPENBGI_ACTION_CONFIRM, 1);
	OpenBGI_Action(o, OPENBGI_ACTION_CONFIRM, 0);
	OpenBGI_Action(o, 0, 1);
	OpenBGI_Action(o, 99, 1);
	Frames(o, 3);
	LOG("0:W+ 1:R+ 3:R-");
	CHECK(Presses(o, 0x0d) == 1);
	// SKIP, BACKLOG and FORWARD leave a mouse its turn; the others are the pad's
	OpenBGI_Pointer(o, 50, 50);
	OpenBGI_Action(o, OPENBGI_ACTION_FORWARD, 1);
	OpenBGI_Action(o, OPENBGI_ACTION_FORWARD, 0);
	CHECK(o->input.device == INPUT_DEV_MOUSE);
	OpenBGI_Action(o, OPENBGI_ACTION_UP, 1);
	OpenBGI_Action(o, OPENBGI_ACTION_UP, 0);
	CHECK(o->input.device == INPUT_DEV_PAD);
	Frames(o, 2);
	LOG("0:W-");
	// two pads are one: a button held on both is let go when both let it go
	memset(&two, 0, sizeof two);
	two.size = sizeof two;
	two.buttons = OPENBGI_PAD_EAST;
	Hold(o, OPENBGI_PAD_EAST, 0);
	Frames(o, 2);
	OpenBGI_Pad(o, 1, &two);
	Frames(o, 2);
	Hold(o, 0, 0);
	Frames(o, 2);
	OpenBGI_Pad(o, 1, NULL); // (the pad is gone)
	Frames(o, 2);
	LOG("0:R+ 6:R-");
	OpenBGI_Pad(o, 4, &two); // no such port
	OpenBGI_Pad(o, -1, &two);
	Frames(o, 2);
	LOG("");
	// a state of a host's older header, which ends after the buttons and the directions: the sticks are at rest
	{
		size_t cut = offsetof(OpenBGI_PadState_t, leftX);
		void* part = malloc(cut); // (of just that size: the sanitizer sees a read behind it)
		two.leftX = 30000;
		two.rightY = -30000;
		two.size = (uint32_t)cut;
		memcpy(part, &two, cut);
		two.size = sizeof two;
		OpenBGI_Pad(o, 1, (const OpenBGI_PadState_t*)part);
		free(part);
		CHECK(o->input.pad.port[1].size == sizeof two && o->input.pad.port[1].buttons == OPENBGI_PAD_EAST);
		CHECK(o->input.pad.port[1].leftX == 0 && o->input.pad.port[1].rightY == 0 && o->input.pad.ly == 0);
		// the whole one has them; one without its size is a pad at rest
		OpenBGI_Pad(o, 1, &two);
		CHECK(o->input.pad.port[1].leftX == 30000 && o->input.pad.lx == 30000 && o->input.pad.ry == -30000);
		two.size = 0;
		OpenBGI_Pad(o, 1, &two);
		CHECK(o->input.pad.port[1].buttons == 0 && o->input.pad.lx == 0 && (o->input.pad.attached & 2));
		OpenBGI_Pad(o, 1, NULL);
		Frames(o, 2);
	}
	EngineDown(o);
}

static void TestOwn(void)
{
	OpenBGI_t* o = EngineUp();
	uint32_t s;
	printf("own\n");
	if(!o)
		return;
	Settle(o);
	s = Button(o, 100, 100);
	// the bottom button with a key of the game's: the key, tapped as it goes down; nothing else
	CHECK(Pad_SetKey(o, 0, 'Z') == 0);
	Press(o, OPENBGI_PAD_SOUTH, 3);
	LOG("");
	CHECK(Presses(o, 'Z') == 1 && !(o->input.keyboard['Z'] & 0x80) && o->input.pad.mode == PAD_MODE_ADVANCE);
	Press(o, OPENBGI_PAD_EAST, 3); // the others as before
	LOG("0:R+ 2:R-");
	// a direction with a key: the D-pad taps it, and so does the left stick past a quarter of its travel
	CHECK(Pad_SetKey(o, 33, 'S') == 0);
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	CHECK(Presses(o, 'S') == 1);
	Sticks(o, 0, FULL / 2, 0, 0);
	Frames(o, 20);
	Sticks(o, 0, 0, 0, 0);
	Frames(o, 2);
	LOG("");
	CHECK(Presses(o, 'S') == 2 && o->input.pad.mode == PAD_MODE_ADVANCE);
	Sticks(o, FULL, 0, 0, 0); // a direction without a key: the focus, as the D-pad's
	Frames(o, 2);
	Sticks(o, 0, 0, 0, 0);
	Frames(o, 2);
	LOG("0:M120,115");
	CHECK(Focused(o, s));
	// the keys switched off: OpenBGI's own again
	o->input.pad.ownKeysOff = 1;
	Press(o, OPENBGI_PAD_SOUTH, 3);
	LOG("0:L+ 1:L-");
	CHECK(Presses(o, 'Z') == 1);
	Sticks(o, 0, FULL, 0, 0);
	Frames(o, 5);
	Sticks(o, 0, 0, 0, 0);
	CHECK(Presses(o, 'S') == 2 && o->input.pad.mode == PAD_MODE_POINTER);
	EngineDown(o);
}

static void TestKnob(void)
{
	OpenBGI_t* o = EngineUp();
	OpenBGI_Target_t t[4];
	uint32_t thumb, knob;
	printf("knob\n");
	if(!o)
		return;
	Settle(o);
	MakeBitmap(o, 11, 20, 30);
	thumb = MakeSprite(o, 11, 200, 300, 2);
	CHECK(GfxCall_KnobCreate(o, &knob, thumb) == 0);
	CHECK(GfxCall_KnobSetRange(o, knob, 220, 30) == 0);
	GfxCall_KnobShow(o, knob, 1);
	CHECK(Knob_Register(o, knob));
	CHECK(OpenBGI_Targets(o, t, 4, sizeof(OpenBGI_Target_t)) == 1 && t[0].kind == OPENBGI_TARGET_KNOB && t[0].drag.w == 220);
	// the script tests the pointer against the thumb as well ("90 3D"): two targets in one place, one for the focus
	Input_NoteProbe(o, PROBE_OBJECT, thumb);
	CHECK(OpenBGI_Targets(o, t, 4, sizeof(OpenBGI_Target_t)) == 2 && InputMap_Focusables(o, t, 4) == 1 && t[0].kind == OPENBGI_TARGET_KNOB);
	memset(o->input.probes, 0, sizeof o->input.probes);
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	LOG("0:M210,315");
	CHECK(o->input.pad.mode == PAD_MODE_FOCUS && o->input.pad.focusKind == OPENBGI_TARGET_KNOB);
	// along its travel: the thumb taken, moved a sixteenth of the way (13 pixels), let go
	Dir(o, OPENBGI_DPAD_RIGHT, 4);
	LOG("0:L+ 1:M223,315 2:L-");
	CHECK(OpenBGI_Targets(o, t, 4, sizeof(OpenBGI_Target_t)) == 1 && t[0].rect.x == 213 && !(t[0].flags & OPENBGI_TARGET_HELD));
	Frames(o, 8); // the focus stays on it where it is now
	LOG("");
	CHECK(o->input.pad.mode == PAD_MODE_FOCUS && o->input.pointerX == 223);
	Dir(o, OPENBGI_DPAD_LEFT, 4);
	LOG("0:L+ 1:M210,315 2:L-");
	CHECK(OpenBGI_Targets(o, t, 4, sizeof(OpenBGI_Target_t)) == 1 && t[0].rect.x == 200);
	// across it: not the knob's, and nothing lies that way
	Dir(o, OPENBGI_DPAD_UP, 4);
	LOG("");
	CHECK(o->input.pad.mode == PAD_MODE_FOCUS && t[0].rect.x == 200);
	EngineDown(o);
}

static void TestDevice(void)
{
	OpenBGI_t* o = EngineUp();
	uint32_t s;
	printf("device\n");
	if(!o)
		return;
	Settle(o);
	CHECK(o->input.device == INPUT_DEV_NONE && !Pad_OwnsPointer(o));
	s = Button(o, 100, 100);
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	LOG("0:M120,115");
	CHECK(o->input.device == INPUT_DEV_PAD && Pad_OwnsPointer(o) && Focused(o, s));
	// the host's pointer where the pad put it (a system that rounds differently): no mouse
	OpenBGI_Pointer(o, 122, 113);
	CHECK(o->input.device == INPUT_DEV_PAD && Focused(o, s) && o->input.pointerX == 120 && o->input.pointerY == 115);
	// further off: the mouse's turn, the focus goes
	OpenBGI_Pointer(o, 124, 115);
	CHECK(o->input.device == INPUT_DEV_MOUSE && o->input.pad.mode == PAD_MODE_ADVANCE && o->input.pointerX == 124);
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	CHECK(Focused(o, s));
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 1, 120, 115, 0); // a button where it is
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 0, 120, 115, 0);
	CHECK(o->input.device == INPUT_DEV_MOUSE && o->input.pad.mode == PAD_MODE_ADVANCE);
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	OpenBGI_Wheel(o, 120);
	CHECK(o->input.device == INPUT_DEV_MOUSE && o->input.pad.mode == PAD_MODE_ADVANCE);
	// a key
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	CHECK(Focused(o, s));
	OpenBGI_Key(o, 'A', 0, 0); // (a release is none)
	CHECK(o->input.device == INPUT_DEV_PAD);
	OpenBGI_Key(o, 'A', 1, 0);
	CHECK(o->input.device == INPUT_DEV_KEYBOARD && o->input.pad.mode == PAD_MODE_ADVANCE);
	// a finger
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	CHECK(Focused(o, s));
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_DOWN, 300, 300);
	CHECK(o->input.device == INPUT_DEV_TOUCH && o->input.pad.mode == PAD_MODE_ADVANCE);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_CANCEL, 300, 300);
	// the mapper's own clicks are nobody's turn
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	Press(o, OPENBGI_PAD_SOUTH, 4);
	CHECK(o->input.device == INPUT_DEV_PAD && Focused(o, s));
	EngineDown(o);
}

// ---- the settings of openbgi.json -----------------------------------------------------------------

// the "input" object of `text` ({...}), NULL when it says nothing
static InputSettings_t* Parsed(const char* text)
{
	size_t err;
	JsonNode_t* root = Json_Parse(text, strlen(text), &err);
	InputSettings_t* s = InputSettings_Parse(root, "test");
	Json_Free(root);
	return s;
}

static void TestSettings(void)
{
	static const char kFile[] = "openbgi_test_input.json";
	static const char kJson[] =
		"{ \"input\": { \"pad\": { \"west\": \"key:A\", \"l2\": \"mouse:middle\" }, \"tapDelay\": 40 },\n"
		"  \"games\": [ { \"product\": \"TestGame\", \"profile\": \"1.58\",\n"
		"     \"input\": { \"pad\": { \"west\": \"none\", \"select\": \"wheel:down\" }, \"ownPadKeys\": false,\n"
		"                \"hotspots\": [ { \"name\": \"door \\\"A\\\"\", \"rect\": [10, 20, 30, 40], \"program\": \"Map\" } ] } },\n"
		"   { \"product\": \"Other\", \"profile\": \"1.58\" } ] }\n";
	OpenBGI_t* o = EngineUp();
	InputSettings_t *a, *b;
	const InputSettings_t* g;
	OpenBGI_Target_t t[8];
	Thread_t* thread;
	ModuleNode_t* m;
	EngineGame_t game;
	uint16_t bind;
	char name[16], text[32], *json;
	uint32_t s;
	FILE* f;
	int i, n, entry = -1, at[2], kind;
	printf("settings\n");
	if(!o)
		return;
	Settle(o);
	// the names: every control and every binding reads back as it is written
	for(i = 0; i < INPUT_CONTROLS; i++)
		CHECK(InputControl_ByName(InputControl_Name(i, name)) == i);
	CHECK(InputControl_ByName("south") == 0 && InputControl_ByName("guide") == 12 && InputControl_ByName("button13") == 13 &&
		InputControl_ByName("button0") == 0 && InputControl_ByName("right") == 35);
	CHECK(InputControl_ByName("button32") < 0 && InputControl_ByName("button") < 0 && InputControl_ByName("South") < 0 && InputControl_ByName("") < 0);
	CHECK(InputBind_Parse("confirm", &bind) && bind == OPENBGI_ACTION_CONFIRM && InputBind_Parse("keyboard", &bind) && bind == OPENBGI_ACTION_KEYBOARD);
	CHECK(InputBind_Parse("none", &bind) && bind == 0);
	CHECK(InputBind_Parse("key:RETURN", &bind) && bind == (INPUT_BIND_KEY | 0x0d) && InputBind_Parse("key:a", &bind) && bind == (INPUT_BIND_KEY | 'A'));
	CHECK(InputBind_Parse("key:F12", &bind) && bind == (INPUT_BIND_KEY | 0x7b) && InputBind_Parse("key:0x5B", &bind) && bind == (INPUT_BIND_KEY | 0x5b));
	CHECK(InputBind_Parse("key:ctrl", &bind) && bind == (INPUT_BIND_KEY | 0x11) && InputBind_Parse("key:7", &bind) && bind == (INPUT_BIND_KEY | '7'));
	CHECK(InputBind_Parse("mouse:right", &bind) && bind == (INPUT_BIND_MOUSE | OPENBGI_BUTTON_RIGHT));
	CHECK(InputBind_Parse("wheel:up", &bind) && bind == INPUT_BIND_WHEEL && InputBind_Parse("wheel:down", &bind) && bind == (INPUT_BIND_WHEEL | 1));
	CHECK(!InputBind_Parse("key:", &bind) && !InputBind_Parse("key:F13", &bind) && !InputBind_Parse("key:0x100", &bind) && !InputBind_Parse("key:0x", &bind));
	CHECK(!InputBind_Parse("mouse:x", &bind) && !InputBind_Parse("wheel:", &bind) && !InputBind_Parse("Confirm", &bind) && !InputBind_Parse("", &bind));
	for(i = 0; i < 0x400; i++)
	{ // what is written reads back the same
		int known = i <= 10 || (i > INPUT_BIND_KEY && i < INPUT_BIND_KEY + 256) || (i >= INPUT_BIND_MOUSE && i < INPUT_BIND_MOUSE + 3) ||
			i == INPUT_BIND_WHEEL || i == INPUT_BIND_WHEEL + 1;
		InputBind_Format((uint16_t)i, text, sizeof text);
		if(known)
			CHECK(InputBind_Parse(text, &bind) && bind == i);
	}
	// an object: what it says, and nothing where it says nothing that is understood
	CHECK(Parsed("{}") == NULL && Parsed("[]") == NULL && Parsed("{\"pad\": {\"nose\": \"confirm\", \"south\": \"sing\"}}") == NULL);
	CHECK(Parsed("{\"tapDelay\": -1}") == NULL && Parsed("{\"tapDelay\": 2001}") == NULL && Parsed("{\"hotspots\": [{\"rect\": [1, 2, 0, 4]}]}") == NULL);
	a = Parsed("{\"pad\": {\"south\": \"cancel\", \"east\": \"confirm\", \"button20\": \"key:F5\", \"up\": \"wheel:up\"}, \"ownPadKeys\": true,"
			   " \"tapDelay\": 0, \"hotspots\": [{\"rect\": [1, 2, 3, 4]}, {\"name\": \"x\", \"rect\": [5, 6, 7, 8], \"program\": \"p._bp\"}, {\"rect\": [0, 0]}]}");
	CHECK(a != NULL);
	if(a)
	{
		CHECK(a->bound[0] && a->bind[0] == OPENBGI_ACTION_CANCEL && a->bound[1] && a->bind[1] == OPENBGI_ACTION_CONFIRM && !a->bound[2]);
		CHECK(a->bound[20] && a->bind[20] == (INPUT_BIND_KEY | 0x74) && a->bound[32] && a->bind[32] == INPUT_BIND_WHEEL);
		CHECK(a->ownPadKeys == 1 && a->tapDelay == 0 && a->hotspotCount == 2);
		CHECK(a->hotspots[0].x == 1 && a->hotspots[0].h == 4 && !a->hotspots[0].name[0] && !a->hotspots[0].program[0]);
		CHECK(a->hotspots[1].w == 7 && strcmp(a->hotspots[1].name, "x") == 0 && strcmp(a->hotspots[1].program, "p._bp") == 0);
		json = InputSettings_Format(a);
		CHECK(json && strcmp(json, "{\"pad\": {\"south\": \"cancel\", \"east\": \"confirm\", \"button20\": \"key:F5\", \"up\": \"wheel:up\"}, "
								   "\"ownPadKeys\": true, \"tapDelay\": 0, \"hotspots\": [{\"rect\": [1, 2, 3, 4]}, "
								   "{\"name\": \"x\", \"rect\": [5, 6, 7, 8], \"program\": \"p._bp\"}]}") == 0);
		b = json ? Parsed(json) : NULL;
		CHECK(b && memcmp(a->bind, b->bind, sizeof a->bind) == 0 && memcmp(a->bound, b->bound, sizeof a->bound) == 0 && b->hotspotCount == 2 &&
			memcmp(a->hotspots, b->hotspots, 2 * sizeof *a->hotspots) == 0);
		InputSettings_Free(b);
		free(json);
	}
	InputSettings_Free(a);
	// the pointer, its size, the stick's speed and the keyboard's switch
	CHECK(Parsed("{\"pointer\": \"sometimes\"}") == NULL && Parsed("{\"pointer\": 1}") == NULL && Parsed("{\"pointerSize\": \"huge\"}") == NULL);
	CHECK(Parsed("{\"stickSpeed\": 24}") == NULL && Parsed("{\"stickSpeed\": 401}") == NULL && Parsed("{\"onScreenKeyboard\": 0}") == NULL);
	a = Parsed("{\"pointer\": \"always\", \"pointerSize\": \"large\", \"stickSpeed\": 150, \"onScreenKeyboard\": false}");
	CHECK(a != NULL);
	if(a)
	{
		CHECK(a->pointer == INPUT_POINTER_ALWAYS && a->pointerSize == INPUT_POINTER_LARGE && a->stickSpeed == 150 && a->onScreenKeyboard == 0);
		CHECK(a->ownPadKeys == -1 && a->tapDelay == -1 && a->hotspotCount == 0);
		json = InputSettings_Format(a);
		CHECK(json && strcmp(json, "{\"pointer\": \"always\", \"pointerSize\": \"large\", \"stickSpeed\": 150, \"onScreenKeyboard\": false}") == 0);
		free(json);
	}
	InputSettings_Free(a);
	// the settings key: a key by its name, or none
	CHECK(Parsed("{\"settingsKey\": \"\"}") == NULL && Parsed("{\"settingsKey\": 120}") == NULL && Parsed("{\"settingsKey\": \"confirm\"}") == NULL);
	a = Parsed("{\"settingsKey\": \"f8\"}");
	CHECK(a && a->settingsKey == 0x77 && a->pointer == -1);
	if(a)
	{
		json = InputSettings_Format(a);
		CHECK(json && strcmp(json, "{\"settingsKey\": \"F8\"}") == 0);
		free(json);
	}
	InputSettings_Free(a);
	a = Parsed("{\"settingsKey\": \"key:Home\"}");
	CHECK(a && a->settingsKey == 0x24);
	InputSettings_Free(a);
	a = Parsed("{\"settingsKey\": \"none\"}");
	CHECK(a && a->settingsKey == 0);
	if(a)
	{
		json = InputSettings_Format(a);
		CHECK(json && strcmp(json, "{\"settingsKey\": \"none\"}") == 0);
		free(json);
	}
	InputSettings_Free(a);
	a = Parsed("{\"pointer\": \"never\", \"pointerSize\": \"small\", \"onScreenKeyboard\": true}");
	CHECK(a && a->pointer == INPUT_POINTER_NEVER && a->pointerSize == INPUT_POINTER_SMALL && a->stickSpeed == -1 && a->onScreenKeyboard == 1);
	InputSettings_Free(a);
	a = Parsed("{\"pointer\": \"pad\", \"pointerSize\": \"normal\"}");
	CHECK(a && a->pointer == INPUT_POINTER_PAD && a->pointerSize == INPUT_POINTER_NORMAL);
	InputSettings_Free(a);
	// the file: the object at the top for every game, an entry's for that game; both survive the file's rewriting
	f = fopen(kFile, "wb");
	CHECK(f != NULL);
	if(f)
	{
		fwrite(kJson, 1, sizeof kJson - 1, f);
		fclose(f);
	}
	n = Engine_LoadGamesJson(o, kFile);
	CHECK(n >= 2 && Engine_InputAll(o) != NULL && Engine_InputAll(o)->tapDelay == 40 && Engine_InputAll(o)->hotspotCount == 0);
	for(i = 0; i < n; i++)
		if(Engine_GameAt(o, i, &game) && game.product && strcmp(game.product, "TestGame") == 0)
			entry = i;
	g = Engine_GameInput(o, entry);
	CHECK(entry >= 0 && g != NULL && g->ownPadKeys == 0 && g->tapDelay == -1 && g->hotspotCount == 1 && g->bound[2] && g->bind[2] == 0);
	CHECK(g && strcmp(g->hotspots[0].name, "door \"A\"") == 0 && Engine_GameInput(o, entry + 1) == NULL && Engine_GameInput(o, -1) == NULL &&
		Engine_GameInput(o, n) == NULL);
	json = Engine_FormatGamesJson(o, NULL);
	CHECK(json && strstr(json, "\t\"input\": {\"pad\": {\"west\": \"key:A\", \"l2\": \"mouse:middle\"}, \"tapDelay\": 40},\n\t\"profiles\"") != NULL);
	CHECK(json && strstr(json, "\"profile\": \"1.58\", \"input\": {\"pad\": {\"west\": \"none\", \"select\": \"wheel:down\"}, \"ownPadKeys\": false, "
							   "\"hotspots\": [{\"name\": \"door \\\"A\\\"\", \"rect\": [10, 20, 30, 40], \"program\": \"Map\"}]} }") != NULL);
	free(json);
	CHECK(Engine_SaveGamesJson(o, kFile) && Engine_LoadGamesJson(o, kFile) == n && Engine_InputAll(o) && Engine_InputAll(o)->tapDelay == 40);
	g = Engine_GameInput(o, entry);
	CHECK(g != NULL && g->hotspotCount == 1 && g->hotspots[0].w == 30 && strcmp(g->hotspots[0].program, "Map") == 0);
	remove(kFile);
	// applied: the built-in bindings, the file's over them, the game's over those
	CHECK(o->input.pad.bind[2] == 0 && o->input.pad.bind[10] == 0 && o->input.steps.tapDelay == 0 && !o->input.pad.ownKeysOff);
	InputMap_Configure(o, Engine_InputAll(o), NULL);
	CHECK(o->input.pad.bind[2] == (INPUT_BIND_KEY | 'A') && o->input.pad.bind[10] == (INPUT_BIND_MOUSE | OPENBGI_BUTTON_MIDDLE));
	CHECK(o->input.pad.bind[0] == OPENBGI_ACTION_CONFIRM && o->input.steps.tapDelay == 40 && !o->input.pad.ownKeysOff && o->input.hotspotCount == 0);
	InputMap_Configure(o, Engine_InputAll(o), g);
	CHECK(o->input.pad.bind[2] == 0 && o->input.pad.bind[6] == (INPUT_BIND_WHEEL | 1) && o->input.pad.bind[10] == (INPUT_BIND_MOUSE | OPENBGI_BUTTON_MIDDLE));
	CHECK(o->input.steps.tapDelay == 40 && o->input.pad.ownKeysOff && o->input.hotspotCount == 1);
	// the host's settings over both: what they name wins, the rest stays; a new set replaces the one before
	CHECK(o->input.pad.arrowFor == INPUT_POINTER_PAD && o->input.pad.arrowSize == INPUT_POINTER_NORMAL && o->input.pad.stickSpeed == 100 && !o->input.oskAsked);
	CHECK(OpenBGI_InputSettings(o, "{\"pad\": {\"west\": \"skip\", \"north\": \"none\"}, \"tapDelay\": 70, \"pointer\": \"always\", \"stickSpeed\": 200}") == 1);
	CHECK(o->input.pad.bind[2] == OPENBGI_ACTION_SKIP && o->input.pad.bind[3] == 0 && o->input.pad.bind[6] == (INPUT_BIND_WHEEL | 1));
	CHECK(o->input.steps.tapDelay == 70 && o->input.pad.ownKeysOff && o->input.hotspotCount == 1);
	CHECK(o->input.pad.arrowFor == INPUT_POINTER_ALWAYS && o->input.pad.stickSpeed == 200 && o->input.pad.arrowSize == INPUT_POINTER_NORMAL);
	CHECK(OpenBGI_InputSettings(o, "{\"pointerSize\": \"small\", \"onScreenKeyboard\": false, \"ownPadKeys\": true}") == 1);
	CHECK(o->input.pad.bind[2] == 0 && o->input.pad.bind[3] == OPENBGI_ACTION_KEYBOARD && o->input.steps.tapDelay == 40 && !o->input.pad.ownKeysOff);
	CHECK(o->input.pad.arrowFor == INPUT_POINTER_PAD && o->input.pad.stickSpeed == 100 && o->input.pad.arrowSize == INPUT_POINTER_SMALL && o->input.oskAsked);
	// what is no object changes nothing; they hold over a new set-up of the file's, and go with NULL or ""
	CHECK(OpenBGI_InputSettings(o, "[1, 2]") == 0 && OpenBGI_InputSettings(o, "{\"pointer\": ") == 0 && o->input.pad.arrowSize == INPUT_POINTER_SMALL);
	InputMap_Configure(o, Engine_InputAll(o), NULL);
	CHECK(o->input.pad.arrowSize == INPUT_POINTER_SMALL && o->input.oskAsked && o->input.pad.bind[2] == (INPUT_BIND_KEY | 'A') && o->input.hotspotCount == 0);
	CHECK(OpenBGI_InputSettings(o, "") == 1 && o->input.pad.arrowSize == INPUT_POINTER_NORMAL && !o->input.oskAsked && o->input.steps.tapDelay == 40);
	CHECK(OpenBGI_InputSettings(o, "{\"pointerSize\": \"large\"}") == 1 && o->input.pad.arrowSize == INPUT_POINTER_LARGE);
	CHECK(OpenBGI_InputSettings(o, NULL) == 1 && o->input.pad.arrowSize == INPUT_POINTER_NORMAL && o->input.cfgHost == NULL);
	// its "defaults" lie under the file's: the file's west and delay stay, Start is the host's; what the host says beside them is over all
	CHECK(OpenBGI_InputSettings(o, "{\"defaults\": {\"pad\": {\"start\": \"confirm\", \"west\": \"skip\"}, \"tapDelay\": 90}}") == 1);
	CHECK(o->input.pad.bind[7] == OPENBGI_ACTION_CONFIRM && o->input.pad.bind[2] == (INPUT_BIND_KEY | 'A') && o->input.steps.tapDelay == 40);
	CHECK(o->input.cfgHost == NULL && o->input.cfgHostBase != NULL);
	CHECK(OpenBGI_InputSettings(o, "{\"defaults\": {\"pad\": {\"start\": \"confirm\"}}, \"pad\": {\"start\": \"backlog\", \"west\": \"none\"}}") == 1);
	CHECK(o->input.pad.bind[7] == OPENBGI_ACTION_BACKLOG && o->input.pad.bind[2] == 0 && o->input.cfgHost != NULL);
	CHECK(OpenBGI_InputSettings(o, "{\"defaults\": 5, \"pointerSize\": \"small\"}") == 1 && o->input.cfgHostBase == NULL && o->input.pad.bind[7] == OPENBGI_ACTION_CANCEL);
	CHECK(OpenBGI_InputSettings(o, NULL) == 1 && o->input.pad.bind[7] == OPENBGI_ACTION_CANCEL && o->input.cfgHostBase == NULL && o->input.cfgHost == NULL);
	InputMap_Configure(o, Engine_InputAll(o), g);
	// a control bound to the wheel, to a mouse button (held), to nothing; the game's own keys switched off
	CHECK(Pad_SetKey(o, 0, 'Z') == 0);
	Press(o, OPENBGI_PAD_SELECT, 2);
	Hold(o, OPENBGI_PAD_L2, 0);
	Frames(o, 4);
	CHECK(o->input.keyboard[4] & 0x80);
	Hold(o, 0, 0);
	Frames(o, 2);
	CHECK(!(o->input.keyboard[4] & 0x80));
	Press(o, OPENBGI_PAD_WEST, 2);
	LOG("0:W-");
	CHECK(Presses(o, 'A') == 0 && Presses(o, 'Z') == 0 && Presses(o, 4) == 1);
	/* a click the mapper makes waits the tap delay (40 ms, three frames of 16) after the pointer's move and
	 * before its release; where the pointer has rested, only the release waits */
	MouseLayer_Push(o, LOW);
	OpenBGI_Pointer(o, 300, 200);
	Press(o, OPENBGI_PAD_SOUTH, 8);
	LOG("0:L+ 3:L-");
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_DOWN, 100, 100);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_UP, 100, 100);
	Frames(o, 10);
	LOG("0:M100,100 3:L+ 6:L-");
	MouseLayer_Pop(o, LOW);
	// a control bound to a key holds it; the built-in bindings come back with the next game
	InputMap_Configure(o, Engine_InputAll(o), NULL);
	Hold(o, OPENBGI_PAD_WEST, 0);
	CHECK((o->input.keyboard['A'] & 0x80) && o->input.device == INPUT_DEV_PAD);
	Frames(o, 3);
	CHECK(o->input.keyboard['A'] & 0x80);
	Hold(o, 0, 0);
	CHECK(!(o->input.keyboard['A'] & 0x80) && Presses(o, 'A') == 1);
	Press(o, OPENBGI_PAD_SOUTH, 2);
	CHECK(Presses(o, 'Z') == 1); // (the game's own key again)
	Pad_SetKey(o, 0, 0);
	// the hotspot: no target while its program is not loaded
	InputMap_Configure(o, NULL, g);
	CHECK(o->input.steps.tapDelay == 0 && o->input.pad.ownKeysOff); // (the file's delay went with its settings)
	CHECK(OpenBGI_Targets(o, t, 8, sizeof(OpenBGI_Target_t)) == 0 && InputMap_Focusables(o, t, 8) == 0);
	o->vm.rootThread = Thread_New(o, 0, 0, 0);
	thread = Thread_SpawnChild(o, o->vm.rootThread, 0x100, 0x100, 0x400);
	m = (ModuleNode_t*)BGI_Calloc(sizeof *m);
	m->name = BGI_Strdup("other._bp");
	thread->modules = m;
	CHECK(OpenBGI_Targets(o, t, 8, sizeof(OpenBGI_Target_t)) == 0);
	m = (ModuleNode_t*)BGI_Calloc(sizeof *m);
	m->name = BGI_Strdup("MA._bp"); // (a name that only begins the hotspot's, and one that goes on after it)
	m->next = thread->modules;
	thread->modules = m;
	m = (ModuleNode_t*)BGI_Calloc(sizeof *m);
	m->name = BGI_Strdup("MAPS._bp");
	m->next = thread->modules;
	thread->modules = m;
	CHECK(OpenBGI_Targets(o, t, 8, sizeof(OpenBGI_Target_t)) == 0);
	m = (ModuleNode_t*)BGI_Calloc(sizeof *m);
	m->name = BGI_Strdup("MAP._bp");
	m->next = thread->modules;
	thread->modules = m;
	CHECK(OpenBGI_Targets(o, t, 8, sizeof(OpenBGI_Target_t)) == 1 && t[0].kind == OPENBGI_TARGET_HOTSPOT && t[0].index == 0 && t[0].layer == 0 && t[0].flags == 0);
	CHECK(t[0].rect.x == 10 && t[0].rect.y == 20 && t[0].rect.w == 30 && t[0].rect.h == 40 && t[0].clickX == 25 && t[0].clickY == 40);
	// the D-pad finds it, a tap beside it snaps to it, a click anywhere else is nobody's
	gLog[0] = 0;
	gFrame = 0;
	Dir(o, OPENBGI_DPAD_DOWN, 2);
	LOG("0:M25,40");
	CHECK(o->input.pad.mode == PAD_MODE_FOCUS && o->input.pad.focusKind == OPENBGI_TARGET_HOTSPOT);
	CHECK(InputMap_TapPoint(o, 50, 30, 32, at, &kind, &s) == 2 && kind == OPENBGI_TARGET_HOTSPOT && at[0] == 25 && at[1] == 40);
	CHECK(InputMap_TapPoint(o, 12, 59, 32, at, &kind, &s) == 1 && kind == OPENBGI_TARGET_HOTSPOT && at[0] == 12);
	CHECK(InputMap_TapPoint(o, 300, 300, 32, at, &kind, &s) == 0);
	// a layer of the script's takes nothing from it (a click anywhere reaches that); a button of the game's does
	MouseLayer_Push(o, LOW);
	CHECK(OpenBGI_Targets(o, t, 8, sizeof(OpenBGI_Target_t)) == 2 && t[0].kind == OPENBGI_TARGET_LAYER && t[1].kind == OPENBGI_TARGET_HOTSPOT);
	MouseLayer_Pop(o, LOW);
	s = Button(o, 200, 200);
	CHECK(OpenBGI_Targets(o, t, 8, sizeof(OpenBGI_Target_t)) == 1 && t[0].kind == OPENBGI_TARGET_SPRITE);
	o->input.inputEnable = 0; // .. unless it takes no click now
	CHECK(OpenBGI_Targets(o, t, 8, sizeof(OpenBGI_Target_t)) == 2 && t[1].kind == OPENBGI_TARGET_HOTSPOT && !(t[1].flags & OPENBGI_TARGET_DISABLED));
	o->input.inputEnable = 1;
	Target_Clear(o);
	GfxCall_SpriteShow(o, s, 0);
	/* several: each has its own index; a program may be named with its "._bp"; one whose program is not
	 * loaded is none */
	a = Parsed("{\"hotspots\": [{\"rect\": [10, 20, 30, 40], \"program\": \"map._BP\"}, {\"rect\": [100, 20, 30, 40], \"program\": \"nowhere\"},"
			   " {\"rect\": [200, 20, 30, 40]}]}");
	CHECK(a != NULL);
	InputMap_Configure(o, Engine_InputAll(o), a);
	InputSettings_Free(a); // (the engine keeps its own copy)
	n = OpenBGI_Targets(o, t, 8, sizeof(OpenBGI_Target_t));
	CHECK(n == 2 && t[0].kind == OPENBGI_TARGET_HOTSPOT && t[1].kind == OPENBGI_TARGET_HOTSPOT && t[0].index != t[1].index);
	for(i = 0; i < n && i < 2; i++)
		CHECK((t[i].index == 0 && t[i].rect.x == 10) || (t[i].index == 2 && t[i].rect.x == 200));
	// confirm on the focus is a click the mapper makes: the file's tap delay holds its release back
	Hold(o, 0, 0);
	Frames(o, 4);
	Dir(o, OPENBGI_DPAD_DOWN, 6);
	CHECK(o->input.pad.mode == PAD_MODE_FOCUS && o->input.pad.focusKind == OPENBGI_TARGET_HOTSPOT);
	gLog[0] = 0;
	gFrame = 0;
	Press(o, OPENBGI_PAD_SOUTH, 8);
	LOG("0:L+ 3:L-");
	// icons of a selection keep the hotspots away, but not while they take no click (another wait has the focus)
	{
		int32_t icons[2][4] = {{10, 20, 10, -1}, {60, 20, 10, -1}}; // x, y, bitmap, focus bitmap
		uint32_t hw = 0;
		CHECK(GfxCall_WindowCreate(o, &hw, 300, 200) == 0);
		GfxCall_WindowSet(o, hw, 50, 60, 0, 0, 0, 4);
		GfxCall_WindowShow(o, hw, 1);
		CHECK(StartIconSelect(o, thread, hw, 2, icons, NULL, 3, 0, 0) == 0);
		n = OpenBGI_Targets(o, t, 8, sizeof(OpenBGI_Target_t));
		CHECK(n == 2 && t[0].kind == OPENBGI_TARGET_ICON && t[1].kind == OPENBGI_TARGET_ICON);
		InputFocus_Add(o, 0x7777, 0xfffffff0u);
		n = OpenBGI_Targets(o, t, 8, sizeof(OpenBGI_Target_t));
		CHECK(n == 4 && (t[0].flags & OPENBGI_TARGET_DISABLED) && (t[1].flags & OPENBGI_TARGET_DISABLED) && t[2].kind == OPENBGI_TARGET_HOTSPOT &&
			t[3].kind == OPENBGI_TARGET_HOTSPOT);
		InputFocus_Remove(o, 0x7777);
		Thread_DropWait(o, thread);
	}
	// the run over: the settings go with it
	InputMap_Stop(o);
	CHECK(o->input.hotspotCount == 0 && o->input.hotspots == NULL && !o->input.pad.ownKeysOff && o->input.pad.bind[6] == 0);
	Thread_KillAllChildren(o, o->vm.rootThread);
	Thread_Delete(o, o->vm.rootThread);
	o->vm.rootThread = NULL;
	Engine_ResetGames(o);
	CHECK(Engine_InputAll(o) == NULL);
	EngineDown(o);
}

// ---- through OpenBGI_RunFrame ---------------------------------------------------------------------

// an engine with the picture of the screen (Screen_Setup), on a host of `caps`
static OpenBGI_t* ScreenUp(uint32_t caps)
{
	OpenBGI_t* o;
	gHost = *OS_Host();
	gHost.ticksMs = TicksMs;
	gHost.caps = caps;
	gHost.childWindows = NULL;
	gHost.textEntry = NULL;
	o = OpenBGI_New(&gHost);
	if(!o)
		return NULL;
	Screen_Setup(o, W, H, PM_RGB32);
	Gfx_Create(o, 0xffffffffu, 1);
	Input_ResetLayers(o);
	o->input.inputEnable = 1;
	o->display.windowAlive = 1;
	memset(&gPad, 0, sizeof gPad);
	gPad.size = sizeof gPad;
	return o;
}

static void ScreenDown(OpenBGI_t* o)
{
	Edit_Destroy(o);
	MsgQueue_Clear(o);
	Knob_UnregisterAll(o);
	Target_Clear(o);
	Input_ResetLayers(o);
	o->display.windowAlive = 0;
	Gfx_Destroy(o);
	OpenBGI_Delete(o);
}

/* `count` frames of the engine (OpenBGI_RunFrame), 16 ms apart.  The
 * engine was not started, so no pass runs: the mapper's part of one is
 * called here (while a dialog is up in the frame the engine calls it) */
static void RunFrames(OpenBGI_t* o, OpenBGI_Frame_t* f, int count)
{
	while(count-- > 0)
	{
		if(!Inframe_Up(o))
			InputMap_Frame(o);
		OpenBGI_RunFrame(o, f);
		gNow += FRAME;
	}
}

// the frame's pixel at (x, y), 0x00RRGGBB
static uint32_t Pixel(const OpenBGI_Frame_t* f, int x, int y)
{
	return ((const uint32_t*)(f->pixels + (size_t)y * f->pitch))[x] & 0xffffff;
}

// a button of the pad through `count` frames of the engine, then let go for as many
static void RunPress(OpenBGI_t* o, OpenBGI_Frame_t* f, uint32_t buttons, uint32_t dpad, int count)
{
	Hold(o, buttons, dpad);
	RunFrames(o, f, count);
	Hold(o, 0, 0);
	RunFrames(o, f, count);
}

static void TestArrow(void)
{
	OpenBGI_t* o;
	OpenBGI_Frame_t f = {.size = sizeof f};
	uint32_t caps;
	printf("arrow\n");
	for(caps = 0; caps <= OPENBGI_CAP_POINTER; caps += OPENBGI_CAP_POINTER)
	{
		o = ScreenUp(caps);
		if(!o)
			return;
		Button(o, 100, 100);
		RunFrames(o, &f, 2);
		CHECK(f.width == W && f.height == H && Pixel(&f, 120, 115) == 0); // (the test's sprites are not composed into the picture)
		// a focus: the arrow with its tip on the pointer, black around white
		RunPress(o, &f, 0, OPENBGI_DPAD_DOWN, 2);
		CHECK(o->input.pointerX == 120 && o->input.pointerY == 115 && Pad_OwnsPointer(o));
		if(caps)
			CHECK(Pixel(&f, 122, 120) == 0 && !o->input.pad.arrowShown); // the host shows its own
		else
		{
			int far1, far2;
			// white inside, nothing beside it; a thirty-sixth of the picture's height, 16 pixels at least
			CHECK(Pixel(&f, 122, 120) == 0xffffff && Pixel(&f, 123, 122) == 0xffffff && Pixel(&f, 119, 115) == 0 && Pixel(&f, 120, 114) == 0);
			CHECK(Pixel(&f, 120 + 12, 115) == 0 && Pixel(&f, 120, 115 + 16) == 0 && Pixel(&f, 120 + 9, 115 + 8) == 0);
			CHECK(o->input.pad.arrowShown && o->input.pad.arrowH == 16 && o->input.pad.arrowW == 11);
			// its edge is dark and takes of what is under it, its inside covers it
			CHECK((o->input.pad.arrow[0] >> 24) > 0 && (o->input.pad.arrow[0] >> 24) < 255 && (o->input.pad.arrow[0] & 0xffffff) == 0);
			CHECK(o->input.pad.arrow[5 * 11 + 2] == 0xffffffffu && o->input.pad.arrow[8 * 11 + 9] == 0 && (o->input.pad.arrow[8 * 11] & 0xffffff) == 0);
			// it follows the pointer
			Sticks(o, FULL, 0, 0, 0);
			RunFrames(o, &f, 10);
			Sticks(o, 0, 0, 0, 0);
			RunFrames(o, &f, 2);
			far1 = o->input.pointerX - 120;
			CHECK(o->input.pointerX > 180 && Pixel(&f, 122, 120) == 0 && Pixel(&f, o->input.pointerX + 2, 120) == 0xffffff);
			// in the picture's corner only its tip is in the picture
			Sticks(o, FULL, FULL, 0, 0);
			RunFrames(o, &f, 120);
			Sticks(o, 0, 0, 0, 0);
			RunFrames(o, &f, 2);
			CHECK(o->input.pointerX == W - 1 && o->input.pointerY == H - 1 && o->input.pad.arrowShown && Pixel(&f, W - 1, H - 1) == 0);
			Sticks(o, -FULL, -FULL, 0, 0);
			RunFrames(o, &f, 60);
			Sticks(o, 0, 0, 0, 0);
			RunFrames(o, &f, 2);
			CHECK(o->input.pointerX > 180 && o->input.pointerX < W - 100 && Pixel(&f, o->input.pointerX + 2, o->input.pointerY + 5) == 0xffffff);
			// not while the game shows no cursor
			o->cursor.cursorShown = 0;
			RunFrames(o, &f, 1);
			CHECK(Pixel(&f, o->input.pointerX + 2, o->input.pointerY + 5) == 0 && !o->input.pad.arrowShown);
			o->cursor.cursorShown = 1;
			RunFrames(o, &f, 1);
			CHECK(Pixel(&f, o->input.pointerX + 2, o->input.pointerY + 5) == 0xffffff);
			// its size as the settings say, at once
			CHECK(OpenBGI_InputSettings(o, "{\"pointerSize\": \"large\"}"));
			RunFrames(o, &f, 1);
			CHECK(o->input.pad.arrowH == 24 && o->input.pad.arrowW == 16 && Pixel(&f, o->input.pointerX + 4, o->input.pointerY + 14) == 0xffffff);
			CHECK(OpenBGI_InputSettings(o, "{\"pointerSize\": \"small\"}"));
			RunFrames(o, &f, 1);
			CHECK(o->input.pad.arrowH == 12 && o->input.pad.arrowW == 9 && Pixel(&f, o->input.pointerX + 4, o->input.pointerY + 14) == 0 &&
				Pixel(&f, o->input.pointerX + 2, o->input.pointerY + 5) == 0xffffff);
			// on a screen that says its density: 4.5 mm at least, here 45 pixels; the sizes by that
			CHECK(OpenBGI_InputSettings(o, NULL));
			OpenBGI_TouchScale(o, 254);
			RunFrames(o, &f, 1);
			CHECK(o->input.pad.arrowH == 45 && o->input.pad.arrowW == 29 && Pixel(&f, o->input.pointerX + 8, o->input.pointerY + 26) == 0xffffff);
			CHECK(OpenBGI_InputSettings(o, "{\"pointerSize\": \"large\"}"));
			RunFrames(o, &f, 1);
			CHECK(o->input.pad.arrowH == 67);
			OpenBGI_TouchScale(o, 2000); // (never larger than its picture has room for)
			RunFrames(o, &f, 1);
			CHECK(o->input.pad.arrowH == PAD_ARROW_MAX_H && o->input.pad.arrowW <= PAD_ARROW_MAX_W);
			OpenBGI_TouchScale(o, 60); // a coarse screen: the picture's own rule
			RunFrames(o, &f, 1);
			CHECK(o->input.pad.arrowH == 24);
			OpenBGI_TouchScale(o, 0);
			CHECK(OpenBGI_InputSettings(o, "{\"pointerSize\": \"small\"}"));
			RunFrames(o, &f, 1);
			CHECK(o->input.pad.arrowH == 12);
			// never: none for the pad either
			CHECK(OpenBGI_InputSettings(o, "{\"pointer\": \"never\"}"));
			RunFrames(o, &f, 1);
			CHECK(Pad_OwnsPointer(o) && !o->input.pad.arrowShown && Pixel(&f, o->input.pointerX + 2, o->input.pointerY + 5) == 0);
			CHECK(OpenBGI_InputSettings(o, NULL));
			RunFrames(o, &f, 1);
			CHECK(o->input.pad.arrowShown && o->input.pad.arrowH == 16 && Pixel(&f, o->input.pointerX + 2, o->input.pointerY + 5) == 0xffffff);
			// a stick at twice the speed goes twice as far
			OpenBGI_InputSettings(o, "{\"stickSpeed\": 200}");
			Sticks(o, -FULL, -FULL, 0, 0);
			RunFrames(o, &f, 120);
			Sticks(o, 0, 0, 0, 0);
			RunFrames(o, &f, 2);
			CHECK(o->input.pointerX == 0 && o->input.pointerY == 0);
			Sticks(o, FULL, 0, 0, 0);
			RunFrames(o, &f, 10);
			Sticks(o, 0, 0, 0, 0);
			RunFrames(o, &f, 2);
			far2 = o->input.pointerX;
			CHECK(far1 > 60 && far2 > far1 * 2 - 6 && far2 < far1 * 2 + 6);
			OpenBGI_InputSettings(o, NULL);
			// a mouse takes over: gone
			OpenBGI_Pointer(o, 300, 300);
			RunFrames(o, &f, 1);
			CHECK(Pixel(&f, 302, 305) == 0 && !o->input.pad.arrowShown);
			// .. unless it is drawn for a mouse as well; then the pad's again
			CHECK(OpenBGI_InputSettings(o, "{\"pointer\": \"always\"}"));
			RunFrames(o, &f, 1);
			CHECK(o->input.pad.arrowShown && Pixel(&f, 302, 305) == 0xffffff);
			OpenBGI_Pointer(o, 310, 300);
			RunFrames(o, &f, 1);
			CHECK(o->input.pad.arrowShown && Pixel(&f, 312, 305) == 0xffffff && Pixel(&f, 301, 305) == 0);
			OpenBGI_Key(o, 'A', 1, 0);
			OpenBGI_Key(o, 'A', 0, 0);
			RunFrames(o, &f, 1);
			CHECK(!o->input.pad.arrowShown && Pixel(&f, 312, 305) == 0); // a keyboard has no pointer
			OpenBGI_Pointer(o, 300, 300);
			CHECK(OpenBGI_InputSettings(o, NULL));
			RunFrames(o, &f, 1);
			CHECK(Pixel(&f, 302, 305) == 0 && !o->input.pad.arrowShown);
			// nothing driven (CONFIRM going on): none
			MouseLayer_Push(o, LOW);
			RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 3);
			CHECK(o->input.device == INPUT_DEV_PAD && !Pad_OwnsPointer(o) && Pixel(&f, 302, 305) == 0);
			MouseLayer_Pop(o, LOW);
		}
		ScreenDown(o);
	}
}

// the entry's text is `sjis`
static int EntryIs(OpenBGI_t* o, const char* sjis)
{
	char buf[EDIT_TEXT_MAX];
	return Edit_GetText(o, buf) >= 0 && strcmp(buf, sjis) == 0;
}

// the middle of the keyboard's key (row, col), the picture's pixels
static void KeyAt(OpenBGI_t* o, int row, int half, int* x, int* y)
{
	const Osk_t* k = &o->input.osk;
	*x = k->x + 6 + half * k->keyW / 2;
	*y = k->y + 6 + row * k->keyH + k->keyH / 2;
}

// a click of the mouse at (x, y)
static void Click(OpenBGI_t* o, OpenBGI_Frame_t* f, int x, int y)
{
	OpenBGI_Pointer(o, x, y);
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 1, x, y, 0);
	RunFrames(o, f, 1);
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 0, x, y, 0);
	RunFrames(o, f, 1);
}

static void TestKeyboard(void)
{
	OpenBGI_t* o = ScreenUp(0);
	OpenBGI_Frame_t f = {.size = sizeof f};
	OpenBGI_Target_t t[4];
	OpenBGI_Rect_t r;
	const Osk_t* k;
	uint32_t s, under;
	int x, y, enters;
	printf("keyboard\n");
	if(!o)
		return;
	k = &o->input.osk;
	RunFrames(o, &f, 2);
	// no text field has the keys: KEYBOARD brings nothing up
	OpenBGI_Action(o, OPENBGI_ACTION_KEYBOARD, 1);
	OpenBGI_Action(o, OPENBGI_ACTION_KEYBOARD, 0);
	CHECK(!Osk_Up(o));
	RunFrames(o, &f, 1);
	// the entry gets the keys while a mouse is in use: no keyboard
	OpenBGI_Pointer(o, 10, 10);
	CHECK(Edit_Setup(o, 100, 50, 300, 30, 0, 20, 20, 1) == 0);
	RunFrames(o, &f, 3);
	CHECK(f.textInput == 1 && f.textRect.x == 100 && f.textRect.w == 300 && !Osk_Up(o));
	CHECK(OpenBGI_Targets(o, t, 4, sizeof(OpenBGI_Target_t)) == 1 && t[0].kind == OPENBGI_TARGET_ENTRY && (t[0].flags & OPENBGI_TARGET_SELECTED));
	CHECK(t[0].rect.x == 100 && t[0].rect.y == 50 && t[0].rect.w == 300 && t[0].rect.h == 30 && t[0].clickX == 250 && t[0].clickY == 65);
	// KEYBOARD asks for it: along the bottom, 17 keys across; the host's input method is not asked for while it is up
	OpenBGI_Action(o, OPENBGI_ACTION_KEYBOARD, 1);
	OpenBGI_Action(o, OPENBGI_ACTION_KEYBOARD, 0);
	RunFrames(o, &f, 2);
	CHECK(Osk_Up(o) && f.textInput == 0 && k->keyW == 36 && k->keyH == 36 && k->w == 624 && k->h == 228 && k->x == 8 && k->y == 252);
	CHECK(Pixel(&f, 8, 252) == 0x303438 && Pixel(&f, 8, 251) == 0 && Pixel(&f, 631, 479) == 0x303438);
	CHECK(Osk_Over(o, 8, 252) && !Osk_Over(o, 7, 252) && !Osk_Over(o, 8, 251));
	CHECK(OpenBGI_Targets(o, t, 4, sizeof(OpenBGI_Target_t)) == 1 && t[0].clickX == 250); // (the entry is clear of it)
	// CONFIRM types the key the directions are on; they go around the edges
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 1);
	CHECK(EntryIs(o, "\x82\xa0")); // a
	RunPress(o, &f, 0, OPENBGI_DPAD_RIGHT, 1);
	RunPress(o, &f, 0, OPENBGI_DPAD_DOWN, 1);
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 1);
	CHECK(EntryIs(o, "\x82\xa0\x82\xab")); // a ki
	RunPress(o, &f, 0, OPENBGI_DPAD_LEFT, 1);
	RunPress(o, &f, 0, OPENBGI_DPAD_LEFT, 1); // around: the last column
	CHECK(k->row == 1 && k->col == 16);
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 1);
	CHECK(EntryIs(o, "\x82\xa0\x82\xab\x82\xe1")); // a ki small-ya
	// the sticks rest while it is up
	x = o->input.pointerX;
	Sticks(o, FULL, FULL, 0, -FULL);
	RunFrames(o, &f, 20);
	Sticks(o, 0, 0, 0, 0);
	RunFrames(o, &f, 1);
	CHECK(o->input.pointerX == x && o->input.pad.mode != PAD_MODE_POINTER && Presses(o, 0x0e) == 0);
	// CANCEL deletes, BACKLOG and FORWARD move the caret
	RunPress(o, &f, OPENBGI_PAD_EAST, 0, 1);
	CHECK(EntryIs(o, "\x82\xa0\x82\xab"));
	RunPress(o, &f, OPENBGI_PAD_L1, 0, 1);
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 1);
	CHECK(EntryIs(o, "\x82\xa0\x82\xe1\x82\xab"));
	RunPress(o, &f, OPENBGI_PAD_R1, 0, 1);
	RunPress(o, &f, OPENBGI_PAD_EAST, 0, 1);
	CHECK(EntryIs(o, "\x82\xa0\x82\xe1"));
	// SKIP turns to the next table: katakana, full-width, half-width, and around
	RunPress(o, &f, OPENBGI_PAD_R2, 0, 1);
	CHECK(k->page == OSK_PAGE_KATA && !(o->input.keyboard[0x11] & 0x80));
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 1);
	CHECK(EntryIs(o, "\x82\xa0\x82\xe1\x83\x83"));
	RunPress(o, &f, OPENBGI_PAD_R2, 0, 1);
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 1);
	CHECK(k->page == OSK_PAGE_WIDE && EntryIs(o, "\x82\xa0\x82\xe1\x83\x83\x82\x56")); // full-width 7
	RunPress(o, &f, OPENBGI_PAD_R2, 0, 1);
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 1);
	CHECK(k->page == OSK_PAGE_HALF && EntryIs(o, "\x82\xa0\x82\xe1\x83\x83\x82\x56"
												 "7"));
	RunPress(o, &f, OPENBGI_PAD_R2, 0, 1);
	CHECK(k->page == OSK_PAGE_HIRA);
	// the row below: from the last column down to "done", up again to the table key over its middle
	RunPress(o, &f, 0, OPENBGI_DPAD_UP, 1);
	RunPress(o, &f, 0, OPENBGI_DPAD_UP, 1); // around the top
	CHECK(k->row == OSK_ROWS && k->col == OSK_KEY_DONE);
	RunPress(o, &f, 0, OPENBGI_DPAD_UP, 1);
	CHECK(k->row == OSK_ROWS - 1 && k->col == 16);
	RunPress(o, &f, 0, OPENBGI_DPAD_DOWN, 1);
	RunPress(o, &f, 0, OPENBGI_DPAD_LEFT, 1);
	CHECK(k->row == OSK_ROWS && k->col == OSK_KEY_DELETE);
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 1);
	CHECK(EntryIs(o, "\x82\xa0\x82\xe1\x83\x83\x82\x56"));
	// the pointer: a press and its release on a key type it; let go elsewhere, nothing
	KeyAt(o, 0, 5, &x, &y); // the third key of the first row
	Click(o, &f, x, y);
	CHECK(EntryIs(o, "\x82\xa0\x82\xe1\x83\x83\x82\x56\x82\xb3") && Osk_Up(o) && o->input.device == INPUT_DEV_MOUSE); // sa
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 1, x, y, 0);
	RunFrames(o, &f, 1);
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 0, x + 40, y, 0);
	RunFrames(o, &f, 1);
	CHECK(EntryIs(o, "\x82\xa0\x82\xe1\x83\x83\x82\x56\x82\xb3"));
	KeyAt(o, OSK_ROWS, 5, &x, &y); // katakana
	Click(o, &f, x, y);
	CHECK(k->page == OSK_PAGE_KATA);
	KeyAt(o, OSK_ROWS, 21, &x, &y); // the space (three keys wide, to its right end): full-width but on the half-width table
	Click(o, &f, x, y);
	CHECK(EntryIs(o, "\x82\xa0\x82\xe1\x83\x83\x82\x56\x82\xb3\x81\x40"));
	KeyAt(o, OSK_ROWS, 27, &x, &y); // delete (from its left end)
	Click(o, &f, x, y);
	Click(o, &f, x, y);
	CHECK(EntryIs(o, "\x82\xa0\x82\xe1\x83\x83\x82\x56"));
	KeyAt(o, OSK_ROWS, 23, &x, &y); // the caret left, then a key: inserted before the last
	Click(o, &f, x, y);
	KeyAt(o, 0, 1, &x, &y);
	Click(o, &f, x, y);
	CHECK(EntryIs(o, "\x82\xa0\x82\xe1\x83\x83\x83\x41\x82\x56")); // katakana a
	// a press on the keyboard is not the entry's nor the game's
	CHECK(Edit_Rect(o, &r) == 2);
	// "done": it goes, and Enter gives the entry up
	enters = Presses(o, 0x0d);
	KeyAt(o, OSK_ROWS, 32, &x, &y);
	Click(o, &f, x, y);
	RunFrames(o, &f, 1);
	CHECK(!Osk_Up(o) && Edit_Rect(o, &r) == 1 && Presses(o, 0x0d) == enters && f.textInput == 0); // (the Enter was the entry's, not the game's)
	CHECK(Pixel(&f, 8, 252) == 0);
	// the focus on the entry and CONFIRM: its click gives it the keys, the keyboard follows
	RunPress(o, &f, 0, OPENBGI_DPAD_DOWN, 2);
	CHECK(o->input.pad.mode == PAD_MODE_FOCUS && o->input.pad.focusKind == OPENBGI_TARGET_ENTRY && o->input.pointerX == 250);
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 4);
	CHECK(Edit_Rect(o, &r) == 2 && Osk_Up(o) && k->page == OSK_PAGE_KATA);
	// a key of a real keyboard: it goes
	OpenBGI_Key(o, 0x25, 1, 0);
	OpenBGI_Key(o, 0x25, 0, 0);
	RunFrames(o, &f, 1);
	CHECK(!Osk_Up(o) && Edit_Rect(o, &r) == 2 && f.textInput == 1);
	// the focus first shows on what the pointer is on, else on what is selected (the entry with the keys), else on the nearest
	s = Button(o, 450, 150);
	OpenBGI_Pointer(o, 460, 220); // nearer the button
	RunPress(o, &f, 0, OPENBGI_DPAD_DOWN, 2);
	CHECK(o->input.pad.mode == PAD_MODE_FOCUS && o->input.pad.focusKind == OPENBGI_TARGET_ENTRY && !Osk_Up(o));
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 4); // CONFIRM on the entry that has the keys already: its keyboard
	CHECK(Osk_Up(o) && Edit_Rect(o, &r) == 2);
	// a target under the keyboard takes no click, one beside it does; the focus does not go under it
	under = Button(o, 300, 400);
	CHECK(OpenBGI_Targets(o, t, 4, sizeof(OpenBGI_Target_t)) == 3 && InputMap_Focusables(o, t, 4) == 2);
	CHECK(t[0].kind == OPENBGI_TARGET_ENTRY && t[1].owner == s);
	GfxCall_SpriteShow(o, under, 0);
	RunPress(o, &f, OPENBGI_PAD_NORTH, 0, 2);
	CHECK(!Osk_Up(o) && Edit_Rect(o, &r) == 2);
	OpenBGI_Pointer(o, 470, 165); // on the button
	RunPress(o, &f, 0, OPENBGI_DPAD_DOWN, 2);
	CHECK(Focused(o, s) && Edit_Rect(o, &r) == 2);
	GfxCall_SpriteShow(o, s, 0);
	Target_Clear(o);
	OpenBGI_Pointer(o, 10, 10);
	// CONFIRM with the entry holding the keys: up again; it goes when the entry loses them
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 2);
	CHECK(Osk_Up(o));
	Edit_Show(o, 0);
	RunFrames(o, &f, 2);
	CHECK(!Osk_Up(o) && OpenBGI_Targets(o, t, 4, sizeof(OpenBGI_Target_t)) == 0);
	// the entry gets the keys under a pad: the keyboard comes by itself, along the top when the entry is at the bottom
	CHECK(o->input.device == INPUT_DEV_PAD);
	CHECK(Edit_Setup(o, 100, 420, 300, 30, 0, 20, 20, 1) == 0);
	RunFrames(o, &f, 3);
	CHECK(Osk_Up(o) && k->y == 0 && Pixel(&f, 8, 0) == 0x303438 && Pixel(&f, 8, 228) == 0);
	Edit_Show(o, 0);
	RunFrames(o, &f, 2);
	// a touch screen on a host without a keyboard: the same, and a tap in the entry brings it back
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_DOWN, 600, 20);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_CANCEL, 600, 20);
	CHECK(o->input.device == INPUT_DEV_TOUCH);
	CHECK(Edit_Setup(o, 100, 50, 300, 30, 0, 20, 20, 1) == 0);
	RunFrames(o, &f, 3);
	CHECK(Osk_Up(o) && k->y == 252);
	OpenBGI_Action(o, OPENBGI_ACTION_KEYBOARD, 1); // put away (a pad's turn; then the finger's again)
	OpenBGI_Action(o, OPENBGI_ACTION_KEYBOARD, 0);
	RunFrames(o, &f, 2);
	CHECK(!Osk_Up(o) && Edit_Rect(o, &r) == 2);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_DOWN, 200, 60); // the entry has the keys still: a tap in it brings the keyboard
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_UP, 200, 60);
	RunFrames(o, &f, 5);
	CHECK(Osk_Up(o) && Edit_Rect(o, &r) == 2);
	OpenBGI_Action(o, OPENBGI_ACTION_KEYBOARD, 1);
	OpenBGI_Action(o, OPENBGI_ACTION_KEYBOARD, 0);
	RunFrames(o, &f, 2);
	CHECK(!Osk_Up(o));
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_DOWN, 600, 300); // a tap elsewhere takes the keys from the entry
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_UP, 600, 300);
	RunFrames(o, &f, 5);
	CHECK(!Osk_Up(o) && Edit_Rect(o, &r) == 1);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_DOWN, 200, 60);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_UP, 200, 60);
	RunFrames(o, &f, 5);
	CHECK(Osk_Up(o) && Edit_Rect(o, &r) == 2);
	// a tap on a key types it (a finger beside a key does not snap to something of the game's)
	KeyAt(o, 0, 1, &x, &y);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_DOWN, x, y);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_UP, x, y);
	RunFrames(o, &f, 5);
	CHECK(Osk_Up(o) && EntryIs(o, "\x83\x41") && o->input.touch.tapKind == 0 && o->input.touch.tapToX == x); // (a setup empties the entry)
	s = Button(o, 20, 215);                                                                                  // a button of the game's 8 pixels above the keyboard: a tap on the key below it is the key's
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_DOWN, x, y);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_UP, x, y);
	RunFrames(o, &f, 5);
	CHECK(Osk_Up(o) && EntryIs(o, "\x83\x41\x83\x41") && o->input.touch.tapKind == 0 && Edit_Rect(o, &r) == 2);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_DOWN, x, k->y - 2); // .. one just above the keyboard snaps to the button
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_UP, x, k->y - 2);
	RunFrames(o, &f, 5);
	CHECK(o->input.touch.tapKind == OPENBGI_TARGET_SPRITE && o->input.touch.tapOwner == s);
	ScreenDown(o);
	// a host with a keyboard: a finger does not bring it up
	o = ScreenUp(OPENBGI_CAP_KEYBOARD);
	if(!o)
		return;
	RunFrames(o, &f, 2);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_DOWN, 600, 20);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_CANCEL, 600, 20);
	CHECK(Edit_Setup(o, 100, 50, 300, 30, 0, 20, 20, 1) == 0);
	RunFrames(o, &f, 3);
	CHECK(!Osk_Up(o) && f.textInput == 1);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_DOWN, 200, 60);
	OpenBGI_Touch(o, 1, OPENBGI_TOUCH_UP, 200, 60);
	RunFrames(o, &f, 5);
	CHECK(!Osk_Up(o) && Edit_Rect(o, &r) == 2);
	// .. a pad does
	RunPress(o, &f, OPENBGI_PAD_NORTH, 0, 2);
	CHECK(Osk_Up(o));
	RunPress(o, &f, OPENBGI_PAD_NORTH, 0, 2); // and puts it away
	CHECK(!Osk_Up(o));
	// what a control held for the game is let go when the keyboard has the turn by then
	Hold(o, OPENBGI_PAD_R2 | OPENBGI_PAD_EAST, 0);
	RunFrames(o, &f, 2);
	CHECK((o->input.keyboard[0x11] & 0x80) && (o->input.keyboard[2] & 0x80));
	Hold(o, OPENBGI_PAD_R2 | OPENBGI_PAD_EAST | OPENBGI_PAD_NORTH, 0);
	RunFrames(o, &f, 2);
	CHECK(Osk_Up(o));
	Hold(o, 0, 0);
	RunFrames(o, &f, 2);
	CHECK(Osk_Up(o) && !(o->input.keyboard[0x11] & 0x80) && !(o->input.keyboard[2] & 0x80) && o->input.osk.page == OSK_PAGE_HIRA);
	RunPress(o, &f, OPENBGI_PAD_NORTH, 0, 2);
	CHECK(!Osk_Up(o));
	// the engine stopped with it up: it goes
	RunPress(o, &f, OPENBGI_PAD_NORTH, 0, 2);
	CHECK(Osk_Up(o));
	InputMap_Stop(o);
	CHECK(!Osk_Up(o) && !o->screen.overlay[SCREEN_OVERLAY_OSK].pix);
	ScreenDown(o);
}

static void TestDialog(void)
{
	OpenBGI_t* o = ScreenUp(0);
	OpenBGI_Frame_t f = {.size = sizeof f};
	OpenBGI_Request_t q = {.size = sizeof q};
	char out[0x100] = "", sur[0x100] = "\x82\xa0", giv[0x100] = "\x82\xa0", nick[0x100] = "\x82\xa0", pro[0x100] = "";
	int32_t month = 0, day = 0;
	uint32_t id;
	int answer = -1;
	printf("dialog\n");
	if(!o)
		return;
	RunFrames(o, &f, 2);
	// an input box drawn in the frame: its field has the keys, no keyboard by itself
	memset(&q, 0, sizeof q);
	q.size = sizeof q;
	q.kind = OPENBGI_ASK_INPUT;
	q.u.input.title = "t";
	q.u.input.initial = "";
	q.u.input.maxLen = 8;
	q.u.input.out = out;
	Hold(o, OPENBGI_PAD_L3, 0); // (a pad is in use)
	Hold(o, 0, 0);
	id = Request_Post(o, &q, REQUEST_TREAT_NONE);
	RunFrames(o, &f, 3);
	CHECK(Inframe_Up(o) && f.textInput == 1 && !Osk_Up(o) && o->input.osk.field && !o->input.osk.fieldEntry);
	// CONFIRM on the field: the keyboard; it types into the field
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 2);
	CHECK(Osk_Up(o) && f.textInput == 0 && Inframe_Up(o));
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 1);
	RunPress(o, &f, 0, OPENBGI_DPAD_RIGHT, 1);
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 1);
	// "done" puts it away; the dialog stays (no Enter)
	RunPress(o, &f, 0, OPENBGI_DPAD_UP, 1);
	RunPress(o, &f, 0, OPENBGI_DPAD_LEFT, 1);
	CHECK(o->input.osk.row == OSK_ROWS && o->input.osk.col == OSK_KEY_DONE);
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 2);
	CHECK(!Osk_Up(o) && Inframe_Up(o) && !Request_Take(o, id, &answer) && f.textInput == 1);
	// DOWN is Tab: to the OK button, where CONFIRM is Enter; UP is Shift+Tab: back to the field
	RunPress(o, &f, 0, OPENBGI_DPAD_DOWN, 1);
	CHECK(Inframe_Up(o) && f.textInput == 0);
	RunPress(o, &f, 0, OPENBGI_DPAD_UP, 1);
	CHECK(Inframe_Up(o) && f.textInput == 1 && !(o->input.keyboard[0x10] & 0x80));
	RunPress(o, &f, 0, OPENBGI_DPAD_DOWN, 1);
	CHECK(Inframe_Up(o) && f.textInput == 0);
	RunPress(o, &f, 0, OPENBGI_DPAD_LEFT, 1); // on a button: along the buttons (around to Cancel), not back to the field
	CHECK(Inframe_Focus(o) == INFRAME_FOCUS_BUTTON && f.textInput == 0);
	RunPress(o, &f, 0, OPENBGI_DPAD_RIGHT, 1); // OK again
	CHECK(Inframe_Focus(o) == INFRAME_FOCUS_BUTTON);
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 2);
	CHECK(!Inframe_Up(o) && Request_Take(o, id, &answer) && answer == 1 && strcmp(out, "\x82\xa0\x82\xa9") == 0); // a ka
	// CANCEL is Escape
	out[0] = 0;
	id = Request_Post(o, &q, REQUEST_TREAT_NONE);
	RunFrames(o, &f, 3);
	CHECK(Inframe_Up(o));
	RunPress(o, &f, OPENBGI_PAD_EAST, 0, 2);
	CHECK(!Inframe_Up(o) && Request_Take(o, id, &answer) && answer == 0);
	// the profile: the focus starts on OK; UP goes back to the day, which UP and DOWN pick; LEFT goes on to the month
	memset(&q, 0, sizeof q);
	q.size = sizeof q;
	q.kind = OPENBGI_ASK_PROFILE;
	q.u.profile.surname = sur;
	q.u.profile.givenName = giv;
	q.u.profile.nickname = nick;
	q.u.profile.pronoun = pro;
	q.u.profile.month = &month;
	q.u.profile.day = &day;
	id = Request_Post(o, &q, REQUEST_TREAT_NONE);
	RunFrames(o, &f, 3);
	CHECK(Inframe_Up(o) && Inframe_Focus(o) == INFRAME_FOCUS_BUTTON);
	RunPress(o, &f, 0, OPENBGI_DPAD_UP, 1);
	CHECK(Inframe_Focus(o) == INFRAME_FOCUS_PICKER);
	RunPress(o, &f, 0, OPENBGI_DPAD_UP, 1);
	RunPress(o, &f, 0, OPENBGI_DPAD_UP, 1); // the 3rd
	RunPress(o, &f, 0, OPENBGI_DPAD_LEFT, 1);
	CHECK(Inframe_Focus(o) == INFRAME_FOCUS_PICKER);
	RunPress(o, &f, 0, OPENBGI_DPAD_DOWN, 1); // around to December
	RunPress(o, &f, 0, OPENBGI_DPAD_LEFT, 1);
	CHECK(Inframe_Focus(o) == INFRAME_FOCUS_FIELD && !Osk_Up(o));
	// the pronoun by the keyboard (its directions stood on "done": down around to the first row, over its middle)
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 2);
	CHECK(Osk_Up(o));
	RunPress(o, &f, 0, OPENBGI_DPAD_DOWN, 1);
	CHECK(o->input.osk.row == 0 && o->input.osk.col == 16);
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 1);
	RunPress(o, &f, OPENBGI_PAD_NORTH, 0, 2);
	CHECK(!Osk_Up(o) && Inframe_Focus(o) == INFRAME_FOCUS_FIELD);
	// R1 goes from control to control whatever has the focus: the month, the day, OK
	RunPress(o, &f, OPENBGI_PAD_R1, 0, 1);
	RunPress(o, &f, OPENBGI_PAD_R1, 0, 1);
	CHECK(Inframe_Focus(o) == INFRAME_FOCUS_PICKER);
	RunPress(o, &f, OPENBGI_PAD_R1, 0, 1);
	CHECK(Inframe_Focus(o) == INFRAME_FOCUS_BUTTON);
	RunPress(o, &f, OPENBGI_PAD_L1, 0, 1);
	CHECK(Inframe_Focus(o) == INFRAME_FOCUS_PICKER);
	RunPress(o, &f, OPENBGI_PAD_R1, 0, 1);
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 2);
	CHECK(!Inframe_Up(o) && Request_Take(o, id, &answer) && answer == 1 && month == 11 && day == 2 && strcmp(pro, "\x82\xc1") == 0);
	CHECK(Inframe_Focus(o) == 0);
	// a box: the directions move between its buttons, CONFIRM presses the one they are on
	id = Request_Box(o, "t", "c", OPENBGI_MB_YESNO, REQUEST_TREAT_NONE);
	RunFrames(o, &f, 3);
	CHECK(Inframe_Up(o));
	RunPress(o, &f, 0, OPENBGI_DPAD_RIGHT, 1);
	RunPress(o, &f, OPENBGI_PAD_SOUTH, 0, 2);
	CHECK(!Inframe_Up(o) && Request_Take(o, id, &answer) && answer == OPENBGI_IDNO);
	ScreenDown(o);
}

// ---- a host with windows of its own -------------------------------------------------------------

// the host's text entry: it records what it is told
static struct
{
	int shown, focus, destroyed, reject, moves, colours;
	int x, y, w, h;
	uint32_t colour;
	char text[0x100];
} gEntry;

static void EntryShow(void* user, int show)
{
	BGI_UNUSED(user);
	gEntry.shown = show;
}

static void EntryMove(void* user, int x, int y, int w, int h)
{
	BGI_UNUSED(user);
	gEntry.x = x;
	gEntry.y = y;
	gEntry.w = w;
	gEntry.h = h;
	gEntry.moves++;
}

static void EntrySetText(void* user, const char* text)
{
	BGI_UNUSED(user);
	snprintf(gEntry.text, sizeof gEntry.text, "%s", text);
}

static void EntryFocus(void* user, int toEntry)
{
	BGI_UNUSED(user);
	gEntry.focus = toEntry;
}

static int EntryGetText(void* user, char* buf, size_t n)
{
	BGI_UNUSED(user);
	snprintf(buf, n, "%s", gEntry.text);
	return (int)strlen(buf);
}

static void EntrySetColour(void* user, uint32_t rgb)
{
	BGI_UNUSED(user);
	gEntry.colour = rgb;
	gEntry.colours++;
}

static void EntryDestroy(void* user)
{
	BGI_UNUSED(user);
	gEntry.destroyed++;
}

static void EntryRejectAscii(void* user, int reject)
{
	BGI_UNUSED(user);
	gEntry.reject = reject;
}

static int EntryShown(void* user)
{
	BGI_UNUSED(user);
	return gEntry.shown;
}

static const OpenBGI_TextEntry_t kEntry = {.size = sizeof(OpenBGI_TextEntry_t), .show = EntryShow, .move = EntryMove, .setText = EntrySetText, .focus = EntryFocus, .getText = EntryGetText, .setColour = EntrySetColour, .destroy = EntryDestroy, .rejectAscii = EntryRejectAscii, .shown = EntryShown};

static void TestWindows(void)
{
	OpenBGI_t* o = ScreenUp(OPENBGI_CAP_DIALOGS | OPENBGI_CAP_POINTER | OPENBGI_CAP_KEYBOARD);
	OpenBGI_Frame_t f = {.size = sizeof f};
	OpenBGI_Rect_t r;
	OpenBGI_PadState_t rest;
	int kind;
	printf("windows\n");
	if(!o)
		return;
	memset(&gEntry, 0, sizeof gEntry);
	memset(&rest, 0, sizeof rest);
	rest.size = sizeof rest;
	o->host.textEntry = &kEntry;
	RunFrames(o, &f, 2);
	// the dialogs: the host's; the engine's while a pad has the turn - used last, or there with nothing used yet
	for(kind = OPENBGI_ASK_MESSAGE; kind <= OPENBGI_ASK_PROFILE; kind++)
		CHECK(!Inframe_Takes(o, kind));
	CHECK(!Pad_HasTurn(o));
	OpenBGI_Pad(o, 2, &rest); // a pad is plugged in
	CHECK(Pad_HasTurn(o) && o->input.device == INPUT_DEV_NONE);
	for(kind = OPENBGI_ASK_MESSAGE; kind <= OPENBGI_ASK_PROFILE; kind++)
		CHECK(Inframe_Takes(o, kind));
	CHECK(!Inframe_Takes(o, OPENBGI_ASK_FILE) && !Inframe_Takes(o, OPENBGI_ASK_GAME_CHOOSER)); // (always the host's)
	OpenBGI_Key(o, 'A', 1, 0);
	OpenBGI_Key(o, 'A', 0, 0);
	CHECK(!Pad_HasTurn(o) && !Inframe_Takes(o, OPENBGI_ASK_MESSAGE)); // a key was used: the pad is only there
	RunPress(o, &f, OPENBGI_PAD_WEST, 0, 2);
	CHECK(Pad_HasTurn(o) && Inframe_Takes(o, OPENBGI_ASK_MESSAGE));
	OpenBGI_Pointer(o, 300, 300);
	CHECK(!Pad_HasTurn(o) && !Inframe_Takes(o, OPENBGI_ASK_INPUT));
	o->input.device = INPUT_DEV_NONE; // (as a run starts)
	CHECK(Pad_HasTurn(o));
	OpenBGI_Pad(o, 2, NULL); // unplugged; the one the buttons came from is still there
	CHECK(Pad_HasTurn(o));
	OpenBGI_Pad(o, 0, NULL);
	CHECK(!Pad_HasTurn(o) && !Inframe_Takes(o, OPENBGI_ASK_MESSAGE));
	OpenBGI_Pad(o, 3, &rest);
	CHECK(Inframe_Takes(o, OPENBGI_ASK_MESSAGE));
	o->engine.optDialogs = 2; // --dialogs=host: the host's whatever is used
	CHECK(!Inframe_Takes(o, OPENBGI_ASK_MESSAGE));
	o->engine.optDialogs = 1; // --dialogs=frame
	OpenBGI_Pointer(o, 310, 310);
	CHECK(Inframe_Takes(o, OPENBGI_ASK_MESSAGE));
	o->engine.optDialogs = 0;
	CHECK(!Inframe_Takes(o, OPENBGI_ASK_MESSAGE));
	{ // a host with boxes alone (OPENBGI_CAP_MESSAGES): the messages are its own, the dialogs the engine's
		uint32_t caps = o->host.caps;
		o->host.caps = (caps & ~(uint32_t)OPENBGI_CAP_DIALOGS) | OPENBGI_CAP_MESSAGES;
		CHECK(!Inframe_Takes(o, OPENBGI_ASK_MESSAGE));
		for(kind = OPENBGI_ASK_INPUT; kind <= OPENBGI_ASK_PROFILE; kind++)
			CHECK(Inframe_Takes(o, kind));
		o->host.caps = caps & ~(uint32_t)(OPENBGI_CAP_DIALOGS | OPENBGI_CAP_MESSAGES);
		CHECK(Inframe_Takes(o, OPENBGI_ASK_MESSAGE)); // (a host with neither)
		o->host.caps = caps;
	}

	// a place told is no move of the mouse (the window came up under a pointer at rest): the pad keeps its turn
	RunPress(o, &f, OPENBGI_PAD_WEST, 0, 2);
	OpenBGI_PointerPlace(o, 500, 400);
	CHECK(o->input.device == INPUT_DEV_PAD && o->input.pointerX == 500 && o->input.pointerY == 400 && Inframe_Takes(o, OPENBGI_ASK_MESSAGE));
	OpenBGI_Pointer(o, 520, 400);
	CHECK(o->input.device == INPUT_DEV_MOUSE && o->input.pointerX == 520 && !Inframe_Takes(o, OPENBGI_ASK_MESSAGE));
	o->input.device = INPUT_DEV_NONE;
	OpenBGI_PointerPlace(o, 300, 300); // .. and with nothing used yet, nothing is
	CHECK(o->input.device == INPUT_DEV_NONE && o->input.pointerX == 300);

	// the text entry under a mouse: the host's
	OpenBGI_Pointer(o, 310, 310);
	Edit_ToFrame(o); // (nothing was set up yet: nothing to move)
	CHECK(!o->edit.own && Edit_Rect(o, &r) == 0);
	Edit_SetText(o, "abc");
	CHECK(Edit_Setup(o, 100, 50, 300, 30, 0, 20, 20, 1) == 0);
	CHECK(!o->edit.own && gEntry.shown && gEntry.focus && gEntry.x == 100 && gEntry.w == 300 && strcmp(gEntry.text, "abc") == 0);
	CHECK(Edit_Rect(o, &r) == 0 && Edit_Shown(o));
	RunFrames(o, &f, 2);
	CHECK(f.textInput == 0); // (the host's control has its own input method)
	// the pad is taken up: the entry moves into the picture, with what was typed, the caret at its end
	snprintf(gEntry.text, sizeof gEntry.text, "abcd");
	RunPress(o, &f, OPENBGI_PAD_WEST, 0, 3);
	CHECK(o->edit.own && !gEntry.shown && Edit_Rect(o, &r) == 2 && r.x == 100 && r.y == 50 && r.w == 300 && r.h == 30);
	CHECK(EntryIs(o, "abcd") && Edit_Shown(o) && Osk_Up(o));
	Steps_TextNow(o, "e");
	CHECK(EntryIs(o, "abcde"));
	// what the scripts set meanwhile is the engine's entry's; the host's hears of it when it is back
	Edit_SetColour(o, 0x112233);
	Edit_RejectAscii(o, 1);
	CHECK(gEntry.colours == 0 && gEntry.reject == 0);
	Edit_Show(o, 0);
	CHECK(!gEntry.shown && Edit_Rect(o, &r) == 0 && !Edit_Shown(o));
	// the next setup under a pad is the engine's as well
	CHECK(Edit_Setup(o, 120, 60, 200, 30, 0, 20, 20, 1) == 0);
	CHECK(o->edit.own && !gEntry.shown && gEntry.moves == 1 && Edit_Rect(o, &r) == 2 && r.x == 120 && EntryIs(o, "abc"));
	// .. and under a keyboard the host's: the engine's goes, the host's is told the colour and "B0 29"
	OpenBGI_Key(o, 'A', 1, 0);
	OpenBGI_Key(o, 'A', 0, 0);
	Edit_SetText(o, "xyz");
	CHECK(Edit_Setup(o, 140, 70, 200, 30, 0, 20, 20, 1) == 0);
	CHECK(!o->edit.own && gEntry.shown && gEntry.x == 140 && strcmp(gEntry.text, "xyz") == 0 && Edit_Rect(o, &r) == 0);
	CHECK(gEntry.colours == 1 && gEntry.colour == 0x112233 && gEntry.reject == 1);
	RunFrames(o, &f, 2);
	CHECK(!Osk_Up(o));
	// an entry of the host's that is hidden stays where it is when the pad is taken up
	Edit_Show(o, 0);
	RunPress(o, &f, OPENBGI_PAD_WEST, 0, 2);
	CHECK(!o->edit.own && Edit_Rect(o, &r) == 0 && !gEntry.shown);
	// a pad that is only there, with nothing used yet: the setup's entry is the engine's
	o->input.device = INPUT_DEV_NONE;
	CHECK(Edit_Setup(o, 100, 50, 300, 30, 0, 20, 20, 1) == 0);
	CHECK(o->edit.own && !gEntry.shown && Edit_Rect(o, &r) == 2);
	// "B0 21" and the window's end: both entries go
	Edit_Remove(o);
	CHECK(Edit_Rect(o, &r) == 0 && !gEntry.shown);
	CHECK(Edit_Setup(o, 100, 50, 300, 30, 0, 20, 20, 1) == 0 && o->edit.own);
	ScreenDown(o);
	CHECK(gEntry.destroyed == 1);
}

// ---- the display settings ----------------------------------------------------------------

static int gViewRule = -1; // what the host was last told to scale by

static void ViewWorkArea(void* user, int* x, int* y, int* w, int* h)
{
	BGI_UNUSED(user);
	*x = *y = 0;
	*w = 1920;
	*h = 1040;
}

static void ViewSetScaling(void* user, int rule)
{
	BGI_UNUSED(user);
	gViewRule = rule;
}

// a window table of a host that fits the window to its screen, and nothing else
static const OpenBGI_Window_t kViewWindow = {.size = sizeof(OpenBGI_Window_t), .setScaling = ViewSetScaling, .workArea = ViewWorkArea};

static const char* ViewFilterName(void* user, int n)
{
	BGI_UNUSED(user);
	return n == 0 ? "Sharp" : NULL;
}

// .. and one with a filter of its own
static const OpenBGI_Window_t kFilterWindow = {.size = sizeof(OpenBGI_Window_t), .setScaling = ViewSetScaling, .workArea = ViewWorkArea, .filterName = ViewFilterName};

static void TestDisplay(void)
{
	OpenBGI_t* o = ScreenUp(OPENBGI_CAP_DIALOGS | OPENBGI_CAP_POINTER | OPENBGI_CAP_KEYBOARD);
	OpenBGI_Frame_t f = {.size = sizeof f};
	OpenBGI_Request_t q = {.size = sizeof q};
	char out[0x100] = "";
	int size, filter, whole, answer = -1;
	uint32_t id;
	printf("display\n");
	if(!o)
		return;
	RunFrames(o, &f, 2);
	InputMap_Configure(o, NULL, NULL); // (as a run starts: the key is F9)
	// a host that does not fit the window: the key is the game's
	o->host.window = NULL;
	OpenBGI_Key(o, 0x78, 1, 0);
	CHECK(!Inframe_Up(o) && Input_KeyDown(o, 0x78));
	OpenBGI_Key(o, 0x78, 0, 0);
	CHECK(!Input_KeyDown(o, 0x78));
	// one that does: the settings come up, whoever shows the dialogs; the game never sees the key
	o->host.window = &kViewWindow;
	OpenBGI_Key(o, 0x78, 1, 0);
	CHECK(Inframe_Up(o) && Inframe_SettingsUp(o) && !Input_KeyDown(o, 0x78));
	OpenBGI_Key(o, 0x78, 1, OPENBGI_KEY_REPEAT);
	CHECK(Inframe_SettingsUp(o));
	OpenBGI_Key(o, 0x78, 0, 0);
	RunFrames(o, &f, 2);
	CHECK(Inframe_SettingsUp(o) && f.textInput == 0 && Inframe_Focus(o) == INFRAME_FOCUS_PICKER);
	// the size picker has the focus: Down steps it on, typing does nothing
	OpenBGI_Key(o, 0x28, 1, 0);
	OpenBGI_Key(o, 0x28, 0, 0);
	MainWin_ViewSettings(o, &size, &filter, &whole);
	CHECK(size == VIEW_SIZE_100 && filter == VIEW_FILTER_GAME && !whole);
	OpenBGI_Key(o, 'A', 1, 0);
	OpenBGI_Text(o, "a");
	OpenBGI_Key(o, 'A', 0, 0);
	OpenBGI_Key(o, 0x26, 1, 0);
	OpenBGI_Key(o, 0x26, 0, 0);
	MainWin_ViewSettings(o, &size, &filter, &whole);
	CHECK(size == VIEW_SIZE_AUTO);
	// Tab to the filter; Right steps as Down does: the host scales by the player's rule
	OpenBGI_Key(o, 0x09, 1, 0);
	OpenBGI_Key(o, 0x09, 0, 0);
	OpenBGI_Key(o, 0x27, 1, 0);
	OpenBGI_Key(o, 0x27, 0, 0);
	MainWin_ViewSettings(o, &size, &filter, &whole);
	CHECK(filter == VIEW_FILTER_BICUBIC && gViewRule == (OPENBGI_SCALE_BICUBIC | OPENBGI_SCALE_CHOSEN));
	// Tab to the steps; Space steps on
	OpenBGI_Key(o, 0x09, 1, 0);
	OpenBGI_Key(o, 0x09, 0, 0);
	OpenBGI_Key(o, 0x20, 1, 0);
	OpenBGI_Key(o, 0x20, 0, 0);
	MainWin_ViewSettings(o, &size, &filter, &whole);
	CHECK(whole == 1 && gViewRule == (OPENBGI_SCALE_BICUBIC | OPENBGI_SCALE_CHOSEN | OPENBGI_SCALE_WHOLE));
	OpenBGI_Key(o, 0x20, 1, 0);
	OpenBGI_Key(o, 0x20, 0, 0);
	MainWin_ViewSettings(o, &size, &filter, &whole);
	CHECK(whole == 0);
	// Escape closes them; the key brings them back with what was chosen, and puts them down
	OpenBGI_Key(o, 0x1b, 1, 0);
	OpenBGI_Key(o, 0x1b, 0, 0);
	CHECK(!Inframe_Up(o));
	OpenBGI_Key(o, 0x78, 1, 0);
	OpenBGI_Key(o, 0x78, 0, 0);
	CHECK(Inframe_SettingsUp(o));
	MainWin_ViewSettings(o, &size, &filter, &whole);
	CHECK(filter == VIEW_FILTER_BICUBIC);
	OpenBGI_Key(o, 0x78, 1, 0);
	OpenBGI_Key(o, 0x78, 0, 0);
	CHECK(!Inframe_Up(o));
	MainWin_ViewApply(o, VIEW_SIZE_AUTO, VIEW_FILTER_GAME, 0);
	// a host with a filter of its own: it comes after the four, and its quality has a picker after the filter's
	o->host.window = &kFilterWindow;
	OpenBGI_Key(o, 0x78, 1, 0);
	OpenBGI_Key(o, 0x78, 0, 0);
	CHECK(Inframe_SettingsUp(o));
	OpenBGI_Key(o, 0x09, 1, 0); // to the filter; Up from the game's: the last one, the host's
	OpenBGI_Key(o, 0x09, 0, 0);
	OpenBGI_Key(o, 0x26, 1, 0);
	OpenBGI_Key(o, 0x26, 0, 0);
	MainWin_ViewSettings(o, &size, &filter, &whole);
	CHECK(filter == VIEW_FILTER_OWN && gViewRule == (OPENBGI_SCALE_OWN | OPENBGI_SCALE_CHOSEN));
	OpenBGI_Key(o, 0x09, 1, 0); // to the quality
	OpenBGI_Key(o, 0x09, 0, 0);
	OpenBGI_Key(o, 0x28, 1, 0);
	OpenBGI_Key(o, 0x28, 0, 0);
	CHECK(MainWin_ViewHigh(o) == 1 && gViewRule == (OPENBGI_SCALE_OWN | OPENBGI_SCALE_CHOSEN | OPENBGI_SCALE_HIGH));
	OpenBGI_Key(o, 0x09, 1, 0); // to the steps, as before
	OpenBGI_Key(o, 0x09, 0, 0);
	OpenBGI_Key(o, 0x20, 1, 0);
	OpenBGI_Key(o, 0x20, 0, 0);
	MainWin_ViewSettings(o, &size, &filter, &whole);
	CHECK(whole == 1 && (gViewRule & OPENBGI_SCALE_WHOLE));
	OpenBGI_Key(o, 0x1b, 1, 0);
	OpenBGI_Key(o, 0x1b, 0, 0);
	CHECK(!Inframe_Up(o));
	MainWin_ViewSetHigh(o, 0);
	MainWin_ViewApply(o, VIEW_SIZE_AUTO, VIEW_FILTER_GAME, 0);
	o->host.window = &kViewWindow;
	// another key by the settings, or none
	CHECK(OpenBGI_InputSettings(o, "{\"settingsKey\": \"F8\"}"));
	OpenBGI_Key(o, 0x78, 1, 0);
	CHECK(!Inframe_Up(o) && Input_KeyDown(o, 0x78));
	OpenBGI_Key(o, 0x78, 0, 0);
	OpenBGI_Key(o, 0x77, 1, 0);
	OpenBGI_Key(o, 0x77, 0, 0);
	CHECK(Inframe_SettingsUp(o));
	OpenBGI_Key(o, 0x77, 1, 0);
	OpenBGI_Key(o, 0x77, 0, 0);
	CHECK(!Inframe_Up(o));
	CHECK(OpenBGI_InputSettings(o, "{\"settingsKey\": \"none\"}"));
	OpenBGI_Key(o, 0x77, 1, 0);
	OpenBGI_Key(o, 0x77, 0, 0);
	OpenBGI_Key(o, 0x78, 1, 0);
	OpenBGI_Key(o, 0x78, 0, 0);
	CHECK(!Inframe_Up(o));
	CHECK(OpenBGI_InputSettings(o, NULL));
	// not over another dialog: the key is the dialog's then
	o->engine.optDialogs = 1;
	memset(&q, 0, sizeof q);
	q.size = sizeof q;
	q.kind = OPENBGI_ASK_INPUT;
	q.u.input.title = "t";
	q.u.input.initial = "";
	q.u.input.maxLen = 8;
	q.u.input.out = out;
	id = Request_Post(o, &q, REQUEST_TREAT_NONE);
	RunFrames(o, &f, 3);
	CHECK(Inframe_Up(o) && !Inframe_SettingsUp(o));
	OpenBGI_Key(o, 0x78, 1, 0);
	OpenBGI_Key(o, 0x78, 0, 0);
	CHECK(Inframe_Up(o) && !Inframe_SettingsUp(o));
	OpenBGI_Key(o, 0x1b, 1, 0);
	OpenBGI_Key(o, 0x1b, 0, 0);
	RunFrames(o, &f, 2);
	CHECK(!Inframe_Up(o) && Request_Take(o, id, &answer) && answer == 0);
	o->engine.optDialogs = 0;
	o->host.window = NULL;
	ScreenDown(o);
}

static void TestRestart(void)
{
	OpenBGI_t* o;
	OpenBGI_PadState_t rest;
	printf("restart\n");
	gHost = *OS_Host();
	gHost.ticksMs = TicksMs;
	o = OpenBGI_New(&gHost);
	CHECK(o != NULL);
	if(!o)
		return;
	memset(&rest, 0, sizeof rest);
	rest.size = sizeof rest;
	OpenBGI_Pad(o, 2, &rest);
	OpenBGI_TouchScale(o, 160);
	o->input.device = INPUT_DEV_MOUSE;
	o->input.pointerX = 77;
	// an engine made new for its next run: what the host said of its devices stays, as do its services
	Engine_Renew(o);
	CHECK(o->input.pad.attached == 4 && o->input.touch.ppi == 160 && Pad_HasTurn(o));
	CHECK(o->input.device == INPUT_DEV_NONE && o->input.pointerX != 77);
	CHECK(o->host.ticksMs == TicksMs && o->host.size == sizeof(OpenBGI_Host_t) && o->host.files == &o->hostTables.files);
	CHECK(o->hostTables.files.size == sizeof(OpenBGI_Files_t) && o->hostTables.files.open == OS_Host()->files->open);
	OpenBGI_Pad(o, 2, NULL);
	Engine_Renew(o);
	CHECK(o->input.pad.attached == 0 && !Pad_HasTurn(o));
	OpenBGI_Delete(o);
}

int main(void)
{
	OS_Init();
	TestConfirm();
	TestFocus();
	TestPick();
	TestRepeat();
	TestCancel();
	TestUpkeep();
	TestStick();
	TestActions();
	TestOwn();
	TestKnob();
	TestDevice();
	TestSettings();
	TestArrow();
	TestKeyboard();
	TestDialog();
	TestWindows();
	TestDisplay();
	TestRestart();
	OS_Shutdown();
	if(gFails)
	{
		printf("pad: %d checks failed\n", gFails);
		return 1;
	}
	printf("pad: all tests passed\n");
	return 0;
}
