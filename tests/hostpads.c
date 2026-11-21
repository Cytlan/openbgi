/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/hostpads.c - unit test of what the hosts make of the systems' pad
 *                    interfaces (src/host/os_pads.c, host/os_pads.h);
 *                    built as test_hostpads by `make test`
 *
 * The functions only translate numbers, so every system's are tested on
 * every system.  The groups of tests, in the order they run:
 *
 *   which    - which of Linux's event devices are pads: a gamepad, a
 *              joystick; not a keyboard, a mouse, a pad's touch surface
 *              or its motion sensor
 *   gamepad  - a gamepad's event device: its buttons by what they are,
 *              the Xbox driver's swapped pair, the triggers as buttons
 *              and as axes, the D-pad as a hat and as buttons, the sticks
 *              over their ranges, the right stick on Z and RZ, what
 *              counts as a change, and events that say nothing
 *   joystick - a joystick's event device: its buttons by their numbers
 *   xinput   - XInput's buttons, triggers, D-pad and sticks
 *   winmm    - the Windows joystick API: the buttons, the hat's eight
 *              ways, the sticks over their ranges, the right stick on
 *              Z and R or on U and V
 *   path     - the HID paths that XInput serves
 *   browser  - the Gamepad API: the standard mapping, another pad's
 *              buttons by their numbers, the axes
 */
#include "host/os_pads.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

// the numbers of linux/input-event-codes.h the tests use
enum
{
	EV_SYN = 0,
	EV_KEY = 1,
	EV_REL = 2,
	EV_ABS = 3,
	KEY_A = 30,
	BTN_LEFT = 0x110,
	BTN_TRIGGER = 0x120,
	BTN_SOUTH = 0x130,
	BTN_EAST = 0x131,
	BTN_C = 0x132,
	BTN_NORTH = 0x133,
	BTN_WEST = 0x134,
	BTN_Z = 0x135,
	BTN_TL = 0x136,
	BTN_TR = 0x137,
	BTN_TL2 = 0x138,
	BTN_TR2 = 0x139,
	BTN_SELECT = 0x13a,
	BTN_START = 0x13b,
	BTN_MODE = 0x13c,
	BTN_THUMBL = 0x13d,
	BTN_THUMBR = 0x13e,
	BTN_TOUCH = 0x14a,
	BTN_DPAD_UP = 0x220,
	BTN_DPAD_DOWN = 0x221,
	BTN_DPAD_LEFT = 0x222,
	BTN_DPAD_RIGHT = 0x223,
	BTN_TRIGGER_HAPPY = 0x2c0,
	ABS_X = 0,
	ABS_Y = 1,
	ABS_Z = 2,
	ABS_RX = 3,
	ABS_RY = 4,
	ABS_RZ = 5,
	ABS_HAT0X = 16,
	ABS_HAT0Y = 17
};

static uint8_t gKeys[OSPADS_KEY_BYTES];        // a device's keys and buttons
static uint8_t gAbsBits[OSPADS_ABS_COUNT / 8]; // .. and its axes
static OsPadsAxis_t gAbs[OSPADS_ABS_COUNT];

static void Clear(void)
{
	memset(gKeys, 0, sizeof gKeys);
	memset(gAbsBits, 0, sizeof gAbsBits);
	memset(gAbs, 0, sizeof gAbs);
}

static void Key(int code)
{
	gKeys[code / 8] |= (uint8_t)(1 << (code % 8));
}

static void Axis(int code, int min, int max)
{
	gAbsBits[code / 8] |= (uint8_t)(1 << (code % 8));
	gAbs[code].has = 1;
	gAbs[code].min = min;
	gAbs[code].max = max;
}

