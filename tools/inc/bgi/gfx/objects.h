/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * objects.h - the small display object classes: Effector, Filter, Group,
 * Knob and VirtualObject (src/gfx/effector.c, src/gfx/filter.c and
 * src/gfx/knob.c).
 *
 *   class order 6  Effector      full-screen geometric post-process
 *   class order 7  Filter        full-screen colour operation
 *   class order 8  VirtualObject a stand-in sorted just above another object
 *   class order 9  Group         an invisible parent for child offsets
 *   class order 10 Knob          turns another object into a slider thumb
 */
#ifndef BGI_GFX_OBJECTS_H_
#define BGI_GFX_OBJECTS_H_

#include "bgi/gfx/dispobj.h"

// --- Effector ("91 60".."91 68") -------------------------------------------
typedef struct Effector
{
	DispObj_t obj;
	int32_t mode;             // -1 none, 0 vector, 1 gradient, 2 ripple, 3 zoom
	int32_t vec1, vec2;       // mode 0: vector maps (pixel mode 4), vec2 may be -1
	int32_t vec1Gen, vec2Gen; // their generations when set
	int32_t amount;           // mode 0: passed to Blit_Displace (non-zero: bilinear)
	Bmp_t snap;               // screen-sized copy of what lies below
	int32_t gradType;         // mode 1: 0 pads with black, 1 repeats the edge
	int32_t rippleMap;        // mode 2: vector + distance map (pixel mode 6)
	int32_t rippleMapGen;
	int32_t rings;
	uint32_t* ringTable; // rings * 4 dwords, filled by the bitmap manager
	int32_t rippleNo;    // the ripple definition ("92 00")
	int32_t rippleSel;   // the phase (setParam 0x100)
	int32_t cx, cy;      // mode 3: centre, 16.16
	int32_t angle;       // 16.16 degrees
	int32_t sx, sy;      // 16.16 scale
	int32_t smooth;      // bilinear sampling
	int32_t dCx, dCy;    // deltas over the full progress (setParam 0x80..0x82)
	int32_t dAngle;
	int32_t dSx, dSy;
	int32_t curCx, curCy; // values in effect (level and progress applied)
	int32_t curAngle;
	int32_t curSx, curSy;
	int32_t curve; // Ease curve of the angle delta (setParam 0x8F)
} Effector_t;

typedef struct EffNode // the global list the compositor asks (Effector_AnyVisible)
{
	Effector_t* obj;
	struct EffNode* next;
} EffNode_t;
extern EffNode_t* gEffectors;

extern const DispObjVtbl_t Effector_Vtbl;

void Effector_Ctor(Effector_t* e, int slotId);
void Effector_Dtor(Effector_t* e);
// the mode setters: 0 ok, 0x8000000n (listed in effector.c)
int Effector_SetVector(Effector_t* e, int vec1, int vec2, int level, int amount, int prio);   // "91 65"
int Effector_SetGradient(Effector_t* e, int type, int level, int prio);                       // "91 66"
int Effector_SetRipple(Effector_t* e, int map, int rings, int rippleNo, int level, int prio); // "91 67"
int Effector_SetZoom(Effector_t* e, int32_t cx, int32_t cy, int32_t angle, int32_t sx, int32_t sy,
	int smooth, int level, int prio);                              // "91 68"
int Effector_MatchScreenSize(Effector_t* e);                       // 1 if resized
int Effector_SelectRipple(Effector_t* e, int selector, int level); // setParam 0x100; 0, 0x80000007, 0x80000008
int Effector_FreeSnapshot(Effector_t* e);                          // 1 if there was one
void Effector_FreeTable(Effector_t* e);
void Effector_RecalcZoom(Effector_t* e); // mode 3: the transform in effect
// int Effector_AnyVisible(void) is declared in compositor.h

// --- Filter ("90 60".."90 66") ---------------------------------------------
typedef struct Filter
{
	DispObj_t obj;
	int32_t useBitmap; // 1: modulated by a gray bitmap
	int32_t kind;      // 0 tint, 1 add, 2 luminance tint, 3 XOR blend (see Filter_Draw)
	uint32_t colour;   // 0x00RRGGBB
	int32_t bmp;       // gray bitmap (pixel mode 3) of screen size
	int32_t bmpParam;  // its transition curve (Blit_TintByGray)
	int32_t bmpGen;    // its generation when set
} Filter_t;

extern const DispObjVtbl_t Filter_Vtbl;

void Filter_Ctor(Filter_t* f, int slotId);
void Filter_Set(Filter_t* f, int kind, uint32_t colour, int level, int prio); // "90 65" / "90 66"
int Filter_SetBitmap(Filter_t* f, int bmp, int param);                        // "90 66"; 0 ok, 0x80000001..3 (see filter.c)
int Filter_MatchScreenSize(Filter_t* f);                                      // 1 if resized
void Filter_SetColour(Filter_t* f, uint32_t colour);

// --- Group ("90 E0".."90 E9") ----------------------------------------------
typedef struct Group
{
	DispObj_t obj;
} Group_t;
extern const DispObjVtbl_t Group_Vtbl;
void Group_Ctor(Group_t* g, int slotId); // class order 9

// --- VirtualObject (internal, no handle) ------------------------------------
typedef struct VirtualObject
{
	DispObj_t obj;
	DispObj_t* target; // the object it sorts just above
} VirtualObject_t;
extern const DispObjVtbl_t VirtualObject_Vtbl;
void VirtualObject_Ctor(VirtualObject_t* v, int slotId, DispObj_t* target);

// --- Knob ("90 D0".."90 DF") -----------------------------------------------
typedef struct Knob
{
	DispObj_t obj;
	DispObj_t* target;      // the thumb
	int32_t valX, valY;     // current value (pixels when continuous, else the step)
	int32_t draggable;      // default 1
	int32_t grabX, grabY;   // mouse offset inside the thumb at grab time, pixels
	int32_t stepsX, stepsY; // 0 = continuous, n = n positions
	Rect_t range;           // travel rectangle relative to the track origin, pixels
	int32_t nudged;         // a keyboard / wheel nudge happened since "90 DA" last asked
	int32_t nudgeX, nudgeY; // its step
	int32_t nudgeFailed;    // the range rejected it
} Knob_t;

extern const DispObjVtbl_t Knob_Vtbl;

void Knob_Ctor(Knob_t* k, int slotId, DispObj_t* target);
void Knob_Dtor(Knob_t* k);
int Knob_SetSteps(Knob_t* k, int x, int y);                         // "90 D8"; 1 ok
int Knob_SetRange(Knob_t* k, int w, int h);                         // "90 D9"; 1 ok
void Knob_SetDraggable(Knob_t* k, int f);                           // "90 DC"
void Knob_Grab(Knob_t* k, int mouseX, int mouseY);                  // the mouse took hold of the thumb
int Knob_Drag(Knob_t* k, int mouseX, int mouseY);                   // 1 if the value changed
int Knob_SetValue(Knob_t* k, int x, int y);                         // "90 D6"; 1 ok
void Knob_GetValue(Knob_t* k, int32_t out[2]);                      // "90 D7"
int Knob_Nudge(Knob_t* k, int dx, int dy);                          // move by steps; 1 ok
int Knob_TakeNudge(Knob_t* k, int32_t outDelta[2], int* outFailed); // "90 DA"; 1 if there was a nudge
int32_t Knob_StepSizeX(Knob_t* k);                                  // pixels per step, 16.16
int32_t Knob_StepSizeY(Knob_t* k);
void Knob_MaxValue(Knob_t* k, int32_t out[2]); // the largest value per axis

#endif // BGI_GFX_OBJECTS_H_
