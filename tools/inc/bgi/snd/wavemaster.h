/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wavemaster.h - the entry points of the statically linked "Wave Master"
 *                sound library that the sound manager (bgi/sound.h) calls;
 *                the reimplementation is src/snd/wm/ (wavemaster.c, bgm.c,
 *                se.c, record.c, voice.c) with the container and codecs in
 *                src/snd/bw/.
 *
 * Every entry takes the library's lock around the operation.  Inside, the
 * original raises C++ exceptions carrying an int code that the locked
 * wrappers turn into the return value:
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
 */
#ifndef BGI_SND_WAVEMASTER_H_
#define BGI_SND_WAVEMASTER_H_

#include "bgi/common.h"

#define WM_BGM_CHANNELS 0x10 // music channels
#define WM_SE_SLOTS     0x40 // sound effect slots

int Wm_Init(void); // start the library and the audio device; 0, 1 already started, 5 no device
/* An extra source mixed with the channels: `fn` adds its stereo 16-bit
 * frames into `acc` (interleaved 32-bit accumulators, `frames` of them)
 * on the mixer's thread, under the library lock.  The movie player feeds
 * its sound track this way.  NULL removes it.  Wm_MixRunning tells
 * whether the mixer pulls at all (the audio device opened). */
typedef void (*WmExternalMix_t)(void* user, int32_t* acc, int frames);
void Wm_SetExternalMix(WmExternalMix_t fn, void* user);
int Wm_MixRunning(void);
int Wm_StartTimer(void); // start the 20 ms fade timer; 0, or 0x14 when the library is not started
int Wm_StopTimer(void);  // stop the fade timer; 0, or 0x14 when it was not running
int Wm_Shutdown(void);   // stop everything, release the device; always 0
int Wm_TimerPoll(void);  // run the 20 ms fade tick when it is due; 1 when it ran

// BGM channels
int Wm_BgmSetMasterVolume(int ch, uint32_t vol); // the channel's master volume (not clamped)
int Wm_BgmStop(int ch);                          // stop and rewind the channel's stream
/* the openers: a loose file, an archive entry, and the intro / loop
 * pairs of both (the same name twice: one file, looping while `mode` is
 * non-zero); the channel's previous sound is replaced */
int Wm_BgmOpenFile(int ch, const char* path, int volume, int pan, double gain);
int Wm_BgmOpenArc(int ch, const char* arcPath, const char* name, int volume, int pan, double gain);
int Wm_BgmOpenFilePair(int ch, const char* intro, const char* loop, int mode, int volume, int pan, double gain);
int Wm_BgmOpenArcPair(int ch, const char* arcPath, const char* intro, const char* loop, int mode, int volume, int pan,
	double gain);
int Wm_BgmFadeOut(int ch, int ms);                // the fade level to 0 over `ms`
int Wm_BgmFadeIn(int ch, int ms);                 // the fade level to 0x80 over `ms`
int Wm_BgmPause(int ch, int paused);              // non-zero pauses, 0 resumes
int Wm_BgmFadeVolume(int ch, int volume, int ms); // the channel volume towards `volume` (clamped to 0x80) over `ms`
int Wm_BgmSetPan(int ch, int pan);                // 0 .. 0x80, clamped
int Wm_BgmIsPlaying(int ch, uint32_t* out);       // *out = the voice is playing
int Wm_BgmLoopCount(int ch, uint32_t* out);       // *out = loop restarts so far (see WmStream_LoopCount)

// sound effect slots
int Wm_SeSetMasterVolume(int slot, uint32_t vol); // the slot's master volume, clamped to 0x80
/* load a whole BW file held in memory into the slot, decoded at once,
 * with a fade-in of `fadeInMs` burnt into the samples */
uint32_t Wm_SeLoad(int slot, const void* bwFile, int fadeInMs, double gain);
int Wm_SeUnload(int slot);                    // drop the slot's sound
int Wm_SePlay(int slot, int volume, int pan); // from the start, at `volume` and `pan`
int Wm_SeStop(int slot);                      // stop and rewind
int Wm_SeFadeOut(int slot, int ms);           // the fade level to 0 over `ms`

const char* Wm_Version(void); // "2.0010DS8"

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
int Wm_DebugVoice(int bgm, int index, WmVoiceInfo_t* out);

#endif // BGI_SND_WAVEMASTER_H_
