/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sound.c - the sound manager and CD audio (bgi/sound.h)
 *
 * The manager sits between the "A0 xx" handlers (src/vm/ops_snd.c) and
 * the Wave Master library: it keeps the master volumes (re-applied after
 * a mute), finds the files to stream and turns archive names into archive
 * paths.  A missing file is retried through the engine's retry prompt
 * where the original does so (the archive forms); the loose-file form
 * just reports the result.  CD audio is the host's "cdaudio" device
 * (OpenBGI_CdAudio_t, Host_Cd*).
 */
#include <string.h>
#include "bgi/sound.h"
#include "bgi/snd/wavemaster.h"
#include "bgi/file.h"
#include "bgi/msg.h"
#include "bgi/error.h"
#include "bgi/host.h"
#include "bgi/fs.h"
#include "bgi/version.h"
#include "bgi/core.h"

// the sample gain the manager passes to every stream opener (the double 1.0)
#define WM_GAIN 1.0

// ---- volumes -----------------------------------------------------------------

// "A0 08": remember the channel's master volume and apply it unless muted; 1 ok, 0 out of range
int Bgm_SetMasterVolume(OpenBGI_t* o, int ch, uint32_t vol)
{
	if((uint32_t)ch >= BGM_CHANNELS || vol > 0x80)
		return 0;
	o->snd.bgmMasterVolume[ch] = vol;
	if(!o->snd.soundOff)
		Wm_BgmSetMasterVolume(o, ch, vol);
	return 1;
}

// "A0 1C" (1.553 on): the channel's own volume, straight to the library (not remembered, not muted); its result
int Bgm_SetChannelVolume(OpenBGI_t* o, int ch, uint32_t vol)
{
	return Wm_BgmSetChannelVolume(o, ch, vol);
}

// "A0 2C" (1.588 on): the same for an effect slot
int Se_SetChannelVolume(OpenBGI_t* o, int slot, uint32_t vol)
{
	return Wm_SeSetChannelVolume(o, slot, vol);
}

// "A0 09": the same for an effect slot
int Se_SetMasterVolume(OpenBGI_t* o, int slot, uint32_t vol)
{
	if((uint32_t)slot >= SE_SLOTS || vol > 0x80)
		return 0;
	o->snd.seMasterVolume[slot] = vol;
	if(!o->snd.soundOff)
		Wm_SeSetMasterVolume(o, slot, vol);
	return 1;
}

// push every remembered master volume to the library
void Sound_ApplyMasterVolumes(OpenBGI_t* o)
{
	int i;
	for(i = 0; i < BGM_CHANNELS; i++)
		Wm_BgmSetMasterVolume(o, i, o->snd.bgmMasterVolume[i]);
	for(i = 0; i < SE_SLOTS; i++)
		Wm_SeSetMasterVolume(o, i, o->snd.seMasterVolume[i]);
}

// every master volume back to full (0x80) and applied; part of each script boot
void Sound_ResetVolumes(OpenBGI_t* o)
{
	int i;
	for(i = 0; i < BGM_CHANNELS; i++)
		o->snd.bgmMasterVolume[i] = 0x80;
	for(i = 0; i < SE_SLOTS; i++)
		o->snd.seMasterVolume[i] = 0x80;
	Sound_ApplyMasterVolumes(o);
}

// "80 65" (minimise): mute every channel and slot; the master volumes stay remembered
void Sound_StopAll(OpenBGI_t* o)
{
	int i;
	if(o->snd.soundOff)
		return;
	o->snd.soundOff = 1;
	for(i = 0; i < BGM_CHANNELS; i++)
		Wm_BgmSetMasterVolume(o, i, 0);
	for(i = 0; i < SE_SLOTS; i++)
		Wm_SeSetMasterVolume(o, i, 0);
}

// the window's activation after the minimise: the remembered volumes back
void Sound_ResumeAll(OpenBGI_t* o)
{
	if(o->snd.soundOff)
	{
		Sound_ApplyMasterVolumes(o);
		o->snd.soundOff = 0;
	}
}

/* 1.547 on: the window lost the focus while "81 68" is set - every
 * playing sound stops where it is - or got it back - the held ones play
 * on (Wm_HoldAll) */
