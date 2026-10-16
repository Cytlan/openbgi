/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * engine.c - the particle engine: the particles of one screen, their
 *            births, motion and projection, and the automatic redraw list
 *            of "C0 09".  Interface: inc/bgi/gfx/screens.h (the
 *            ParticleEngine_* functions and Ptcl_SetAuto); the per-pass
 *            services Ptcl_UpdateAll / Ptcl_AutoTick / Ptcl_AutoClear are
 *            declared in bgi/gfx.h.
 *
 * The engine runs on its own clock: every `interval` milliseconds a tick
 * steps the wind and every particle and culls the dead and the ones that
 * left the world; after the ticks each (type, pattern) group bears the
 * particles that are due.  ParticleEngine_Update brings the engine up to
 * the present once per scheduler pass; ParticleEngine_Render projects the
 * live particles through the camera into the screen's surface.
 */
#include "particle_internal.h"

struct ParticleEngine
{
	uint32_t interval;                         // ms per tick, 10 by default, 0 = paused
	uint32_t lastTick;                         // the time the ticks have been run up to, ms (GetTicks)
	int32_t live;                              // particles in the slots
	Particle_t* slot[PTCL_MAX];                // the particles, NULL = free (owned)
	int32_t maxCount[2][PTCL_PATTERNS];        // per type and pattern: the most alive at a time
	int32_t liveCount[2][PTCL_PATTERNS];       // per type and pattern: alive now
	uint32_t spawnEvery[2][PTCL_PATTERNS];     // ms between births
	uint32_t nextSpawn[2][PTCL_PATTERNS];      // when the next birth is due; 0 = idle (the group is full)
	int32_t cam[3];                            // camera position, 24.8
	double sinA, cosA, sinB, cosB, sinC, cosC; // the three camera angles
	int32_t dist;                              // projection distance, > 0
	Wind_t* wind;                              // owned
	/* 1.494 on: the per-screen parameters of "C0 29" for the type 1
	 * particles, 14 values per pattern.  That build's type 1 particle is
	 * driven by them together with the global "C0 2D" pattern; this engine
	 * keeps the 1.69 type 1 motion, so they are only stored (see
	 * docs/versions.md). */
	int32_t patternBExtra[PTCL_PATTERNS][14];
};

/* "C0 29" (1.494 on): store the 14 extra values of a type 1 pattern.  The
 * 1st, 3rd and 13th are 24.8 (>> 8), the 2nd, 5th, 6th and 14th their
 * magnitudes >> 8, the 7th .. 9th magnitudes, the rest as given.  0 ok,
 * 0x80000002 for a pattern out of range. */
uint32_t ParticleEngine_SetPatternBExtra(ParticleEngine_t* e, int pattern, const int32_t v[14])
{
	int32_t* d;
	if((uint32_t)pattern >= PtclPatterns())
		return 0x80000002;
	d = e->patternBExtra[pattern];
	d[0] = v[0] >> 8;
	d[1] = (v[1] < 0 ? -v[1] : v[1]) >> 8;
	d[2] = v[2] >> 8;
	d[3] = v[3];
	d[4] = (v[4] < 0 ? -v[4] : v[4]) >> 8;
	d[5] = (v[5] < 0 ? -v[5] : v[5]) >> 8;
	d[6] = v[6] < 0 ? -v[6] : v[6];
	d[7] = v[7] < 0 ? -v[7] : v[7];
	d[8] = v[8] < 0 ? -v[8] : v[8];
	d[9] = v[9];
	d[10] = v[10];
	d[11] = v[11];
	d[12] = v[12] >> 8;
	d[13] = (v[13] < 0 ? -v[13] : v[13]) >> 8;
	return 0;
}

