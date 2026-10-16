/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * types.c - the two particle types of the engine: the common base, type 0
 *           (constant velocity, lives until it leaves the world) and type 1
 *           (limited life, velocity wandering between random keys, fades in
 *           and out).  Interface: particle_internal.h.
 *
 * A particle is born by ParticleA_New / ParticleB_New from the global
 * pattern of its type (patterns.c) and then driven through its vtable by
 * the engine (engine.c): `step` once per tick, `bitmap` and `level` when
 * it is rendered, `destroy` when it dies or the engine is cleared.
 */
#include "particle_internal.h"

// the base: type and pattern, not yet alive; a valid pattern starts the frame animation
void Particle_Ctor(Particle_t* p, int type, int pattern)
{
	p->type = type;
	p->pattern = pattern;
	p->alive = 0;
	if((uint32_t)pattern < PtclPatterns())
		Particle_StartFrames(p);
	else
		p->framePos = p->frameRate = 0;
}

// the wind: pos += windWeight * vec >> 8 while alive
void Particle_ApplyForce(Particle_t* p, const int32_t vec[3])
{
	if(!p->alive)
		return;
	p->x += (p->windWeight * vec[0]) >> 8;
	p->y += (p->windWeight * vec[1]) >> 8;
	p->z += (p->windWeight * vec[2]) >> 8;
}

void Particle_Destroy(Particle_t* p)
{
	BGI_Free(p);
}

// ---- type 0 -------------------------------------------------------------------------------

static const Bmp_t* ParticleA_Bitmap(Particle_t* p, int sizeIndex)
{
	return Pattern_Bitmap(gPatternBmpA, p, sizeIndex);
}

static int ParticleA_Level(Particle_t* p) // the fixed level of the base (always 0)
{
	return p->fixedLevel;
}

// one tick: move by the velocity; 0 once dead
static int ParticleA_Step(Particle_t* base)
{
	ParticleA_t* q = (ParticleA_t*)base;
	if(q->p.alive)
	{
		q->p.x += q->vx;
		q->p.y += q->vy;
		q->p.z += q->vz;
		Particle_StepFrames(&q->p);
	}
	return q->p.alive;
}

static const ParticleVtbl_t ParticleA_Vtbl = {Particle_Destroy, ParticleA_Bitmap, ParticleA_Level, ParticleA_Step};

/* a type 0 particle of `pattern`, drawn with effect 0x20 at level 0; it
 * stays dead (and is removed by the next tick) when the pattern is out of
 * range or undefined.  The random draws happen in this order: x, z, vx,
 * vy, vz.  The caller owns the particle (Particle_Destroy frees it). */
Particle_t* ParticleA_New(int pattern)
{
	ParticleA_t* q = (ParticleA_t*)BGI_Alloc(sizeof *q);
	const PatternA_t* d;
	memset(q, 0, sizeof *q); // the original leaves the fields below unset for an undefined pattern
	Particle_Ctor(&q->p, 0, pattern);
	q->p.vt = &ParticleA_Vtbl;
	if((uint32_t)pattern >= PtclPatterns() || !gPatternA[pattern].defined)
		return &q->p;
	d = &gPatternA[pattern];
	q->p.x = Ptcl_Rnd(2 * d->xSpread) - d->xSpread;
	q->p.y = d->yStart;
	q->p.z = Ptcl_Rnd(2 * d->zSpread) - d->zSpread;
	q->vx = d->vxBase - d->vxVar + Ptcl_Rnd(2 * d->vxVar);
	q->vy = d->vyBase - d->vyVar + Ptcl_Rnd(2 * d->vyVar);
	q->vz = d->vzBase - d->vzVar + Ptcl_Rnd(2 * d->vzVar);
	q->p.windWeight = d->windWeight;
	q->p.effect = 0x20;
	q->p.fixedLevel = 0;
	q->p.alive = 1;
	return &q->p;
}

// ---- type 1 -------------------------------------------------------------------------------

static const Bmp_t* ParticleB_Bitmap(Particle_t* p, int sizeIndex)
{
	return Pattern_Bitmap(gPatternBmpB, p, sizeIndex);
}

/* the transparency: rises from 0 to 0x100 over the last `fadeOut` ticks
 * of the life (checked first), falls from 0x100 to 0 over the first
 * `fadeIn` ticks, 0 in between and when dead */
