/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * settings.h - the settings over a running game: a second engine that
 *              runs the launcher's program in its settings mode, its
 *              picture a part of the first one's screen layer
 *              (src/sys/settings.c); not part of the original engine
 *
 * The settings are a screen of the launcher's program (launcher.bpc,
 * settings_run).  Over a game they are run by an engine of their own, the
 * panel, which the game's engine makes and frees: the game goes on under
 * it.
 *
 *   * The panel's host is made of the game's: its clocks and its fonts,
 *     and nothing else - no files, no window, no registry, no sound.  The
 *     OS layer keeps one text encoding and one game directory for the
 *     process, and the game has both.
 *   * Its program is the launcher's as the game's engine keeps it
 *     (Launcher_Program), started without a command line
 *     (Engine_StartBuiltin).
 *   * Its picture has the surface's size and is see-through ("EF 31").
 *     The game's engine runs a frame of it with each of its own and puts
 *     the picture into the screen layer, under the on-screen controls
 *     (overlay.c).
 *   * While it is up the touches beside the controls, the on-screen pad,
 *     the keys and the buttons are the panel's.  The game gets none.
 *   * The program reaches the game's engine through settings of the path
 *     "" ("EF 2C", "EF 2D"; Settings_Value, Settings_Set):
 *
 *       "mode"      "settings" in a panel, "launcher" in any other run
 *       "surface"   the panel's surface: its width, its height, its
 *                   pixels an inch (values 0, 1, 2)
 *       "pad"       "on" or "off": the on-screen pad, read and set
 *       "through"   "on" or "off": whether a touch beside the pad reaches
 *                   the game's picture while the pad is on, read and set
 *       "perf"      "on" or "off": the performance display (perf.h),
 *                   read and set
 *       "filters"   the filters the host scales the picture by, a list:
 *                   "game" (the one the game asks for), "bicubic",
 *                   "bilinear", "nearest", then the host's own by their
 *                   names; no value where the host does not scale
 *       "filter"    the one in use, by that name; read and set
 *       "quality"   "high" or "normal": the larger networks of a filter
 *                   of the host's own; read and set
 *       "whole"     "on" or "off": the picture only in whole steps
 *       "game"      "game" or "launcher": what the game's engine runs;
 *                   set to "close": the panel goes, and the game is
 *                   closed as by its window's close button (it asks as
 *                   it does, and the launcher follows it)
 *       "panel"     set to "close": the panel goes at the frame's end
 *
 * A host that said no surface (OpenBGI_SetSurface) has no layer and so no
 * panel: Settings_Open answers 0 there.
 */
#ifndef BGI_SETTINGS_H_
#define BGI_SETTINGS_H_

#include "bgi/common.h"
#include "openbgi.h"

typedef struct SettingsState
{
	OpenBGI_t* panel;      // the engine that runs the settings over this one; NULL: none
	OpenBGI_t* game;       // in a panel: the engine it lies over
	int close;             // the panel is to go at this frame's end ("panel": "close", or its program ended)
	int closeGame;         // .. and the game is then to be closed as by its window ("game": "close")
	char* error;           // .. or its program stopped with this script error, which the game's engine shows (Settings_Failed)
	OpenBGI_Frame_t frame; // the panel's last frame: its picture and what changed in it
	int shown;             // .. was taken into the layer (Settings_Picture)
	uint32_t openMs;       // the host's clock when the panel was made,
	int timed;             // .. and how long its first picture took was said
} SettingsState_t;

int Settings_Open(OpenBGI_t* o);   // 1 when the panel is up (it was, or it is now)
void Settings_Close(OpenBGI_t* o); // the panel goes, at once
int Settings_Up(OpenBGI_t* o);
// the game's frame: a frame of the panel, and its end when it asked for it; `f` gets the shorter wait
void Settings_Frame(OpenBGI_t* o, OpenBGI_Frame_t* f);
/* The panel's picture for the layer: w x h pixels, blue green red alpha,
 * `pitch` bytes a row, valid until the game's next frame; NULL while
 * there is none.  *changes is 0 when it is as it was handed out last, -1
 * for all of it, else the number of rectangles in `change`
 * (OPENBGI_FRAME_CHANGES at most). */
const uint8_t* Settings_Picture(OpenBGI_t* o, int* w, int* h, int* pitch, int* changes, const OpenBGI_Rect_t** change);
// the host's input while the panel is up: 1 when the panel took it
int Settings_Touch(OpenBGI_t* o, int id, int phase, int x, int y); // (the surface's pixels)
int Settings_Pad(OpenBGI_t* o, const OpenBGI_PadState_t* state);   // (the on-screen pad's)
int Settings_Key(OpenBGI_t* o, int vk, int down, int flags);
int Settings_Button(OpenBGI_t* o, int button, int down, int flags);
int Settings_Pointer(OpenBGI_t* o, int x, int y); // the mouse's place in the game's picture, for the panel; 1 while the panel is up
int Settings_OwnKey(OpenBGI_t* o, int vk, int down); // the settings key while the panel is up: it takes the panel down; 1 when it was that key
/* What a program reads and sets of the path "": Settings_Value is 0 when
 * `key` is none of these, 1 with value `i` in dst, -1 when there is no
 * such value; Settings_Set is -1 when `key` is none of these, else
 * whether it was done. */
int Settings_Value(OpenBGI_t* o, const char* key, int i, char* dst, size_t size);
int Settings_Set(OpenBGI_t* o, const char* key, const char* value);
/* In a panel's engine: its program raised a script error with this text.
 * The panel has no screen of its own for a box: the game's engine puts
 * the panel away at the frame's end, writes the text to standard error
 * and shows it in a box of its own.  The game goes on. */
void Settings_Failed(OpenBGI_t* panel, const char* text);

#endif // BGI_SETTINGS_H_
