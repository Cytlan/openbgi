/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * plain.c - background type 1: one screen-sized bitmap, copied as it is
 *           (src/include/bgi/gfx/background.h; selected by "90 40")
 */
#include "background_internal.h"

static void Bg1_Destroy(OpenBGI_t* o, DispObj_t* obj, int flags)
{
	Background_Dtor((Background_t*)obj);
	if(flags & 1)
		BGI_Free(obj);
}

void Bg1_Ctor(OpenBGI_t* o, Bg1_t* b)
{
	Background_Ctor(o, &b->bg, 1);
	b->bg.obj.vt = &Bg1_Vtbl.base;
	b->bmp = -1;
}

// the bitmap to show; 1 ok, 0 when it is missing or not screen sized
int Bg1_Set(OpenBGI_t* o, Bg1_t* b, int bmp)
{
	if(!Bg_BitmapUsable(o, bmp))
		return 0;
	b->bmp = bmp;
	b->gen = BgGen(o, bmp);
	return 1;
}

// copy the dirty part; 0 once the bitmap has been replaced
static int Bg1_Render(OpenBGI_t* o, Background_t* bg, Bmp_t* dst, const Rect_t* local)
{
	Bg1_t* b = (Bg1_t*)bg;
	Bmp_t src;
	if(!BgCurrent(o, &src, b->bmp, b->gen))
		return 0;
	Bmp_Crop(&src, local);
	Bmp_BlitEffect(dst, &src, 0x80, 0); // raw copy
	return 1;
}

BG_VTABLE(Bg1_Vtbl, Bg1_Destroy, DispObj_SetVisible, Background_Invalidate, Background_NopSetPos,
	DispObj_GetPos, DispObj_SetOffset, DispObj_SetLevel, DispObj_GetLevel,
	DispObj_SetParam, DispObj_NopNotify, Background_SetMode, Background_MatchScreenSize,
	Bg1_Render);