static void TestWhich(void)
{
	int i;
	Clear();
	CHECK(!OsPads_EvdevIsPad(gKeys, gAbsBits)); // nothing at all
	Key(BTN_SOUTH);
	CHECK(OsPads_EvdevIsPad(gKeys, gAbsBits)); // a gamepad, whatever else it has
	Clear();
	Key(BTN_TRIGGER);
	CHECK(!OsPads_EvdevIsPad(gKeys, gAbsBits)); // a joystick's button without a stick
	Axis(ABS_X, 0, 255);
	CHECK(!OsPads_EvdevIsPad(gKeys, gAbsBits));
	Axis(ABS_Y, 0, 255);
	CHECK(OsPads_EvdevIsPad(gKeys, gAbsBits)); // a joystick
	Clear();
	Key(BTN_TRIGGER + 15);
	Axis(ABS_X, 0, 255);
	Axis(ABS_Y, 0, 255);
	CHECK(OsPads_EvdevIsPad(gKeys, gAbsBits)); // (any of its sixteen buttons)
	Clear();
	for(i = 1; i < 128; i++)
		Key(i);
	CHECK(!OsPads_EvdevIsPad(gKeys, gAbsBits)); // a keyboard
	Clear();
	Key(BTN_LEFT);
	Key(BTN_LEFT + 1);
	CHECK(!OsPads_EvdevIsPad(gKeys, gAbsBits)); // a mouse
	Clear();
	Key(BTN_LEFT);
	Key(BTN_TOUCH);
	Axis(ABS_X, 0, 1919);
	Axis(ABS_Y, 0, 1079);
	CHECK(!OsPads_EvdevIsPad(gKeys, gAbsBits)); // a pad's touch surface
	Clear();
	Axis(ABS_X, -32768, 32767);
	Axis(ABS_Y, -32768, 32767);
	Axis(ABS_Z, -32768, 32767);
	CHECK(!OsPads_EvdevIsPad(gKeys, gAbsBits)); // its motion sensor
}

// a gamepad as the kernel's drivers describe one: sticks -32768 .. 32767, triggers 0 .. 255, a hat
static void Gamepad(OsPadsEvdev_t* p, const char* name, int vendor)
{
	static const int kKeys[] = {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};
	size_t i;
	Clear();
	for(i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++)
		Key(kKeys[i]);
	Axis(ABS_X, -32768, 32767);
	Axis(ABS_Y, -32768, 32767);
	Axis(ABS_RX, -32768, 32767);
	Axis(ABS_RY, -32768, 32767);
	Axis(ABS_Z, 0, 255);
	Axis(ABS_RZ, 0, 255);
	Axis(ABS_HAT0X, -1, 1);
	Axis(ABS_HAT0Y, -1, 1);
	OsPads_EvdevInit(p, gKeys, gAbs, name, vendor);
}