void Sound_HoldAll(OpenBGI_t* o, int hold)
{
	Wm_HoldAll(o, hold);
}

// ---- BGM -----------------------------------------------------------------------

/* "A0 10": stream a loose file from the base directory, then (when the
 * file is missing and the disc is in) from the disc directory; pan
 * centred.  The channel is stopped first.  The library's result. */
int Bgm_PlayFile(OpenBGI_t* o, int ch, const char* name, int volume)
{
	char path[0x100];
	int r;
	Wm_BgmStop(o, ch);
	PathJoin(path, o->files.baseDir, name);
	r = Wm_BgmOpenFile(o, ch, path, volume, 0x40, WM_GAIN);
	if(r != WM_FILE_NOT_FOUND || !DriveReady(o, o->files.altDir))
		return r;
	PathJoin(path, o->files.altDir, name);
	return Wm_BgmOpenFile(o, ch, path, volume, 0x40, WM_GAIN);
}

/* stream a loose file in `dir`, then in each "80 3x" search
 * sub-directory of it while the file is missing; WM_FILE_NOT_FOUND when
 * the drive is not ready */
static int Bgm_TryDir(OpenBGI_t* o, int ch, const char* dir, const char* name, int volume, int pan)
{
	char path[0x100];
	const SearchNode_t* n;
	int r;
	if(!DriveReady(o, dir))
		return WM_FILE_NOT_FOUND;
	sprintf(path, "%s%s", dir, name);
	r = Wm_BgmOpenFile(o, ch, path, volume, pan, WM_GAIN);
	for(n = o->files.fileSearchOn ? o->files.searchList : NULL; n && r == WM_FILE_NOT_FOUND; n = n->next)
	{
		if(!DriveReady(o, dir))
			continue;
		sprintf(path, "%s%s\\%s", dir, n->name, name);
		r = Wm_BgmOpenFile(o, ch, path, volume, pan, WM_GAIN);
	}
	return r;
}

// Bgm_TryDir for an intro / loop pair (both names in the same directory)
static int Bgm_TryDirPair(OpenBGI_t* o, int ch, const char* dir, const char* intro, const char* loop, int mode, int volume, int pan)
{
	char p1[0x100], p2[0x100];
	const SearchNode_t* n;
	int r;
	if(!DriveReady(o, dir))
		return WM_FILE_NOT_FOUND;
	sprintf(p1, "%s%s", dir, intro);
	sprintf(p2, "%s%s", dir, loop);
	r = Wm_BgmOpenFilePair(o, ch, p1, p2, mode, volume, pan, WM_GAIN);
	for(n = o->files.fileSearchOn ? o->files.searchList : NULL; n && r == WM_FILE_NOT_FOUND; n = n->next)
	{
		if(!DriveReady(o, dir))
			continue;
		sprintf(p1, "%s%s\\%s", dir, n->name, intro);
		sprintf(p2, "%s%s\\%s", dir, n->name, loop);
		r = Wm_BgmOpenFilePair(o, ch, p1, p2, mode, volume, pan, WM_GAIN);
	}
	return r;
}

/* "A0 11": stream `name` from archive `arc` (NULL: a loose file through
 * Bgm_TryDir).  A loose file in the base directory wins over the archive.
 * The base directory is tried first; while the file is missing the disc
 * directory is tried and the user asked to retry (RetryPrompt throws when
 * the user aborts).  The library's result. */
