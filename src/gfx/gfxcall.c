/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * gfxcall.c - the cdecl forwarders onto the engine's graphics manager
 *             (src/include/bgi/gfx/gfxcall.h)
 *
 * Every GfxCall_* function calls the Gfx_* method of the same name on
 * o->gfx.gfx (or the bitmap manager o->gfx.bmpMgr) and hands the
 * arguments through.  The plain forwarders return the method's result
 * unchanged; the ones at the end translate the method's code into the
 * small code the instruction reports (0 ok, positive errors, -1 unknown
 * handle) and, for a code they do not expect, return their last argument,
 * as the original does.
 */
#include "bgi/gfx/gfxcall.h"
#include "bgi/gfx.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/text.h"
#include "bgi/input.h"
#include "bgi/core.h"

// ---- manager-wide operations ------------------------------------------------

// every object but the rain screens, then every bitmap slot
void Gfx_ResetAll(OpenBGI_t* o)
{
	Gfx_Reset(o, o->gfx.gfx);
	BmpMgr_FreeAll(o->gfx.bmpMgr);
}

// "90 0B": when set, a re-created bitmap slot keeps its generation number (objects keep showing it)
void BmpMgr_SetOption(OpenBGI_t* o, int v)
{
	o->gfx.bmpMgr->keepGen = v;
}

// ========================================================================
// plain forwarders
// ========================================================================

int GfxCall_Snapshot(OpenBGI_t* o, int bmpNo)
{
	Present_Freshen(o); // (painting may be off: the copy is of what the objects show)
	return Gfx_Snapshot(o, o->gfx.gfx, bmpNo);
}

int GfxCall_SnapshotPrio(OpenBGI_t* o, int bmpNo, int prio)
{
	return Gfx_SnapshotPrio(o, o->gfx.gfx, bmpNo, prio);
}

void GfxCall_SetGlobalEffect(OpenBGI_t* o, int v)
{
	Gfx_SetGlobalEffect(o, o->gfx.gfx, v);
}

void GfxCall_SetDrawLimit(OpenBGI_t* o, int prio)
{
	Gfx_SetDrawLimit(o->gfx.gfx, prio);
}

// "92 00" / "92 01": the ripple definitions live in the bitmap manager
int GfxCall_DefineRipple(OpenBGI_t* o, int no, int period, int amplitude, int count, int rings)
{
	return BmpMgr_DefineRipple(o->gfx.bmpMgr, no, period, amplitude, count, rings);
}

int GfxCall_DefineRippleEx(OpenBGI_t* o, int no, int period, int amplitude, int fadeIn, int fadeOut, int count, int rings)
{
	return BmpMgr_DefineRippleEx(o->gfx.bmpMgr, no, period, amplitude, fadeIn, fadeOut, count, rings);
}

int GfxCall_ObjShow(OpenBGI_t* o, uint32_t h, int on)
{
	return Gfx_ObjShow(o, o->gfx.gfx, h, on);
}

int GfxCall_ObjSetEnabled(OpenBGI_t* o, uint32_t h, int v)
{
	return Gfx_ObjSetEnabled(o, o->gfx.gfx, h, v);
}

int GfxCall_ObjSetHidden(OpenBGI_t* o, uint32_t h, int v)
{
	return Gfx_ObjSetHidden(o, o->gfx.gfx, h, v);
}

int GfxCall_ObjSetLevel(OpenBGI_t* o, uint32_t h, int level)
{
	return Gfx_ObjSetLevel(o, o->gfx.gfx, h, level);
}

int GfxCall_ObjSetOpacity(OpenBGI_t* o, uint32_t h, int v) // 1.599 on
{
	return Gfx_ObjSetOpacity(o, o->gfx.gfx, h, v);
}

int GfxCall_ObjSetPos(OpenBGI_t* o, uint32_t h, int x, int y)
{
	return Gfx_ObjSetPos(o, o->gfx.gfx, h, x, y);
}

int GfxCall_ObjSetFixedPos(OpenBGI_t* o, uint32_t h, int32_t fx, int32_t fy, int32_t fz)
{
	return Gfx_ObjSetFixedPos(o, o->gfx.gfx, h, fx, fy, fz);
}

int GfxCall_ObjSetFade(OpenBGI_t* o, uint32_t h, int v)
{
	return Gfx_ObjSetFade(o, o->gfx.gfx, h, v);
}

