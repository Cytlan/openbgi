/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * particle_internal.h - what the files of the particle engine share
 *                       (src/gfx/particle/: patterns.c, types.c, wind.c,
 *                       engine.c)
 *
 * The public interface is inc/bgi/gfx/screens.h: a particle screen owns one
 * engine and draws what the engine renders.  An engine owns up to PTCL_MAX
 * particles, a camera and a wind generator (wind.c).  Particles come in two
 * types (types.c) with PTCL_PATTERNS patterns each; the patterns are global
 * tables shared by every screen (patterns.c):
 *   type 0: constant velocity, lives until it leaves the world
 *   type 1: limited life, wandering velocity, fades in and out
 * Each pattern also owns PTCL_SCALES pre-scaled copies of its bitmap
 * (32/32 .. 1/32) that stand in for perspective scaling.
 *
 * World coordinates are 24.8 fixed point; script values arrive as 16.16
 * and are shifted right by 8 on the way in.  The instructions that reach
 * the engine are "C0 0x" .. "C0 2x" (src/vm/ops_ext1.c).  See
 * docs/particle_rain.md.
 */
#ifndef BGI_GFX_PARTICLE_INTERNAL_H_
#define BGI_GFX_PARTICLE_INTERNAL_H_

#include "bgi/gfx.h"
#include "bgi/gfx/screens.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/display.h"
#include "bgi/sys.h"
#include "bgi/version.h"
#include <math.h>

/* particles per engine: 0x1000 up to 1.69, 0x4000 in 1.494, 0x8000 from
 * 1.529; the tables take the largest, the checks the generation's */
#define PTCL_MAX  0x8000
#define PtclMax() (gEngine->gen >= GEN_1_529 ? 0x8000 : gEngine->gen >= GEN_1_494 ? 0x4000 \
																				  : 0x1000)
/* 8 patterns per type until 1.616; 1.640 has 64.
 * The tables are sized for 64, the checks use the generation's count. */
#define PTCL_PATTERNS  64
#define PtclPatterns() ((uint32_t)(gEngine->gen >= GEN_1_640 ? 64 : 8))
#define PTCL_SCALES    32 // pre-scaled copies per pattern: 32/32 down to 1/32

typedef struct Particle Particle_t;
typedef struct ParticleVtbl
{
	void (*destroy)(Particle_t* p);
	const Bmp_t* (*bitmap)(Particle_t* p, int sizeIndex); // the picture at a scale index (0 = full size); NULL = nothing to draw
	int (*level)(Particle_t* p);                          // transparency 0 .. 0x100 for the blit
	int (*step)(Particle_t* p);                           // one tick of motion; 0 when dead
} ParticleVtbl_t;

struct Particle // base, 0x28 bytes
{
	const ParticleVtbl_t* vt;
	int32_t type;       // 0 or 1
	int32_t pattern;    // 0 .. 7 (0 .. 63 from 1.640)
	int32_t alive;      // 0 once dead; the next tick removes it from its slot
	int32_t x, y, z;    // 24.8
	int32_t windWeight; // 24.8: how strongly the wind moves it
	int32_t effect;     // effect mode it is drawn with (0x20 for type 0, the pattern's for type 1)
	int32_t fixedLevel; // the level a type 0 particle reports (always 0; type 1 computes its own)
	/* 1.494 on: the frame of an animated pattern ("C0 18"): 16.16 frame
	 * position and its advance per tick */
	int32_t framePos, frameRate;
	// 1.640 on: the same for the pattern's alternate image ("C0 1A", type 1)
	int32_t altPos, altRate;
};

typedef struct ParticleA // type 0, 0x34 bytes
{
	Particle_t p;
	int32_t vx, vy, vz; // velocity, 24.8 per tick
} ParticleA_t;

typedef struct ParticleB // type 1, 0x58 bytes
{
	Particle_t p;
	uint32_t life, age; // ticks
	int32_t vx0, vx1;   // current and next key velocity, 24.8 per tick
	int32_t vy0, vy1;
	int32_t vz0, vz1;
	uint32_t turn, t;         // ticks per stretch, position in it
	uint32_t fadeIn, fadeOut; // ticks
} ParticleB_t;

typedef struct PatternA // the type 0 pattern parameters of "C0 25", 44 bytes; all 24.8
{
	int32_t defined;       // set by "C0 25"; an undefined pattern bears dead particles
	int32_t xSpread;       // start x in [-xSpread, +xSpread]
	int32_t yStart;        // start y
	int32_t zSpread;       // start z in [-zSpread, +zSpread]
	int32_t vxBase, vxVar; // velocity = base - var + Rnd(2 var)
	int32_t vyBase, vyVar;
	int32_t vzBase, vzVar;
	int32_t windWeight;
} PatternA_t;