int Bgm_PlayArc(OpenBGI_t* o, int ch, const char* arc, const char* name, int volume, int pan)
{
	char path[0x100], arcPath[0x100], msg[0x100];
	int r;
	Wm_BgmStop(o, ch);
	r = Bgm_TryDir(o, ch, o->files.baseDir, name, volume, pan);
	if(r != WM_FILE_NOT_FOUND)
		return r;
	if(!arc)
	{
		for(;;)
		{
			r = Bgm_TryDir(o, ch, o->files.altDir, name, volume, pan);
			if(r != WM_FILE_NOT_FOUND)
				return r;
			sprintf(msg, MSG_FILE_NOT_FOUND, name);
			RetryPrompt(o, msg);
		}
	}
	PathJoin(path, o->files.baseDir, arc);
	r = WM_FILE_NOT_FOUND;
	if(ArcMgr_ArcNameFor(o, o->files.arcMgr, arcPath, path, name)) // the path of the archive file that holds `name`
	{
		r = Wm_BgmOpenArc(o, ch, arcPath, name, volume, pan, WM_GAIN);
		if(r != WM_FILE_NOT_FOUND)
			return r;
	}
	for(;;)
	{
		if(DriveReady(o, o->files.altDir))
		{
			PathJoin(path, o->files.altDir, arc);
			r = WM_FILE_NOT_FOUND;
			if(ArcMgr_ArcNameFor(o, o->files.arcMgr, arcPath, path, name))
				r = Wm_BgmOpenArc(o, ch, arcPath, name, volume, pan, WM_GAIN);
		}
		if(r != WM_FILE_NOT_FOUND)
			return r;
		sprintf(msg, MSG_ARC_FILE_NOT_FOUND, arc, name);
		RetryPrompt(o, msg);
	}
}

/* "A0 12": Bgm_PlayArc for an intro / loop pair; `mode` non-zero repeats
 * the loop part (the same name twice makes a single looping file).  The
 * archive is located by the intro's name. */
int Bgm_PlayArcPair(OpenBGI_t* o, int ch, const char* arc, const char* intro, const char* loop, int mode, int volume, int pan)
{
	char path[0x100], arcPath[0x100], msg[0x100];
	int r;
	Wm_BgmStop(o, ch);
	r = Bgm_TryDirPair(o, ch, o->files.baseDir, intro, loop, mode, volume, pan);
	if(r != WM_FILE_NOT_FOUND)
		return r;
	if(!arc)
	{
		for(;;)
		{
			r = Bgm_TryDirPair(o, ch, o->files.altDir, intro, loop, mode, volume, pan);
			if(r != WM_FILE_NOT_FOUND)
				return r;
			sprintf(msg, MSG_SND_PAIR_NOT_FOUND, intro, loop);
			RetryPrompt(o, msg);
		}
	}
	PathJoin(path, o->files.baseDir, arc);
	r = WM_FILE_NOT_FOUND;
	if(ArcMgr_ArcNameFor(o, o->files.arcMgr, arcPath, path, intro))
	{
		r = Wm_BgmOpenArcPair(o, ch, arcPath, intro, loop, mode, volume, pan, WM_GAIN);
		if(r != WM_FILE_NOT_FOUND)
			return r;
	}
	for(;;)
	{
		if(DriveReady(o, o->files.altDir))
		{
			PathJoin(path, o->files.altDir, arc);
			r = WM_FILE_NOT_FOUND;
			if(ArcMgr_ArcNameFor(o, o->files.arcMgr, arcPath, path, intro))
				r = Wm_BgmOpenArcPair(o, ch, arcPath, intro, loop, mode, volume, pan, WM_GAIN);
		}
		if(r != WM_FILE_NOT_FOUND)
			return r;
		sprintf(msg, MSG_SND_ARC_PAIR_NOT_FOUND, arc, intro, loop);
		RetryPrompt(o, msg);
	}
}

/* "A0 15": whether the channel is playing (1 / 0; 0 for a bad channel);
 * while it is, *outLength receives the stream's loop count.  The names
 * "position / length" are historical: what the library answers is the
 * voice's playing status and the loop counter. */
uint32_t Bgm_GetPosition(OpenBGI_t* o, int ch, uint32_t* outLength)
{
	uint32_t playing = 0;
	if((uint32_t)ch >= BGM_CHANNELS)
		return 0;
	Wm_BgmIsPlaying(o, ch, &playing);
	if(playing)
		Wm_BgmLoopCount(o, ch, outLength);
	return playing;
}

// "A0 14"
int Bgm_Pause(OpenBGI_t* o, int ch, int paused)
{
	return Wm_BgmPause(o, ch, paused);
}

// "A0 16"
int Bgm_FadeVolume(OpenBGI_t* o, int ch, int volume, int ms)
{
	return Wm_BgmFadeVolume(o, ch, volume, ms);
}

// "A0 17"
int Bgm_SetPan(OpenBGI_t* o, int ch, int pan)
{
	return Wm_BgmSetPan(o, ch, pan);
}

