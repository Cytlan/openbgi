/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * screens.h - the particle screen (class order 4; src/gfx/particle_screen.c)
 * and the rain screen (class order 5; src/gfx/rain_screen.c), with the
 * simulations behind them: the particle engine (src/gfx/particle/) and the
 * rain generator (src/gfx/rain.c).  See docs/dev/particle_rain.md for the
 * simulations, src/vm/ops_ext1.c for the screens' instructions.
 *
 * Both screens are thin display objects: a private surface bitmap that a
 * simulation paints into, plus the position / effect / level of an ordinary
 * object.  The particle screen is driven by the instructions "C0 0x" ..
 * "C0 2x", the rain screen by "C0 4x" (src/vm/ops_ext1.c), through the
 * graphics manager (src/gfx/mgr/screens.c).  Result codes: the screens and
 * simulations return 0 for success and 0x8000000n for a failure, except
 * where a comment says 1 / 0.
 */
#ifndef BGI_GFX_SCREENS_H_
#define BGI_GFX_SCREENS_H_

#include "bgi/gfx/dispobj.h"

//  --- particle engine (src/gfx/particle/engine.c) ----------------
typedef struct ParticleEngine ParticleEngine_t;

ParticleEngine_t* ParticleEngine_New(OpenBGI_t* o);                                                                            // an empty engine with a 10 ms tick; the caller deletes it
void ParticleEngine_Delete(OpenBGI_t* o, ParticleEngine_t* e);                                                                 // destroy every particle and free the engine
void ParticleEngine_Update(OpenBGI_t* o, ParticleEngine_t* e);                                                                 // bring the simulation up to the present (once per pass)
void ParticleEngine_Prerun(OpenBGI_t* o, ParticleEngine_t* e, int ms);                                                         // "C0 0D": run `ms` milliseconds ahead, then restore the clock
int ParticleEngine_Render(OpenBGI_t* o, ParticleEngine_t* e, Bmp_t* dst, int cx, int cy, int32_t* outCount, Rect_t* outRects); // draw into `dst` with the centre at (cx, cy); the rectangles touched go to outRects, their number to *outCount and the result
int ParticleEngine_SetCamera(ParticleEngine_t* e, int32_t x, int32_t y, int32_t z,
	int32_t a, int32_t b, int32_t c, int32_t dist);                                                            // "C0 0B": position 16.16, angles 16.16 degrees; 0 for dist <= 0
int ParticleEngine_SetInterval(ParticleEngine_t* e, int ms);                                                   // "C0 0C": the tick length, 0 .. 100 ms (0 pauses); 0 when rejected
void ParticleEngine_Clear(OpenBGI_t* o, ParticleEngine_t* e);                                                  // "C0 0F": remove every particle
int ParticleEngine_SetWind(OpenBGI_t* o, ParticleEngine_t* e, const int32_t args[11]);                         // "C0 10": the 11 wind values (src/gfx/particle/wind.c); always 1
int ParticleEngine_SetGroupA(OpenBGI_t* o, ParticleEngine_t* e, int pattern, int max, int spawnEvery);         // "C0 20": at most `max` type 0 particles of the pattern, one born every `spawnEvery` ms
int ParticleEngine_SetGroupB(OpenBGI_t* o, ParticleEngine_t* e, int pattern, int max, int spawnEvery);         // "C0 28": the same for type 1.  0 / 0x80000002 bad pattern / 0x80000003 bad maximum
uint32_t ParticleEngine_SetPatternBExtra(OpenBGI_t* o, ParticleEngine_t* e, int pattern, const int32_t v[14]); // 1.492 on, "C0 29": 14 more values of a type 1 pattern, stored only
/* the global pattern tables ("C0 24" / "C0 25" / "C0 2C" / "C0 2D"); the
 * setters return 1, or 0 for a pattern outside the generation's count (8,
 * 64 from 1.640).  Spreads and variances are stored as absolute values;
 * everything 16.16 becomes 24.8. */
