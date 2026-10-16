/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * mosaic.c - background type 11: the mosaic ("flip") transition between two
 *            bitmaps (inc/bgi/gfx/background.h; selected by "90 4A")
 *
 * The front bitmap is drawn as a mosaic of (level + 1)-pixel blocks
 * (Blit_Flip).  With `link` set the back bitmap (or a solid colour) is
 * painted underneath first, mosaiced by 0x100 - level, and the front
 * blocks are blended over it with the level as their transparency, so a
 * level tween from 0x100 down to 0 dissolves the back picture into the
 * front one.  The compositor does not band this type.
 */
#include "background_internal.h"

static void Bg11_Destroy(DispObj_t* o, int flags)
{
	Background_Dtor((Background_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

void Bg11_Ctor(Bg11_t* b)
{
	Background_Ctor(&b->bg, 11);
	b->bg.obj.vt = &Bg11_Vtbl.base;
	b->front = -1;
	b->back = -1;
	Bg11_SetStyle(b, 0);
	Bg11_SetLink(b, 0);
}

/* front and back are screen-sized bitmaps, or back is BG_BLACK / BG_WHITE.
 * 0 ok, 0x80000001 front unusable, 0x80000002 back unusable. */
int Bg11_Set(Bg11_t* b, int front, int back)
{
	if(!Bg_BitmapUsable(front))
		return (int)0x80000001;
	if(back == BG_BLACK || back == BG_WHITE)
	{
		b->front = front;
		b->frontGen = BgGen(front);
		b->back = back;
		return 0;
	}
	if(!Bg_BitmapUsable(back))
		return (int)0x80000002;
	b->frontGen = BgGen(front);
	b->backGen = BgGen(back);
	b->front = front;
	b->back = back;
	return 0;
}

// only style 0 exists; 1 ok, 0 for anything else
int Bg11_SetStyle(Bg11_t* b, int style)
{
	if(style != 0)
		return 0;
	b->style = 0;
	return 1;
}

// link 0 or 1 (also mosaic the back bitmap); 1 ok, 0 for anything else
int Bg11_SetLink(Bg11_t* b, int link)
{
	if((uint32_t)link > 1)
		return 0;
	b->link = link;
	return 1;
}

/* with link == 1 the back bitmap is mosaiced by 0x100 - level first (or
 * the solid colour is filled), then the front one by the level.  The
 * bitmaps are not cropped to `local`: the mosaic reads whole blocks, and
 * the compositor does not band this background type.  0 once the front
 * bitmap has been replaced; a replaced back bitmap is skipped. */
static int Bg11_Render(Background_t* bg, Bmp_t* dst, const Rect_t* local)
{
	Bg11_t* b = (Bg11_t*)bg;
	Bmp_t front, back;
	int level;
	(void)local;
	if(!BgCurrent(&front, b->front, b->frontGen))
		return 0;
	level = DispObj_EffectiveLevel(&b->bg.obj);
	if(b->back == BG_BLACK || b->back == BG_WHITE)
	{
		if(b->link == 1)
			Bmp_Fill(dst, NULL, b->back == BG_BLACK ? 0 : 0xffffff);
	}
	else if(b->link == 1)
	{
		if(BgCurrent(&back, b->back, b->backGen))
			Blit_Flip(dst, &back, 0x100 - level, b->style, 0);
	}
	Blit_Flip(dst, &front, level, b->style, b->link == 1 ? level : 0);
	return 1;
}

BG_VTABLE(Bg11_Vtbl, Bg11_Destroy, DispObj_SetVisible, Background_Invalidate, Background_NopSetPos,
	DispObj_GetPos, DispObj_SetOffset, DispObj_SetLevel, DispObj_GetLevel,
	DispObj_SetParam, DispObj_NopNotify, Background_SetMode, Background_MatchScreenSize,
	Bg11_Render);
