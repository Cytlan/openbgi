/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * screens.c - the graphics manager's particle screen and rain screen
 *             operations: the "C0 0x" .. "C0 2x" (particles) and "C0 4x"
 *             (rain) instructions (inc/bgi/gfx/gfxmgr.h)
 *
 * Both are display objects with a surface of their own that a simulation
 * repaints: a particle screen runs a particle engine, a rain screen a
 * rain generator.  Eight slots each, addressed by H_PARTICLE | index and
 * H_RAIN | index.  The simulations advance once per scheduler pass through
 * Gfx_UpdateParticles / Gfx_UpdateRains; the rain screens survive
 * Gfx_Reset.
 */
#include "mgr_internal.h"

// ---- particle screens --------------------------------------------------------------------------

// advance every particle screen's simulation (once per scheduler pass)
void Gfx_UpdateParticles(Gfx_t* g)
{
	int i;
	for(i = 0; i < GFX_PARTICLES; i++)
		if(g->particles[i])
			ParticleScreen_Update((ParticleScreen_t*)g->particles[i]);
}

void Gfx_DeleteAllParticles(Gfx_t* g)
{
	Mgr_DeleteAll(g->particles, GFX_PARTICLES, &g->particleCount, &g->particleNextId);
}

/* "C0 00": create a particle screen of w x h pixels in the first free
 * slot and return its handle in *outHandle.  0 ok, 1 the 8 slots are
 * taken, 2 the size is rejected (the screen is destroyed again). */
int Gfx_ParticleCreate(Gfx_t* g, uint32_t* outHandle, int w, int h)
{
	ParticleScreen_t* p;
	uint32_t i;

	if(g->particleCount >= GFX_PARTICLES)
		return 1;
	i = Mgr_FreeSlot(g->particles);
	p = (ParticleScreen_t*)BGI_Calloc(sizeof(ParticleScreen_t));
	ParticleScreen_Ctor(p, g->particleNextId++);
	if(!ParticleScreen_SetSize(p, w, h))
	{
		p->obj.vt->destroy(&p->obj, 1);
		return 2;
	}
	g->particles[i] = &p->obj;
	g->particleCount++;
	Compositor_Add(g->comp, &p->obj);
	*outHandle = H_PARTICLE | i;
	return 0;
}

// "C0 01": 1 ok, 0 unknown handle
int Gfx_ParticleDelete(Gfx_t* g, uint32_t h)
{
	return Mgr_DeleteFromTable(g, g->particles, Gfx_FindParticle(g, h), h, &g->particleCount, 0);
}

// "C0 04": show / hide; 1 ok, 0 unknown handle
int Gfx_ParticleShow(Gfx_t* g, uint32_t h, int on)
{
	return Mgr_ShowBracket(Gfx_FindParticle(g, h), on);
}

// "C0 05": position, effect, level and priority; 1 ok, 0 unknown handle
int Gfx_ParticleSetDraw(Gfx_t* g, uint32_t h, int x, int y, int effect, int level, int prio)
{
	DispObj_t* o = Gfx_FindParticle(g, h);

	if(!o)
		return 0;
	InvalidateIfVisible(o);
	ParticleScreen_SetDraw((ParticleScreen_t*)o, x, y, effect, level, prio);
	InvalidateIfVisible(o);
	Compositor_Resort(g->comp, o);
	return 1;
}

// "C0 08": repaint the surface and dirty what changed; 1 ok, 0 unknown handle
int Gfx_ParticleRedraw(Gfx_t* g, uint32_t h)
{
	DispObj_t* o = Gfx_FindParticle(g, h);

	if(o)
		ParticleScreen_Redraw((ParticleScreen_t*)o);
	return o != NULL;
}

// "C0 0A": from this many dirty rectangles on a redraw dirties the whole layer instead; 1 ok, 0 unknown handle
int Gfx_ParticleSetRectLimit(Gfx_t* g, uint32_t h, int n)
{
	DispObj_t* o = Gfx_FindParticle(g, h);

	if(o)
		ParticleScreen_SetRectLimit((ParticleScreen_t*)o, n);
	return o != NULL;
}

