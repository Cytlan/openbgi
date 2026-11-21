/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * gfxmgr.h - Gfx, the graphics manager (src/gfx/mgr/).
 *
 * The manager owns the back buffer, the compositor, the single background
 * object and fixed-size slot tables for every other object class.  Script
 * instructions never touch an object directly: an opcode handler calls a
 * cdecl wrapper (gfxcall.h), the wrapper calls a Gfx_* method here, and
 * the method resolves the handle, brackets the change with invalidate()
 * calls and re-sorts the object when its sort key may have changed.
 * Each engine has one manager, o->gfx.gfx; the display objects reach it
 * through their own alias o->gfx.dispGfx (dispobj.h).
 *
 * Handles are a class byte plus a table index (see the H_* macros);
 * handle 0 is the background.  A stale handle whose slot was reused
 * resolves to the new object, exactly as in the original.
 *
 * Result codes of the wrappers: 0 ok, 0xFF unknown handle, small positive
 * numbers for the object's own error codes (each wrapper documents its
 * mapping); the wrappers without an error path return 1 ok, 0 unknown
 * handle.  Where the original falls through an unhandled result it
 * returns one of its own arguments (usually the handle); those paths are
 * reproduced and marked "(original: returns the handle)".
 */
#ifndef BGI_GFX_GFXMGR_H_
#define BGI_GFX_GFXMGR_H_

#include "bgi/gfx/compositor.h"
#include "bgi/gfx/background.h"

// the class byte of a handle (C99 enumerators must fit an int, so macros)
#define H_SPRITE        0x80000000u
#define H_FILTER        0x90000000u
#define H_EFFECTOR      0x91000000u
#define H_MAP           0xA0000000u
#define H_WINDOW        0xB0000000u
#define H_PARTICLE      0xC0000000u
#define H_RAIN          0xC1000000u
#define H_KNOB          0xF0000000u
#define H_GROUP         0xF1000000u
#define HANDLE_INDEX(h) ((h) & 0x00FFFFFFu)

// the slots per class
#define GFX_SPRITES     256
#define GFX_FILTERS     8
#define GFX_EFFECTORS   8
#define GFX_MAPS        8
#define GFX_WINDOWS     16
#define GFX_PARTICLES   8
#define GFX_RAINS       8
#define GFX_KNOBS       32
#define GFX_GROUPS      8

struct Gfx
{
	void* hWnd;           // the main window (unused here; the host knows it)
	int32_t copyJobCount; // the scaled-present job queue: band rectangles of the scaled surface
	Rect_t* copyJobs;
	int32_t copyJobNext; // the next job to take
	WorkerPool_t* pool;  // shared worker pool
	Compositor_t* comp;
	uint8_t* backMem;         // the back buffer allocation: (h + 2) rows, one guard row above and below
	uint8_t* scaledMem;       // the scaled surface, when the window is scaled; else NULL
	Bmp_t back;               // the surface every object draws into (row 1 of backMem)
	Rect_t screen;            // {0,0,w-1,h-1}; must follow `back` (ScreenBmp)
	Bmp_t scaled;             // stretched copy presented to the window
	int32_t scaleMode;        // index into the factor table of Gfx_ScaleFactor
	Background_t* background; // never NULL
	int32_t bgVisible;        // last arguments of "90 4C", applied to a new background object
	int32_t bgMode;
	DispObj_t* sprites[GFX_SPRITES]; // the slot tables, NULL = free; count and the running slot id per class
	int32_t spriteCount, spriteNextId;
	DispObj_t* filters[GFX_FILTERS];
	int32_t filterCount, filterNextId;
	DispObj_t* effectors[GFX_EFFECTORS];
	int32_t effectorCount, effectorNextId;
	DispObj_t* maps[GFX_MAPS];
	int32_t mapCount, mapNextId;
	DispObj_t* windows[GFX_WINDOWS];
	int32_t windowCount, windowNextId;
	DispObj_t* particles[GFX_PARTICLES];
	int32_t particleCount, particleNextId;
	DispObj_t* rains[GFX_RAINS];
	int32_t rainCount, rainNextId;
	DispObj_t* knobs[GFX_KNOBS];
	int32_t knobCount, knobNextId;
	DispObj_t* groups[GFX_GROUPS];
	int32_t groupCount, groupNextId;
	/* 1.535 on: the vanishing point of the projected sprites ("90 06");
	 * (-1, -1) = the centre of the back buffer, which every earlier build
	 * uses */
	int32_t projCentre[2];
};

