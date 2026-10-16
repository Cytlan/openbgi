/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wm_internal.h - shared between the files of the Wave Master sound
 *                 library (src/snd/wm: wavemaster.c, voice.c, record.c,
 *                 se.c, bgm.c); the public interface is
 *                 inc/bgi/snd/wavemaster.h
 *
 * A record per music channel and per effect slot holds the volumes and
 * fades (record.c) and points at its voice, the playing sound (voice.c);
 * the entry points in bgm.c and se.c take the library lock, run the
 * checks and work on the records, while the mixer (voice.c) sums the
 * voices on the audio thread under the same lock.
 */
#ifndef BGI_SND_WM_INTERNAL_H
#define BGI_SND_WM_INTERNAL_H

#include "bgi/snd/wavemaster.h"
#include "bgi/snd/bw.h"
#include "bgi/os.h"
#include "bgi/error.h"

#include <math.h>

#define WM_MIX_RATE     44100 // the mixer's output rate in Hz (the original's primary buffer)
#define WM_RING_SECONDS 4     // the pump's ring of decoded music, in seconds of the stream

/* BGI_SND_DEBUG in the environment reports music opens (with the file's
 * loop specification) and the pump's restarts / ends on stderr */
extern int gSndDebug;
#define SND_DEBUG(...)                            \
	do                                            \
	{                                             \
		if(gSndDebug)                             \
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

typedef struct Voice
{
	int isBgm;               // a music channel (streamed through the ring) rather than an effect slot
	WmStream_t* stream;      // the sound; NULL when nothing is loaded
	uint32_t rate, channels; // the stream's (Hz, 1 or 2)
	int playing, paused;
	int32_t dsVolume;    // DirectSound units, hundredths of a dB: -10000 .. 0
	int32_t pan;         // DirectSound units, -10000 (left) .. 10000 (right), from the cubic curve of Voice_SetPan
	double gainL, gainR; // the linear gains the mixer applies, from dsVolume and pan
	// effects: the decoded samples
	const int16_t* samples; // 16-bit interleaved, owned by the stream
	uint32_t frames;
	// music: the pump's ring of decoded frames
	int16_t* ring;
	uint32_t ringFrames, ringRead, ringCount; // capacity, the mixer's read index, frames in the ring
	int done;                                 // the pump found the end
	// playback position: source frame and 16.16 fraction (for the interpolation)
	uint32_t pos, posFrac;
} Voice_t;

extern Voice_t gBgmVoices[WM_BGM_CHANNELS];
extern Voice_t gSeVoices[WM_SE_SLOTS];

void Voice_SetVolume(Voice_t* v, int level);  // the attenuation level of Record_Level
void Voice_SetPan(Voice_t* v, int pan);       // -128 .. 128 through the cubic pan curve
void Voice_Attach(Voice_t* v, WmStream_t* s); // take the stream (and close the previous one)
void Voice_Detach(Voice_t* v);                // close the stream
void Voice_Stop(Voice_t* v);                  // stop and rewind
void Voice_Play(Voice_t* v, int level);       // set the level and play from the current position
void Voice_Pause(Voice_t* v, int paused);
int Voice_IsPlaying(const Voice_t* v);
void Voice_Pump(Voice_t* v); // music: keep the ring half a second ahead, restarting the stream at a loop

// The mixer callback of OS_AudioStart: `frames` stereo 16-bit frames into out.
void Mixer_Fill(void* user, int16_t* out, int frames);

// ---- record.c: the per-channel records ---------------------------------------------------------

typedef struct Fade
{
	int active;          // a fade is running
	uint32_t start, end; // ticks (ms)
	int32_t from, to;    // the levels at `start` and `end`
} Fade_t;

typedef struct WmRecord
{
	int active; // something is loaded
	Voice_t* voice;
	WmStream_t* stream; // (the voice owns it here)
	int32_t master;     // 0 .. 0x80, the master volume of the sound manager
	int32_t aux;        // 0x80, never changed
	Fade_t fadeVol;     // towards a channel volume
	int32_t volume;     // 0 .. 0x80, the volume the script gave the sound
	Fade_t fade;        // the fade in / out level
	int32_t fadeLevel;  // 0 .. 0x80, 0x80 = no fade
} WmRecord_t;

extern WmRecord_t gBgm[WM_BGM_CHANNELS];
extern WmRecord_t gSe[WM_SE_SLOTS];

void Record_Reset(WmRecord_t* r);           // every volume to full, no fades
int Record_Level(const WmRecord_t* r);      // the attenuation of the four volumes
int32_t Fade_Step(Fade_t* f, uint32_t now); // the value at `now`; ends the fade at its target
void Fade_Start(Fade_t* f, uint32_t now, int ms, int32_t from, int32_t to);
void Records_Tick(WmRecord_t* recs, int count, uint32_t now); // step the fades and apply the levels

// The checks every call makes: 0 or a WM_E_* code.
int Wm_Running(void); // initialised and the timer running
int Wm_SeCheck(int slot);
int Wm_BgmCheck(int ch);

// ---- wavemaster.c: the library state -----------------------------------------------------------

extern OsLock_t* gWmLock;
extern int gAudioRunning;    // OS_AudioStart succeeded: the mixer thread runs
extern uint32_t gWmFlags;    // bit 0 initialised, bit 1 timer running
extern uint32_t gWmNextTick; // when the 20 ms timer is next due (ms)

void Wm_Lock(void); // the library lock (also taken by the mixer thread)
void Wm_Unlock(void);
int Wm_BgmRelease(int ch); // drop the stream of a music channel

#endif