// "A0 18"
int Bgm_FadeIn(OpenBGI_t* o, int ch, int ms)
{
	return Wm_BgmFadeIn(o, ch, ms);
}

// "A0 19"
int Bgm_FadeOut(OpenBGI_t* o, int ch, int ms)
{
	return Wm_BgmFadeOut(o, ch, ms);
}

// ---- sound effects ----------------------------------------------------------------

// forget every effect descriptor (the library's slots are not touched); part of each script boot
void Sound_ClearTable(OpenBGI_t* o)
{
	memset(o->snd.seDesc, 0, sizeof o->snd.seDesc);
}

/* "A0 20" / "A0 21" / "A0 27" through the loader: copy the BW header of
 * the file in memory to the slot's descriptor and hand the file to the
 * library; the library's result (the slot's descriptor is kept even when
 * the load fails).  From 1.547 on the descriptor's word at +0x3c becomes
 * 65536 / rate, which scales the length Se_GetLength reports. */
uint32_t Sound_LoadSe(OpenBGI_t* o, int slot, const void* desc, int fadeInMs, double gain, double rate)
{
	memcpy(o->snd.seDesc[slot], desc, SE_DESC_SIZE);
	if(o->engine.profile->gen >= GEN_1_547)
	{
		double step = 65536.0 / rate;
		uint32_t v = step >= -9.2e18 && step <= 9.2e18 ? (uint32_t)(int64_t)step : 0x80000000u; // (the x87's "indefinite" otherwise)
		memcpy(o->snd.seDesc[slot] + 0x3c, &v, 4);
	}
	return Wm_SeLoad(o, slot, desc, fadeInMs, gain, rate);
}

// "A0 22"
int Se_Unload(OpenBGI_t* o, int slot)
{
	return Wm_SeUnload(o, slot);
}

// "A0 24"
int Se_Play(OpenBGI_t* o, int slot, int volume, int pan)
{
	return Wm_SePlay(o, slot, volume, pan);
}

// "A0 25"
int Se_Stop(OpenBGI_t* o, int slot)
{
	return Wm_SeStop(o, slot);
}

// "A0 26"
int Se_FadeOut(OpenBGI_t* o, int slot, int ms)
{
	return Wm_SeFadeOut(o, slot, ms);
}

/* the effect's length in ms from its descriptor (the BW header): frames
 * * 1000 / sample rate, truncated; 0 for an empty slot.  From 1.547 on it
 * is scaled by the descriptor's word at +0x3c (65536 / the load's rate,
 * Sound_LoadSe): ms * that, in its low 32 bits, shifted down by 16 - so
 * the length of a sound longer than 65.5 s wraps, as in the original.
 * "A0 24" pushes it when the effect started, "A0 2F" always. */
int32_t Se_GetLength(OpenBGI_t* o, int slot)
{
	const uint32_t* d = (const uint32_t*)o->snd.seDesc[slot];
	double ms;
	if(d[4] == 0) // the sample rate
		return 0;
	ms = (double)(uint64_t)d[3] * 1000.0 / (double)(uint64_t)d[4];
	if(o->engine.profile->gen >= GEN_1_547)
		return (int32_t)((uint32_t)(int64_t)(ms * (double)(uint64_t)d[15]) >> 16);
	return BGI_Ftol(ms);
}

/* "A0 C0": play a wave file of the base directory, as the original's
 * PlaySound does (Wm_PlayWave: once, at full volume, cutting off the one
 * before); 1 when it plays, 0 when the file is missing or not one the
 * mixer takes */
int Sound_PlayWave(OpenBGI_t* o, const char* name)
{
	char path[0x104];
	uint32_t size;
	uint8_t* file;
	int r;
	snprintf(path, sizeof path, "%s%s", o->files.baseDir, name);
	file = Fs_LoadFile(o, path, &size);
	if(!file)
		return 0;
	r = Wm_PlayWave(o, file, size);
	free(file);
	return r;
}

/* start the library and its fade timer (Engine_Init).  The original shows
 * MSG_WAVEMASTER_START_FAILED when the timer start returns 8 - a code the
 * library never produces - and reports success regardless, so a missing
 * sound device does not stop the engine. */