// ---- life cycle ------------------------------------------------------------
void Gfx_Ctor(OpenBGI_t* o, Gfx_t* g, void* hWnd, uint32_t bandPixels, WorkerPool_t* pool);
void Gfx_Dtor(OpenBGI_t* o, Gfx_t* g);
void Gfx_SetBmpMgrPtr(OpenBGI_t* o, BmpMgr_t* m);
void Gfx_Reset(OpenBGI_t* o, Gfx_t* g); // every object but the rains
void Gfx_FreeScreen(OpenBGI_t* o, Gfx_t* g);
/* the back buffer of w x (h+2) rows (one guard row above and below) in
 * pixel mode `mode`; with `scaled`, a surface of sw x sh for the stretched
 * presentation in the given scale mode */
void Gfx_SetupScreen(OpenBGI_t* o, Gfx_t* g, int w, int h, int mode, int scaled, int sw, int sh, int scaleMode);
// setupScreen + screen rect + objects match the size; 1 if a buffer exists
int Gfx_Resize(OpenBGI_t* o, Gfx_t* g, int w, int h, int mode, int scaled, int sw, int sh, int scaleMode);
void Gfx_CopyBackBmp(Gfx_t* g, Bmp_t* out); // a view of the back buffer (also in dispobj.h as Gfx_GetBackBmp)
void Gfx_GetScreenRect(Gfx_t* g, Rect_t* out);
int Gfx_GetScaledBmp(Gfx_t* g, Bmp_t* out); // 1 when there is a scaled surface

// ---- frame cycle -------------------------------------------------------------
int Gfx_RenderAll(OpenBGI_t* o, Gfx_t* g);                         // 0 without a back buffer
int Gfx_Render(OpenBGI_t* o, Gfx_t* g, int* count, Rect_t* rects); // *count = -1 everything
int Gfx_PresentAll(OpenBGI_t* o, Gfx_t* g, int x, int y);          // to the window's picture (bgi/display.h)
int Gfx_PresentAllAt0(OpenBGI_t* o, Gfx_t* g);
int Gfx_Present(OpenBGI_t* o, Gfx_t* g, const Rect_t* rects, int count);
void Gfx_UpdateScaled(OpenBGI_t* o, Gfx_t* g, int count, const Rect_t* rects); // NULL rects: everything
int Gfx_CopyJob(Gfx_t* g);                                                     // one scaled-copy job; 0 = none left
int32_t Gfx_ScaleFactor(Gfx_t* g);                                             // the vertical stretch, 16.16
void Gfx_MapRectScaled(Gfx_t* g, Rect_t* out, const Rect_t* in);
void Gfx_MapPoint(Gfx_t* g, int32_t out[2], int x, int y, int dir);                    // 0 back->window, 1 window->back
uint32_t Gfx_SplitCopyJobs(Rect_t* outOrNull, const Rect_t* r, uint32_t pixelsPerJob); // the band count; fills `out` when given
int Gfx_Snapshot(OpenBGI_t* o, Gfx_t* g, int bmpNo);                                   // "90 04"; the bitmap manager's 1 ok / 0
int Gfx_SnapshotPrio(OpenBGI_t* o, Gfx_t* g, int bmpNo, int prio);                     // "90 05"
void Gfx_SetProjCentre(Gfx_t* g, int x, int y);                                        // "90 06"
int Gfx_GetProjCentre(Gfx_t* g, int32_t out[2]);                                       // 1 when inside the back buffer
void Gfx_SetGlobalEffect(OpenBGI_t* o, Gfx_t* g, int v);                               // "90 08"
void Gfx_SetDrawLimit(Gfx_t* g, int prio);                                             // "90 09"
uint32_t Gfx_GetDrawLimit(Gfx_t* g);                                                   // priority << 20
uint32_t Gfx_GetBandPixels(Gfx_t* g);
void Gfx_InvalidateAll(Gfx_t* g);
void Gfx_AddDirty(Gfx_t* g, uint32_t key, const Rect_t* r);
int Gfx_Resort(OpenBGI_t* o, Gfx_t* g, DispObj_t* obj);

