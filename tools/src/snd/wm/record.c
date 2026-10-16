/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * record.c - the per-channel records of the Wave Master library: the four
 *            volumes that make up a channel's level, the two fades a 20 ms
 *            timer steps, and the checks every call makes (wm_internal.h)
 */
#include "wm_internal.h"

// every volume at full (0x80), no fades, nothing loaded; the voice pointer is cleared as well
void Record_Reset(WmRecord_t* r)
{
	memset(r, 0, sizeof *r);
	r->master = 0x80;
	r->aux = 0x80;
	r->volume = 0x80;
	r->fadeLevel = 0x80;
}

/* the attenuation in dB from the four volumes (each 0x80 = full),
 * 0.375 dB per step below full; 48 dB or more is cut to silence (0x80) */
int Record_Level(const WmRecord_t* r)
{
	int level = BGI_Ftol((double)(0x200 - r->fadeLevel - r->volume - r->aux - r->master) * 0.375);
	return level < 0x30 ? level : 0x80;
}

/* one fade step at `now`: the value interpolated linearly in time; the
 * fade ends (goes inactive) once its target is reached */
int32_t Fade_Step(Fade_t* f, uint32_t now)
{
	int32_t span = (int32_t)(f->end - f->start);
	int32_t range = f->to - f->from;
	double t;
	if(span > 0)
	{
		t = (double)(int32_t)(now - f->start) / (double)span;
		if(!(t < 1.0))
		{
			t = 1.0;
			f->active = 0;
		}
	}
	else
	{ // a zero-length fade jumps to its target
		t = 1.0;
		f->active = 0;
	}
	return BGI_Ftol((double)range * t + (double)f->from);
}

// start a fade from `from` to `to` over `ms` milliseconds from `now`
void Fade_Start(Fade_t* f, uint32_t now, int ms, int32_t from, int32_t to)
{
	f->active = 1;
	f->start = now;
	f->end = now + (uint32_t)ms;
	f->from = from;
	f->to = to;
}

/* step the fades of a table at `now` and apply the changed levels to the
 * voices (the 20 ms timer tick) */
void Records_Tick(WmRecord_t* recs, int count, uint32_t now)
{
	int i;
	for(i = 0; i < count; i++)
	{
		WmRecord_t* r = &recs[i];
		int changed = 0;
		if(!r->active)
			continue;
		if(r->fadeVol.active)
		{
			r->volume = Fade_Step(&r->fadeVol, now);
			changed = 1;
		}
		if(r->fade.active)
		{
			r->fadeLevel = Fade_Step(&r->fade, now);
			changed = 1;
		}
		if(changed)
			Voice_SetVolume(r->voice, Record_Level(r));
	}
}

// the library is initialised and its timer runs
int Wm_Running(void)
{
	return (gWmFlags & 3) == 3;
}

// the checks of an effect call: 0, or WM_E_NOT_RUNNING, WM_E_BAD_CHANNEL, WM_E_NOT_LOADED
int Wm_SeCheck(int slot)
{
	if(!Wm_Running())
		return WM_E_NOT_RUNNING;
	if((uint32_t)slot >= WM_SE_SLOTS)
		return WM_E_BAD_CHANNEL;
	return gSe[slot].active ? 0 : WM_E_NOT_LOADED;
}

// the checks of a music call: the same codes for a channel
int Wm_BgmCheck(int ch)
{
	if(!Wm_Running())
		return WM_E_NOT_RUNNING;
	if((uint32_t)ch >= WM_BGM_CHANNELS)
		return WM_E_BAD_CHANNEL;
	return gBgm[ch].active ? 0 : WM_E_NOT_LOADED;
}
