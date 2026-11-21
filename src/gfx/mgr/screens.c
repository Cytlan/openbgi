/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * screens.c - the graphics manager's particle screen and rain screen
 *             operations: the "C0 0x" .. "C0 2x" (particles) and "C0 4x"
 *             (rain) instructions (src/include/bgi/gfx/gfxmgr.h)
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
void Gfx_UpdateParticles(OpenBGI_t* o, Gfx_t* g)
{
	int i;
	for(i = 0; i < GFX_PARTICLES; i++)
		if(g->particles[i])
			ParticleScreen_Update(o, (ParticleScreen_t*)g->particles[i]);
}

void Gfx_DeleteAllParticles(OpenBGI_t* o, Gfx_t* g)
{
	Mgr_DeleteAll(o, g->particles, GFX_PARTICLES, &g->particleCount, &g->particleNextId);
}

/* "C0 00": create a particle screen of w x h pixels in the first free
 * slot and return its handle in *outHandle.  0 ok, 1 the 8 slots are
 * taken, 2 the size is rejected (the screen is destroyed again). */
int Gfx_ParticleCreate(OpenBGI_t* o, Gfx_t* g, uint32_t* outHandle, int w, int h)
{
	ParticleScreen_t* p;
	uint32_t i;

	if(g->particleCount >= GFX_PARTICLES)
		return 1;
	i = Mgr_FreeSlot(g->particles);
	p = (ParticleScreen_t*)BGI_Calloc(sizeof(ParticleScreen_t));
	ParticleScreen_Ctor(o, p, g->particleNextId++);
	if(!ParticleScreen_SetSize(o, p, w, h))
	{
		p->obj.vt->destroy(o, &p->obj, 1);
		return 2;
	}
	g->particles[i] = &p->obj;
	g->particleCount++;
	Compositor_Add(o, g->comp, &p->obj);
	*outHandle = H_PARTICLE | i;
	return 0;
}

// "C0 01": 1 ok, 0 unknown handle
int Gfx_ParticleDelete(OpenBGI_t* o, Gfx_t* g, uint32_t h)
{
	return Mgr_DeleteFromTable(o, g, g->particles, Gfx_FindParticle(g, h), h, &g->particleCount, 0);
}

// "C0 04": show / hide; 1 ok, 0 unknown handle
int Gfx_ParticleShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on)
{
	return Mgr_ShowBracket(o, Gfx_FindParticle(g, h), on);
}

// "C0 05": position, effect, level and priority; 1 ok, 0 unknown handle
int Gfx_ParticleSetDraw(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int effect, int level, int prio)
{
	DispObj_t* obj = Gfx_FindParticle(g, h);

	if(!obj)
		return 0;
	InvalidateIfVisible(o, obj);
	ParticleScreen_SetDraw(o, (ParticleScreen_t*)obj, x, y, effect, level, prio);
	InvalidateIfVisible(o, obj);
	Compositor_Resort(o, g->comp, obj);
	return 1;
}

// "C0 08": repaint the surface and dirty what changed; 1 ok, 0 unknown handle
int Gfx_ParticleRedraw(OpenBGI_t* o, Gfx_t* g, uint32_t h)
{
	DispObj_t* obj = Gfx_FindParticle(g, h);

	if(obj)
		ParticleScreen_Redraw(o, (ParticleScreen_t*)obj);
	return obj != NULL;
}

// "C0 0A": from this many dirty rectangles on a redraw dirties the whole layer instead; 1 ok, 0 unknown handle
int Gfx_ParticleSetRectLimit(Gfx_t* g, uint32_t h, int n)
{
	DispObj_t* obj = Gfx_FindParticle(g, h);

	if(obj)
		ParticleScreen_SetRectLimit((ParticleScreen_t*)obj, n);
	return obj != NULL;
}

/* "C0 0B": the engine's camera (position, angles, distance) and the
 * projection centre (cx, cy) on the surface.  0 ok, 3 `dist` is not
 * positive, 0xFF unknown handle. */