// ---- background ----------------------------------------------------------------
void Gfx_SetBgType(OpenBGI_t* o, Gfx_t* g, int type);                                      // replace the background by one of `type`
int Gfx_BgTypeId(Gfx_t* g);                                                                // "90 4D"
void Gfx_BgShow(OpenBGI_t* o, Gfx_t* g, int visible, int mode);                            // "90 4C"
int Gfx_BgSetBitmap(OpenBGI_t* o, Gfx_t* g, int bmp);                                      // "90 40" (type 1); 1 ok, 0 unusable
int Gfx_BgSetFade(OpenBGI_t* o, Gfx_t* g, int bmp, int second, int level);                 // "90 41" (type 2); 1 ok, 0 unusable
int Gfx_BgSetScroll(OpenBGI_t* o, Gfx_t* g, int b0, int b1, int b2, int b3, int x, int y); // "90 42" (type 3); 0 / 1 / 2
int Gfx_BgSetBlend(OpenBGI_t* o, Gfx_t* g, int x1, int y1, int bmp1, int x2, int y2, int bmp2,
	int gray, int param, int level);                                                               // "90 43" (type 4); 0 / 1..5 / -1
int Gfx_BgSetFrames(OpenBGI_t* o, Gfx_t* g, int n, const int* list, int level);                    // "90 44" (type 5); 0 / 1 / 2
int Gfx_BgSetDisplace(OpenBGI_t* o, Gfx_t* g, int bmp, int vec1, int vec2, int level, int amount); // "90 45" (type 6); 0 / 1..6
int Gfx_BgSetGradient(OpenBGI_t* o, Gfx_t* g, int bmp, int type, int level);                       // "90 46" (type 7); 0 / 1..3
int Gfx_BgSetRipple(OpenBGI_t* o, Gfx_t* g, int bmp, int map, int rings, int rippleNo, int level); // "90 47" (type 8); 0 / 1..7
int Gfx_BgSetView(OpenBGI_t* o, Gfx_t* g, int bmp, int x, int y, int w, int h);                    // "90 48" (type 9); 0 / 1 / 2
int Gfx_BgSetZoom(OpenBGI_t* o, Gfx_t* g, int bmp, int scale, int option);                         // "90 49" (type 10); 0 / 1 / 2
int Gfx_BgSetFlip(OpenBGI_t* o, Gfx_t* g, int front, int back, int style, int link, int level);    // "90 4A" (type 11); 0 / 1..4
int Gfx_BgSetLayers(OpenBGI_t* o, Gfx_t* g, int x, int y, int bmp, int32_t cx, int32_t cy, int32_t angle,
	int32_t sx, int32_t sy, int smooth); // "91 40" (type 12, layer 0); 0 / 3 / 4
// the layer operations: 0 ok, 1 not type 12, 2 bad layer (and the setter's own codes)
int Gfx_BglSelect(Gfx_t* g, int layer);                                                          // "91 41"
int Gfx_BglShow(Gfx_t* g, int layer, int on);                                                    // "91 42"
int Gfx_BglSetPos(Gfx_t* g, int layer, int32_t x, int32_t y);                                    // "91 43"
int Gfx_BglSetEffect(Gfx_t* g, int layer, int effect);                                           // "91 44"
int Gfx_BglSetLevel(Gfx_t* g, int layer, int level);                                             // "91 45"
int Gfx_BglSetBitmap(OpenBGI_t* o, Gfx_t* g, int layer, int bmp, int32_t cx, int32_t cy);        // "91 46"; 3 no such bitmap
int Gfx_BglSetTransform(Gfx_t* g, int layer, int32_t angle, int32_t sx, int32_t sy, int smooth); // "91 47"; 4 zero scale
int Gfx_BglSetCurves(Gfx_t* g, int layer, int angleCurve, int scaleCurve);                       // "91 48"
int Gfx_BglSetCentreDelta(Gfx_t* g, int layer, int32_t dcx, int32_t dcy);                        // "91 49"
int Gfx_BglSetXformDelta(Gfx_t* g, int layer, int32_t dAngle, int32_t dsx, int32_t dsy);         // "91 4A"

