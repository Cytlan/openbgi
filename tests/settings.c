/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/settings.c - unit test of the settings over a running game
 *                    (src/sys/settings.c, bgi/settings.h): the panel's
 *                    engine, its see-through picture ("EF 31") in the
 *                    screen layer, the input it takes and what its
 *                    program reads and sets; built as test_settings by
 *                    `make test`
 *
 * The "game" is an engine as tests/overlay.c has it - its graphics, a
 * picture of 640 x 480, a surface of 600 x 300 at 160 pixels an inch with
 * the picture at (100, 0) as 400 x 300 - whose launcher program is the
 * test's own, compiled here.  That program is a panel in small: it notes
 * what it reads of the path "" in its memory, makes its picture the
 * surface's size and see-through, shows an opaque sprite of 100 x 50 at
 * (200, 100) and a half clear one of 40 x 40 at (10, 200), counts the
 * clicks, turns the pad on at Enter and asks to be closed at Escape.
 * A frame is the panel's (Settings_Frame) and the layer's
 * (Overlay_Frame), as OpenBGI_RunFrame has them.  The groups:
 *
 *   closed  - without a launcher program, or without a surface, nothing
 *             opens; "mode" is "launcher" outside a panel, and the
 *             panel's settings are no settings there
 *   open    - the settings button opens the panel: an engine of its own
 *             that runs the program; what the program read; its picture
 *             ARGB32 at the surface's size
 *   layer   - the picture in the layer under the controls: the sprites
 *             with their alpha, clear around them, the settings button
 *             over it; a part that changes; all clear again when the
 *             panel goes
 *   input   - a touch beside the controls is the panel's, in the
 *             surface's pixels, and none of the game's; the on-screen
 *             pad plays the panel; keys and buttons of the host go to it
 *   bridge  - "pad" set by the program turns the game's pad on and is
 *             kept; "panel": "close" puts the panel away at the frame's
 *             end; the settings button closes it too
 *   options - what the settings screen reads and sets besides: "through"
 *             (touches beside the pad), "perf" (the performance display,
 *             whose panel lies under the settings' picture), "game" (which engine is under
 *             the panel; "close" ends it as the window's close button
 *             does, after the panel went); with a host that scales the
 *             picture (setScaling; a surface was said) its filters by
 *             name ("filters", "filter": the four every host has, then
 *             the host's own), "quality" and "whole", each told to the
 *             host as its scaling rule; with a host that does not, none
 *             of the three is a value
 *   ends    - a panel whose program ends is put away; one whose program
 *             raises a script error too, and the game's engine shows
 *             the message; the game's stop and the surface's end take
 *             it along
 */
#include "bgi/core.h"
#include "bgi/engine.h"
#include "bgi/input.h"
#include "bgi/inputmap.h"
#include "bgi/overlay.h"
#include "bgi/settings.h"
#include "bgi/perf.h"
#include "bgi/launcher.h"
#include "bgi/version.h"
#include "bgi/bpc.h"
#include "bgi/gfx.h"
#include "bgi/display.h"
#include "bgi/sysobj/events.h"
#include "host/os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

#define W  640
#define H  480
#define SW 600
#define SH 300

static uint32_t gNow = 1000;
static uint8_t gPicture[W * H * 4];
static OpenBGI_Frame_t gLast;
static OpenBGI_Host_t gHost;
static int gRule = -1; // what the host was last told to scale by

static uint32_t TicksMs(void* user)
{
	(void)user;
	return gNow;
}

static void SetScaling(void* user, int rule)
{
	(void)user;
	gRule = rule;
}

static const char* FilterName(void* user, int n)
{
	(void)user;
	return n == 0 ? "Sharp" : n == 1 ? "Soft"
									 : NULL;
}

// a window table of a host that scales the picture and has two filters of its own, and nothing else
// (no work area: it does not fit a window to a screen, as a phone's does not)
static const OpenBGI_Window_t kScales = {.size = sizeof(OpenBGI_Window_t), .setScaling = SetScaling, .filterName = FilterName};

// where the program keeps what the test looks at (its global memory)
#define AT_MODE   0x1000
#define AT_SURF_W 0x1020
#define AT_SURF_H 0x1040
#define AT_SURF_D 0x1060
#define AT_PAD    0x1080
#define AT_NONE   0x10a0 // what a fourth value of "surface" answered
#define AT_CLICKS 0x10a4
#define AT_X      0x10a8
#define AT_Y      0x10ac
#define AT_RIGHT  0x10b0
#define AT_TURNS  0x10b4
#define AT_GROW   0x10b8 // set by the test: the opaque sprite moves

static const char kPanel[] = "#engine 1.669\n"
							 "global char gMode[32] @ 0x1000;\n"
							 "global char gSurfW[32] @ 0x1020;\n"
							 "global char gSurfH[32] @ 0x1040;\n"
							 "global char gSurfD[32] @ 0x1060;\n"
							 "global char gPad[32] @ 0x1080;\n"
							 "global int gNone @ 0x10a0;\n"
							 "global int gClicks @ 0x10a4;\n"
							 "global int gX @ 0x10a8;\n"
							 "global int gY @ 0x10ac;\n"
							 "global int gRight @ 0x10b0;\n"
							 "global int gTurns @ 0x10b4;\n"
							 "global int gGrow @ 0x10b8;\n"
							 "global char gTmp[32] @ 0x10c0;\n"
							 "start() { int a; int b; int down; int was; int moved;\n"
							 "    sys.set_gmem_size(1);\n"
							 "    obgi.cfg_get(\"\", \"mode\", 0, gMode, 32);\n"
							 "    obgi.cfg_get(\"\", \"surface\", 0, gSurfW, 32);\n"
							 "    obgi.cfg_get(\"\", \"surface\", 1, gSurfH, 32);\n"
							 "    obgi.cfg_get(\"\", \"surface\", 2, gSurfD, 32);\n"
							 "    gNone = obgi.cfg_get(\"\", \"surface\", 3, gTmp, 32);\n"
							 "    obgi.cfg_get(\"\", \"pad\", 0, gPad, 32);\n"
							 "    obgi.see_through(1);\n"
							 "    sys81.set_size_entry(0, 600, 300);\n"
							 "    sys.set_display_mode(0, 1, 0);\n"
							 "    gfx0.bmp_create(5, 100, 50, 2);\n"
							 "    gfx0.bmp_clear(5, 0xff2040e0);\n"
							 "    a = gfx0.sprite_create();\n"
							 "    gfx0.sprite_set(a, 200, 100, 5, 1, 0, 100);\n"
							 "    gfx0.sprite_show(a, 1);\n"
							 "    gfx0.bmp_create(6, 40, 40, 2);\n"
							 "    gfx0.bmp_clear(6, 0x80ffffff);\n"
							 "    b = gfx0.sprite_create();\n"
							 "    gfx0.sprite_set(b, 10, 200, 6, 1, 0, 101);\n"
							 "    gfx0.sprite_show(b, 1);\n"
							 "    sys.input_layer_push(8);\n"
							 "    sys.show_window(1);\n"
							 "    was = 0;\n"
							 "    moved = 0;\n"
							 "    while (1) {\n"
							 "        gTurns = gTurns + 1;\n"
							 "        down = sys.key_down(1);\n"
							 "        if (down && !was) {\n"
							 "            sys.get_mouse_pos();\n"
							 "            gY <- __pop;\n"
							 "            gX <- __pop;\n"
							 "            gClicks = gClicks + 1;\n"
							 "        }\n"
							 "        was = down;\n"
							 "        if (sys.key_down(2)) gRight = 1;\n"
							 "        if (sys.key_trigger(8, 13)) obgi.cfg_set(\"\", \"pad\", 0, \"on\");\n"
							 "        if (sys.key_trigger(8, 27)) obgi.cfg_set(\"\", \"panel\", 0, \"close\");\n"
							 "        if (sys.key_trigger(8, 35)) return 0;\n"
							 "        if (sys.key_trigger(8, 36)) { *(int*)0 = 1; }\n"
							 "        if (gGrow && !moved) {\n"
							 "            moved = 1;\n"
							 "            gfx0.sprite_set(a, 300, 100, 5, 1, 0, 100);\n"
							 "        }\n"
							 "        gfx0.present(0);\n"
							 "        sys.timer_set(5);\n"
							 "        sys.timer_wait();\n"
							 "    }\n"
							 "}\n";

// ---- the game's engine ----------------------------------------------------------------------------

static OpenBGI_t* EngineUp(int program)
{
	OpenBGI_t* o;
	gHost = *OS_Host();
	gHost.ticksMs = TicksMs;
	gHost.window = program == 2 ? &kScales : NULL;
	gRule = -1;
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
	if(program)
	{ // the launcher's program as its last run would have left it: the test's
		BpcResult_t res;
		char* said = NULL;
		memset(&res, 0, sizeof res);
		Bpc_CompileText(o, kPanel, (uint32_t)(sizeof kPanel - 1), "panel.bpc", GEN_COUNT, NULL, &said, &res);
		if(said && said[0])
			printf("  the test's program: %s", said);
		free(said);
		BGI_Free(res.labels);
		CHECK(!res.errors && res.file);
		o->launcher.kept = res.file;
		o->launcher.keptSize = res.size;
	}
	return o;
}

static void EngineDown(OpenBGI_t* o)
{
	Settings_Close(o);
	MsgQueue_Clear(o);
	Input_ResetLayers(o);
	memset(&o->screen.pic, 0, sizeof o->screen.pic);
	o->display.windowAlive = 0;
	Gfx_Destroy(o);
	BGI_Free(o->launcher.kept);
	o->launcher.kept = NULL;
	OpenBGI_Delete(o);
}

static void Surface(OpenBGI_t* o)
{
	OpenBGI_Surface_t s;
	memset(&s, 0, sizeof s);
	s.size = sizeof s;
	s.width = SW;
	s.height = SH;
	s.picture.x = 100;
	s.picture.w = 400;
	s.picture.h = 300;
	s.dpi = 160;
	OpenBGI_SetSurface(o, &s);
}

// `count` frames of the game's engine as far as the settings go: its input, the panel's frame, the layer
static void Frames(OpenBGI_t* o, int count)
{
	while(count-- > 0)
	{
		InputMap_Frame(o);
		memset(&gLast, 0, sizeof gLast);
		gLast.size = sizeof gLast;
		gLast.idleMs = 16;
		Settings_Frame(o, &gLast);
		Overlay_Frame(o, &gLast);
		gNow += 8;
	}
}

static const uint8_t* At(int x, int y)
{
	static const uint8_t kNone[4] = {1, 2, 3, 4};
	if(!gLast.layer || x < 0 || y < 0 || x >= gLast.layerWidth || y >= gLast.layerHeight)
		return kNone;
	return gLast.layer + (size_t)y * (size_t)gLast.layerPitch + (size_t)x * 4;
}

static int Is(int x, int y, int b, int g, int r, int a)
{
	const uint8_t* p = At(x, y);
	return p[0] == b && p[1] == g && p[2] == r && p[3] == a;
}

// the panel's memory
static const char* Str(OpenBGI_t* o, int at)
{
	OpenBGI_t* p = o->settings.panel;
	return p && p->mem.globalMem ? (const char*)p->mem.globalMem + at : "";
}

static int Int(OpenBGI_t* o, int at)
{
	OpenBGI_t* p = o->settings.panel;
	int32_t v = -99;
	if(p && p->mem.globalMem)
		memcpy(&v, p->mem.globalMem + at, sizeof v);
	return (int)v;
}

static void Tap(OpenBGI_t* o, int k)
{
	const OverlayControl_t* c = &o->overlay.c[k];
	OpenBGI_SurfaceTouch(o, 9, OPENBGI_TOUCH_DOWN, c->cx, c->cy);
	Frames(o, 2);
	OpenBGI_SurfaceTouch(o, 9, OPENBGI_TOUCH_UP, c->cx, c->cy);
	Frames(o, 2);
}

// a game with its panel up and the program at its loop; `scales`: its host scales the picture
static OpenBGI_t* PanelUp(int scales)
{
	OpenBGI_t* o = EngineUp(scales ? 2 : 1);
	if(!o)
		return NULL;
	Surface(o);
	Frames(o, 1);
	CHECK(Settings_Open(o));
	Frames(o, 6);
	return o;
}

// ---- the groups -----------------------------------------------------------------------------------

static void TestClosed(void)
{
	OpenBGI_t* o = EngineUp(0);
	char v[32];
	printf("closed\n");
	if(!o)
		return;
	// no surface: no layer to show a panel in
	CHECK(!Settings_Open(o) && !Settings_Up(o));
	Surface(o);
	Frames(o, 1);
	// no launcher program: nothing to run
	CHECK(!Settings_Open(o) && !Settings_Up(o));
	Tap(o, OVERLAY_MENU);
	CHECK(!Settings_Up(o) && !(o->overlay.held & 1u << OVERLAY_MENU));
	// what a program reads of the path "" outside a panel
	CHECK(Settings_Value(o, "mode", 0, v, sizeof v) == 1 && strcmp(v, "launcher") == 0);
	CHECK(Settings_Value(o, "mode", 1, v, sizeof v) == -1);
	CHECK(Settings_Value(o, "surface", 0, v, sizeof v) == 0 && Settings_Value(o, "pad", 0, v, sizeof v) == 0);
	CHECK(Settings_Value(o, "panel", 0, v, sizeof v) == 0 && Settings_Value(o, "width", 0, v, sizeof v) == 0);
	CHECK(Settings_Set(o, "mode", "settings") == 0); // (only read)
	CHECK(Settings_Set(o, "pad", "on") == -1 && Settings_Set(o, "panel", "close") == -1 && Settings_Set(o, "width", "1") == -1);
	CHECK(!Overlay_PadOn(o));
	// nothing is the panel's while there is none
	CHECK(!Settings_Touch(o, 1, OPENBGI_TOUCH_DOWN, 5, 5) && !Settings_Key(o, 13, 1, 0) && !Settings_Button(o, OPENBGI_BUTTON_LEFT, 1, 0));
	CHECK(!Settings_Pad(o, NULL));
	EngineDown(o);
}

static void TestOpen(void)
{
	OpenBGI_t* o = EngineUp(1);
	OpenBGI_t* p;
	char v[32];
	printf("open\n");
	if(!o)
		return;
	Surface(o);
	Frames(o, 2);
	CHECK(!Settings_Up(o));
	Tap(o, OVERLAY_MENU);
	CHECK(Settings_Up(o));
	p = o->settings.panel;
	if(!p)
	{
		EngineDown(o);
		return;
	}
	Frames(o, 6);
	// an engine of its own, which knows the game's; the game's knows none above it
	CHECK(p != o && p->settings.game == o && !p->settings.panel && !o->settings.game);
	CHECK(p->engine.phase != 0 && Int(o, AT_TURNS) > 0);
	// what its program read
	CHECK(strcmp(Str(o, AT_MODE), "settings") == 0);
	CHECK(strcmp(Str(o, AT_SURF_W), "600") == 0 && strcmp(Str(o, AT_SURF_H), "300") == 0 && strcmp(Str(o, AT_SURF_D), "160") == 0);
	CHECK(Int(o, AT_NONE) == -1 && strcmp(Str(o, AT_PAD), "off") == 0);
	// its picture: the surface's size, with alpha; its gestures by the surface's density
	CHECK(p->gfx.seeThrough && p->screen.pic.mode == PM_ARGB32 && p->screen.pic.w == SW && p->screen.pic.h == SH);
	CHECK(p->input.touch.ppi == 160);
	// asked for again: it is up
	CHECK(Settings_Open(o) && o->settings.panel == p);
	// a panel opens none of its own
	CHECK(!Settings_Open(p));
	CHECK(Settings_Value(p, "mode", 0, v, sizeof v) == 1 && strcmp(v, "settings") == 0);
	CHECK(Settings_Value(p, "panel", 0, v, sizeof v) == -1 && Settings_Value(p, "width", 0, v, sizeof v) == 0);
	CHECK(Settings_Set(p, "surface", "1") == 0 && Settings_Set(p, "pad", "maybe") == 0 && Settings_Set(p, "panel", "open") == 0);
	EngineDown(o);
}

static void TestLayer(void)
{
	OpenBGI_t* o = PanelUp(0);
	const OverlayControl_t* m;
	int32_t one = 1;
	printf("layer\n");
	if(!o)
		return;
	if(!Settings_Up(o))
	{
		EngineDown(o);
		return;
	}
	m = &o->overlay.c[OVERLAY_MENU];
	// the opaque sprite: its colour and all of its alpha (0xff2040e0: red 0x20, green 0x40, blue 0xe0)
	CHECK(Is(200, 100, 0xe0, 0x40, 0x20, 255) && Is(299, 149, 0xe0, 0x40, 0x20, 255) && Is(250, 125, 0xe0, 0x40, 0x20, 255));
	// around it: clear
	CHECK(At(199, 100)[3] == 0 && At(300, 149)[3] == 0 && At(250, 99)[3] == 0 && At(250, 150)[3] == 0 && At(450, 250)[3] == 0);
	// the half clear one: white at half
	CHECK(At(20, 220)[3] >= 127 && At(20, 220)[3] <= 129 && At(20, 220)[0] >= 254 && At(20, 220)[2] >= 254);
	// the settings button over the panel's picture: drawn, and light while the panel is up
	CHECK(At(m->cx + m->r * 7 / 10, m->cy)[3] > 0x60);
	// the frame says nothing changed while nothing does
	Frames(o, 3);
	CHECK(gLast.layer && gLast.layerChanges == 0);
	// the sprite moves 100 to the right: the layer follows, and the frame says where
	memcpy(o->settings.panel->mem.globalMem + AT_GROW, &one, sizeof one);
	Frames(o, 4);
	CHECK(Is(350, 125, 0xe0, 0x40, 0x20, 255) && Is(399, 149, 0xe0, 0x40, 0x20, 255) && At(250, 125)[3] == 0 && At(299, 100)[3] == 0);
	Frames(o, 2);
	CHECK(gLast.layerChanges == 0);
	// the panel goes: all of the layer anew, clear but for the controls
	Settings_Close(o);
	Frames(o, 1);
	CHECK(gLast.layer && gLast.layerChanges == -1);
	CHECK(At(350, 125)[3] == 0 && At(20, 220)[3] == 0 && At(m->cx + m->r * 7 / 10, m->cy)[3] > 0 && At(m->cx + m->r * 7 / 10, m->cy)[3] < 0x60);
	EngineDown(o);
}

static void TestInput(void)
{
	OpenBGI_t* o = PanelUp(0);
	OpenBGI_t* p;
	OpenBGI_PadState_t s;
	printf("input\n");
	if(!o)
		return;
	p = o->settings.panel;
	if(!p)
	{
		EngineDown(o);
		return;
	}
	// a tap beside the controls: the panel's, where the finger is on the surface; the game's pointer rests
	o->input.touch.tapValid = 0;
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_DOWN, 250, 120);
	Frames(o, 3);
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_UP, 250, 120);
	Frames(o, 12);
	CHECK(Int(o, AT_CLICKS) == 1 && Int(o, AT_X) == 250 && Int(o, AT_Y) == 120);
	CHECK(!o->input.touch.tapValid && !(o->input.keyboard[1] & 0x80));
	// .. even with the pad on and the picture left to the pad alone: the panel is no picture
	Overlay_SetPad(o, 1, 0);
	o->overlay.padOnly = 1;
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_DOWN, 450, 40);
	Frames(o, 3);
	OpenBGI_SurfaceTouch(o, 1, OPENBGI_TOUCH_UP, 450, 40);
	Frames(o, 12);
	CHECK(Int(o, AT_CLICKS) == 2 && Int(o, AT_X) == 450 && Int(o, AT_Y) == 40);
	o->overlay.padOnly = 0;
	// the on-screen pad plays the panel, not the game
	{
		const OverlayControl_t* c = &o->overlay.c[OVERLAY_CANCEL];
		OpenBGI_SurfaceTouch(o, 2, OPENBGI_TOUCH_DOWN, c->cx, c->cy);
		CHECK(p->input.pad.port[PAD_PORT_OWN].buttons == OPENBGI_PAD_EAST && o->input.pad.port[PAD_PORT_OWN].buttons == 0);
		Frames(o, 6);
		CHECK(Int(o, AT_RIGHT) == 1); // (Cancel is the right button)
		OpenBGI_SurfaceTouch(o, 2, OPENBGI_TOUCH_UP, c->cx, c->cy);
		CHECK(p->input.pad.port[PAD_PORT_OWN].buttons == 0);
		Frames(o, 4);
	}
	// the host's own pads stay the game's
	memset(&s, 0, sizeof s);
	s.size = sizeof s;
	s.buttons = OPENBGI_PAD_NORTH;
	OpenBGI_Pad(o, 0, &s);
	CHECK(o->input.pad.buttons == OPENBGI_PAD_NORTH && p->input.pad.buttons == 0);
	OpenBGI_Pad(o, 0, NULL);
	// the host's keys and buttons are the panel's; its pointer and wheel move nothing of the game
	o->input.pointerX = 7;
	o->input.pointerY = 9;
	OpenBGI_Pointer(o, 300, 200);
	OpenBGI_Wheel(o, 120);
	OpenBGI_Key(o, 0x41, 1, 0);
	CHECK(o->input.pointerX == 7 && o->input.pointerY == 9 && !(o->input.keyboard[0x41] & 0x80) && (p->input.keyboard[0x41] & 0x80));
	OpenBGI_Key(o, 0x41, 0, 0);
	CHECK(!(p->input.keyboard[0x41] & 0x80));
	EngineDown(o);
}