static void TestGamepad(void)
{
	static const struct
	{
		int code;
		uint32_t bit;
	} kButtons[] = {
		{BTN_SOUTH, OPENBGI_PAD_SOUTH},
		{BTN_EAST, OPENBGI_PAD_EAST},
		{BTN_NORTH, OPENBGI_PAD_NORTH},
		{BTN_WEST, OPENBGI_PAD_WEST},
		{BTN_TL, OPENBGI_PAD_L1},
		{BTN_TR, OPENBGI_PAD_R1},
		{BTN_TL2, OPENBGI_PAD_L2},
		{BTN_TR2, OPENBGI_PAD_R2},
		{BTN_SELECT, OPENBGI_PAD_SELECT},
		{BTN_START, OPENBGI_PAD_START},
		{BTN_MODE, OPENBGI_PAD_GUIDE},
		{BTN_THUMBL, OPENBGI_PAD_L3},
		{BTN_THUMBR, OPENBGI_PAD_R3},
		{BTN_C, 1u << 13},
		{BTN_Z, 1u << 14},
	};
	OsPadsEvdev_t p;
	OpenBGI_PadState_t none;
	size_t i;
	memset(&none, 0, sizeof none);
	none.size = sizeof none;
	Gamepad(&p, "Sony Interactive Entertainment Wireless Controller", 0x054c);
	CHECK(p.gamepad && !p.swapNorthWest && !p.zIsStick);
	CHECK(memcmp(&p.state, &none, sizeof none) == 0);
	// every button by itself: its bit goes down and up, and each is a change
	for(i = 0; i < sizeof kButtons / sizeof kButtons[0]; i++)
	{
		CHECK(OsPads_EvdevEvent(&p, EV_KEY, kButtons[i].code, 1) == 1);
		CHECK(p.state.buttons == kButtons[i].bit);
		CHECK(OsPads_EvdevEvent(&p, EV_KEY, kButtons[i].code, 1) == 0); // (said again: no change)
		CHECK(OsPads_EvdevEvent(&p, EV_KEY, kButtons[i].code, 0) == 1);
		CHECK(p.state.buttons == 0);
	}
	// two at once
	OsPads_EvdevEvent(&p, EV_KEY, BTN_SOUTH, 1);
	OsPads_EvdevEvent(&p, EV_KEY, BTN_TR, 1);
	CHECK(p.state.buttons == (OPENBGI_PAD_SOUTH | OPENBGI_PAD_R1));
	OsPads_EvdevEvent(&p, EV_KEY, BTN_SOUTH, 0);
	CHECK(p.state.buttons == OPENBGI_PAD_R1);
	OsPads_EvdevEvent(&p, EV_KEY, BTN_TR, 0);
	// what says nothing: keys of other devices, other event types, codes out of range
	CHECK(OsPads_EvdevEvent(&p, EV_KEY, KEY_A, 1) == 0);
	CHECK(OsPads_EvdevEvent(&p, EV_KEY, BTN_TRIGGER, 1) == 0); // (a joystick's number on a gamepad)
	CHECK(OsPads_EvdevEvent(&p, EV_KEY, 0x2ff, 1) == 0);
	CHECK(OsPads_EvdevEvent(&p, EV_SYN, 0, 0) == 0);
	CHECK(OsPads_EvdevEvent(&p, EV_REL, 0, 5) == 0);
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, OSPADS_ABS_COUNT, 5) == 0);
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, -1, 5) == 0);
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, 40, 5) == 0); // (an axis that is none of a pad's)
	CHECK(memcmp(&p.state, &none, sizeof none) == 0);

	// the triggers as axes: down past half their travel
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, ABS_Z, 127) == 0);
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, ABS_Z, 128) == 1);
	CHECK(p.state.buttons == OPENBGI_PAD_L2);
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, ABS_RZ, 255) == 1);
	CHECK(p.state.buttons == (OPENBGI_PAD_L2 | OPENBGI_PAD_R2));
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, ABS_Z, 0) == 1);
	CHECK(p.state.buttons == OPENBGI_PAD_R2);
	// .. beside the same trigger's button: either holds it
	OsPads_EvdevEvent(&p, EV_KEY, BTN_TR2, 1);
	OsPads_EvdevEvent(&p, EV_ABS, ABS_RZ, 0);
	CHECK(p.state.buttons == OPENBGI_PAD_R2);
	OsPads_EvdevEvent(&p, EV_KEY, BTN_TR2, 0);
	CHECK(p.state.buttons == 0);
	CHECK(p.state.rightX == 0 && p.state.rightY == 0); // (the triggers are no stick here)

	// the D-pad as a hat
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, ABS_HAT0X, -1) == 1 && p.state.dpad == OPENBGI_DPAD_LEFT);
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, ABS_HAT0Y, -1) == 1 && p.state.dpad == (OPENBGI_DPAD_LEFT | OPENBGI_DPAD_UP));
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, ABS_HAT0X, 1) == 1 && p.state.dpad == (OPENBGI_DPAD_RIGHT | OPENBGI_DPAD_UP));
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, ABS_HAT0Y, 1) == 1 && p.state.dpad == (OPENBGI_DPAD_RIGHT | OPENBGI_DPAD_DOWN));
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, ABS_HAT0X, 0) == 1 && p.state.dpad == OPENBGI_DPAD_DOWN);
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, ABS_HAT0Y, 0) == 1 && p.state.dpad == 0);
	// .. and as buttons, and both together
	CHECK(OsPads_EvdevEvent(&p, EV_KEY, BTN_DPAD_UP, 1) == 1 && p.state.dpad == OPENBGI_DPAD_UP);
	CHECK(OsPads_EvdevEvent(&p, EV_KEY, BTN_DPAD_UP, 0) == 1 && p.state.dpad == 0);
	CHECK(OsPads_EvdevEvent(&p, EV_KEY, BTN_DPAD_DOWN, 1) == 1 && p.state.dpad == OPENBGI_DPAD_DOWN);
	CHECK(OsPads_EvdevEvent(&p, EV_KEY, BTN_DPAD_LEFT, 1) == 1 && p.state.dpad == (OPENBGI_DPAD_DOWN | OPENBGI_DPAD_LEFT));
	OsPads_EvdevEvent(&p, EV_KEY, BTN_DPAD_DOWN, 0);
	OsPads_EvdevEvent(&p, EV_KEY, BTN_DPAD_LEFT, 0);
	CHECK(OsPads_EvdevEvent(&p, EV_KEY, BTN_DPAD_RIGHT, 1) == 1 && p.state.dpad == OPENBGI_DPAD_RIGHT);
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, ABS_HAT0X, 1) == 0); // (the hat says the same)
	CHECK(OsPads_EvdevEvent(&p, EV_KEY, BTN_DPAD_RIGHT, 0) == 0 && p.state.dpad == OPENBGI_DPAD_RIGHT);
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, ABS_HAT0X, 0) == 1 && p.state.dpad == 0);
	CHECK(p.state.buttons == 0);

	// the sticks over their ranges
	OsPads_EvdevEvent(&p, EV_ABS, ABS_X, -32768);
	OsPads_EvdevEvent(&p, EV_ABS, ABS_Y, 32767);
	OsPads_EvdevEvent(&p, EV_ABS, ABS_RX, 16384);
	OsPads_EvdevEvent(&p, EV_ABS, ABS_RY, 0);
	CHECK(p.state.leftX == -32768 && p.state.leftY == 32767);
	CHECK(p.state.rightX >= 16383 && p.state.rightX <= 16384 && p.state.rightY >= -1 && p.state.rightY <= 0);
	OsPads_EvdevEvent(&p, EV_ABS, ABS_X, 99999); // (beyond what the device said)
	CHECK(p.state.leftX == 32767);
	OsPads_EvdevEvent(&p, EV_ABS, ABS_X, -99999);
	CHECK(p.state.leftX == -32768);

	// what was down is forgotten on request
	OsPads_EvdevEvent(&p, EV_KEY, BTN_SOUTH, 1);
	OsPads_EvdevEvent(&p, EV_ABS, ABS_Z, 255);
	OsPads_EvdevEvent(&p, EV_ABS, ABS_HAT0X, 1);
	OsPads_EvdevEvent(&p, EV_KEY, BTN_DPAD_UP, 1);
	OsPads_EvdevReset(&p);
	CHECK(memcmp(&p.state, &none, sizeof none) == 0);
	CHECK(OsPads_EvdevEvent(&p, EV_KEY, BTN_EAST, 1) == 1 && p.state.buttons == OPENBGI_PAD_EAST && p.state.dpad == 0);
	CHECK(p.gamepad && p.abs[ABS_X].has); // (what the device is stays)

	// the Xbox pads' driver: X, the left button, has BTN_NORTH's number
	Gamepad(&p, "Microsoft X-Box 360 pad", 0x045e);
	CHECK(p.swapNorthWest);
	OsPads_EvdevEvent(&p, EV_KEY, BTN_NORTH, 1);
	CHECK(p.state.buttons == OPENBGI_PAD_WEST);
	OsPads_EvdevEvent(&p, EV_KEY, BTN_WEST, 1);
	CHECK(p.state.buttons == (OPENBGI_PAD_WEST | OPENBGI_PAD_NORTH));
	Gamepad(&p, "Some Maker's Pad for Xbox", 0x0e6f); // (another maker's, by its name)
	CHECK(p.swapNorthWest);
	Gamepad(&p, "Generic X-BOX pad", 0x24c6);
	CHECK(p.swapNorthWest);
	Gamepad(&p, "Some Pad", 0x045e); // (Microsoft's, by the vendor)
	CHECK(p.swapNorthWest);
	Gamepad(&p, "", 0);
	CHECK(!p.swapNorthWest);

	// an older pad: 0 .. 255 on every axis, the right stick on Z and RZ, the triggers buttons
	Clear();
	Key(BTN_SOUTH);
	Key(BTN_TL2);
	Key(BTN_TR2);
	Axis(ABS_X, 0, 255);
	Axis(ABS_Y, 0, 255);
	Axis(ABS_Z, 0, 255);
	Axis(ABS_RZ, 0, 255);
	OsPads_EvdevInit(&p, gKeys, gAbs, "USB Gamepad", 0x0079);
	CHECK(p.gamepad && p.zIsStick);
	OsPads_EvdevEvent(&p, EV_ABS, ABS_Z, 255);
	OsPads_EvdevEvent(&p, EV_ABS, ABS_RZ, 0);
	CHECK(p.state.rightX == 32767 && p.state.rightY == -32768 && p.state.buttons == 0);
	OsPads_EvdevEvent(&p, EV_ABS, ABS_X, 128); // the centre of an eight-bit axis: all but none
	OsPads_EvdevEvent(&p, EV_ABS, ABS_Y, 127);
	CHECK(p.state.leftX > 0 && p.state.leftX < 400 && p.state.leftY < 0 && p.state.leftY > -400);
	OsPads_EvdevEvent(&p, EV_ABS, ABS_RX, 255); // (it has no such axis)
	CHECK(p.state.rightX == 32767);
	// with RX and RY beside them, Z and RZ are the triggers
	Axis(ABS_RX, 0, 255);
	Axis(ABS_RY, 0, 255);
	OsPads_EvdevInit(&p, gKeys, gAbs, "USB Gamepad", 0x0079);
	CHECK(!p.zIsStick);
	// an axis whose range says nothing stays at rest
	Clear();
	Key(BTN_SOUTH);
	Axis(ABS_X, 5, 5);
	Axis(ABS_Y, 0, 255);
	OsPads_EvdevInit(&p, gKeys, gAbs, "Odd", 0);
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, ABS_X, 5) == 0 && p.state.leftX == 0);
}