// ---- handles --------------------------------------------------------------------
DispObj_t* Gfx_FindObject(Gfx_t* g, uint32_t h); // any class; 0 = background; NULL if unknown
DispObj_t* Gfx_FindSprite(Gfx_t* g, uint32_t h); // one class each; NULL if unknown
DispObj_t* Gfx_FindFilter(Gfx_t* g, uint32_t h);
DispObj_t* Gfx_FindEffector(Gfx_t* g, uint32_t h);
DispObj_t* Gfx_FindMap(Gfx_t* g, uint32_t h);
DispObj_t* Gfx_FindWindow(Gfx_t* g, uint32_t h);
DispObj_t* Gfx_FindParticle(Gfx_t* g, uint32_t h);
DispObj_t* Gfx_FindRain(Gfx_t* g, uint32_t h);
DispObj_t* Gfx_FindKnob(Gfx_t* g, uint32_t h);
DispObj_t* Gfx_FindGroup(Gfx_t* g, uint32_t h);

// ---- generic object operations (any handle, 0 = background) ----------------------
int Gfx_ObjShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on);         // "90 30"
int Gfx_ObjSetEnabled(OpenBGI_t* o, Gfx_t* g, uint32_t h, int v);    // "90 31"
int Gfx_ObjSetHidden(OpenBGI_t* o, Gfx_t* g, uint32_t h, int v);     // "91 31" (1.573 on)
int Gfx_ObjSetLevel(OpenBGI_t* o, Gfx_t* g, uint32_t h, int level);  // "90 32"
int Gfx_ObjSetPos(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y); // "90 33"
int Gfx_ObjGetPos(OpenBGI_t* o, Gfx_t* g, int32_t out[2], uint32_t h);
int Gfx_ObjSetFixedPos(OpenBGI_t* o, Gfx_t* g, uint32_t h, int32_t fx, int32_t fy, int32_t fz); // "91 33"
// which 1: "91 37" (1.69 build 472 on); which 2: "91 36" (1.492 on), "92 37" (build 472)
int Gfx_ObjSetFixedOffset(OpenBGI_t* o, Gfx_t* g, uint32_t h, int which, int32_t x, int32_t y, int32_t z);
int Gfx_ObjSetPriority(OpenBGI_t* o, Gfx_t* g, uint32_t h, int prio);                         // "90 3A" (1.69 build 472 on); 0 / 0xff
int Gfx_ObjSetOffset2(OpenBGI_t* o, Gfx_t* g, uint32_t h, int dx, int dy);                    // 1.492 on, "90 36"
int Gfx_ObjAttach(OpenBGI_t* o, Gfx_t* g, uint32_t hMaster, uint32_t hSlave, int dx, int dy); // 1.492 on, "91 3E"
int Gfx_ObjDetach(Gfx_t* g, uint32_t hMaster, uint32_t hSlave);                               // "91 3F"
int Gfx_ObjGetParam(OpenBGI_t* o, Gfx_t* g, uint32_t h, int no, int32_t* out);                // "91 38" (1.69 build 472 on); 0 / 5 / 0xfe / 0xff
int Gfx_ObjSetFade(OpenBGI_t* o, Gfx_t* g, uint32_t h, int v);                                // "90 34"
int Gfx_ObjSetOpacity(OpenBGI_t* o, Gfx_t* g, uint32_t h, int v);                             // 1.599 on ("90 39")
int Gfx_ObjSetProgress(OpenBGI_t* o, Gfx_t* g, uint32_t h, int v);                            // "90 35"
int Gfx_ObjSetOffset(OpenBGI_t* o, Gfx_t* g, uint32_t h, int dx, int dy);                     // "90 37"
int Gfx_ObjSetParam(OpenBGI_t* o, Gfx_t* g, uint32_t h, int no, int a, int b);                // "90 38"; 0 / 5 unsupported / 0xFE rejected / 0xFF
int Gfx_ObjSetHitMask(OpenBGI_t* o, Gfx_t* g, uint32_t h, int bmp);                           // "90 3C"; bmp -1 none, -2 never hits
int Gfx_ObjHitTest(OpenBGI_t* o, Gfx_t* g, int* out, uint32_t h, int x, int y);               // "90 3D"
int Gfx_ObjHasOwner(Gfx_t* g, uint32_t h);
int Gfx_ObjBuildCache(OpenBGI_t* o, Gfx_t* g, uint32_t h); // "90 3F"; 0 / 3 / 4 / 0xFF