static void TestBridge(void)
{
	OpenBGI_t* o = PanelUp(0);
	char v[32];
	printf("bridge\n");
	if(!o)
		return;
	if(!Settings_Up(o))
	{
		EngineDown(o);
		return;
	}
	// Enter: the program sets "pad" to "on" - the game's pad comes, and the choice is kept
	CHECK(!Overlay_PadOn(o));
	OpenBGI_Key(o, 13, 1, 0);
	Frames(o, 4);
	OpenBGI_Key(o, 13, 0, 0);
	Frames(o, 4);
	CHECK(Overlay_PadOn(o));
	CHECK(Engine_CfgGet(o, "", "touchPad", 0, v, sizeof v) == 2 && strcmp(v, "on") == 0);
	CHECK(Settings_Value(o->settings.panel, "pad", 0, v, sizeof v) == 1 && strcmp(v, "on") == 0);
	CHECK(Settings_Set(o->settings.panel, "pad", "off") == 1 && !Overlay_PadOn(o));
	CHECK(Engine_CfgGet(o, "", "touchPad", 0, v, sizeof v) < 0);
	// Escape: the program sets "panel" to "close" - the panel is gone when the frame is over
	OpenBGI_Key(o, 27, 1, 0);
	Frames(o, 4);
	CHECK(!Settings_Up(o) && !o->settings.close);
	Frames(o, 1);
	CHECK(gLast.layer && At(250, 125)[3] == 0);
	// the settings button: up again, and down again
	Tap(o, OVERLAY_MENU);
	CHECK(Settings_Up(o));
	Frames(o, 6);
	CHECK(Is(250, 125, 0xe0, 0x40, 0x20, 255));
	Tap(o, OVERLAY_MENU);
	CHECK(!Settings_Up(o));
	Frames(o, 1);
	CHECK(At(250, 125)[3] == 0);
	// the host's Back button, the right mouse button, is the program's while the panel is up
	CHECK(Settings_Open(o));
	Frames(o, 6);
	OpenBGI_Button(o, OPENBGI_BUTTON_RIGHT, 1, 5, 5, 0);
	Frames(o, 4);
	CHECK(Int(o, AT_RIGHT) == 1 && !(o->input.keyboard[2] & 0x80));
	OpenBGI_Button(o, OPENBGI_BUTTON_RIGHT, 0, 5, 5, 0);
	EngineDown(o);
}

