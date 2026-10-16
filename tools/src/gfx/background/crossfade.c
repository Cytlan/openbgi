/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * crossfade.c - background type 2: a cross fade between two bitmaps
 *               (inc/bgi/gfx/background.h; selected by "90 41")
 *
 * The object's level (0..0x100) is the position of the fade: at 0 the
 * first bitmap is shown as it is, at 0x100 the second one (or solid black
 * / white) shows through completely.  A script animates it with the level
 * tween of handle 0.
 */
#include "background_internal.h"

static void Bg2_Destroy(DispObj_t* o, int flags)
{
	Background_Dtor((Background_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

// effect 1 (alpha blend), no bitmaps
void Bg2_Ctor(Bg2_t* b)
{
	Background_Ctor(&b->bg, 2);
	b->bg.obj.vt = &Bg2_Vtbl.base;
	b->bmp = -1;
	b->second = -1;
	DispObj_SetEffect(&b->bg.obj, 1);
}

/* `bmp` must be a screen-sized bitmap; `second` is one as well or
 * BG_BLACK / BG_WHITE.  1 ok, 0 when a bitmap is unusable. */
int Bg2_Set(Bg2_t* b, int bmp, int second)
{
	if(!Bg_BitmapUsable(bmp))
		return 0;
	if(second == BG_BLACK || second == BG_WHITE)
	{
		b->bmp = bmp;
		b->second = second;
		b->gen = BgGen(bmp);
		return 1;
	}
	if(!Bg_BitmapUsable(second))
		return 0;
	b->bmp = bmp;
	b->second = second;
	b->gen = BgGen(bmp);
	b->secondGen = BgGen(second);
	return 1;
}

/* the second bitmap (or black / white) with `bmp` blended over it by the
 * effective level; 0 once a bitmap has been replaced */
static int Bg2_Render(Background_t* bg, Bmp_t* dst, const Rect_t* local)
{
	Bg2_t* b = (Bg2_t*)bg;
	Bmp_t a, s;
	int level;
	if(!BgCurrent(&a, b->bmp, b->gen))
		return 0;
	level = DispObj_EffectiveLevel(&b->bg.obj);
	if(b->second == BG_BLACK || b->second == BG_WHITE)
	{
		int effect = b->second == BG_BLACK ? 0xc0 : 0xc1; // dim toward black / tint toward white
		Bmp_Crop(&a, local);
		Bmp_BlitEffect(dst, &a, effect, level);
		return 1;
	}
	if(!BgCurrent(&s, b->second, b->secondGen))
		return 0;
	Bmp_Crop(&s, local);
	Bmp_BlitEffect(dst, &s, 0x80, 0); // raw copy of the second bitmap
	Bmp_Crop(&a, local);
	Bmp_BlitEffect(dst, &a, 0xf0, level); // the first blended over it, ignoring its alpha
	return 1;
}

BG_VTABLE(Bg2_Vtbl, Bg2_Destroy, DispObj_SetVisible, Background_Invalidate, Background_NopSetPos,
	DispObj_GetPos, DispObj_SetOffset, DispObj_SetLevel, DispObj_GetLevel,
	DispObj_SetParam, DispObj_NopNotify, Background_SetMode, Background_MatchScreenSize,
	Bg2_Render);
