/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sprite.h - the Sprite display object (class order 2), the general-purpose
 * image object; implemented in src/gfx/sprite.c.
 *
 * The sprite has six modes, each selected by one "set" instruction together
 * with that mode's sources:
 *
 *   0 plain      one bitmap                                        "90 56"
 *   1 blend      two bitmaps of equal size cross-faded by mixRatio "90 58"
 *   2 transform  one bitmap rotated and scaled about an origin,    "90 59"
 *                animatable through the progress
 *   3 wipe       one bitmap revealed through a grayscale pattern   "90 5A"
 *                by the progress
 *   4 ripple     one bitmap distorted through a vector + distance  "90 5B"
 *                map
 *   5 projected  mode 2 placed in 3-D: depth sorted, perspective   "90 5C"
 *                scaled, optionally skewed and cross-faded with a
 *                second bitmap
 *
 * Position, effect, level, priority, visibility and the optional
 * screen-space mask are common to all modes.  obj.bmp only carries the
 * output size; the pixels always come from the bitmap manager at draw time,
 * and a bitmap that was freed or reloaded since the mode was set (its
 * generation changed) makes the sprite draw nothing.
 */
#ifndef BGI_GFX_SPRITE_H_
#define BGI_GFX_SPRITE_H_

#include "bgi/gfx/dispobj.h"

typedef struct Sprite
{
	DispObj_t obj;
	int32_t mode;    // 0..5, see above
	int32_t maskOn;  // "90 55" mask in effect
	int32_t maskBmp; // screen-sized gray bitmap, -1 = none
	int32_t maskGen; // its generation when set
	/* 1.529 on: "91 55" links two sprites: this one is masked by the picture
	 * of `maskObj` at that sprite's position, and `maskUser` is the sprite a
	 * mask object serves (one at a time) */
	struct Sprite* maskObj;
	struct Sprite* maskUser;
	int32_t bmp1;         // the bitmap of every mode; -1 = none
	int32_t bmp2;         // modes 1 and 5: the second bitmap; -1 = none
	int32_t gen1, gen2;   // their generations when set
	Bmp_t mixCache;       // mode 5 with two bitmaps: pre-mixed picture
	int32_t mixRatio;     // 0..0x100: 0 = bitmap 1, 0x100 = bitmap 2
	int32_t levelRoute;   // what setLevel drives: -1/0 level, 1 mix, 2 both
	int32_t ox, oy;       // modes 2, 5: origin inside the bitmap, 16.16
	int32_t angle;        // 16.16 degrees
	int32_t sx, sy;       // 16.16 scale (mode 5: the perspective factor)
	int32_t projDist;     // mode 5: projection distance (0: no depth scaling)
	int32_t projPos;      // mode 5: also scale the position by the perspective
	int32_t smooth;       // bilinear sampling
	int32_t curOx, curOy; // transform in effect after animation, 16.16
	int32_t curAngle;
	int32_t curSx, curSy;
	int32_t subX, subY; // mode 5: fractional screen position, 0..0xFFFF
	int32_t dOx, dOy;   // animation deltas over the full progress (setParam 0x80..0x82)
	int32_t dAngle;
	int32_t dSx, dSy;
	/* 1.494 on: setParam 0x42, a 16.16 zoom multiplied into the animated
	 * scale of modes 2 and 5; 1.0 whenever a mode is set.  One value until
	 * 1.573, a pair from 1.588 (0 for the second: the same as the first) */
	int32_t zoomX, zoomY;
	int32_t curve;            // Ease curve of the angle delta (setParam 0x8F)
	int32_t outW, outH;       // bounding box of the transformed picture, pixels
	int32_t anchorX, anchorY; // where the origin lands inside that box, pixels
	int32_t skewOn;           // setParam 0x60: the period of the skew wave in rows, 0 = off
	int32_t skewArg;          // its phase, rows
	int32_t skewAmount;       // 16.16
	Bmp_t skewCache;          // pre-skewed picture
	int32_t rippleMap;        // mode 4: vector + distance map (pixel mode 6)
	int32_t rippleMapGen;
	int32_t rings;
	uint32_t* ringTable;   // rings * 4 dwords, filled by the bitmap manager
	int32_t rippleNo;      // the ripple definition ("92 00")
	int32_t rippleSel;     // the phase (setParam 0x100)
	int32_t wipeMode;      // mode 3: argument of the wipe blitter
	Bmp_t wipeGray;        // mode 3: private copy of the pattern
	int32_t useProjCentre; // 1.535 on: the manager's sprites project from the "90 06" point (the window sub-sprites keep the screen centre)
} Sprite_t;

extern const DispObjVtbl_t Sprite_Vtbl;

void Sprite_Ctor(Sprite_t* s, int slotId);
void Sprite_Dtor(Sprite_t* s);

/* the "set" instructions: mode state, then position / effect / level /
 * priority.  Results: 0 ok, else the mode core's 0x8000000n (listed with
 * the cores in sprite.c). */
int Sprite_Set(Sprite_t* s, int x, int y, int bmp, int effect, int level, int prio);                    // "90 56"
int Sprite_SetBlendEx(Sprite_t* s, int x, int y, int bmp1, int bmp2, int mixRatio, int level, int prio, // "90 58"
	int levelRoute);