static void TestJoystick(void)
{
	OsPadsEvdev_t p;
	int i;
	Clear();
	for(i = 0; i < 12; i++)
		Key(BTN_TRIGGER + i);
	Axis(ABS_X, 0, 1023);
	Axis(ABS_Y, 0, 1023);
	Axis(ABS_HAT0X, -1, 1);
	Axis(ABS_HAT0Y, -1, 1);
	OsPads_EvdevInit(&p, gKeys, gAbs, "Some Flight Stick for Xbox", 0x045e);
	CHECK(!p.gamepad && !p.swapNorthWest); // (nothing to swap: its buttons are numbers)
	for(i = 0; i < 16; i++)
	{
		CHECK(OsPads_EvdevEvent(&p, EV_KEY, BTN_TRIGGER + i, 1) == 1);
		CHECK(p.state.buttons == 1u << i);
		CHECK(OsPads_EvdevEvent(&p, EV_KEY, BTN_TRIGGER + i, 0) == 1);
	}
	for(i = 0; i < 16; i++)
	{ // the buttons past the sixteenth
		CHECK(OsPads_EvdevEvent(&p, EV_KEY, BTN_TRIGGER_HAPPY + i, 1) == 1);
		CHECK(p.state.buttons == 1u << (16 + i));
		CHECK(OsPads_EvdevEvent(&p, EV_KEY, BTN_TRIGGER_HAPPY + i, 0) == 1);
	}
	CHECK(OsPads_EvdevEvent(&p, EV_KEY, BTN_SOUTH, 1) == 0); // (a gamepad's name on a joystick)
	OsPads_EvdevEvent(&p, EV_ABS, ABS_X, 0);
	OsPads_EvdevEvent(&p, EV_ABS, ABS_Y, 1023);
	OsPads_EvdevEvent(&p, EV_ABS, ABS_HAT0Y, -1);
	CHECK(p.state.leftX == -32768 && p.state.leftY == 32767 && p.state.dpad == OPENBGI_DPAD_UP);
	// a joystick's twist is no trigger
	Axis(ABS_RZ, 0, 255);
	OsPads_EvdevInit(&p, gKeys, gAbs, "Stick", 0);
	CHECK(OsPads_EvdevEvent(&p, EV_ABS, ABS_RZ, 255) == 0 && p.state.buttons == 0);
}