// an empty engine with a 10 ms tick, its clock at the present, no camera and a wind at rest
ParticleEngine_t* ParticleEngine_New(void)
{
	ParticleEngine_t* e = (ParticleEngine_t*)BGI_Alloc(sizeof *e);
	memset(e, 0, sizeof *e); // slots, counters, camera
	e->interval = 10;
	e->lastTick = GetTicks();
	e->wind = (Wind_t*)BGI_Alloc(sizeof *e->wind);
	Wind_Ctor(e->wind);
	return e;
}

// "C0 0F": destroy every particle and restart the birth schedule of every group; the groups and the camera stay
void ParticleEngine_Clear(ParticleEngine_t* e)
{
	int i;
	for(i = 0; i < PTCL_MAX; i++)
		if(e->slot[i])
		{
			e->slot[i]->vt->destroy(e->slot[i]);
			e->slot[i] = NULL;
		}
	e->live = 0;
	memset(e->liveCount, 0, sizeof e->liveCount);
	memset(e->nextSpawn, 0, sizeof e->nextSpawn);
}

void ParticleEngine_Delete(ParticleEngine_t* e)
{
	ParticleEngine_Clear(e);
	if(e->wind)
		BGI_Free(e->wind);
	BGI_Free(e);
}

// "C0 0C": the tick length, 0 .. 100 ms (0 pauses the engine); 0 for a value above 100
int ParticleEngine_SetInterval(ParticleEngine_t* e, int ms)
{
	if((uint32_t)ms > 100)
		return 0;
	e->interval = (uint32_t)ms;
	Wind_SetInterval(e->wind, (uint32_t)ms);
	return 1;
}

int ParticleEngine_SetWind(ParticleEngine_t* e, const int32_t args[11]) // "C0 10", see Wind_Set
{
	return Wind_Set(e->wind, args);
}

/* "C0 0B": the camera; x, y, z are 16.16 (stored as 24.8), the angles
 * 16.16 degrees, `dist` the projection distance.  1 ok, 0 when `dist` is
 * not positive (nothing changes). */
int ParticleEngine_SetCamera(ParticleEngine_t* e, int32_t x, int32_t y, int32_t z, int32_t a, int32_t b, int32_t c,
	int32_t dist)
{
	const double toRad = 2.663161090079238e-07; // pi / 180 / 65536 (16.16 degrees to radians)
	if(dist <= 0)
		return 0;
	e->cam[0] = x >> 8;
	e->cam[1] = y >> 8;
	e->cam[2] = z >> 8;
	e->dist = dist;
	e->sinA = sin(a * toRad);
	e->cosA = cos(a * toRad);
	e->sinB = sin(b * toRad);
	e->cosB = cos(b * toRad);
	e->sinC = sin(c * toRad);
	e->cosC = cos(c * toRad);
	return 1;
}

/* the births of a (type, pattern) group: at most `max` alive, one every
 * `spawnEvery` ms.  0 ok, 0x80000001 bad type, 0x80000002 bad pattern,
 * 0x80000003 max < 0 or more particles over all groups than the
 * generation allows (PtclMax: 4096 up to 1.69). */
static uint32_t ParticleEngine_SetGroup(ParticleEngine_t* e, uint32_t type, uint32_t pattern, int32_t max,
	uint32_t spawnEvery)
{
	int32_t sum = 0;
	uint32_t t, p;
	if(type >= 2)
		return 0x80000001;
	if(pattern >= PtclPatterns())
		return 0x80000002;
	for(t = 0; t < 2; t++)
		for(p = 0; p < PTCL_PATTERNS; p++)
			if(t != type || p != pattern)
				sum += e->maxCount[t][p];
	if(max < 0 || sum + max > PtclMax())
		return 0x80000003;
	e->maxCount[type][pattern] = max;
	e->spawnEvery[type][pattern] = spawnEvery;
	return 0;
}

int ParticleEngine_SetGroupA(ParticleEngine_t* e, int pattern, int max, int spawnEvery) // "C0 20": a type 0 group
{
	return (int)ParticleEngine_SetGroup(e, 0, (uint32_t)pattern, max, (uint32_t)spawnEvery);
}

