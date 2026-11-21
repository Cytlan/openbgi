/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/input.c - unit test of the input a host hands the engine
 *                 (openbgi.h: OpenBGI_Key, OpenBGI_Pointer, OpenBGI_Button,
 *                 OpenBGI_Wheel, OpenBGI_Focus; src/sys/window.c) and of the
 *                 state the engine keeps from it; built as test_input by
 *                 `make test`
 *
 * An engine without a window or a game: what the calls leave behind is
 * read where the scripts read it - the event queue of "80 AC" (event 3
 * for a fresh press, 0x80 / 0x81 for double clicks and right presses), the
 * pending presses of "80 1D", the key state of "80 11" and "81 11".  The
 * groups of tests, in the order they run:
 *
 *   keys    - a press posts event 3 once, the auto-repeat posts nothing new,
 *             the key state follows press and release, GetKeyboardState's
 *             toggle bit flips with every press; a code past 255 is ignored
 *   system  - F10 as a system key counts as a key and its release too, any
 *             other Alt combination only bumps the input serial
 *   buttons - presses post event 3 with VK_LBUTTON / VK_RBUTTON, set the
 *             pointer, a double click posts 0x80 / 0x81 before the press;
 *             the script's swap ("80 1E") makes the right button the left
 *   wheel   - a notch is a press and release of 0x0E / 0x0F
 *   focus   - a focus loss releases every key, the toggles stay
 *   pointer - where the host says it is, through the display scaling;
 *             a move by the engine goes out with the next frame once
 */
#include "bgi/core.h"
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/sysobj/events.h"
#include "host/os.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

// the next script event: 1 and its three values, 0 when the queue is empty
static int Next(OpenBGI_t* o, uint32_t* a, uint32_t* b)
{
	uint32_t v[3];
	if(!MsgQueue_Get(o, v))
		return 0;
	*a = v[0];
	*b = v[1];
	return 1;
}

// the queue holds exactly the event (a, b)
static int Only(OpenBGI_t* o, uint32_t a, uint32_t b)
{
	uint32_t x, y;
	return Next(o, &x, &y) && x == a && y == b && !Next(o, &x, &y);
}

static void Drain(OpenBGI_t* o)
{
	uint32_t a, b;
	while(Next(o, &a, &b))
	{
	}
}

static int Down(OpenBGI_t* o, int vk)
{
	return (GetKeyStateEx(o, vk) & 0x8000) != 0;
}

static uint8_t Keyboard(OpenBGI_t* o, int vk)
{
	uint8_t k[256];
	Input_KeyboardState(o, k);
	return k[vk];
}

static void TestKeys(OpenBGI_t* o)
{
	printf("keys\n");
	OpenBGI_Key(o, 0x0d, 1, 0);
	CHECK(Only(o, 3, 0x0d));
	CHECK(Down(o, 0x0d) && Keyboard(o, 0x0d) == 0x81);
	OpenBGI_Key(o, 0x0d, 1, OPENBGI_KEY_REPEAT); // the keyboard's auto-repeat: nothing new
	CHECK(!Next(o, &(uint32_t){0}, &(uint32_t){0}));
	OpenBGI_Key(o, 0x0d, 0, 0);
	CHECK(!Down(o, 0x0d) && Keyboard(o, 0x0d) == 0x01); // released, still toggled
	CHECK((GetKeyTrigger(o, 0x0d) & 0x7fffffff) == 1);  // one press pending
	OpenBGI_Key(o, 0x0d, 1, 0);
	OpenBGI_Key(o, 0x0d, 0, 0);
	CHECK(Keyboard(o, 0x0d) == 0x00); // the second press flipped the toggle back
	Drain(o);
	o->vm.sched.running = 1;
	OpenBGI_Key(o, 300, 1, 0); // not a virtual key: ignored, not a script error that stops the machine
	OpenBGI_Key(o, 0, 1, 0);
	CHECK(!Next(o, &(uint32_t){0}, &(uint32_t){0}));
	CHECK(o->vm.sched.running == 1);
	o->vm.sched.running = 0;
}

static void TestSystem(OpenBGI_t* o)
{
	uint32_t serial;
	printf("system\n");
	OpenBGI_Key(o, 0x79, 1, OPENBGI_KEY_SYSTEM); // F10
	CHECK(Only(o, 3, 0x79));
	CHECK(o->input.keyState[0x79].down);
	OpenBGI_Key(o, 0x79, 0, OPENBGI_KEY_SYSTEM);
	CHECK(!o->input.keyState[0x79].down);
	serial = InputSerial_Get(o);
	OpenBGI_Key(o, 'X', 1, OPENBGI_KEY_SYSTEM); // Alt+X: only the serial
	CHECK(InputSerial_Get(o) == serial + 1);
	CHECK(!Next(o, &(uint32_t){0}, &(uint32_t){0}));
	CHECK(!o->input.keyState['X'].down && Down(o, 'X')); // (the window's key state has it all the same)
	OpenBGI_Key(o, 'X', 0, OPENBGI_KEY_SYSTEM);
	CHECK(!Down(o, 'X'));
}

