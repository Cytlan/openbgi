/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * background.h - the Background display object family (src/gfx/background/)
 *
 * A background is a screen-sized display object (class order 0, slot 0)
 * that paints the whole back buffer from one or more script bitmaps.  The
 * base class adds a type id, a "mode" (0 = paint nothing but black) and
 * three virtual methods (setMode, matchScreenSize, render); twelve
 * subclasses give the twelve background types the "90 4x" instructions
 * select:
 *
 *   1  plain bitmap                      7  bitmap through the box blur
 *   2  cross fade to bitmap/black/white  8  ripple through a vector+distance map
 *   3  2x2 tiled scrolling ("S mode")    9  stretched rectangle of a bitmap
 *   4  two positioned bitmaps + wipe    10  zoom about the centre
 *   5  frame flip book                  11  mosaic ("flip") transition
 *   6  displacement through vector maps 12  eight transformed layers
 *
 * The graphics manager owns exactly one background at a time; Gfx_SetBgType
 * (src/gfx/mgr/background.c) swaps it for another type and the Gfx_BgSet*
 * wrappers feed it the instruction's arguments.  Handle 0 of the generic
 * object instructions ("90 3x") addresses it, so setPos, setLevel and
 * setParam of the types below are what those instructions reach.
 *
 * All bitmaps are referred to by their slot number in the bitmap manager and
 * every object remembers the slot's generation so that it stops drawing (and
 * returns 0 from render, which paints black) once the script has replaced
 * the bitmap.  The special slot numbers BG_BLACK (0x7000), BG_WHITE (0x7001)
 * and BG_NONE (0x7fff) / -1 are accepted where the comments say "sentinel".
 *
 * Result codes follow the original: most setters return 0 for success and
 * 0x8000000n for the n-th check that failed; a few (types 1, 2, 3, 11) return
 * 1 for success and 0 for failure.  The manager wrappers map them onto the
 * small numbers the instructions report.
 */
#ifndef BGI_GFX_BACKGROUND_H_
#define BGI_GFX_BACKGROUND_H_

#include "bgi/gfx/dispobj.h"

typedef struct Background Background_t;

typedef struct BackgroundVtbl
{
	DispObjVtbl_t base;
	void (*setMode)(Background_t* b, int mode);                      // 0: paint black, else render
	int (*matchScreenSize)(Background_t* b);                         // 1 if resized
	int (*render)(Background_t* b, Bmp_t* dst, const Rect_t* local); // 1 drawn, 0 nothing (dst is cleared)
} BackgroundVtbl_t;

struct Background
{
	DispObj_t obj;
	int32_t typeId; // 1..12
	int32_t mode;   // 0: draw black, else render
};

// sentinel bitmap numbers
#define BG_BLACK 0x7000 // a solid black picture
#define BG_WHITE 0x7001 // a solid white picture
#define BG_NONE  0x7fff // no picture (-1 is accepted as well)

//  --- base class -----------------------------------------------
void Background_Ctor(Background_t* b, int typeId); // on zeroed memory
void Background_Dtor(Background_t* b);
void Background_Destroy(DispObj_t* o, int flags);
void Background_SetMode(Background_t* b, int mode);
void Background_Invalidate(DispObj_t* o);
void Background_Draw(DispObj_t* o, Bmp_t* dst, const Rect_t* local, uint32_t minKey);
void Background_ScreenRect(DispObj_t* o, Rect_t* out);
int Background_MatchScreenSize(Background_t* b);
int Background_NoRender(Background_t* b, Bmp_t* dst, const Rect_t* local); // always 0
int Bmp_MatchesScreen(const Bmp_t* b);                                     // w, h, mode of the back buffer
int Bg_BitmapUsable(int bmpNo);                                            // exists and matches the screen
int Bmp_IsScreenVectorMap(const Bmp_t* b);                                 // mode 4 (PM_VECTOR), screen size
int Bmp_IsScreenVecDist(const Bmp_t* b);                                   // mode 6 (PM_VECDIST), screen size

// convenience: the virtual render call
#define Background_Render(b, dst, local) \
	(((const BackgroundVtbl_t*)(b)->obj.vt)->render((b), (dst), (local)))

//  --- type 1: plain bitmap -------------------------------------
typedef struct Bg1
{
	Background_t bg;
	int32_t bmp; // the bitmap, -1 none
	int32_t gen; // its generation when it was set
} Bg1_t;
void Bg1_Ctor(Bg1_t* b);
int Bg1_Set(Bg1_t* b, int bmp); // 1 ok, 0 unusable

