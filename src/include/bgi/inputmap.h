/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * inputmap.h - the input mapper (src/sys/inputmap.c): what a click
 *              reaches
 *
 * OpenBGI_Targets (openbgi.h) asks everything that takes the mouse for
 * what it takes it on: the waits of the threads, the panels, the knobs
 * and the sprites registered as click targets; it adds the layers the
 * scripts pushed themselves ("80 18") and what the scripts test the
 * pointer with (input.h, Input_NoteProbe).  An owner reports each target
 * with TargetSink_Add: what it is, its rectangle in back-buffer
 * coordinates, the input layer it is hit through and a function that
 * tells whether a click at a point would reach it.  An owner that pushed
 * a mouse layer covering everything claims it with TargetSink_Claim,
 * whether it reports a target for it or not, so that the layers nobody
 * claims are the scripts' own.  Nothing of this changes the engine's
 * state: the reports and the reach functions only read it.
 */
#ifndef BGI_INPUTMAP_H_
#define BGI_INPUTMAP_H_

#include "bgi/common.h"
#include "openbgi.h"
#include "bgi/gfx/bitmap.h"

typedef struct TargetSink TargetSink_t;
typedef struct TargetDesc TargetDesc_t;
struct Wait;
struct ButtonPanel;

/* would a click at back-buffer position (x, y) reach target d?  Only
 * reads the engine's state */
typedef int (*TargetReach_t)(OpenBGI_t* o, const TargetDesc_t* d, int x, int y);

struct TargetDesc // one target, as its owner reports it
{
	int kind;            // OPENBGI_TARGET_* (openbgi.h)
	uint32_t flags;      // what the owner knows: OPENBGI_TARGET_DISABLED, _SELECTED, _HELD, _ANYWHERE, _PROBED
	int whole;           // the target is all of the picture (rect is not used)
	Rect_t rect;         // where it is: back-buffer coordinates, inclusive
	int hasDrag;         // drag is set (a knob's travel)
	Rect_t drag;         // back-buffer coordinates, inclusive
	uint32_t owner;      // OpenBGI_Target_t.owner
	int group, index;    // OpenBGI_Target_t.group, .index
	uint32_t layer;      // the mouse layer key it is hit through
	TargetReach_t reach; // NULL: never
	const void* ctx;     // the owner's object, for `reach`
	int part;            // which of its parts, for `reach`
};

// a descriptor of `kind` hit through `layer`: no flags, no rectangle, owner 0, group and index -1
void TargetDesc_Init(TargetDesc_t* d, int kind, uint32_t layer);
void TargetSink_Add(TargetSink_t* s, const TargetDesc_t* d); // the sink keeps a copy
void TargetSink_Claim(TargetSink_t* s, uint32_t key);        // a layer of that key covering everything is the reporter's
// the reach of a target that a click anywhere on its layer reaches: the layer d->layer is hit at (x, y)
int Target_ReachLayer(OpenBGI_t* o, const TargetDesc_t* d, int x, int y);

// ---- where a touch goes (src/sys/touch.c) -----------------------------------------------------
/* Where a tap at the picture's pixel (x, y) clicks, into out[]: on the
 * target under it (one a click anywhere reaches only when no other is
 * there and none lies within `radius` pixels), else at the click point of
 * the nearest target within `radius` that takes a click now.  *kind and
 * *owner say which target (0, 0 for none).  1 at (x, y), 2 snapped, 0
 * nothing there (out[] is (x, y)). */
int InputMap_TapPoint(OpenBGI_t* o, int x, int y, int radius, int out[2], int* kind, uint32_t* owner);
/* Where a drag from (x, y) takes hold of a knob: (x, y) on a thumb, else,
 * when no other target is under it, the click point of the nearest thumb
 * within `radius`; 0 for none. */
int InputMap_KnobPoint(OpenBGI_t* o, int x, int y, int radius, int out[2]);

/* OpenBGI_Targets (openbgi.h) for the engine's own use: out[] is the
 * engine's, of the engine's size. */
int InputMap_Targets(OpenBGI_t* o, OpenBGI_Target_t* out, int max);

// ---- where a pad goes (src/sys/pad.c) ---------------------------------------------------------
/* The targets a pad's directions move between, in the order of
 * OpenBGI_Targets, into out[] (at most `max`; the number written): those
 * with a place of their own that take a click now and have a point where
 * one reaches them.  A panel's own area (PANEL) is none, and of several
 * with the same rectangle only the first is. */
int InputMap_Focusables(OpenBGI_t* o, OpenBGI_Target_t* out, int max);
/* Where a click makes the screen go on, into at[]: the first target that
 * a click anywhere reaches (TEXT, WAIT, LAYER, RAW) and that takes one
 * now - at the pointer when a click there is its alone, else at its click
 * point (for a script that reads the buttons itself, RAW, a point of the
 * picture that lies on no other target).  0 when the screen has none. */
int InputMap_AdvancePoint(OpenBGI_t* o, int at[2]);

// ---- the owners' reports ----------------------------------------------------------------------
// a thread's wait (wait.c): the class's file reports it; each file answers 1 for its own classes
void Wait_Targets(OpenBGI_t* o, struct Wait* w, TargetSink_t* s);
int WaitSys_Targets(OpenBGI_t* o, struct Wait* w, TargetSink_t* s);
int WaitTween_Targets(OpenBGI_t* o, struct Wait* w, TargetSink_t* s);
int WaitIcon_Targets(OpenBGI_t* o, struct Wait* w, TargetSink_t* s);
int WaitMenu_Targets(OpenBGI_t* o, struct Wait* w, TargetSink_t* s);
int WaitText_Targets(OpenBGI_t* o, struct Wait* w, TargetSink_t* s);
int WaitQuake_Targets(OpenBGI_t* o, struct Wait* w, TargetSink_t* s);
void Panel_TargetsAll(OpenBGI_t* o, TargetSink_t* s);                           // objproc.c: every panel
void ButtonPanel_Targets(OpenBGI_t* o, struct ButtonPanel* p, TargetSink_t* s); // panel.c: one panel, a sprite panel too
void Knob_Targets(OpenBGI_t* o, TargetSink_t* s);                               // knobreg.c: the registered knobs
void Target_Targets(OpenBGI_t* o, TargetSink_t* s);                             // sysobj/targets.c: the registered sprites

#endif // BGI_INPUTMAP_H_
