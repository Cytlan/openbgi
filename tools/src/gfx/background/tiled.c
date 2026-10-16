/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tiled.c - background type 3: a 2 x 2 tiled view of four bitmaps
 *           ("S mode" scrolling; inc/bgi/gfx/background.h; selected by "90 42")
 *
 * Four screen-sized bitmaps form a plane of 2W x 2H pixels; the screen
 * shows the W x H window of it whose top-left corner is the origin
 * (ox, oy).  The setPos slot ("90 33" on handle 0) moves the origin;
 * getPos is not overridden and reports the object's own position, which
 * the override does not touch.
 */
#include "background_internal.h"

static int Bg3_MatchScreenSize(Background_t* bg);

static void Bg3_Destroy(DispObj_t* o, int flags)
{
	Background_Dtor((Background_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

// no bitmaps; the quadrants are laid out for the current screen size
void Bg3_Ctor(Bg3_t* b)
{
	int i;
	Background_Ctor(&b->bg, 3);
	b->bg.obj.vt = &Bg3_Vtbl.base;
	Bg3_MatchScreenSize(&b->bg);
	for(i = 0; i < 4; i++)
		b->bmp[i] = -1;
}

/* the four bitmaps: top-left, top-right, bottom-left, bottom-right, all
 * screen sized.  1 ok, 0 when one is unusable (nothing is changed). */
int Bg3_Set(Bg3_t* b, int b0, int b1, int b2, int b3)
{
	int i;
	if(!Bg_BitmapUsable(b0) || !Bg_BitmapUsable(b1) || !Bg_BitmapUsable(b2) || !Bg_BitmapUsable(b3))
		return 0;
	b->bmp[0] = b0;
	b->bmp[1] = b1;
	b->bmp[2] = b2;
	b->bmp[3] = b3;
	for(i = 0; i < 4; i++)
		b->gen[i] = BgGen(b->bmp[i]);
	return 1;
}

// the origin may run from (0, 0) to (W, H) inclusive; 1 ok, 0 out of range
int Bg3_SetOrigin(Bg3_t* b, int x, int y)
{
	Bmp_t back;
	Gfx_GetBackBmp(&back);
	if(x < 0 || x > back.w || y < 0 || y > back.h)
		return 0;
	b->ox = x;
	b->oy = y;
	return 1;
}

// the setPos slot moves the origin (an out-of-range position is ignored)
static void Bg3_SetPos(DispObj_t* o, int x, int y)
{
	Bg3_SetOrigin((Bg3_t*)o, x, y);
}

// the quadrants are rebuilt from the back buffer size
static int Bg3_MatchScreenSize(Background_t* bg)
{
	Bg3_t* b = (Bg3_t*)bg;
	Bmp_t back;
	int r = Background_MatchScreenSize(bg), W, H;
	Gfx_GetBackBmp(&back);
	W = back.w;
	H = back.h;
	b->quad[0].l = 0;
	b->quad[0].t = 0;
	b->quad[0].r = W - 1;
	b->quad[0].b = H - 1;
	b->quad[1].l = W;
	b->quad[1].t = 0;
	b->quad[1].r = 2 * W - 1;
	b->quad[1].b = H - 1;
	b->quad[2].l = 0;
	b->quad[2].t = H;
	b->quad[2].r = W - 1;
	b->quad[2].b = 2 * H - 1;
	b->quad[3].l = W;
	b->quad[3].t = H;
	b->quad[3].r = 2 * W - 1;
	b->quad[3].b = 2 * H - 1;
	return r;
}

/* the view starts at (ox, oy) in the 2W x 2H plane; the part of every
 * quadrant that intersects the dirty rectangle is copied.  0 once a
 * bitmap has been replaced. */
static int Bg3_Render(Background_t* bg, Bmp_t* dst, const Rect_t* local)
{
	Bg3_t* b = (Bg3_t*)bg;
	Bmp_t src[4];
	int i, ok = 1;
	for(i = 0; i < 4; i++)
	{
		ok = BgCurrent(&src[i], b->bmp[i], b->gen[i]);
		if(!ok)
			return 0;
	}
	for(i = 0; i < 4; i++)
	{
		const Rect_t* q = &b->quad[i];
		Rect_t r = *local;
		Bmp_t view;
		Rect_Offset(&r, b->ox - q->l, b->oy - q->t); // into the quadrant's bitmap
		if(!Rect_Clip(&r, &b->quad[0]))              // quad[0] = the bitmap size
			continue;
		view = *dst;
		Bmp_Crop(&src[i], &r);
		Rect_Offset(&r, q->l - b->ox - local->l, q->t - b->oy - local->t); // back to dst
		Bmp_Crop(&view, &r);
		Bmp_BlitEffect(&view, &src[i], 0x80, 0); // raw copy
	}
	return ok;
}

BG_VTABLE(Bg3_Vtbl, Bg3_Destroy, DispObj_SetVisible, Background_Invalidate, Bg3_SetPos,
	DispObj_GetPos, DispObj_SetOffset, DispObj_SetLevel, DispObj_GetLevel,
	DispObj_SetParam, DispObj_NopNotify, Background_SetMode, Bg3_MatchScreenSize,
	Bg3_Render);