int GfxCall_ObjSetProgress(OpenBGI_t* o, uint32_t h, int v)
{
	return Gfx_ObjSetProgress(o, o->gfx.gfx, h, v);
}

int GfxCall_ObjSetOffset(OpenBGI_t* o, uint32_t h, int dx, int dy)
{
	return Gfx_ObjSetOffset(o, o->gfx.gfx, h, dx, dy);
}

// 1.494 / 1.69 build 472 on
int GfxCall_ObjSetFixedOffset(OpenBGI_t* o, uint32_t h, int which, int32_t x, int32_t y, int32_t z)
{
	return Gfx_ObjSetFixedOffset(o, o->gfx.gfx, h, which, x, y, z);
}

int GfxCall_ObjGetParam(OpenBGI_t* o, uint32_t h, int no, int32_t* out)
{
	return Gfx_ObjGetParam(o, o->gfx.gfx, h, no, out);
}

int GfxCall_ObjSetOffset2(OpenBGI_t* o, uint32_t h, int dx, int dy) // 1.492 on
{
	return Gfx_ObjSetOffset2(o, o->gfx.gfx, h, dx, dy);
}

int GfxCall_ObjAttach(OpenBGI_t* o, uint32_t hMaster, uint32_t hSlave, int dx, int dy) // 1.492 on
{
	return Gfx_ObjAttach(o, o->gfx.gfx, hMaster, hSlave, dx, dy);
}

int GfxCall_ObjDetach(OpenBGI_t* o, uint32_t hMaster, uint32_t hSlave)
{
	return Gfx_ObjDetach(o->gfx.gfx, hMaster, hSlave);
}

int GfxCall_ObjSetPriority(OpenBGI_t* o, uint32_t h, int prio)
{
	return Gfx_ObjSetPriority(o, o->gfx.gfx, h, prio);
}

int GfxCall_ObjSetParam(OpenBGI_t* o, uint32_t h, int no, int a, int b)
{
	return Gfx_ObjSetParam(o, o->gfx.gfx, h, no, a, b);
}

int GfxCall_ObjSetHitMask(OpenBGI_t* o, uint32_t h, int bmp)
{
	return Gfx_ObjSetHitMask(o, o->gfx.gfx, h, bmp);
}

int GfxCall_ObjHasOwner(OpenBGI_t* o, uint32_t h)
{
	return Gfx_ObjHasOwner(o->gfx.gfx, h);
}

int GfxCall_ObjBuildCache(OpenBGI_t* o, uint32_t h)
{
	return Gfx_ObjBuildCache(o, o->gfx.gfx, h);
}

int GfxCall_BgSetBitmap(OpenBGI_t* o, int bmp)
{
	return Gfx_BgSetBitmap(o, o->gfx.gfx, bmp);
}

int GfxCall_BgSetFade(OpenBGI_t* o, int bmp, int second, int level)
{
	return Gfx_BgSetFade(o, o->gfx.gfx, bmp, second, level);
}

int GfxCall_BgSetScroll(OpenBGI_t* o, int b0, int b1, int b2, int b3, int x, int y)
{
	return Gfx_BgSetScroll(o, o->gfx.gfx, b0, b1, b2, b3, x, y);
}

int GfxCall_BgSetBlend(OpenBGI_t* o, int x1, int y1, int bmp1, int x2, int y2, int bmp2, int gray, int param, int level)
{
	return Gfx_BgSetBlend(o, o->gfx.gfx, x1, y1, bmp1, x2, y2, bmp2, gray, param, level);
}

int GfxCall_BgSetFrames(OpenBGI_t* o, int n, const int* list, int level)
{
	return Gfx_BgSetFrames(o, o->gfx.gfx, n, list, level);
}

int GfxCall_BgSetDisplace(OpenBGI_t* o, int bmp, int vec1, int vec2, int level, int amount)
{
	return Gfx_BgSetDisplace(o, o->gfx.gfx, bmp, vec1, vec2, level, amount);
}

int GfxCall_BgSetGradient(OpenBGI_t* o, int bmp, int type, int level)
{
	return Gfx_BgSetGradient(o, o->gfx.gfx, bmp, type, level);
}

int GfxCall_BgSetRipple(OpenBGI_t* o, int bmp, int map, int rings, int rippleNo, int level)
{
	return Gfx_BgSetRipple(o, o->gfx.gfx, bmp, map, rings, rippleNo, level);
}

