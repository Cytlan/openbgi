/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wavemaster.h - the entry points of the statically linked "Wave Master"
 *                sound library that the sound manager (bgi/sound.h) calls;
 *                the reimplementation is src/snd/wm/ (wavemaster.c, bgm.c,
 *                se.c, record.c, voice.c, wave.c) with the container and
 *                codecs in src/snd/bw/.
 *
 * Every entry of the original takes the library's lock around the
 * operation (its DirectSound threads share the records); here everything,
 * the mixer included, runs on the engine's thread and there is no lock.
 * Inside, the original raises C++ exceptions carrying an int code that
 * the wrappers turn into the return value:
 *   0      ok
 *   1      already initialised              5     the sound device could not be opened
 *   0xc    file not found                   0xe   not a BW file (or an unknown codec)
 *   0x13   the slot has nothing loaded      0x14  the library is not running
 *   0x15   bad channel / slot number        0x16  the sound could not be loaded
 *   0x17   the sound could not be started
 *
 * Music plays on 16 channels (0 .. 15) streamed from a file or an archive
 * entry; the "pair" forms take an intro and a loop part (two Vorbis files,
 * the loop part repeating while `mode` is non-zero - the same name twice
 * makes a single looping file).  Sound effects live in 64 slots (0 .. 63)
 * and are decoded completely when loaded.  Volumes are 0 .. 0x80, pans
 * 0 .. 0x80 with 0x40 centred.  `gain` multiplies every decoded sample;
 * the engine always passes 1.0.
 *
 * The 20 ms fade timer of the original is a window timer; the message pump
 * drives it here through Wm_TimerPoll().
 *
 * The voices are summed by a software mixer (Wm_Mix) at 44.1 kHz 16-bit
 * stereo, the original's DirectSound format, into the engine's sound out
 * (bgi/snd/sndout.h), which runs it for the time that passes.
 */
#ifndef BGI_SND_WAVEMASTER_H_
#define BGI_SND_WAVEMASTER_H_

#include "bgi/common.h"

#define WM_BGM_CHANNELS 0x10 // music channels
#define WM_SE_SLOTS     0x40 // sound effect slots

int Wm_Init(OpenBGI_t* o); // start the library and its mixer; 0, 1 already started, 5 no sound output
/* An extra source mixed with the channels: `fn` adds its stereo 16-bit
 * frames into `acc` (interleaved 32-bit accumulators, `frames` of them)
 * whenever the mixer runs.  The movie player feeds its sound track this
 * way.  NULL removes it.  Wm_MixRunning tells whether the mixer runs at
 * all (the library started). */
typedef void (*WmExternalMix_t)(void* user, int32_t* acc, int frames);
void Wm_SetExternalMix(OpenBGI_t* o, WmExternalMix_t fn, void* user);
int Wm_MixRunning(OpenBGI_t* o);
/* The mixer: every playing voice (the music channels, the effect slots,
 * the wave of Wm_PlayWave) and the external source summed into `frames`
 * interleaved stereo frames of `out`, clamped to 16 bits; the voices move
 * on by as much.  The sound out calls it (bgi/snd/sndout.h). */
void Wm_Mix(OpenBGI_t* o, int16_t* out, int frames);
/* "A0 C0", the original's PlaySound of a RIFF WAVE file (`size` bytes at
 * `file`): played once on a voice of its own at full volume, cutting off
 * the one before; 1 when it plays, 0 when the library is not running or
 * the file is not uncompressed 8- or 16-bit PCM in one or two channels. */
int Wm_PlayWave(OpenBGI_t* o, const void* file, uint32_t size);
void Wm_HoldAll(OpenBGI_t* o, int hold); // every playing voice held where it is (non-zero) / the held ones play on

int Wm_StartTimer(OpenBGI_t* o); // start the 20 ms fade timer; 0, or 0x14 when the library is not started
int Wm_StopTimer(OpenBGI_t* o);  // stop the fade timer; 0, or 0x14 when it was not running
int Wm_Shutdown(OpenBGI_t* o);   // stop everything, release the device; always 0
int Wm_TimerPoll(OpenBGI_t* o);  // run the 20 ms fade tick when it is due; 1 when it ran

// BGM channels
int Wm_BgmSetMasterVolume(OpenBGI_t* o, int ch, uint32_t vol);  // the channel's master volume (not clamped)
int Wm_BgmSetChannelVolume(OpenBGI_t* o, int ch, uint32_t vol); // 1.553 on ("A0 1C"): the channel's own volume, clamped to 0x80
int Wm_BgmStop(OpenBGI_t* o, int ch);                           // stop and rewind the channel's stream
/* the openers: a loose file, an archive entry, and the intro / loop
 * pairs of both (the same name twice: one file, looping while `mode` is
 * non-zero); the channel's previous sound is replaced */