int Gfx_ParticleSetCamera(Gfx_t* g, uint32_t h, int32_t x, int32_t y, int32_t z, int32_t a, int32_t b,
	int32_t c, int32_t dist, int cx, int cy)
{
	DispObj_t* obj = Gfx_FindParticle(g, h);

	if(!obj)
		return NO_OBJECT;
	return ParticleScreen_SetCamera((ParticleScreen_t*)obj, x, y, z, a, b, c, dist, cx, cy) ? 0 : 3;
}

// "C0 0C": the simulation tick in milliseconds; 0 ok, 4 rejected (not 0..100), 0xFF unknown handle
int Gfx_ParticleSetInterval(Gfx_t* g, uint32_t h, int ms)
{
	DispObj_t* obj = Gfx_FindParticle(g, h);

	if(!obj)
		return NO_OBJECT;
	return ParticleScreen_SetInterval((ParticleScreen_t*)obj, ms) ? 0 : 4;
}

// "C0 0D": run the simulation `ms` milliseconds ahead without drawing; 1 ok, 0 unknown handle
int Gfx_ParticlePrerun(OpenBGI_t* o, Gfx_t* g, uint32_t h, int ms)
{
	DispObj_t* obj = Gfx_FindParticle(g, h);

	if(obj)
		ParticleScreen_Prerun(o, (ParticleScreen_t*)obj, ms);
	return obj != NULL;
}

// "C0 0F": remove every particle; 1 ok, 0 unknown handle
int Gfx_ParticleClear(OpenBGI_t* o, Gfx_t* g, uint32_t h)
{
	DispObj_t* obj = Gfx_FindParticle(g, h);

	if(obj)
		ParticleScreen_Clear(o, (ParticleScreen_t*)obj);
	return obj != NULL;
}

// "C0 10": the 11 wind values; 1 ok, 0 unknown handle
int Gfx_ParticleSetWind(OpenBGI_t* o, Gfx_t* g, uint32_t h, const int32_t args[11])
{
	DispObj_t* obj = Gfx_FindParticle(g, h);

	if(obj)
		ParticleScreen_SetWind(o, (ParticleScreen_t*)obj, args);
	return obj != NULL;
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
int Gfx_ParticleSetGroupA(OpenBGI_t* o, Gfx_t* g, uint32_t h, int pattern, int max, int spawnEvery)
{
	DispObj_t* obj = Gfx_FindParticle(g, h);

	if(!obj)
		return NO_OBJECT;
	return GroupResult((uint32_t)ParticleScreen_SetGroupA(o, (ParticleScreen_t*)obj, pattern, max, spawnEvery), h);
}

// "C0 28": the same for a type 1 group
int Gfx_ParticleSetGroupB(OpenBGI_t* o, Gfx_t* g, uint32_t h, int pattern, int max, int spawnEvery)
{
	DispObj_t* obj = Gfx_FindParticle(g, h);

	if(!obj)
		return NO_OBJECT;
	return GroupResult((uint32_t)ParticleScreen_SetGroupB(o, (ParticleScreen_t*)obj, pattern, max, spawnEvery), h);
}

// "C0 29" of 1.492 on: 14 more values of a type 1 pattern; 0 ok, 5 bad pattern, 0xFF no screen
int Gfx_ParticleSetPatternBExtra(OpenBGI_t* o, Gfx_t* g, uint32_t h, int pattern, const int32_t v[14])
{
	DispObj_t* obj = Gfx_FindParticle(g, h);
	uint32_t r;
	if(!obj)
		return NO_OBJECT;
	r = ParticleEngine_SetPatternBExtra(o, ((ParticleScreen_t*)obj)->engine, pattern, v);
	return r == 0x80000002u ? 5 : (int)r;
}

// ---- rain screens ------------------------------------------------------------------------------

// advance every rain screen's generator (once per scheduler pass)
void Gfx_UpdateRains(OpenBGI_t* o, Gfx_t* g)
{
	int i;
	for(i = 0; i < GFX_RAINS; i++)
		if(g->rains[i])
			RainScreen_Update(o, (RainScreen_t*)g->rains[i]);
}

/* redraw the rain screens for an effect frame; 1 when one was redrawn,
 * 0 when none could be.  Only one screen is redrawn per call: once a
 * redraw succeeded the remaining screens are skipped, while a failed
 * redraw (no surface or no generator yet) lets the next screen try. */
int Gfx_RedrawRains(OpenBGI_t* o, Gfx_t* g)
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
		done = RainScreen_Redraw(o, (RainScreen_t*)g->rains[i]) == 0;
	}
	return done;
}

