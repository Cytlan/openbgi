/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ops_ext1.c - the "C0 xx" extension instruction family: particle screens
 *              and their patterns, rain screens, the effect frame rate and
 *              the "bwef" wind tables.  Interface: bgi/vm.h (vm_optable_C0,
 *              Vm_OptableC0Init).
 *
 * Same conventions as ops_gfx0.c: every handler pops its operands in the
 * order the original does, validates them with the Check* routines (which
 * raise a script error and never return), calls the service and maps the
 * service's result code to a script error.  The stack comments on the
 * signature lines use the notation "a, b → r": b is on top of the stack.
 * The original reaches the screens through a layer of cdecl wrappers
 * (Fx_ptcl_*, Fx_rain_*) that only fetch the manager pointer; the handlers
 * here call the graphics manager (gGfx) directly.
 */
#include "bgi/vm.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/sys.h"
#include "bgi/gfx.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/screens.h"
#include "bgi/gfx/bmpops.h"

VmHandler_t vm_optable_C0[256]; // filled by Vm_OptableC0Init for the selected engine profile

// raise a script error with a printf-formatted message; never returns
#define EXT_ERROR(t, ...)           \
	do                              \
	{                               \
		char msg_[0x104];           \
		sprintf(msg_, __VA_ARGS__); \
		ScriptError(msg_, (t));     \
	} while(0)

#define NO_OBJECT 0xff // the manager wrappers' result for an unknown handle

// -------------------------------------------------------------------------
// particle screens
// -------------------------------------------------------------------------

/* create a particle screen of w x h pixels and push its handle; no free
 * slot or a rejected size is a script error */
static int Opcode_Ext1_PtclCreate(Thread_t* t) // C0 00: w, h → handle
{
	uint32_t handle = 0;
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	switch(Gfx_ParticleCreate(gGfx, &handle, w, h))
	{
		case 1: ScriptError(MSG_NO_MORE_PTCL_SCREENS, t); break;
		case 2: EXT_ERROR(t, MSG_BAD_FX_SCREEN_SIZE, w, h); break;
		default: break;
	}
	Thread_Push(t, handle);
	return 0;
}

static int Opcode_Ext1_PtclDelete(Thread_t* t) // C0 01: handle →
{
	if(!Gfx_ParticleDelete(gGfx, Thread_Pop(t)))
		ScriptError(MSG_BAD_PTCL_HANDLE, t);
	return 0;
}

static int Opcode_Ext1_PtclShow(Thread_t* t) // C0 04: handle, on →
{
	int on = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!Gfx_ParticleShow(gGfx, h, on))
		ScriptError(MSG_BAD_PTCL_HANDLE, t);
	return 0;
}

// place the screen at (x, y) on the display with an effect mode, a level and a priority
static int Opcode_Ext1_PtclSetDraw(Thread_t* t) // C0 05: handle, x, y, effect, level, prio →
{
	int prio = (int)Thread_Pop(t);
	int level = (int)Thread_Pop(t);
	int effect = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority((uint32_t)prio, t);
	CheckAlpha((uint32_t)level, t);
	CheckEffectMode((uint32_t)effect, t);
	if(!Gfx_ParticleSetDraw(gGfx, h, x, y, effect, level, prio))
		ScriptError(MSG_BAD_PTCL_HANDLE, t);
	return 0;
}

// repaint the screen's surface from the current particle state
static int Opcode_Ext1_PtclRedraw(Thread_t* t) // C0 08: handle →
{
	if(!Gfx_ParticleRedraw(gGfx, Thread_Pop(t)))
		ScriptError(MSG_BAD_PTCL_HANDLE, t);
	return 0;
}

// redraw the screen on its own every intervalMs milliseconds (0 stops that)
static int Opcode_Ext1_PtclSetAuto(Thread_t* t) // C0 09: handle, intervalMs →
{
	uint32_t ms = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!Ptcl_SetAuto(h, ms))
		ScriptError(MSG_BAD_PTCL_HANDLE, t);
	return 0;
}

/* the number of dirty rectangles a redraw may collect before it dirties
 * the whole layer instead (0x1000 after creation) */
static int Opcode_Ext1_PtclSetRectLimit(Thread_t* t) // C0 0A: handle, limit →
{
	int limit = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!Gfx_ParticleSetRectLimit(gGfx, h, limit))
		ScriptError(MSG_BAD_PTCL_HANDLE, t);
	return 0;
}

