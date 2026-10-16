/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * stretch.c - background type 9: a stretched view of a bitmap
 *             (inc/bgi/gfx/background.h; selected by "90 48")
 *
 * A rectangle of the bitmap (the view) is stretched over the whole screen.
 * At level 0 the view is (x, y, viewW, viewH); at level 0x100 it is the
 * target rectangle (tx, ty, tw, th), and in between it moves and grows
 * linearly, which gives a pan-and-zoom over a picture larger than the
 * screen.  The view origin is the object's position (in 16.16), the target
 * is set with "90 38" (parameter 0x102).  The compositor does not band
 * this type.
 */
#include "background_internal.h"

static void Bg9_Destroy(DispObj_t* o, int flags)
{
	Background_Dtor((Background_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

void Bg9_Ctor(Bg9_t* b)
{
	Background_Ctor(&b->bg, 9);
	b->bg.obj.vt = &Bg9_Vtbl.base;
	b->bmp = -1;
}

// a bitmap of any size; 0 ok, 0x80000001 missing (type 10 shares this: its fields coincide)
int Bg9_SetBitmap(Bg9_t* b, int bmp)
{
	Bmp_t info;
	if(!BgGetInfo(&info, bmp))
		return (int)0x80000001;
	b->bmp = bmp;
	b->gen = BgGen(bmp);
	return 0;
}

int Bg10_SetBitmap(Bg10_t* b, int bmp)
{
	return Bg9_SetBitmap((Bg9_t*)b, bmp);
}

// the setPos / getPos slots address the view origin (16.16)
static void Bg9_SetPos(DispObj_t* o, int x, int y)
{
	Bg9_t* b = (Bg9_t*)o;
	b->x = x;
	b->y = y;
}

static void Bg9_GetPos(DispObj_t* o, int32_t out[2])
{
	Bg9_t* b = (Bg9_t*)o;
	out[0] = b->x;
	out[1] = b->y;
}

// the view size at level 0 (pixels); 0 ok, 0x80000002 a side below 2
int Bg9_SetViewSize(Bg9_t* b, int w, int h)
{
	if((uint32_t)w < 2 || (uint32_t)h < 2)
		return (int)0x80000002;
	b->viewH = h;
	b->viewW = w;
	return 0;
}

// the target rectangle at level 0x100 (pixels); 0 ok, 0x80000002 a side below 2
int Bg9_SetTarget(Bg9_t* b, int x, int y, int w, int h)
{
	if((uint32_t)w < 2 || (uint32_t)h < 2)
		return (int)0x80000002;
	b->tx = x;
	b->ty = y;
	b->tw = w;
	b->th = h;
	return 0;
}

/* the setParam slot: parameter 0x102 packs the target, a = x | y << 16
 * (signed halves), c = w | h << 16 (unsigned halves); 0xFFFF0002 when the
 * size is rejected */
static int Bg9_SetParam(DispObj_t* o, int no, int a, int c)
{
	if(no != 0x102)
		return DispObj_SetParam(o, no, a, c);
	return Bg9_SetTarget((Bg9_t*)o, (int16_t)(a & 0xffff), (int16_t)((uint32_t)a >> 16),
			   (int)(c & 0xffff), (int)((uint32_t)c >> 16)) != 0
		? (int)0xffff0002
		: 0;
}

/* the view rectangle (x, y, viewW, viewH at level 0) moves and grows
 * towards the target rectangle with the level and is stretched over the
 * destination with Blit_Scale2().  The 65540 (not 65536) in the scale
 * factors is the original's; the arithmetic is in doubles in the original
 * order.  0 once the bitmap has been replaced. */
static int Bg9_Render(Background_t* bg, Bmp_t* dst, const Rect_t* local)
{
	Bg9_t* b = (Bg9_t*)bg;
	Bmp_t src;
	double level, wp, hp;
	int32_t A, B, C, D, X, Y;
	(void)local;
	if(!BgCurrent(&src, b->bmp, b->gen))
		return 0;
	level = (double)(uint32_t)GET_LEVEL(bg);
	wp = (double)(uint32_t)b->viewW + ((double)(int32_t)((uint32_t)b->tw - (uint32_t)b->viewW) * level) * 0.00390625; // 1 / 256
	hp = (double)(uint32_t)b->viewH + ((double)(int32_t)((uint32_t)b->th - (uint32_t)b->viewH) * level) * 0.00390625;
	A = BGI_Ftol((double)((uint32_t)dst->h * 65540u) / hp); // vertical scale
	B = BGI_Ftol((double)((uint32_t)dst->w * 65540u) / wp); // horizontal scale
	C = BGI_Ftol(hp * 65536.0);                             // view height 16.16
	D = BGI_Ftol(wp * 65536.0);                             // view width 16.16
	Y = BGI_Ftol(((double)(int32_t)(((uint32_t)b->ty << 16) - (uint32_t)b->y) * level) * 0.00390625 + (double)b->y);
	X = BGI_Ftol(((double)(int32_t)(((uint32_t)b->tx << 16) - (uint32_t)b->x) * level) * 0.00390625 + (double)b->x);
	Blit_Scale2(dst, &src, X, Y, D, C, B, A, 1);
	return 1;
}

BG_VTABLE(Bg9_Vtbl, Bg9_Destroy, DispObj_SetVisible, Background_Invalidate, Bg9_SetPos,
	Bg9_GetPos, DispObj_SetOffset, DispObj_SetLevel, DispObj_GetLevel,
	Bg9_SetParam, DispObj_NopNotify, Background_SetMode, Background_MatchScreenSize,
	Bg9_Render);