/* "C0 0B": the engine's camera (position, angles, distance) and the
 * projection centre (cx, cy) on the surface.  0 ok, 3 `dist` is not
 * positive, 0xFF unknown handle. */
int Gfx_ParticleSetCamera(Gfx_t* g, uint32_t h, int32_t x, int32_t y, int32_t z, int32_t a, int32_t b,
	int32_t c, int32_t dist, int cx, int cy)
{
	DispObj_t* o = Gfx_FindParticle(g, h);

	if(!o)
		return NO_OBJECT;
	return ParticleScreen_SetCamera((ParticleScreen_t*)o, x, y, z, a, b, c, dist, cx, cy) ? 0 : 3;
}

// "C0 0C": the simulation tick in milliseconds; 0 ok, 4 rejected (not 0..100), 0xFF unknown handle
int Gfx_ParticleSetInterval(Gfx_t* g, uint32_t h, int ms)
{
	DispObj_t* o = Gfx_FindParticle(g, h);

	if(!o)
		return NO_OBJECT;
	return ParticleScreen_SetInterval((ParticleScreen_t*)o, ms) ? 0 : 4;
}

// "C0 0D": run the simulation `ms` milliseconds ahead without drawing; 1 ok, 0 unknown handle
int Gfx_ParticlePrerun(Gfx_t* g, uint32_t h, int ms)
{
	DispObj_t* o = Gfx_FindParticle(g, h);

	if(o)
		ParticleScreen_Prerun((ParticleScreen_t*)o, ms);
	return o != NULL;
}

// "C0 0F": remove every particle; 1 ok, 0 unknown handle
int Gfx_ParticleClear(Gfx_t* g, uint32_t h)
{
	DispObj_t* o = Gfx_FindParticle(g, h);

	if(o)
		ParticleScreen_Clear((ParticleScreen_t*)o);
	return o != NULL;
}

// "C0 10": the 11 wind values; 1 ok, 0 unknown handle
int Gfx_ParticleSetWind(Gfx_t* g, uint32_t h, const int32_t args[11])
{
	DispObj_t* o = Gfx_FindParticle(g, h);

	if(o)
		ParticleScreen_SetWind((ParticleScreen_t*)o, args);
	return o != NULL;
}

// the two group setters share the mapping: 0 ok, 5 bad pattern (0x80000002), 6 bad maximum (0x80000003)
static int GroupResult(uint32_t r, uint32_t h)
{
	switch(r)
	{
		case 0: return 0;
		case 0x80000002u: return 5;
		case 0x80000003u: return 6;
		default: return (int)h;
	}
}

// "C0 20": a type 0 particle group of `pattern`, at most `max` particles, one spawned every `spawnEvery`; the codes of GroupResult, 0xFF unknown handle
int Gfx_ParticleSetGroupA(Gfx_t* g, uint32_t h, int pattern, int max, int spawnEvery)
{
	DispObj_t* o = Gfx_FindParticle(g, h);

	if(!o)
		return NO_OBJECT;
	return GroupResult((uint32_t)ParticleScreen_SetGroupA((ParticleScreen_t*)o, pattern, max, spawnEvery), h);
}

// "C0 28": the same for a type 1 group
int Gfx_ParticleSetGroupB(Gfx_t* g, uint32_t h, int pattern, int max, int spawnEvery)
{
	DispObj_t* o = Gfx_FindParticle(g, h);

	if(!o)
		return NO_OBJECT;
	return GroupResult((uint32_t)ParticleScreen_SetGroupB((ParticleScreen_t*)o, pattern, max, spawnEvery), h);
}

// "C0 29" of 1.494 on: 14 more values of a type 1 pattern; 0 ok, 5 bad pattern, 0xFF no screen
int Gfx_ParticleSetPatternBExtra(Gfx_t* g, uint32_t h, int pattern, const int32_t v[14])
{
	DispObj_t* o = Gfx_FindParticle(g, h);
	uint32_t r;
	if(!o)
		return NO_OBJECT;
	r = ParticleEngine_SetPatternBExtra(((ParticleScreen_t*)o)->engine, pattern, v);
	return r == 0x80000002u ? 5 : (int)r;
}

// ---- rain screens ------------------------------------------------------------------------------