int PatternA_Set(OpenBGI_t* o, int pattern, int32_t xSpread, int32_t yStart, int32_t zSpread, int32_t vxBase, int32_t vxVar,
	int32_t vyBase, int32_t vyVar, int32_t vzBase, int32_t vzVar, int32_t windWeight);
int PatternB_Set(OpenBGI_t* o, int pattern, int32_t lifeBase, int32_t lifeVar, int32_t xSpread, int32_t yStart, int32_t zSpread,
	int32_t vxBase, int32_t vxVar, int32_t vyBase, int32_t vyVar, int32_t vzBase, int32_t vzVar, int32_t turnBase,
	int32_t turnVar, int32_t windWeight, int32_t fadeIn, int32_t fadeOut, int32_t effect);
// bind a managed bitmap (-1 = none) to a pattern: 0 / 0x80000002 bad pattern / 0x80000004 no such bitmap / 0x80000005 unusable
uint32_t PatternA_BindBitmap(OpenBGI_t* o, int pattern, int bmp); // "C0 24"
// 1.492 on, "C0 18": an animation of `count` frames for a pattern of a type; 0 / 0x80000001 type / 2 pattern / 5 frames not alike / 6 too many
uint32_t Pattern_SetAnimation(OpenBGI_t* o, int type, int pattern, int count, const Bmp_t* frames, int32_t rate1, int32_t rate2);
/* 1.640 on, "C0 1A" / "C0 1B": the alternate image of a type 1 pattern -
 * a second animation cross-faded into the picture by `level` (0 .. 0x100)
 * - and that level alone.  0 / 0x80000001 type not 1 / 2 pattern (1B: no
 * alternate image) / 5 frames not alike / 6 too many / 7 (1B) pattern */
uint32_t Pattern_SetAltAnimation(OpenBGI_t* o, int type, int pattern, int count, const Bmp_t* frames, int32_t rate1, int32_t rate2,
	int level);
uint32_t Pattern_SetAltLevel(OpenBGI_t* o, int type, int pattern, int level);
uint32_t PatternB_BindBitmap(OpenBGI_t* o, int pattern, int bmp); // "C0 2C"
// "C0 09": redraw the screen every `intervalMs` without the script (0 stops); 0 = no such screen
int Ptcl_SetAuto(OpenBGI_t* o, uint32_t handle, uint32_t intervalMs);

//  --- particle screen (0x200CC bytes) -----------------------
typedef struct ParticleScreen
{
	DispObj_t obj;
	ParticleEngine_t* engine; // owned
	Bmp_t surf;               // w x h, screen mode with alpha, owned
	int32_t cx, cy;           // projection centre on the surface, pixels
	int32_t rectLimit;        // from this many dirty rectangles on a redraw invalidates the whole layer; default 0x1000
	int32_t flip;             // which rectangle list is current
	int32_t rectCount[2];     // rectangles in each list
	Rect_t rects[2][4096];    // the rectangles the particles touched, previous and current frame
} ParticleScreen_t;

extern const DispObjVtbl_t ParticleScreen_Vtbl;

void ParticleScreen_Ctor(OpenBGI_t* o, ParticleScreen_t* p, int slotId);
void ParticleScreen_Dtor(OpenBGI_t* o, ParticleScreen_t* p);
void ParticleScreen_Update(OpenBGI_t* o, ParticleScreen_t* p);                                                 // once per pass
void ParticleScreen_Prerun(OpenBGI_t* o, ParticleScreen_t* p, int ms);                                         // "C0 0D"
void ParticleScreen_SetDraw(OpenBGI_t* o, ParticleScreen_t* p, int x, int y, int effect, int level, int prio); // "C0 05"
int ParticleScreen_SetSize(OpenBGI_t* o, ParticleScreen_t* p, int w, int h);                                   // "C0 00": 1 ok, 0 rejected
void ParticleScreen_SetRectLimit(ParticleScreen_t* p, int limit);                                              // "C0 0A"
void ParticleScreen_Redraw(OpenBGI_t* o, ParticleScreen_t* p);                                                 // "C0 08": repaint the surface and dirty what changed
int ParticleScreen_SetCamera(ParticleScreen_t* p, int32_t x, int32_t y, int32_t z, int32_t a, int32_t b,
	int32_t c, int32_t dist, int cx, int cy);                                                          // "C0 0B": the engine's camera and the centre (cx, cy); 0 for dist <= 0
