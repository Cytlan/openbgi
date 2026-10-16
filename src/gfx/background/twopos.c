/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * twopos.c - background type 4: two positioned bitmaps, with a gray wipe
 *            between them (inc/bgi/gfx/background.h; selected by "90 43")
 *
 * The lower bitmap (or a solid colour) is copied at (x2, y2); the upper
 * one is drawn over it at (x1, y1), either alpha-blended by the object's
 * level or revealed through a grayscale map (the wipe).  Neither bitmap
 * has to be screen sized.  The position of the upper bitmap is the
 * object's position, so "90 33" on handle 0 and the move tween scroll it.
 *
 * The wipe map is screen sized and aligned with the upper bitmap up to
 * 1.494; from 1.529 on it may be of any size and is aligned with the
 * screen (the renderer crops it like the destination band and hands it
 * to the blitter's screen-aligned form), so a mask smaller than the
 * screen reveals the upper picture inside its own area only.
 */
#include "background_internal.h"
#include "bgi/version.h"

// the wipe map is screen aligned and of any size (1.529 on)
static int Bg4_ScreenGray(void)
{
	return gEngine->gen >= GEN_1_529;
}

// the wipe of the upper bitmap `a` at (ax, ay) of `dst` through the map `g`, in the form of the build
static void Bg4_Wipe(const Bg4_t* b, Bmp_t* dst, int ax, int ay, const Bmp_t* a, const Bmp_t* g, const Rect_t* local, int level)
{
	if(Bg4_ScreenGray())
	{
		Bmp_t band = *g;
		Rect_t r;
		Rect_FromBmp(&r, g);
		if(!Rect_Clip(&r, local)) // the band lies outside the map: nothing is revealed there
			return;
		Bmp_Crop(&band, &r); // the band's (0, 0) is local's corner, which the map covers
		Blit_ThroughGrayScreen(dst, ax, ay, a, &band, b->grayParam, level);
	}
	else
		Blit_ThroughGray(dst, ax, ay, a, g, b->grayParam, level);
}

static void Bg4_Destroy(DispObj_t* o, int flags)
{
	Background_Dtor((Background_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

// effect 1 (alpha blend), no bitmaps, no wipe
void Bg4_Ctor(Bg4_t* b)
{
	Background_Ctor(&b->bg, 4);
	b->bg.obj.vt = &Bg4_Vtbl.base;
	b->bmp1 = -1;
	b->bmp2 = -1;
	b->gray = -1;
	DispObj_SetEffect(&b->bg.obj, 1);
}

// the setPos / getPos slots address the upper bitmap's position
static void Bg4_SetPos(DispObj_t* o, int x, int y)
{
	Bg4_t* b = (Bg4_t*)o;
	b->x1 = x;
	b->y1 = y;
}

static void Bg4_GetPos(DispObj_t* o, int32_t out[2])
{
	Bg4_t* b = (Bg4_t*)o;
	out[0] = b->x1;
	out[1] = b->y1;
}

// a lower "bitmap" number that stands for a colour or for nothing
static int Bg4_IsSentinel(int no)
{
	return no == BG_BLACK || no == BG_WHITE || no == BG_NONE || no == -1;
}

/* bmp1 may have any size (0x80000001 if missing); bmp2 is a bitmap of any
 * size (0x80000002 if missing) or a sentinel.  0 ok. */
int Bg4_Set(Bg4_t* b, int x1, int y1, int bmp1, int x2, int y2, int bmp2)
{
	Bmp_t info;
	if(!BgGetInfo(&info, bmp1))
		return (int)0x80000001;
	if(Bg4_IsSentinel(bmp2))
	{
		b->gen1 = BgGen(bmp1);
	}
	else
	{
		if(!BgGetInfo(&info, bmp2))
			return (int)0x80000002;
		b->gen1 = BgGen(bmp1);
		b->gen2 = BgGen(bmp2);
	}
	b->x1 = x1;
	b->y1 = y1;
	b->bmp2 = bmp2;
	b->bmp1 = bmp1;
	b->x2 = x2;
	b->y2 = y2;
	return 0;
}

/* a grayscale map (mode 3) that drives the wipe between the two bitmaps,
 * with `param` as the threshold parameter of Blit_ThroughGray; gray -1
 * selects a plain alpha blend.  0 ok, 0x80000003 map missing, 0x80000004
 * not grayscale, 0x80000005 not screen sized (up to 1.494 only: later
 * builds take a map of any size). */
int Bg4_SetWipe(Bg4_t* b, int gray, int param)
{
	Bmp_t info, back;
	if(gray == -1)
	{
		b->gray = -1;
		return 0;
	}
	if(!BgGetInfo(&info, gray))
		return (int)0x80000003;
	if(info.mode != 3)
		return (int)0x80000004;
	Gfx_GetBackBmp(&back);
	if(!Bg4_ScreenGray() && (info.w != back.w || info.h != back.h))
		return (int)0x80000005;
	b->gray = gray;
	b->grayParam = param;
	b->grayGen = BgGen(gray);
	return 0;
}

/* the lower bitmap (or colour) with the upper one blended over it by the
 * effective level, or revealed through the wipe map; 0 once a bitmap has
 * been replaced.  The bitmap positions are relative to the screen, so
 * they are offset by the dirty rectangle's corner. */
static int Bg4_Render(Background_t* bg, Bmp_t* dst, const Rect_t* local)
{
	Bg4_t* b = (Bg4_t*)bg;
	Bmp_t a, s, g;
	int level, ax, ay;
	if(!BgCurrent(&a, b->bmp1, b->gen1))
		return 0;
	level = DispObj_EffectiveLevel(&b->bg.obj);
	ax = -(b->x1 + local->l); // bitmap positions relative to `dst`
	ay = -(b->y1 + local->t);

	if(!Bg4_IsSentinel(b->bmp2))
	{
		if(!BgCurrent(&s, b->bmp2, b->gen2))
			return 0;
		if(b->gray != -1)
		{
			if(!BgCurrent(&g, b->gray, b->grayGen))
				return 0;
			Bmp_Blit(dst, -(b->x2 + local->l), -(b->y2 + local->t), &s, 0x80, 0);
			Bg4_Wipe(b, dst, ax, ay, &a, &g, local, level);
		}
		else
		{
			Bmp_Blit(dst, -(b->x2 + local->l), -(b->y2 + local->t), &s, 0x80, 0);
			Bmp_Blit(dst, ax, ay, &a, 1, level);
		}
		return 1;
	}

	// sentinel: the lower layer is a solid colour.  It is only painted when
	// the upper bitmap does not cover the whole screen on its own or a wipe
	// is active; the fill test is skipped (no fill) for BG_NONE / -1.
	{
		int effect = 0x80, needFill = 0;
		uint32_t fill = 0;
		if(b->bmp2 == BG_BLACK || b->bmp2 == BG_WHITE)
		{
			Bmp_t back;
			if(b->bmp2 == BG_BLACK)
			{
				fill = 0;
				effect = 0xc0; // dim toward black
			}
			else
			{
				fill = 0xffffff;
				effect = 0xc1; // tint toward white
			}
			Gfx_GetBackBmp(&back);
			needFill = !(b->x1 >= 0 && b->y1 >= 0 &&
				(uint32_t)(a.w - b->x1) >= (uint32_t)back.w &&
				(uint32_t)(a.h - b->y1) >= (uint32_t)back.h &&
				b->gray == -1);
		}
		if(b->gray != -1)
		{
			if(!BgCurrent(&g, b->gray, b->grayGen))
				return 0;
			if(needFill)
				Bmp_Fill(dst, NULL, fill);
			Bg4_Wipe(b, dst, ax, ay, &a, &g, local, level);
		}
		else
		{
			if(needFill)
				Bmp_Fill(dst, NULL, fill);
			else if((uint32_t)level > 0)
				effect = 0xc0; // the original fades toward black here even for BG_WHITE
			Bmp_Blit(dst, ax, ay, &a, effect, level);
		}
		return 1;
	}
}

BG_VTABLE(Bg4_Vtbl, Bg4_Destroy, DispObj_SetVisible, Background_Invalidate, Bg4_SetPos,
	Bg4_GetPos, DispObj_SetOffset, DispObj_SetLevel, DispObj_GetLevel,
	DispObj_SetParam, DispObj_NopNotify, Background_SetMode, Background_MatchScreenSize,
	Bg4_Render);
