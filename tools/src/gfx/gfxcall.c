/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * gfxcall.c - the cdecl forwarders onto the global graphics manager
 *             (inc/bgi/gfx/gfxcall.h)
 *
 * Every GfxCall_* function calls the Gfx_* method of the same name on
 * gGfx (or the bitmap manager gBmpMgr) and hands the arguments through.
 * The plain forwarders return the method's result unchanged; the ones at
 * the end translate the method's code into the small code the instruction
 * reports (0 ok, positive errors, -1 unknown handle) and, for a code they
 * do not expect, return their last argument, as the original does.
 */
#include "bgi/gfx/gfxcall.h"
#include "bgi/gfx.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/text.h"
#include "bgi/input.h"

// ---- manager-wide operations ------------------------------------------------

// every object but the rain screens, then every bitmap slot
void Gfx_ResetAll(void)
{
	Gfx_Reset(gGfx);
	BmpMgr_FreeAll(gBmpMgr);
}

// "90 0B": when set, a re-created bitmap slot keeps its generation number (objects keep showing it)
void BmpMgr_SetOption(int v)
{
	gBmpMgr->keepGen = v;
}

// ========================================================================
// plain forwarders
// ========================================================================

int GfxCall_Snapshot(int bmpNo)
{
	return Gfx_Snapshot(gGfx, bmpNo);
}

int GfxCall_SnapshotPrio(int bmpNo, int prio)
{
	return Gfx_SnapshotPrio(gGfx, bmpNo, prio);
}

void GfxCall_SetGlobalEffect(int v)
{
	Gfx_SetGlobalEffect(gGfx, v);
}

void GfxCall_SetDrawLimit(int prio)
{
	Gfx_SetDrawLimit(gGfx, prio);
}

uint32_t GfxCall_GetBandPixels(void)
{
	return Gfx_GetBandPixels(gGfx);
}

// "92 00" / "92 01": the ripple definitions live in the bitmap manager
int GfxCall_DefineRipple(int no, int period, int amplitude, int count, int rings)
{
	return BmpMgr_DefineRipple(gBmpMgr, no, period, amplitude, count, rings);
}

int GfxCall_DefineRippleEx(int no, int period, int amplitude, int fadeIn, int fadeOut, int count, int rings)
{
	return BmpMgr_DefineRippleEx(gBmpMgr, no, period, amplitude, fadeIn, fadeOut, count, rings);
}

int GfxCall_ObjShow(uint32_t h, int on)
{
	return Gfx_ObjShow(gGfx, h, on);
}

int GfxCall_ObjSetEnabled(uint32_t h, int v)
{
	return Gfx_ObjSetEnabled(gGfx, h, v);
}

int GfxCall_ObjSetLevel(uint32_t h, int level)
{
	return Gfx_ObjSetLevel(gGfx, h, level);
}

int GfxCall_ObjSetOpacity(uint32_t h, int v) // 1.599 on
{
	return Gfx_ObjSetOpacity(gGfx, h, v);
}

int GfxCall_ObjSetPos(uint32_t h, int x, int y)
{
	return Gfx_ObjSetPos(gGfx, h, x, y);
}

int GfxCall_ObjSetFixedPos(uint32_t h, int32_t fx, int32_t fy, int32_t fz)
{
	return Gfx_ObjSetFixedPos(gGfx, h, fx, fy, fz);
}

int GfxCall_ObjSetFade(uint32_t h, int v)
{
	return Gfx_ObjSetFade(gGfx, h, v);
}

int GfxCall_ObjSetProgress(uint32_t h, int v)
{
	return Gfx_ObjSetProgress(gGfx, h, v);
}

int GfxCall_ObjSetOffset(uint32_t h, int dx, int dy)
{
	return Gfx_ObjSetOffset(gGfx, h, dx, dy);
}