static void TestButtons(OpenBGI_t* o)
{
	printf("buttons\n");
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 1, 10, 20, 0);
	CHECK(Only(o, 3, 1));
	CHECK(Down(o, 1) && o->input.pointerX == 10 && o->input.pointerY == 20);
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 0, 11, 21, 0);
	CHECK(!Down(o, 1) && o->input.pointerX == 11);
	// a double click: 0x80 before the press
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 1, 11, 21, OPENBGI_CLICK_DOUBLE);
	{
		uint32_t a, b;
		CHECK(Next(o, &a, &b) && a == 0x80);
		CHECK(Next(o, &a, &b) && a == 3 && b == 1);
		CHECK(!Next(o, &a, &b));
	}
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 0, 11, 21, 0);
	// the right button: 0x81 with every press, a double click's 0x81 first
	OpenBGI_Button(o, OPENBGI_BUTTON_RIGHT, 1, 0, 0, OPENBGI_CLICK_DOUBLE);
	{
		uint32_t a, b;
		CHECK(Next(o, &a, &b) && a == 0x81);
		CHECK(Next(o, &a, &b) && a == 0x81);
		CHECK(Next(o, &a, &b) && a == 3 && b == 2);
		CHECK(!Next(o, &a, &b));
	}
	OpenBGI_Button(o, OPENBGI_BUTTON_RIGHT, 0, 0, 0, 0);
	// the middle button's double click is a plain press (WM_MBUTTONDBLCLK has no event)
	OpenBGI_Button(o, OPENBGI_BUTTON_MIDDLE, 1, 0, 0, OPENBGI_CLICK_DOUBLE);
	CHECK(Only(o, 3, 4));
	OpenBGI_Button(o, OPENBGI_BUTTON_MIDDLE, 0, 0, 0, 0);
	// "80 1E": the right button acts as the left one, and the left reads either
	CHECK(Input_SetSwapButtons(o, 1));
	OpenBGI_Button(o, OPENBGI_BUTTON_RIGHT, 1, 0, 0, 0);
	{
		uint32_t a, b;
		CHECK(Next(o, &a, &b) && a == 0x81);
		CHECK(Next(o, &a, &b) && a == 3 && b == 1);
	}
	CHECK(Down(o, 1) && !Down(o, 2));
	OpenBGI_Button(o, OPENBGI_BUTTON_RIGHT, 0, 0, 0, 0);
	CHECK(Input_SetSwapButtons(o, 0));
	OpenBGI_Button(o, 7, 1, 0, 0, 0); // no such button
	CHECK(!Next(o, &(uint32_t){0}, &(uint32_t){0}));
	Drain(o);
}

static void TestWheel(OpenBGI_t* o)
{
	printf("wheel\n");
	OpenBGI_Wheel(o, 120);
	CHECK(Only(o, 3, 0x0e));
	CHECK((GetKeyTrigger(o, 0x0e) & 0x7fffffff) == 1);
	OpenBGI_Wheel(o, -240);
	CHECK(Only(o, 3, 0x0f));
	OpenBGI_Wheel(o, 0); // nothing
	CHECK(!Next(o, &(uint32_t){0}, &(uint32_t){0}));
}

static void TestFocus(OpenBGI_t* o)
{
	printf("focus\n");
	OpenBGI_Focus(o, 1);
	CHECK(Window_IsActive(o));
	OpenBGI_Key(o, 'A', 1, 0);
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 1, 0, 0, 0);
	CHECK(Down(o, 'A') && Down(o, 1));
	OpenBGI_Focus(o, 0);
	CHECK(!Window_IsActive(o));
	CHECK(!(Keyboard(o, 'A') & 0x80) && !(Keyboard(o, 1) & 0x80));
	CHECK(Keyboard(o, 'A') & 1); // the toggle stays
	OpenBGI_Focus(o, 1);
	CHECK((GetKeyTrigger(o, 'A') & 0x80000000u) == 0); // the lost release was taken
	Drain(o);
}

static void TestPointer(OpenBGI_t* o)
{
	OpenBGI_Frame_t f = {.size = sizeof f};
	int xy[2];
	printf("pointer\n");
	GetMouseClientPos(o, xy);
	CHECK(xy[0] == 0 && xy[1] == 0); // no window: 0, 0
	o->display.windowAlive = 1;
	OpenBGI_Pointer(o, 123, 45);
	GetMouseClientPos(o, xy);
	CHECK(xy[0] == 123 && xy[1] == 45);
	OpenBGI_Pointer(o, -5, 700); // outside the picture
	GetMouseClientPos(o, xy);
	CHECK(xy[0] == -5 && xy[1] == 700);
	memset(&f, 0, sizeof f);
	Pointer_TakeFrame(o, &f);
	CHECK(!f.pointerMoved); // the host's own moves are not reported back
	SetCursorBufferPos(o, 30, 40);
	GetMouseClientPos(o, xy);
	CHECK(xy[0] == 30 && xy[1] == 40); // at once
	Pointer_TakeFrame(o, &f);
	CHECK(f.pointerMoved && f.pointerX == 30 && f.pointerY == 40);
	Pointer_TakeFrame(o, &f);
	CHECK(!f.pointerMoved); // once
	o->display.windowAlive = 0;
}

int main(void)
{
	OpenBGI_t* o = OpenBGI_New(OS_Host());
	if(!o)
		return 1;
	TestKeys(o);
	TestSystem(o);
	TestButtons(o);
	TestWheel(o);
	TestFocus(o);
	TestPointer(o);
	MsgQueue_Clear(o);
	OpenBGI_Delete(o);
	if(gFails)
	{
		printf("input: %d checks failed\n", gFails);
		return 1;
	}
	printf("input: all tests passed\n");
	return 0;
}