typedef struct PatternB // the type 1 pattern parameters of "C0 2D", 72 bytes
{
	int32_t defined;           // set by "C0 2D"; an undefined pattern bears dead particles
	int32_t lifeBase, lifeVar; // ticks: life = base + Rnd(var)
	int32_t xSpread;           // start x in [-xSpread, +xSpread], 24.8
	int32_t yStart;            // start y, 24.8
	int32_t zSpread;           // start z in [-zSpread, +zSpread], 24.8
	int32_t vxBase, vxVar;     // velocity keys = base - var + Rnd(2 var), 24.8
	int32_t vyBase, vyVar;
	int32_t vzBase, vzVar;
	int32_t turnBase, turnVar; // ticks per stretch between two keys: base + Rnd(var)
	int32_t windWeight;        // 24.8
	int32_t fadeIn, fadeOut;   // ticks, never 0
	int32_t effect;            // effect mode the particles are drawn with
} PatternB_t;

extern Bmp_t gPatternBmpB[PTCL_PATTERNS][PTCL_SCALES]; // type 1: the scaled copies per pattern
extern PatternB_t gPatternB[PTCL_PATTERNS];
extern Bmp_t gPatternBmpA[PTCL_PATTERNS][PTCL_SCALES]; // type 0
extern PatternA_t gPatternA[PTCL_PATTERNS];
extern int gPatternsReady; // Ptcl_InitGlobals ran

/* 1.494 on: "C0 18" replaces a pattern's picture by an animation of up to
 * PTCL_FRAMES frames (count, then per scale an array of scaled frames, then
 * the two 16.16 rates).  While `count` is set the particles of the pattern
 * draw frames[sizeIndex][frame] instead of the single picture. */
#define PTCL_FRAMES 32
typedef struct PatternAnim
{
	int count;                  // frames; 0 = the pattern uses its single picture
	Bmp_t* frames[PTCL_SCALES]; // per scale an array of `count` scaled copies
	int32_t rate1, rate2;       // frames per tick, 16.16: base and random spread
} PatternAnim_t;
extern PatternAnim_t gPatternAnim[2][PTCL_PATTERNS]; // [type][pattern]
/* 1.640 on: the "alternate image" of a type 1 pattern ("C0 1A"): a second
 * animation, mixed into the picture by a level of 0 .. 0x100 ("C0 1B";
 * 0 shows the picture alone, 0x100 the alternate frame alone) */
extern PatternAnim_t gPatternAlt[PTCL_PATTERNS];
extern int32_t gPatternAltLevel[PTCL_PATTERNS];

// ---- patterns.c ---------------------------------------------------------------------------
// a random integer: ((rand << 15) | rand) % |n + 1|, i.e. 0 .. n for n >= 0
int32_t Ptcl_Rnd(int32_t n);
// the pre-scaled copy of the particle's pattern at a scale index, or NULL; an animated pattern's current frame
const Bmp_t* Pattern_Bitmap(Bmp_t table[PTCL_PATTERNS][PTCL_SCALES], const Particle_t* p, int sizeIndex);
// the frame animation of a particle: draw its rate at birth, advance it every tick
void Particle_StartFrames(Particle_t* p);
void Particle_StepFrames(Particle_t* p);

// ---- types.c ------------------------------------------------------------------------------
void Particle_Ctor(Particle_t* p, int type, int pattern);
void Particle_ApplyForce(Particle_t* p, const int32_t vec[3]); // pos += windWeight * vec >> 8 while alive (the wind)
void Particle_Destroy(Particle_t* p);
Particle_t* ParticleA_New(int pattern); // a particle of the pattern, dead when the pattern is undefined
Particle_t* ParticleB_New(int pattern);

// ---- wind.c -------------------------------------------------------------------------------
/* The wind: a vector that holds for a while, then glides to a new random
 * target; every particle moves with it by its wind weight. */
typedef struct Wind
{
	uint32_t interval;          // the engine's tick length, ms
	int32_t enabled;            // 0: the vector is zero
	int32_t cur[3];             // the vector of this tick, 24.8
	int32_t base[3];            // centre of the random targets, 24.8
	int32_t var[3];             // target = base - var + Rnd(2 var)
	uint32_t holdMs, holdVarMs; // the hold phase: length and its random spread, ms
	uint32_t moveMs, moveVarMs; // the glide phase: length and its random spread, ms
	uint32_t holdT, holdVarT;   // the four times in ticks
	uint32_t moveT, moveVarT;
	int32_t phase;     // 0 hold, 1 glide
	uint32_t len, pos; // ticks in the phase, ticks done
	int32_t from[3];   // the vector held, and the start of the glide
	int32_t to[3];     // the glide's target
} Wind_t;

void Wind_Ctor(Wind_t* w);
void Wind_SetInterval(Wind_t* w, uint32_t ms);
int Wind_Get(const Wind_t* w, int32_t out[3]); // the current vector; zero when disabled.  Returns the enabled flag
void Wind_SetCurrent(Wind_t* w, int32_t x, int32_t y, int32_t z);
int Wind_Set(Wind_t* w, const int32_t a[11]); // the 11 values of "C0 10": enabled, base and var per axis, the four times
void Wind_Step(Wind_t* w);                    // one tick

#endif // BGI_GFX_PARTICLE_INTERNAL_H_
