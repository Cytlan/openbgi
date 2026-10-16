/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wind.c - the wind generator of a particle engine: a vector that holds
 *          for a random time, then glides linearly to a new random target
 *          ("C0 10").  Interface: particle_internal.h.
 *
 * The engine steps the wind once per tick and adds the current vector,
 * scaled by each particle's wind weight, to every live particle
 * (Particle_ApplyForce).  Times arrive in milliseconds and are converted
 * to ticks with the engine's tick length.
 */
#include "particle_internal.h"

void Wind_Ctor(Wind_t* w)
{
	memset(w, 0, sizeof *w);
}

// milliseconds to ticks; 0 when there is no tick length
static int Wind_RecalcTimes(Wind_t* w)
{
	if(w->interval == 0)
		return 0;
	w->holdT = w->holdMs / w->interval;
	w->holdVarT = w->holdVarMs / w->interval;
	w->moveT = w->moveMs / w->interval;
	w->moveVarT = w->moveVarMs / w->interval;
	return 1;
}

// the engine's tick length changed ("C0 0C"): convert the four times again
void Wind_SetInterval(Wind_t* w, uint32_t ms)
{
	w->interval = ms;
	Wind_RecalcTimes(w);
}

int Wind_Get(const Wind_t* w, int32_t out[3]) // the current vector, zero when disabled; returns the enabled flag
{
	if(w->enabled)
	{
		out[0] = w->cur[0];
		out[1] = w->cur[1];
		out[2] = w->cur[2];
	}
	else
		out[0] = out[1] = out[2] = 0;
	return w->enabled;
}

void Wind_SetCurrent(Wind_t* w, int32_t x, int32_t y, int32_t z)
{
	w->cur[0] = x;
	w->cur[1] = y;
	w->cur[2] = z;
}

// draw a random target: base - var + Rnd(2 var) per axis
static void Wind_Pick(const Wind_t* w, int32_t out[3])
{
	int i;
	for(i = 0; i < 3; i++)
		out[i] = Ptcl_Rnd(2 * w->var[i]) + w->base[i] - w->var[i];
}

// enter a phase of `len` ticks (at least 1)
static void Wind_SetPhase(Wind_t* w, int phase, uint32_t len)
{
	w->phase = phase;
	w->pos = 0;
	w->len = len != 0 ? len : 1;
}

// begin a hold; `first` draws both vectors, else the old target becomes the start
static void Wind_NextTarget(Wind_t* w, int first)
{
	Wind_SetPhase(w, 0, w->holdT + (uint32_t)Ptcl_Rnd((int32_t)w->holdVarT));
	if(first)
	{
		Wind_Pick(w, w->from);
		Wind_Pick(w, w->to);
	}
	else
	{
		w->from[0] = w->to[0];
		w->from[1] = w->to[1];
		w->from[2] = w->to[2];
		Wind_Pick(w, w->to);
	}
}

/* "C0 10": the 11 values enable, base x, var x, base y, var y, base z,
 * var z, hold ms, hold spread ms, glide ms, glide spread ms.  The vectors
 * arrive 16.16 and are stored 24.8.  The wind is enabled only when
 * `enable` is set and the engine has a tick length; a fresh hold with two
 * new random vectors starts at once.  Always 1. */
int Wind_Set(Wind_t* w, const int32_t a[11])
{
	w->base[0] = a[1] >> 8;
	w->var[0] = a[2] >> 8;
	w->base[1] = a[3] >> 8;
	w->var[1] = a[4] >> 8;
	w->base[2] = a[5] >> 8;
	w->var[2] = a[6] >> 8;
	w->holdMs = (uint32_t)a[7];
	w->holdVarMs = (uint32_t)a[8];
	w->moveMs = (uint32_t)a[9];
	w->moveVarMs = (uint32_t)a[10];
	w->enabled = a[0] != 0 && Wind_RecalcTimes(w);
	Wind_NextTarget(w, 1);
	return 1;
}

/* one tick: hold the vector, then glide linearly to the freshly drawn
 * target, then hold that one, and so on.  A disabled wind reports a zero
 * vector. */
void Wind_Step(Wind_t* w)
{
	if(!w->enabled)
	{
		Wind_SetCurrent(w, 0, 0, 0);
		return;
	}
	if(w->phase == 0)
		Wind_SetCurrent(w, w->from[0], w->from[1], w->from[2]);
	else if(w->phase == 1)
	{
		int32_t pos = (int32_t)w->pos, rest = (int32_t)(w->len - w->pos), len = (int32_t)w->len;
		Wind_SetCurrent(w, (w->to[0] * pos + w->from[0] * rest) / len, (w->to[1] * pos + w->from[1] * rest) / len,
			(w->to[2] * pos + w->from[2] * rest) / len);
	}
	if(++w->pos < w->len)
		return;
	if(w->phase == 0)
		Wind_SetPhase(w, 1, w->moveT + (uint32_t)Ptcl_Rnd((int32_t)w->moveVarT));
	else if(w->phase == 1)
		Wind_NextTarget(w, 0);
}
