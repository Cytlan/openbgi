/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * filter.c - Filter, the full-screen colour operation (class order 7).
 * Interface in inc/bgi/gfx/objects.h.
 *
 * A filter has no picture: its draw() applies one colour operation to the
 * pixels already in the back buffer, either uniformly (kinds 0..3) or
 * modulated by a screen-sized gray bitmap (kind 0 only).  It sits in class
 * order 7 with effect 0xC0, which puts it in the "strength" family of
 * DispObj_EffectiveLevel() so that an inherited fade weakens the filter.
 * Instructions "90 60".."90 66".
 */
#include "bgi/common.h"
#include "bgi/gfx/objects.h"

// class order 7, effect 0xC0, kind 0 in black at level 0, sized to the screen
void Filter_Ctor(Filter_t* f, int slotId)
{
	DispObj_Ctor(&f->obj, 7, slotId);
	f->obj.vt = &Filter_Vtbl;
	DispObj_SetEffect(&f->obj, 0xc0);
	Filter_Set(f, 0, 0, 0, 0);
	Filter_MatchScreenSize(f);
}

static void Filter_Destroy(DispObj_t* o, int flags)
{
	o->vt = &Filter_Vtbl;
	DispObj_Dtor(o);
	if(flags & 1)
		BGI_Free(o);
}

/* Apply the operation in place to the dirty piece: kind 0 blends toward the
 * colour, 1 adds colour * level, 2 tints by luminance, 3 blends toward the
 * picture XOR the colour (see the Blit_FilterKind* routines).  With a gray
 * bitmap the tint is weighted per pixel by the bitmap under the piece;
 * nothing is drawn when that bitmap was freed or reloaded. */
static void Filter_Draw(DispObj_t* o, Bmp_t* dst, const Rect_t* local, uint32_t minKey)
{
	Filter_t* f = (Filter_t*)o;
	int level = DispObj_EffectiveLevel(o);
	(void)minKey;
	if(f->useBitmap == 0)
	{
		switch(f->kind)
		{ // all work in place
			case 0: Blit_Tint(dst, dst, f->colour, level); break;
			case 1: Blit_FilterKind1(dst, dst, f->colour, level); break;
			case 2: Blit_FilterKind2(dst, dst, f->colour, level); break;
			case 3: Blit_FilterKind3(dst, dst, f->colour, level); break;
			default: break;
		}
	}
	else if(f->useBitmap == 1)
	{
		Bmp_t g;
		if(!BmpMgr_GetInfo(gDispBmpMgr, &g, f->bmp))
			return;
		if(BmpMgr_Generation(gDispBmpMgr, f->bmp) != f->bmpGen)
			return;
		Bmp_Crop(&g, local);
		Blit_TintByGray(dst, f->colour, &g, f->bmpParam, level);
	}
}

// "90 65" / "90 66": kind, colour (0x00RRGGBB), level and priority; the gray bitmap is dropped
void Filter_Set(Filter_t* f, int kind, uint32_t colour, int level, int prio)
{
	f->kind = kind;
	Filter_SetColour(f, colour);
	f->obj.vt->setLevel(&f->obj, level);
	DispObj_SetPriority(&f->obj, (uint32_t)prio);
	f->useBitmap = 0;
}

/* "90 66": a screen-sized GRAY8 bitmap modulates the tint per pixel, with
 * `param` the transition curve handed to Blit_TintByGray; -1 returns to the
 * uniform operation.  0 ok, 0x80000001 no such bitmap, 0x80000002 not
 * GRAY8, 0x80000003 not the size of the screen. */
int Filter_SetBitmap(Filter_t* f, int bmp, int param)
{
	Bmp_t info, back;
	if(bmp == -1)
	{
		f->useBitmap = 0;
		return 0;
	}
	if(!BmpMgr_GetInfo(gDispBmpMgr, &info, bmp))
		return (int)0x80000001;
	if(info.mode != 3)
		return (int)0x80000002;
	Gfx_CopyBackBmp(gDispGfx, &back);
	if(info.w != back.w || info.h != back.h)
		return (int)0x80000003;
	f->bmp = bmp;
	f->bmpParam = param;
	f->bmpGen = BmpMgr_Generation(gDispBmpMgr, bmp);
	f->useBitmap = 1;
	return 0;
}

// after a resolution change: take the back buffer's size and drop the (now wrongly sized) bitmap; 1 if anything changed
int Filter_MatchScreenSize(Filter_t* f)
{
	Bmp_t back, own;
	Gfx_GetBackBmp(&back);
	DispObj_CopyBmp(&f->obj, &own);
	if(back.w == own.w && back.h == own.h && back.mode == own.mode)
		return 0;
	f->useBitmap = 0;
	f->obj.vt->setSize(&f->obj, back.w, back.h);
	return 1;
}

void Filter_SetColour(Filter_t* f, uint32_t colour)
{
	f->colour = colour;
}

const DispObjVtbl_t Filter_Vtbl = {
	Filter_Destroy,
	DispObj_SetVisible,
	DispObj_IsVisible,
	DispObj_Invalidate,
	Filter_Draw,
	DispObj_SortKey,
	DispObj_LocalRect,
	DispObj_ScreenRect,
	DispObj_SetPosEx,
	DispObj_SetPos,
	DispObj_GetPos,
	DispObj_GetDrawPos,
	DispObj_SetOffset,
	DispObj_SetFixedPos,
	DispObj_GetFixedPos,
	DispObj_SetLevel,
	DispObj_GetLevel,
	DispObj_SetProgress,
	DispObj_SetParam,
	DispObj_HitTest,
	DispObj_NopNotify,
	DispObj_BuildCache,
	DispObj_SetSize,
	DispObj_GetParam,
};
