/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sound.h - the sound manager used by the "A0 xx" instructions
 *           (src/snd/sound.c) on top of the "Wave Master" library
 *           (inc/bgi/snd/wavemaster.h).
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

extern int gSoundOff;                           // muted by Sound_StopAll (minimise)
extern uint32_t gBgmMasterVolume[BGM_CHANNELS]; // 0 .. 0x80 per channel
extern uint32_t gSeMasterVolume[SE_SLOTS];      // 0 .. 0x80 per slot
extern uint8_t gSeDesc[SE_SLOTS][SE_DESC_SIZE]; // the loaded effects' descriptors

int Sound_Init(void);                           // start-up (Engine_Init); always 1
void Sound_Shutdown(void);                      // stop the fade timer and the library
void Sound_ResetVolumes(void);                  // every master volume to 0x80, applied (each script boot)
void Sound_ApplyMasterVolumes(void);            // push the remembered master volumes to the library
int Bgm_SetMasterVolume(int ch, uint32_t vol);  // "A0 08": 1 ok, 0 out of range (not applied while muted)
int Se_SetMasterVolume(int slot, uint32_t vol); // "A0 09": 1 ok, 0 out of range
/* "A0 10": play a loose file from the base (then the disc) directory;
 * "A0 11": from an archive (NULL arc = loose file with the search
 * sub-directories), asking to retry while the file is missing; "A0 12":
 * an intro / loop pair, `flag` non-zero repeating the loop part.  Library
 * results. */
int Bgm_PlayFile(int ch, const char* name, int volume);
int Bgm_PlayArc(int ch, const char* arc, const char* name, int volume, int pan);
int Bgm_PlayArcPair(int ch, const char* arc, const char* intro, const char* loop, int flag, int volume, int pan);
/* "A0 15": 1 while the channel plays, with *outLength = the stream's loop
 * count; 0 otherwise and for a bad channel */
uint32_t Bgm_GetPosition(int ch, uint32_t* outLength);
void Sound_ClearTable(void); // forget every effect descriptor (each script boot)
/* "A0 20" / "A0 21" through the loader: load a sound effect into a slot
 * from the BW file in memory (its 0x40-byte header is copied to the slot
 * table); `fadeInMs` is a fade-in burnt into the decoded samples, `gain`
 * multiplies them; the library's result */
uint32_t Sound_LoadSe(int slot, const void* desc, int fadeInMs, double gain);
int Se_Unload(int slot);                    // "A0 22"
int Se_Play(int slot, int volume, int pan); // "A0 24"
int Se_Stop(int slot);                      // "A0 25"
int Se_FadeOut(int slot, int ms);           // "A0 26"
int32_t Se_GetLength(int slot);             // "A0 24": ms from the descriptor (frames * 1000 / rate); 0 when empty
void Sound_StopAll(void);                   // "80 65", minimise: mute everything
void Sound_ResumeAll(void);                 // the window's activation after the minimise: the volumes back
int Sound_PlayWave(const char* name);       // "A0 C0": PlaySound of a file in the base directory; 1 when accepted
const char* Sound_LibVersion(void);         // "Wave Master" version string

// BGM channel control (the library's locked entry points)
int Bgm_Pause(int ch, int paused);              // "A0 14"
int Bgm_FadeVolume(int ch, int volume, int ms); // "A0 16"
int Bgm_SetPan(int ch, int pan);                // "A0 17"
int Bgm_FadeIn(int ch, int ms);                 // "A0 18"
int Bgm_FadeOut(int ch, int ms);                // "A0 19"

// CD audio
int Cd_Open(void);                // "A0 80": 1 when the device is open
void Sound_CdClose(void);         // "A0 81"
int Cd_Play(int track, int loop); // "A0 84": 1 when the track started
void Cd_Replay(void);             // MM_MCINOTIFY: the looping track again
void Cd_Stop(void);               // "A0 85"
int Cd_Status(int32_t* out);      // "A0 86": *out = the device's mode; 1 ok, 0 when closed or not answering

#endif // BGI_SOUND_H_
