/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * zoom.c - background type 10: a bitmap zoomed and rotated about the centre
 *          (inc/bgi/gfx/background.h; selected by "90 49")
 *
 * The bitmap (any size) is drawn with Blit_RotateCentred: its centre on
 * the centre of the destination, scaled by `scale` and rotated by `option`
 * (nearest-neighbour sampling).  Both may
 * change with the object's level: the deltas set by "90 38" (parameter
 * 0x80, or 0x101 for the base values) are applied at level 0x100, the zoom
 * eased by a quarter sine and the angle linearly.  The compositor does not
 * band this type.
 */
#include "background_internal.h"

static void Bg10_Destroy(DispObj_t* o, int flags)
{
	Background_Dtor((Background_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

void Bg10_Ctor(Bg10_t* b)
{
	Background_Ctor(&b->bg, 10);
	b->bg.obj.vt = &Bg10_Vtbl.base;
	b->bmp = -1;
}

// zoom factor (16.16, non-zero) and angle (16.16 degrees); the deltas are cleared.  0 ok, 0x80000002 zero scale
int Bg10_SetScale(Bg10_t* b, int scale, int option)
{
	if((uint32_t)scale == 0)
		return (int)0x80000002;
	b->scale = scale;
	b->option = option;
	b->scaleDelta = 0;
	b->deltaOption = 0;
	return 0;
}

// the zoom and angle change reached at level 0x100; 0 ok, 0x80000002 when scale + delta would be zero
int Bg10_SetScaleDelta(Bg10_t* b, int delta, int option)
{
	if(b->scale + delta == 0)
		return (int)0x80000002;
	b->scaleDelta = delta;
	b->deltaOption = option;
	return 0;
}

// the setParam slot: 0x80 is Bg10_SetScaleDelta(a, c), 0x101 is Bg10_SetScale(a, c); 0xFFFF0002 when rejected
static int Bg10_SetParam(DispObj_t* o, int no, int a, int c)
{
	Bg10_t* b = (Bg10_t*)o;
	if(no == 0x80)
		return Bg10_SetScaleDelta(b, a, c) != 0 ? (int)0xffff0002 : 0;
	if(no == 0x101)
		return Bg10_SetScale(b, a, c) != 0 ? (int)0xffff0002 : 0;
	return DispObj_SetParam(o, no, a, c);
}

/* with a scale delta the zoom follows a quarter sine of the level (level
 * 0x100 = 90 degrees); the angle grows linearly.  The arithmetic is in
 * doubles in the original order.  0 once the bitmap has been replaced. */
static int Bg10_Render(Background_t* bg, Bmp_t* dst, const Rect_t* local)
{
	Bg10_t* b = (Bg10_t*)bg;
	Bmp_t src;
	int level;
	int32_t zoom, angle;
	(void)local;
	if(!BgCurrent(&src, b->bmp, b->gen))
		return 0;
	level = GET_LEVEL(bg);
	zoom = b->scale;
	if(b->scaleDelta != 0 && level != 0)
	{
		int32_t delta = b->scaleDelta;
		int32_t ad = delta < 0 ? (int32_t)(0u - (uint32_t)delta) : delta;
		double s = sin((double)(uint32_t)level * 0.006135923151542565); // 2 pi / 1024
		double s256 = s * 256.0;
		double q = (double)delta /
			((double)ad - (((double)(int32_t)((uint32_t)ad - 65536u) * s256) * 0.00390625));
		double sign = delta < 0 ? 1.0 : -1.0;
		zoom = BGI_Ftol((double)(uint32_t)b->scale +
			((((256.0 - s256) * sign) * 0.00390625) + q) * 65536.0);
	}
	angle = b->option + MulShr(b->deltaOption, (uint32_t)level, 8);
	Blit_RotateCentred(dst, &src, zoom, angle, 0);
	return 1;
}

BG_VTABLE(Bg10_Vtbl, Bg10_Destroy, DispObj_SetVisible, Background_Invalidate, Background_NopSetPos,
	DispObj_GetPos, DispObj_SetOffset, DispObj_SetLevel, DispObj_GetLevel,
	Bg10_SetParam, DispObj_NopNotify, Background_SetMode, Background_MatchScreenSize,
	Bg10_Render);
