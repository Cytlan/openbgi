/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/targets.c - unit test of what a click reaches (OpenBGI_Targets of
 *                   openbgi.h, src/sys/inputmap.c and the owners' reports
 *                   of bgi/inputmap.h); built as test_targets by `make test`
 *
 * An engine with its graphics but no window of the host's: the test says
 * the window is up and gives it a picture of 640 x 480, makes the owners
 * of mouse input itself - layers, waits on a thread, sprites, knobs,
 * windows with icons, menus and a panel - and reads the list.  Each group
 * also checks that asking changes nothing: the next call gives the same
 * list.  The groups, in the order they run:
 *
 *   nothing  - no window: nothing; the base layer alone: nothing
 *   layers   - a layer of the script's ("80 18") is a target anywhere,
 *              PROBED once polled and for half a second after; a higher
 *              one covers it; input off disables them; a timed wait that
 *              input ends claims its layer (WAIT, owner the thread); the
 *              buttons read directly ("80 11") are RAW, last
 *   sprites  - a registered sprite ("90 FA") at its rectangle, clicked at
 *              its centre, HOVER under the pointer; a higher one over its
 *              middle moves the click point to the nearest free point; a
 *              hidden one is not listed, one half outside is cut; with the
 *              picture scaled from the back buffer (a window whose shape
 *              differs, Gfx_MapPoint) the rectangle and the click point
 *              are the picture's
 *   knobs    - a registered knob: its thumb and its travel, HELD while
 *              dragged; an object tested by "90 3D" (PROBE) is clicked on
 *              its mask
 *   icons    - an icon selection: the icons at the window's text area,
 *              DISABLED while another wait has the input focus
 *   panel    - a button panel: the drawn buttons, SELECTED for the
 *              group's selection, DISABLED while its input is off, a
 *              button without picture not listed, one with a hit mask
 *              clicked on its mask, a modal panel's layer claimed
 *   sprite panel - a sprite panel by the engine's build: which buttons
 *              get a sub-sprite (build 444: the enabled or shown ones,
 *              and the painter draws them all; from 451 on the shown
 *              ones alone, in both), whose the frames are (always the
 *              shown picture's; from 1.553 on the focus picture's with
 *              the button's flag 0x01, the focused or selected button's
 *              with 0x08, starting anew each time), and the focused
 *              button drawn over the others when its group asks (1.492
 *              on)
 *   menu     - a text menu: its entries, the cursor SELECTED (skipped
 *              when no Japanese font can be opened)
 *   text     - a text output: TEXT anywhere on its layer, owner the thread
 *   reset    - the subsystems' reset of a reboot (Vm_ResetSubsystems) with
 *              a knob, a panel and a registered sprite alive: nothing is
 *              left to click (and nothing is used after it was freed: a
 *              knob goes before its thumb, a panel before its window)
 */
#include "bgi/core.h"
#include "bgi/input.h"
#include "bgi/inputmap.h"
#include "bgi/gfx.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/sprite.h"
#include "bgi/gfx/window.h"
#include "bgi/gfx/text.h"
#include "bgi/panel.h"
#include "bgi/sysobj.h"
#include "bgi/vm.h"
#include "bgi/waitobj.h"
#include "bgi/engine.h"
#include "bgi/sysobj/locks.h"
#include "bgi/sysobj/events.h"
#include "host/os.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "check.h"

#define W 640
#define H 480

static uint32_t gNow = 1000; // the host's clock (ms)
static uint8_t gPicture[W * H * 4];

static uint32_t TicksMs(void* user)
{
	(void)user;
	return gNow;
}

static OpenBGI_Host_t gHost;

// ---- the engine ---------------------------------------------------------------------------------

static OpenBGI_t* EngineUp(Thread_t** thread)
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
	o->vm.rootThread = Thread_New(o, 0, 0, 0);
	*thread = Thread_SpawnChild(o, o->vm.rootThread, 0x100, 0x100, 0x400);
	return o;
}

// the window is up and shows a picture of W x H
static void WindowUp(OpenBGI_t* o)
{
	o->display.windowAlive = 1;
	memset(&o->screen.pic, 0, sizeof o->screen.pic);
	o->screen.pic.pixels = gPicture;
	o->screen.pic.w = W;
	o->screen.pic.h = H;
	o->screen.pic.pitch = W * 4;
	o->screen.pic.bpp = 4;
	o->screen.pic.mode = PM_RGB32;
}

static void EngineDown(OpenBGI_t* o)
{
	MsgQueue_Clear(o); // (the events for the game a knob's drag posted)
	Thread_KillAllChildren(o, o->vm.rootThread);
	Thread_Delete(o, o->vm.rootThread);
	o->vm.rootThread = NULL;
	Panel_DeleteAll(o);
	Knob_UnregisterAll(o);
	Target_Clear(o);
	Input_ResetLayers(o);
	memset(&o->screen.pic, 0, sizeof o->screen.pic);
	o->display.windowAlive = 0;
	Gfx_Destroy(o);
	OpenBGI_Delete(o);
}

static void Pointer(OpenBGI_t* o, int x, int y)
{
	o->input.pointerX = x;
	o->input.pointerY = y;
}

