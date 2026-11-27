/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wavemaster.c - the life cycle of the "Wave Master" sound library
 *                (src/include/bgi/snd/wavemaster.h): start-up and shutdown, the
 *                20 ms fade timer and the debugger's view
 *
 * Structure of the original: one record per music channel (16) and per
 * effect slot (64) holding the voice, the stream, the master and channel
 * volumes and two fades; a DirectSound "voice" per record; a window timer
 * stepping the fades.  Here the records are in record.c, the voices and
 * the software mixer in voice.c, the effect and music calls in se.c and
 * bgm.c; the volume arithmetic, the fade interpolation and every result
 * code match the original.
 */
#include "wm_internal.h"
#include "bgi/host.h"
#include "bgi/core.h"

/* Start the library: every record and voice reset, and the mixer started
 * (44.1 kHz 16-bit stereo, the original's DirectSound format), which
 * fills the engine's sound out (bgi/snd/sndout.h).  0 ok, 1 already
 * started, 5 when the machine has no sound output (Host_AudioAvailable:
 * the library then stays uninitialised). */
int Wm_Init(OpenBGI_t* o)
{
	int i, r = 0;
	SndOut_Enter(o);
	o->snd.sndDebug = getenv("BGI_SND_DEBUG") != NULL; // music opens, the pump's restarts and ends on stderr
	if(o->snd.wmFlags & 1)
		r = WM_E_ALREADY_INIT;
	else
	{
		o->snd.wmFlags = 0;
		for(i = 0; i < WM_BGM_CHANNELS; i++)
		{
			Record_Reset(&o->snd.bgm[i]);
			memset(&o->snd.bgmVoices[i], 0, sizeof o->snd.bgmVoices[i]);
			o->snd.bgmVoices[i].isBgm = 1;
			o->snd.bgm[i].voice = &o->snd.bgmVoices[i];
		}
		for(i = 0; i < WM_SE_SLOTS; i++)
		{
			Record_Reset(&o->snd.se[i]);
			memset(&o->snd.seVoices[i], 0, sizeof o->snd.seVoices[i]);
			o->snd.se[i].voice = &o->snd.seVoices[i];
		}
		memset(&o->snd.waveVoice, 0, sizeof o->snd.waveVoice);
		if(!Host_AudioAvailable(o))
			r = WM_E_NO_DEVICE;
		else
		{
			SndOut_Start(o);
			o->snd.audioRunning = 1;
			o->snd.wmFlags |= 1;
		}
	}
	SndOut_Leave(o);
	return r;
}

// start the 20 ms fade timer; 0, or 0x14 when the library is not started
int Wm_StartTimer(OpenBGI_t* o)
{
	int r = 0;
	if(!(o->snd.wmFlags & 1))
		r = WM_E_NOT_RUNNING;
	else
	{
		o->snd.wmFlags |= 2;
		o->snd.wmNextTick = Clock_HostMs(o) + 20;
	}
	return r;
}

// stop the fade timer; 0, or 0x14 when the library or the timer is not running
int Wm_StopTimer(OpenBGI_t* o)
{
	int r = 0;
	if((o->snd.wmFlags & 3) != 3)
		r = WM_E_NOT_RUNNING;
	else
		o->snd.wmFlags &= ~2u;
	return r;
}

// drop the stream of a music channel; 0, 0x14 not running, 0x15 bad channel
int Wm_BgmRelease(OpenBGI_t* o, int ch)
{
	if(!Wm_Running(o))
		return WM_E_NOT_RUNNING;
	if((uint32_t)ch >= WM_BGM_CHANNELS)
		return WM_E_BAD_CHANNEL;
	SndOut_Enter(o);
	Voice_Detach(o->snd.bgm[ch].voice);
	o->snd.bgm[ch].stream = NULL;
	o->snd.bgm[ch].active = 0;
	SndOut_Leave(o);
	return 0;
}

/* Stop the library: the timer flag is cleared, every effect slot
 * released, the music channels' streams let go (the original's release
 * call does nothing by then, see below, and its streams go with the
 * process), the flags zeroed, and the mixer stopped (the sound out then
 * stays silent).  Always 0, also when the library was not started. */
int Wm_Shutdown(OpenBGI_t* o)
{
	int i;
	SndOut_Enter(o);
	if(o->snd.wmFlags & 1)
	{
		if((o->snd.wmFlags & 3) == 3)
			o->snd.wmFlags &= ~2u;
		// answers 0x14 now that the timer flag is gone, so the music streams are not released
		// here (the original frees its voice objects directly instead)
		for(i = 0; i < WM_BGM_CHANNELS; i++)
		{
			Wm_BgmRelease(o, i);
			Voice_Detach(o->snd.bgm[i].voice); // (an engine that is stopped keeps nothing)
			o->snd.bgm[i].stream = NULL;
			o->snd.bgm[i].active = 0;
		}
		for(i = 0; i < WM_SE_SLOTS; i++)
		{
			Voice_Detach(o->snd.se[i].voice);
			o->snd.se[i].stream = NULL;
			o->snd.se[i].active = 0;
		}
		o->snd.wmFlags = 0;
	}
	if(o->snd.audioRunning)
	{
		SndOut_Stop(o);
		Voice_Detach(&o->snd.waveVoice);
		o->snd.audioRunning = 0;
	}
	SndOut_Leave(o);
	return 0;
}

/* Run the 20 ms fade tick when it is due (the message pump calls this
 * when its queue is empty, in place of the original's WM_TIMER, and so
 * does OpenBGI_RunAudio while a request holds the engine - a modal loop
 * of the original dispatches the timer); 1 when it ran, 0 when the timer
 * is not running or the tick is not due yet. */
int Wm_TimerPoll(OpenBGI_t* o)
{
	uint32_t now;
	if((o->snd.wmFlags & 3) != 3)
		return 0;
	now = Clock_HostMs(o);
	if((int32_t)(now - o->snd.wmNextTick) < 0)
		return 0;
	o->snd.wmNextTick = now + 20;
	SndOut_Enter(o);
	Records_Tick(o, o->snd.bgm, WM_BGM_CHANNELS, now);
	Records_Tick(o, o->snd.se, WM_SE_SLOTS, now);
	SndOut_Leave(o);
	return 1;
}

/* The debugger's view of voice `index` of the music channels (`bgm`) or
 * the effect slots: its state, format, length, position and levels into
 * *out (zeroed first); 1, or 0 for an index out of range. */
int Wm_DebugVoice(OpenBGI_t* o, int bgm, int index, WmVoiceInfo_t* out)
{
	const Voice_t* v;
	memset(out, 0, sizeof *out);
	if(bgm ? (index < 0 || index >= WM_BGM_CHANNELS) : (index < 0 || index >= WM_SE_SLOTS))
		return 0;
	v = bgm ? &o->snd.bgmVoices[index] : &o->snd.seVoices[index];
	SndOut_Enter(o);
	out->loaded = v->stream != NULL;
	out->playing = v->playing;
	out->paused = v->paused;
	out->rate = v->rate;
	out->channels = v->channels;
	out->frames = v->stream ? WmStream_Frames(v->stream) : 0;
	out->pos = v->pos;
	out->dsVolume = v->dsVolume;
	out->pan = v->pan;
	out->loops = v->stream && bgm ? WmStream_LoopCount(o, v->stream) : 0;
	SndOut_Leave(o);
	return 1;
}