static int ParticleB_Level(Particle_t* base)
{
	const ParticleB_t* q = (const ParticleB_t*)base;
	uint32_t left = q->life - q->age;
	if(!q->p.alive)
		return 0;
	if(left < q->fadeOut)
		return 0x100 - (int)((left << 8) / q->fadeOut);
	if(q->age < q->fadeIn)
		return 0x100 - (int)((q->age << 8) / q->fadeIn);
	return 0;
}

/* one tick: age the particle (0 once its life is over), move it by the
 * velocity interpolated between the current and the next key; at the end
 * of a stretch the next key becomes the current one and a new next key is
 * drawn (x, y, z, then the length of the new stretch) */
static int ParticleB_Step(Particle_t* base)
{
	ParticleB_t* q = (ParticleB_t*)base;
	const PatternB_t* d;
	uint32_t age;
	if(!q->p.alive)
		return 0;
	age = q->age++;
	if(age >= q->life)
		return 0;
	Particle_StepFrames(&q->p);
	q->p.x += (q->vx1 - q->vx0) * (int32_t)q->t / (int32_t)q->turn + q->vx0;
	q->p.y += (q->vy1 - q->vy0) * (int32_t)q->t / (int32_t)q->turn + q->vy0;
	q->p.z += (q->vz1 - q->vz0) * (int32_t)q->t / (int32_t)q->turn + q->vz0;
	if(q->t < q->turn)
	{
		q->t++;
		return 1;
	}
	q->vy0 = q->vy1;
	q->vx0 = q->vx1;
	q->vz0 = q->vz1;
	d = &gPatternB[q->p.pattern];
	q->vx1 = d->vxBase - d->vxVar + Ptcl_Rnd(2 * d->vxVar);
	q->vy1 = d->vyBase - d->vyVar + Ptcl_Rnd(2 * d->vyVar);
	q->vz1 = d->vzBase - d->vzVar + Ptcl_Rnd(2 * d->vzVar);
	q->turn = (uint32_t)(d->turnBase + Ptcl_Rnd(d->turnVar));
	if(q->turn == 0) // a zero stretch would divide by zero above (the 1.494 type 1 reads "C0 2D" differently and can produce one)
		q->turn = 1;
	q->t = 0;
	return 1;
}

static const ParticleVtbl_t ParticleB_Vtbl = {Particle_Destroy, ParticleB_Bitmap, ParticleB_Level, ParticleB_Step};

/* a type 1 particle of `pattern`, drawn with the pattern's effect; it
 * stays dead when the pattern is out of range or undefined.  The random
 * draws happen in this order: life, x, z, then the two velocity keys (x,
 * y, z each), then the first stretch.  The caller owns the particle. */
Particle_t* ParticleB_New(int pattern)
{
	ParticleB_t* q = (ParticleB_t*)BGI_Alloc(sizeof *q);
	const PatternB_t* d;
	memset(q, 0, sizeof *q);
	Particle_Ctor(&q->p, 1, pattern);
	q->p.vt = &ParticleB_Vtbl;
	if((uint32_t)pattern >= PtclPatterns() || !gPatternB[pattern].defined)
		return &q->p;
	d = &gPatternB[pattern];
	q->age = 0;
	q->life = (uint32_t)(d->lifeBase + Ptcl_Rnd(d->lifeVar));
	q->p.x = Ptcl_Rnd(2 * d->xSpread) - d->xSpread;
	q->p.y = d->yStart;
	q->p.z = Ptcl_Rnd(2 * d->zSpread) - d->zSpread;
	q->vx0 = d->vxBase - d->vxVar + Ptcl_Rnd(2 * d->vxVar);
	q->vy0 = d->vyBase - d->vyVar + Ptcl_Rnd(2 * d->vyVar);
	q->vz0 = d->vzBase - d->vzVar + Ptcl_Rnd(2 * d->vzVar);
	q->vx1 = d->vxBase - d->vxVar + Ptcl_Rnd(2 * d->vxVar);
	q->vy1 = d->vyBase - d->vyVar + Ptcl_Rnd(2 * d->vyVar);
	q->vz1 = d->vzBase - d->vzVar + Ptcl_Rnd(2 * d->vzVar);
	q->t = 0;
	q->turn = (uint32_t)(d->turnBase + Ptcl_Rnd(d->turnVar));
	if(q->turn == 0) // a zero stretch would divide by zero in ParticleB_Step (the 1.494 type 1 reads "C0 2D" differently and can produce one)
		q->turn = 1;
	q->p.windWeight = d->windWeight;
	q->fadeIn = (uint32_t)d->fadeIn;
	q->fadeOut = (uint32_t)d->fadeOut;
	q->p.effect = d->effect;
	q->p.alive = 1;
	return &q->p;
}