/* bitmap `no` of w x h, ARGB, opaque; with `hole` the left two thirds of
 * its pixels are transparent, its centre too (for the masks) */
static void MakeBitmap(OpenBGI_t* o, int no, int w, int h, int hole)
{
	Bmp_t b;
	int x, y;
	BmpMgr_Create(o->gfx.bmpMgr, no, w, h, PM_ARGB32);
	BmpMgr_GetInfo(o->gfx.bmpMgr, &b, no);
	for(y = 0; y < h; y++)
		for(x = 0; x < w; x++)
			((uint32_t*)(b.pixels + (size_t)y * b.pitch))[x] = hole && x < w * 2 / 3 ? 0 : 0xff808080u;
}

// a shown sprite of bitmap `bmp` at (x, y) with priority `prio`
static uint32_t MakeSprite(OpenBGI_t* o, int bmp, int x, int y, int prio)
{
	uint32_t h = GfxCall_SpriteCreate(o);
	GfxCall_SpriteSet(o, h, x, y, bmp, 0, 0, prio);
	GfxCall_SpriteShow(o, h, 1);
	return h;
}

// the targets, and whether a second call gives the same ones (asking changes nothing)
static int Gather(OpenBGI_t* o, OpenBGI_Target_t* out, int max)
{
	OpenBGI_Target_t again[64];
	int n = OpenBGI_Targets(o, out, max, sizeof(OpenBGI_Target_t)), m = OpenBGI_Targets(o, again, 64, sizeof(OpenBGI_Target_t));
	CHECK(n == m);
	CHECK(n > max || n > 64 || memcmp(out, again, (size_t)n * sizeof *out) == 0);
	if(m > 0 && m <= 64)
	{ // elements of a host's other header, shorter and longer: what each holds of a target, zero behind what the engine knows
		size_t small = offsetof(OpenBGI_Target_t, drag), large = sizeof(OpenBGI_Target_t) + 8;
		uint8_t* a = (uint8_t*)malloc((size_t)m * small); // (of just their sizes: the sanitizer sees a byte too many)
		uint8_t* b = (uint8_t*)malloc((size_t)m * large);
		static const uint8_t kZero[8];
		int i, same = 1;
		memset(b, 0xa5, (size_t)m * large);
		CHECK(OpenBGI_Targets(o, (OpenBGI_Target_t*)(void*)a, m, small) == m);
		CHECK(OpenBGI_Targets(o, (OpenBGI_Target_t*)(void*)b, m, large) == m);
		for(i = 0; i < m; i++)
		{
			same &= memcmp(a + (size_t)i * small, &again[i], small) == 0;
			same &= memcmp(b + (size_t)i * large, &again[i], sizeof again[i]) == 0;
			same &= memcmp(b + (size_t)i * large + sizeof again[i], kZero, sizeof kZero) == 0;
		}
		CHECK(same);
		// fewer than there are: the first ones, the count the whole
		CHECK(m < 2 || (OpenBGI_Targets(o, (OpenBGI_Target_t*)(void*)a, m - 1, small) == m && memcmp(a + (size_t)(m - 2) * small, &again[m - 2], small) == 0));
		free(a);
		free(b);
	}
	return n;
}

// the first target of `kind` (and owner, when not 0), NULL for none
static const OpenBGI_Target_t* Find(const OpenBGI_Target_t* t, int n, int kind, uint32_t owner)
{
	int i;
	for(i = 0; i < n; i++)
		if(t[i].kind == kind && (!owner || t[i].owner == owner))
			return &t[i];
	return NULL;
}

static int Count(const OpenBGI_Target_t* t, int n, int kind)
{
	int i, k = 0;
	for(i = 0; i < n; i++)
		k += t[i].kind == kind;
	return k;
}

// ---- the groups ---------------------------------------------------------------------------------

static void TestNothing(void)
{
	Thread_t* th;
	OpenBGI_t* o = EngineUp(&th);
	OpenBGI_Target_t t[8];
	printf("nothing\n");
	if(!o)
		return;
	MouseLayer_Push(o, (5u << 20) | 0xfffff);
	CHECK(OpenBGI_Targets(o, t, 8, sizeof(OpenBGI_Target_t)) == 0); // no window yet
	MouseLayer_Pop(o, (5u << 20) | 0xfffff);
	WindowUp(o);
	CHECK(Gather(o, t, 8) == 0); // the base layer is nobody's target
	EngineDown(o);
}

