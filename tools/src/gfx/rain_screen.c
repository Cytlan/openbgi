/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * rain_screen.c - the rain screen display object: streaks falling through
 *                 a perspective camera (the generator of src/gfx/rain.c),
 *                 drawn into the screen's own surface, optionally through
 *                 a gray mask.  Interface: inc/bgi/gfx/screens.h.
 *
 * The screen is reached through the graphics manager (src/gfx/mgr/screens.c)
 * by the "C0 4x" instructions.  The parameters (area, fall per stage,
 * streak length, colour, drops per stage and stage interval, camera,
 * angles, projection distance) are kept in the screen and pushed to the
 * generator as a block by every setter; the generator exists only from
 * "C0 42" on, so a setter called before it keeps the value and reports
 * 0x80000002.  The screens are updated once per scheduler pass and redrawn
 * at the effect frame rate (Rain_FrameTick); gEffectsEnabled, the switch
 * of "C0 4F", hides them all.
 */
#include <string.h>

#include "bgi/common.h"
#include "bgi/gfx/screens.h"

int gEffectsEnabled = 1; // the switch of "C0 4F": 0 hides every rain screen and stops the effect frames

void Fx_SetEffectsEnabled(int on)
{
	gEffectsEnabled = on;
}

int Fx_GetEffectsEnabled(void)
{
	return gEffectsEnabled;
}

/* a display object of class order 5 with no generator, no surface and the
 * default parameters, which place the spawn plane below the floor (yTop
 * -4000, yBottom 4000, y up): a script has to set the area before the
 * rain can be seen */
void RainScreen_Ctor(RainScreen_t* r, int slotId)
{
	DispObj_Ctor(&r->obj, 5, slotId);
	r->obj.vt = &RainScreen_Vtbl;
	r->gen = NULL;
	memset(&r->surf, 0, sizeof r->surf);
	r->params.xMin = -6000;
	r->params.yTop = -4000;
	r->params.zMin = -2000;
	r->params.xMax = 6000;
	r->params.yBottom = 4000;
	r->params.zMax = 2000;
	r->params.dir[0] = 0;
	r->params.dir[1] = -1;
	r->params.dir[2] = 0;
	r->params.fall = 60 << 8;
	r->params.length = 350 << 8;
	r->params.colour = 0xffffffffu;
	r->params.clearColour = 0;
	r->params.perStage = 20;
	r->params.stageMs = 50;
	r->params.hiResTimer = 1;
	memset(&r->view, 0, sizeof r->view);
	r->view.dist = 100;
	r->maskBmp = -1;
}

void RainScreen_Dtor(RainScreen_t* r)
{
	r->obj.vt = &RainScreen_Vtbl;
	if(r->gen)
		RainGen_Delete(r->gen);
	BGI_Free(r->surf.pixels);
	DispObj_Dtor(&r->obj);
}