// ---- sprites --------------------------------------------------------------------
void Gfx_DeleteAllSprites(OpenBGI_t* o, Gfx_t* g);
uint32_t Gfx_SpriteCreate(OpenBGI_t* o, Gfx_t* g);                                                             // "90 50"; 0 when full
int Gfx_SpriteDelete(OpenBGI_t* o, Gfx_t* g, uint32_t h);                                                      // "90 51"
int Gfx_SpriteSet(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int bmp, int effect, int level, int prio); // "90 56"
int Gfx_SpriteSetBlend(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int bmp1, int bmp2, int mixRatio, int level,
	int prio, int levelRoute); // "90 58"
int Gfx_SpriteSetTransform(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int bmp, int ox, int oy, int32_t angle,
	int32_t sx, int32_t sy, int smooth, int effect, int level, int prio);                               // "90 59"
int Gfx_SpriteRescale(OpenBGI_t* o, Gfx_t* g, uint32_t h, int bmp, int32_t sx, int32_t sy, int smooth); // "90 5B" of 1.58
int Gfx_SpriteSetWipe(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int bmp, int gray, int wipeMode, int progress,
	int effect, int level, int prio); // "90 5A"
int Gfx_SpriteSetRipple(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int bmp, int map, int rings, int rippleNo,
	int level, int fade, int prio); // "90 5B"
int Gfx_SpriteSetProjected(OpenBGI_t* o, Gfx_t* g, uint32_t h, int32_t fx, int32_t fy, int32_t fz, int bmp1, int bmp2,
	int mixRatio, int levelRoute, int ox, int oy, int32_t angle, int projDist,
	int projPos, int smooth, int effect, int level, int prio);                              // "90 5C"
int Gfx_SpriteChangeBitmap(OpenBGI_t* o, Gfx_t* g, uint32_t h, int bmp);                    // "90 57"
int Gfx_SpriteSetMask(OpenBGI_t* o, Gfx_t* g, uint32_t h, int bmp);                         // "90 55"
int Gfx_SpriteLinkMask(OpenBGI_t* o, Gfx_t* g, uint32_t h, uint32_t h2);                    // 1.529 on: "91 55"
int Gfx_SpriteShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on);                             // "90 54"
int Gfx_SpriteUpdateRect(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int w, int hgt); // "90 53"

// ---- filters --------------------------------------------------------------------
void Gfx_DeleteAllFilters(OpenBGI_t* o, Gfx_t* g);
uint32_t Gfx_FilterCreate(OpenBGI_t* o, Gfx_t* g);                                                                         // "90 60"; 0 when full
int Gfx_FilterDelete(OpenBGI_t* o, Gfx_t* g, uint32_t h);                                                                  // "90 61"
int Gfx_FilterSet(OpenBGI_t* o, Gfx_t* g, uint32_t h, int kind, uint32_t colour, int bmp, int param, int level, int prio); // "90 65" / "90 66"
int Gfx_FilterShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on);                                                            // "90 64"

// ---- effectors ------------------------------------------------------------------
void Gfx_DeleteAllEffectors(OpenBGI_t* o, Gfx_t* g);
uint32_t Gfx_EffectorCreate(OpenBGI_t* o, Gfx_t* g);                                                                  // "91 60"; 0 when full
int Gfx_EffectorDelete(OpenBGI_t* o, Gfx_t* g, uint32_t h);                                                           // "91 61"
int Gfx_EffectorSetVector(OpenBGI_t* o, Gfx_t* g, uint32_t h, int vec1, int vec2, int level, int amount, int prio);   // "91 65"
int Gfx_EffectorSetGradient(OpenBGI_t* o, Gfx_t* g, uint32_t h, int type, int level, int prio);                       // "91 66"
int Gfx_EffectorSetRipple(OpenBGI_t* o, Gfx_t* g, uint32_t h, int map, int rings, int rippleNo, int level, int prio); // "91 67"
int Gfx_EffectorSetZoom(OpenBGI_t* o, Gfx_t* g, uint32_t h, int32_t cx, int32_t cy, int32_t angle, int32_t sx, int32_t sy,
	int smooth, int level, int prio);                             // "91 68"