static void TestLayers(void)
{
	Thread_t* th;
	OpenBGI_t* o = EngineUp(&th);
	OpenBGI_Target_t t[16];
	const OpenBGI_Target_t* a;
	uint32_t low = (5u << 20) | 0xfffff, high = (7u << 20) | 0xfffff;
	int n;
	printf("layers\n");
	if(!o)
		return;
	WindowUp(o);
	MouseLayer_Push(o, low);
	KeyLayer_Push(o, low);
	n = Gather(o, t, 16);
	CHECK(n == 1 && t[0].kind == OPENBGI_TARGET_LAYER && t[0].owner == 5 && t[0].layer == low);
	CHECK(t[0].flags == (OPENBGI_TARGET_ANYWHERE | OPENBGI_TARGET_HOVER));
	CHECK(t[0].rect.x == 0 && t[0].rect.y == 0 && t[0].rect.w == W && t[0].rect.h == H);
	CHECK(t[0].clickX == W / 2 && t[0].clickY == H / 2 && t[0].group == -1 && t[0].index == -1);
	Input_NoteProbe(o, PROBE_LAYER, low); // "80 1A" polled it
	n = Gather(o, t, 16);
	CHECK(n == 1 && (t[0].flags & OPENBGI_TARGET_PROBED));
	gNow += INPUT_PROBE_MS;
	CHECK(Gather(o, t, 16) == 1 && (t[0].flags & OPENBGI_TARGET_PROBED));
	gNow += 1;
	CHECK(Gather(o, t, 16) == 1 && !(t[0].flags & OPENBGI_TARGET_PROBED)); // not lately
	// a higher layer covers it
	MouseLayer_Push(o, high);
	n = Gather(o, t, 16);
	CHECK(n == 2 && t[0].layer == high && t[1].layer == low);
	CHECK(t[1].clickX == -1 && t[1].clickY == -1 && (t[1].flags & OPENBGI_TARGET_COVERED) && !(t[1].flags & OPENBGI_TARGET_HOVER));
	// input off
	o->input.inputEnable = 0;
	n = Gather(o, t, 16);
	CHECK(n == 2 && (t[0].flags & OPENBGI_TARGET_DISABLED) && (t[1].flags & OPENBGI_TARGET_DISABLED));
	o->input.inputEnable = 1;
	MouseLayer_Pop(o, high);
	// a timed wait that input ends claims its layer: no LAYER of its key
	Thread_SetWait(o, th, WaitTimerKey_New(o, th, 1000, 1, 9));
	n = Gather(o, t, 16);
	a = Find(t, n, OPENBGI_TARGET_WAIT, 0);
	CHECK(n == 2 && a == &t[0] && a->owner == Thread_GetId(th) && a->layer == ((9u << 20) | 0xfffff));
	CHECK(Count(t, n, OPENBGI_TARGET_LAYER) == 1 && t[1].layer == low && (t[1].flags & OPENBGI_TARGET_COVERED));
	Thread_DropWait(o, th);
	Thread_SetWait(o, th, WaitTimerKey_New(o, th, 1000, 0, 9)); // without keys: nothing
	CHECK(Gather(o, t, 16) == 1 && t[0].kind == OPENBGI_TARGET_LAYER);
	Thread_DropWait(o, th);
	// the buttons read directly
	Input_NoteProbe(o, PROBE_BUTTONS, 1);
	n = Gather(o, t, 16);
	CHECK(n == 2 && t[1].kind == OPENBGI_TARGET_RAW && t[1].layer == 0 && t[1].owner == 1);
	CHECK(t[1].flags == (OPENBGI_TARGET_ANYWHERE | OPENBGI_TARGET_PROBED | OPENBGI_TARGET_HOVER) && t[1].clickX == W / 2);
	o->input.inputEnable = 0;
	n = Gather(o, t, 16);
	CHECK(n == 2 && !(t[1].flags & OPENBGI_TARGET_DISABLED)); // the script reads the buttons whatever the layers say
	o->input.inputEnable = 1;
	KeyLayer_Pop(o, low);
	MouseLayer_Pop(o, low);
	EngineDown(o);
}