int Sprite_SetTransformEx(Sprite_t* s, int x, int y, int bmp, int ox, int oy, int32_t angle, int32_t sx, // "90 59"
	int32_t sy, int smooth, int effect, int level, int prio);
int Sprite_SetWipeEx(Sprite_t* s, int x, int y, int bmp, int gray, int wipeMode, int progress, int effect, // "90 5A"
	int level, int prio);
int Sprite_SetRippleEx(Sprite_t* s, int x, int y, int bmp, int map, int rings, int rippleNo, int level, // "90 5B"
	int fade, int prio);
int Sprite_SetProjectedEx(Sprite_t* s, int32_t fx, int32_t fy, int32_t fz, int bmp1, int bmp2, int mixRatio, // "90 5C"
	int levelRoute, int ox, int oy, int32_t angle, int projDist, int projPos,
	int smooth, int effect, int level, int prio);
int Sprite_ChangeBitmap(Sprite_t* s, int bmp); // "90 57": modes 0, 2, 5; 0 in the others

// the mode cores: 0 ok, 0x8000000n (see sprite.c)
int Sprite_SetPlain(Sprite_t* s, int bmp);
int Sprite_SetBlend(Sprite_t* s, int bmp1, int bmp2, int mixRatio, int levelRoute);
int Sprite_SetTransform(Sprite_t* s, int bmp, int ox, int oy, int32_t angle, int32_t sx, int32_t sy, int smooth);
int Sprite_SetWipe(Sprite_t* s, int bmp, int gray);
int Sprite_SetRipple(Sprite_t* s, int bmp, int map, int rings, int rippleNo, int level);
int Sprite_SetProjected(Sprite_t* s, int bmp1, int bmp2, int mixRatio, int levelRoute, int ox, int oy,
	int32_t angle, int projDist, int projPos, int smooth);
int Sprite_SetMask(Sprite_t* s, int bmp);                          // "90 55"; -1 removes it
void Sprite_SetOrigin(Sprite_t* s, int x, int y);                  // modes 2, 5 (setParam 0x40)
void Sprite_SetAngle(Sprite_t* s, int32_t angle);                  // modes 2, 5 (setParam 0x41)
void Sprite_SetSkew(Sprite_t* s, int on, int arg, int32_t amount); // mode 5 (setParam 0x60)
int Sprite_UpdateRect(Sprite_t* s, const Rect_t* r);               // "90 53"; 1 if something was dirtied
int Sprite_SelectRipple(Sprite_t* s, int selector, int level);     // mode 4 (setParam 0x100); 0, 0x80000008, 0x80000009
void Sprite_RebuildMix(Sprite_t* s);
int Sprite_FreeMix(Sprite_t* s); // 1 if there was a cache
void Sprite_RebuildSkew(Sprite_t* s);
int Sprite_FreeSkew(Sprite_t* s); // 1 if there was a cache
void Sprite_FreeRingTable(Sprite_t* s);
int Sprite_FreeWipeGray(Sprite_t* s); // 1 if there was a copy
/* the transform at the current progress: the base values plus the deltas
 * (the angle through the curve), the zoom multiplied into the scales */
void Sprite_Animate(Sprite_t* s, int32_t outCentre[2], int32_t* outAngle, int32_t* outSx, int32_t* outSy,
	int32_t ox, int32_t oy, int32_t angle, int32_t sx, int32_t sy);
void Sprite_SetZoom(Sprite_t* s, int32_t zx, int32_t zy);             // setParam 0x42 of 1.494 on
uint32_t Sprite_LinkMask(Sprite_t* s, Sprite_t* mask);                // "91 55" of 1.529 on; 0 ok, 0x8000000B..E (see sprite.c)
void Sprite_RecalcTransform(Sprite_t* s);                             // mode 2: the current transform and box
void Sprite_RecalcProjected(Sprite_t* s);                             // mode 5: the same, with the position re-projected
void Sprite_ProjectPosition(Sprite_t* s);                             // mode 5: world position -> pixel position + sub-pixel remainder
int Sprite_MapUpdateRect(Sprite_t* s, Rect_t* out, const Rect_t* in); // bitmap rectangle -> rectangle to dirty; 0 = dirty everything

/* the axis-aligned box of a w x h picture scaled by (sx, sy),
 * rotated by `angle` about (cx, cy) (16.16), widened by `skew`; (fx, fy)
 * is the 16.16 position whose fraction extends the box.  Writes the box
 * size and where the origin lands inside it. */
void Sprite_BoundsProjected(int32_t* outW, int32_t* outH, int32_t anchor[2], int w, int h, int32_t skew,
	int32_t cx, int32_t cy, int32_t angle, int32_t sx, int32_t sy,
	int32_t fx, int32_t fy);
void Sprite_BoundsTransform(int32_t* outW, int32_t* outH, int32_t anchor[2], int w, int h, int32_t skew,
	int32_t cx, int32_t cy, int32_t angle, int32_t sx, int32_t sy); // the same without a position

#endif // BGI_GFX_SPRITE_H_