// 1.494 / 1.69 build 472 on
int GfxCall_ObjSetFixedOffset(uint32_t h, int which, int32_t x, int32_t y, int32_t z)
{
	return Gfx_ObjSetFixedOffset(gGfx, h, which, x, y, z);
}

int GfxCall_ObjGetParam(uint32_t h, int no, int32_t* out)
{
	return Gfx_ObjGetParam(gGfx, h, no, out);
}

int GfxCall_ObjSetOffset2(uint32_t h, int dx, int dy) // 1.494 on
{
	return Gfx_ObjSetOffset2(gGfx, h, dx, dy);
}

int GfxCall_ObjAttach(uint32_t hMaster, uint32_t hSlave, int dx, int dy) // 1.494 on
{
	return Gfx_ObjAttach(gGfx, hMaster, hSlave, dx, dy);
}

int GfxCall_ObjDetach(uint32_t hMaster, uint32_t hSlave)
{
	return Gfx_ObjDetach(gGfx, hMaster, hSlave);
}

int GfxCall_ObjSetPriority(uint32_t h, int prio)
{
	return Gfx_ObjSetPriority(gGfx, h, prio);
}

int GfxCall_ObjSetParam(uint32_t h, int no, int a, int b)
{
	return Gfx_ObjSetParam(gGfx, h, no, a, b);
}

int GfxCall_ObjSetHitMask(uint32_t h, int bmp)
{
	return Gfx_ObjSetHitMask(gGfx, h, bmp);
}

int GfxCall_ObjHasOwner(uint32_t h)
{
	return Gfx_ObjHasOwner(gGfx, h);
}

int GfxCall_ObjBuildCache(uint32_t h)
{
	return Gfx_ObjBuildCache(gGfx, h);
}

int GfxCall_BgSetBitmap(int bmp)
{
	return Gfx_BgSetBitmap(gGfx, bmp);
}

int GfxCall_BgSetFade(int bmp, int second, int level)
{
	return Gfx_BgSetFade(gGfx, bmp, second, level);
}

int GfxCall_BgSetScroll(int b0, int b1, int b2, int b3, int x, int y)
{
	return Gfx_BgSetScroll(gGfx, b0, b1, b2, b3, x, y);
}

int GfxCall_BgSetBlend(int x1, int y1, int bmp1, int x2, int y2, int bmp2, int gray, int param, int level)
{
	return Gfx_BgSetBlend(gGfx, x1, y1, bmp1, x2, y2, bmp2, gray, param, level);
}

int GfxCall_BgSetFrames(int n, const int* list, int level)
{
	return Gfx_BgSetFrames(gGfx, n, list, level);
}

int GfxCall_BgSetDisplace(int bmp, int vec1, int vec2, int level, int amount)
{
	return Gfx_BgSetDisplace(gGfx, bmp, vec1, vec2, level, amount);
}

int GfxCall_BgSetGradient(int bmp, int type, int level)
{
	return Gfx_BgSetGradient(gGfx, bmp, type, level);
}

int GfxCall_BgSetRipple(int bmp, int map, int rings, int rippleNo, int level)
{
	return Gfx_BgSetRipple(gGfx, bmp, map, rings, rippleNo, level);
}

int GfxCall_BgSetView(int bmp, int x, int y, int w, int h)
{
	return Gfx_BgSetView(gGfx, bmp, x, y, w, h);
}

int GfxCall_BgSetZoom(int bmp, int scale, int option)
{
	return Gfx_BgSetZoom(gGfx, bmp, scale, option);
}

int GfxCall_BgSetFlip(int front, int back, int style, int link, int level)
{
	return Gfx_BgSetFlip(gGfx, front, back, style, link, level);
}

int GfxCall_BgSetLayers(int x, int y, int bmp, int32_t cx, int32_t cy, int32_t angle, int32_t sx, int32_t sy, int smooth)
{
	return Gfx_BgSetLayers(gGfx, x, y, bmp, cx, cy, angle, sx, sy, smooth);
}

int GfxCall_BglSelect(int layer)
{
	return Gfx_BglSelect(gGfx, layer);
}