static void TestSprites(void)
{
	Thread_t* th;
	OpenBGI_t* o = EngineUp(&th);
	OpenBGI_Target_t t[16];
	const OpenBGI_Target_t* a;
	uint32_t s1, s2;
	int n;
	printf("sprites\n");
	if(!o)
		return;
	WindowUp(o);
	MakeBitmap(o, 10, 40, 30, 0);
	MakeBitmap(o, 11, 10, 10, 0);
	s1 = MakeSprite(o, 10, 100, 100, 3);
	CHECK(Target_Add(o, s1));
	Pointer(o, 0, 0);
	n = Gather(o, t, 16);
	CHECK(n == 1 && t[0].kind == OPENBGI_TARGET_SPRITE && t[0].owner == s1 && t[0].index == 0);
	CHECK(t[0].rect.x == 100 && t[0].rect.y == 100 && t[0].rect.w == 40 && t[0].rect.h == 30);
	CHECK(t[0].clickX == 120 && t[0].clickY == 115 && t[0].flags == 0);
	CHECK(t[0].drag.x == 100 && t[0].drag.w == 40); // no knob: the rectangle
	Pointer(o, 139, 129);
	CHECK(Gather(o, t, 16) == 1 && t[0].flags == OPENBGI_TARGET_HOVER);
	// a higher one over its middle
	s2 = MakeSprite(o, 11, 115, 110, 4);
	CHECK(Target_Add(o, s2));
	n = Gather(o, t, 16);
	CHECK(n == 2 && t[0].owner == s2 && t[1].owner == s1);
	CHECK(t[0].clickX == 120 && t[0].clickY == 115);
	a = &t[1];
	CHECK(a->clickX >= 100 && a->clickX < 140 && a->clickY >= 100 && a->clickY < 130);
	CHECK(!(a->clickX >= 115 && a->clickX < 125 && a->clickY >= 110 && a->clickY < 120));         // beside the higher one
	CHECK((a->clickX - 120) * (a->clickX - 120) + (a->clickY - 115) * (a->clickY - 115) < 8 * 8); // and near the centre
	// hidden; half outside
	GfxCall_SpriteShow(o, s2, 0);
	CHECK(Gather(o, t, 16) == 1 && t[0].owner == s1 && t[0].clickX == 120);
	GfxCall_SpriteSet(o, s1, W - 20, 100, 10, 0, 0, 3);
	n = Gather(o, t, 16);
	CHECK(n == 1 && t[0].rect.x == W - 20 && t[0].rect.w == 20 && t[0].clickX == W - 10);
	GfxCall_SpriteSet(o, s1, W + 5, 100, 10, 0, 0, 3);
	CHECK(Gather(o, t, 16) == 0); // outside the picture
	// the picture scaled from the back buffer: 0.9375 high (scale mode 1), about the middle; near the bottom
	GfxCall_SpriteSet(o, s1, 100, 440, 10, 0, 0, 3);
	o->gfx.gfx->back.w = W;
	o->gfx.gfx->back.h = H;
	o->gfx.gfx->scaled.w = W;
	o->gfx.gfx->scaled.h = H * 15 / 16;
	o->gfx.gfx->scaleMode = 1;
	o->gfx.gfx->scaledMem = gPicture; // (only tested for NULL here)
	o->screen.pic.h = H * 15 / 16;
	n = Gather(o, t, 16);
	CHECK(n == 1 && t[0].rect.x == 100 && t[0].rect.y == 412 && t[0].rect.h == 28 && t[0].rect.w == 40);
	CHECK(t[0].clickX == 120 && t[0].clickY == 412 + 14); // (back in the back buffer: y 454)
	o->gfx.gfx->scaledMem = NULL;
	o->gfx.gfx->scaleMode = 0;
	EngineDown(o);
}

static void TestKnobs(void)
{
	Thread_t* th;
	OpenBGI_t* o = EngineUp(&th);
	OpenBGI_Target_t t[16];
	const OpenBGI_Target_t* a;
	uint32_t thumb, knob, probe;
	int n;
	printf("knobs\n");
	if(!o)
		return;
	WindowUp(o);
	MakeBitmap(o, 10, 20, 30, 0);
	MakeBitmap(o, 12, 40, 40, 1);
	thumb = MakeSprite(o, 10, 200, 300, 2);
	CHECK(GfxCall_KnobCreate(o, &knob, thumb) == 0);
	CHECK(GfxCall_KnobSetRange(o, knob, 220, 30) == 0);
	GfxCall_KnobShow(o, knob, 1);
	CHECK(Knob_Register(o, knob));
	n = Gather(o, t, 16);
	a = Find(t, n, OPENBGI_TARGET_KNOB, knob);
	CHECK(n == 1 && a && a->rect.x == 200 && a->rect.y == 300 && a->rect.w == 20 && a->rect.h == 30);
	CHECK(a && a->drag.x == 200 && a->drag.y == 300 && a->drag.w == 220 && a->drag.h == 30 && a->clickX == 210 && a->flags == 0);
	Knob_BeginDrag(o, Knob_UnderMouse(o) ? Knob_UnderMouse(o) : o->input.knobHead.next, 210, 315);
	n = Gather(o, t, 16);
	CHECK(n == 1 && (t[0].flags & OPENBGI_TARGET_HELD));
	Knob_BeginDrag(o, NULL, 0, 0);
	// an object a script tests: its mask decides where to click
	probe = MakeSprite(o, 12, 400, 100, 1);
	CHECK(GfxCall_ObjSetHitMask(o, probe, 12) == 0);
	Input_NoteProbe(o, PROBE_OBJECT, probe);
	n = Gather(o, t, 16);
	a = Find(t, n, OPENBGI_TARGET_PROBE, probe);
	CHECK(n == 2 && a && a->flags == OPENBGI_TARGET_PROBED && a->rect.x == 400 && a->rect.w == 40);
	CHECK(a && a->clickX >= 426 && a->clickX < 440 && a->clickY >= 100 && a->clickY < 140); // the right third is opaque
	gNow += INPUT_PROBE_MS + 1;
	CHECK(Gather(o, t, 16) == 1);
	EngineDown(o);
}

// a window of w x h at (x, y), shown, with priority `prio`
static uint32_t MakeWindow(OpenBGI_t* o, int x, int y, int w, int h, int prio)
{
	uint32_t hw = 0;
	CHECK(GfxCall_WindowCreate(o, &hw, w, h) == 0);
	GfxCall_WindowSet(o, hw, x, y, 0, 0, 0, prio);
	GfxCall_WindowShow(o, hw, 1);
	return hw;
}