int Gfx_EffectorShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on); // "91 64"

// ---- maps ------------------------------------------------------------------------
void Gfx_DeleteAllMaps(OpenBGI_t* o, Gfx_t* g);
uint32_t Gfx_MapCreate(OpenBGI_t* o, Gfx_t* g);                                                             // "90 70"; 0 when full
int Gfx_MapDelete(OpenBGI_t* o, Gfx_t* g, uint32_t h);                                                      // "90 71"
int Gfx_MapShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on);                                                // "90 74"
int Gfx_MapSet(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int bmp, int effect, int level, int prio); // "90 75"
int Gfx_MapSetSize(OpenBGI_t* o, Gfx_t* g, uint32_t h, int cols, int rows, int chipW, int chipH);           // "90 76"
int Gfx_MapSetTerrain(Gfx_t* g, uint32_t h, int w, int hgt, const uint16_t* data);                          // "90 78"
int Gfx_MapSetView(OpenBGI_t* o, Gfx_t* g, uint32_t h, int col, int row, int offX, int offY, int wrap);     // "90 79"
int Gfx_MapInvalidateChip(Gfx_t* g, uint32_t h, int tile);                                                  // "90 7A"

// ---- windows ---------------------------------------------------------------------
void Gfx_DeleteAllWindows(OpenBGI_t* o, Gfx_t* g);
void Gfx_WindowGlobal(OpenBGI_t* o, Gfx_t* g, int shown, int level);                                              // "90 0C"
int Gfx_WindowCreate(OpenBGI_t* o, Gfx_t* g, uint32_t* outHandle, int w, int h);                                  // "90 80"; 0 / 9 no slot / 10 bad size
int Gfx_WindowDelete(OpenBGI_t* o, Gfx_t* g, uint32_t h);                                                         // "90 81"
int Gfx_WindowSetFrame(OpenBGI_t* o, Gfx_t* g, uint32_t h, int unused1, int unused2, int bmp);                    // "90 86"
int Gfx_WindowSet(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int effect, int level, int unused, int prio); // "90 85"
int Gfx_WindowSetPunch(OpenBGI_t* o, Gfx_t* g, uint32_t h, int f);                                                // "90 87"
int Gfx_WindowSetLayerOrder(OpenBGI_t* o, Gfx_t* g, uint32_t h, int order);                                       // 1.492 on: "90 82"
int Gfx_WindowSetTextArea(Gfx_t* g, uint32_t h, int x, int y, int w, int hgt);                                    // "90 88"
int Gfx_WindowGetTextArea(Gfx_t* g, Rect_t* out, uint32_t h);                                                     // "90 89"
int Gfx_WindowSetDrawStyle(Gfx_t* g, uint32_t h, int style);                                                      // "91 8A"
int Gfx_WindowSetSwingStyle(Gfx_t* g, uint32_t h, int style);                                                     // "91 8B"
int Gfx_WindowSetFont(OpenBGI_t* o, Gfx_t* g, uint32_t h, const char* face, int size, int widthPct, int bold,
	int proportional, int rubyReserve);                                                                      // "91 88"
