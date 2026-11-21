/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * gfxcall.h - the cdecl forwarders onto the engine's graphics manager
 *             (src/gfx/gfxcall.c)
 *
 * The opcode handlers never touch o->gfx.gfx / o->gfx.bmpMgr directly:
 * the original routes every call through a one-line cdecl stub that loads
 * the manager pointer and calls the thiscall method, and these are those
 * stubs.  Most forward the result unchanged (see gfxmgr.h for the codes);
 * the window, knob and group stubs at the end translate the method's
 * error code into the small code the instruction reports (0 ok, positive
 * errors, -1 unknown handle) and - for an unexpected code - return their
 * last argument (an original quirk, marked "(original: returns <arg>)").
 *
 * Stubs that only tail-jump to a plain cdecl routine (the Text_Set*
 * family, Ruby_AddList, Gfx_SetBmpOption) have no counterpart here; the
 * handlers call the routine directly.
 */
#ifndef BGI_GFX_GFXCALL_H_
#define BGI_GFX_GFXCALL_H_

#include "bgi/gfx/gfxmgr.h"

// ---- plain forwarders ----------------------------------------------------
int GfxCall_Snapshot(OpenBGI_t* o, int bmpNo);                                                                              // "90 04"
int GfxCall_SnapshotPrio(OpenBGI_t* o, int bmpNo, int prio);                                                                // "90 05"
void GfxCall_SetGlobalEffect(OpenBGI_t* o, int v);                                                                          // "90 08"
void GfxCall_SetDrawLimit(OpenBGI_t* o, int prio);                                                                          // "90 09"
int GfxCall_DefineRipple(OpenBGI_t* o, int no, int period, int amplitude, int count, int rings);                            // "92 00"
int GfxCall_DefineRippleEx(OpenBGI_t* o, int no, int period, int amplitude, int fadeIn, int fadeOut, int count, int rings); // "92 01"
int GfxCall_ObjShow(OpenBGI_t* o, uint32_t h, int on);                                                                      // "90 30"
int GfxCall_ObjSetEnabled(OpenBGI_t* o, uint32_t h, int v);                                                                 // "90 31"
int GfxCall_ObjSetHidden(OpenBGI_t* o, uint32_t h, int v);                                                                  // "91 31" (1.573 on)
int GfxCall_ObjSetLevel(OpenBGI_t* o, uint32_t h, int level);                                                               // "90 32"
int GfxCall_ObjSetOpacity(OpenBGI_t* o, uint32_t h, int v);                                                                 // 1.599 on ("90 39")
int GfxCall_ObjSetPos(OpenBGI_t* o, uint32_t h, int x, int y);                                                              // "90 33"
int GfxCall_ObjSetFixedPos(OpenBGI_t* o, uint32_t h, int32_t fx, int32_t fy, int32_t fz);                                   // "91 33"
int GfxCall_ObjSetFade(OpenBGI_t* o, uint32_t h, int v);                                                                    // "90 34"
int GfxCall_ObjSetProgress(OpenBGI_t* o, uint32_t h, int v);                                                                // "90 35"
int GfxCall_ObjSetOffset(OpenBGI_t* o, uint32_t h, int dx, int dy);                                                         // "90 37"
// which 1: "91 37" (1.69 build 472 on); which 2: "91 36" (1.492 on), "92 37" (build 472)
int GfxCall_ObjSetFixedOffset(OpenBGI_t* o, uint32_t h, int which, int32_t x, int32_t y, int32_t z);
int GfxCall_ObjGetParam(OpenBGI_t* o, uint32_t h, int no, int32_t* out);                // "91 38" (1.69 build 472 on): 0 ok, 5 unsupported, 0xfe other, 0xff handle
int GfxCall_ObjSetPriority(OpenBGI_t* o, uint32_t h, int prio);                         // "90 3A" (1.69 build 472 on): 0 ok, 0xff handle
int GfxCall_ObjSetOffset2(OpenBGI_t* o, uint32_t h, int dx, int dy);                    // 1.492 on, "90 36"
int GfxCall_ObjAttach(OpenBGI_t* o, uint32_t hMaster, uint32_t hSlave, int dx, int dy); // 1.492 on, "91 3E"
int GfxCall_ObjDetach(OpenBGI_t* o, uint32_t hMaster, uint32_t hSlave);                 // "91 3F"
int GfxCall_ObjSetParam(OpenBGI_t* o, uint32_t h, int no, int a, int b);                // "90 38"
int GfxCall_ObjSetHitMask(OpenBGI_t* o, uint32_t h, int bmp);                           // "90 3C"
int GfxCall_ObjHasOwner(OpenBGI_t* o, uint32_t h);
int GfxCall_ObjBuildCache(OpenBGI_t* o, uint32_t h);                                                                                                                                                                                                   // "90 3F"
int GfxCall_BgSetBitmap(OpenBGI_t* o, int bmp);                                                                                                                                                                                                        // "90 40"
int GfxCall_BgSetFade(OpenBGI_t* o, int bmp, int second, int level);                                                                                                                                                                                   // "90 41"
int GfxCall_BgSetScroll(OpenBGI_t* o, int b0, int b1, int b2, int b3, int x, int y);                                                                                                                                                                   // "90 42"
int GfxCall_BgSetBlend(OpenBGI_t* o, int x1, int y1, int bmp1, int x2, int y2, int bmp2, int gray, int param, int level);                                                                                                                              // "90 43"
int GfxCall_BgSetFrames(OpenBGI_t* o, int n, const int* list, int level);                                                                                                                                                                              // "90 44"
int GfxCall_BgSetDisplace(OpenBGI_t* o, int bmp, int vec1, int vec2, int level, int amount);                                                                                                                                                           // "90 45"
int GfxCall_BgSetGradient(OpenBGI_t* o, int bmp, int type, int level);                                                                                                                                                                                 // "90 46"
int GfxCall_BgSetRipple(OpenBGI_t* o, int bmp, int map, int rings, int rippleNo, int level);                                                                                                                                                           // "90 47"
int GfxCall_BgSetView(OpenBGI_t* o, int bmp, int x, int y, int w, int h);                                                                                                                                                                              // "90 48"
int GfxCall_BgSetZoom(OpenBGI_t* o, int bmp, int scale, int option);                                                                                                                                                                                   // "90 49"
int GfxCall_BgSetFlip(OpenBGI_t* o, int front, int back, int style, int link, int level);                                                                                                                                                              // "90 4A"
int GfxCall_BgSetLayers(OpenBGI_t* o, int x, int y, int bmp, int32_t cx, int32_t cy, int32_t angle, int32_t sx, int32_t sy, int smooth);                                                                                                               // "91 40"
int GfxCall_BglSelect(OpenBGI_t* o, int layer);                                                                                                                                                                                                        // "91 41"
int GfxCall_BglShow(OpenBGI_t* o, int layer, int on);                                                                                                                                                                                                  // "91 42"
int GfxCall_BglSetPos(OpenBGI_t* o, int layer, int32_t x, int32_t y);                                                                                                                                                                                  // "91 43"
int GfxCall_BglSetEffect(OpenBGI_t* o, int layer, int effect);                                                                                                                                                                                         // "91 44"
int GfxCall_BglSetLevel(OpenBGI_t* o, int layer, int level);                                                                                                                                                                                           // "91 45"
int GfxCall_BglSetBitmap(OpenBGI_t* o, int layer, int bmp, int32_t cx, int32_t cy);                                                                                                                                                                    // "91 46"
int GfxCall_BglSetTransform(OpenBGI_t* o, int layer, int32_t angle, int32_t sx, int32_t sy, int smooth);                                                                                                                                               // "91 47"
int GfxCall_BglSetCurves(OpenBGI_t* o, int layer, int angleCurve, int scaleCurve);                                                                                                                                                                     // "91 48"
int GfxCall_BglSetCentreDelta(OpenBGI_t* o, int layer, int32_t dcx, int32_t dcy);                                                                                                                                                                      // "91 49"
int GfxCall_BglSetXformDelta(OpenBGI_t* o, int layer, int32_t dAngle, int32_t dsx, int32_t dsy);                                                                                                                                                       // "91 4A"
void GfxCall_BgShow(OpenBGI_t* o, int visible, int mode);                                                                                                                                                                                              // "90 4C"
int GfxCall_BgTypeId(OpenBGI_t* o);                                                                                                                                                                                                                    // "90 4D"
uint32_t GfxCall_SpriteCreate(OpenBGI_t* o);                                                                                                                                                                                                           // "90 50"
int GfxCall_SpriteDelete(OpenBGI_t* o, uint32_t h);                                                                                                                                                                                                    // "90 51"
int GfxCall_SpriteSet(OpenBGI_t* o, uint32_t h, int x, int y, int bmp, int effect, int level, int prio);                                                                                                                                               // "90 56"
int GfxCall_SpriteSetBlend(OpenBGI_t* o, uint32_t h, int x, int y, int bmp1, int bmp2, int mixRatio, int level, int prio, int levelRoute);                                                                                                             // "90 58"
int GfxCall_SpriteSetTransform(OpenBGI_t* o, uint32_t h, int x, int y, int bmp, int ox, int oy, int32_t angle, int32_t sx, int32_t sy, int smooth, int effect, int level, int prio);                                                                   // "90 59"
int GfxCall_SpriteSetWipe(OpenBGI_t* o, uint32_t h, int x, int y, int bmp, int gray, int wipeMode, int progress, int effect, int level, int prio);                                                                                                     // "90 5A"
int GfxCall_SpriteSetRipple(OpenBGI_t* o, uint32_t h, int x, int y, int bmp, int map, int rings, int rippleNo, int level, int fade, int prio);                                                                                                         // "90 5B"
int GfxCall_SpriteSetProjected(OpenBGI_t* o, uint32_t h, int32_t fx, int32_t fy, int32_t fz, int bmp1, int bmp2, int mixRatio, int levelRoute, int ox, int oy, int32_t angle, int projDist, int projPos, int smooth, int effect, int level, int prio); // "90 5C"
int GfxCall_SpriteChangeBitmap(OpenBGI_t* o, uint32_t h, int bmp);                                                                                                                                                                                     // "90 57"
int GfxCall_SpriteLinkMask(OpenBGI_t* o, uint32_t h, uint32_t h2);                                                                                                                                                                                     // 1.529 on: "91 55"
int GfxCall_SpriteSetMask(OpenBGI_t* o, uint32_t h, int bmp);                                                                                                                                                                                          // "90 55"
int GfxCall_SpriteShow(OpenBGI_t* o, uint32_t h, int on);                                                                                                                                                                                              // "90 54"
int GfxCall_SpriteUpdateRect(OpenBGI_t* o, uint32_t h, int x, int y, int w, int hgt);                                                                                                                                                                  // "90 53"
uint32_t GfxCall_FilterCreate(OpenBGI_t* o);                                                                                                                                                                                                           // "90 60"
int GfxCall_FilterDelete(OpenBGI_t* o, uint32_t h);                                                                                                                                                                                                    // "90 61"
int GfxCall_FilterSet(OpenBGI_t* o, uint32_t h, int kind, uint32_t colour, int bmp, int param, int level, int prio);                                                                                                                                   // "90 65" / "90 66"
int GfxCall_FilterShow(OpenBGI_t* o, uint32_t h, int on);                                                                                                                                                                                              // "90 64"
uint32_t GfxCall_EffectorCreate(OpenBGI_t* o);                                                                                                                                                                                                         // "91 60"
int GfxCall_EffectorDelete(OpenBGI_t* o, uint32_t h);                                                                                                                                                                                                  // "91 61"
int GfxCall_EffectorSetVector(OpenBGI_t* o, uint32_t h, int vec1, int vec2, int level, int amount, int prio);                                                                                                                                          // "91 65"
int GfxCall_EffectorSetGradient(OpenBGI_t* o, uint32_t h, int type, int level, int prio);                                                                                                                                                              // "91 66"
int GfxCall_EffectorSetRipple(OpenBGI_t* o, uint32_t h, int map, int rings, int rippleNo, int level, int prio);                                                                                                                                        // "91 67"
int GfxCall_EffectorSetZoom(OpenBGI_t* o, uint32_t h, int32_t cx, int32_t cy, int32_t angle, int32_t sx, int32_t sy, int smooth, int level, int prio);                                                                                                 // "91 68"
int GfxCall_EffectorShow(OpenBGI_t* o, uint32_t h, int on);                                                                                                                                                                                            // "91 64"
uint32_t GfxCall_MapCreate(OpenBGI_t* o);                                                                                                                                                                                                              // "90 70"
int GfxCall_MapDelete(OpenBGI_t* o, uint32_t h);                                                                                                                                                                                                       // "90 71"
int GfxCall_MapShow(OpenBGI_t* o, uint32_t h, int on);                                                                                                                                                                                                 // "90 74"
int GfxCall_MapSet(OpenBGI_t* o, uint32_t h, int x, int y, int bmp, int effect, int level, int prio);                                                                                                                                                  // "90 75"
int GfxCall_MapSetSize(OpenBGI_t* o, uint32_t h, int cols, int rows, int chipW, int chipH);                                                                                                                                                            // "90 76"
int GfxCall_MapSetTerrain(OpenBGI_t* o, uint32_t h, int w, int hgt, const uint16_t* data);                                                                                                                                                             // "90 78"
int GfxCall_MapSetView(OpenBGI_t* o, uint32_t h, int col, int row, int offX, int offY, int wrap);                                                                                                                                                      // "90 79"
int GfxCall_MapInvalidateChip(OpenBGI_t* o, uint32_t h, int tile);                                                                                                                                                                                     // "90 7A"
void GfxCall_WindowGlobal(OpenBGI_t* o, int shown, int level);                                                                                                                                                                                         // "90 0C"
int GfxCall_WindowDelete(OpenBGI_t* o, uint32_t h);                                                                                                                                                                                                    // "90 81"
int GfxCall_WindowSet(OpenBGI_t* o, uint32_t h, int x, int y, int effect, int level, int unused, int prio);                                                                                                                                            // "90 85"
int GfxCall_WindowSetPunch(OpenBGI_t* o, uint32_t h, int f);                                                                                                                                                                                           // "90 87"
int GfxCall_WindowSetLayerOrder(OpenBGI_t* o, uint32_t h, int order);                                                                                                                                                                                  // 1.492 on ("90 82"): 0 / 0x14 / 0xff
int GfxCall_WindowShow(OpenBGI_t* o, uint32_t h, int on);                                                                                                                                                                                              // "90 84"
int GfxCall_WindowShowFrame(OpenBGI_t* o, uint32_t h, int f);                                                                                                                                                                                          // "92 88"
int GfxCall_WindowShowText(OpenBGI_t* o, uint32_t h, int f);                                                                                                                                                                                           // "92 8C"
int GfxCall_WindowClear(OpenBGI_t* o, uint32_t h);                                                                                                                                                                                                     // "92 8E"
int GfxCall_WindowSetCursor(OpenBGI_t* o, uint32_t h, int x, int y);                                                                                                                                                                                   // "91 8C"
int GfxCall_WindowGetCursor(OpenBGI_t* o, int32_t out[2], uint32_t h);                                                                                                                                                                                 // "91 8D"
int GfxCall_WindowSetTable(OpenBGI_t* o, uint32_t h, const int32_t* table);                                                                                                                                                                            // "90 A7"
int GfxCall_KnobShow(OpenBGI_t* o, uint32_t h, int on);                                                                                                                                                                                                // "90 D4"
int GfxCall_KnobSetPos(OpenBGI_t* o, uint32_t h, int x, int y);                                                                                                                                                                                        // "90 D5"
int GfxCall_KnobSetValue(OpenBGI_t* o, uint32_t h, int x, int y);                                                                                                                                                                                      // "90 D6"
int GfxCall_KnobGetValue(OpenBGI_t* o, uint32_t h, int32_t out[2]);                                                                                                                                                                                    // "90 D7"
int GfxCall_KnobTakeNudge(OpenBGI_t* o, uint32_t h, int32_t* out);                                                                                                                                                                                     // "90 DA"
int GfxCall_KnobSetDraggable(OpenBGI_t* o, uint32_t h, int f);                                                                                                                                                                                         // "90 DC"
uint32_t GfxCall_GroupCreate(OpenBGI_t* o);                                                                                                                                                                                                            // "90 E0"
int GfxCall_GroupDelete(OpenBGI_t* o, uint32_t h);                                                                                                                                                                                                     // "90 E1"
int GfxCall_GroupShow(OpenBGI_t* o, uint32_t h, int on);                                                                                                                                                                                               // "90 E4"
int GfxCall_GroupSet(OpenBGI_t* o, uint32_t h, int x, int y, int level);                                                                                                                                                                               // "90 E5"