static void TestIcons(void)
{
	Thread_t* th;
	OpenBGI_t* o = EngineUp(&th);
	OpenBGI_Target_t t[16];
	int32_t icons[2][4] = {{10, 20, 10, -1}, {60, 20, 10, -1}}; // x, y, bitmap, focus bitmap
	Rect_t area;
	uint32_t hw;
	int n;
	printf("icons\n");
	if(!o)
		return;
	WindowUp(o);
	MakeBitmap(o, 10, 40, 30, 0);
	hw = MakeWindow(o, 50, 60, 300, 200, 4);
	CHECK(GfxCall_WindowGetTextArea(o, &area, hw) == 0);
	CHECK(StartIconSelect(o, th, hw, 2, icons, NULL, 3, 0, 0) == 0);
	n = Gather(o, t, 16);
	CHECK(n == 2 && Count(t, n, OPENBGI_TARGET_ICON) == 2); // mode 3's layer is claimed: no LAYER
	CHECK(t[0].owner == Thread_GetId(th) && t[0].index == 0 && t[1].index == 1 && t[0].flags == 0);
	CHECK(t[0].rect.x == 50 + area.l + 10 && t[0].rect.y == 60 + area.t + 20 && t[0].rect.w == 40 && t[0].rect.h == 30);
	CHECK(t[1].rect.x == 50 + area.l + 60 && t[1].clickX == t[1].rect.x + 20);
	InputFocus_Add(o, 0x7777, 0xfffffff0u); // a wait above has the focus
	n = Gather(o, t, 16);
	CHECK(n == 2 && (t[0].flags & OPENBGI_TARGET_DISABLED) && (t[1].flags & OPENBGI_TARGET_DISABLED));
	InputFocus_Remove(o, 0x7777);
	Thread_DropWait(o, th);
	CHECK(Gather(o, t, 16) == 0);
	EngineDown(o);
}

static void TestPanel(void)
{
	Thread_t* th;
	OpenBGI_t* o = EngineUp(&th);
	OpenBGI_Target_t t[16];
	PanelButton_t buttons[4];
	PanelGroup_t group;
	PanelDesc_t desc;
	ButtonPanel_t* p;
	Rect_t area;
	uint32_t hw, id;
	int n, i;
	printf("panel\n");
	if(!o)
		return;
	WindowUp(o);
	MakeBitmap(o, 10, 40, 30, 0);
	MakeBitmap(o, 13, 40, 30, 1); // a mask clear on the left two thirds
	hw = MakeWindow(o, 100, 50, 400, 300, 5);
	CHECK(GfxCall_WindowGetTextArea(o, &area, hw) == 0);
	id = SubObj_Create(o, hw, 0);
	p = (ButtonPanel_t*)SubObj_Find(o, id);
	memset(buttons, 0, sizeof buttons);
	for(i = 0; i < 4; i++)
	{
		buttons[i].enabled = 1;
		buttons[i].x = 10 + 50 * i;
		buttons[i].y = 10;
		buttons[i].bmpNormal = i == 2 ? -1 : 10; // the third has no picture
		buttons[i].bmpSelected = buttons[i].bmpFocus = -1;
		buttons[i].hitMaskBmp = i == 3 ? 13 : -1; // the fourth is hit by its mask
	}
	memset(&group, 0, sizeof group);
	group.count = 4;
	group.columns = 4;
	group.buttons = buttons;
	group.selected = 1;
	group.selectable = 1;
	group.linkId = -1;
	memset(&desc, 0, sizeof desc);
	desc.groupCount = 1;
	desc.groups = &group;
	desc.currentGroup = 0;
	desc.modal = 1;
	CHECK(p && ButtonPanel_Start(o, p, &desc) == 0);
	n = Gather(o, t, 16);
	CHECK(n == 3 && Count(t, n, OPENBGI_TARGET_BUTTON) == 3); // the modal panel's layer is claimed
	CHECK(t[0].owner == id && t[0].group == 0 && t[0].index == 0 && t[1].index == 1 && t[2].index == 3);
	CHECK(t[0].rect.x == 100 + area.l + 10 && t[0].rect.y == 50 + area.t + 10 && t[0].rect.w == 40);
	CHECK(t[0].flags == 0 && t[1].flags == OPENBGI_TARGET_SELECTED);
	CHECK(t[0].clickX == t[0].rect.x + 20 && t[0].clickY == t[0].rect.y + 15);
	CHECK(t[2].rect.x == 100 + area.l + 160 && t[2].clickX >= t[2].rect.x + 26 && t[2].clickX < t[2].rect.x + 40); // on the mask
	CHECK(t[2].flags == 0);
	p->inputEnabled = 0; // "80 AC" 0x10000003
	n = Gather(o, t, 16);
	CHECK(n == 3 && (t[0].flags & OPENBGI_TARGET_DISABLED) && (t[1].flags & OPENBGI_TARGET_DISABLED));
	CHECK(t[0].clickX >= 0); // still there to click
	p->inputEnabled = 1;
	CHECK(SubObj_Delete(o, id));
	CHECK(Gather(o, t, 16) == 0);
	EngineDown(o);
}

// the picture sub-sprite `k` of the panel's window shows (-1 without one), and its priority
static int SubPicture(const ButtonPanel_t* p, int k)
{
	return k < p->win->subCount && p->win->sub[k] ? ((const Sprite_t*)p->win->sub[k])->bmp1 : -1;
}