int GfxCall_BgSetView(OpenBGI_t* o, int bmp, int x, int y, int w, int h)
{
	return Gfx_BgSetView(o, o->gfx.gfx, bmp, x, y, w, h);
}

int GfxCall_BgSetZoom(OpenBGI_t* o, int bmp, int scale, int option)
{
	return Gfx_BgSetZoom(o, o->gfx.gfx, bmp, scale, option);
}

int GfxCall_BgSetFlip(OpenBGI_t* o, int front, int back, int style, int link, int level)
{
	return Gfx_BgSetFlip(o, o->gfx.gfx, front, back, style, link, level);
}

int GfxCall_BgSetLayers(OpenBGI_t* o, int x, int y, int bmp, int32_t cx, int32_t cy, int32_t angle, int32_t sx, int32_t sy, int smooth)
{
	return Gfx_BgSetLayers(o, o->gfx.gfx, x, y, bmp, cx, cy, angle, sx, sy, smooth);
}

int GfxCall_BglSelect(OpenBGI_t* o, int layer)
{
	return Gfx_BglSelect(o->gfx.gfx, layer);
}

int GfxCall_BglShow(OpenBGI_t* o, int layer, int on)
{
	return Gfx_BglShow(o->gfx.gfx, layer, on);
}

int GfxCall_BglSetPos(OpenBGI_t* o, int layer, int32_t x, int32_t y)
{
	return Gfx_BglSetPos(o->gfx.gfx, layer, x, y);
}

int GfxCall_BglSetEffect(OpenBGI_t* o, int layer, int effect)
{
	return Gfx_BglSetEffect(o->gfx.gfx, layer, effect);
}

int GfxCall_BglSetLevel(OpenBGI_t* o, int layer, int level)
{
	return Gfx_BglSetLevel(o->gfx.gfx, layer, level);
}

int GfxCall_BglSetBitmap(OpenBGI_t* o, int layer, int bmp, int32_t cx, int32_t cy)
{
	return Gfx_BglSetBitmap(o, o->gfx.gfx, layer, bmp, cx, cy);
}

int GfxCall_BglSetTransform(OpenBGI_t* o, int layer, int32_t angle, int32_t sx, int32_t sy, int smooth)
{
	return Gfx_BglSetTransform(o->gfx.gfx, layer, angle, sx, sy, smooth);
}

int GfxCall_BglSetCurves(OpenBGI_t* o, int layer, int angleCurve, int scaleCurve)
{
	return Gfx_BglSetCurves(o->gfx.gfx, layer, angleCurve, scaleCurve);
}

int GfxCall_BglSetCentreDelta(OpenBGI_t* o, int layer, int32_t dcx, int32_t dcy)
{
	return Gfx_BglSetCentreDelta(o->gfx.gfx, layer, dcx, dcy);
}

int GfxCall_BglSetXformDelta(OpenBGI_t* o, int layer, int32_t dAngle, int32_t dsx, int32_t dsy)
{
	return Gfx_BglSetXformDelta(o->gfx.gfx, layer, dAngle, dsx, dsy);
}

void GfxCall_BgShow(OpenBGI_t* o, int visible, int mode)
{
	Gfx_BgShow(o, o->gfx.gfx, visible, mode);
}

int GfxCall_BgTypeId(OpenBGI_t* o)
{
	return Gfx_BgTypeId(o->gfx.gfx);
}

uint32_t GfxCall_SpriteCreate(OpenBGI_t* o)
{
	return Gfx_SpriteCreate(o, o->gfx.gfx);
}

int GfxCall_SpriteDelete(OpenBGI_t* o, uint32_t h)
{
	return Gfx_SpriteDelete(o, o->gfx.gfx, h);
}

int GfxCall_SpriteSet(OpenBGI_t* o, uint32_t h, int x, int y, int bmp, int effect, int level, int prio)
{
	return Gfx_SpriteSet(o, o->gfx.gfx, h, x, y, bmp, effect, level, prio);
}

int GfxCall_SpriteSetBlend(OpenBGI_t* o, uint32_t h, int x, int y, int bmp1, int bmp2, int mixRatio, int level, int prio, int levelRoute)
{
	return Gfx_SpriteSetBlend(o, o->gfx.gfx, h, x, y, bmp1, bmp2, mixRatio, level, prio, levelRoute);
}