int Wm_BgmOpenFile(OpenBGI_t* o, int ch, const char* path, int volume, int pan, double gain);
int Wm_BgmOpenArc(OpenBGI_t* o, int ch, const char* arcPath, const char* name, int volume, int pan, double gain);
int Wm_BgmOpenFilePair(OpenBGI_t* o, int ch, const char* intro, const char* loop, int mode, int volume, int pan, double gain);
int Wm_BgmOpenArcPair(OpenBGI_t* o, int ch, const char* arcPath, const char* intro, const char* loop, int mode, int volume, int pan,
	double gain);
int Wm_BgmFadeOut(OpenBGI_t* o, int ch, int ms);                // the fade level to 0 over `ms`
int Wm_BgmFadeIn(OpenBGI_t* o, int ch, int ms);                 // the fade level to 0x80 over `ms`
int Wm_BgmPause(OpenBGI_t* o, int ch, int paused);              // non-zero pauses, 0 resumes
int Wm_BgmFadeVolume(OpenBGI_t* o, int ch, int volume, int ms); // the channel volume towards `volume` (clamped to 0x80) over `ms`
int Wm_BgmSetPan(OpenBGI_t* o, int ch, int pan);                // 0 .. 0x80, clamped
int Wm_BgmIsPlaying(OpenBGI_t* o, int ch, uint32_t* out);       // *out = the voice is playing
int Wm_BgmLoopCount(OpenBGI_t* o, int ch, uint32_t* out);       // *out = loop restarts so far (see WmStream_LoopCount)

// sound effect slots
int Wm_SeSetMasterVolume(OpenBGI_t* o, int slot, uint32_t vol);  // the slot's master volume, clamped to 0x80
int Wm_SeSetChannelVolume(OpenBGI_t* o, int slot, uint32_t vol); // 1.588 on ("A0 2C"): the slot's own volume, clamped to 0x80
/* load a whole BW file held in memory into the slot, decoded at once,
 * with a fade-in of `fadeInMs` burnt into the samples, `gain` applied
 * and the sound played `rate` times as fast at the same pitch */
uint32_t Wm_SeLoad(OpenBGI_t* o, int slot, const void* bwFile, int fadeInMs, double gain, double rate);
int Wm_SeUnload(OpenBGI_t* o, int slot);                    // drop the slot's sound
int Wm_SePlay(OpenBGI_t* o, int slot, int volume, int pan); // from the start, at `volume` and `pan`
int Wm_SeStop(OpenBGI_t* o, int slot);                      // stop and rewind
int Wm_SeFadeOut(OpenBGI_t* o, int slot, int ms);           // the fade level to 0 over `ms`


// the debugger's view of a voice (src/dbg/views/sound.c)
typedef struct WmVoiceInfo
{
	int loaded, playing, paused;
	uint32_t rate, channels, frames; // the stream's (Hz, 1 or 2, total length)
	uint32_t pos;                    // effects: the play cursor in frames
	int32_t dsVolume, pan;           // DirectSound units: hundredths of a dB (-10000 .. 0), -10000 (left) .. 10000 (right)
	int loops;                       // music: restarts so far
} WmVoiceInfo_t;
// voice `index` of the music channels (`bgm` non-zero) or the effect slots; 1 when the index is valid
int Wm_DebugVoice(OpenBGI_t* o, int bgm, int index, WmVoiceInfo_t* out);

// ---- the types of the library's state (o->snd, bgi/core.h) ------------------------------

// a playing sound (src/snd/wm/voice.c)
typedef struct Voice
{
	int isBgm;               // a music channel (streamed through the ring) rather than an effect slot
	struct WmStream* stream; // the sound; NULL when nothing is loaded
	uint32_t rate, channels; // the stream's (Hz, 1 or 2)
	int playing, paused;
	int held;            // stopped by Wm_HoldAll, plays on when it lets go
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

// a fade of a record's level (src/snd/wm/record.c)
typedef struct Fade
{
	int active;          // a fade is running
	uint32_t start, end; // ticks (ms)
	int32_t from, to;    // the levels at `start` and `end`
} Fade_t;

// the record of a music channel or an effect slot (src/snd/wm/record.c)
typedef struct WmRecord
{
	int active; // something is loaded
	Voice_t* voice;
	struct WmStream* stream; // (the voice owns it here)
	int32_t master;          // 0 .. 0x80, the master volume of the sound manager
	int32_t aux;             // 0 .. 0x80, the channel's own volume ("A0 1C" / "A0 2C"); kept across loads
	Fade_t fadeVol;          // towards a channel volume
	int32_t volume;          // 0 .. 0x80, the volume the script gave the sound
	Fade_t fade;             // the fade in / out level
	int32_t fadeLevel;       // 0 .. 0x80, 0x80 = no fade
} WmRecord_t;

#endif // BGI_SND_WAVEMASTER_H_