static void TestXInput(void)
{
	static const struct
	{
		uint32_t from, to;
	} kButtons[] = {
		{0x1000, OPENBGI_PAD_SOUTH},
		{0x2000, OPENBGI_PAD_EAST},
		{0x4000, OPENBGI_PAD_WEST},
		{0x8000, OPENBGI_PAD_NORTH},
		{0x0100, OPENBGI_PAD_L1},
		{0x0200, OPENBGI_PAD_R1},
		{0x0020, OPENBGI_PAD_SELECT},
		{0x0010, OPENBGI_PAD_START},
		{0x0040, OPENBGI_PAD_L3},
		{0x0080, OPENBGI_PAD_R3},
	};
	OpenBGI_PadState_t s;
	size_t i;
	memset(&s, 0xff, sizeof s);
	OsPads_XInput(0, 0, 0, 0, 0, 0, 0, &s);
	CHECK(s.buttons == 0 && s.dpad == 0 && s.leftX == 0 && s.leftY == 0 && s.rightX == 0 && s.rightY == 0);
	for(i = 0; i < sizeof kButtons / sizeof kButtons[0]; i++)
	{
		OsPads_XInput(kButtons[i].from, 0, 0, 0, 0, 0, 0, &s);
		CHECK(s.buttons == kButtons[i].to && s.dpad == 0);
	}
	OsPads_XInput(0x1000 | 0x0200, 0, 0, 0, 0, 0, 0, &s);
	CHECK(s.buttons == (OPENBGI_PAD_SOUTH | OPENBGI_PAD_R1));
	// the D-pad: up, down, left, right
	OsPads_XInput(1, 0, 0, 0, 0, 0, 0, &s);
	CHECK(s.dpad == OPENBGI_DPAD_UP && s.buttons == 0);
	OsPads_XInput(2, 0, 0, 0, 0, 0, 0, &s);
	CHECK(s.dpad == OPENBGI_DPAD_DOWN);
	OsPads_XInput(4, 0, 0, 0, 0, 0, 0, &s);
	CHECK(s.dpad == OPENBGI_DPAD_LEFT);
	OsPads_XInput(8 | 1, 0, 0, 0, 0, 0, 0, &s);
	CHECK(s.dpad == (OPENBGI_DPAD_RIGHT | OPENBGI_DPAD_UP));
	// the triggers: down past half
	OsPads_XInput(0, 127, 127, 0, 0, 0, 0, &s);
	CHECK(s.buttons == 0);
	OsPads_XInput(0, 128, 0, 0, 0, 0, 0, &s);
	CHECK(s.buttons == OPENBGI_PAD_L2);
	OsPads_XInput(0, 0, 128, 0, 0, 0, 0, &s);
	CHECK(s.buttons == OPENBGI_PAD_R2);
	OsPads_XInput(0, 255, 255, 0, 0, 0, 0, &s);
	CHECK(s.buttons == (OPENBGI_PAD_L2 | OPENBGI_PAD_R2));
	// the sticks: XInput's up is the engine's down
	OsPads_XInput(0, 0, 0, 1000, 2000, -3000, -4000, &s);
	CHECK(s.leftX == 1000 && s.leftY == -2000 && s.rightX == -3000 && s.rightY == 4000);
	OsPads_XInput(0, 0, 0, -32768, -32768, 32767, 32767, &s);
	CHECK(s.leftX == -32768 && s.leftY == 32767 && s.rightX == 32767 && s.rightY == -32767);
}