int GfxCall_SpriteSetTransform(OpenBGI_t* o, uint32_t h, int x, int y, int bmp, int ox, int oy, int32_t angle, int32_t sx, int32_t sy, int smooth, int effect, int level, int prio)
{
	return Gfx_SpriteSetTransform(o, o->gfx.gfx, h, x, y, bmp, ox, oy, angle, sx, sy, smooth, effect, level, prio);
}

int GfxCall_SpriteSetWipe(OpenBGI_t* o, uint32_t h, int x, int y, int bmp, int gray, int wipeMode, int progress, int effect, int level, int prio)
{
	return Gfx_SpriteSetWipe(o, o->gfx.gfx, h, x, y, bmp, gray, wipeMode, progress, effect, level, prio);
}

int GfxCall_SpriteSetRipple(OpenBGI_t* o, uint32_t h, int x, int y, int bmp, int map, int rings, int rippleNo, int level, int fade, int prio)
{
	return Gfx_SpriteSetRipple(o, o->gfx.gfx, h, x, y, bmp, map, rings, rippleNo, level, fade, prio);
}

int GfxCall_SpriteSetProjected(OpenBGI_t* o, uint32_t h, int32_t fx, int32_t fy, int32_t fz, int bmp1, int bmp2, int mixRatio, int levelRoute, int ox, int oy, int32_t angle, int projDist, int projPos, int smooth, int effect, int level, int prio)
{
	return Gfx_SpriteSetProjected(o, o->gfx.gfx, h, fx, fy, fz, bmp1, bmp2, mixRatio, levelRoute, ox, oy, angle, projDist, projPos, smooth, effect, level, prio);
}

int GfxCall_SpriteChangeBitmap(OpenBGI_t* o, uint32_t h, int bmp)
{
	return Gfx_SpriteChangeBitmap(o, o->gfx.gfx, h, bmp);
}

int GfxCall_SpriteLinkMask(OpenBGI_t* o, uint32_t h, uint32_t h2)
{
	return Gfx_SpriteLinkMask(o, o->gfx.gfx, h, h2);
}

int GfxCall_SpriteSetMask(OpenBGI_t* o, uint32_t h, int bmp)
{
	return Gfx_SpriteSetMask(o, o->gfx.gfx, h, bmp);
}

int GfxCall_SpriteShow(OpenBGI_t* o, uint32_t h, int on)
{
	return Gfx_SpriteShow(o, o->gfx.gfx, h, on);
}

int GfxCall_SpriteUpdateRect(OpenBGI_t* o, uint32_t h, int x, int y, int w, int hgt)
{
	return Gfx_SpriteUpdateRect(o, o->gfx.gfx, h, x, y, w, hgt);
}

uint32_t GfxCall_FilterCreate(OpenBGI_t* o)
{
	return Gfx_FilterCreate(o, o->gfx.gfx);
}

int GfxCall_FilterDelete(OpenBGI_t* o, uint32_t h)
{
	return Gfx_FilterDelete(o, o->gfx.gfx, h);
}

int GfxCall_FilterSet(OpenBGI_t* o, uint32_t h, int kind, uint32_t colour, int bmp, int param, int level, int prio)
{
	return Gfx_FilterSet(o, o->gfx.gfx, h, kind, colour, bmp, param, level, prio);
}

int GfxCall_FilterShow(OpenBGI_t* o, uint32_t h, int on)
{
	return Gfx_FilterShow(o, o->gfx.gfx, h, on);
}

uint32_t GfxCall_EffectorCreate(OpenBGI_t* o)
{
	return Gfx_EffectorCreate(o, o->gfx.gfx);
}

int GfxCall_EffectorDelete(OpenBGI_t* o, uint32_t h)
{
	return Gfx_EffectorDelete(o, o->gfx.gfx, h);
}

int GfxCall_EffectorSetVector(OpenBGI_t* o, uint32_t h, int vec1, int vec2, int level, int amount, int prio)
{
	return Gfx_EffectorSetVector(o, o->gfx.gfx, h, vec1, vec2, level, amount, prio);
}

int GfxCall_EffectorSetGradient(OpenBGI_t* o, uint32_t h, int type, int level, int prio)
{
	return Gfx_EffectorSetGradient(o, o->gfx.gfx, h, type, level, prio);
}

