/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wait_quake.c - the screen quake of "B0 08" (WaitQuake)
 *
 * Interface: waitobj.h (StartQuake).
 *
 * While it runs the normal presenting is suspended (Present_Allow(0)) and
 * every frame the whole back buffer is presented shifted by the quake's
 * offset.  The offset is a triangle wave of the amplitude at `frequency`
 * cycles per second, sampled `fps` times a second; pattern 0 shakes
 * horizontally, 1 vertically, 2 diagonally, 3 along a random direction that
 * changes every cycle (interpolated over the cycle).  After each cycle the
 * amplitude decays by `decay` percent; `repeat` cycles end the wait, as does
 * any input when `skip` is set.  The wait pushes nothing; it repaints the
 * window unshifted when it ends.
 */
#include "bgi/wait.h"
#include "bgi/waitobj.h"
#include "bgi/input.h"
#include "bgi/inputmap.h"
#include "bgi/display.h"
#include "bgi/sys.h"

typedef struct WaitQuake // 0x70 bytes
{
	Wait_t w;
	int active;           // the setup succeeded and the cycles are not over
	int pattern;          // 0 horizontal, 1 vertical, 2 diagonal, 3 random direction
	int32_t amplitude;    // pixels << 11 (the wave is sampled with 12 fraction bits)
	int frequency;        // cycles per second
	int repeat;           // cycles
	int decay;            // percent per cycle
	int fps;              // frames per second
	int skip;             // input ends the wait (an input layer is pushed)
	int framesPerCycle;   // fps / frequency
	int32_t phase;        // the triangle wave value
	int32_t step;         // per frame
	int cycle;            // cycles completed
	int frame;            // within the cycle
	int32_t dirStart[2];  // pattern 3: offset at the start of the cycle
	int32_t dirEnd[2];    // and at its end
	int32_t dirDelta[2];  // dirEnd - dirStart
	uint32_t inputSerial; // input state when the wait started
} WaitQuake_t;

static void WaitQuake_Destroy(OpenBGI_t* o, Wait_t* w);
static int WaitQuake_Poll(OpenBGI_t* o, Wait_t* w);

static const WaitVtbl_t WaitQuake_Vtbl = {
	WaitQuake_Destroy, WaitQuake_Poll, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage};

// presenting is suspended from the constructor on, even when the setup then fails
static void WaitQuake_Ctor(OpenBGI_t* o, WaitQuake_t* w, Thread_t* t)
{
	Wait_Ctor(o, &w->w, t);
	w->w.vt = &WaitQuake_Vtbl;
	Present_Allow(o, 0);
	w->active = 0;
	w->skip = 0;
}

static void WaitQuake_Dtor(OpenBGI_t* o, WaitQuake_t* w)
{
	w->w.vt = &WaitQuake_Vtbl;
	if(w->skip)
	{
		Input_Poll(o, 0xffffffffu, 0xffffffffu);
		MouseLayer_Pop(o, 0xffffffffu);
		KeyLayer_Pop(o, 0xffffffffu);
	}
	Present_Allow(o, 1);
	Wait_Dtor(o, &w->w);
}

static void WaitQuake_Destroy(OpenBGI_t* o, Wait_t* w)
{
	WaitQuake_Dtor(o, (WaitQuake_t*)w);
	BGI_Free(w);
}

/* validate and store the parameters, arm the timer and (with skip) push
 * the input layers: 0 / 0x80000001 bad pattern / 0x80000002 frequency < 1 /
 * 0x80000003 repeat < 1 / 0x80000004 fps < 1 or below the frequency.  The
 * input layers of a skippable quake use the top-most key 0xffffffff. */
static uint32_t WaitQuake_Setup(OpenBGI_t* o, WaitQuake_t* w, int pattern, int amplitude, int frequency, int repeat, int decay,
	int fps, int skip)
{
	if((uint32_t)pattern >= 4)
		return 0x80000001;
	if(frequency < 1)
		return 0x80000002;
	if(repeat < 1)
		return 0x80000003;
	if(fps < 1 || fps < frequency)
		return 0x80000004;
	if(w->skip)
	{
		MouseLayer_Pop(o, 0xffffffffu);
		KeyLayer_Pop(o, 0xffffffffu);
	}
	w->pattern = pattern;
	w->decay = decay;
	w->repeat = repeat;
	w->amplitude = amplitude << 11;
	w->frequency = frequency;
	w->fps = fps;
	w->skip = skip;
	w->cycle = 0;
	w->frame = 0;
	w->dirEnd[0] = w->dirEnd[1] = 0;
	w->framesPerCycle = fps / frequency;
	if(skip)
	{
		MouseLayer_Push(o, 0xffffffffu);
		KeyLayer_Push(o, 0xffffffffu);
		Input_Poll(o, 0xffffffffu, 0xffffffffu);
		w->inputSerial = InputSerial_Get(o);
	}
	w->w.vt->setTimer(o, &w->w, 0);
	w->active = 1;
	return 0;
}

// a random value in 0 .. n, the mean of eight 30-bit draws
static int32_t Quake_Random(OpenBGI_t* o, int32_t n)
{
	int32_t sum = 0;
	int i;
	for(i = 0; i < 8; i++)
	{
		int32_t r = (BGI_Rand(o) << 15) | BGI_Rand(o);
		sum += r % (n + 1);
	}
	return sum >> 3;
}

/* pattern 3 only: a random offset of up to the amplitude in the
 * quadrant selected by `quadrant` (0 ++, 1 -+, 2 +-, 3 --).  1 when written. */