void Gfx_DeleteAllRains(OpenBGI_t* o, Gfx_t* g)
{
	Mgr_DeleteAll(o, g->rains, GFX_RAINS, &g->rainCount, &g->rainNextId);
}

/* "C0 40": create a rain screen of w x h pixels in the first free slot
 * and return its handle in *outHandle.  0 ok, 1 the 8 slots are taken, 3
 * the size is rejected (RainScreen_SetSize returns 0 on success; the
 * screen is destroyed again). */
int Gfx_RainCreate(OpenBGI_t* o, Gfx_t* g, uint32_t* outHandle, int w, int h)
{
	RainScreen_t* r;
	uint32_t i;

	if(g->rainCount >= GFX_RAINS)
		return 1;
	i = Mgr_FreeSlot(g->rains);
	r = (RainScreen_t*)BGI_Calloc(sizeof(RainScreen_t));
	RainScreen_Ctor(o, r, g->rainNextId++);
	if(RainScreen_SetSize(o, r, w, h) != 0)
	{
		r->obj.vt->destroy(o, &r->obj, 1);
		return 3;
	}
	g->rains[i] = &r->obj;
	g->rainCount++;
	Compositor_Add(o, g->comp, &r->obj);
	*outHandle = H_RAIN | i;
	return 0;
}

// "C0 41": 1 ok, 0 unknown handle
int Gfx_RainDelete(OpenBGI_t* o, Gfx_t* g, uint32_t h)
{
	return Mgr_DeleteFromTable(o, g, g->rains, Gfx_FindRain(g, h), h, &g->rainCount, 0);
}

// "C0 42": start a fresh generator with the screen's values, run `prerollMs` ahead; 1 ok, 0 unknown handle
int Gfx_RainStart(OpenBGI_t* o, Gfx_t* g, uint32_t h, uint32_t prerollMs)
{
	DispObj_t* obj = Gfx_FindRain(g, h);

	if(obj)
		RainScreen_Start(o, (RainScreen_t*)obj, prerollMs);
	return obj != NULL;
}

/* "C0 43": a screen-sized grayscale mask the rain shows through (-1
 * removes it).  0 ok, 4 no such bitmap (0x80000005), 5 wrong mode or size
 * (0x80000006), 0xFF unknown handle or any other result. */
int Gfx_RainSetMask(OpenBGI_t* o, Gfx_t* g, uint32_t h, int bmp)
{
	DispObj_t* obj = Gfx_FindRain(g, h);
	uint32_t r;

	if(!obj)
		return NO_OBJECT;
	r = (uint32_t)RainScreen_SetMask(o, (RainScreen_t*)obj, bmp);
	if(r == 0)
	{
		InvalidateIfVisible(o, obj);
		return 0;
	}
	if(r == 0x80000005u)
		return 4;
	if(r == 0x80000006u)
		return 5;
	return NO_OBJECT;
}

// "C0 44": show / hide; 1 ok, 0 unknown handle
int Gfx_RainShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on)
{
	return Mgr_ShowBracket(o, Gfx_FindRain(g, h), on);
}

// "C0 45": position, effect, level and priority; 0 ok, 0xFF unknown handle
int Gfx_RainSetDraw(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int effect, int level, int prio)
{
	DispObj_t* obj = Gfx_FindRain(g, h);

	if(!obj)
		return NO_OBJECT;
	InvalidateIfVisible(o, obj);
	RainScreen_SetDraw(o, (RainScreen_t*)obj, x, y, effect, level, prio);
	InvalidateIfVisible(o, obj);
	Compositor_Resort(o, g->comp, obj);
	return 0;
}

/* the mapping shared by the rain parameter setters: 0 ok (repaint when
 * visible), 2 the value is stored but there is no generator yet
 * (0x80000002), 3 the value is rejected (0x80000004: zero where not
 * allowed); anything else returns the handle */
