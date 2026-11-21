/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sound.h - the sound manager used by the "A0 xx" instructions
 *           (src/snd/sound.c) on top of the "Wave Master" library
 *           (src/include/bgi/snd/wavemaster.h).
 *
 * BGM plays on 16 channels (a streamed file, from a loose file or an
 * archive entry, optionally as an intro / loop pair); sound effects live in
 * 64 numbered slots loaded by the background loader.  The manager keeps the
 * per-channel / per-slot master volumes, resolves file names against the
 * base and disc directories (with the "80 3x" search sub-directories and a
 * retry prompt when a file is missing) and forwards everything else to the
 * library.  Library results: 0 ok, 12 file not found, 14 not a BW file,
 * 0x14 the library is not running, 0x15 bad channel.
 */
#ifndef BGI_SOUND_H_
#define BGI_SOUND_H_

#include "bgi/common.h"

#define BGM_CHANNELS      0x10 // music channels
#define SE_SLOTS          0x40 // sound effect slots
#define SE_DESC_SIZE      0x40 // an effect's descriptor: the BW header of its file

// the library's result codes the manager and the handlers test
#define WM_OK             0
#define WM_FILE_NOT_FOUND 12
#define WM_NOT_BW         14
#define WM_NOT_RUNNING    0x14
#define WM_BAD_CHANNEL    0x15

int Sound_Init(OpenBGI_t* o);                                  // start-up (Engine_Init); always 1
void Sound_Shutdown(OpenBGI_t* o);                             // stop the fade timer and the library
void Sound_ResetVolumes(OpenBGI_t* o);                         // every master volume to 0x80, applied (each script boot)
void Sound_ApplyMasterVolumes(OpenBGI_t* o);                   // push the remembered master volumes to the library
int Bgm_SetMasterVolume(OpenBGI_t* o, int ch, uint32_t vol);   // "A0 08": 1 ok, 0 out of range (not applied while muted)
int Se_SetMasterVolume(OpenBGI_t* o, int slot, uint32_t vol);  // "A0 09": 1 ok, 0 out of range
int Bgm_SetChannelVolume(OpenBGI_t* o, int ch, uint32_t vol);  // "A0 1C" (1.553 on): the channel's own volume; the library's result
int Se_SetChannelVolume(OpenBGI_t* o, int slot, uint32_t vol); // "A0 2C" (1.588 on): the slot's own volume; the library's result
/* "A0 10": play a loose file from the base (then the disc) directory;
 * "A0 11": from an archive (NULL arc = loose file with the search
 * sub-directories), asking to retry while the file is missing; "A0 12":
 * an intro / loop pair, `flag` non-zero repeating the loop part.  Library
 * results. */
int Bgm_PlayFile(OpenBGI_t* o, int ch, const char* name, int volume);
int Bgm_PlayArc(OpenBGI_t* o, int ch, const char* arc, const char* name, int volume, int pan);
int Bgm_PlayArcPair(OpenBGI_t* o, int ch, const char* arc, const char* intro, const char* loop, int flag, int volume, int pan);
/* "A0 15": 1 while the channel plays, with *outLength = the stream's loop
 * count; 0 otherwise and for a bad channel */
uint32_t Bgm_GetPosition(OpenBGI_t* o, int ch, uint32_t* outLength);
void Sound_ClearTable(OpenBGI_t* o); // forget every effect descriptor (each script boot)
/* "A0 20" / "A0 21" / "A0 27" through the loader: load a sound effect
 * into a slot from the BW file in memory (its 0x40-byte header is copied
 * to the slot table); `fadeInMs` is a fade-in burnt into the decoded
 * samples, `gain` multiplies them, `rate` (1.0 but for "A0 27" / "A0 28")
 * makes the sound play that many times as fast at the same pitch; the
 * library's result */
uint32_t Sound_LoadSe(OpenBGI_t* o, int slot, const void* desc, int fadeInMs, double gain, double rate);
int Se_Unload(OpenBGI_t* o, int slot);                    // "A0 22"
int Se_Play(OpenBGI_t* o, int slot, int volume, int pan); // "A0 24"
int Se_Stop(OpenBGI_t* o, int slot);                      // "A0 25"
int Se_FadeOut(OpenBGI_t* o, int slot, int ms);           // "A0 26"
int32_t Se_GetLength(OpenBGI_t* o, int slot);             // "A0 24", "A0 2F": ms from the descriptor (frames * 1000 / rate, 1.547 on divided by the playback rate); 0 when empty
void Sound_StopAll(OpenBGI_t* o);                         // "80 65", minimise: mute everything
void Sound_ResumeAll(OpenBGI_t* o);                       // the window's activation after the minimise: the volumes back
void Sound_HoldAll(OpenBGI_t* o, int hold);               // 1.547 on, "81 68": the playing sounds held while the window is inactive / let go
int Sound_PlayWave(OpenBGI_t* o, const char* name);       // "A0 C0": a wave file of the base directory played once (the original's PlaySound); 1 when it plays

// BGM channel control (the library's locked entry points)
int Bgm_Pause(OpenBGI_t* o, int ch, int paused);              // "A0 14"
int Bgm_FadeVolume(OpenBGI_t* o, int ch, int volume, int ms); // "A0 16"
int Bgm_SetPan(OpenBGI_t* o, int ch, int pan);                // "A0 17"
int Bgm_FadeIn(OpenBGI_t* o, int ch, int ms);                 // "A0 18"
int Bgm_FadeOut(OpenBGI_t* o, int ch, int ms);                // "A0 19"

// CD audio
int Cd_Open(OpenBGI_t* o);                      // "A0 80": 1 when the device is open
void Sound_CdClose(OpenBGI_t* o);               // "A0 81"
int Cd_Play(OpenBGI_t* o, int track, int loop); // "A0 84": 1 when the track started
void Cd_Replay(OpenBGI_t* o);                   // MM_MCINOTIFY: the looping track again
void Cd_Stop(OpenBGI_t* o);                     // "A0 85"
int Cd_Status(OpenBGI_t* o, int32_t* out);      // "A0 86": *out = the device's mode; 1 ok, 0 when closed or not answering

#endif // BGI_SOUND_H_
