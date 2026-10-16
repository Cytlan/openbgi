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
 * just reports the result.  CD audio goes through the OS layer's MCI
 * wrappers.
 */
#include <string.h>
#include "bgi/sound.h"
#include "bgi/snd/wavemaster.h"
#include "bgi/file.h"
#include "bgi/msg.h"
#include "bgi/error.h"
#include "bgi/os.h"

int gSoundOff;                           // 1 while Sound_StopAll has muted everything
uint32_t gBgmMasterVolume[BGM_CHANNELS]; // 0 .. 0x80 per channel, kept across the mute
uint32_t gSeMasterVolume[SE_SLOTS];      // 0 .. 0x80 per slot
uint8_t gSeDesc[SE_SLOTS][SE_DESC_SIZE]; // the BW header of every loaded effect (zero: empty)

// the sample gain the manager passes to every stream opener (the double 1.0)
#define WM_GAIN 1.0

// ---- volumes -----------------------------------------------------------------

// "A0 08": remember the channel's master volume and apply it unless muted; 1 ok, 0 out of range
int Bgm_SetMasterVolume(int ch, uint32_t vol)
{
	if((uint32_t)ch >= BGM_CHANNELS || vol > 0x80)
		return 0;
	gBgmMasterVolume[ch] = vol;
	if(!gSoundOff)
		Wm_BgmSetMasterVolume(ch, vol);
	return 1;
}

// "A0 09": the same for an effect slot
int Se_SetMasterVolume(int slot, uint32_t vol)
{
	if((uint32_t)slot >= SE_SLOTS || vol > 0x80)
		return 0;
	gSeMasterVolume[slot] = vol;
	if(!gSoundOff)
		Wm_SeSetMasterVolume(slot, vol);
	return 1;
}

// push every remembered master volume to the library
void Sound_ApplyMasterVolumes(void)
{
	int i;
	for(i = 0; i < BGM_CHANNELS; i++)
		Wm_BgmSetMasterVolume(i, gBgmMasterVolume[i]);
	for(i = 0; i < SE_SLOTS; i++)
		Wm_SeSetMasterVolume(i, gSeMasterVolume[i]);
}

// every master volume back to full (0x80) and applied; part of each script boot
void Sound_ResetVolumes(void)
{
	int i;
	for(i = 0; i < BGM_CHANNELS; i++)
		gBgmMasterVolume[i] = 0x80;
	for(i = 0; i < SE_SLOTS; i++)
		gSeMasterVolume[i] = 0x80;
	Sound_ApplyMasterVolumes();
}

// "80 65" (minimise): mute every channel and slot; the master volumes stay remembered
void Sound_StopAll(void)
{
	int i;
	if(gSoundOff)
		return;
	gSoundOff = 1;
	for(i = 0; i < BGM_CHANNELS; i++)
		Wm_BgmSetMasterVolume(i, 0);
	for(i = 0; i < SE_SLOTS; i++)
		Wm_SeSetMasterVolume(i, 0);
}

// the window's activation after the minimise: the remembered volumes back
void Sound_ResumeAll(void)
{
	if(gSoundOff)
	{
		Sound_ApplyMasterVolumes();
		gSoundOff = 0;
	}
}

// ---- BGM -----------------------------------------------------------------------

/* "A0 10": stream a loose file from the base directory, then (when the
 * file is missing and the disc is in) from the disc directory; pan
 * centred.  The channel is stopped first.  The library's result. */
int Bgm_PlayFile(int ch, const char* name, int volume)
{
	char path[0x100];
	int r;
	Wm_BgmStop(ch);
	PathJoin(path, gBaseDir, name);
	r = Wm_BgmOpenFile(ch, path, volume, 0x40, WM_GAIN);
	if(r != WM_FILE_NOT_FOUND || !DriveReady(gAltDir))
		return r;
	PathJoin(path, gAltDir, name);
	return Wm_BgmOpenFile(ch, path, volume, 0x40, WM_GAIN);
}

/* stream a loose file in `dir`, then in each "80 3x" search
 * sub-directory of it while the file is missing; WM_FILE_NOT_FOUND when
 * the drive is not ready */