//  --- type 2: fade to a second bitmap --------------------------
typedef struct Bg2
{
	Background_t bg;
	int32_t bmp;       // the bitmap faded in by the level, -1 none
	int32_t second;    // bitmap behind it, or BG_BLACK / BG_WHITE
	int32_t gen;       // generation of bmp
	int32_t secondGen; // generation of second (unused for a sentinel)
} Bg2_t;
void Bg2_Ctor(Bg2_t* b);                    // effect 1
int Bg2_Set(Bg2_t* b, int bmp, int second); // 1 ok, 0 unusable

//  --- type 3: 2x2 tiled view -----------------------------------
typedef struct Bg3
{
	Background_t bg;
	int32_t bmp[4]; // top-left, top-right, bottom-left, bottom-right; -1 none
	int32_t gen[4]; // their generations
	int32_t ox, oy; // origin of the view on the 2W x 2H plane, 0..W / 0..H (pixels)
	Rect_t quad[4]; // where each bitmap sits on the plane (screen-sized quadrants)
} Bg3_t;
void Bg3_Ctor(Bg3_t* b);
int Bg3_Set(Bg3_t* b, int b0, int b1, int b2, int b3); // 1 ok, 0 unusable
int Bg3_SetOrigin(Bg3_t* b, int x, int y);             // 1 ok, 0 out of range

//  --- type 4: two positioned bitmaps ---------------------------
typedef struct Bg4
{
	Background_t bg;
	int32_t x1, y1;    // position of the upper bitmap (pixels; setPos / getPos)
	int32_t bmp1;      // upper bitmap, any size; -1 none
	int32_t gen1;      // its generation
	int32_t x2, y2;    // position of the lower bitmap (pixels)
	int32_t bmp2;      // lower bitmap or a sentinel; -1 none
	int32_t gen2;      // its generation
	int32_t gray;      // grayscale wipe map (mode 3, screen sized), -1 plain blend
	int32_t grayParam; // the threshold parameter handed to Blit_ThroughGray
	int32_t grayGen;   // generation of gray
} Bg4_t;
void Bg4_Ctor(Bg4_t* b); // effect 1
/* 0 ok, 0x80000001 bmp1 missing, 0x80000002 bmp2 missing (and not a
 * sentinel) */
int Bg4_Set(Bg4_t* b, int x1, int y1, int bmp1, int x2, int y2, int bmp2);
/* 0 ok, 0x80000003 gray missing, 0x80000004 not grayscale, 0x80000005 not
 * screen sized; gray -1 selects the plain blend */
int Bg4_SetWipe(Bg4_t* b, int gray, int param);

//  --- type 5: flip book ----------------------------------------
typedef struct Bg5Diff
{
	int32_t count; // rectangles in use
	Rect_t* rects; // up to 384 (one allocation of 0x1800 bytes), NULL when unset
} Bg5Diff_t;
typedef struct Bg5Frame
{
	int32_t bmp;        // the frame's bitmap, -1 none
	int32_t gen;        // its generation
	Bg5Diff_t diff[32]; // diff[j]: the cells of this frame that differ from frame j
} Bg5Frame_t;
typedef struct Bg5
{
	Background_t bg;
	int32_t shown;        // frame on screen, -1 none
	int32_t count;        // frames in use, 2..32 (0 until Bg5_Set)
	Bg5Frame_t frame[32]; // the frames; the level selects one
} Bg5_t;
void Bg5_Ctor(Bg5_t* b);
int Bg5_Set(Bg5_t* b, int n, const int* bmpList); // 0 ok, 0x80000001 bad n, 0x80000002 a bitmap unusable
void Bg5_Clear(Bg5_t* b);
void Bg5_CellSize(int* cw, int* ch); // the comparison cell (pixels): the screen in 32 x 24 cells
int Bg5_DiffFrames(Bg5_t* b, Bg5Diff_t* out, int bmpA, int bmpB);

//  --- type 6: displacement -------------------------------------
typedef struct Bg6
{
	Background_t bg;
	int32_t bmp;             // the bitmap, -1 none
	int32_t vec1;            // vector map (mode 4, screen sized), -1 none
	int32_t vec2;            // optional second vector map, -1 none
	int32_t gen, gen1, gen2; // generations of bmp, vec1, vec2
	int32_t amount;          // passed through to Blit_Displace: non-zero samples bilinearly
} Bg6_t;
void Bg6_Ctor(Bg6_t* b);
int Bg6_Set(Bg6_t* b, int bmp, int vec1, int vec2); // 0 ok, 0x80000001 .. 0x80000006 (see displace.c)
void Bg6_SetAmount(Bg6_t* b, int amount);