int GfxCall_EffectorSetRipple(OpenBGI_t* o, uint32_t h, int map, int rings, int rippleNo, int level, int prio)
{
	return Gfx_EffectorSetRipple(o, o->gfx.gfx, h, map, rings, rippleNo, level, prio);
}

int GfxCall_EffectorSetZoom(OpenBGI_t* o, uint32_t h, int32_t cx, int32_t cy, int32_t angle, int32_t sx, int32_t sy, int smooth, int level, int prio)
{
	return Gfx_EffectorSetZoom(o, o->gfx.gfx, h, cx, cy, angle, sx, sy, smooth, level, prio);
}

int GfxCall_EffectorShow(OpenBGI_t* o, uint32_t h, int on)
{
	return Gfx_EffectorShow(o, o->gfx.gfx, h, on);
}

uint32_t GfxCall_MapCreate(OpenBGI_t* o)
{
	return Gfx_MapCreate(o, o->gfx.gfx);
}

int GfxCall_MapDelete(OpenBGI_t* o, uint32_t h)
{
	return Gfx_MapDelete(o, o->gfx.gfx, h);
}

int GfxCall_MapShow(OpenBGI_t* o, uint32_t h, int on)
{
	return Gfx_MapShow(o, o->gfx.gfx, h, on);
}

int GfxCall_MapSet(OpenBGI_t* o, uint32_t h, int x, int y, int bmp, int effect, int level, int prio)
{
	return Gfx_MapSet(o, o->gfx.gfx, h, x, y, bmp, effect, level, prio);
}

int GfxCall_MapSetSize(OpenBGI_t* o, uint32_t h, int cols, int rows, int chipW, int chipH)
{
	return Gfx_MapSetSize(o, o->gfx.gfx, h, cols, rows, chipW, chipH);
}

int GfxCall_MapSetTerrain(OpenBGI_t* o, uint32_t h, int w, int hgt, const uint16_t* data)
{
	return Gfx_MapSetTerrain(o->gfx.gfx, h, w, hgt, data);
}

int GfxCall_MapSetView(OpenBGI_t* o, uint32_t h, int col, int row, int offX, int offY, int wrap)
{
	return Gfx_MapSetView(o, o->gfx.gfx, h, col, row, offX, offY, wrap);
}

int GfxCall_MapInvalidateChip(OpenBGI_t* o, uint32_t h, int tile)
{
	return Gfx_MapInvalidateChip(o->gfx.gfx, h, tile);
}

void GfxCall_WindowGlobal(OpenBGI_t* o, int shown, int level)
{
	Gfx_WindowGlobal(o, o->gfx.gfx, shown, level);
}

int GfxCall_WindowDelete(OpenBGI_t* o, uint32_t h)
{
	return Gfx_WindowDelete(o, o->gfx.gfx, h);
}

int GfxCall_WindowSet(OpenBGI_t* o, uint32_t h, int x, int y, int effect, int level, int unused, int prio)
{
	return Gfx_WindowSet(o, o->gfx.gfx, h, x, y, effect, level, unused, prio);
}

int GfxCall_WindowSetPunch(OpenBGI_t* o, uint32_t h, int f)
{
	return Gfx_WindowSetPunch(o, o->gfx.gfx, h, f);
}

int GfxCall_WindowSetLayerOrder(OpenBGI_t* o, uint32_t h, int order) // 1.492 on
{
	return Gfx_WindowSetLayerOrder(o, o->gfx.gfx, h, order);
}

int GfxCall_WindowShow(OpenBGI_t* o, uint32_t h, int on)
{
	return Gfx_WindowShow(o, o->gfx.gfx, h, on);
}

int GfxCall_WindowShowFrame(OpenBGI_t* o, uint32_t h, int f)
{
	return Gfx_WindowShowFrame(o, o->gfx.gfx, h, f);
}

int GfxCall_WindowShowText(OpenBGI_t* o, uint32_t h, int f)
{
	return Gfx_WindowShowText(o, o->gfx.gfx, h, f);
}

int GfxCall_WindowClear(OpenBGI_t* o, uint32_t h)
{
	return Gfx_WindowClear(o, o->gfx.gfx, h);
}

int GfxCall_WindowSetCursor(OpenBGI_t* o, uint32_t h, int x, int y)
{
	return Gfx_WindowSetCursor(o->gfx.gfx, h, x, y);
}