static int Bgm_TryDir(int ch, const char* dir, const char* name, int volume, int pan)
{
	char path[0x100];
	const SearchNode_t* n;
	int r;
	if(!DriveReady(dir))
		return WM_FILE_NOT_FOUND;
	sprintf(path, "%s%s", dir, name);
	r = Wm_BgmOpenFile(ch, path, volume, pan, WM_GAIN);
	for(n = gFileSearchOn ? gSearchList : NULL; n && r == WM_FILE_NOT_FOUND; n = n->next)
	{
		if(!DriveReady(dir))
			continue;
		sprintf(path, "%s%s\\%s", dir, n->name, name);
		r = Wm_BgmOpenFile(ch, path, volume, pan, WM_GAIN);
	}
	return r;
}

// Bgm_TryDir for an intro / loop pair (both names in the same directory)
static int Bgm_TryDirPair(int ch, const char* dir, const char* intro, const char* loop, int mode, int volume, int pan)
{
	char p1[0x100], p2[0x100];
	const SearchNode_t* n;
	int r;
	if(!DriveReady(dir))
		return WM_FILE_NOT_FOUND;
	sprintf(p1, "%s%s", dir, intro);
	sprintf(p2, "%s%s", dir, loop);
	r = Wm_BgmOpenFilePair(ch, p1, p2, mode, volume, pan, WM_GAIN);
	for(n = gFileSearchOn ? gSearchList : NULL; n && r == WM_FILE_NOT_FOUND; n = n->next)
	{
		if(!DriveReady(dir))
			continue;
		sprintf(p1, "%s%s\\%s", dir, n->name, intro);
		sprintf(p2, "%s%s\\%s", dir, n->name, loop);
		r = Wm_BgmOpenFilePair(ch, p1, p2, mode, volume, pan, WM_GAIN);
	}
	return r;
}

/* "A0 11": stream `name` from archive `arc` (NULL: a loose file through
 * Bgm_TryDir).  A loose file in the base directory wins over the archive.
 * The base directory is tried first; while the file is missing the disc
 * directory is tried and the user asked to retry (RetryPrompt throws when
 * the user aborts).  The library's result. */
int Bgm_PlayArc(int ch, const char* arc, const char* name, int volume, int pan)
{
	char path[0x100], arcPath[0x100], msg[0x100];
	int r;
	Wm_BgmStop(ch);
	r = Bgm_TryDir(ch, gBaseDir, name, volume, pan);
	if(r != WM_FILE_NOT_FOUND)
		return r;
	if(!arc)
	{
		for(;;)
		{
			r = Bgm_TryDir(ch, gAltDir, name, volume, pan);
			if(r != WM_FILE_NOT_FOUND)
				return r;
			sprintf(msg, MSG_FILE_NOT_FOUND, name);
			RetryPrompt(msg);
		}
	}
	PathJoin(path, gBaseDir, arc);
	r = WM_FILE_NOT_FOUND;
	if(ArcMgr_ArcNameFor(gArcMgr, arcPath, path, name)) // the path of the archive file that holds `name`
	{
		r = Wm_BgmOpenArc(ch, arcPath, name, volume, pan, WM_GAIN);
		if(r != WM_FILE_NOT_FOUND)
			return r;
	}
	for(;;)
	{
		if(DriveReady(gAltDir))
		{
			PathJoin(path, gAltDir, arc);
			r = WM_FILE_NOT_FOUND;
			if(ArcMgr_ArcNameFor(gArcMgr, arcPath, path, name))
				r = Wm_BgmOpenArc(ch, arcPath, name, volume, pan, WM_GAIN);
		}
		if(r != WM_FILE_NOT_FOUND)
			return r;
		sprintf(msg, MSG_ARC_FILE_NOT_FOUND, arc, name);
		RetryPrompt(msg);
	}
}

/* "A0 12": Bgm_PlayArc for an intro / loop pair; `mode` non-zero repeats
 * the loop part (the same name twice makes a single looping file).  The
 * archive is located by the intro's name. */