//  --- type 7: gradient (box blur) ------------------------------
typedef struct Bg7
{
	Background_t bg;
	int32_t bmp;  // the bitmap, -1 none
	int32_t gen;  // its generation
	int32_t type; // Blit_Gradient type: 0 pads with black, 1 repeats the edge
} Bg7_t;
void Bg7_Ctor(Bg7_t* b);
int Bg7_SetBitmap(Bg7_t* b, int bmp); // 0 ok, 0x80000001 missing, 0x80000002 not screen sized
int Bg7_SetType(Bg7_t* b, int type);  // 0 ok, 0x80000003 type not 0 or 1

//  --- type 8: ripple -------------------------------------------
typedef struct Bg8
{
	Background_t bg;
	int32_t bmp;         // the bitmap, -1 none
	int32_t vecdist;     // vector + distance map (mode 6, screen sized), -1 none
	int32_t gen, vecGen; // generations of bmp and vecdist
	int32_t rings;       // rings of the ripple (> 0)
	uint32_t* table;     // the ring table, rings * 4 dwords, refilled on every setLevel
	int32_t rippleNo;    // the ripple definition ("92 00" / "92 01") in the bitmap manager
	int32_t selector;    // the phase selector of the definition (setParam 0x100)
} Bg8_t;
void Bg8_Ctor(Bg8_t* b);
void Bg8_Dtor(Bg8_t* b);
int Bg8_SetBitmap(Bg8_t* b, int bmp); // 0 ok, 0x80000001 missing, 0x80000002 not screen sized
/* 0 ok, 0x80000003 map missing, 0x80000004 not a screen-sized vecdist map,
 * 0x80000005 zero rings, 0x80000006 unknown ripple definition, 0x80000007
 * definition too short for `rings` */
int Bg8_SetRipple(Bg8_t* b, int vecdist, int rings, int rippleNo, int level);
int Bg8_SelectRipple(Bg8_t* b, int selector, int level); // 0 ok, 0x80000006 / 0x80000007 as above
void Bg8_FreeTable(Bg8_t* b);

//  --- type 9: stretched view -----------------------------------
typedef struct Bg9
{
	Background_t bg;
	int32_t bmp;                // the bitmap, any size; -1 none
	int32_t gen;                // its generation
	int32_t unusedA8;           // never read
	int32_t x, y;               // 16.16 view origin at level 0 (setPos / getPos)
	int32_t viewW, viewH;       // view size at level 0 (pixels, >= 2)
	int32_t unusedBC, unusedC0; // never read
	int32_t tx, ty;             // target origin (pixels) at level 0x100 (setParam 0x102)
	int32_t tw, th;             // target size (pixels, >= 2)
} Bg9_t;
void Bg9_Ctor(Bg9_t* b);
int Bg9_SetBitmap(Bg9_t* b, int bmp);                    // 0 ok, 0x80000001 missing (shared with type 10)
int Bg9_SetViewSize(Bg9_t* b, int w, int h);             // 0 ok, 0x80000002 a side below 2
int Bg9_SetTarget(Bg9_t* b, int x, int y, int w, int h); // 0 ok, 0x80000002 a side below 2

//  --- type 10: zoom --------------------------------------------
typedef struct Bg10
{
	Background_t bg;
	int32_t bmp;         // the bitmap, -1 none
	int32_t gen;         // its generation
	int32_t scale;       // zoom factor, 16.16, > 0
	int32_t option;      // rotation angle (16.16 degrees)
	int32_t scaleDelta;  // zoom change over the level (16.16), eased by a quarter sine
	int32_t deltaOption; // angle change over the level (16.16 degrees), linear
} Bg10_t;
void Bg10_Ctor(Bg10_t* b);
int Bg10_SetBitmap(Bg10_t* b, int bmp);                   // = Bg9_SetBitmap
int Bg10_SetScale(Bg10_t* b, int scale, int option);      // 0 ok, 0x80000002 zero scale; clears the deltas
int Bg10_SetScaleDelta(Bg10_t* b, int delta, int option); // 0 ok, 0x80000002 scale + delta is zero