int GfxCall_BglShow(int layer, int on)
{
	return Gfx_BglShow(gGfx, layer, on);
}

int GfxCall_BglSetPos(int layer, int32_t x, int32_t y)
{
	return Gfx_BglSetPos(gGfx, layer, x, y);
}

int GfxCall_BglSetEffect(int layer, int effect)
{
	return Gfx_BglSetEffect(gGfx, layer, effect);
}

int GfxCall_BglSetLevel(int layer, int level)
{
	return Gfx_BglSetLevel(gGfx, layer, level);
}

int GfxCall_BglSetBitmap(int layer, int bmp, int32_t cx, int32_t cy)
{
	return Gfx_BglSetBitmap(gGfx, layer, bmp, cx, cy);
}

int GfxCall_BglSetTransform(int layer, int32_t angle, int32_t sx, int32_t sy, int smooth)
{
	return Gfx_BglSetTransform(gGfx, layer, angle, sx, sy, smooth);
}

int GfxCall_BglSetCurves(int layer, int angleCurve, int scaleCurve)
{
	return Gfx_BglSetCurves(gGfx, layer, angleCurve, scaleCurve);
}

int GfxCall_BglSetCentreDelta(int layer, int32_t dcx, int32_t dcy)
{
	return Gfx_BglSetCentreDelta(gGfx, layer, dcx, dcy);
}

int GfxCall_BglSetXformDelta(int layer, int32_t dAngle, int32_t dsx, int32_t dsy)
{
	return Gfx_BglSetXformDelta(gGfx, layer, dAngle, dsx, dsy);
}

void GfxCall_BgShow(int visible, int mode)
{
	Gfx_BgShow(gGfx, visible, mode);
}

int GfxCall_BgTypeId(void)
{
	return Gfx_BgTypeId(gGfx);
}

uint32_t GfxCall_SpriteCreate(void)
{
	return Gfx_SpriteCreate(gGfx);
}

int GfxCall_SpriteDelete(uint32_t h)
{
	return Gfx_SpriteDelete(gGfx, h);
}

int GfxCall_SpriteSet(uint32_t h, int x, int y, int bmp, int effect, int level, int prio)
{
	return Gfx_SpriteSet(gGfx, h, x, y, bmp, effect, level, prio);
}

int GfxCall_SpriteSetBlend(uint32_t h, int x, int y, int bmp1, int bmp2, int mixRatio, int level, int prio, int levelRoute)
{
	return Gfx_SpriteSetBlend(gGfx, h, x, y, bmp1, bmp2, mixRatio, level, prio, levelRoute);
}

int GfxCall_SpriteSetTransform(uint32_t h, int x, int y, int bmp, int ox, int oy, int32_t angle, int32_t sx, int32_t sy, int smooth, int effect, int level, int prio)
{
	return Gfx_SpriteSetTransform(gGfx, h, x, y, bmp, ox, oy, angle, sx, sy, smooth, effect, level, prio);
}

int GfxCall_SpriteSetWipe(uint32_t h, int x, int y, int bmp, int gray, int wipeMode, int progress, int effect, int level, int prio)
{
	return Gfx_SpriteSetWipe(gGfx, h, x, y, bmp, gray, wipeMode, progress, effect, level, prio);
}

int GfxCall_SpriteSetRipple(uint32_t h, int x, int y, int bmp, int map, int rings, int rippleNo, int level, int fade, int prio)
{
	return Gfx_SpriteSetRipple(gGfx, h, x, y, bmp, map, rings, rippleNo, level, fade, prio);
}

int GfxCall_SpriteSetProjected(uint32_t h, int32_t fx, int32_t fy, int32_t fz, int bmp1, int bmp2, int mixRatio, int levelRoute, int ox, int oy, int32_t angle, int projDist, int projPos, int smooth, int effect, int level, int prio)
{
	return Gfx_SpriteSetProjected(gGfx, h, fx, fy, fz, bmp1, bmp2, mixRatio, levelRoute, ox, oy, angle, projDist, projPos, smooth, effect, level, prio);
}