int Bgm_PlayArcPair(int ch, const char* arc, const char* intro, const char* loop, int mode, int volume, int pan)
{
	char path[0x100], arcPath[0x100], msg[0x100];
	int r;
	Wm_BgmStop(ch);
	r = Bgm_TryDirPair(ch, gBaseDir, intro, loop, mode, volume, pan);
	if(r != WM_FILE_NOT_FOUND)
		return r;
	if(!arc)
	{
		for(;;)
		{
			r = Bgm_TryDirPair(ch, gAltDir, intro, loop, mode, volume, pan);
			if(r != WM_FILE_NOT_FOUND)
				return r;
			sprintf(msg, MSG_SND_PAIR_NOT_FOUND, intro, loop);
			RetryPrompt(msg);
		}
	}
	PathJoin(path, gBaseDir, arc);
	r = WM_FILE_NOT_FOUND;
	if(ArcMgr_ArcNameFor(gArcMgr, arcPath, path, intro))
	{
		r = Wm_BgmOpenArcPair(ch, arcPath, intro, loop, mode, volume, pan, WM_GAIN);
		if(r != WM_FILE_NOT_FOUND)
			return r;
	}
	for(;;)
	{
		if(DriveReady(gAltDir))
		{
			PathJoin(path, gAltDir, arc);
			r = WM_FILE_NOT_FOUND;
			if(ArcMgr_ArcNameFor(gArcMgr, arcPath, path, intro))
				r = Wm_BgmOpenArcPair(ch, arcPath, intro, loop, mode, volume, pan, WM_GAIN);
		}
		if(r != WM_FILE_NOT_FOUND)
			return r;
		sprintf(msg, MSG_SND_ARC_PAIR_NOT_FOUND, arc, intro, loop);
		RetryPrompt(msg);
	}
}

/* "A0 15": whether the channel is playing (1 / 0; 0 for a bad channel);
 * while it is, *outLength receives the stream's loop count.  The names
 * "position / length" are historical: what the library answers is the
 * voice's playing status and the loop counter. */
uint32_t Bgm_GetPosition(int ch, uint32_t* outLength)
{
	uint32_t playing = 0;
	if((uint32_t)ch >= BGM_CHANNELS)
		return 0;
	Wm_BgmIsPlaying(ch, &playing);
	if(playing)
		Wm_BgmLoopCount(ch, outLength);
	return playing;
}

// "A0 14"
int Bgm_Pause(int ch, int paused)
{
	return Wm_BgmPause(ch, paused);
}

// "A0 16"
int Bgm_FadeVolume(int ch, int volume, int ms)
{
	return Wm_BgmFadeVolume(ch, volume, ms);
}

// "A0 17"
int Bgm_SetPan(int ch, int pan)
{
	return Wm_BgmSetPan(ch, pan);
}

// "A0 18"
int Bgm_FadeIn(int ch, int ms)
{
	return Wm_BgmFadeIn(ch, ms);
}

// "A0 19"
int Bgm_FadeOut(int ch, int ms)
{
	return Wm_BgmFadeOut(ch, ms);
}

// ---- sound effects ----------------------------------------------------------------

// forget every effect descriptor (the library's slots are not touched); part of each script boot
void Sound_ClearTable(void)
{
	memset(gSeDesc, 0, sizeof gSeDesc);
}

/* "A0 20" / "A0 21" through the loader: copy the BW header of the file
 * in memory to the slot's descriptor and hand the file to the library;
 * the library's result (the slot's descriptor is kept even when the load
 * fails) */
uint32_t Sound_LoadSe(int slot, const void* desc, int fadeInMs, double gain)
{
	memcpy(gSeDesc[slot], desc, SE_DESC_SIZE);
	return Wm_SeLoad(slot, desc, fadeInMs, gain);
}

// "A0 22"
int Se_Unload(int slot)
{
	return Wm_SeUnload(slot);
}

// "A0 24"
int Se_Play(int slot, int volume, int pan)
{
	return Wm_SePlay(slot, volume, pan);
}

// "A0 25"
int Se_Stop(int slot)
{
	return Wm_SeStop(slot);
}

// "A0 26"
int Se_FadeOut(int slot, int ms)
{
	return Wm_SeFadeOut(slot, ms);
}

/* the effect's length in ms from its descriptor (the BW header): frames
 * * 1000 / sample rate, truncated; 0 for an empty slot.  "A0 24" pushes
 * it when the effect started. */
int32_t Se_GetLength(int slot)
{
	const uint32_t* d = (const uint32_t*)gSeDesc[slot];
	if(d[4] == 0) // the sample rate
		return 0;
	return BGI_Ftol((double)(uint64_t)d[3] * 1000.0 / (double)(uint64_t)d[4]);
}

// "A0 C0": play a wave file of the base directory through the OS (PlaySound); 1 when accepted
int Sound_PlayWave(const char* name)
{
	char path[0x104];
	sprintf(path, "%s%s", gBaseDir, name);
	return OS_PlaySoundFile(path);
}

