/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_pads.c - what the systems' pad interfaces report, as the engine's pad
 *             state (host/os_pads.h)
 *
 * Linux's event devices name a gamepad's buttons by where they sit
 * (BTN_SOUTH ..) and its axes by their use; a joystick without those
 * buttons is taken by its button numbers, which is also what a game's
 * own pad keys ("81 1B") count by.  XInput has one fixed layout.  The
 * Windows joystick API has numbers only: its buttons keep them, as the
 * original engine's DirectInput poll had them.
 */
#include "host/os_pads.h"

#include <string.h>

// ---- Linux (the numbers of linux/input-event-codes.h) ---------------------------------------------

#define EV_KEY       1
#define EV_ABS       3

#define BTN_JOYSTICK 0x120 // .. 0x12f: a joystick's buttons, the trigger first
#define BTN_SOUTH    0x130
#define BTN_EAST     0x131
#define BTN_C        0x132
#define BTN_NORTH    0x133
#define BTN_WEST     0x134
#define BTN_Z        0x135
#define BTN_TL       0x136
#define BTN_TR       0x137
#define BTN_TL2      0x138
#define BTN_TR2      0x139
#define BTN_SELECT   0x13a
#define BTN_START    0x13b
#define BTN_MODE     0x13c
#define BTN_THUMBL   0x13d
#define BTN_THUMBR   0x13e
#define BTN_DPAD_UP  0x220 // .. 0x223: up, down, left, right
#define BTN_HAPPY    0x2c0 // .. 0x2cf: a joystick's buttons past its sixteenth

#define ABS_X        0
#define ABS_Y        1
#define ABS_Z        2
#define ABS_RX       3
#define ABS_RY       4
#define ABS_RZ       5
#define ABS_HAT0X    16
#define ABS_HAT0Y    17

static int Bit(const uint8_t* bits, int n)
{
	return (bits[n / 8] >> (n % 8)) & 1;
}

int OsPads_EvdevIsPad(const uint8_t* keyBits, const uint8_t* absBits)
{
	int i;
	if(Bit(keyBits, BTN_SOUTH))
		return 1;
	if(!Bit(absBits, ABS_X) || !Bit(absBits, ABS_Y))
		return 0;
	for(i = 0; i < 16; i++)
		if(Bit(keyBits, BTN_JOYSTICK + i))
			return 1;
	return 0;
}

// `word` is in `text`, whatever the case of ASCII letters
static int Contains(const char* text, const char* word)
{
	size_t n = strlen(word), i;
	for(; *text; text++)
	{
		for(i = 0; i < n; i++)
		{
			int a = text[i], b = word[i];
			a = a >= 'A' && a <= 'Z' ? a - 'A' + 'a' : a;
			b = b >= 'A' && b <= 'Z' ? b - 'A' + 'a' : b;
			if(a != b)
				break;
		}
		if(i == n)
			return 1;
	}
	return 0;
}

void OsPads_EvdevInit(OsPadsEvdev_t* p, const uint8_t* keyBits, const OsPadsAxis_t* abs, const char* name, int vendor)
{
	memset(p, 0, sizeof *p);
	p->state.size = sizeof p->state;
	memcpy(p->abs, abs, sizeof p->abs);
	p->gamepad = Bit(keyBits, BTN_SOUTH);
	p->zIsStick = !(abs[ABS_RX].has && abs[ABS_RY].has) && abs[ABS_Z].has && abs[ABS_RZ].has;
	// the Xbox pads' driver names the buttons by their letters, and X (left) is BTN_NORTH's number
	p->swapNorthWest = p->gamepad && (vendor == 0x045e || Contains(name, "x-box") || Contains(name, "xbox"));
}

void OsPads_EvdevReset(OsPadsEvdev_t* p)
{
	p->keyButtons = p->axisButtons = p->keyDpad = p->hatDpad = 0;
	memset(&p->state, 0, sizeof p->state);
	p->state.size = sizeof p->state;
}

// an axis's value as -32768 .. 32767
static int Stick(const OsPadsAxis_t* a, int value)
{
	int64_t span = (int64_t)a->max - a->min;
	int64_t v;
	if(!a->has || span <= 0)
		return 0;
	v = ((int64_t)value - a->min) * 65535 / span - 32768;
	return v < -32768 ? -32768 : v > 32767 ? 32767
										   : (int)v;
}