/* the camera: its position (x, y, z in 16.16), its rotation angles (a, b,
 * c in 16.16 degrees), the projection distance and the projection centre
 * (cx, cy in pixels); a distance that is not positive is a script error */
static int Opcode_Ext1_PtclSetCamera(Thread_t* t) // C0 0B: handle, x, y, z, a, b, c, dist, cx, cy →
{
	int cy = (int)Thread_Pop(t);
	int cx = (int)Thread_Pop(t);
	int32_t dist = (int32_t)Thread_Pop(t);
	int32_t c = (int32_t)Thread_Pop(t);
	int32_t b = (int32_t)Thread_Pop(t);
	int32_t a = (int32_t)Thread_Pop(t);
	int32_t z = (int32_t)Thread_Pop(t);
	int32_t y = (int32_t)Thread_Pop(t);
	int32_t x = (int32_t)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(Gfx_ParticleSetCamera(gGfx, h, x, y, z, a, b, c, dist, cx, cy))
	{
		case 3: EXT_ERROR(t, MSG_BAD_PTCL_SCALE, dist); break;
		case NO_OBJECT: ScriptError(MSG_BAD_PTCL_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// the simulation step in milliseconds, 0 .. 100 (0 halts the simulation); outside that a script error
static int Opcode_Ext1_PtclSetInterval(Thread_t* t) // C0 0C: handle, ms →
{
	int ms = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(Gfx_ParticleSetInterval(gGfx, h, ms))
	{
		case 4: EXT_ERROR(t, MSG_BAD_PTCL_INTERVAL, ms); break;
		case NO_OBJECT: ScriptError(MSG_BAD_PTCL_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// advance the simulation by ms milliseconds without drawing, so the screen does not start empty
static int Opcode_Ext1_PtclPrerun(Thread_t* t) // C0 0D: handle, ms →
{
	int ms = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!Gfx_ParticlePrerun(gGfx, h, ms))
		ScriptError(MSG_BAD_PTCL_HANDLE, t);
	return 0;
}

// remove every live particle of the screen
static int Opcode_Ext1_PtclClear(Thread_t* t) // C0 0F: handle →
{
	if(!Gfx_ParticleClear(gGfx, Thread_Pop(t)))
		ScriptError(MSG_BAD_PTCL_HANDLE, t);
	return 0;
}

/* the wind of the screen: on / off, the base and variance of its strength
 * per axis, and the hold and move times (base and variance, milliseconds)
 * of its changes */
static int Opcode_Ext1_PtclSetWind(Thread_t* t) // C0 10: handle, enable, baseX, varX, baseY, varY, baseZ, varZ, holdMs, holdVarMs, moveMs, moveVarMs →
{
	int32_t args[11];
	uint32_t h;
	int i;
	for(i = 10; i >= 0; i--)
		args[i] = (int32_t)Thread_Pop(t);
	h = Thread_Pop(t);
	if(!Gfx_ParticleSetWind(gGfx, h, args))
		ScriptError(MSG_BAD_PTCL_HANDLE, t);
	return 0;
}

/* map the result of a group setting ("C0 20" / "C0 28") to a script error:
 * 5 unknown pattern, 6 a rejected maximum, NO_OBJECT unknown handle */
static void Ext1_GroupResult(Thread_t* t, int r, int pattern, int max)
{
	switch(r)
	{
		case 5: EXT_ERROR(t, MSG_BAD_PATTERN_NO, pattern); break;
		case 6: EXT_ERROR(t, MSG_BAD_MAX_OBJECTS, max); break;
		case NO_OBJECT: ScriptError(MSG_BAD_PTCL_HANDLE, t); break;
		default: break;
	}
}

/* the births of type A particles of a pattern on the screen: at most `max`
 * alive at a time (4096 over all groups), one born every `spawnEvery`
 * milliseconds */
static int Opcode_Ext1_PtclGroupA(Thread_t* t) // C0 20: handle, pattern, max, spawnEvery →
{
	int spawnEvery = (int)Thread_Pop(t);
	int max = (int)Thread_Pop(t);
	int pattern = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	Ext1_GroupResult(t, Gfx_ParticleSetGroupA(gGfx, h, pattern, max, spawnEvery), pattern, max);
	return 0;
}

// the same for the type B particles of a pattern
static int Opcode_Ext1_PtclGroupB(Thread_t* t) // C0 28: handle, pattern, max, spawnEvery →
{
	int spawnEvery = (int)Thread_Pop(t);
	int max = (int)Thread_Pop(t);
	int pattern = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	Ext1_GroupResult(t, Gfx_ParticleSetGroupB(gGfx, h, pattern, max, spawnEvery), pattern, max);
	return 0;
}

/* 1.494 on: fourteen more parameters of a type B pattern of the screen
 * (ParticleEngine_SetPatternBExtra says how each is scaled); an unknown
 * pattern is a script error */
static int Opcode_Ext1_PtclPatternBExtra(Thread_t* t) // C0 29 (1.494 on): handle, pattern, v[14] →
{
	int32_t v[14];
	int i, pattern;
	uint32_t h;
	for(i = 13; i >= 0; i--)
		v[i] = (int32_t)Thread_Pop(t);
	pattern = (int)Thread_Pop(t);
	h = Thread_Pop(t);
	switch(Gfx_ParticleSetPatternBExtra(gGfx, h, pattern, v))
	{
		case 5: EXT_ERROR(t, MSG_BAD_PATTERN_NO, pattern); break;
		case NO_OBJECT: ScriptError(MSG_BAD_PTCL_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* map the result of binding a bitmap to a pattern ("C0 24" / "C0 2C") to a
 * script error: unknown pattern, no such bitmap, a bitmap the particles
 * cannot use */
static void Ext1_BitmapResult(Thread_t* t, uint32_t r, int pattern, int bmp)
{
	switch(r)
	{
		case 0x80000002u: EXT_ERROR(t, MSG_BAD_PATTERN_NO, pattern); break;
		case 0x80000004u: EXT_ERROR(t, MSG_BMP_NOT_EXIST, bmp); break;
		case 0x80000005u: EXT_ERROR(t, MSG_BMP_NOT_FOR_PARTICLES, bmp); break;
		default: break;
	}
}

/* 1.494 on: the control mode of the particle animations that "C0 18" sets
 * up (see there): 0 or 1, and 2 from 1.640 on; any other value is a script
 * error */
int gPtclAnimMode;                               // the mode "C0 1F" selected, read by "C0 18"
static int Opcode_Ext1_PtclAnimMode(Thread_t* t) // C0 1F (1.494 on): mode →
{
	uint32_t mode = Thread_Pop(t);
	if(mode > (gEngine->gen >= GEN_1_640 ? 2u : 1u))
		EXT_ERROR(t, MSG_BAD_CONTROL_MODE, mode);
	gPtclAnimMode = (int)mode;
	return 0;
}

/* 1.494 on: the bitmaps firstBmp .. firstBmp + count - 1 become the
 * animation frames of the particles of a pattern (type 0 / 1).  Every
 * particle advances its frame position by a 16.16 rate drawn once from
 * rate1 + Rnd(rate2), where the rates come from the three numbers and the
 * control mode of "C0 1F": mode 0 rate1 = frames / a, rate2 = frames / b;
 * mode 1 rate1 = frames / a, rate2 = frames / (a + b) - rate1; mode 2
 * (1.640 on) rate1 = frames / a, rate2 = rate1 * b / a (a divisor of 0
 * gives a rate of 0).  Count 0 drops the animation; a missing bitmap of the
 * range, a bad type or pattern, a range of unequal bitmaps and a rejected
 * count are script errors. */
/* the shared part of "C0 18" and "C0 1A": the frames firstBmp .. firstBmp
 * + count - 1 into a new list (NULL for a count of 0; a missing bitmap is
 * a script error) and the two rates from a, b, frames and the control
 * mode (see Opcode_Ext1_PtclAnimation) */
static Bmp_t* PtclAnimFrames(Thread_t* t, int firstBmp, int count, int32_t a, int32_t b, int32_t frames,
	uint32_t* rate1, uint32_t* rate2)
{
	Bmp_t* list = NULL;
	uint32_t f16 = (uint32_t)frames << 16;
	int i, ok = 1;
	if(count > 0 && firstBmp != -1)
	{
		list = (Bmp_t*)BGI_Calloc((size_t)count * sizeof(Bmp_t));
		for(i = 0; i < count && ok; i++)
			ok = BmpMgr_GetInfo(gBmpMgr, &list[i], firstBmp + i);
	}
	else if(count > 0)
		ok = 0;
	if(!ok)
	{
		BGI_Free(list);
		EXT_ERROR(t, MSG_BMP_NOT_EXIST, firstBmp);
	}
	switch(gPtclAnimMode)
	{
		case 0:
			*rate1 = a ? f16 / (uint32_t)a : 0;
			*rate2 = b ? f16 / (uint32_t)b : 0;
			if(!a || !b)
				*rate2 = 0;
			break;
		case 1:
			*rate1 = a ? f16 / (uint32_t)a : 0;
			*rate2 = (a && b) ? f16 / (uint32_t)(a + b) - *rate1 : 0;
			break;
		case 2:
			*rate1 = a ? f16 / (uint32_t)a : 0;
			*rate2 = a ? (uint32_t)(((uint64_t)*rate1 * (uint32_t)b) / (uint32_t)a) : 0;
			break;
		default: *rate1 = *rate2 = (uint32_t)count; break;
	}
	return list;
}

// the script errors of the animation setters (0 passes)
static void PtclAnimResult(Thread_t* t, uint32_t r, int type, int pattern, int firstBmp, int count)
{
	switch(r)
	{
		case 0x80000001u: EXT_ERROR(t, MSG_BAD_PTCL_TYPE, type); break;
		case 0x80000002u: EXT_ERROR(t, MSG_BAD_OBJ_PATTERN, pattern); break;
		case 0x80000004u: EXT_ERROR(t, MSG_BMP_NOT_EXIST, firstBmp); break;
		case 0x80000005u: EXT_ERROR(t, MSG_BMP_RANGE_NOT_CONFORM, firstBmp, firstBmp + count - 1); break;
		case 0x80000006u: EXT_ERROR(t, MSG_BAD_BMP_COUNT, count); break;
		default: break;
	}
}

static int Opcode_Ext1_PtclAnimation(Thread_t* t) // C0 18 (1.494 on): type, pattern, count, firstBmp, a, b, frames →
{
	int32_t frames = (int32_t)Thread_Pop(t), b = (int32_t)Thread_Pop(t), a = (int32_t)Thread_Pop(t);
	int firstBmp = (int)Thread_Pop(t), count = (int)Thread_Pop(t);
	int pattern = (int)Thread_Pop(t), type = (int)Thread_Pop(t);
	uint32_t rate1 = 0, rate2 = 0, r;
	Bmp_t* list = PtclAnimFrames(t, firstBmp, count, a, b, frames, &rate1, &rate2);
	r = Pattern_SetAnimation(type, pattern, count, list, (int32_t)rate1, (int32_t)rate2);
	BGI_Free(list);
	PtclAnimResult(t, r, type, pattern, firstBmp, count);
	return 0;
}

/* 1.640 on: "C0 18" for the alternate image of a type 1 pattern - a second
 * animation cross-faded into the pattern's picture by `level` (0 .. 0x100:
 * 0 the picture alone, 0x100 the alternate frame alone; prtclctrl._bp
 * fades a new animation in through it); only type 1 has one */
static int Opcode_Ext1_PtclAltAnimation(Thread_t* t) // C0 1A (1.640 on): type, pattern, count, firstBmp, a, b, frames, level →
{
	uint32_t level = Thread_Pop(t);
	int32_t frames = (int32_t)Thread_Pop(t), b = (int32_t)Thread_Pop(t), a = (int32_t)Thread_Pop(t);
	int firstBmp = (int)Thread_Pop(t), count = (int)Thread_Pop(t);
	int pattern = (int)Thread_Pop(t), type = (int)Thread_Pop(t);
	uint32_t rate1 = 0, rate2 = 0, r;
	Bmp_t* list;
	if(level > 0x100)
		EXT_ERROR(t, MSG_BAD_MIX_RATIO, level);
	list = PtclAnimFrames(t, firstBmp, count, a, b, frames, &rate1, &rate2);
	r = Pattern_SetAltAnimation(type, pattern, count, list, (int32_t)rate1, (int32_t)rate2, (int)level);
	BGI_Free(list);
	PtclAnimResult(t, r, type, pattern, firstBmp, count);
	return 0;
}

// 1.640 on: the mix level of the alternate image of a type 1 pattern ("C0 1A")
static int Opcode_Ext1_PtclAltLevel(Thread_t* t) // C0 1B (1.640 on): type, pattern, level →
{
	uint32_t level = Thread_Pop(t);
	int pattern = (int)Thread_Pop(t), type = (int)Thread_Pop(t);
	if(level > 0x100)
		EXT_ERROR(t, MSG_BAD_MIX_RATIO, level);
	switch(Pattern_SetAltLevel(type, pattern, (int)level))
	{
		case 0x80000001u: EXT_ERROR(t, MSG_BAD_PTCL_TYPE, type); break;
		case 0x80000002u: EXT_ERROR(t, MSG_PTCL_NO_ALT_IMAGE, type, pattern); break;
		case 0x80000007u: EXT_ERROR(t, MSG_BAD_OBJ_PATTERN, pattern); break;
		default: break;
	}
	return 0;
}

// the managed bitmap drawn for the type A particles of a pattern (-1 for none)
static int Opcode_Ext1_PtclBitmapA(Thread_t* t) // C0 24: pattern, bmp →
{
	int bmp = (int)Thread_Pop(t);
	int pattern = (int)Thread_Pop(t);
	Ext1_BitmapResult(t, PatternA_BindBitmap(pattern, bmp), pattern, bmp);
	return 0;
}

// the managed bitmap drawn for the type B particles of a pattern (-1 for none)
static int Opcode_Ext1_PtclBitmapB(Thread_t* t) // C0 2C: pattern, bmp →
{
	int bmp = (int)Thread_Pop(t);
	int pattern = (int)Thread_Pop(t);
	Ext1_BitmapResult(t, PatternB_BindBitmap(pattern, bmp), pattern, bmp);
	return 0;
}

/* the birth parameters of the type A particles of a pattern (shared by
 * every screen): the spawn spread in x and z, the start height, the base
 * and variance of the velocity per axis, and the wind weight, which is
 * passed by address (the original copies it into a local and hands that
 * on).  An unknown pattern is a script error. */
static int Opcode_Ext1_PtclParamA(Thread_t* t) // C0 25: pattern, xSpread, yStart, zSpread, vxBase, vxVar, vyBase, vyVar, vzBase, vzVar, &windWeight →
{
	const int32_t* windWeight = (const int32_t*)PopPtr(t);
	int32_t vzVar = (int32_t)Thread_Pop(t);
	int32_t vzBase = (int32_t)Thread_Pop(t);
	int32_t vyVar = (int32_t)Thread_Pop(t);
	int32_t vyBase = (int32_t)Thread_Pop(t);
	int32_t vxVar = (int32_t)Thread_Pop(t);
	int32_t vxBase = (int32_t)Thread_Pop(t);
	int32_t zSpread = (int32_t)Thread_Pop(t);
	int32_t yStart = (int32_t)Thread_Pop(t);
	int32_t xSpread = (int32_t)Thread_Pop(t);
	int pattern = (int)Thread_Pop(t);
	if(!PatternA_Set(pattern, xSpread, yStart, zSpread, vxBase, vxVar, vyBase, vyVar, vzBase, vzVar, *windWeight))
		EXT_ERROR(t, MSG_BAD_PATTERN_NO, pattern);
	return 0;
}

/* the birth parameters of the type B particles of a pattern: those of
 * "C0 25" plus a life time, a turn rate (base and variance each), fade-in
 * and fade-out times and an effect mode.  An unknown pattern is a script
 * error. */
static int Opcode_Ext1_PtclParamB(Thread_t* t) // C0 2D: pattern, lifeBase, lifeVar, xSpread, yStart, zSpread, vxBase, vxVar, vyBase, vyVar, vzBase, vzVar, turnBase, turnVar, &windWeight, fadeIn, fadeOut, effect →
{
	int32_t effect = (int32_t)Thread_Pop(t);
	int32_t fadeOut = (int32_t)Thread_Pop(t);
	int32_t fadeIn = (int32_t)Thread_Pop(t);
	const int32_t* windWeight = (const int32_t*)PopPtr(t);
	int32_t turnVar = (int32_t)Thread_Pop(t);
	int32_t turnBase = (int32_t)Thread_Pop(t);
	int32_t vzVar = (int32_t)Thread_Pop(t);
	int32_t vzBase = (int32_t)Thread_Pop(t);
	int32_t vyVar = (int32_t)Thread_Pop(t);
	int32_t vyBase = (int32_t)Thread_Pop(t);
	int32_t vxVar = (int32_t)Thread_Pop(t);
	int32_t vxBase = (int32_t)Thread_Pop(t);
	int32_t zSpread = (int32_t)Thread_Pop(t);
	int32_t yStart = (int32_t)Thread_Pop(t);
	int32_t xSpread = (int32_t)Thread_Pop(t);
	int32_t lifeVar = (int32_t)Thread_Pop(t);
	int32_t lifeBase = (int32_t)Thread_Pop(t);
	int pattern = (int)Thread_Pop(t);
	if(!PatternB_Set(pattern, lifeBase, lifeVar, xSpread, yStart, zSpread, vxBase, vxVar, vyBase, vyVar, vzBase,
		   vzVar, turnBase, turnVar, *windWeight, fadeIn, fadeOut, effect))
		EXT_ERROR(t, MSG_BAD_PATTERN_NO, pattern);
	return 0;
}

// -------------------------------------------------------------------------
// rain screens
// -------------------------------------------------------------------------

/* create a rain screen of w x h pixels and push its handle; no free slot is
 * a script error.  The original tests for result 2 as the size error while
 * the manager reports 3, so a rejected size raises no error and the unset
 * handle (0) is pushed; that is reproduced. */
static int Opcode_Ext1_RainCreate(Thread_t* t) // C0 40: w, h → handle
{
	uint32_t handle = 0;
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	switch(Gfx_RainCreate(gGfx, &handle, w, h))
	{
		case 1: ScriptError(MSG_NO_MORE_RAIN_SCREENS, t); break;
		case 2: EXT_ERROR(t, MSG_BAD_FX_SCREEN_SIZE, w, h); break;
		default: break;
	}
	Thread_Push(t, handle);
	return 0;
}

static int Opcode_Ext1_RainDelete(Thread_t* t) // C0 41: handle →
{
	if(!Gfx_RainDelete(gGfx, Thread_Pop(t)))
		ScriptError(MSG_BAD_RAIN_HANDLE, t);
	return 0;
}

// start the rain with a fresh generator, aged by prerollMs milliseconds so it does not begin empty
static int Opcode_Ext1_RainStart(Thread_t* t) // C0 42: handle, prerollMs →
{
	uint32_t preroll = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!Gfx_RainStart(gGfx, h, preroll))
		ScriptError(MSG_BAD_RAIN_HANDLE, t);
	return 0;
}

/* the mask bitmap of the screen: a gray (mode 3) bitmap of the screen's
 * own size, or -1 for none; a missing or unsuitable bitmap is a script error */
static int Opcode_Ext1_RainSetMask(Thread_t* t) // C0 43: handle, bmp →
{
	int bmp = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(Gfx_RainSetMask(gGfx, h, bmp))
	{
		case 4: EXT_ERROR(t, MSG_RAIN_MASK_NOT_EXIST, bmp); break;
		case 5: EXT_ERROR(t, MSG_RAIN_MASK_INVALID, bmp); break;
		case NO_OBJECT: ScriptError(MSG_BAD_RAIN_HANDLE, t); break;
		default: break;
	}
	return 0;
}

static int Opcode_Ext1_RainShow(Thread_t* t) // C0 44: handle, on →
{
	int on = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!Gfx_RainShow(gGfx, h, on))
		ScriptError(MSG_BAD_RAIN_HANDLE, t);
	return 0;
}

// place the screen at (x, y) on the display with an effect mode, a level and a priority
static int Opcode_Ext1_RainSetDraw(Thread_t* t) // C0 45: handle, x, y, effect, level, prio →
{
	int prio = (int)Thread_Pop(t);
	int level = (int)Thread_Pop(t);
	int effect = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority((uint32_t)prio, t);
	CheckAlpha((uint32_t)level, t);
	CheckEffectMode((uint32_t)effect, t);
	if(Gfx_RainSetDraw(gGfx, h, x, y, effect, level, prio) == NO_OBJECT)
		ScriptError(MSG_BAD_RAIN_HANDLE, t);
	return 0;
}

/* the box the drops live in: they are born at height yTop with x in
 * [xMin, xMax) and z in the given range, and die at or below yBottom */
static int Opcode_Ext1_RainSetArea(Thread_t* t) // C0 46: handle, xMin, yTop, zMin, xMax, yBottom, zMax →
{
	int zMax = (int)Thread_Pop(t);
	int yBottom = (int)Thread_Pop(t);
	int xMax = (int)Thread_Pop(t);
	int zMin = (int)Thread_Pop(t);
	int yTop = (int)Thread_Pop(t);
	int xMin = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(Gfx_RainSetArea(gGfx, h, xMin, yTop, zMin, xMax, yBottom, zMax) == NO_OBJECT)
		ScriptError(MSG_BAD_RAIN_HANDLE, t);
	return 0;
}

/* map the result of a one-value rain setter to a script error: 3 the value
 * was rejected (`badValueMsg` takes it), NO_OBJECT unknown handle */
static void Ext1_RainValueResult(Thread_t* t, int r, const char* badValueMsg, int value)
{
	if(r == 3)
		EXT_ERROR(t, badValueMsg, value);
	else if(r == NO_OBJECT)
		ScriptError(MSG_BAD_RAIN_HANDLE, t);
}

// the distance a drop falls per stage (0 is rejected)
static int Opcode_Ext1_RainSetFall(Thread_t* t) // C0 47: handle, n →
{
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	Ext1_RainValueResult(t, Gfx_RainSetFall(gGfx, h, n), MSG_BAD_RAIN_FALL, n);
	return 0;
}

// the length of a drop's streak (0 is rejected)
static int Opcode_Ext1_RainSetLength(Thread_t* t) // C0 48: handle, n →
{
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	Ext1_RainValueResult(t, Gfx_RainSetLength(gGfx, h, n), MSG_BAD_RAIN_LENGTH, n);
	return 0;
}

// the ARGB colour of the streaks
static int Opcode_Ext1_RainSetColour(Thread_t* t) // C0 49: handle, argb →
{
	uint32_t argb = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(Gfx_RainSetColour(gGfx, h, argb) == NO_OBJECT)
		ScriptError(MSG_BAD_RAIN_HANDLE, t);
	return 0;
}

// the number of drops born per stage
static int Opcode_Ext1_RainSetPerStage(Thread_t* t) // C0 4A: handle, n →
{
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(Gfx_RainSetPerStage(gGfx, h, n) == NO_OBJECT)
		ScriptError(MSG_BAD_RAIN_HANDLE, t);
	return 0;
}

// the milliseconds per stage (0 is rejected)
static int Opcode_Ext1_RainSetStageMs(Thread_t* t) // C0 4B: handle, ms →
{
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	Ext1_RainValueResult(t, Gfx_RainSetStageMs(gGfx, h, n), MSG_BAD_RAIN_STAGE_TIME, n);
	return 0;
}

// the camera position
static int Opcode_Ext1_RainSetCamera(Thread_t* t) // C0 4C: handle, x, y, z →
{
	int z = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(Gfx_RainSetCamera(gGfx, h, x, y, z) == NO_OBJECT)
		ScriptError(MSG_BAD_RAIN_HANDLE, t);
	return 0;
}

// the camera angles, in 0.1 degree units
static int Opcode_Ext1_RainSetAngles(Thread_t* t) // C0 4D: handle, a, b, c →
{
	int c = (int)Thread_Pop(t);
	int b = (int)Thread_Pop(t);
	int a = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(Gfx_RainSetAngles(gGfx, h, a, b, c) == NO_OBJECT)
		ScriptError(MSG_BAD_RAIN_HANDLE, t);
	return 0;
}

// the projection distance, which is also the near plane (0 is rejected)
static int Opcode_Ext1_RainSetDistance(Thread_t* t) // C0 4E: handle, n →
{
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	Ext1_RainValueResult(t, Gfx_RainSetDistance(gGfx, h, n), MSG_BAD_RAIN_PROJECTION, n);
	return 0;
}

/* switch the particle and rain effects on or off and set their frame rate;
 * a rate outside 1 .. 1000 is a script error */
static int Opcode_Ext1_EffectFps(Thread_t* t) // C0 4F: enable, fps →
{
	uint32_t fps = Thread_Pop(t);
	int enable = (int)Thread_Pop(t);
	if(!Fx_SetEffectFps(enable, fps))
		EXT_ERROR(t, MSG_BAD_FRAME_RATE, fps);
	return 0;
}

// -------------------------------------------------------------------------
// wind tables
// -------------------------------------------------------------------------

/* load a "bwef" table from the file `name` of archive `arc` into `out` (8
 * bytes per entry; `base` is added to every entry's first number) and its
 * entry count into *count.  Pushes 0, or 0x80000001 for a missing or empty
 * file, 0x80000002 for a bad magic, 0x80000003 for a size that does not
 * match the count; no script error.
 *
 * The argument order is the one the programs push: Nursery Rhyme's
 * anmmngrm calls it as (out, &count, "nrarc05.arc", <voice name>, <first
 * mouth bitmap>) for the lip data of a voice - one entry per step, the
 * mouth frame to show (plus `base`) and the time of a step. */
static int Opcode_Ext1_LoadBwef(Thread_t* t) // C0 F0: out, &count, arc, name, base → result
{
	uint32_t base = Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	int32_t* count = (int32_t*)PopPtr(t);
	BwefEntry_t* out = (BwefEntry_t*)PopPtr(t);
	Thread_Push(t, (uint32_t)LoadBwef(arc, name, out, count, base));
	return 0;
}

// register the handlers of the family (Vm_FillTable picks those of the selected engine profile)
void Vm_OptableC0Init(void)
{
	static const OpEntry_t ops[] = {
		{0x00, Opcode_Ext1_PtclCreate, GEN_FIRST, GEN_LAST},
		{0x01, Opcode_Ext1_PtclDelete, GEN_FIRST, GEN_LAST},
		{0x04, Opcode_Ext1_PtclShow, GEN_FIRST, GEN_LAST},
		{0x05, Opcode_Ext1_PtclSetDraw, GEN_FIRST, GEN_LAST},
		{0x08, Opcode_Ext1_PtclRedraw, GEN_FIRST, GEN_LAST},
		{0x09, Opcode_Ext1_PtclSetAuto, GEN_FIRST, GEN_LAST},
		{0x0A, Opcode_Ext1_PtclSetRectLimit, GEN_FIRST, GEN_LAST},
		{0x0B, Opcode_Ext1_PtclSetCamera, GEN_FIRST, GEN_LAST},
		{0x0C, Opcode_Ext1_PtclSetInterval, GEN_FIRST, GEN_LAST},
		{0x0D, Opcode_Ext1_PtclPrerun, GEN_FIRST, GEN_LAST},
		{0x0F, Opcode_Ext1_PtclClear, GEN_FIRST, GEN_LAST},
		{0x10, Opcode_Ext1_PtclSetWind, GEN_FIRST, GEN_LAST},
		{0x20, Opcode_Ext1_PtclGroupA, GEN_FIRST, GEN_LAST},
		{0x18, Opcode_Ext1_PtclAnimation, GEN_1_494, GEN_LAST},
		{0x1A, Opcode_Ext1_PtclAltAnimation, GEN_1_640, GEN_LAST},
		{0x1B, Opcode_Ext1_PtclAltLevel, GEN_1_640, GEN_LAST},
		{0x1F, Opcode_Ext1_PtclAnimMode, GEN_1_494, GEN_LAST},
		{0x24, Opcode_Ext1_PtclBitmapA, GEN_FIRST, GEN_LAST},
		{0x25, Opcode_Ext1_PtclParamA, GEN_FIRST, GEN_LAST},
		{0x28, Opcode_Ext1_PtclGroupB, GEN_FIRST, GEN_LAST},
		{0x29, Opcode_Ext1_PtclPatternBExtra, GEN_1_494, GEN_LAST},
		{0x2C, Opcode_Ext1_PtclBitmapB, GEN_FIRST, GEN_LAST},
		{0x2D, Opcode_Ext1_PtclParamB, GEN_FIRST, GEN_LAST},
		{0x40, Opcode_Ext1_RainCreate, GEN_FIRST, GEN_LAST},
		{0x41, Opcode_Ext1_RainDelete, GEN_FIRST, GEN_LAST},
		{0x42, Opcode_Ext1_RainStart, GEN_FIRST, GEN_LAST},
		{0x43, Opcode_Ext1_RainSetMask, GEN_FIRST, GEN_LAST},
		{0x44, Opcode_Ext1_RainShow, GEN_FIRST, GEN_LAST},
		{0x45, Opcode_Ext1_RainSetDraw, GEN_FIRST, GEN_LAST},
		{0x46, Opcode_Ext1_RainSetArea, GEN_FIRST, GEN_LAST},
		{0x47, Opcode_Ext1_RainSetFall, GEN_FIRST, GEN_LAST},
		{0x48, Opcode_Ext1_RainSetLength, GEN_FIRST, GEN_LAST},
		{0x49, Opcode_Ext1_RainSetColour, GEN_FIRST, GEN_LAST},
		{0x4A, Opcode_Ext1_RainSetPerStage, GEN_FIRST, GEN_LAST},
		{0x4B, Opcode_Ext1_RainSetStageMs, GEN_FIRST, GEN_LAST},
		{0x4C, Opcode_Ext1_RainSetCamera, GEN_FIRST, GEN_LAST},
		{0x4D, Opcode_Ext1_RainSetAngles, GEN_FIRST, GEN_LAST},
		{0x4E, Opcode_Ext1_RainSetDistance, GEN_FIRST, GEN_LAST},
		{0x4F, Opcode_Ext1_EffectFps, GEN_FIRST, GEN_LAST},
		{0xF0, Opcode_Ext1_LoadBwef, GEN_FIRST, GEN_LAST},
	};
	Vm_FillTable(vm_optable_C0, OPFAM_C0, ops, BGI_COUNTOF(ops));
}