static int Reads(OpenBGI_t* p, const char* key, int i, const char* want)
{
	char v[32] = "?";
	return Settings_Value(p, key, i, v, sizeof v) == 1 && strcmp(v, want) == 0;
}

static void TestOptions(void)
{
	OpenBGI_t* o = PanelUp(0);
	OpenBGI_t* p;
	char v[32];
	uint32_t ev[3];
	int x, y;
	printf("options\n");
	if(!o)
		return;
	p = o->settings.panel;
	if(!p)
	{
		EngineDown(o);
		return;
	}
	// touches beside the pad: on until the player says otherwise, and kept
	CHECK(Reads(p, "through", 0, "on") && Settings_Value(p, "through", 1, v, sizeof v) == -1);
	CHECK(Settings_Set(p, "through", "off") == 1 && Reads(p, "through", 0, "off") && !Overlay_Through(o));
	CHECK(Engine_CfgGet(o, "", "touchThrough", 0, v, sizeof v) == 3 && strcmp(v, "off") == 0);
	CHECK(Settings_Set(p, "through", "maybe") == 0 && !Overlay_Through(o));
	CHECK(Settings_Set(p, "through", "on") == 1 && Overlay_Through(o));
	CHECK(Engine_CfgGet(o, "", "touchThrough", 0, v, sizeof v) < 0);
	// the performance display: off until it is turned on, and kept
	CHECK(Reads(p, "perf", 0, "off") && Settings_Value(p, "perf", 1, v, sizeof v) == -1 && !Perf_On(o));
	CHECK(Settings_Set(p, "perf", "on") == 1 && Reads(p, "perf", 0, "on") && Perf_On(o));
	CHECK(Engine_CfgGet(o, "", "perf", 0, v, sizeof v) == 2 && strcmp(v, "on") == 0);
	CHECK(Settings_Set(p, "perf", "maybe") == 0 && Perf_On(o));
	// .. its panel lies under the settings' picture (as if half a second had been measured): the opaque sprite
	// over it is as it was, and where the settings are clear the panel is seen
	o->perf.shown.valid = 1;
	o->perf.changed = 1;
	Frames(o, 1);
	CHECK(o->overlay.perfShown && o->overlay.perfX <= 200 && o->overlay.perfX + o->overlay.perfW >= 300);
	CHECK(o->overlay.perfY <= 100 && o->overlay.perfY + o->overlay.perfH >= 150);
	CHECK(Is(250, 125, 0xe0, 0x40, 0x20, 255) && Is(200, 100, 0xe0, 0x40, 0x20, 255));
	x = o->overlay.perfX + 2;
	y = o->overlay.perfY + 2;
	CHECK(Is(x, y, 0x16, 0x10, 0x0c, 0xd8));
	CHECK(Settings_Set(p, "perf", "off") == 1 && !Perf_On(o) && Engine_CfgGet(o, "", "perf", 0, v, sizeof v) < 0);
	Frames(o, 1);
	CHECK(!o->overlay.perfShown && At(x, y)[3] == 0 && Is(250, 125, 0xe0, 0x40, 0x20, 255));
	// this host does not scale the picture: no filters, nothing to set
	CHECK(!MainWin_ViewScales(o));
	CHECK(Settings_Value(p, "filters", 0, v, sizeof v) == -1 && Settings_Value(p, "filter", 0, v, sizeof v) == -1);
	CHECK(Settings_Value(p, "quality", 0, v, sizeof v) == -1 && Settings_Value(p, "whole", 0, v, sizeof v) == -1);
	CHECK(Settings_Set(p, "filter", "nearest") == 0 && Settings_Set(p, "whole", "on") == 0 && Settings_Set(p, "quality", "high") == 0);
	CHECK(Settings_Set(p, "filters", "x") == 0); // (only read)
	// which engine is under the panel
	CHECK(Reads(p, "game", 0, "game") && Settings_Value(p, "game", 1, v, sizeof v) == -1);
	o->launcher.running = 1;
	CHECK(Reads(p, "game", 0, "launcher"));
	o->launcher.running = 0;
	// .. and its end: the panel goes when the frame is over, then the game is asked to close
	// (here its script decides, "80 69": event 2, as from the window's close button)
	CHECK(Settings_Set(p, "game", "stay") == 0 && !o->settings.close);
	o->display.closeMode = 0;
	MsgQueue_Clear(o);
	CHECK(Settings_Set(p, "game", "close") == 1 && o->settings.close && o->settings.closeGame && Settings_Up(o));
	Frames(o, 1);
	CHECK(!Settings_Up(o) && !o->settings.close && !o->settings.closeGame);
	CHECK(MsgQueue_Get(o, ev) && ev[0] == 2 && !MsgQueue_Get(o, ev));
	// "panel": "close" asks nothing of the game
	CHECK(Settings_Open(o));
	Frames(o, 4);
	CHECK(Settings_Set(o->settings.panel, "panel", "close") == 1 && !o->settings.closeGame);
	Frames(o, 1);
	CHECK(!Settings_Up(o) && !MsgQueue_Get(o, ev));
	EngineDown(o);

	// a host that scales the picture, with two filters of its own
	o = PanelUp(1);
	if(!o)
		return;
	p = o->settings.panel;
	if(!p)
	{
		EngineDown(o);
		return;
	}
	CHECK(MainWin_ViewScales(o) && !MainWin_ViewOn(o));
	CHECK(Reads(p, "filters", 0, "game") && Reads(p, "filters", 1, "bicubic") && Reads(p, "filters", 2, "bilinear"));
	CHECK(Reads(p, "filters", 3, "nearest") && Reads(p, "filters", 4, "Sharp") && Reads(p, "filters", 5, "Soft"));
	CHECK(Settings_Value(p, "filters", 6, v, sizeof v) == -1 && Settings_Value(p, "filters", -1, v, sizeof v) == -1);
	CHECK(Reads(p, "filter", 0, "game") && Reads(p, "quality", 0, "normal") && Reads(p, "whole", 0, "off"));
	CHECK(Settings_Value(p, "filter", 1, v, sizeof v) == -1);
	// each is told to the host as the rule it scales by
	CHECK(Settings_Set(p, "filter", "nearest") == 1 && Reads(p, "filter", 0, "nearest"));
	CHECK(gRule == (OPENBGI_SCALE_NEAREST | OPENBGI_SCALE_CHOSEN));
	CHECK(Settings_Set(p, "filter", "Soft") == 1 && Reads(p, "filter", 0, "Soft"));
	CHECK(gRule == ((OPENBGI_SCALE_OWN + 1) | OPENBGI_SCALE_CHOSEN));
	CHECK(Settings_Set(p, "quality", "high") == 1 && Reads(p, "quality", 0, "high"));
	CHECK(gRule == ((OPENBGI_SCALE_OWN + 1) | OPENBGI_SCALE_CHOSEN | OPENBGI_SCALE_HIGH));
	CHECK(Settings_Set(p, "whole", "on") == 1 && Reads(p, "whole", 0, "on"));
	CHECK(gRule == ((OPENBGI_SCALE_OWN + 1) | OPENBGI_SCALE_CHOSEN | OPENBGI_SCALE_HIGH | OPENBGI_SCALE_WHOLE));
	CHECK(Settings_Set(p, "quality", "normal") == 1 && Settings_Set(p, "whole", "off") == 1);
	CHECK(gRule == ((OPENBGI_SCALE_OWN + 1) | OPENBGI_SCALE_CHOSEN));
	// what is no filter, no quality, no switch changes nothing
	CHECK(Settings_Set(p, "filter", "Sharper") == 0 && Settings_Set(p, "filter", NULL) == 0 && Reads(p, "filter", 0, "Soft"));
	CHECK(Settings_Set(p, "quality", "best") == 0 && Settings_Set(p, "whole", "yes") == 0);
	CHECK(gRule == ((OPENBGI_SCALE_OWN + 1) | OPENBGI_SCALE_CHOSEN));
	// "game": the game's own choice again
	CHECK(Settings_Set(p, "filter", "game") == 1 && Reads(p, "filter", 0, "game") && !(gRule & OPENBGI_SCALE_CHOSEN));
	// the host scales only what it draws on: without a surface it is no view
	Settings_Close(o);
	OpenBGI_SetSurface(o, NULL);
	CHECK(!MainWin_ViewScales(o));
	EngineDown(o);
}