int GfxCall_WindowGetCursor(OpenBGI_t* o, int32_t out[2], uint32_t h)
{
	return Gfx_WindowGetCursor(o->gfx.gfx, out, h);
}

int GfxCall_WindowSetTable(OpenBGI_t* o, uint32_t h, const int32_t* table)
{
	return Gfx_WindowSetTable(o->gfx.gfx, h, table);
}

int GfxCall_KnobShow(OpenBGI_t* o, uint32_t h, int on)
{
	return Gfx_KnobShow(o, o->gfx.gfx, h, on);
}

int GfxCall_KnobSetPos(OpenBGI_t* o, uint32_t h, int x, int y)
{
	return Gfx_KnobSetPos(o, o->gfx.gfx, h, x, y);
}

int GfxCall_KnobSetValue(OpenBGI_t* o, uint32_t h, int x, int y)
{
	return Gfx_KnobSetValue(o, o->gfx.gfx, h, x, y);
}

int GfxCall_KnobGetValue(OpenBGI_t* o, uint32_t h, int32_t out[2])
{
	return Gfx_KnobGetValue(o->gfx.gfx, h, out);
}

int GfxCall_KnobTakeNudge(OpenBGI_t* o, uint32_t h, int32_t* out)
{
	return Gfx_KnobTakeNudge(o->gfx.gfx, h, out);
}

int GfxCall_KnobSetDraggable(OpenBGI_t* o, uint32_t h, int f)
{
	return Gfx_KnobSetDraggable(o->gfx.gfx, h, f);
}

uint32_t GfxCall_GroupCreate(OpenBGI_t* o)
{
	return Gfx_GroupCreate(o, o->gfx.gfx);
}

int GfxCall_GroupDelete(OpenBGI_t* o, uint32_t h)
{
	return Gfx_GroupDelete(o, o->gfx.gfx, h);
}

int GfxCall_GroupShow(OpenBGI_t* o, uint32_t h, int on)
{
	return Gfx_GroupShow(o, o->gfx.gfx, h, on);
}

int GfxCall_GroupSet(OpenBGI_t* o, uint32_t h, int x, int y, int level)
{
	return Gfx_GroupSet(o, o->gfx.gfx, h, x, y, level);
}

// ========================================================================
// forwarders with their own result codes
// ========================================================================

/* "90 3D": *out = whether the mouse (in back-buffer coordinates) is over
 * the object, tested relative to the object's position; 1 ok, 0 when the
 * object does not exist */
int GfxCall_ObjHitTest(OpenBGI_t* o, int* out, uint32_t h)
{
	int32_t pos[2], mouse[2];
	if(!Gfx_ObjGetPos(o, o->gfx.gfx, pos, h))
		return 0;
	GetMouseClientPos(o, mouse);
	return Gfx_ObjHitTest(o, o->gfx.gfx, out, h, mouse[0] - pos[0], mouse[1] - pos[1]);
}

// "90 80": 0 ok, 1 no free slot, 2 the size is rejected
int GfxCall_WindowCreate(OpenBGI_t* o, uint32_t* outHandle, int w, int h)
{
	switch(Gfx_WindowCreate(o, o->gfx.gfx, outHandle, w, h))
	{
		case 0: return 0;
		case 9: return 1;
		case 10: return 2;
		default: return h; // (original: returns h)
	}
}

// "90 86": 0 ok, 1..3 the window's errors (no surfaces, no such bitmap, not a frame bitmap), -1 unknown handle
int GfxCall_WindowSetFrame(OpenBGI_t* o, uint32_t h, int unused1, int unused2, int bmp)
{
	switch(Gfx_WindowSetFrame(o, o->gfx.gfx, h, unused1, unused2, bmp))
	{
		case 1: return 1;
		case 2: return 2;
		case 3: return 3;
		case 0xff: return -1;
		default: return 0;
	}
}

// "90 88": 0 ok, 1 the area is outside the text layer, -1 unknown handle
int GfxCall_WindowSetTextArea(OpenBGI_t* o, uint32_t h, int x, int y, int w, int hgt)
{
	int r = Gfx_WindowSetTextArea(o->gfx.gfx, h, x, y, w, hgt);
	return r == 4 ? 1 : r == 0xff ? -1
								  : 0;
}