int GfxCall_SpriteChangeBitmap(uint32_t h, int bmp)
{
	return Gfx_SpriteChangeBitmap(gGfx, h, bmp);
}

int GfxCall_SpriteLinkMask(uint32_t h, uint32_t h2)
{
	return Gfx_SpriteLinkMask(gGfx, h, h2);
}

int GfxCall_SpriteSetMask(uint32_t h, int bmp)
{
	return Gfx_SpriteSetMask(gGfx, h, bmp);
}

int GfxCall_SpriteShow(uint32_t h, int on)
{
	return Gfx_SpriteShow(gGfx, h, on);
}

int GfxCall_SpriteUpdateRect(uint32_t h, int x, int y, int w, int hgt)
{
	return Gfx_SpriteUpdateRect(gGfx, h, x, y, w, hgt);
}

uint32_t GfxCall_FilterCreate(void)
{
	return Gfx_FilterCreate(gGfx);
}

int GfxCall_FilterDelete(uint32_t h)
{
	return Gfx_FilterDelete(gGfx, h);
}

int GfxCall_FilterSet(uint32_t h, int kind, uint32_t colour, int bmp, int param, int level, int prio)
{
	return Gfx_FilterSet(gGfx, h, kind, colour, bmp, param, level, prio);
}

int GfxCall_FilterShow(uint32_t h, int on)
{
	return Gfx_FilterShow(gGfx, h, on);
}

uint32_t GfxCall_EffectorCreate(void)
{
	return Gfx_EffectorCreate(gGfx);
}

int GfxCall_EffectorDelete(uint32_t h)
{
	return Gfx_EffectorDelete(gGfx, h);
}

int GfxCall_EffectorSetVector(uint32_t h, int vec1, int vec2, int level, int amount, int prio)
{
	return Gfx_EffectorSetVector(gGfx, h, vec1, vec2, level, amount, prio);
}

int GfxCall_EffectorSetGradient(uint32_t h, int type, int level, int prio)
{
	return Gfx_EffectorSetGradient(gGfx, h, type, level, prio);
}

int GfxCall_EffectorSetRipple(uint32_t h, int map, int rings, int rippleNo, int level, int prio)
{
	return Gfx_EffectorSetRipple(gGfx, h, map, rings, rippleNo, level, prio);
}

int GfxCall_EffectorSetZoom(uint32_t h, int32_t cx, int32_t cy, int32_t angle, int32_t sx, int32_t sy, int smooth, int level, int prio)
{
	return Gfx_EffectorSetZoom(gGfx, h, cx, cy, angle, sx, sy, smooth, level, prio);
}

int GfxCall_EffectorShow(uint32_t h, int on)
{
	return Gfx_EffectorShow(gGfx, h, on);
}

uint32_t GfxCall_MapCreate(void)
{
	return Gfx_MapCreate(gGfx);
}

int GfxCall_MapDelete(uint32_t h)
{
	return Gfx_MapDelete(gGfx, h);
}

int GfxCall_MapShow(uint32_t h, int on)
{
	return Gfx_MapShow(gGfx, h, on);
}

int GfxCall_MapSet(uint32_t h, int x, int y, int bmp, int effect, int level, int prio)
{
	return Gfx_MapSet(gGfx, h, x, y, bmp, effect, level, prio);
}

int GfxCall_MapSetSize(uint32_t h, int cols, int rows, int chipW, int chipH)
{
	return Gfx_MapSetSize(gGfx, h, cols, rows, chipW, chipH);
}

int GfxCall_MapSetTerrain(uint32_t h, int w, int hgt, const uint16_t* data)
{
	return Gfx_MapSetTerrain(gGfx, h, w, hgt, data);
}

