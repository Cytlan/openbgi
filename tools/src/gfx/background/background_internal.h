/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * background_internal.h - what the background types share (src/gfx/background/)
 *
 * The public interface is inc/bgi/gfx/background.h.  The base class and the
 * factory are in base.c; the twelve types are one file each (plain.c,
 * crossfade.c, tiled.c, twopos.c, flipbook.c, displace.c, gradient.c,
 * ripple.c, stretch.c, zoom.c, mosaic.c, layers.c).  Every type file ends
 * with its vtable, built with BG_VTABLE from the base methods and the
 * slots the type overrides.
 */
#ifndef BGI_GFX_BACKGROUND_INTERNAL_H_
#define BGI_GFX_BACKGROUND_INTERNAL_H_

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "bgi/common.h"
#include "bgi/gfx/background.h"

// the description of bitmap `no` of the shared bitmap manager; 0 when the slot is empty
static inline int BgGetInfo(Bmp_t* out, int no)
{
	return BmpMgr_GetInfo(gDispBmpMgr, out, no);
}

// the generation of slot `no` (bumped whenever the script re-creates the bitmap)
static inline int BgGen(int no)
{
	return BmpMgr_Generation(gDispBmpMgr, no);
}

// the description of bitmap `no` when its generation is still `gen` (it was not replaced); 1 if usable
static inline int BgCurrent(Bmp_t* out, int no, int gen)
{
	return BgGetInfo(out, no) && BgGen(no) == gen;
}

// (a * b) >> sh in 64 bits, b zero-extended
static inline int32_t MulShr(int32_t a, uint32_t b, int sh)
{
	uint64_t p = (uint64_t)(int64_t)a * (uint64_t)b;
	return (int32_t)((int64_t)p >> sh);
}

// the virtual methods of a background `b`
#define VT(b)        ((const DispObjVtbl_t*)(b)->obj.vt)
#define GET_LEVEL(b) (VT(b)->getLevel(&(b)->obj))

/* a vtable that differs from the base only in the given slots: the DispObj
 * methods a type may override, then the three background methods */
#define BG_VTABLE(name, destroyFn, setVisibleFn, invalidateFn, setPosFn, getPosFn, setOffsetFn, setLevelFn, getLevelFn,   \
	setParamFn, notifyFn, setModeFn, matchFn, renderFn)                                                                   \
	const BackgroundVtbl_t name = {{destroyFn, setVisibleFn, DispObj_IsVisible, invalidateFn, Background_Draw,            \
									   DispObj_SortKey, DispObj_LocalRect, Background_ScreenRect, DispObj_SetPosEx,       \
									   setPosFn, getPosFn, DispObj_GetDrawPos, setOffsetFn, DispObj_SetFixedPos,          \
									   DispObj_GetFixedPos, setLevelFn, getLevelFn, DispObj_SetProgress, setParamFn,      \
									   DispObj_HitTest, notifyFn, DispObj_BuildCache, DispObj_SetSize, DispObj_GetParam}, \
		setModeFn, matchFn, renderFn}

// setPos of the types whose position is fixed: does nothing
void Background_NopSetPos(DispObj_t* o, int x, int y);

#endif // BGI_GFX_BACKGROUND_INTERNAL_H_