int Sound_Init(OpenBGI_t* o)
{
	if(Wm_Init(o) == 0)
	{
		int r = Wm_StartTimer(o);
		if(r != 0 && r == 8)
			ShowErrorBox(o, MSG_WAVEMASTER_START_FAILED);
	}
	return 1;
}

// stop the fade timer and the library (Engine_Shutdown)
void Sound_Shutdown(OpenBGI_t* o)
{
	Wm_StopTimer(o);
	Wm_Shutdown(o);
}

// ---- CD audio -------------------------------------------------------------------

/* "A0 80": open the "cdaudio" MCI device; when the base (or else the
 * disc) directory is on a CD-ROM drive, that drive is the element.  Time
 * format TMSF.  1 when open (also when it already was), 0 when the device
 * could not be opened or set up. */
int Cd_Open(OpenBGI_t* o)
{
	char drive[3];
	int onCd;
	uint32_t dev;
	if(o->snd.cdDevice)
		return 1;
	memcpy(drive, o->files.baseDir, 2); // "X:" of the base directory
	drive[2] = 0;
	onCd = Drive_TypeOf(o, drive[0]) == OPENBGI_DRIVE_CDROM;
	if(!onCd && o->files.altDir[0])
	{
		memcpy(drive, o->files.altDir, 2);
		onCd = Drive_TypeOf(o, drive[0]) == OPENBGI_DRIVE_CDROM;
	}
	drive[0] = (char)(drive[0] | 0x20); // lower-case drive letter
	dev = Host_CdOpen(o, onCd ? drive : NULL);
	if(!dev)
		return 0;
	if(!Host_CdSetTimeFormatTmsf(o, dev))
	{
		Host_CdClose(o, dev); // (the original calls Sound_CdClose before recording the device, so it leaks it)
		return 0;
	}
	o->snd.cdDevice = dev;
	return 1;
}

// "A0 81": close the device, if open
void Sound_CdClose(OpenBGI_t* o)
{
	if(o->snd.cdDevice)
	{
		Host_CdClose(o, o->snd.cdDevice);
		o->snd.cdDevice = 0;
	}
}

// the number of tracks on the disc; 0 when the device is closed or does not answer
static uint32_t Cd_TrackCount(OpenBGI_t* o)
{
	uint32_t n = 0;
	if(!o->snd.cdDevice || !Host_CdStatus(o, o->snd.cdDevice, OPENBGI_MCI_STATUS_TRACKS, &n))
		return 0;
	return n;
}

/* "A0 84": play `track` (TMSF: from track to track + 1); `loop` asks for
 * the end-of-play notification, on which Cd_Replay starts the track again.
 * 1 when the play command was accepted, 0 when the device is closed or
 * the disc has no tracks. */
int Cd_Play(OpenBGI_t* o, int track, int loop)
{
	int ok;
	if(!Cd_TrackCount(o))
		return 0;
	ok = Host_CdPlay(o, o->snd.cdDevice, (uint32_t)track & 0xff, (uint32_t)(track + 1) & 0xff, loop != 0);
	o->snd.cdTrack = track;
	return ok;
}

// MM_MCINOTIFY: the looping track reached its end; play it again
void Cd_Replay(OpenBGI_t* o)
{
	Cd_Play(o, o->snd.cdTrack, 1);
}

// "A0 85"
void Cd_Stop(OpenBGI_t* o)
{
	if(o->snd.cdDevice)
		Host_CdStop(o, o->snd.cdDevice);
}

/* "A0 86": the device's mode into *out: 0 not ready, 1 stopped, 2
 * playing, 3 recording, 4 seeking, 5 paused, 6 open, -1 any other; 1 ok,
 * 0 when the device is closed or does not answer (*out untouched) */
int Cd_Status(OpenBGI_t* o, int32_t* out)
{
	uint32_t mode;
	if(!o->snd.cdDevice || !Host_CdStatus(o, o->snd.cdDevice, OPENBGI_MCI_STATUS_MODE, &mode))
		return 0;
	if(mode - OPENBGI_MCI_MODE_NOT_READY <= 6) // the MCI_MODE_* constants are consecutive from "not ready"
		*out = (int32_t)(mode - OPENBGI_MCI_MODE_NOT_READY);
	else
		*out = -1;
	return 1;
}