int GfxCall_MapSetView(uint32_t h, int col, int row, int offX, int offY, int wrap)
{
	return Gfx_MapSetView(gGfx, h, col, row, offX, offY, wrap);
}

int GfxCall_MapInvalidateChip(uint32_t h, int tile)
{
	return Gfx_MapInvalidateChip(gGfx, h, tile);
}

void GfxCall_WindowGlobal(int shown, int level)
{
	Gfx_WindowGlobal(gGfx, shown, level);
}

int GfxCall_WindowDelete(uint32_t h)
{
	return Gfx_WindowDelete(gGfx, h);
}

int GfxCall_WindowSet(uint32_t h, int x, int y, int effect, int level, int unused, int prio)
{
	return Gfx_WindowSet(gGfx, h, x, y, effect, level, unused, prio);
}

int GfxCall_WindowSetPunch(uint32_t h, int f)
{
	return Gfx_WindowSetPunch(gGfx, h, f);
}

int GfxCall_WindowSetLayerOrder(uint32_t h, int order) // 1.494 on
{
	return Gfx_WindowSetLayerOrder(gGfx, h, order);
}

int GfxCall_WindowShow(uint32_t h, int on)
{
	return Gfx_WindowShow(gGfx, h, on);
}

int GfxCall_WindowShowFrame(uint32_t h, int f)
{
	return Gfx_WindowShowFrame(gGfx, h, f);
}

int GfxCall_WindowShowText(uint32_t h, int f)
{
	return Gfx_WindowShowText(gGfx, h, f);
}

int GfxCall_WindowClear(uint32_t h)
{
	return Gfx_WindowClear(gGfx, h);
}

int GfxCall_WindowSetCursor(uint32_t h, int x, int y)
{
	return Gfx_WindowSetCursor(gGfx, h, x, y);
}

int GfxCall_WindowGetCursor(int32_t out[2], uint32_t h)
{
	return Gfx_WindowGetCursor(gGfx, out, h);
}

int GfxCall_WindowSetTable(uint32_t h, const int32_t* table)
{
	return Gfx_WindowSetTable(gGfx, h, table);
}

int GfxCall_KnobShow(uint32_t h, int on)
{
	return Gfx_KnobShow(gGfx, h, on);
}

int GfxCall_KnobSetPos(uint32_t h, int x, int y)
{
	return Gfx_KnobSetPos(gGfx, h, x, y);
}

int GfxCall_KnobSetValue(uint32_t h, int x, int y)
{
	return Gfx_KnobSetValue(gGfx, h, x, y);
}

int GfxCall_KnobGetValue(uint32_t h, int32_t out[2])
{
	return Gfx_KnobGetValue(gGfx, h, out);
}

int GfxCall_KnobTakeNudge(uint32_t h, int32_t* out)
{
	return Gfx_KnobTakeNudge(gGfx, h, out);
}

int GfxCall_KnobSetDraggable(uint32_t h, int f)
{
	return Gfx_KnobSetDraggable(gGfx, h, f);
}

uint32_t GfxCall_GroupCreate(void)
{
	return Gfx_GroupCreate(gGfx);
}

int GfxCall_GroupDelete(uint32_t h)
{
	return Gfx_GroupDelete(gGfx, h);
}

int GfxCall_GroupShow(uint32_t h, int on)
{
	return Gfx_GroupShow(gGfx, h, on);
}

int GfxCall_GroupSet(uint32_t h, int x, int y, int level)
{
	return Gfx_GroupSet(gGfx, h, x, y, level);
}

// ========================================================================
// forwarders with their own result codes
// ========================================================================

/* "90 3D": *out = whether the mouse (in back-buffer coordinates) is over
 * the object, tested relative to the object's position; 1 ok, 0 when the
 * object does not exist */
int GfxCall_ObjHitTest(int* out, uint32_t h)
{
	int32_t pos[2], mouse[2];
	if(!Gfx_ObjGetPos(gGfx, pos, h))
		return 0;
	GetMouseClientPos(mouse);
	return Gfx_ObjHitTest(gGfx, out, h, mouse[0] - pos[0], mouse[1] - pos[1]);
}