//  --- type 11: mosaic transition -------------------------------
typedef struct Bg11
{
	Background_t bg;
	int32_t front;    // the bitmap mosaiced by the level, -1 none
	int32_t frontGen; // its generation
	int32_t back;     // bitmap behind it or BG_BLACK / BG_WHITE, -1 none
	int32_t backGen;  // its generation (unused for a sentinel)
	int32_t style;    // only 0 is accepted
	int32_t link;     // 0 or 1: also mosaic the back bitmap (by 0x100 - level)
} Bg11_t;
void Bg11_Ctor(Bg11_t* b);
int Bg11_Set(Bg11_t* b, int front, int back); // 0 ok, 0x80000001 front unusable, 0x80000002 back unusable
int Bg11_SetStyle(Bg11_t* b, int style);      // 1 ok, 0 style not 0
int Bg11_SetLink(Bg11_t* b, int link);        // 1 ok, 0 link not 0 or 1

//  --- type 12: transformed layers ------------------------------
typedef struct Bg12Layer
{
	int32_t hasBitmap;  // a bitmap was set (Bg12_LayerSetBitmap)
	int32_t visible;    // drawn when set
	int32_t x, y;       // 16.16 screen position of the centre
	int32_t effect;     // Bmp_BlitEffect effect of layers 1..7 (layer 0 is copied)
	int32_t level;      // 0..0x100, handed to the blit (0 shows the bitmap as it is)
	int32_t bmp;        // the bitmap, any size
	int32_t gen;        // its generation
	int32_t cx, cy;     // 16.16 centre inside the bitmap
	int32_t angle;      // 16.16 degrees
	int32_t sx, sy;     // 16.16 scale (non-zero)
	int32_t angleCurve; // Ease curve of the angle delta
	int32_t scaleCurve; // Ease curve of the scale delta
	int32_t dcx, dcy;   // centre delta (16.16) reached at progress 0x100
	int32_t dAngle;     // angle delta (16.16 degrees) reached at progress 0x100, through angleCurve
	int32_t dsx, dsy;   // scale delta (16.16) reached at progress 0x100, through scaleCurve
	int32_t smooth;     // bilinear sampling
} Bg12Layer_t;
typedef struct Bg12
{
	Background_t bg;
	int32_t current;      // layer addressed by setPos/getPos/setLevel/getLevel/setParam
	Bg12Layer_t layer[8]; // layer 0 is the base picture, 1..7 are blended over it
} Bg12_t;
// every layer operation: 0 ok, 0x80000001 for a layer index outside 0..7
void Bg12_Ctor(Bg12_t* b);
int Bg12_LayerShow(Bg12_t* b, int i, int on);
int Bg12_LayerIsShown(Bg12_t* b, int* out, int i);
int Bg12_LayerSetPos(Bg12_t* b, int i, int32_t x, int32_t y);
int Bg12_LayerGetPos(Bg12_t* b, int32_t out[2], int i);
int Bg12_LayerSetEffect(Bg12_t* b, int i, int effect);
int Bg12_LayerSetLevel(Bg12_t* b, int i, int level);
int Bg12_LayerGetLevel(Bg12_t* b, int* out, int i);
int Bg12_LayerSetBitmap(Bg12_t* b, int i, int bmp, int32_t cx, int32_t cy);                      // bmp -1 removes it; 0x80000002 missing
int Bg12_LayerSetTransform(Bg12_t* b, int i, int32_t angle, int32_t sx, int32_t sy, int smooth); // 0x80000003 zero scale
int Bg12_LayerSetCurves(Bg12_t* b, int i, int angleCurve, int scaleCurve);
int Bg12_LayerSetCentreDelta(Bg12_t* b, int i, int32_t dcx, int32_t dcy);
int Bg12_LayerSetAngleDelta(Bg12_t* b, int i, int32_t d);
int Bg12_LayerSetScaleDelta(Bg12_t* b, int i, int32_t dsx, int32_t dsy);
int Bg12_Select(Bg12_t* b, int i); // the layer the generic object operations address

// the vtables, for anyone who needs to recognise a class
extern const BackgroundVtbl_t Background_Vtbl, Bg1_Vtbl, Bg2_Vtbl, Bg3_Vtbl, Bg4_Vtbl, Bg5_Vtbl, Bg6_Vtbl,
	Bg7_Vtbl, Bg8_Vtbl, Bg9_Vtbl, Bg10_Vtbl, Bg11_Vtbl, Bg12_Vtbl;

/* allocate (zero filled) and construct the background of `type` 1..12;
 * NULL for an unknown type.  Destroy it through obj.vt->destroy(o, 1). */
Background_t* Background_Create(int type);

#endif // BGI_GFX_BACKGROUND_H_