// the library's version string (shown in the window title)
const char* Sound_LibVersion(void)
{
	return Wm_Version();
}

/* start the library and its fade timer (Engine_Init).  The original shows
 * MSG_WAVEMASTER_START_FAILED when the timer start returns 8 - a code the
 * library never produces - and reports success regardless, so a missing
 * sound device does not stop the engine. */
int Sound_Init(void)
{
	if(Wm_Init() == 0)
	{
		int r = Wm_StartTimer();
		if(r != 0 && r == 8)
			ShowErrorBox(MSG_WAVEMASTER_START_FAILED);
	}
	return 1;
}

// stop the fade timer and the library (Engine_Shutdown)
void Sound_Shutdown(void)
{
	Wm_StopTimer();
	Wm_Shutdown();
}

// ---- CD audio -------------------------------------------------------------------

static uint32_t gCdDevice; // MCI device id, 0 = closed
static int gCdTrack;       // the track last started (replayed on MM_MCINOTIFY)

/* "A0 80": open the "cdaudio" MCI device; when the base (or else the
 * disc) directory is on a CD-ROM drive, that drive is the element.  Time
 * format TMSF.  1 when open (also when it already was), 0 when the device
 * could not be opened or set up. */
int Cd_Open(void)
{
	char drive[3];
	int onCd;
	uint32_t dev;
	if(gCdDevice)
		return 1;
	memcpy(drive, gBaseDir, 2); // "X:" of the base directory
	drive[2] = 0;
	onCd = Drive_TypeOf(drive[0]) == OS_DRIVE_CDROM;
	if(!onCd && gAltDir[0])
	{
		memcpy(drive, gAltDir, 2);
		onCd = Drive_TypeOf(drive[0]) == OS_DRIVE_CDROM;
	}
	drive[0] = (char)(drive[0] | 0x20); // lower-case drive letter
	dev = OS_MciOpenCdAudio(onCd ? drive : NULL);
	if(!dev)
		return 0;
	if(!OS_MciSetTimeFormatTmsf(dev))
	{
		OS_MciClose(dev); // (the original calls Sound_CdClose before recording the device, so it leaks it)
		return 0;
	}
	gCdDevice = dev;
	return 1;
}

// "A0 81": close the device, if open
void Sound_CdClose(void)
{
	if(gCdDevice)
	{
		OS_MciClose(gCdDevice);
		gCdDevice = 0;
	}
}

// the number of tracks on the disc; 0 when the device is closed or does not answer
static uint32_t Cd_TrackCount(void)
{
	uint32_t n = 0;
	if(!gCdDevice || !OS_MciStatus(gCdDevice, OS_MCI_STATUS_TRACKS, &n))
		return 0;
	return n;
}

/* "A0 84": play `track` (TMSF: from track to track + 1); `loop` asks for
 * the end-of-play notification, on which Cd_Replay starts the track again.
 * 1 when the play command was accepted, 0 when the device is closed or
 * the disc has no tracks. */
int Cd_Play(int track, int loop)
{
	int ok;
	if(!Cd_TrackCount())
		return 0;
	ok = OS_MciPlay(gCdDevice, (uint32_t)track & 0xff, (uint32_t)(track + 1) & 0xff, loop != 0);
	gCdTrack = track;
	return ok;
}

// MM_MCINOTIFY: the looping track reached its end; play it again
void Cd_Replay(void)
{
	Cd_Play(gCdTrack, 1);
}

// "A0 85"
void Cd_Stop(void)
{
	if(gCdDevice)
		OS_MciStop(gCdDevice);
}

/* "A0 86": the device's mode into *out: 0 not ready, 1 stopped, 2
 * playing, 3 recording, 4 seeking, 5 paused, 6 open, -1 any other; 1 ok,
 * 0 when the device is closed or does not answer (*out untouched) */
int Cd_Status(int32_t* out)
{
	uint32_t mode;
	if(!gCdDevice || !OS_MciStatus(gCdDevice, OS_MCI_STATUS_MODE, &mode))
		return 0;
	if(mode - OS_MCI_MODE_NOT_READY <= 6) // the MCI_MODE_* constants are consecutive from "not ready"
		*out = (int32_t)(mode - OS_MCI_MODE_NOT_READY);
	else
		*out = -1;
	return 1;
}