int ParticleScreen_SetInterval(ParticleScreen_t* p, int ms);                                           // "C0 0C"
void ParticleScreen_Clear(OpenBGI_t* o, ParticleScreen_t* p);                                          // "C0 0F"
int ParticleScreen_SetWind(OpenBGI_t* o, ParticleScreen_t* p, const int32_t args[11]);                 // "C0 10"
int ParticleScreen_SetGroupA(OpenBGI_t* o, ParticleScreen_t* p, int pattern, int max, int spawnEvery); // "C0 20"
int ParticleScreen_SetGroupB(OpenBGI_t* o, ParticleScreen_t* p, int pattern, int max, int spawnEvery); // "C0 28"

//  --- rain generator (src/gfx/rain.c) -----------------------
typedef struct RainParams // the 16 values of "C0 46" .. "C0 4B"
{
	int32_t xMin; // spawn x in [xMin, xMax)
	int32_t yTop; // spawn height (y up); a drop starts up to 99 below it
	int32_t zMin; // spawn z in [zMin, zMax)
	int32_t xMax;
	int32_t yBottom; // a drop dies when its tail is at or below this
	int32_t zMax;
	int32_t dir[3];       // fall direction, unit steps (the screens use (0, -1, 0))
	int32_t fall;         // distance per stage, 24.8 (integer part used)
	int32_t length;       // streak length, 24.8 (integer part used)
	uint32_t colour;      // ARGB of the streaks; the alpha fades with depth
	uint32_t clearColour; // handed to the line renderer, which never uses it
	int32_t perStage;     // drops born per stage
	uint32_t stageMs;     // milliseconds per stage
	int32_t hiResTimer;   // the original's clock choice (timeGetTime or GetTickCount); no effect here
} RainParams_t;

typedef struct RainView // the 7 values of "C0 4C" .. "C0 4E"
{
	int32_t cam[3]; // camera position
	int32_t ang[3]; // camera angles, 0.1 degree units
	int32_t dist;   // projection distance and near plane
} RainView_t;

typedef struct RainGen RainGen_t;

RainGen_t* RainGen_New(void);                                                         // an empty generator; the caller deletes it
void RainGen_Delete(RainGen_t* g);                                                    // free the drops and the generator
void RainGen_SetParams(RainGen_t* g, RainParams_t* old, const RainParams_t* p);       // install `p`, *old receives the previous block
void RainGen_SetView(RainGen_t* g, RainView_t* old, const RainView_t* v);             // install `v`, *old receives the previous view
void RainGen_Start(OpenBGI_t* o, RainGen_t* g, uint32_t prerollMs);                   // "C0 42": drop every drop, run `prerollMs` (< 500) at the next update
void RainGen_Update(OpenBGI_t* o, RainGen_t* g);                                      // run the stages that are due (once per pass)
int RainGen_Render(RainGen_t* g, uint8_t* pixels, int w, int h, int pitch, int bits); // draw the drops into a 32-bit surface (`bits` unused); result meaningless

//  --- rain screen (0x118 bytes) -----------------------------
typedef struct RainScreen
{
	DispObj_t obj;
	RainParams_t params; // the values the setters collect; pushed to `gen` as a block
	RainView_t view;
	RainGen_t* gen;  // NULL until "C0 42" (RainScreen_Start); owned
	Bmp_t surf;      // owned
	int32_t maskBmp; // gray bitmap of the screen's size, -1 = none
	int32_t maskGen; // the generation of that bitmap's slot when it was set
} RainScreen_t;