static int RainResult(OpenBGI_t* o, DispObj_t* obj, uint32_t r, uint32_t h)
{
	switch(r)
	{
		case 0:
			InvalidateIfVisible(o, obj);
			return 0;
		case 0x80000002u: return 2;
		case 0x80000004u: return 3;
		default: return (int)h;
	}
}

// "C0 46": the spawn box of the drops (x and z ranges, spawn height, death height); the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetArea(OpenBGI_t* o, Gfx_t* g, uint32_t h, int xMin, int yTop, int zMin, int xMax, int yBottom, int zMax)
{
	DispObj_t* obj = Gfx_FindRain(g, h);
	if(!obj)
		return NO_OBJECT;
	return RainResult(o, obj, (uint32_t)RainScreen_SetArea((RainScreen_t*)obj, xMin, yTop, zMin, xMax, yBottom, zMax), h);
}

// "C0 47": the distance a drop falls per stage (24.8, not 0); the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetFall(OpenBGI_t* o, Gfx_t* g, uint32_t h, int n)
{
	DispObj_t* obj = Gfx_FindRain(g, h);
	if(!obj)
		return NO_OBJECT;
	return RainResult(o, obj, (uint32_t)RainScreen_SetFall((RainScreen_t*)obj, n), h);
}

// "C0 48": the streak length (24.8, not 0); the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetLength(OpenBGI_t* o, Gfx_t* g, uint32_t h, int n)
{
	DispObj_t* obj = Gfx_FindRain(g, h);
	if(!obj)
		return NO_OBJECT;
	return RainResult(o, obj, (uint32_t)RainScreen_SetLength((RainScreen_t*)obj, n), h);
}

// "C0 49": the streak colour (0xAARRGGBB; the alpha fades with depth); the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetColour(OpenBGI_t* o, Gfx_t* g, uint32_t h, uint32_t argb)
{
	DispObj_t* obj = Gfx_FindRain(g, h);
	if(!obj)
		return NO_OBJECT;
	return RainResult(o, obj, (uint32_t)RainScreen_SetColour((RainScreen_t*)obj, argb), h);
}

// "C0 4A": the drops born per stage; the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetPerStage(OpenBGI_t* o, Gfx_t* g, uint32_t h, int n)
{
	DispObj_t* obj = Gfx_FindRain(g, h);
	if(!obj)
		return NO_OBJECT;
	return RainResult(o, obj, (uint32_t)RainScreen_SetPerStage((RainScreen_t*)obj, n), h);
}

// "C0 4B": the stage length in milliseconds (not 0); the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetStageMs(OpenBGI_t* o, Gfx_t* g, uint32_t h, int n)
{
	DispObj_t* obj = Gfx_FindRain(g, h);
	if(!obj)
		return NO_OBJECT;
	return RainResult(o, obj, (uint32_t)RainScreen_SetStageMs((RainScreen_t*)obj, n), h);
}

// "C0 4C": the camera position; the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetCamera(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int z)
{
	DispObj_t* obj = Gfx_FindRain(g, h);
	if(!obj)
		return NO_OBJECT;
	return RainResult(o, obj, (uint32_t)RainScreen_SetCamera((RainScreen_t*)obj, x, y, z), h);
}

// "C0 4D": the camera angles (0.1 degree units); the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetAngles(OpenBGI_t* o, Gfx_t* g, uint32_t h, int a, int b, int c)
{
	DispObj_t* obj = Gfx_FindRain(g, h);
	if(!obj)
		return NO_OBJECT;
	return RainResult(o, obj, (uint32_t)RainScreen_SetAngles((RainScreen_t*)obj, a, b, c), h);
}

// "C0 4E": the projection distance and near plane (not 0); the codes of RainResult, 0xFF unknown handle
int Gfx_RainSetDistance(OpenBGI_t* o, Gfx_t* g, uint32_t h, int n)
{
	DispObj_t* obj = Gfx_FindRain(g, h);
	if(!obj)
		return NO_OBJECT;
	return RainResult(o, obj, (uint32_t)RainScreen_SetDistance((RainScreen_t*)obj, n), h);
}