// "90 89": 0 ok, -1 unknown handle
int GfxCall_WindowGetTextArea(OpenBGI_t* o, Rect_t* out, uint32_t h)
{
	int r = Gfx_WindowGetTextArea(o->gfx.gfx, out, h);
	return r == 0 ? 0 : r == 0xff ? -1
								  : (int)h; // (original: returns h)
}

// "91 8A": 0 ok, 1 bad style, -1 unknown handle
int GfxCall_WindowSetDrawStyle(OpenBGI_t* o, uint32_t h, int style)
{
	int r = Gfx_WindowSetDrawStyle(o->gfx.gfx, h, style);
	return r == 0x12 ? 1 : r == 0xff ? -1
									 : 0;
}

// "91 8B": 0 ok, 1 bad style, -1 unknown handle
int GfxCall_WindowSetSwingStyle(OpenBGI_t* o, uint32_t h, int style)
{
	int r = Gfx_WindowSetSwingStyle(o->gfx.gfx, h, style);
	return r == 0x13 ? 1 : r == 0xff ? -1
									 : 0;
}

/* "91 88": the font by its number (0 MS Gothic, 1 MS Mincho, then the
 * registered faces).  0 ok, 1 size out of range, 2 width percentage out
 * of range, 3 the font could not be created, -1 unknown handle. */
int GfxCall_WindowSetFont(OpenBGI_t* o, uint32_t h, int fontNo, int size, int widthPct, int bold, int proportional,
	int rubyReserve)
{
	switch(Gfx_WindowSetFont(o, o->gfx.gfx, h, FontNameByNo(o, fontNo), size, widthPct, bold, proportional, rubyReserve))
	{
		case 0: return 0;
		case 5: return 1;
		case 6: return 2;
		case 7: return 3;
		case 0xff: return -1;
		default: return rubyReserve; // (original: returns rubyReserve)
	}
}

// "91 89": 0 ok, 1 the percentage is out of range, -1 unknown handle
int GfxCall_WindowSetSpacing(OpenBGI_t* o, uint32_t h, int pct)
{
	switch(Gfx_WindowSetSpacing(o->gfx.gfx, h, pct))
	{
		case 0: return 0;
		case 8: return 1;
		case 0xff: return -1;
		default: return pct; // (original: returns pct)
	}
}

// "92 8A": 0 ok, -1 unknown handle
int GfxCall_WindowFillFrame(OpenBGI_t* o, uint32_t h, uint32_t colour)
{
	int r = Gfx_WindowFillFrame(o, o->gfx.gfx, h, colour);
	return r == 0 ? 0 : r == 0xff ? -1
								  : (int)colour; // (original: returns colour)
}

/* the shared result map of the two "draw a bitmap into a layer" stubs:
 * 0 ok, 1 no bitmap (the manager's 2), 2 incompatible pixel modes (0xC),
 * 3 unknown effect (0xD), 4 nothing overlaps (0xF), 5 level above 0x100
 * (0xE), -1 unknown handle; a window without surfaces (the manager
 * returns the handle) falls through to `level` */
static int WindowDrawResult(int r, int level)
{
	switch(r)
	{
		case 0: return 0;
		case 2: return 1;
		case 0xc: return 2;
		case 0xd: return 3;
		case 0xf: return 4;
		case 0xe: return 5;
		case 0xff: return -1;
		default: return level; // (original: returns level)
	}
}

int GfxCall_WindowDrawToFrame(OpenBGI_t* o, uint32_t h, int x, int y, int bmp, int effect, int level) // "92 89"
{
	return WindowDrawResult(Gfx_WindowDrawToFrame(o, o->gfx.gfx, h, x, y, bmp, effect, level), level);
}

int GfxCall_WindowDrawToText(OpenBGI_t* o, uint32_t h, int x, int y, int bmp, int effect, int level) // "92 8D"
{
	return WindowDrawResult(Gfx_WindowDrawToText(o, o->gfx.gfx, h, x, y, bmp, effect, level), level);
}

// "90 83": 0 ok, 1 the bitmap could not be created, -1 unknown handle
int GfxCall_WindowCopyToBitmap(OpenBGI_t* o, int bmpNo, uint32_t h)
{
	switch(Gfx_WindowCopyToBitmap(o, o->gfx.gfx, bmpNo, h))
	{
		case 0: return 0;
		case 2: return 1;
		case 0xff: return -1;
		default: return (int)h; // (original: returns h)
	}
}