// ---- forwarders with their own result codes ------------------------------
// "90 3D": the object's hit test against the current mouse position; 0 when the object does not exist
int GfxCall_ObjHitTest(OpenBGI_t* o, int* out, uint32_t h);
int GfxCall_WindowCreate(OpenBGI_t* o, uint32_t* outHandle, int w, int h);               // "90 80": 0 / 1 no slot / 2 bad size (original: returns h)
int GfxCall_WindowSetFrame(OpenBGI_t* o, uint32_t h, int unused1, int unused2, int bmp); // "90 86": 0 / 1..3 / -1 bad handle
int GfxCall_WindowSetTextArea(OpenBGI_t* o, uint32_t h, int x, int y, int w, int hgt);   // "90 88": 0 / 1 bad area / -1
int GfxCall_WindowGetTextArea(OpenBGI_t* o, Rect_t* out, uint32_t h);                    // "90 89": 0 / -1 (original: returns h)
int GfxCall_WindowSetDrawStyle(OpenBGI_t* o, uint32_t h, int style);                     // "91 8A": 0 / 1 bad style / -1
int GfxCall_WindowSetSwingStyle(OpenBGI_t* o, uint32_t h, int style);                    // "91 8B": 0 / 1 bad style / -1
// "91 88": font by number; 0 / 1 bad size / 2 bad width / 3 the font could not be created / -1 (original: returns rubyReserve)
int GfxCall_WindowSetFont(OpenBGI_t* o, uint32_t h, int fontNo, int size, int widthPct, int bold, int proportional, int rubyReserve);
int GfxCall_WindowSetSpacing(OpenBGI_t* o, uint32_t h, int pct);        // "91 89": 0 / 1 / -1 (original: returns pct)
int GfxCall_WindowFillFrame(OpenBGI_t* o, uint32_t h, uint32_t colour); // "92 8A": 0 / -1 (original: returns colour)
/* "92 89" / "92 8D": 0 / 1 no bitmap / 2 incompatible pixel modes / 3 unknown effect / 4 nothing overlaps /
 * 5 level above 0x100 / -1 (original: returns level) */