static void TestWinmm(void)
{
	// what a pad of the common kind reports: sixteen bits an axis; the hat in hundredths of a degree
	static const uint32_t kMin[6] = {0, 0, 0, 0, 0, 0}, kMax[6] = {65535, 65535, 65535, 65535, 65535, 65535};
	static const struct
	{
		uint32_t pov, dpad;
	} kHat[] = {
		{0, OPENBGI_DPAD_UP},
		{4500, OPENBGI_DPAD_UP | OPENBGI_DPAD_RIGHT},
		{9000, OPENBGI_DPAD_RIGHT},
		{13500, OPENBGI_DPAD_RIGHT | OPENBGI_DPAD_DOWN},
		{18000, OPENBGI_DPAD_DOWN},
		{22500, OPENBGI_DPAD_DOWN | OPENBGI_DPAD_LEFT},
		{27000, OPENBGI_DPAD_LEFT},
		{31500, OPENBGI_DPAD_LEFT | OPENBGI_DPAD_UP},
		{36000, OPENBGI_DPAD_UP}, // (a full turn)
		{2000, OPENBGI_DPAD_UP},  // a hat that is not exact: the nearest of the eight ways
		{2500, OPENBGI_DPAD_UP | OPENBGI_DPAD_RIGHT},
		{7000, OPENBGI_DPAD_RIGHT},
		{34000, OPENBGI_DPAD_UP},
		{0xffff, 0}, // centred
	};
	enum
	{
		HAS_Z = 1,
		HAS_R = 2,
		HAS_U = 4,
		HAS_V = 8,
		HAS_POV = 16
	};
	uint32_t pos[6] = {32768, 32768, 32768, 32768, 32768, 32768};
	OpenBGI_PadState_t s;
	size_t i;
	memset(&s, 0xff, sizeof s);
	OsPads_Joystick(0, HAS_Z | HAS_R | HAS_POV, 0xffff, pos, kMin, kMax, &s);
	CHECK(s.buttons == 0 && s.dpad == 0);
	CHECK(s.leftX == 0 && s.leftY == 0 && s.rightX == 0 && s.rightY == 0);
	// the buttons keep their numbers
	OsPads_Joystick(0x80000005u, HAS_POV, 0xffff, pos, kMin, kMax, &s);
	CHECK(s.buttons == 0x80000005u);
	for(i = 0; i < sizeof kHat / sizeof kHat[0]; i++)
	{
		OsPads_Joystick(0, HAS_POV, kHat[i].pov, pos, kMin, kMax, &s);
		CHECK(s.dpad == kHat[i].dpad);
	}
	OsPads_Joystick(0, 0, 0, pos, kMin, kMax, &s); // without a hat its position says nothing
	CHECK(s.dpad == 0);
	// the left stick on X and Y
	pos[0] = 0;
	pos[1] = 65535;
	OsPads_Joystick(0, 0, 0xffff, pos, kMin, kMax, &s);
	CHECK(s.leftX == -32768 && s.leftY == 32767 && s.rightX == 0 && s.rightY == 0);
	// the right one on Z and R
	pos[2] = 65535;
	pos[3] = 0;
	pos[4] = 100;
	pos[5] = 200;
	OsPads_Joystick(0, HAS_Z | HAS_R | HAS_U | HAS_V, 0xffff, pos, kMin, kMax, &s);
	CHECK(s.rightX == 32767 && s.rightY == -32768);
	OsPads_Joystick(0, HAS_Z, 0xffff, pos, kMin, kMax, &s); // (Z alone is no stick)
	CHECK(s.rightX == 0 && s.rightY == 0);
	OsPads_Joystick(0, HAS_R, 0xffff, pos, kMin, kMax, &s);
	CHECK(s.rightX == 0 && s.rightY == 0);
	// .. or on U and V when there is no R (Z is the two triggers then)
	pos[4] = 0;
	pos[5] = 65535;
	OsPads_Joystick(0, HAS_Z | HAS_U | HAS_V, 0xffff, pos, kMin, kMax, &s);
	CHECK(s.rightX == -32768 && s.rightY == 32767);
	OsPads_Joystick(0, HAS_Z | HAS_U, 0xffff, pos, kMin, kMax, &s);
	CHECK(s.rightX == 0 && s.rightY == 0);
	{ // another range, a position beyond it, and a range that says nothing
		static const uint32_t min[6] = {100, 0, 0, 0, 0, 0}, max[6] = {355, 0, 0, 0, 0, 0};
		uint32_t at[6] = {100, 7, 0, 0, 0, 0};
		OsPads_Joystick(0, 0, 0xffff, at, min, max, &s);
		CHECK(s.leftX == -32768 && s.leftY == 0);
		at[0] = 355;
		OsPads_Joystick(0, 0, 0xffff, at, min, max, &s);
		CHECK(s.leftX == 32767);
		at[0] = 228;
		OsPads_Joystick(0, 0, 0xffff, at, min, max, &s);
		CHECK(s.leftX > 0 && s.leftX < 400);
		at[0] = 1000;
		OsPads_Joystick(0, 0, 0xffff, at, min, max, &s);
		CHECK(s.leftX == 32767);
		at[0] = 3;
		OsPads_Joystick(0, 0, 0xffff, at, min, max, &s);
		CHECK(s.leftX == -32768);
	}
}

