/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * overlay.h - the screen layer and the engine's on-screen controls
 *             (src/sys/overlay.c; OpenBGI_SetSurface, OpenBGI_SurfaceTouch
 *             and OpenBGI_Frame_t.layer of openbgi.h); not part of the
 *             original engine
 *
 * A host whose picture does not fill what it draws on - a phone, with
 * bars beside the picture - says its surface.  The engine then draws a
 * layer of the surface's size over everything, which the host shows a
 * pixel a pixel, and takes the touches in the surface's pixels.
 *
 * What is in the layer: the pad's button and the settings button,
 * always, and the pad when it is on - a D-pad, a stick, and the buttons
 * Confirm, Cancel, Skip, Backlog and Forward.  Under them lies the
 * picture of the settings while those are up (settings.h): the layer is
 * that picture with the controls drawn over it, and the touches beside
 * the controls and the pad are the settings'.  Under both lies the
 * performance display's panel while that is on (perf.h), beside the
 * settings button: it takes no touch.  The pad is a pad to the rest of the engine: its state
 * goes through the mapper's own port (Pad_Port, PAD_PORT_OWN; pad.c), so
 * the D-pad moves the focus, the stick the pointer, and the bindings of
 * the settings and a game's own pad keys hold for it.  The controls are
 * placed by the edges of the surface's free part and sized in
 * millimetres; a surface too small for them has them smaller.
 *
 * A contact that goes down on a control is the control's until it lifts.
 * Any other is a touch on the picture (OpenBGI_Touch, in the picture's
 * pixels) - unless the pad is on and the settings say that only the pad
 * plays ("touchThrough": "off").
 *
 * The pad is off at first.  Its button (a small pad drawn on it) turns
 * it on and off, and the choice is kept: the setting "touchPad" of the launcher's own settings
 * in openbgi.json (Engine_CfgSet with the path ""), which the launcher's
 * program reads and sets as any of its settings.
 */
#ifndef BGI_OVERLAY_H_
#define BGI_OVERLAY_H_

#include "bgi/common.h"
#include "openbgi.h"

// the controls, in the order they are laid out and drawn
enum
{
	OVERLAY_SHOW, // the pad's button: shows and hides the rest
	OVERLAY_MENU, // the settings button: the settings come up over the game, and go (settings.h)
	OVERLAY_DPAD,
	OVERLAY_STICK,
	OVERLAY_CONFIRM,
	OVERLAY_CANCEL,
	OVERLAY_SKIP,
	OVERLAY_BACKLOG,
	OVERLAY_FORWARD,
	OVERLAY_CONTROLS
};
#define OVERLAY_TOUCHES 10 // contacts followed at once

// a control on the surface
typedef struct OverlayControl
{
	int cx, cy; // its centre, in the surface's pixels
	int r;      // half its size
	int x, y;   // the square that is drawn anew when it changes, and its side
	int side;
} OverlayControl_t;

// a contact that is down
typedef struct OverlayTouch
{
	int used;
	int id;      // the host's
	int control; // the control that has it, -1 for a touch on the picture, -2 for one that goes nowhere
} OverlayTouch_t;

typedef struct OverlayState
{
	OpenBGI_Surface_t surface; // as the host said it; a width of 0: none (kept when the engine starts again)
	int loaded;                // the settings were read for this run
	int bare;                  // no on-screen controls: the host fits a window to a screen, which is worked with a mouse and keys
	int padOn;                 // the pad is shown and plays
	int padOnly;               // while the pad is on a touch beside the controls does not reach the picture ("touchThrough": "off")
	uint8_t* pix;              // the layer: four bytes a pixel, w x h
	int w, h;
	int laid;           // the controls are placed for the surface as it is
	int scale;          // their size in 1/256 of the full one
	int handed;         // the frames had the layer handed over as it is
	int all;            // the next frame hands over all of it
	int x0, y0, x1, y1; // .. or this part, which changed (x1 <= x0: none)
	uint32_t stale;     // a bit per control that is to be drawn anew
	OverlayControl_t c[OVERLAY_CONTROLS];
	OverlayTouch_t t[OVERLAY_TOUCHES];
	uint32_t held;      // a bit per control a finger is on
	int dirs;           // the D-pad's direction under the finger (OPENBGI_DPAD_*)
	int stickX, stickY; // the stick, -32767 .. 32767
	int ppiSet;         // the picture's pixels an inch as the touch gestures were told last
	// the settings' picture under the controls, during Overlay_Frame (Settings_Picture): the layer's size; NULL for none
	const uint8_t* under;
	int underPitch;
	int underWas; // .. there was one in the frame before
	int underAll; // .. all of it is to be taken (the settings came, or went)
	// the performance display's panel (perf.h), under the settings' picture and the controls: its pixels during
	// Overlay_Frame (0x00RRGGBB; NULL for none), where it lies in the layer and its size
	const uint32_t* perf;
	int perfX, perfY, perfW, perfH;
	int perfShown; // .. there was one in the frame before
} OverlayState_t;

void Overlay_Frame(OpenBGI_t* o, OpenBGI_Frame_t* f); // the engine's frame: the layer as it is now, and what changed in it
void Overlay_Free(OpenBGI_t* o);                      // the layer's pixels go (the surface stays as it was said)
/* Whether the layer carries the on-screen controls (the settings button,
 * the pad): on a surface that is a whole screen, a phone's.  A host that
 * fits a window to its screen (OpenBGI_Window_t.workArea) is worked with
 * a mouse and keys: its layer carries the settings and the performance
 * display alone, and is there only while one of them is. */
int Overlay_Controls(OpenBGI_t* o);
int Overlay_PadOn(OpenBGI_t* o);
// the pad on or off, kept in the settings or (keep 0) for this run only
void Overlay_SetPad(OpenBGI_t* o, int on, int keep);
// whether a touch beside the controls reaches the picture while the pad is on; set, and kept in the settings
int Overlay_Through(OpenBGI_t* o);
void Overlay_SetThrough(OpenBGI_t* o, int on);
// the settings came up or went: the fingers on the picture are the other's no longer, nothing is held on the pad, and the layer is made anew
void Overlay_PanelChanged(OpenBGI_t* o);
// where a point of the surface is in the picture (OpenBGI_Surface_t.picture; the picture's pixels as the frame has them)
void Overlay_ToPicture(OpenBGI_t* o, int sx, int sy, int* px, int* py);

#endif // BGI_OVERLAY_H_
