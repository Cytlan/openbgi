/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * inputcfg.h - the input settings of openbgi.json (src/core/inputcfg.c):
 *              what a pad's controls do, a tap's delay, the switch for
 *              a game's own pad keys, the drawn pointer and the hotspots;
 *              not part of the original engine
 *
 * The file may carry an "input" object at its top level, for every game,
 * and one in a game's entry, for that game (its members win); a host may
 * lay one of its own over both (openbgi.h, OpenBGI_InputSettings):
 *
 *     "input": {
 *         "pad": { "west": "key:A", "select": "key:F1", "l2": "mouse:middle" },
 *         "ownPadKeys": false,
 *         "tapDelay": 50,
 *         "pointer": "always", "pointerSize": "large", "stickSpeed": 150,
 *         "onScreenKeyboard": false,
 *         "hotspots": [ { "name": "door", "rect": [100, 200, 80, 60], "program": "map" } ]
 *     }
 *
 * "pad" binds controls: south, east, west, north (the face buttons by
 * where they sit), l1, r1, l2, r2, l3, r3, select, start, guide, up,
 * down, left, right, and button0 .. button31 for a button by its number.
 * A control does an action (confirm, cancel, skip, backlog, forward, up,
 * down, left, right, keyboard: openbgi.h, OpenBGI_Action), holds a key
 * ("key:" and RETURN, SPACE, ESCAPE, BACK, TAB, SHIFT, CONTROL, ALT, UP,
 * DOWN, LEFT, RIGHT, PRIOR, NEXT, HOME, END, INSERT, DELETE, F1 .. F12, a
 * letter, a digit or a virtual-key code as 0x41), holds a mouse button
 * ("mouse:left", "mouse:right", "mouse:middle"), turns the wheel a notch
 * ("wheel:up" away from the player, "wheel:down") or does nothing
 * ("none").  Controls the object does not name keep what they had.
 *
 * "ownPadKeys": false makes the engine ignore the keys the game gives the
 * pad's controls itself ("81 1B").  "tapDelay" is the time in
 * milliseconds between the pointer's move and the press of a click the
 * input mapper makes (a tap, a pad's click on its focus), and between the
 * press and the release, for a game that needs the pointer to rest on a
 * button first; a pass of the scripts always runs between them.
 *
 * "pointer" says for whom the engine draws its arrow on a host that shows
 * no pointer of its own (openbgi.h, OPENBGI_CAP_POINTER): "pad", as
 * without it, while a pad's focus or stick has put the pointer where it
 * is; "always" for a mouse as well, on a host that hands a mouse over and
 * shows nothing for it; "never".  "pointerSize" is the arrow's size:
 * "small", "normal" (a thirty-sixth of the picture's height, 16 pixels at
 * least) or "large".  "stickSpeed" is how fast a stick moves the pointer,
 * in percent of the usual (25 .. 400), which is the picture's height in
 * about a second.  "onScreenKeyboard": false keeps the engine's keyboard
 * down until it is asked for (the keyboard action, a pad's confirm on a
 * text field): a text field that gets the keys, or a touch in one, does
 * not bring it up.
 *
 * "hotspots" are places a script reads the mouse at itself, where the
 * engine sees nothing to click: each a rectangle [x, y, width, height] in
 * the game's own coordinates, with a name for the reader.  They are
 * targets (OPENBGI_TARGET_HOTSPOT) while the screen has none of its own
 * that takes a click, and, with "program", only while a thread has the
 * program of that name loaded (the file's name; "._bp" may be left out).
 */
#ifndef BGI_INPUTCFG_H_
#define BGI_INPUTCFG_H_

#include "bgi/common.h"

struct JsonNode;

#define INPUT_CONTROLS       36 // buttons 0 .. 31, then up, down, left, right (PAD_CONTROLS of input.h)

/* What a control is bound to: 0 nothing, 1 .. 10 an OPENBGI_ACTION_*,
 * else one of these with its argument in the low byte */
#define INPUT_BIND_KEY       0x100 // + the virtual key: held while the control is
#define INPUT_BIND_MOUSE     0x200 // + OPENBGI_BUTTON_LEFT / _RIGHT / _MIDDLE: held while the control is
#define INPUT_BIND_WHEEL     0x300 // + 0 a notch away from the player, 1 towards
#define INPUT_BIND_KIND      0xff00

#define INPUT_HOTSPOTS_MAX   64 // hotspots kept of a list

// for whom the engine draws its arrow ("pointer"), and how large ("pointerSize")
#define INPUT_POINTER_PAD    0
#define INPUT_POINTER_ALWAYS 1
#define INPUT_POINTER_NEVER  2
#define INPUT_POINTER_SMALL  0
#define INPUT_POINTER_NORMAL 1
#define INPUT_POINTER_LARGE  2
#define INPUT_STICK_MIN      25 // "stickSpeed", percent
#define INPUT_STICK_MAX      400

typedef struct InputHotspot
{
	char name[48];    // for the reader (UTF-8)
	char program[64]; // "" always; else only while a thread has this program loaded
	int x, y, w, h;   // the game's own coordinates
} InputHotspot_t;

typedef struct InputSettings // one "input" object
{
	uint8_t bound[INPUT_CONTROLS]; // the object names the control
	uint16_t bind[INPUT_CONTROLS]; // .. and binds it to this
	int ownPadKeys;                // -1 not said, else 0 or 1
	int tapDelay;                  // milliseconds; -1 not said
	int pointer;                   // INPUT_POINTER_*; -1 not said
	int pointerSize;               // INPUT_POINTER_SMALL / _NORMAL / _LARGE; -1 not said
	int stickSpeed;                // percent; -1 not said
	int onScreenKeyboard;          // -1 not said, else 0 or 1
	int settingsKey;               // the virtual key that opens the display settings, 0 for none; -1 not said
	InputHotspot_t* hotspots;      // NULL for none
	int hotspotCount;
} InputSettings_t;

/* The settings of an "input" object; NULL when `obj` is none or says
 * nothing that was understood.  What is not understood is said on stderr
 * with `path` (the file) and skipped.  InputSettings_Free frees them (NULL
 * allowed). */
InputSettings_t* InputSettings_Parse(const struct JsonNode* obj, const char* path);
void InputSettings_Free(InputSettings_t* s);
// the object as JSON text on one line, "{...}"; free() it
char* InputSettings_Format(const InputSettings_t* s);

// a control by its name; -1 for none.  Its name ("button13" for one without a name of its own)
int InputControl_ByName(const char* name);
const char* InputControl_Name(int control, char buf[16]);
// a binding from its text ("confirm", "key:A", ...) into *bind; 0 when it is not understood.  Its text into out[n]
int InputBind_Parse(const char* text, uint16_t* bind);
void InputBind_Format(uint16_t bind, char* out, size_t n);
// a key by its name (as "pad" has them, without "key:"); 0 for none
int InputKey_ByName(const char* name);

#endif // BGI_INPUTCFG_H_
