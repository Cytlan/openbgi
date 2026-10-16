/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * particle_screen.c - the particle screen display object: a ParticleEngine
 *                     (src/gfx/particle/) drawn into its own surface and
 *                     composed like a sprite.  Interface:
 *                     inc/bgi/gfx/screens.h.
 *
 * The screen is reached through the graphics manager (src/gfx/mgr/screens.c)
 * by the "C0 0x" .. "C0 2x" instructions, which set its camera, tick
 * length, wind and particle groups; the pattern tables the particles draw
 * from are global (src/gfx/particle/patterns.c).  Every scheduler pass
 * brings the engine up to date (ParticleScreen_Update); the surface is
 * repainted only on "C0 08" or by the automatic redraw of "C0 09".  The
 * screen keeps two lists of the rectangles the particles touched (the
 * previous and the current frame) so that a redraw dirties only the pixels
 * that changed.
 */
#include <string.h>

#include "bgi/common.h"
#include "bgi/gfx/screens.h"

// a display object of class order 4 with its own engine and no surface yet (ParticleScreen_SetSize makes one)
void ParticleScreen_Ctor(ParticleScreen_t* p, int slotId)
{
	DispObj_Ctor(&p->obj, 4, slotId);
	p->obj.vt = &ParticleScreen_Vtbl;
	p->engine = ParticleEngine_New();
	memset(&p->surf, 0, sizeof p->surf);
	p->cx = p->cy = 0;
	p->rectLimit = 0x1000;
	p->flip = 0;
	p->rectCount[0] = p->rectCount[1] = 0;
}

void ParticleScreen_Dtor(ParticleScreen_t* p)
{
	p->obj.vt = &ParticleScreen_Vtbl;
	if(p->engine)
		ParticleEngine_Delete(p->engine);
	BGI_Free(p->surf.pixels);
	DispObj_Dtor(&p->obj);
}

static void ParticleScreen_Destroy(DispObj_t* o, int flags)
{
	ParticleScreen_Dtor((ParticleScreen_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

// once per scheduler pass: bring the engine up to the present
void ParticleScreen_Update(ParticleScreen_t* p)
{
	ParticleEngine_Update(p->engine);
}

// "C0 0D": run `ms` milliseconds of simulation ahead of time
void ParticleScreen_Prerun(ParticleScreen_t* p, int ms)
{
	ParticleEngine_Prerun(p->engine, ms);
}

// the compositor's draw: a plain blit of the surface with the object's effect and level
static void ParticleScreen_Draw(DispObj_t* o, Bmp_t* dst, const Rect_t* local, uint32_t minKey)
{
	ParticleScreen_t* p = (ParticleScreen_t*)o;
	Bmp_t src;
	(void)minKey;
	if(!p->surf.pixels)
		return;
	src = p->surf;
	Bmp_Crop(&src, local);
	Bmp_BlitEffect(dst, &src, DispObj_GetEffect(o), DispObj_EffectiveLevel(o));
}

// "C0 05": position, effect, level and priority in one call
void ParticleScreen_SetDraw(ParticleScreen_t* p, int x, int y, int effect, int level, int prio)
{
	p->obj.vt->setPos(&p->obj, x, y);
	DispObj_SetEffect(&p->obj, effect);
	p->obj.vt->setLevel(&p->obj, level);
	DispObj_SetPriority(&p->obj, (uint32_t)prio);
}

/* "C0 00": size the object and allocate its surface (the back buffer's
 * mode with alpha); the projection centre becomes the middle of the
 * surface.  1 ok, 0 when the object rejects the size (the old surface
 * stays). */
int ParticleScreen_SetSize(ParticleScreen_t* p, int w, int h)
{
	int r = p->obj.vt->setSize(&p->obj, w, h);
	if(r)
	{
		BGI_Free(p->surf.pixels);
		Bmp_AllocScreen(&p->surf, w, h, 1);
		Bmp_Clear(&p->surf, NULL);
		p->cx = (int32_t)((uint32_t)w >> 1);
		p->cy = (int32_t)((uint32_t)h >> 1);
	}
	return r;
}

// "C0 0A": from this many dirty rectangles on, a redraw dirties the whole layer instead
void ParticleScreen_SetRectLimit(ParticleScreen_t* p, int limit)
{
	p->rectLimit = limit;
}

/* "C0 08" (and the automatic redraw of "C0 09"): clear the surface,
 * render the particles into it and dirty where they were in the previous
 * frame and where they are now; when the two lists together reach the
 * rectangle limit the whole layer is invalidated instead.  Nothing happens
 * without a surface. */
void ParticleScreen_Redraw(ParticleScreen_t* p)
{
	int cur = p->flip, k, i;
	if(!p->surf.pixels)
		return;
	Bmp_Clear(&p->surf, NULL);
	ParticleEngine_Render(p->engine, &p->surf, p->cx, p->cy, &p->rectCount[cur], p->rects[cur]);
	if(p->rectCount[0] + p->rectCount[1] < p->rectLimit)
	{
		uint32_t key = p->obj.vt->sortKey(&p->obj);
		int32_t pos[2];
		p->obj.vt->getPos(&p->obj, pos);
		for(k = 0; k < 2; k++)
		{
			for(i = 0; i < p->rectCount[k]; i++)
			{
				Rect_t r = p->rects[k][i];
				Rect_Offset(&r, pos[0], pos[1]);
				Gfx_AddDirty(gDispGfx, key, &r);
			}
		}
	}
	else
	{
		p->obj.vt->invalidate(&p->obj);
	}
	p->flip ^= 1;
}

/* "C0 0B": the engine's camera (see ParticleEngine_SetCamera) and the
 * projection centre (cx, cy) on the surface.  1 ok, 0 when `dist` is not
 * positive; the centre is stored either way. */
int ParticleScreen_SetCamera(ParticleScreen_t* p, int32_t x, int32_t y, int32_t z, int32_t a, int32_t b,
	int32_t c, int32_t dist, int cx, int cy)
{
	p->cx = cx;
	p->cy = cy;
	return ParticleEngine_SetCamera(p->engine, x, y, z, a, b, c, dist);
}

int ParticleScreen_SetInterval(ParticleScreen_t* p, int ms) // "C0 0C": the tick length, 0 .. 100 ms; 0 when rejected
{
	return ParticleEngine_SetInterval(p->engine, ms);
}

void ParticleScreen_Clear(ParticleScreen_t* p) // "C0 0F": remove every particle
{
	ParticleEngine_Clear(p->engine);
}

int ParticleScreen_SetWind(ParticleScreen_t* p, const int32_t args[11]) // "C0 10": the 11 wind values; always 1
{
	return ParticleEngine_SetWind(p->engine, args);
}

// "C0 20" / "C0 28": a type 0 / type 1 group: 0 ok, 0x80000002 bad pattern, 0x80000003 bad maximum
int ParticleScreen_SetGroupA(ParticleScreen_t* p, int pattern, int max, int spawnEvery)
{
	return ParticleEngine_SetGroupA(p->engine, pattern, max, spawnEvery);
}

int ParticleScreen_SetGroupB(ParticleScreen_t* p, int pattern, int max, int spawnEvery)
{
	return ParticleEngine_SetGroupB(p->engine, pattern, max, spawnEvery);
}

const DispObjVtbl_t ParticleScreen_Vtbl = {
	ParticleScreen_Destroy,
	DispObj_SetVisible,
	DispObj_IsVisible,
	DispObj_Invalidate,
	ParticleScreen_Draw,
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