static int SubPriority(const ButtonPanel_t* p, int k)
{
	return k < p->win->subCount && p->win->sub[k] ? (int)DispObj_GetPriority(p->win->sub[k]) : -1;
}

// four buttons of picture 20: enabled and shown, enabled only, shown only, neither
static void SpriteButtons(SpriteButton_t* b)
{
	int i;
	memset(b, 0, 4 * sizeof *b);
	for(i = 0; i < 4; i++)
	{
		b[i].enabled = i < 2;
		b[i].shown = i == 0 || i == 2;
		b[i].x = 10 + 50 * i;
		b[i].y = 10;
		b[i].bmpNormal = 20;
		b[i].bmpFocus = b[i].bmpSelected = b[i].bmpSelFocus = b[i].hitMaskBmp = -1;
	}
}

static void TestSpritePanel(void)
{
	Thread_t* th;
	OpenBGI_t* o = EngineUp(&th);
	SpriteButton_t buttons[4];
	SpriteGroup_t group;
	SpriteDesc_t desc;
	ButtonPanel_t* p;
	Window_t* paint;
	uint32_t hw, hw2, id;
	int i;
	printf("sprite panel\n");
	if(!o)
		return;
	WindowUp(o);
	for(i = 20; i < 30; i++)
		MakeBitmap(o, i, 40, 30, 0);
	hw = MakeWindow(o, 0, 0, 400, 300, 5);
	hw2 = MakeWindow(o, 0, 300, 400, 100, 6);
	id = SubObj_Create(o, hw, 1);
	p = (ButtonPanel_t*)SubObj_Find(o, id);
	paint = (Window_t*)Gfx_FindWindow(o->gfx.dispGfx, hw2);
	CHECK(p && paint);
	if(!p || !paint)
	{
		EngineDown(o);
		return;
	}
	memset(&group, 0, sizeof group);
	group.count = 4;
	group.columns = 4;
	group.buttons = buttons;
	group.selected = -1;
	group.selectable = 1;
	group.linkId = -1;
	memset(&desc, 0, sizeof desc);
	desc.groupCount = 1;
	desc.groups = &group;
	desc.currentGroup = 0;
	desc.mouse = 1;

	// which buttons are drawn
	SpriteButtons(buttons);
	CHECK(Engine_SelectProfile(o, "1.69/444"));
	CHECK(SpritePanel_Start(o, p, &desc) == 0);
	CHECK(SubPicture(p, 0) == 20 && SubPicture(p, 1) == 20 && SubPicture(p, 2) == 20 && SubPicture(p, 3) == -1); // enabled or shown
	CHECK(SpritePanel_Paint(o, paint, &desc) == 0);
	CHECK(paint->sub[0] && paint->sub[1] && paint->sub[2] && paint->sub[3]); // the painter: all of them
	CHECK(Engine_SelectProfile(o, "1.69/451"));
	CHECK(SpritePanel_Start(o, p, &desc) == 0);
	CHECK(SubPicture(p, 0) == 20 && SubPicture(p, 1) == -1 && SubPicture(p, 2) == 20 && SubPicture(p, 3) == -1); // the shown ones
	CHECK(SpritePanel_Paint(o, paint, &desc) == 0);
	CHECK(paint->sub[0] && !paint->sub[1] && paint->sub[2] && !paint->sub[3]);
	CHECK(Engine_SelectProfile(o, "1.553"));
	CHECK(SpritePanel_Start(o, p, &desc) == 0);
	CHECK(SubPicture(p, 0) == 20 && SubPicture(p, 1) == -1 && SubPicture(p, 2) == 20 && SubPicture(p, 3) == -1); // (a hidden page's buttons)
	CHECK(SubPriority(p, 0) == 0 && SubPriority(p, 2) == 2);                                                     // in their order

	// the frames: three of 10 ms from the picture on, 20 at rest, 23 with the pointer on the button, 26 selected
	SpriteButtons(buttons);
	buttons[1].shown = 1;
	for(i = 0; i < 2; i++)
	{
		buttons[i].frames = 3;
		buttons[i].frameMs = 10;
		buttons[i].bmpFocus = 23;
		buttons[i].bmpSelected = 26;
	}
	CHECK(Engine_SelectProfile(o, "1.547")); // before the flags: the frames of whatever the button shows
	buttons[0].flags = 0x1;
	CHECK(SpritePanel_Start(o, p, &desc) == 0);
	PANEL_VT(p)->animate(o, p);
	gNow += 10;
	PANEL_VT(p)->animate(o, p);
	CHECK(SubPicture(p, 0) == 21 && SubPicture(p, 1) == 21);
	CHECK(Engine_SelectProfile(o, "1.553"));
	buttons[0].flags = 0x1; // the focus picture's frames
	buttons[1].flags = 0x8; // the focused or selected button's
	CHECK(SpritePanel_Start(o, p, &desc) == 0);
	for(i = 0; i < 3; i++, gNow += 10)
		PANEL_VT(p)->animate(o, p);
	CHECK(SubPicture(p, 0) == 20 && SubPicture(p, 1) == 20); // still at rest
	CHECK(PANEL_VT(p)->setFocus(o, p, 0, 0) == 1 && SubPicture(p, 0) == 23);
	PANEL_VT(p)->animate(o, p);
	gNow += 10;
	PANEL_VT(p)->animate(o, p);
	CHECK(SubPicture(p, 0) == 24 && SubPicture(p, 1) == 20);
	CHECK(PANEL_VT(p)->setFocus(o, p, 1, 0) == 1);
	CHECK(SubPicture(p, 0) == 20 && SubPicture(p, 1) == 23); // the first back at rest, without a frame
	PANEL_VT(p)->animate(o, p);
	gNow += 10;
	PANEL_VT(p)->animate(o, p);
	CHECK(SubPicture(p, 1) == 24);
	CHECK(PANEL_VT(p)->setFocus(o, p, 0, 0) == 1 && SubPicture(p, 0) == 23); // from its first frame again
	CHECK(SubPicture(p, 1) == 20);                                           // and the second, neither focused nor selected, at rest
	CHECK(PANEL_VT(p)->setFocus(o, p, -1, 0) == 1);
	CHECK(PANEL_VT(p)->setSelection(o, p, 0, 1) == 1 && SubPicture(p, 1) == 26); // selected: its frames run without the pointer
	PANEL_VT(p)->animate(o, p);
	gNow += 10;
	PANEL_VT(p)->animate(o, p);
	CHECK(SubPicture(p, 1) == 27);
	CHECK(PANEL_VT(p)->setSelection(o, p, 0, 0) == 1);
	CHECK(SubPicture(p, 0) == 26 && SubPicture(p, 1) == 20); // the first shows its selected picture, still: its frames are the focus picture's
	gNow += 10;
	PANEL_VT(p)->animate(o, p);
	CHECK(SubPicture(p, 0) == 26);
	CHECK(PANEL_VT(p)->setFocus(o, p, 0, 0) == 1 && SubPicture(p, 0) == 26); // selected and focused, without a picture for both: the selected one
	PANEL_VT(p)->animate(o, p);
	gNow += 10;
	PANEL_VT(p)->animate(o, p);                                                  // (its frames run under the pointer)
	CHECK(SubPicture(p, 0) == 26);                                               // and it stays still
	CHECK(PANEL_VT(p)->setSelection(o, p, 0, 1) == 1 && SubPicture(p, 1) == 26); // the second selected again: from its first frame
	CHECK(SubPicture(p, 0) == 24);                                               // the first, focused and no longer selected: the focus picture's frame
	buttons[0].bmpFocus = -1;                                                    // no focus picture: nothing for the frames to run on
	CHECK(SpritePanel_Start(o, p, &desc) == 0);
	CHECK(PANEL_VT(p)->setFocus(o, p, 0, 0) == 1 && SubPicture(p, 0) == 20);
	PANEL_VT(p)->animate(o, p);
	gNow += 10;
	PANEL_VT(p)->animate(o, p);
	CHECK(SubPicture(p, 0) == 20);

	// the focused button over the others
	SpriteButtons(buttons);
	buttons[1].shown = 1;
	group.flags = 0x1;
	CHECK(Engine_SelectProfile(o, "1.69/472"));
	CHECK(SpritePanel_Start(o, p, &desc) == 0);
	CHECK(PANEL_VT(p)->setFocus(o, p, 1, 0) == 1 && SubPriority(p, 1) == 1); // not yet
	CHECK(Engine_SelectProfile(o, "1.494"));
	CHECK(SpritePanel_Start(o, p, &desc) == 0);
	CHECK(SubPriority(p, 0) == 0 && SubPriority(p, 1) == 1);
	CHECK(PANEL_VT(p)->setFocus(o, p, 1, 0) == 1 && SubPriority(p, 1) == 0x101 && SubPriority(p, 0) == 0);
	CHECK(PANEL_VT(p)->setFocus(o, p, 0, 0) == 1 && SubPriority(p, 1) == 1 && SubPriority(p, 0) == 0x100);
	CHECK(PANEL_VT(p)->setFocus(o, p, -1, 0) == 1 && SubPriority(p, 0) == 0);
	group.flags = 0;
	CHECK(SpritePanel_Start(o, p, &desc) == 0);
	CHECK(PANEL_VT(p)->setFocus(o, p, 1, 0) == 1 && SubPriority(p, 1) == 1);
	CHECK(SubObj_Delete(o, id));
	EngineDown(o);
}

