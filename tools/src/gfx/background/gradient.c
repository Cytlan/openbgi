/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * gradient.c - background type 7: a bitmap through the horizontal box blur
 *              (inc/bgi/gfx/background.h; selected by "90 46")
 *
 * The scripts call the effect "gradient"; Blit_Gradient is a horizontal
 * box blur whose width grows with the object's level (0 = the bitmap as it
 * is).  The compositor does not band this type: the whole screen is one
 * job.
 */
#include "background_internal.h"

static void Bg7_Destroy(DispObj_t* o, int flags)
{
	Background_Dtor((Background_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

void Bg7_Ctor(Bg7_t* b)
{
	Background_Ctor(&b->bg, 7);
	b->bg.obj.vt = &Bg7_Vtbl.base;
	b->bmp = -1;
}

// a screen-sized bitmap; 0 ok, 0x80000001 missing, 0x80000002 not screen sized
int Bg7_SetBitmap(Bg7_t* b, int bmp)
{
	Bmp_t info;
	if(!BgGetInfo(&info, bmp))
		return (int)0x80000001;
	if(!Bmp_MatchesScreen(&info))
		return (int)0x80000002;
	b->bmp = bmp;
	b->gen = BgGen(bmp);
	return 0;
}

// the edge handling of the blur: 0 pads with black, 1 repeats the edge; 0 ok, 0x80000003 anything else
int Bg7_SetType(Bg7_t* b, int type)
{
	if((uint32_t)type > 1)
		return (int)0x80000003;
	b->type = type;
	return 0;
}

// the bitmap blurred by the effective level; 0 once it has been replaced
static int Bg7_Render(Background_t* bg, Bmp_t* dst, const Rect_t* local)
{
	Bg7_t* b = (Bg7_t*)bg;
	Bmp_t src;
	if(!BgCurrent(&src, b->bmp, b->gen))
		return 0;
	Bmp_Crop(&src, local);
	Blit_Gradient(dst, &src, b->type, DispObj_EffectiveLevel(&b->bg.obj));
	return 1;
}

BG_VTABLE(Bg7_Vtbl, Bg7_Destroy, DispObj_SetVisible, Background_Invalidate, Background_NopSetPos,
	DispObj_GetPos, DispObj_SetOffset, DispObj_SetLevel, DispObj_GetLevel,
	DispObj_SetParam, DispObj_NopNotify, Background_SetMode, Background_MatchScreenSize,
	Bg7_Render);
