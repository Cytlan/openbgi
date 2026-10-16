/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wavemaster.c - the life cycle of the "Wave Master" sound library
 *                (inc/bgi/snd/wavemaster.h): start-up and shutdown, the
 *                20 ms fade timer, the lock, and the debugger's view
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

/* BGI_SND_DEBUG in the environment reports music opens (with the file's
 * loop specification) and the pump's restarts / ends on stderr */
int gSndDebug;
OsLock_t* gWmLock; // created by the first Wm_Lock
int gAudioRunning; // OS_AudioStart succeeded: the mixer thread runs

WmRecord_t gBgm[WM_BGM_CHANNELS];
WmRecord_t gSe[WM_SE_SLOTS];
Voice_t gBgmVoices[WM_BGM_CHANNELS];
Voice_t gSeVoices[WM_SE_SLOTS];
uint32_t gWmFlags;          // bit 0 initialised, bit 1 timer running
uint32_t gWmNextTick;       // when the 20 ms timer is next due (ms)
static char gWmVersion[32]; // the buffer Wm_Version formats into

// take the library lock, creating it on first use
void Wm_Lock(void)
{
	if(!gWmLock)
		gWmLock = OS_LockCreate();
	OS_LockEnter(gWmLock);
}

void Wm_Unlock(void)
{
	OS_LockLeave(gWmLock);
}

/* Start the library: every record and voice reset, the audio device
 * opened at 44.1 kHz 16-bit stereo with the mixer as its callback.
 * 0 ok, 1 already started, 5 when there is no audio device or it could
 * not be started (the library then stays uninitialised). */
int Wm_Init(void)
{
	gSndDebug = getenv("BGI_SND_DEBUG") != NULL;
	int i, r = 0;
	Wm_Lock();
	if(gWmFlags & 1)
		r = WM_E_ALREADY_INIT;
	else
	{
		gWmFlags = 0;
		for(i = 0; i < WM_BGM_CHANNELS; i++)
		{
			Record_Reset(&gBgm[i]);
			memset(&gBgmVoices[i], 0, sizeof gBgmVoices[i]);
			gBgmVoices[i].isBgm = 1;
			gBgm[i].voice = &gBgmVoices[i];
		}
		for(i = 0; i < WM_SE_SLOTS; i++)
		{
			Record_Reset(&gSe[i]);
			memset(&gSeVoices[i], 0, sizeof gSeVoices[i]);
			gSe[i].voice = &gSeVoices[i];
		}
		// DirectSound at 44.1 kHz, 16-bit stereo
		if(!OS_AudioAvailable() || !OS_AudioStart(WM_MIX_RATE, Mixer_Fill, NULL))
			r = WM_E_NO_DEVICE;
		else
		{
			gAudioRunning = 1;
			gWmFlags |= 1;
		}
	}
	Wm_Unlock();
	return r;
}

// start the 20 ms fade timer; 0, or 0x14 when the library is not started
int Wm_StartTimer(void)
{
	int r = 0;
	Wm_Lock();
	if(!(gWmFlags & 1))
		r = WM_E_NOT_RUNNING;
	else
	{
		gWmFlags |= 2;
		gWmNextTick = OS_TicksMs() + 20;
	}
	Wm_Unlock();
	return r;
}

// stop the fade timer; 0, or 0x14 when the library or the timer is not running
int Wm_StopTimer(void)
{
	int r = 0;
	Wm_Lock();
	if((gWmFlags & 3) != 3)
		r = WM_E_NOT_RUNNING;
	else
		gWmFlags &= ~2u;
	Wm_Unlock();
	return r;
}

// drop the stream of a music channel (the caller holds the lock); 0, 0x14 not running, 0x15 bad channel
int Wm_BgmRelease(int ch)
{
	if(!Wm_Running())
		return WM_E_NOT_RUNNING;
	if((uint32_t)ch >= WM_BGM_CHANNELS)
		return WM_E_BAD_CHANNEL;
	Voice_Detach(gBgm[ch].voice);
	gBgm[ch].stream = NULL;
	gBgm[ch].active = 0;
	return 0;
}

/* Stop the library: the timer flag is cleared, every effect slot
 * released (the music channels' streams are left to the process exit,
 * see below), the flags zeroed, and the audio device stopped outside
 * the lock (the mixer thread takes it).  Always 0, also when the library
 * was not started. */
int Wm_Shutdown(void)
{
	int i;
	Wm_Lock();
	if(gWmFlags & 1)
	{
		if((gWmFlags & 3) == 3)
			gWmFlags &= ~2u;
		// answers 0x14 now that the timer flag is gone, so the music streams are not released
		// here (the original frees its voice objects directly instead)
		for(i = 0; i < WM_BGM_CHANNELS; i++)
			Wm_BgmRelease(i);
		for(i = 0; i < WM_SE_SLOTS; i++)
		{
			Voice_Detach(gSe[i].voice);
			gSe[i].stream = NULL;
			gSe[i].active = 0;
		}
		gWmFlags = 0;
	}
	Wm_Unlock();
	if(gAudioRunning)
	{
		OS_AudioStop();
		gAudioRunning = 0;
	}
	return 0;
}

/* Run the 20 ms fade tick when it is due (the message pump calls this
 * when its queue is empty, in place of the original's WM_TIMER); 1 when
 * it ran, 0 when the timer is not running or the tick is not due yet.
 * The fades of every record are stepped under the lock. */
int Wm_TimerPoll(void)
{
	uint32_t now;
	if((gWmFlags & 3) != 3)
		return 0;
	now = OS_TicksMs();
	if((int32_t)(now - gWmNextTick) < 0)
		return 0;
	gWmNextTick = now + 20;
	Wm_Lock(); // the original's timer procedure does not take the lock; the mixer thread here does
	Records_Tick(gBgm, WM_BGM_CHANNELS, now);
	Records_Tick(gSe, WM_SE_SLOTS, now);
	Wm_Unlock();
	return 1;
}

// the version string "%d.%04dDS%d" of version 2.10 and DirectSound 8: "2.0010DS8"
const char* Wm_Version(void)
{
	sprintf(gWmVersion, "%d.%04dDS%d", 2, 10, 8);
	return gWmVersion;
}

/* The debugger's view of voice `index` of the music channels (`bgm`) or
 * the effect slots: its state, format, length, position and levels into
 * *out (zeroed first); 1, or 0 for an index out of range.  No lock is
 * taken: the view is a snapshot. */
int Wm_DebugVoice(int bgm, int index, WmVoiceInfo_t* out)
{
	const Voice_t* v;
	memset(out, 0, sizeof *out);
	if(bgm ? (index < 0 || index >= WM_BGM_CHANNELS) : (index < 0 || index >= WM_SE_SLOTS))
		return 0;
	v = bgm ? &gBgmVoices[index] : &gSeVoices[index];
	out->loaded = v->stream != NULL;
	out->playing = v->playing;
	out->paused = v->paused;
	out->rate = v->rate;
	out->channels = v->channels;
	out->frames = v->stream ? WmStream_Frames(v->stream) : 0;
	out->pos = v->pos;
	out->dsVolume = v->dsVolume;
	out->pan = v->pan;
	out->loops = v->stream && bgm ? WmStream_LoopCount(v->stream) : 0;
	return 1;
}