static int Quake_RandomDir(OpenBGI_t* o, WaitQuake_t* w, int32_t out[2], int quadrant)
{
	int32_t rx, ry;
	if(w->pattern != 3)
		return 0;
	if(w->amplitude <= 0)
	{
		out[0] = out[1] = 0;
		return 1;
	}
	rx = Quake_Random(o, w->amplitude);
	ry = Quake_Random(o, w->amplitude);
	switch(quadrant)
	{
		case 0:
			out[0] = rx;
			out[1] = ry;
			break;
		case 1:
			out[0] = -rx;
			out[1] = ry;
			break;
		case 2:
			out[0] = rx;
			out[1] = -ry;
			break;
		default:
			out[0] = -rx;
			out[1] = -ry;
			break;
	}
	return 1;
}

/* one pass.  A frame is produced each 1000 / fps ms: the triangle wave
 * advances by `step` and reflects at +-amplitude; patterns with a random
 * direction add the interpolated direction.  The frame is skipped (not
 * presented) when the timer has already expired again.  The wait ends
 * after `repeat` cycles, on input (skip) or when waits stop blocking; a
 * wait whose setup failed ends only in the latter case. */
static int WaitQuake_Poll(OpenBGI_t* o, Wait_t* base)
{
	// which patterns use the wave on x, the random direction, the wave on y
	static const int kWaveX[4] = {1, 0, 1, 0}, kRandom[4] = {0, 0, 0, 1}, kWaveY[4] = {0, 1, 1, 0};
	WaitQuake_t* w = (WaitQuake_t*)base;
	int cont;
	if(!w->active) // (only before a successful setup)
	{
		if(Wait_IsBlocking(o, base))
			return 0;
		goto end;
	}
	cont = w->skip ? w->inputSerial == InputSerial_Get(o) : 1;
	if(cont && Wait_TimerExpired(o, base))
	{
		Wait_TimerAdd(base, (uint32_t)(1000 / w->fps));
		if(w->frame == 0)
		{ // a new cycle: full wave period over the cycle's frames, new random direction
			w->step = (w->amplitude * 4) / w->framesPerCycle;
			w->phase = 0;
			w->dirStart[0] = w->dirEnd[0];
			w->dirStart[1] = w->dirEnd[1];
			Quake_RandomDir(o, w, w->dirEnd, w->cycle & 3);
			w->dirDelta[0] = w->dirEnd[0] - w->dirStart[0];
			w->dirDelta[1] = w->dirEnd[1] - w->dirStart[1];
		}
		w->phase += w->step;
		if(w->phase > w->amplitude || w->phase < -w->amplitude)
			w->step = -w->step;
		if(w->phase > w->amplitude)
			w->phase = 2 * w->amplitude - w->phase;
		if(w->phase < -w->amplitude)
			w->phase = -w->phase - 2 * w->amplitude;
		if(!Wait_TimerExpired(o, base))
		{
			int32_t x = kWaveX[w->pattern] ? w->phase : 0;
			int32_t y = kWaveY[w->pattern] ? w->phase : 0;
			if(kRandom[w->pattern])
			{
				x += w->dirDelta[0] * w->frame / w->framesPerCycle + w->dirStart[0];
				y += w->dirDelta[1] * w->frame / w->framesPerCycle + w->dirStart[1];
			}
			Window_PresentOffset(o, x >> 12, y >> 12);
		}
		if(++w->frame >= w->framesPerCycle)
		{
			w->frame = 0;
			w->amplitude = (100 - w->decay) * w->amplitude / 100;
			cont = ++w->cycle < w->repeat;
		}
	}
	w->active = cont;
	if(cont && Wait_IsBlocking(o, base))
		return 0;
end:
	Window_Repaint(o);
	return 1;
}

/* allocate the quake into *out and set it up; 0 or the setup's codes (see
 * WaitQuake_Setup).  *out is set before the validation, as in the
 * original; the "B0 08" handler raises a script error for a rejected one
 * and installs the object only on 0. */
int StartQuake(OpenBGI_t* o, Thread_t* t, int pattern, int amplitude, int frequency, int repeat, int decay, int fps, int skip,
	Wait_t** out)
{
	WaitQuake_t* w = (WaitQuake_t*)BGI_Alloc(sizeof *w);
	WaitQuake_Ctor(o, w, t);
	*out = &w->w;
	return (int)WaitQuake_Setup(o, w, pattern, amplitude, frequency, repeat, decay, fps, skip);
}

// the name of a wait class of this file, for the debugger (NULL: not one of these)
/* The targets of the quake (bgi/inputmap.h): one that may be skipped
 * ends on any input, a click anywhere included, through the top-most
 * layer; 1 when `w` is a quake. */
int WaitQuake_Targets(OpenBGI_t* o, Wait_t* w, TargetSink_t* s)
{
	const WaitQuake_t* q = (const WaitQuake_t*)w;
	TargetDesc_t d;
	if(w->vt != &WaitQuake_Vtbl)
		return 0;
	if(!q->skip)
		return 1;
	TargetDesc_Init(&d, OPENBGI_TARGET_WAIT, 0xffffffffu);
	TargetSink_Claim(s, d.layer);
	d.flags = OPENBGI_TARGET_ANYWHERE;
	d.whole = 1;
	d.owner = Thread_GetId(w->thread);
	d.reach = Target_ReachLayer;
	TargetSink_Add(s, &d);
	return 1;
}

const char* WaitQuake_ClassName(const WaitVtbl_t* vt)
{
	if(vt == &WaitQuake_Vtbl)
		return "WaitQuake";
	return NULL;
}
