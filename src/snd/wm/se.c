/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * se.c - the sound effect slots of the Wave Master library
 *        (src/include/bgi/snd/wavemaster.h): a BW file already in memory is
 *        decoded whole at load and played from its samples
 */
#include "wm_internal.h"
#include "bgi/core.h"

/* Load the BW file at `bwFile` (header and data, already in memory: the
 * loader read it) into `slot`, decoded completely, with a fade-in of
 * `fadeInMs` burnt into the samples and `gain` applied to them, then
 * played `rate` times as fast at the same pitch (WmStream_TimeStretch;
 * 1.0 leaves it alone).  The slot's previous sound is dropped; the new
 * one is loaded stopped, at full volume with no fades (its own volume of
 * "A0 2C" stays).  0 ok, 0x14 not running, 0x15 bad slot, 0xe not a BW
 * file (or an unknown codec), 0x16 the decoder could not be set up. */
uint32_t Wm_SeLoad(OpenBGI_t* o, int slot, const void* bwFile, int fadeInMs, double gain, double rate)
{
	const BwHeader_t* hdr = (const BwHeader_t*)bwFile;
	WmSource_t* src;
	WmStream_t* s;
	WmRecord_t* r;
	int err;
	uint32_t res = 0;
	if(!Wm_Running(o))
		res = WM_E_NOT_RUNNING;
	else if((uint32_t)slot >= WM_SE_SLOTS)
		res = WM_E_BAD_CHANNEL;
	else
	{
		r = &o->snd.se[slot];
		// the original trusts the header; a non-BW file is caught here, before its size is believed for the copy
		if(memcmp(hdr->magic, "bw  ", 4) != 0 || hdr->headerSize < sizeof *hdr)
			return WM_E_NOT_BW;
		src = WmSource_FromMemory(bwFile, hdr->headerSize + hdr->dataSize);
		s = WmStream_Open(o, src, gain, 1, &err);
		if(!s)
			res = err == WM_STREAM_ERR_DECODER ? WM_E_LOAD_FAILED : WM_E_NOT_BW; // (the original answers 0xe for any open failure)
		else
		{
			WmStream_FadeIn(s, (uint32_t)fadeInMs); // the result is ignored: a fade longer than the sound changes nothing
			if(rate != 1.0)
				WmStream_TimeStretch(s, rate);
			SndOut_Enter(o); // (the sound was decoded outside: the reader waits for the slot's change only)
			Voice_Attach(r->voice, s);
			r->stream = s;
			r->fadeVol.active = 0;
			r->volume = 0x80;
			r->fadeLevel = 0x80;
			r->fade.active = 0;
			r->active = 1;
			Voice_SetVolume(r->voice, Record_Level(o, r));
			SndOut_Leave(o);
		}
	}
	return res;
}

// drop the slot's sound; 0, 0x14 not running, 0x15 bad slot (an empty slot is not an error)
int Wm_SeUnload(OpenBGI_t* o, int slot)
{
	int res = 0;
	SndOut_Enter(o);
	if(!Wm_Running(o))
		res = WM_E_NOT_RUNNING;
	else if((uint32_t)slot >= WM_SE_SLOTS)
		res = WM_E_BAD_CHANNEL;
	else
	{
		Voice_Detach(o->snd.se[slot].voice);
		o->snd.se[slot].stream = NULL;
		o->snd.se[slot].active = 0;
	}
	SndOut_Leave(o);
	return res;
}

// pan 0 .. 0x80 -> (pan - 0x40) * 2 for the voice (no clamping for effects, unlike the music side)
static void Se_SetPan(Voice_t* v, int pan)
{
	Voice_SetPan(v, BGI_Ftol((double)(pan - 0x40) * 0.015625 * 128.0));
}

/* start the slot's sound from the beginning at `volume` (0 .. 0x80, the
 * record's channel volume) and `pan` (0 .. 0x80, 0x40 centred); a
 * running fade-out is cancelled.  0, or the check's code. */
int Wm_SePlay(OpenBGI_t* o, int slot, int volume, int pan)
{
	int res;
	SndOut_Enter(o);
	res = Wm_SeCheck(o, slot);
	if(res == 0)
	{
		WmRecord_t* r = &o->snd.se[slot];
		Se_SetPan(r->voice, pan);
		r->volume = volume;
		r->fade.active = 0;
		r->fadeLevel = 0x80;
		Voice_Stop(r->voice);
		Voice_Play(r->voice, Record_Level(o, r));
	}
	SndOut_Leave(o);
	return res;
}

// stop the slot's sound and rewind it; 0, or the check's code
int Wm_SeStop(OpenBGI_t* o, int slot)
{
	int res;
	SndOut_Enter(o);
	res = Wm_SeCheck(o, slot);
	if(res == 0)
		Voice_Stop(o->snd.se[slot].voice);
	SndOut_Leave(o);
	return res;
}

// the slot's master volume (clamped to 0x80), applied at once; 0, 0x14 not running, 0x15 bad slot
int Wm_SeSetMasterVolume(OpenBGI_t* o, int slot, uint32_t vol)
{
	int res = 0;
	SndOut_Enter(o);
	if(!Wm_Running(o))
		res = WM_E_NOT_RUNNING;
	else if((uint32_t)slot >= WM_SE_SLOTS)
		res = WM_E_BAD_CHANNEL;
	else
	{
		if(vol > 0x80)
			vol = 0x80;
		o->snd.se[slot].master = (int32_t)vol;
		Voice_SetVolume(o->snd.se[slot].voice, Record_Level(o, &o->snd.se[slot]));
	}
	SndOut_Leave(o);
	return res;
}

/* the slot's own volume ("A0 2C", 1.588 on; clamped to 0x80), applied at
 * once and kept when the slot is loaded again; 0, 0x14 not running, 0x15
 * bad slot */
int Wm_SeSetChannelVolume(OpenBGI_t* o, int slot, uint32_t vol)
{
	int res = 0;
	SndOut_Enter(o);
	if(!Wm_Running(o))
		res = WM_E_NOT_RUNNING;
	else if((uint32_t)slot >= WM_SE_SLOTS)
		res = WM_E_BAD_CHANNEL;
	else
	{
		o->snd.se[slot].aux = (int32_t)(vol > 0x80 ? 0x80 : vol);
		Voice_SetVolume(o->snd.se[slot].voice, Record_Level(o, &o->snd.se[slot]));
	}
	SndOut_Leave(o);
	return res;
}

// fade the slot's fade level from where it is to silence over `ms`; 0, or the check's code
int Wm_SeFadeOut(OpenBGI_t* o, int slot, int ms)
{
	int res;
	SndOut_Enter(o);
	res = Wm_SeCheck(o, slot);
	if(res == 0)
		Fade_Start(&o->snd.se[slot].fade, Clock_HostMs(o), ms, o->snd.se[slot].fadeLevel, 0);
	SndOut_Leave(o);
	return res;
}