int ParticleEngine_SetGroupB(ParticleEngine_t* e, int pattern, int max, int spawnEvery) // "C0 28": a type 1 group
{
	return (int)ParticleEngine_SetGroup(e, 1, (uint32_t)pattern, max, (uint32_t)spawnEvery);
}

// put a particle into the first free slot; 0 when the generation's maximum is reached
static int ParticleEngine_AddParticle(ParticleEngine_t* e, Particle_t* q)
{
	int i;
	if(e->live + 1 > PtclMax())
		return 0;
	for(i = 0; i < PTCL_MAX; i++)
		if(!e->slot[i])
		{
			e->slot[i] = q;
			e->live++;
			return 1;
		}
	return 0;
}

/* run the simulation ticks up to `now`: per tick step the wind and every
 * particle, then remove the dead and the ones outside the world.  *inSync
 * is cleared when the clock was more than 500 ms behind (the engine then
 * restarts from `now` instead of catching up).  0 when the engine is
 * paused. */
static int ParticleEngine_Tick(ParticleEngine_t* e, int* inSync, uint32_t now)
{
	int i;
	if(e->interval == 0)
		return 0;
	*inSync = !(e->lastTick + 500 < now);
	if(!*inSync)
		e->lastTick = now;
	if(e->lastTick > now)
		return 1;
	do
	{
		int32_t wv[3];
		Wind_Step(e->wind);
		Wind_Get(e->wind, wv);
		for(i = 0; i < PTCL_MAX; i++)
		{
			Particle_t* q = e->slot[i];
			int32_t x, y, z;
			int alive;
			if(!q)
				continue;
			Particle_ApplyForce(q, wv);
			alive = q->vt->step(q);
			x = q->x >> 8;
			y = q->y >> 8;
			z = q->z >> 8;
			// the world is |x| <= 16000, 0 <= y <= 16000, |z| <= 8000
			if(alive && x <= 16000 && x >= -16000 && y <= 16000 && y >= 0 && z <= 8000 && z >= -8000)
				continue;
			e->liveCount[q->type][q->pattern]--;
			q->vt->destroy(q);
			e->slot[i] = NULL;
			e->live--;
		}
		e->lastTick += e->interval;
	} while(e->lastTick <= now);
	return 1;
}

/* the births of one (type, pattern) group due by `now`, one per
 * `spawnEvery` ms; `reset` restarts the schedule at `now`.  The group
 * goes idle (nextSpawn 0) when it is full and resumes at the next call
 * that finds room. */
static void ParticleEngine_Spawn(ParticleEngine_t* e, uint32_t type, uint32_t pattern, uint32_t now, int reset)
{
	uint32_t* next;
	if(type >= 2 || pattern >= PtclPatterns())
		return;
	next = &e->nextSpawn[type][pattern];
	if(reset)
		*next = now;
	while(*next <= now)
	{
		Particle_t* q;
		if(e->liveCount[type][pattern] >= e->maxCount[type][pattern])
		{
			*next = 0;
			return;
		}
		q = type == 1 ? ParticleB_New((int)pattern) : ParticleA_New((int)pattern);
		if(!ParticleEngine_AddParticle(e, q))
			q->vt->destroy(q); // the original leaks the particle when every slot is taken
		e->liveCount[type][pattern]++;
		if(*next == 0)
			*next = now;
		*next += e->spawnEvery[type][pattern];
	}
}

// bring the engine up to `now`: the ticks, then the births of every group
static void ParticleEngine_UpdateTo(ParticleEngine_t* e, uint32_t now)
{
	int inSync = 1;
	uint32_t t, p;
	if(!ParticleEngine_Tick(e, &inSync, now))
		return;
	for(t = 0; t < 2; t++)
		for(p = 0; p < PTCL_PATTERNS; p++)
			ParticleEngine_Spawn(e, t, p, now, !inSync);
}