// a gamepad's button as the engine's bit; 0 for none of them
static uint32_t GamepadBit(const OsPadsEvdev_t* p, int code)
{
	switch(code)
	{
		case BTN_SOUTH: return OPENBGI_PAD_SOUTH;
		case BTN_EAST: return OPENBGI_PAD_EAST;
		case BTN_NORTH: return p->swapNorthWest ? OPENBGI_PAD_WEST : OPENBGI_PAD_NORTH;
		case BTN_WEST: return p->swapNorthWest ? OPENBGI_PAD_NORTH : OPENBGI_PAD_WEST;
		case BTN_TL: return OPENBGI_PAD_L1;
		case BTN_TR: return OPENBGI_PAD_R1;
		case BTN_TL2: return OPENBGI_PAD_L2;
		case BTN_TR2: return OPENBGI_PAD_R2;
		case BTN_SELECT: return OPENBGI_PAD_SELECT;
		case BTN_START: return OPENBGI_PAD_START;
		case BTN_MODE: return OPENBGI_PAD_GUIDE;
		case BTN_THUMBL: return OPENBGI_PAD_L3;
		case BTN_THUMBR: return OPENBGI_PAD_R3;
		case BTN_C: return 1u << 13; // (the six-button pads' third and sixth: past the named ones)
		case BTN_Z: return 1u << 14;
		default: return 0;
	}
}

int OsPads_EvdevEvent(OsPadsEvdev_t* p, int type, int code, int value)
{
	OpenBGI_PadState_t was = p->state;
	if(type == EV_KEY)
	{
		uint32_t bit = 0, dir = 0;
		if(code >= BTN_DPAD_UP && code < BTN_DPAD_UP + 4)
			dir = 1u << (code - BTN_DPAD_UP);
		else if(p->gamepad)
			bit = GamepadBit(p, code);
		else if(code >= BTN_JOYSTICK && code < BTN_JOYSTICK + 16)
			bit = 1u << (code - BTN_JOYSTICK);
		else if(code >= BTN_HAPPY && code < BTN_HAPPY + 16)
			bit = 1u << (16 + code - BTN_HAPPY);
		p->keyButtons = value ? p->keyButtons | bit : p->keyButtons & ~bit;
		p->keyDpad = value ? p->keyDpad | dir : p->keyDpad & ~dir;
	}
	else if(type == EV_ABS && code >= 0 && code < OSPADS_ABS_COUNT && p->abs[code].has)
	{
		const OsPadsAxis_t* a = &p->abs[code];
		switch(code)
		{
			case ABS_X: p->state.leftX = (int16_t)Stick(a, value); break;
			case ABS_Y: p->state.leftY = (int16_t)Stick(a, value); break;
			case ABS_RX: p->state.rightX = (int16_t)Stick(a, value); break;
			case ABS_RY: p->state.rightY = (int16_t)Stick(a, value); break;
			case ABS_Z:
			case ABS_RZ:
				if(p->zIsStick)
				{
					if(code == ABS_Z)
						p->state.rightX = (int16_t)Stick(a, value);
					else
						p->state.rightY = (int16_t)Stick(a, value);
				}
				else if(p->gamepad)
				{ // a trigger: down past half its travel
					uint32_t bit = code == ABS_Z ? OPENBGI_PAD_L2 : OPENBGI_PAD_R2;
					int down = (int64_t)value - a->min > ((int64_t)a->max - a->min) / 2;
					p->axisButtons = down ? p->axisButtons | bit : p->axisButtons & ~bit;
				}
				break;
			case ABS_HAT0X:
				p->hatDpad &= ~(uint32_t)(OPENBGI_DPAD_LEFT | OPENBGI_DPAD_RIGHT);
				p->hatDpad |= value < 0 ? OPENBGI_DPAD_LEFT : value > 0 ? OPENBGI_DPAD_RIGHT
																		: 0;
				break;
			case ABS_HAT0Y:
				p->hatDpad &= ~(uint32_t)(OPENBGI_DPAD_UP | OPENBGI_DPAD_DOWN);
				p->hatDpad |= value < 0 ? OPENBGI_DPAD_UP : value > 0 ? OPENBGI_DPAD_DOWN
																	  : 0;
				break;
			default: break;
		}
	}
	p->state.buttons = p->keyButtons | p->axisButtons;
	p->state.dpad = p->keyDpad | p->hatDpad;
	return memcmp(&was, &p->state, sizeof was) != 0;
}

// ---- Windows --------------------------------------------------------------------------------------

// a stick's axis that is up or left positive, the other way round
static int16_t Flip(int v)
{
	return (int16_t)(v <= -32768 ? 32767 : -v);
}