int Gfx_WindowSetSpacing(Gfx_t* g, uint32_t h, int pct);                                                     // "91 89"
int Gfx_WindowShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on);                                              // "90 84"
int Gfx_WindowShowFrame(OpenBGI_t* o, Gfx_t* g, uint32_t h, int f);                                          // "92 88"
int Gfx_WindowFillFrame(OpenBGI_t* o, Gfx_t* g, uint32_t h, uint32_t colour);                                // "92 8A"
int Gfx_WindowDrawToFrame(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int bmp, int effect, int level); // "92 89"
int Gfx_WindowCopyToBitmap(OpenBGI_t* o, Gfx_t* g, int bmpNo, uint32_t h);                                   // "90 83"
int Gfx_WindowShowText(OpenBGI_t* o, Gfx_t* g, uint32_t h, int f);                                           // "92 8C"
int Gfx_WindowDrawToText(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int bmp, int effect, int level);  // "92 8D"
int Gfx_WindowDrawText(OpenBGI_t* o, Gfx_t* g, uint32_t h, const char* str, int parseTags, int hang, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5]);                         // "91 91" / "91 93" / "92 91"
int Gfx_WindowClear(OpenBGI_t* o, Gfx_t* g, uint32_t h);                  // "92 8E"
int Gfx_WindowSetCursor(Gfx_t* g, uint32_t h, int x, int y);              // "91 8C"
int Gfx_WindowGetCursor(Gfx_t* g, int32_t out[2], uint32_t h);            // "91 8D"
int Gfx_WindowCursorStatus(OpenBGI_t* o, Gfx_t* g, int* out, uint32_t h); // "91 8E"
int Gfx_WindowSetTable(Gfx_t* g, uint32_t h, const int32_t* table);       // "90 A7"

// ---- particle screens -------------------------------------------------------------
void Gfx_UpdateParticles(OpenBGI_t* o, Gfx_t* g); // once per scheduler pass
void Gfx_DeleteAllParticles(OpenBGI_t* o, Gfx_t* g);
int Gfx_ParticleCreate(OpenBGI_t* o, Gfx_t* g, uint32_t* outHandle, int w, int h);                          // "C0 00"; 0 / 1 no slot / 2 bad size
int Gfx_ParticleDelete(OpenBGI_t* o, Gfx_t* g, uint32_t h);                                                 // "C0 01"
int Gfx_ParticleShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on);                                           // "C0 04"
int Gfx_ParticleSetDraw(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int effect, int level, int prio); // "C0 05"
int Gfx_ParticleRedraw(OpenBGI_t* o, Gfx_t* g, uint32_t h);                                                 // "C0 08"
int Gfx_ParticleSetRectLimit(Gfx_t* g, uint32_t h, int n);                                                  // "C0 0A"
int Gfx_ParticleSetCamera(Gfx_t* g, uint32_t h, int32_t x, int32_t y, int32_t z, int32_t a, int32_t b,
	int32_t c, int32_t dist, int cx, int cy);                                                           // "C0 0B"
int Gfx_ParticleSetInterval(Gfx_t* g, uint32_t h, int ms);                                              // "C0 0C"
int Gfx_ParticlePrerun(OpenBGI_t* o, Gfx_t* g, uint32_t h, int ms);                                     // "C0 0D"
int Gfx_ParticleClear(OpenBGI_t* o, Gfx_t* g, uint32_t h);                                              // "C0 0F"
int Gfx_ParticleSetWind(OpenBGI_t* o, Gfx_t* g, uint32_t h, const int32_t args[11]);                    // "C0 10"
int Gfx_ParticleSetGroupA(OpenBGI_t* o, Gfx_t* g, uint32_t h, int pattern, int max, int spawnEvery);    // "C0 20"
int Gfx_ParticleSetGroupB(OpenBGI_t* o, Gfx_t* g, uint32_t h, int pattern, int max, int spawnEvery);    // "C0 28"
int Gfx_ParticleSetPatternBExtra(OpenBGI_t* o, Gfx_t* g, uint32_t h, int pattern, const int32_t v[14]); // 1.492 on: "C0 29"