// once per scheduler pass: bring the engine up to the present
void ParticleEngine_Update(ParticleEngine_t* e)
{
	ParticleEngine_UpdateTo(e, GetTicks());
}

/* "C0 0D": advance the simulation by `ms` milliseconds one millisecond
 * at a time, then restore the clock and the birth schedule as they were,
 * so that the screen starts out populated.  Nothing happens while the
 * engine is paused. */
void ParticleEngine_Prerun(ParticleEngine_t* e, int ms)
{
	uint32_t saved[2][PTCL_PATTERNS];
	uint32_t lastTick, now, i;
	if(e->interval == 0)
		return;
	lastTick = e->lastTick;
	memcpy(saved, e->nextSpawn, sizeof saved);
	now = GetTicks();
	for(i = 1; i <= (uint32_t)ms; i++)
		ParticleEngine_UpdateTo(e, now + i);
	e->lastTick = lastTick;
	memcpy(e->nextSpawn, saved, sizeof saved);
}

typedef struct PtclVis // a projected particle; linked in order from far to near
{
	int32_t x, y, z;      // camera space, 24.8
	Particle_t* q;        // the particle
	struct PtclVis* next; // the next nearer one
} PtclVis_t;

/* draw the live particles into `dst` with the projection centre at (cx,
 * cy): transform every particle into camera space (rotate about x by A,
 * about y by B, about z by C, truncating after each rotation), drop the
 * ones behind the camera, sort from far to near and blit, for each, the
 * pre-scaled copy whose index follows from the depth, centred on its
 * projected point.  The rectangles touched go to outRects (one per blit
 * that succeeded, clipped to `dst`); *outCount and the result are their
 * number. */
static int ParticleEngine_RenderImpl(ParticleEngine_t* e, Bmp_t* dst, int cx, int cy, int32_t* outCount,
	Rect_t* outRects)
{
	static PtclVis_t vis[PTCL_MAX]; // too large for the stack at 0x8000 entries (the original keeps its 4096 there)
	PtclVis_t* head = NULL;
	Rect_t clip;
	int n = 0, count = 0, i;

	for(i = 0; i < PTCL_MAX; i++)
	{
		Particle_t* q = e->slot[i];
		int32_t x, y, z, t;
		if(!q)
			continue;
		x = q->x - e->cam[0];
		y = q->y - e->cam[1];
		z = q->z - e->cam[2];
		t = (int32_t)((double)y * e->cosA - (double)z * e->sinA);
		z = (int32_t)((double)y * e->sinA + (double)z * e->cosA);
		y = t;
		t = (int32_t)((double)z * e->cosB - (double)x * e->sinB);
		x = (int32_t)((double)x * e->cosB + (double)z * e->sinB);
		z = t;
		t = (int32_t)((double)x * e->cosC - (double)y * e->sinC);
		y = (int32_t)((double)x * e->sinC + (double)y * e->cosC);
		x = t;
		if(z < 0) // behind the camera
			continue;
		vis[n].x = x;
		vis[n].y = y;
		vis[n].z = z;
		vis[n].q = q;
		n++;
	}
	// insertion into a list sorted by decreasing z; equal depths keep their slot order
	for(i = 0; i < n; i++)
	{
		PtclVis_t** link = &head;
		while(*link && vis[i].z <= (*link)->z)
			link = &(*link)->next;
		vis[i].next = *link;
		*link = &vis[i];
	}

	Rect_FromBmp(&clip, dst);
	for(; head; head = head->next)
	{
		Particle_t* q = head->q;
		int32_t den = (e->dist << 8) + head->z;
		int idx = 32 - (e->dist * 32) / ((head->z >> 8) + e->dist); // the scale index: 0 = full size at depth 0
		const Bmp_t* b = q->vt->bitmap(q, idx);
		int sx, sy;
		Rect_t r;
		if(!b)
			continue;
		// 32-bit products, as the original
		sx = cx + (int32_t)((uint32_t)head->x * (uint32_t)e->dist) / den - (int)((uint32_t)b->w >> 1);
		sy = cy - (int32_t)((uint32_t)head->y * (uint32_t)e->dist) / den - (int)((uint32_t)b->h >> 1);
		if(Bmp_Blit(dst, sx, sy, b, q->effect, q->vt->level(q)) != 0)
			continue;
		Rect_FromBmp(&r, b);
		Rect_Offset(&r, sx, sy);
		Rect_Clip(&r, &clip);
		outRects[count++] = r;
	}
	*outCount = count;
	return count;
}