// "91 91" / "91 93" / "92 91": 0 ok, 1 nothing drawn (no font), -1 unknown handle
int GfxCall_WindowDrawText(OpenBGI_t* o, uint32_t h, const char* str, int parseTags, int hang, uint32_t colour, uint32_t rubyColour,
	const int32_t style[5])
{
	switch(Gfx_WindowDrawText(o, o->gfx.gfx, h, str, parseTags, hang, colour, rubyColour, style))
	{
		case 0: return 0;
		case 0x11: return 1;
		case 0xff: return -1;
		default: return (int)(intptr_t)style; // (original: returns the style pointer)
	}
}

// "91 8E": 0 ok, -1 unknown handle
int GfxCall_WindowCursorStatus(OpenBGI_t* o, int* out, uint32_t h)
{
	int r = Gfx_WindowCursorStatus(o, o->gfx.gfx, out, h);
	return r == 0 ? 0 : r == 0xff ? -1
								  : (int)h; // (original: returns h)
}

/* "91 94": the ruby dictionary.  A NULL word clears it, a NULL reading
 * removes the word (1 when it was there, 0 otherwise), otherwise the pair
 * is added; 1 */
int GfxCall_RubySet(OpenBGI_t* o, const char* word, const char* reading)
{
	if(!word)
	{
		Ruby_Clear(o);
		return 1;
	}
	if(!reading)
		return Ruby_Remove(o, word);
	Ruby_Add(o, word, reading);
	return 1;
}

// "90 D0": the manager's codes pass through: 0 ok, 1 no slot, 2 unknown target, 3 the target is a virtual object / group / knob
int GfxCall_KnobCreate(OpenBGI_t* o, uint32_t* outHandle, uint32_t hTarget)
{
	int r = Gfx_KnobCreate(o, o->gfx.gfx, outHandle, hTarget);
	return (uint32_t)r <= 3 ? r : (int)hTarget; // (original: returns hTarget)
}

int GfxCall_KnobDelete(OpenBGI_t* o, uint32_t h)
{
	return Gfx_KnobDelete(o, o->gfx.gfx, h);
}

// "90 D8": 0 ok, 4 a negative step count, -1 unknown handle
int GfxCall_KnobSetSteps(OpenBGI_t* o, uint32_t h, int x, int y)
{
	switch(Gfx_KnobSetSteps(o->gfx.gfx, h, x, y))
	{
		case 0: return 0;
		case 4: return 4;
		case 0xff: return -1;
		default: return y; // (original: returns y)
	}
}

// "90 D9": 0 ok, 5 the range does not hold the thumb, -1 unknown handle
int GfxCall_KnobSetRange(OpenBGI_t* o, uint32_t h, int w, int hgt)
{
	switch(Gfx_KnobSetRange(o, o->gfx.gfx, h, w, hgt))
	{
		case 0: return 0;
		case 5: return 5;
		case 0xff: return -1;
		default: return hgt; // (original: returns hgt)
	}
}

// "90 E8": 0 ok, 1 unknown object, 3 the object is the group, 4 it already has an owner, -1 unknown group
int GfxCall_GroupAdd(OpenBGI_t* o, uint32_t hGroup, uint32_t hObj, int dx, int dy)
{
	switch(Gfx_GroupAdd(o, o->gfx.gfx, hGroup, hObj, dx, dy))
	{
		case 0: return 0;
		case 1: return 1;
		case 3: return 3;
		case 4: return 4;
		case 0xff: return -1;
		default: return dy; // (original: returns dy)
	}
}

// "90 E9": 0 ok, 1 unknown object, 2 it was not a child, -1 unknown group
int GfxCall_GroupRemove(OpenBGI_t* o, uint32_t hGroup, uint32_t hObj)
{
	switch(Gfx_GroupRemove(o->gfx.gfx, hGroup, hObj))
	{
		case 0: return 0;
		case 1: return 1;
		case 2: return 2;
		case 0xff: return -1;
		default: return (int)hObj; // (original: returns hObj)
	}
}

// "90 AF": the three selection classes' "window must be active" switches
void Select_RequireActive(OpenBGI_t* o, int on)
{
	IconSelect_SetRequireActive(o, on);
	MenuSelect_SetRequireActive(o, on);
	Panel_SetRequireActive(o, on);
}
