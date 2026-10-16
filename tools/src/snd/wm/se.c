/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * se.c - the sound effect slots of the Wave Master library
 *        (inc/bgi/snd/wavemaster.h): a BW file already in memory is
 *        decoded whole at load and played from its samples
 */
#include "wm_internal.h"

/* Load the BW file at `bwFile` (header and data, already in memory: the
 * loader read it) into `slot`, decoded completely, with a fade-in of
 * `fadeInMs` burnt into the samples and `gain` applied to them.  The
 * slot's previous sound is dropped; the new one is loaded stopped, at
 * full volume with no fades.  0 ok, 0x14 not running, 0x15 bad slot,
 * 0xe not a BW file (or an unknown codec), 0x16 the decoder could not
 * be set up. */
uint32_t Wm_SeLoad(int slot, const void* bwFile, int fadeInMs, double gain)
{
	const BwHeader_t* hdr = (const BwHeader_t*)bwFile;
	WmSource_t* src;
	WmStream_t* s;
	WmRecord_t* r;
	int err;
	uint32_t res = 0;
	Wm_Lock();
	if(!Wm_Running())
		res = WM_E_NOT_RUNNING;
	else if((uint32_t)slot >= WM_SE_SLOTS)
		res = WM_E_BAD_CHANNEL;
	else
	{
		r = &gSe[slot];
		if(memcmp(hdr->magic, "bw  ", 4) != 0 || hdr->headerSize < sizeof *hdr)
		{ // the original trusts the header; a non-BW file is caught here, before its size is believed for the copy
			Wm_Unlock();
			return WM_E_NOT_BW;
		}
		src = WmSource_FromMemory(bwFile, hdr->headerSize + hdr->dataSize);
		s = WmStream_Open(src, gain, 1, &err);
		if(!s)
			res = err == WM_STREAM_ERR_DECODER ? WM_E_LOAD_FAILED : WM_E_NOT_BW; // (the original answers 0xe for any open failure)
		else
		{
			WmStream_FadeIn(s, (uint32_t)fadeInMs); // the result is ignored: a fade longer than the sound changes nothing
			Voice_Attach(r->voice, s);
			r->stream = s;
			r->fadeVol.active = 0;
			r->volume = 0x80;
			r->fadeLevel = 0x80;
			r->fade.active = 0;
			r->active = 1;
			Voice_SetVolume(r->voice, Record_Level(r));
		}
	}
	Wm_Unlock();
	return res;
}

// drop the slot's sound; 0, 0x14 not running, 0x15 bad slot (an empty slot is not an error)
int Wm_SeUnload(int slot)
{
	int res = 0;
	Wm_Lock();
	if(!Wm_Running())
		res = WM_E_NOT_RUNNING;
	else if((uint32_t)slot >= WM_SE_SLOTS)
		res = WM_E_BAD_CHANNEL;
	else
	{
		Voice_Detach(gSe[slot].voice);
		gSe[slot].stream = NULL;
		gSe[slot].active = 0;
	}
	Wm_Unlock();
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
int Wm_SePlay(int slot, int volume, int pan)
{
	int res;
	Wm_Lock();
	res = Wm_SeCheck(slot);
	if(res == 0)
	{
		WmRecord_t* r = &gSe[slot];
		Se_SetPan(r->voice, pan);
		r->volume = volume;
		r->fade.active = 0;
		r->fadeLevel = 0x80;
		Voice_Stop(r->voice);
		Voice_Play(r->voice, Record_Level(r));
	}
	Wm_Unlock();
	return res;
}

// stop the slot's sound and rewind it; 0, or the check's code
int Wm_SeStop(int slot)
{
	int res;
	Wm_Lock();
	res = Wm_SeCheck(slot);
	if(res == 0)
		Voice_Stop(gSe[slot].voice);
	Wm_Unlock();
	return res;
}

// the slot's master volume (clamped to 0x80), applied at once; 0, 0x14 not running, 0x15 bad slot
int Wm_SeSetMasterVolume(int slot, uint32_t vol)
{
	int res = 0;
	Wm_Lock();
	if(!Wm_Running())
		res = WM_E_NOT_RUNNING;
	else if((uint32_t)slot >= WM_SE_SLOTS)
		res = WM_E_BAD_CHANNEL;
	else
	{
		if(vol > 0x80)
			vol = 0x80;
		gSe[slot].master = (int32_t)vol;
		Voice_SetVolume(gSe[slot].voice, Record_Level(&gSe[slot]));
	}
	Wm_Unlock();
	return res;
}

// fade the slot's fade level from where it is to silence over `ms`; 0, or the check's code
int Wm_SeFadeOut(int slot, int ms)
{
	int res;
	Wm_Lock();
	res = Wm_SeCheck(slot);
	if(res == 0)
		Fade_Start(&gSe[slot].fade, OS_TicksMs(), ms, gSe[slot].fadeLevel, 0);
	Wm_Unlock();
	return res;
}