static void TestMenu(void)
{
	Thread_t* th;
	OpenBGI_t* o = EngineUp(&th);
	OpenBGI_Target_t t[16];
	uint32_t items[2], hw;
	const char* face = "\x82\x6c\x82\x72 \x83\x53\x83\x56\x83\x62\x83\x4e"; // ＭＳ ゴシック (MS Gothic)
	int font = 0, n;
	printf("menu\n");
	if(!o)
		return;
	WindowUp(o);
	hw = MakeWindow(o, 20, 30, 400, 300, 6);
	if(BmpMgr_FontOpen(o, o->gfx.bmpMgr, &font, face, 24, 100, 0) != 0 || GfxCall_WindowSetFont(o, hw, 0, 24, 100, 0, 0, 0) != 0)
	{
		printf("  (no Japanese font available: the menu checks are skipped)\n");
		EngineDown(o);
		return;
	}
	strcpy((char*)th->data + 0x10, "one");
	strcpy((char*)th->data + 0x20, "two");
	items[0] = ((uint32_t)o->engine.profile->tagData << o->engine.profile->tagShift) | 0x10;
	items[1] = ((uint32_t)o->engine.profile->tagData << o->engine.profile->tagShift) | 0x20;
	CHECK(StartMenuSelect(o, th, hw, 2, items, 1, 0, 0xffffff, 1, 0) == 0);
	n = Gather(o, t, 16);
	CHECK(n == 2 && Count(t, n, OPENBGI_TARGET_ITEM) == 2); // its window's layer is claimed
	CHECK(t[0].index == 0 && t[1].index == 1 && t[0].owner == Thread_GetId(th));
	CHECK(t[0].flags == 0 && t[1].flags == OPENBGI_TARGET_SELECTED);
	CHECK(t[0].rect.x >= 20 && t[0].rect.y >= 30 && t[1].rect.y > t[0].rect.y && t[0].rect.w > 0 && t[0].rect.h == 24);
	CHECK(t[0].clickX == t[0].rect.x + t[0].rect.w / 2);
	Thread_DropWait(o, th);
	CHECK(Gather(o, t, 16) == 0);
	EngineDown(o);
}