int GfxCall_WindowDrawToFrame(OpenBGI_t* o, uint32_t h, int x, int y, int bmp, int effect, int level);
int GfxCall_WindowDrawToText(OpenBGI_t* o, uint32_t h, int x, int y, int bmp, int effect, int level);
int GfxCall_WindowCopyToBitmap(OpenBGI_t* o, int bmpNo, uint32_t h); // "90 83": 0 / 1 / -1 (original: returns h)
// "91 91" / "91 93" / "92 91": 0 / 1 nothing drawn / -1 (original: returns style)
int GfxCall_WindowDrawText(OpenBGI_t* o, uint32_t h, const char* str, int parseTags, int hang, uint32_t colour, uint32_t rubyColour,
	const int32_t style[5]);
int GfxCall_WindowCursorStatus(OpenBGI_t* o, int* out, uint32_t h);                 // "91 8E": 0 / -1 (original: returns h)
int GfxCall_RubySet(OpenBGI_t* o, const char* word, const char* reading);           // "91 94": dictionary add / remove / clear
int GfxCall_KnobCreate(OpenBGI_t* o, uint32_t* outHandle, uint32_t hTarget);        // "90 D0": 0..3 as the method (original: returns hTarget)
int GfxCall_KnobDelete(OpenBGI_t* o, uint32_t h);                                   // "90 D1"
int GfxCall_KnobSetSteps(OpenBGI_t* o, uint32_t h, int x, int y);                   // "90 D8": 0 / 4 / -1 (original: returns y)
int GfxCall_KnobSetRange(OpenBGI_t* o, uint32_t h, int w, int hgt);                 // "90 D9": 0 / 5 / -1 (original: returns hgt)
int GfxCall_GroupAdd(OpenBGI_t* o, uint32_t hGroup, uint32_t hObj, int dx, int dy); // "90 E8": 0 / 1 / 3 / 4 / -1 (original: returns dy)
int GfxCall_GroupRemove(OpenBGI_t* o, uint32_t hGroup, uint32_t hObj);              // "90 E9": 0 / 1 / 2 / -1 (original: returns hObj)

#endif // BGI_GFX_GFXCALL_H_