extern const DispObjVtbl_t RainScreen_Vtbl;

void RainScreen_Ctor(OpenBGI_t* o, RainScreen_t* r, int slotId);
void RainScreen_Dtor(RainScreen_t* r);
void Fx_SetEffectsEnabled(OpenBGI_t* o, int on);
int Fx_GetEffectsEnabled(OpenBGI_t* o);
int Fx_SetEffectFps(OpenBGI_t* o, int enable, uint32_t fps);              // "C0 4F": the switch and the effect frame rate; 0 for fps outside 1 .. 1000
void RainScreen_Start(OpenBGI_t* o, RainScreen_t* r, uint32_t prerollMs); // "C0 42": a fresh generator with the screen's values
int RainScreen_SetSize(OpenBGI_t* o, RainScreen_t* r, int w, int h);      // "C0 40": 0 ok, 0x80000001 rejected
int RainScreen_Update(OpenBGI_t* o, RainScreen_t* r);                     // once per pass: 0, or 0x80000002 without a generator
int RainScreen_Redraw(OpenBGI_t* o, RainScreen_t* r);                     // every effect frame: 0, 0x80000003 no surface, 0x80000002 no generator
/* the setters: 0 ok, 0x80000002 the value is stored but there is no
 * generator yet, 0x80000004 a zero value where one is not allowed */
int RainScreen_SetArea(RainScreen_t* r, int xMin, int yTop, int zMin, int xMax, int yBottom, int zMax); // "C0 46"
int RainScreen_SetFall(RainScreen_t* r, int n);                                                         // "C0 47": not 0
int RainScreen_SetLength(RainScreen_t* r, int n);                                                       // "C0 48": not 0
int RainScreen_SetColour(RainScreen_t* r, uint32_t argb);                                               // "C0 49"
int RainScreen_SetPerStage(RainScreen_t* r, int n);                                                     // "C0 4A"
int RainScreen_SetStageMs(RainScreen_t* r, int n);                                                      // "C0 4B": not 0
int RainScreen_SetCamera(RainScreen_t* r, int x, int y, int z);                                         // "C0 4C"
int RainScreen_SetAngles(RainScreen_t* r, int a, int b, int c);                                         // "C0 4D": 0.1 degree units
int RainScreen_SetDistance(RainScreen_t* r, int n);                                                     // "C0 4E": not 0
int RainScreen_SetDraw(OpenBGI_t* o, RainScreen_t* r, int x, int y, int effect, int level, int prio);   // "C0 45": always 0
int RainScreen_SetMask(OpenBGI_t* o, RainScreen_t* r, int bmp);                                         // "C0 43": a GRAY8 bitmap of the screen's size or -1; 0, 0x80000005 no such bitmap, 0x80000006 wrong mode or size

// ---- the particle patterns, shared by every particle engine (o->ptcl, src/gfx/particle/) ----

#define PTCL_MAX      0x8000 // particles per engine at most (the generation's count: PtclMax, particle_internal.h)
#define PTCL_PATTERNS 64     // patterns per type at most (the generation's count: PtclPatterns)
#define PTCL_SCALES   32     // pre-scaled copies per pattern: 32/32 down to 1/32
#define PTCL_FRAMES   32     // frames of a pattern's animation at most ("C0 18")

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

// a pattern's animation ("C0 18") or alternate image ("C0 1A")
typedef struct PatternAnim
{
	int count;                  // frames; 0 = the pattern uses its single picture
	Bmp_t* frames[PTCL_SCALES]; // per scale an array of `count` scaled copies
	int32_t rate1, rate2;       // frames per tick, 16.16: base and random spread
} PatternAnim_t;

#endif // BGI_GFX_SCREENS_H_