// "90 80": 0 ok, 1 no free slot, 2 the size is rejected
int GfxCall_WindowCreate(uint32_t* outHandle, int w, int h)
{
	switch(Gfx_WindowCreate(gGfx, outHandle, w, h))
	{
		case 0: return 0;
		case 9: return 1;
		case 10: return 2;
		default: return h; // (original: returns h)
	}
}

// "90 86": 0 ok, 1..3 the window's errors (no surfaces, no such bitmap, not a frame bitmap), -1 unknown handle
int GfxCall_WindowSetFrame(uint32_t h, int unused1, int unused2, int bmp)
{
	switch(Gfx_WindowSetFrame(gGfx, h, unused1, unused2, bmp))
	{
		case 1: return 1;
		case 2: return 2;
		case 3: return 3;
		case 0xff: return -1;
		default: return 0;
	}
}

// "90 88": 0 ok, 1 the area is outside the text layer, -1 unknown handle
int GfxCall_WindowSetTextArea(uint32_t h, int x, int y, int w, int hgt)
{
	int r = Gfx_WindowSetTextArea(gGfx, h, x, y, w, hgt);
	return r == 4 ? 1 : r == 0xff ? -1
								  : 0;
}

// "90 89": 0 ok, -1 unknown handle
int GfxCall_WindowGetTextArea(Rect_t* out, uint32_t h)
{
	int r = Gfx_WindowGetTextArea(gGfx, out, h);
	return r == 0 ? 0 : r == 0xff ? -1
								  : (int)h; // (original: returns h)
}

// "91 8A": 0 ok, 1 bad style, -1 unknown handle
int GfxCall_WindowSetDrawStyle(uint32_t h, int style)
{
	int r = Gfx_WindowSetDrawStyle(gGfx, h, style);
	return r == 0x12 ? 1 : r == 0xff ? -1
									 : 0;
}

// "91 8B": 0 ok, 1 bad style, -1 unknown handle
int GfxCall_WindowSetSwingStyle(uint32_t h, int style)
{
	int r = Gfx_WindowSetSwingStyle(gGfx, h, style);
	return r == 0x13 ? 1 : r == 0xff ? -1
									 : 0;
}

/* "91 88": the font by its number (0 MS Gothic, 1 MS Mincho, then the
 * registered faces).  0 ok, 1 size out of range, 2 width percentage out
 * of range, 3 the font could not be created, -1 unknown handle. */