// advance every rain screen's generator (once per scheduler pass)
void Gfx_UpdateRains(Gfx_t* g)
{
	int i;
	for(i = 0; i < GFX_RAINS; i++)
		if(g->rains[i])
			RainScreen_Update((RainScreen_t*)g->rains[i]);
}

/* redraw the rain screens for an effect frame; 1 when one was redrawn,
 * 0 when none could be.  Only one screen is redrawn per call: once a
 * redraw succeeded the remaining screens are skipped, while a failed
 * redraw (no surface or no generator yet) lets the next screen try. */
int Gfx_RedrawRains(Gfx_t* g)
{
	int i, done = 0;
	for(i = 0; i < GFX_RAINS; i++)
	{
		if(!g->rains[i])
			continue;
		if(done)
			continue;
		// the original stops at the first screen that redraws, so only one
		// rain screen is redrawn per frame
		done = RainScreen_Redraw((RainScreen_t*)g->rains[i]) == 0;
	}
	return done;
}

void Gfx_DeleteAllRains(Gfx_t* g)
{
	Mgr_DeleteAll(g->rains, GFX_RAINS, &g->rainCount, &g->rainNextId);
}

/* "C0 40": create a rain screen of w x h pixels in the first free slot
 * and return its handle in *outHandle.  0 ok, 1 the 8 slots are taken, 3
 * the size is rejected (RainScreen_SetSize returns 0 on success; the
 * screen is destroyed again). */
int Gfx_RainCreate(Gfx_t* g, uint32_t* outHandle, int w, int h)
{
	RainScreen_t* r;
	uint32_t i;

	if(g->rainCount >= GFX_RAINS)
		return 1;
	i = Mgr_FreeSlot(g->rains);
	r = (RainScreen_t*)BGI_Calloc(sizeof(RainScreen_t));
	RainScreen_Ctor(r, g->rainNextId++);
	if(RainScreen_SetSize(r, w, h) != 0)
	{
		r->obj.vt->destroy(&r->obj, 1);
		return 3;
	}
	g->rains[i] = &r->obj;
	g->rainCount++;
	Compositor_Add(g->comp, &r->obj);
	*outHandle = H_RAIN | i;
	return 0;
}

// "C0 41": 1 ok, 0 unknown handle
int Gfx_RainDelete(Gfx_t* g, uint32_t h)
{
	return Mgr_DeleteFromTable(g, g->rains, Gfx_FindRain(g, h), h, &g->rainCount, 0);
}

// "C0 42": start a fresh generator with the screen's values, run `prerollMs` ahead; 1 ok, 0 unknown handle
int Gfx_RainStart(Gfx_t* g, uint32_t h, uint32_t prerollMs)
{
	DispObj_t* o = Gfx_FindRain(g, h);

	if(o)
		RainScreen_Start((RainScreen_t*)o, prerollMs);
	return o != NULL;
}

/* "C0 43": a screen-sized grayscale mask the rain shows through (-1
 * removes it).  0 ok, 4 no such bitmap (0x80000005), 5 wrong mode or size
 * (0x80000006), 0xFF unknown handle or any other result. */