static void TestEnds(void)
{
	OpenBGI_t* o = PanelUp(0);
	printf("ends\n");
	if(!o)
		return;
	// the program ends (End): the panel is put away
	CHECK(Settings_Up(o));
	OpenBGI_Key(o, 35, 1, 0);
	Frames(o, 6);
	CHECK(!Settings_Up(o));
	// the program stores through a null pointer (Home): a script error.  The panel is put away, the game's
	// engine shows the message in a box of its own and goes on when that is answered
	CHECK(Settings_Open(o));
	Frames(o, 6);
	CHECK(!Request_Pending(o));
	OpenBGI_Key(o, 36, 1, 0);
	Frames(o, 6);
	CHECK(!Settings_Up(o) && !o->settings.error && Request_Pending(o) == 1);
	OS_AnswerRequests(o);
	CHECK(!Request_Pending(o));
	OpenBGI_Key(o, 36, 0, 0);
	// the surface goes: the panel with it
	CHECK(Settings_Open(o));
	Frames(o, 4);
	OpenBGI_SetSurface(o, NULL);
	Frames(o, 1);
	CHECK(!Settings_Up(o) && !gLast.layer);
	// the game's stop takes its panel along
	Surface(o);
	Frames(o, 1);
	CHECK(Settings_Open(o));
	Frames(o, 4);
	memset(&o->screen.pic, 0, sizeof o->screen.pic);
	o->display.windowAlive = 0;
	Gfx_Destroy(o);
	OpenBGI_Stop(o);
	CHECK(!Settings_Up(o));
	BGI_Free(o->launcher.kept);
	o->launcher.kept = NULL;
	OpenBGI_Delete(o);
}

int main(void)
{
	OS_Init();
	TestClosed();
	TestOpen();
	TestLayer();
	TestInput();
	TestBridge();
	TestOptions();
	TestEnds();
	OS_Shutdown();
	if(gFails)
	{
		printf("settings: %d checks failed\n", gFails);
		return 1;
	}
	printf("settings: all tests passed\n");
	return 0;
}