static void TestText(void)
{
	Thread_t* th;
	OpenBGI_t* o = EngineUp(&th);
	OpenBGI_Target_t t[16];
	WaitTextOut_t* w;
	uint32_t hw;
	int n;
	printf("text\n");
	if(!o)
		return;
	WindowUp(o);
	hw = MakeWindow(o, 20, 300, 600, 160, 6);
	o->text.textLayerMode = 2; // the window's priority
	w = WaitTextOut_New(o, th, (Window_t*)Gfx_FindWindow(o->gfx.gfx, hw));
	Thread_SetWait(o, th, &w->w);
	n = Gather(o, t, 16);
	CHECK(n == 1 && t[0].kind == OPENBGI_TARGET_TEXT && t[0].owner == Thread_GetId(th) && t[0].layer == ((6u << 20) | 0xfffff));
	CHECK(t[0].flags == (OPENBGI_TARGET_ANYWHERE | OPENBGI_TARGET_HOVER) && t[0].rect.w == W && t[0].clickX == W / 2);
	Thread_DropWait(o, th);
	CHECK(Gather(o, t, 16) == 0);
	EngineDown(o);
}

static void TestReset(void)
{
	Thread_t* th;
	OpenBGI_t* o = EngineUp(&th);
	OpenBGI_Target_t t[16];
	PanelButton_t button;
	PanelGroup_t group;
	PanelDesc_t desc;
	uint32_t thumb, knob, hw, id, s;
	printf("reset\n");
	if(!o)
		return;
	WindowUp(o);
	o->sysobj.semMgr = SemMgr_New();
	MakeBitmap(o, 10, 20, 30, 0);
	thumb = MakeSprite(o, 10, 200, 300, 2);
	CHECK(GfxCall_KnobCreate(o, &knob, thumb) == 0 && Knob_Register(o, knob));
	GfxCall_KnobShow(o, knob, 1);
	s = MakeSprite(o, 10, 20, 30, 1);
	CHECK(Target_Add(o, s));
	hw = MakeWindow(o, 100, 50, 400, 300, 5);
	id = SubObj_Create(o, hw, 0);
	memset(&button, 0, sizeof button);
	button.enabled = 1;
	button.bmpNormal = 10;
	button.bmpSelected = button.bmpFocus = button.hitMaskBmp = -1;
	memset(&group, 0, sizeof group);
	group.count = group.columns = 1;
	group.buttons = &button;
	group.selected = group.linkId = -1;
	memset(&desc, 0, sizeof desc);
	desc.groupCount = 1;
	desc.groups = &group;
	desc.currentGroup = -1;
	CHECK(ButtonPanel_Start(o, (ButtonPanel_t*)SubObj_Find(o, id), &desc) == 0);
	CHECK(Gather(o, t, 16) == 3);
	Vm_ResetSubsystems(o);
	CHECK(Gather(o, t, 16) == 0);
	CHECK(!SubObj_Find(o, id) && !Gfx_FindKnob(o->gfx.gfx, knob));
	SemMgr_Destroy(o->sysobj.semMgr);
	o->sysobj.semMgr = NULL;
	EngineDown(o);
}

int main(void)
{
	OS_Init();
	TestNothing();
	TestLayers();
	TestSprites();
	TestKnobs();
	TestIcons();
	TestPanel();
	TestSpritePanel();
	TestMenu();
	TestText();
	TestReset();
	OS_Shutdown();
	if(gFails)
	{
		printf("targets: %d checks failed\n", gFails);
		return 1;
	}
	printf("targets: all tests passed\n");
	return 0;
}