// ---- rain screens ----------------------------------------------------------------
void Gfx_UpdateRains(OpenBGI_t* o, Gfx_t* g); // once per scheduler pass
int Gfx_RedrawRains(OpenBGI_t* o, Gfx_t* g);  // 1 when a screen was redrawn (only the first that can be)
void Gfx_DeleteAllRains(OpenBGI_t* o, Gfx_t* g);
int Gfx_RainCreate(OpenBGI_t* o, Gfx_t* g, uint32_t* outHandle, int w, int h);                                          // "C0 40"; 0 / 1 no slot / 3 bad size
int Gfx_RainDelete(OpenBGI_t* o, Gfx_t* g, uint32_t h);                                                                 // "C0 41"
int Gfx_RainStart(OpenBGI_t* o, Gfx_t* g, uint32_t h, uint32_t prerollMs);                                              // "C0 42"
int Gfx_RainSetMask(OpenBGI_t* o, Gfx_t* g, uint32_t h, int bmp);                                                       // "C0 43"
int Gfx_RainShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on);                                                           // "C0 44"
int Gfx_RainSetDraw(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int effect, int level, int prio);                 // "C0 45"
int Gfx_RainSetArea(OpenBGI_t* o, Gfx_t* g, uint32_t h, int xMin, int yTop, int zMin, int xMax, int yBottom, int zMax); // "C0 46"
int Gfx_RainSetFall(OpenBGI_t* o, Gfx_t* g, uint32_t h, int n);                                                         // "C0 47"
int Gfx_RainSetLength(OpenBGI_t* o, Gfx_t* g, uint32_t h, int n);                                                       // "C0 48"
int Gfx_RainSetColour(OpenBGI_t* o, Gfx_t* g, uint32_t h, uint32_t argb);                                               // "C0 49"
int Gfx_RainSetPerStage(OpenBGI_t* o, Gfx_t* g, uint32_t h, int n);                                                     // "C0 4A"
int Gfx_RainSetStageMs(OpenBGI_t* o, Gfx_t* g, uint32_t h, int n);                                                      // "C0 4B"
int Gfx_RainSetCamera(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int z);                                         // "C0 4C"
int Gfx_RainSetAngles(OpenBGI_t* o, Gfx_t* g, uint32_t h, int a, int b, int c);                                         // "C0 4D"
int Gfx_RainSetDistance(OpenBGI_t* o, Gfx_t* g, uint32_t h, int n);                                                     // "C0 4E"

// ---- knobs --------------------------------------------------------------------------
void Gfx_DeleteAllKnobs(OpenBGI_t* o, Gfx_t* g);
int Gfx_KnobCreate(OpenBGI_t* o, Gfx_t* g, uint32_t* outHandle, uint32_t hTarget); // "90 D0"; 0 / 1 no slot / 2 bad target / 3 class
int Gfx_KnobDelete(OpenBGI_t* o, Gfx_t* g, uint32_t h);                            // "90 D1"
int Gfx_KnobShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on);                      // "90 D4"
int Gfx_KnobSetPos(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y);              // "90 D5"
int Gfx_KnobSetSteps(Gfx_t* g, uint32_t h, int x, int y);                          // "90 D8"
int Gfx_KnobSetRange(OpenBGI_t* o, Gfx_t* g, uint32_t h, int w, int hgt);          // "90 D9"
int Gfx_KnobSetValue(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y);            // "90 D6"
int Gfx_KnobGetValue(Gfx_t* g, uint32_t h, int32_t out[2]);                        // "90 D7"
int Gfx_KnobTakeNudge(Gfx_t* g, uint32_t h, int32_t* out);                         // "90 DA"
int Gfx_KnobSetDraggable(Gfx_t* g, uint32_t h, int f);                             // "90 DC"

// ---- groups -------------------------------------------------------------------------
void Gfx_DeleteAllGroups(OpenBGI_t* o, Gfx_t* g);
uint32_t Gfx_GroupCreate(OpenBGI_t* o, Gfx_t* g);                                         // "90 E0"; 0 when full
int Gfx_GroupDelete(OpenBGI_t* o, Gfx_t* g, uint32_t h);                                  // "90 E1"
int Gfx_GroupShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on);                            // "90 E4"
int Gfx_GroupSet(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int level);            // "90 E5"
int Gfx_GroupAdd(OpenBGI_t* o, Gfx_t* g, uint32_t hGroup, uint32_t hObj, int dx, int dy); // "90 E8"; 0 / 1 / 3 itself / 4
int Gfx_GroupRemove(Gfx_t* g, uint32_t hGroup, uint32_t hObj);                            // "90 E9"

#endif // BGI_GFX_GFXMGR_H_