// render for ParticleScreen_Redraw: see ParticleEngine_RenderImpl
int ParticleEngine_Render(ParticleEngine_t* e, Bmp_t* dst, int cx, int cy, int32_t* outCount, Rect_t* outRects)
{
	return ParticleEngine_RenderImpl(e, dst, cx, cy, outCount, outRects);
}

// ---- the automatic redraw list ------------------------------------------------------------

typedef struct PtclAuto // a particle screen that redraws itself, 0x10 bytes
{
	uint32_t handle;       // the particle screen
	uint32_t interval;     // ms between redraws
	uint32_t next;         // when the next one is due (0 = now)
	struct PtclAuto* link; // the next entry
} PtclAuto_t;

static PtclAuto_t* gPtclAutoHead; // the registered screens, in order of registration

/* "C0 09": redraw the particle screen `handle` every `intervalMs`
 * milliseconds without the script's help; 0 unregisters it.  Returns 0
 * when the handle names no screen. */
int Ptcl_SetAuto(uint32_t handle, uint32_t intervalMs)
{
	PtclAuto_t **link, *n;
	if(!Gfx_FindParticle(gGfx, handle))
		return 0;
	for(link = &gPtclAutoHead; *link && (*link)->handle != handle; link = &(*link)->link)
		;
	n = *link;
	if(intervalMs > 0)
	{
		if(!n)
		{
			n = (PtclAuto_t*)BGI_Alloc(sizeof *n);
			n->handle = handle;
			n->next = GetTicks();
			n->link = NULL;
			*link = n;
		}
		n->interval = intervalMs;
	}
	else if(n)
	{
		*link = n->link;
		BGI_Free(n);
	}
	return 1;
}

// unregister every screen (a reboot of the machine, and engine shutdown)
void Ptcl_AutoClear(void)
{
	while(gPtclAutoHead)
		Ptcl_SetAuto(gPtclAutoHead->handle, 0);
}

/* once per scheduler pass: redraw every registered screen whose time has
 * come, provided it is visible and not below the draw limit of "90 09";
 * screens that no longer exist drop out of the list.  A redraw requests a
 * dirty present. */
void Ptcl_AutoTick(void)
{
	uint32_t now = GetTicks();
	uint32_t drawLimit = Gfx_GetDrawLimit(gGfx);
	PtclAuto_t** link = &gPtclAutoHead;
	int dirty = 0;
	while(*link)
	{
		PtclAuto_t* n = *link;
		DispObj_t* o;
		if(n->next > now)
		{
			link = &n->link;
			continue;
		}
		o = Gfx_FindParticle(gGfx, n->handle);
		if(!o)
		{
			*link = n->link;
			BGI_Free(n);
			continue;
		}
		if(o->vt->isVisible(o) && drawLimit <= o->vt->sortKey(o))
		{
			Gfx_ParticleRedraw(gGfx, n->handle);
			dirty = 1;
		}
		if(n->next == 0)
			n->next = now;
		n->next += ((now - n->next) / n->interval + 1) * n->interval; // the next grid point after now
		link = &n->link;
	}
	if(dirty)
		Present_RequestDirty();
}

// once per scheduler pass: bring every particle screen's engine up to the present
void Ptcl_UpdateAll(void)
{
	Gfx_UpdateParticles(gGfx);
}