static void RainScreen_Destroy(DispObj_t* o, int flags)
{
	RainScreen_Dtor((RainScreen_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

/* "C0 42": replace the generator by a fresh one with the screen's
 * parameters and view, pre-aged by `prerollMs` milliseconds (RainGen_Start
 * says how much of that takes effect) */
void RainScreen_Start(RainScreen_t* r, uint32_t prerollMs)
{
	RainParams_t oldP;
	RainView_t oldV;
	if(r->gen)
		RainGen_Delete(r->gen);
	r->gen = RainGen_New();
	RainGen_SetParams(r->gen, &oldP, &r->params);
	RainGen_SetView(r->gen, &oldV, &r->view);
	RainGen_Start(r->gen, prerollMs);
}

/* "C0 40": size the object and allocate its surface (the back buffer's
 * mode with alpha).  0 ok, 0x80000001 when the object rejects the size
 * (the old surface stays). */
int RainScreen_SetSize(RainScreen_t* r, int w, int h)
{
	int ok = r->obj.vt->setSize(&r->obj, w, h);
	if(ok)
	{
		BGI_Free(r->surf.pixels);
		Bmp_AllocScreen(&r->surf, w, h, 1);
		Bmp_Clear(&r->surf, NULL);
	}
	return ok ? 0 : (int)0x80000001;
}

// once per scheduler pass: run the stages that are due.  0 ok, 0x80000002 without a generator
int RainScreen_Update(RainScreen_t* r)
{
	if(r->gen)
		RainGen_Update(r->gen);
	return r->gen ? 0 : (int)0x80000002;
}

/* every effect frame (Rain_FrameTick): clear the surface, draw the drops
 * into it and dirty the whole layer.  0 ok, 0x80000003 without a surface,
 * 0x80000002 without a generator. */
int RainScreen_Redraw(RainScreen_t* r)
{
	if(!r->surf.pixels)
		return (int)0x80000003;
	if(!r->gen)
		return (int)0x80000002;
	Bmp_Clear(&r->surf, NULL);
	RainGen_Render(r->gen, r->surf.pixels, r->surf.w, r->surf.h, r->surf.pitch, r->surf.bpp * 8);
	r->obj.vt->invalidate(&r->obj);
	return 0;
}

/* every setter stores the value and re-sends the whole block to the
 * generator; without one they report 0x80000002 (the value is kept and
 * reaches the generator that "C0 42" creates) */
static int RainScreen_PushParams(RainScreen_t* r)
{
	RainParams_t old;
	if(!r->gen)
		return (int)0x80000002;
	RainGen_SetParams(r->gen, &old, &r->params);
	return 0;
}

static int RainScreen_PushView(RainScreen_t* r)
{
	RainView_t old;
	if(!r->gen)
		return (int)0x80000002;
	RainGen_SetView(r->gen, &old, &r->view);
	return 0;
}

// "C0 46": the box the drops are born in (y up: yTop is the spawn plane, yBottom the floor)
int RainScreen_SetArea(RainScreen_t* r, int xMin, int yTop, int zMin, int xMax, int yBottom, int zMax)
{
	r->params.yTop = yTop;
	r->params.xMin = xMin;
	r->params.zMin = zMin;
	r->params.yBottom = yBottom;
	r->params.xMax = xMax;
	r->params.zMax = zMax;
	return RainScreen_PushParams(r);
}

// "C0 47": the distance a drop falls per stage (stored 24.8); 0x80000004 for 0
int RainScreen_SetFall(RainScreen_t* r, int n)
{
	if((uint32_t)n == 0)
		return (int)0x80000004;
	r->params.fall = n << 8;
	return RainScreen_PushParams(r);
}

// "C0 48": the streak length (stored 24.8); 0x80000004 for 0
int RainScreen_SetLength(RainScreen_t* r, int n)
{
	if((uint32_t)n == 0)
		return (int)0x80000004;
	r->params.length = n << 8;
	return RainScreen_PushParams(r);
}

int RainScreen_SetColour(RainScreen_t* r, uint32_t argb) // "C0 49": the streak colour; the alpha fades with depth
{
	r->params.colour = argb;
	return RainScreen_PushParams(r);
}

int RainScreen_SetPerStage(RainScreen_t* r, int n) // "C0 4A": drops born per stage
{
	r->params.perStage = n;
	return RainScreen_PushParams(r);
}

// "C0 4B": milliseconds per stage; 0x80000004 for 0
int RainScreen_SetStageMs(RainScreen_t* r, int n)
{
	if((uint32_t)n == 0)
		return (int)0x80000004;
	r->params.stageMs = (uint32_t)n;
	return RainScreen_PushParams(r);
}

int RainScreen_SetCamera(RainScreen_t* r, int x, int y, int z) // "C0 4C": the camera position
{
	r->view.cam[1] = y;
	r->view.cam[0] = x;
	r->view.cam[2] = z;
	return RainScreen_PushView(r);
}

int RainScreen_SetAngles(RainScreen_t* r, int a, int b, int c) // "C0 4D": the camera angles, 0.1 degree units
{
	r->view.ang[1] = b;
	r->view.ang[0] = a;
	r->view.ang[2] = c;
	return RainScreen_PushView(r);
}

// "C0 4E": the projection distance, which is also the near plane; 0x80000004 for 0
int RainScreen_SetDistance(RainScreen_t* r, int n)
{
	if((uint32_t)n == 0)
		return (int)0x80000004;
	r->view.dist = n;
	return RainScreen_PushView(r);
}

static int RainScreen_IsVisible(DispObj_t* o) // hidden while the effects are switched off ("C0 4F")
{
	return DispObj_IsVisible(o) && Fx_GetEffectsEnabled();
}

/* the compositor's draw: the surface with the object's effect and level,
 * through the gray mask of "C0 43" when one is set (the mask moves with
 * the object and is skipped when its bitmap was replaced since).  Effect
 * 0 copies where the mask is set, 1 and 0x20 blend there with the level;
 * any other effect goes through a masked copy of the surface. */
static void RainScreen_Draw(DispObj_t* o, Bmp_t* dst, const Rect_t* local, uint32_t minKey)
{
	RainScreen_t* r = (RainScreen_t*)o;
	Bmp_t src, mask;
	int effect;
	(void)minKey;
	if(!r->surf.pixels)
		return;
	src = r->surf;
	Bmp_Crop(&src, local);
	if(r->maskBmp == -1)
	{
		Bmp_BlitEffect(dst, &src, DispObj_GetEffect(o), DispObj_EffectiveLevel(o));
		return;
	}
	if(!BmpMgr_GetInfo(gDispBmpMgr, &mask, r->maskBmp))
		return;
	if(BmpMgr_Generation(gDispBmpMgr, r->maskBmp) != r->maskGen)
		return;
	Bmp_Crop(&mask, local);
	effect = DispObj_GetEffect(o);
	if(effect == 0)
	{
		Blit_Masked(dst, &src, &mask, 0);
	}
	else if(effect == 1 || effect == 0x20)
	{
		Blit_Masked(dst, &src, &mask, DispObj_EffectiveLevel(o));
	}
	else
	{
		Bmp_t tmp;
		Bmp_Alloc(&tmp, src.w, src.h, src.mode);
		Blit_MaskedTo(&tmp, &src, &mask);
		Bmp_BlitEffect(dst, &tmp, effect, DispObj_EffectiveLevel(o));
		Bmp_Free(&tmp);
	}
}

// "C0 45": position, effect, level and priority in one call; always 0
int RainScreen_SetDraw(RainScreen_t* r, int x, int y, int effect, int level, int prio)
{
	r->obj.vt->setPos(&r->obj, x, y);
	DispObj_SetEffect(&r->obj, effect);
	r->obj.vt->setLevel(&r->obj, level);
	DispObj_SetPriority(&r->obj, (uint32_t)prio);
	return 0;
}

/* "C0 43": draw through the managed bitmap `bmp`, a GRAY8 bitmap of the
 * screen's own size, or without a mask for -1.  0 ok, 0x80000005 no such
 * bitmap, 0x80000006 wrong mode or size.  The bitmap's generation is
 * remembered: a bitmap loaded into the slot later is ignored. */
int RainScreen_SetMask(RainScreen_t* r, int bmp)
{
	Bmp_t info;
	int w, h;
	if(bmp == -1)
	{
		r->maskBmp = -1;
		return 0;
	}
	if(!BmpMgr_GetInfo(gDispBmpMgr, &info, bmp))
		return (int)0x80000005;
	if(info.mode != 3)
		return (int)0x80000006;
	DispObj_GetSize(&r->obj, &w, &h);
	if(w != info.w || h != info.h)
		return (int)0x80000006;
	r->maskBmp = bmp;
	r->maskGen = BmpMgr_Generation(gDispBmpMgr, bmp);
	return 0;
}

const DispObjVtbl_t RainScreen_Vtbl = {
	RainScreen_Destroy,
	DispObj_SetVisible,
	RainScreen_IsVisible,
	DispObj_Invalidate,
	RainScreen_Draw,
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
