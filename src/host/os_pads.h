/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_pads.h - what the systems' pad interfaces report, as the engine's pad
 *             state (OpenBGI_PadState_t of openbgi.h): Linux's event
 *             devices, XInput, the Windows joystick API and the browser's
 *             Gamepad API
 *
 * The back ends read the devices (pad.c of src/host/posix/, win32/ and
 * wasm/); the functions here only translate what was read,
 * so they are the same on every system and are tested on every system
 * (tests/hostpads.c).  The numbers of the systems' own headers they need
 * are repeated in os_pads.c.
 */
#ifndef BGI_OS_PADS_H_
#define BGI_OS_PADS_H_

#include <stdint.h>
#include "openbgi.h"

// ---- Linux: an event device (linux/input.h) -------------------------------------------------------

#define OSPADS_KEY_BYTES 96 // the bit array of the keys and buttons (KEY_CNT / 8)
#define OSPADS_ABS_COUNT 64 // the absolute axes (ABS_CNT)

typedef struct OsPadsAxis
{
	int has;      // the device has the axis
	int min, max; // its range
} OsPadsAxis_t;

typedef struct OsPadsEvdev // one device
{
	int gamepad;                        // it has a gamepad's buttons (BTN_SOUTH ..), mapped by what they are; else a joystick's, by their numbers
	int swapNorthWest;                  // its driver calls the left face button BTN_NORTH and the top one BTN_WEST (xpad)
	int zIsStick;                       // the right stick is the axes Z and RZ (there is no RX / RY); else those are the triggers
	OsPadsAxis_t abs[OSPADS_ABS_COUNT]; // by ABS_* code
	uint32_t keyButtons, axisButtons;   // the buttons down, and the triggers down by their axes
	uint32_t keyDpad, hatDpad;          // the D-pad by its buttons, and by the hat
	OpenBGI_PadState_t state;           // what the engine gets
} OsPadsEvdev_t;

/* 1 when the device with these keys (EVIOCGBIT(EV_KEY)) and axes
 * (EVIOCGBIT(EV_ABS), OSPADS_ABS_COUNT / 8 bytes) is a pad: a gamepad, or
 * a joystick with two axes. */
int OsPads_EvdevIsPad(const uint8_t* keyBits, const uint8_t* absBits);
/* A device to start with: its keys and axes as above, the ranges of its
 * axes (`abs`, by code; copied), its name and its vendor (EVIOCGNAME,
 * EVIOCGID). */
void OsPads_EvdevInit(OsPadsEvdev_t* p, const uint8_t* keyBits, const OsPadsAxis_t* abs, const char* name, int vendor);
// one event of the device (struct input_event: type, code, value); 1 when p->state changed
int OsPads_EvdevEvent(OsPadsEvdev_t* p, int type, int code, int value);
// forget what is down (before the device's state is read anew, after events were lost)
void OsPads_EvdevReset(OsPadsEvdev_t* p);

// ---- Windows --------------------------------------------------------------------------------------

/* XInput: XINPUT_GAMEPAD's wButtons, the triggers (0 .. 255) and the
 * sticks (up positive) into *out. */
void OsPads_XInput(uint32_t buttons, int leftTrigger, int rightTrigger, int lx, int ly, int rx, int ry, OpenBGI_PadState_t* out);
/* The joystick API (joyGetPosEx): the buttons by their numbers, the POV
 * hat (hundredths of a degree, 0xFFFF centred) and the positions of the
 * six axes X, Y, Z, R, U, V with their ranges (JOYCAPS) into *out; `caps`
 * is the device's wCaps (JOYCAPS_HASZ ..), which says what it has beyond
 * X and Y.  X and Y are the left stick; Z and R the right one, or U and V
 * on a device without R. */
void OsPads_Joystick(uint32_t buttons, uint32_t caps, uint32_t pov, const uint32_t* pos, const uint32_t* min, const uint32_t* max,
	OpenBGI_PadState_t* out);
/* 1 when a HID device's path (GetRawInputDeviceInfo, RIDI_DEVICENAME)
 * says that XInput serves it ("IG_" in it), so that the joystick API's
 * device of the same vendor and product is left alone. */
int OsPads_PathIsXInput(const char* path);

// ---- the browser ----------------------------------------------------------------------------------

/* A pad of the Gamepad API: `buttons` a bit per button of Gamepad.buttons
 * that is pressed (the first 32), the first four axes in thousandths
 * (-1000 .. 1000, down and right positive) into *out.  With `standard`
 * the pad has the API's "standard" mapping, and its buttons are taken by
 * what they are (12 .. 15 are the D-pad); else they keep their numbers,
 * and there is no D-pad. */
void OsPads_Gamepad(int standard, uint32_t buttons, int lx, int ly, int rx, int ry, OpenBGI_PadState_t* out);

#endif // BGI_OS_PADS_H_