static void TestPath(void)
{
	CHECK(OsPads_PathIsXInput("\\\\?\\HID#VID_045E&PID_028E&IG_00#7&2a9a7b5c&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}"));
	CHECK(OsPads_PathIsXInput("\\\\?\\hid#vid_045e&pid_02ea&ig_00#8&1"));
	CHECK(!OsPads_PathIsXInput("\\\\?\\HID#VID_054C&PID_05C4#7&3869ac07&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}"));
	CHECK(!OsPads_PathIsXInput("\\\\?\\HID#VID_0079&PID_0006&MI_00#8&1"));
	CHECK(!OsPads_PathIsXInput(""));
	CHECK(!OsPads_PathIsXInput("IG"));
}

static void TestBrowser(void)
{
	static const uint32_t kStandard[17] = {
		OPENBGI_PAD_SOUTH, OPENBGI_PAD_EAST, OPENBGI_PAD_WEST, OPENBGI_PAD_NORTH, OPENBGI_PAD_L1, OPENBGI_PAD_R1, OPENBGI_PAD_L2, OPENBGI_PAD_R2,
		OPENBGI_PAD_SELECT, OPENBGI_PAD_START, OPENBGI_PAD_L3, OPENBGI_PAD_R3, 0, 0, 0, 0, OPENBGI_PAD_GUIDE};
	static const uint32_t kDpad[4] = {OPENBGI_DPAD_UP, OPENBGI_DPAD_DOWN, OPENBGI_DPAD_LEFT, OPENBGI_DPAD_RIGHT};
	OpenBGI_PadState_t s;
	int i;
	memset(&s, 0xff, sizeof s);
	OsPads_Gamepad(1, 0, 0, 0, 0, 0, &s);
	CHECK(s.buttons == 0 && s.dpad == 0 && s.leftX == 0 && s.leftY == 0 && s.rightX == 0 && s.rightY == 0);
	for(i = 0; i < 17; i++)
	{
		OsPads_Gamepad(1, 1u << i, 0, 0, 0, 0, &s);
		CHECK(s.buttons == kStandard[i]);
		CHECK(s.dpad == (i >= 12 && i < 16 ? kDpad[i - 12] : 0u));
	}
	OsPads_Gamepad(1, 0xfffe0000u, 0, 0, 0, 0, &s); // (the standard mapping has nothing past its seventeen)
	CHECK(s.buttons == 0 && s.dpad == 0);
	OsPads_Gamepad(1, 1u | (1u << 5) | (1u << 13) | (1u << 15), 0, 0, 0, 0, &s);
	CHECK(s.buttons == (OPENBGI_PAD_SOUTH | OPENBGI_PAD_R1) && s.dpad == (OPENBGI_DPAD_DOWN | OPENBGI_DPAD_RIGHT));
	// another pad: its buttons by their numbers, no D-pad
	OsPads_Gamepad(0, 0x8000f005u, 0, 0, 0, 0, &s);
	CHECK(s.buttons == 0x8000f005u && s.dpad == 0);
	// the axes: thousandths, down and right positive as the engine has them
	OsPads_Gamepad(1, 0, 1000, -1000, 500, -250, &s);
	CHECK(s.leftX == 32767 && s.leftY == -32767 && s.rightX == 16383 && s.rightY == -8191);
	OsPads_Gamepad(0, 0, 1500, -1500, 0, 0, &s); // (beyond 1: as 1)
	CHECK(s.leftX == 32767 && s.leftY == -32767);
}

int main(void)
{
	TestWhich();
	TestGamepad();
	TestJoystick();
	TestXInput();
	TestWinmm();
	TestPath();
	TestBrowser();
	if(gFails)
	{
		printf("hostpads: %d checks failed\n", gFails);
		return 1;
	}
	printf("hostpads: all tests passed\n");
	return 0;
}