int GfxCall_WindowSetFont(uint32_t h, int fontNo, int size, int widthPct, int bold, int proportional,
	int rubyReserve)
{
	switch(Gfx_WindowSetFont(gGfx, h, FontNameByNo(fontNo), size, widthPct, bold, proportional, rubyReserve))
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
int GfxCall_WindowSetSpacing(uint32_t h, int pct)
{
	switch(Gfx_WindowSetSpacing(gGfx, h, pct))
	{
		case 0: return 0;
		case 8: return 1;
		case 0xff: return -1;
		default: return pct; // (original: returns pct)
	}
}

// "92 8A": 0 ok, -1 unknown handle
int GfxCall_WindowFillFrame(uint32_t h, uint32_t colour)
{
	int r = Gfx_WindowFillFrame(gGfx, h, colour);
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

int GfxCall_WindowDrawToFrame(uint32_t h, int x, int y, int bmp, int effect, int level) // "92 89"
{
	return WindowDrawResult(Gfx_WindowDrawToFrame(gGfx, h, x, y, bmp, effect, level), level);
}

int GfxCall_WindowDrawToText(uint32_t h, int x, int y, int bmp, int effect, int level) // "92 8D"
{
	return WindowDrawResult(Gfx_WindowDrawToText(gGfx, h, x, y, bmp, effect, level), level);
}

// "90 83": 0 ok, 1 the bitmap could not be created, -1 unknown handle
int GfxCall_WindowCopyToBitmap(int bmpNo, uint32_t h)
{
	switch(Gfx_WindowCopyToBitmap(gGfx, bmpNo, h))
	{
		case 0: return 0;
		case 2: return 1;
		case 0xff: return -1;
		default: return (int)h; // (original: returns h)
	}
}

// "91 91" / "91 93" / "92 91": 0 ok, 1 nothing drawn (no font), -1 unknown handle
int GfxCall_WindowDrawText(uint32_t h, const char* str, int parseTags, int hang, uint32_t colour, uint32_t rubyColour,
	const int32_t style[5])
{
	switch(Gfx_WindowDrawText(gGfx, h, str, parseTags, hang, colour, rubyColour, style))
	{
		case 0: return 0;
		case 0x11: return 1;
		case 0xff: return -1;
		default: return (int)(intptr_t)style; // (original: returns the style pointer)
	}
}

// "91 8E": 0 ok, -1 unknown handle
int GfxCall_WindowCursorStatus(int* out, uint32_t h)
{
	int r = Gfx_WindowCursorStatus(gGfx, out, h);
	return r == 0 ? 0 : r == 0xff ? -1
								  : (int)h; // (original: returns h)
}

/* "91 94": the ruby dictionary.  A NULL word clears it, a NULL reading
 * removes the word (1 when it was there, 0 otherwise), otherwise the pair
 * is added; 1 */
int GfxCall_RubySet(const char* word, const char* reading)
{
	if(!word)
	{
		Ruby_Clear();
		return 1;
	}
	if(!reading)
		return Ruby_Remove(word);
	Ruby_Add(word, reading);
	return 1;
}

// "90 D0": the manager's codes pass through: 0 ok, 1 no slot, 2 unknown target, 3 the target is a virtual object / group / knob
int GfxCall_KnobCreate(uint32_t* outHandle, uint32_t hTarget)
{
	int r = Gfx_KnobCreate(gGfx, outHandle, hTarget);
	return (uint32_t)r <= 3 ? r : (int)hTarget; // (original: returns hTarget)
}

int GfxCall_KnobDelete(uint32_t h)
{
	return Gfx_KnobDelete(gGfx, h);
}

// "90 D8": 0 ok, 4 a negative step count, -1 unknown handle
int GfxCall_KnobSetSteps(uint32_t h, int x, int y)
{
	switch(Gfx_KnobSetSteps(gGfx, h, x, y))
	{
		case 0: return 0;
		case 4: return 4;
		case 0xff: return -1;
		default: return y; // (original: returns y)
	}
}

// "90 D9": 0 ok, 5 the range does not hold the thumb, -1 unknown handle
int GfxCall_KnobSetRange(uint32_t h, int w, int hgt)
{
	switch(Gfx_KnobSetRange(gGfx, h, w, hgt))
	{
		case 0: return 0;
		case 5: return 5;
		case 0xff: return -1;
		default: return hgt; // (original: returns hgt)
	}
}

// "90 E8": 0 ok, 1 unknown object, 3 the object is the group, 4 it already has an owner, -1 unknown group
int GfxCall_GroupAdd(uint32_t hGroup, uint32_t hObj, int dx, int dy)
{
	switch(Gfx_GroupAdd(gGfx, hGroup, hObj, dx, dy))
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
int GfxCall_GroupRemove(uint32_t hGroup, uint32_t hObj)
{
	switch(Gfx_GroupRemove(gGfx, hGroup, hObj))
	{
		case 0: return 0;
		case 1: return 1;
		case 2: return 2;
		case 0xff: return -1;
		default: return (int)hObj; // (original: returns hObj)
	}
}

// "90 AF": the three selection classes' "window must be active" switches
void Select_RequireActive(int on)
{
	IconSelect_SetRequireActive(on);
	MenuSelect_SetRequireActive(on);
	Panel_SetRequireActive(on);
}