void OsPads_XInput(uint32_t buttons, int leftTrigger, int rightTrigger, int lx, int ly, int rx, int ry, OpenBGI_PadState_t* out)
{
	// XINPUT_GAMEPAD_*: the bit of wButtons and the engine's
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
		{0x0400, OPENBGI_PAD_GUIDE},
	};
	size_t i;
	memset(out, 0, sizeof *out);
	out->size = sizeof *out;
	for(i = 0; i < sizeof kButtons / sizeof kButtons[0]; i++)
		if(buttons & kButtons[i].from)
			out->buttons |= kButtons[i].to;
	if(leftTrigger > 127)
		out->buttons |= OPENBGI_PAD_L2;
	if(rightTrigger > 127)
		out->buttons |= OPENBGI_PAD_R2;
	out->dpad = buttons & 15; // (up, down, left, right: the engine's order)
	out->leftX = (int16_t)lx;
	out->leftY = Flip(ly);
	out->rightX = (int16_t)rx;
	out->rightY = Flip(ry);
}

// a joystick axis's position within its range as -32768 .. 32767
static int16_t Axis(uint32_t pos, uint32_t min, uint32_t max)
{
	int64_t v;
	if(max <= min)
		return 0;
	v = ((int64_t)pos - min) * 65535 / ((int64_t)max - min) - 32768;
	return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767
													 : v);
}

void OsPads_Joystick(uint32_t buttons, uint32_t caps, uint32_t pov, const uint32_t* pos, const uint32_t* min, const uint32_t* max,
	OpenBGI_PadState_t* out)
{
	// JOYCAPS_HAS*: the bits of wCaps
	enum
	{
		HAS_Z = 1,
		HAS_R = 2,
		HAS_U = 4,
		HAS_V = 8,
		HAS_POV = 16
	};
	memset(out, 0, sizeof *out);
	out->size = sizeof *out;
	out->buttons = buttons;
	if((caps & HAS_POV) && (pov & 0xffff) != 0xffff)
	{ // the hat's way, a direction 67.5 degrees to either side of its own
		uint32_t a = pov % 36000;
		if(a >= 29250 || a <= 6750)
			out->dpad |= OPENBGI_DPAD_UP;
		if(a >= 2250 && a <= 15750)
			out->dpad |= OPENBGI_DPAD_RIGHT;
		if(a >= 11250 && a <= 24750)
			out->dpad |= OPENBGI_DPAD_DOWN;
		if(a >= 20250 && a <= 33750)
			out->dpad |= OPENBGI_DPAD_LEFT;
	}
	out->leftX = Axis(pos[0], min[0], max[0]);
	out->leftY = Axis(pos[1], min[1], max[1]);
	if((caps & HAS_Z) && (caps & HAS_R))
	{
		out->rightX = Axis(pos[2], min[2], max[2]);
		out->rightY = Axis(pos[3], min[3], max[3]);
	}
	else if((caps & HAS_U) && (caps & HAS_V))
	{ // (a pad whose third axis is its two triggers)
		out->rightX = Axis(pos[4], min[4], max[4]);
		out->rightY = Axis(pos[5], min[5], max[5]);
	}
}

int OsPads_PathIsXInput(const char* path)
{
	return Contains(path, "ig_");
}

// ---- the browser ----------------------------------------------------------------------------------

void OsPads_Gamepad(int standard, uint32_t buttons, int lx, int ly, int rx, int ry, OpenBGI_PadState_t* out)
{
	// the button's number in Gamepad.buttons and the engine's bit; 12 .. 15 are the D-pad
	static const uint32_t kButtons[17] = {
		OPENBGI_PAD_SOUTH, OPENBGI_PAD_EAST, OPENBGI_PAD_WEST, OPENBGI_PAD_NORTH, OPENBGI_PAD_L1, OPENBGI_PAD_R1, OPENBGI_PAD_L2, OPENBGI_PAD_R2,
		OPENBGI_PAD_SELECT, OPENBGI_PAD_START, OPENBGI_PAD_L3, OPENBGI_PAD_R3, 0, 0, 0, 0, OPENBGI_PAD_GUIDE};
	int i, v[4];
	memset(out, 0, sizeof *out);
	out->size = sizeof *out;
	if(standard)
	{
		for(i = 0; i < 17; i++)
			if((buttons >> i) & 1)
				out->buttons |= kButtons[i];
		out->dpad = (buttons >> 12) & 15; // (up, down, left, right: the engine's order)
	}
	else
		out->buttons = buttons;
	v[0] = lx;
	v[1] = ly;
	v[2] = rx;
	v[3] = ry;
	for(i = 0; i < 4; i++)
	{
		v[i] = v[i] < -1000 ? -1000 : v[i] > 1000 ? 1000
												  : v[i];
		v[i] = v[i] * 32767 / 1000;
	}
	out->leftX = (int16_t)v[0];
	out->leftY = (int16_t)v[1];
	out->rightX = (int16_t)v[2];
	out->rightY = (int16_t)v[3];
}
