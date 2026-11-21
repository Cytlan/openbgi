/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wm_internal.h - shared between the files of the Wave Master sound
 *                 library (src/snd/wm: wavemaster.c, voice.c, record.c,
 *                 se.c, bgm.c); the public interface is
 *                 src/include/bgi/snd/wavemaster.h
 *
 * A record per music channel and per effect slot holds the volumes and
 * fades (record.c) and points at its voice, the playing sound (voice.c);
 * the entry points in bgm.c and se.c run the checks and work on the
 * records, and the mixer (voice.c) sums the voices when the sound out
 * runs it - all of it on the engine's thread.
 */
#ifndef BGI_SND_WM_INTERNAL_H
#define BGI_SND_WM_INTERNAL_H

#include "bgi/snd/wavemaster.h"
#include "bgi/snd/bw.h"
#include "bgi/error.h"

#include <math.h>

#define WM_MIX_RATE     44100 // the mixer's output rate in Hz (the original's primary buffer)
#define WM_RING_SECONDS 4     // the pump's ring of decoded music, in seconds of the stream

/* BGI_SND_DEBUG in the environment reports music opens (with the file's
 * loop specification) and the pump's restarts / ends on stderr */
#define SND_DEBUG(o, ...)                         \
	do                                            \
	{                                             \
		if(o->snd.sndDebug)                       \
			fprintf(stderr, "snd: " __VA_ARGS__); \
	} while(0)

// ---- the result codes of the library ----------------------------------------------------------

#define WM_E_ALREADY_INIT 1    // Wm_Init: already started
#define WM_E_NO_DEVICE    5    // Wm_Init: the audio device could not be opened
#define WM_E_NOT_FOUND    0xc  // the file or the archive entry is missing
#define WM_E_NOT_BW       0xe  // not a BW file (or an unknown codec)
#define WM_E_NOT_LOADED   0x13 // the channel / slot has nothing loaded
#define WM_E_NOT_RUNNING  0x14 // the library is not started or its timer is not running
#define WM_E_BAD_CHANNEL  0x15 // bad channel / slot number
#define WM_E_LOAD_FAILED  0x16 // the sound could not be loaded (the decoder could not be set up)
#define WM_E_PLAY_FAILED  0x17 // the sound could not be started (never produced here)

// ---- voice.c: a playing sound ----------------------------------------------------------------

void Voice_SetVolume(Voice_t* v, int level);  // the attenuation level of Record_Level
void Voice_SetPan(Voice_t* v, int pan);       // -128 .. 128 through the cubic pan curve
void Voice_Attach(Voice_t* v, WmStream_t* s); // take the stream (and close the previous one)
void Voice_Detach(Voice_t* v);                // close the stream
void Voice_Stop(Voice_t* v);                  // stop and rewind
void Voice_Play(Voice_t* v, int level);       // set the level and play from the current position
void Voice_Pause(Voice_t* v, int paused);
int Voice_IsPlaying(const Voice_t* v);
void Voice_Pump(OpenBGI_t* o, Voice_t* v); // music: keep the ring half a second ahead, restarting the stream at a loop

// ---- record.c: the per-channel records ---------------------------------------------------------

void Record_Reset(WmRecord_t* r);                    // every volume to full, no fades
int Record_Level(OpenBGI_t* o, const WmRecord_t* r); // the attenuation of the four volumes
int32_t Fade_Step(Fade_t* f, uint32_t now);          // the value at `now`; ends the fade at its target
void Fade_Start(Fade_t* f, uint32_t now, int ms, int32_t from, int32_t to);
void Records_Tick(OpenBGI_t* o, WmRecord_t* recs, int count, uint32_t now); // step the fades and apply the levels

// The checks every call makes: 0 or a WM_E_* code.
int Wm_Running(OpenBGI_t* o); // initialised and the timer running
int Wm_SeCheck(OpenBGI_t* o, int slot);
int Wm_BgmCheck(OpenBGI_t* o, int ch);

// ---- wavemaster.c: the library state -----------------------------------------------------------

int Wm_BgmRelease(OpenBGI_t* o, int ch); // drop the stream of a music channel

#endif