int Gfx_RainSetMask(Gfx_t* g, uint32_t h, int bmp)
{
	DispObj_t* o = Gfx_FindRain(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	r = (uint32_t)RainScreen_SetMask((RainScreen_t*)o, bmp);
	if(r == 0)
	{
		InvalidateIfVisible(o);
		return 0;
	}
	if(r == 0x80000005u)
		return 4;
	if(r == 0x80000006u)
		return 5;
	return NO_OBJECT;
}

// "C0 44": show / hide; 1 ok, 0 unknown handle
int Gfx_RainShow(Gfx_t* g, uint32_t h, int on)
{
	return Mgr_ShowBracket(Gfx_FindRain(g, h), on);
}

// "C0 45": position, effect, level and priority; 0 ok, 0xFF unknown handle
int Gfx_RainSetDraw(Gfx_t* g, uint32_t h, int x, int y, int effect, int level, int prio)
{
	DispObj_t* o = Gfx_FindRain(g, h);

	if(!o)
		return NO_OBJECT;
	InvalidateIfVisible(o);
	RainScreen_SetDraw((RainScreen_t*)o, x, y, effect, level, prio);
	InvalidateIfVisible(o);
	Compositor_Resort(g->comp, o);
	return 0;
}

/* the mapping shared by the rain parameter setters: 0 ok (repaint when
 * visible), 2 the value is stored but there is no generator yet
 * (0x80000002), 3 the value is rejected (0x80000004: zero where not
 * allowed); anything else returns the handle */
static int RainResult(DispObj_t* o, uint32_t r, uint32_t h)
{
	switch(r)
	{
		case 0:
			InvalidateIfVisible(o);
			return 0;
		case 0x80000002u: return 2;
		case 0x80000004u: return 3;
		default: return (int)h;
	}
}

// "C0 46": the spawn box of the drops (x and z ranges, spawn height, death height); the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetArea(Gfx_t* g, uint32_t h, int xMin, int yTop, int zMin, int xMax, int yBottom, int zMax)
{
	DispObj_t* o = Gfx_FindRain(g, h);
	if(!o)
		return NO_OBJECT;
	return RainResult(o, (uint32_t)RainScreen_SetArea((RainScreen_t*)o, xMin, yTop, zMin, xMax, yBottom, zMax), h);
}

// "C0 47": the distance a drop falls per stage (24.8, not 0); the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetFall(Gfx_t* g, uint32_t h, int n)
{
	DispObj_t* o = Gfx_FindRain(g, h);
	if(!o)
		return NO_OBJECT;
	return RainResult(o, (uint32_t)RainScreen_SetFall((RainScreen_t*)o, n), h);
}

// "C0 48": the streak length (24.8, not 0); the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetLength(Gfx_t* g, uint32_t h, int n)
{
	DispObj_t* o = Gfx_FindRain(g, h);
	if(!o)
		return NO_OBJECT;
	return RainResult(o, (uint32_t)RainScreen_SetLength((RainScreen_t*)o, n), h);
}

// "C0 49": the streak colour (0xAARRGGBB; the alpha fades with depth); the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetColour(Gfx_t* g, uint32_t h, uint32_t argb)
{
	DispObj_t* o = Gfx_FindRain(g, h);
	if(!o)
		return NO_OBJECT;
	return RainResult(o, (uint32_t)RainScreen_SetColour((RainScreen_t*)o, argb), h);
}

// "C0 4A": the drops born per stage; the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetPerStage(Gfx_t* g, uint32_t h, int n)
{
	DispObj_t* o = Gfx_FindRain(g, h);
	if(!o)
		return NO_OBJECT;
	return RainResult(o, (uint32_t)RainScreen_SetPerStage((RainScreen_t*)o, n), h);
}

// "C0 4B": the stage length in milliseconds (not 0); the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetStageMs(Gfx_t* g, uint32_t h, int n)
{
	DispObj_t* o = Gfx_FindRain(g, h);
	if(!o)
		return NO_OBJECT;
	return RainResult(o, (uint32_t)RainScreen_SetStageMs((RainScreen_t*)o, n), h);
}

// "C0 4C": the camera position; the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetCamera(Gfx_t* g, uint32_t h, int x, int y, int z)
{
	DispObj_t* o = Gfx_FindRain(g, h);
	if(!o)
		return NO_OBJECT;
	return RainResult(o, (uint32_t)RainScreen_SetCamera((RainScreen_t*)o, x, y, z), h);
}

// "C0 4D": the camera angles (0.1 degree units); the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetAngles(Gfx_t* g, uint32_t h, int a, int b, int c)
{
	DispObj_t* o = Gfx_FindRain(g, h);
	if(!o)
		return NO_OBJECT;
	return RainResult(o, (uint32_t)RainScreen_SetAngles((RainScreen_t*)o, a, b, c), h);
}

// "C0 4E": the projection distance and near plane (not 0); the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetDistance(Gfx_t* g, uint32_t h, int n)
{
	DispObj_t* o = Gfx_FindRain(g, h);
	if(!o)
		return NO_OBJECT;
	return RainResult(o, (uint32_t)RainScreen_SetDistance((RainScreen_t*)o, n), h);
}
