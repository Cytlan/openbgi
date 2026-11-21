/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ops_snd.c - the "A0 xx" sound instruction family: the BGM channels
 * (A0 08 .. A0 19), the sound effect slots (A0 09, A0 20 .. A0 26), CD
 * audio (A0 80 .. A0 86) and PlaySound (A0 C0).  The handlers forward to
 * the sound manager of bgi/sound.h, which sits on the "Wave Master"
 * library; the table is declared in bgi/vm.h.
 *
 * Same conventions as ops_gfx0.c: operands are popped in the original
 * order, "a, b → r" means b is on top of the stack, the Check* validators
 * raise and never return (channels are 0 .. 15, slots 0 .. 63, volumes
 * and pans 0 .. 0x80), library result codes map to script errors.  The
 * effect loads (A0 20, A0 21, A0 27, A0 28) install a wait object and return scheduler
 * code 2 (end of turn, the thread blocked until the load completes);
 * everything else returns 0.
 */
#include "bgi/vm.h"
#include "bgi/wait.h"
#include "bgi/waitobj.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/sys.h"
#include "bgi/sound.h"
#include "bgi/core.h"

// raise a script error with a formatted message; never returns
#define SND_ERROR(o, t, ...)        \
	do                              \
	{                               \
		char msg_[0x104];           \
		sprintf(msg_, __VA_ARGS__); \
		ScriptError(o, msg_, (t));  \
	} while(0)

static int Opcode_Snd_Status(OpenBGI_t* o, Thread_t* t) // A0 00: → 0x14 (the library's "not running" code, always)
{
	Thread_Push(t, 0x14);
	return 0;
}

// the channel's master volume (0 .. 0x80), applied on top of what the scripts set per track
static int Opcode_Snd_BgmMasterVolume(OpenBGI_t* o, Thread_t* t) // A0 08: ch, vol →
{
	uint32_t vol = Thread_Pop(t);
	int ch = (int)Thread_Pop(t);
	CheckVolume(o, vol, t);
	CheckBgmChannel(o, ch, t);
	Bgm_SetMasterVolume(o, ch, vol);
	return 0;
}

// the slot's master volume (0 .. 0x80)
static int Opcode_Snd_SeMasterVolume(OpenBGI_t* o, Thread_t* t) // A0 09: slot, vol →
{
	uint32_t vol = Thread_Pop(t);
	int slot = (int)Thread_Pop(t);
	CheckVolume(o, vol, t);
	CheckSeSlot(o, slot, t);
	Se_SetMasterVolume(o, slot, vol);
	return 0;
}

/* the channel's own volume (0 .. 0x80), a fourth factor besides the
 * master, track and fade volumes, applied at once and kept across tracks
 * (the time signals' volume); the volume is checked before the channel */
static int Opcode_Snd_BgmChannelVolume(OpenBGI_t* o, Thread_t* t) // A0 1C (1.553 on): ch, vol →
{
	uint32_t vol = Thread_Pop(t);
	int ch = (int)Thread_Pop(t);
	CheckVolume(o, vol, t);
	CheckBgmChannel(o, ch, t);
	Bgm_SetChannelVolume(o, ch, vol);
	return 0;
}

// the slot's own volume (0 .. 0x80), kept when the slot is loaded again; the volume is checked first
static int Opcode_Snd_SeChannelVolume(OpenBGI_t* o, Thread_t* t) // A0 2C (1.588 on): slot, vol →
{
	uint32_t vol = Thread_Pop(t);
	int slot = (int)Thread_Pop(t);
	CheckVolume(o, vol, t);
	CheckSeSlot(o, slot, t);
	Se_SetChannelVolume(o, slot, vol);
	return 0;
}

// stream a loose BW file from the base (then the disc) directory on the channel; a missing or non-BW file is a script error
static int Opcode_Snd_BgmPlayFile(OpenBGI_t* o, Thread_t* t) // A0 10: ch, name, vol →
{
	int vol = (int)Thread_Pop(t);
	const char* name = (const char*)PopPtr(o, t);
	int ch = (int)Thread_Pop(t);
	CheckVolume(o, (uint32_t)vol, t);
	CheckBgmChannel(o, ch, t);
	switch(Bgm_PlayFile(o, ch, name, vol))
	{
		case WM_FILE_NOT_FOUND: SND_ERROR(o, t, MSG_BW_NOT_FOUND, name); break;
		case WM_NOT_BW: SND_ERROR(o, t, MSG_NOT_BW, name); break;
		default: break;
	}
	return 0;
}

/* Stream an archive entry (a null arc: a loose file through the search
 * sub-directories) on the channel at `vol` and `pan` (0x40 centred).  The
 * manager asks the user to retry while the file is missing; giving up, or
 * a file that is not a BW file, is a script error. */
static int Opcode_Snd_BgmPlay(OpenBGI_t* o, Thread_t* t) // A0 11: ch, arc, name, vol, pan →
{
	int pan = (int)Thread_Pop(t), vol = (int)Thread_Pop(t);
	const char* name = (const char*)PopPtr(o, t);
	const char* arc = (const char*)PopPtr(o, t);
	int ch = (int)Thread_Pop(t);
	CheckPan(o, (uint32_t)pan, t);
	CheckVolume(o, (uint32_t)vol, t);
	CheckBgmChannel(o, ch, t);
	switch(Bgm_PlayArc(o, ch, arc, name, vol, pan))
	{
		case WM_FILE_NOT_FOUND: SND_ERROR(o, t, MSG_BW_ARC_NOT_FOUND, arc, name); break;
		case WM_NOT_BW: SND_ERROR(o, t, MSG_SE_NOT_BW, arc, name); break;
		default: break;
	}
	return 0;
}

/* As "A0 11" with an intro / loop pair: `intro` plays once, then `loop`
 * repeats while `flag` is non-zero (the same name twice makes a single
 * looping file). */
static int Opcode_Snd_BgmPlayPair(OpenBGI_t* o, Thread_t* t) // A0 12: ch, arc, intro, loop, flag, vol, pan →
{
	int pan = (int)Thread_Pop(t), vol = (int)Thread_Pop(t), flag = (int)Thread_Pop(t);
	const char* loop = (const char*)PopPtr(o, t);
	const char* intro = (const char*)PopPtr(o, t);
	const char* arc = (const char*)PopPtr(o, t);
	int ch = (int)Thread_Pop(t);
	CheckPan(o, (uint32_t)pan, t);
	CheckVolume(o, (uint32_t)vol, t);
	CheckBgmChannel(o, ch, t);
	switch(Bgm_PlayArcPair(o, ch, arc, intro, loop, flag, vol, pan))
	{
		case WM_FILE_NOT_FOUND: SND_ERROR(o, t, MSG_BW_PAIR_NOT_FOUND, arc, intro, loop); break;
		case WM_NOT_BW: SND_ERROR(o, t, MSG_NOT_BW_PAIR, arc, intro, loop); break;
		default: break;
	}
	return 0;
}

static int Opcode_Snd_BgmPause(OpenBGI_t* o, Thread_t* t) // A0 14: ch, run → (run 0 pauses, else resumes)
{
	int run = (int)Thread_Pop(t);
	int ch = (int)Thread_Pop(t);
	CheckBgmChannel(o, ch, t);
	Bgm_Pause(o, ch, run == 0);
	return 0;
}

/* The names are historical: what is pushed is 1 while the channel plays
 * and 0 otherwise, and *outLength receives the stream's loop count (only
 * while it plays). */
static int Opcode_Snd_BgmPosition(OpenBGI_t* o, Thread_t* t) // A0 15: ch, outLength → position
{
	uint32_t* outLength = (uint32_t*)PopPtr(o, t);
	int ch = (int)Thread_Pop(t);
	CheckBgmChannel(o, ch, t);
	Thread_Push(t, Bgm_GetPosition(o, ch, outLength));
	return 0;
}

// fade the channel's volume towards vol (0 .. 0x80) over ms milliseconds
static int Opcode_Snd_BgmFadeVolume(OpenBGI_t* o, Thread_t* t) // A0 16: ch, vol, ms →
{
	int ms = (int)Thread_Pop(t), vol = (int)Thread_Pop(t);
	int ch = (int)Thread_Pop(t);
	CheckVolume(o, (uint32_t)vol, t);
	CheckBgmChannel(o, ch, t);
	Bgm_FadeVolume(o, ch, vol, ms);
	return 0;
}

// the channel's pan, 0 .. 0x80 with 0x40 centred
static int Opcode_Snd_BgmPan(OpenBGI_t* o, Thread_t* t) // A0 17: ch, pan →
{
	int pan = (int)Thread_Pop(t);
	int ch = (int)Thread_Pop(t);
	CheckPan(o, (uint32_t)pan, t);
	CheckBgmChannel(o, ch, t);
	Bgm_SetPan(o, ch, pan);
	return 0;
}

// fade the channel's fade level up to full over ms milliseconds
static int Opcode_Snd_BgmFadeIn(OpenBGI_t* o, Thread_t* t) // A0 18: ch, ms →
{
	int ms = (int)Thread_Pop(t);
	int ch = (int)Thread_Pop(t);
	CheckBgmChannel(o, ch, t);
	Bgm_FadeIn(o, ch, ms);
	return 0;
}

// fade the channel's fade level down to silence over ms milliseconds
static int Opcode_Snd_BgmFadeOut(OpenBGI_t* o, Thread_t* t) // A0 19: ch, ms →
{
	int ms = (int)Thread_Pop(t);
	int ch = (int)Thread_Pop(t);
	CheckBgmChannel(o, ch, t);
	Bgm_FadeOut(o, ch, ms);
	return 0;
}

/* Load the BW file `name` of archive `arc` into effect slot `slot` through
 * the background loader; the thread waits for the load (no fade-in, gain
 * 1.0). */
static int Opcode_Snd_SeLoad(OpenBGI_t* o, Thread_t* t) // A0 20: slot, arc, name →
{
	const char* name = (const char*)PopPtr(o, t);
	const char* arc = (const char*)PopPtr(o, t);
	int slot = (int)Thread_Pop(t);
	CheckSeSlot(o, slot, t);
	Thread_SetWait(o, t, WaitSeLoad_New(o, t, slot, arc, name, 0, 1.0, 1.0));
	return 2;
}

/* A0 21: slot, arc, name, fadeInMs, gain16 →
 * "A0 20" with a fade-in, which the library burns into the decoded
 * samples, and a 16.16 gain that multiplies them (both 0 / 1.0 for
 * "A0 20"). */
static int Opcode_Snd_SeLoadEx(OpenBGI_t* o, Thread_t* t)
{
	int32_t gain16 = (int32_t)Thread_Pop(t);
	int fadeInMs = (int)Thread_Pop(t);
	const char* name = (const char*)PopPtr(o, t);
	const char* arc = (const char*)PopPtr(o, t);
	int slot = (int)Thread_Pop(t);
	CheckSeSlot(o, slot, t);
	Thread_SetWait(o, t, WaitSeLoad_New(o, t, slot, arc, name, fadeInMs, gain16 / 65536.0, 1.0));
	return 2;
}

/* A0 27 (1.547 on): slot, arc, name, fadeInMs, gain16, rate16 →
 * "A0 21" with a 16.16 playback rate: the effect plays that many times as
 * fast at the same pitch (the scripts' voice speed; the library stretches
 * the decoded samples in 30 ms chunks, WmStream_TimeStretch), and the
 * length "A0 24" / "A0 2F" report is divided by it.  Neither the rate nor
 * the gain is range-checked; a rate that is not above 0 plays the effect
 * unstretched (the original's loader hangs on 0). */
static int Opcode_Snd_SeLoadRate(OpenBGI_t* o, Thread_t* t)
{
	int32_t rate16 = (int32_t)Thread_Pop(t);
	int32_t gain16 = (int32_t)Thread_Pop(t);
	int fadeInMs = (int)Thread_Pop(t);
	const char* name = (const char*)PopPtr(o, t);
	const char* arc = (const char*)PopPtr(o, t);
	int slot = (int)Thread_Pop(t);
	CheckSeSlot(o, slot, t);
	Thread_SetWait(o, t, WaitSeLoad_New(o, t, slot, arc, name, fadeInMs, gain16 / 65536.0, rate16 / 65536.0));
	return 2;
}

/* A0 28 (1.547 on): slot, data, fadeInMs, gain16, rate16 →
 * "A0 27" for a BW file the script holds in memory (bsctrl2._bp reads a
 * voice file into a heap block and plays it as an effect): the data is
 * decoded whole before the wait ends, so the script may free it after. */
static int Opcode_Snd_SeLoadMem(OpenBGI_t* o, Thread_t* t)
{
	int32_t rate16 = (int32_t)Thread_Pop(t);
	int32_t gain16 = (int32_t)Thread_Pop(t);
	int fadeInMs = (int)Thread_Pop(t);
	const void* data = PopPtr(o, t);
	int slot = (int)Thread_Pop(t);
	CheckSeSlot(o, slot, t);
	Thread_SetWait(o, t, WaitSeMem_New(o, t, slot, data, fadeInMs, gain16 / 65536.0, rate16 / 65536.0));
	return 2;
}

// A0 2F (1.573 on): the effect's length in ms as "A0 24" reports it, without starting it
static int Opcode_Snd_SeLength(OpenBGI_t* o, Thread_t* t) // A0 2F: slot → length
{
	int slot = (int)Thread_Pop(t);
	CheckSeSlot(o, slot, t);
	Thread_Push(t, (uint32_t)Se_GetLength(o, slot));
	return 0;
}

static int Opcode_Snd_SeUnload(OpenBGI_t* o, Thread_t* t) // A0 22: slot →
{
	int slot = (int)Thread_Pop(t);
	CheckSeSlot(o, slot, t);
	Se_Unload(o, slot);
	return 0;
}

/* A0 24: slot, vol, pan → length
 * Start the effect in the slot at `vol` and `pan`.  The effect's length
 * in milliseconds (from its descriptor) is pushed when it started - or
 * when the library is not running at all - and 0 for any other library
 * result (nothing loaded, could not be started). */
static int Opcode_Snd_SePlay(OpenBGI_t* o, Thread_t* t)
{
	int pan = (int)Thread_Pop(t), vol = (int)Thread_Pop(t);
	int slot = (int)Thread_Pop(t), r;
	CheckPan(o, (uint32_t)pan, t);
	CheckVolume(o, (uint32_t)vol, t);
	CheckSeSlot(o, slot, t);
	r = Se_Play(o, slot, vol, pan);
	if(r != 0 && r != WM_NOT_RUNNING)
		Thread_Push(t, 0);
	else
		Thread_Push(t, (uint32_t)Se_GetLength(o, slot));
	return 0;
}

static int Opcode_Snd_SeStop(OpenBGI_t* o, Thread_t* t) // A0 25: slot →
{
	int slot = (int)Thread_Pop(t);
	CheckSeSlot(o, slot, t);
	Se_Stop(o, slot);
	return 0;
}

static int Opcode_Snd_SeFadeOut(OpenBGI_t* o, Thread_t* t) // A0 26: slot, ms →
{
	int ms = (int)Thread_Pop(t);
	int slot = (int)Thread_Pop(t);
	CheckSeSlot(o, slot, t);
	Se_FadeOut(o, slot, ms);
	return 0;
}

// open the CD audio device (the drive the base or disc directory is on, when it is a CD-ROM)
static int Opcode_Snd_CdOpen(OpenBGI_t* o, Thread_t* t) // A0 80: →
{
	Cd_Open(o);
	return 0;
}

static int Opcode_Snd_CdClose(OpenBGI_t* o, Thread_t* t) // A0 81: →
{
	Sound_CdClose(o);
	return 0;
}

// play the track, replaying it when `loop` is set; r is 1 when it started, 0 when the device is closed or has no tracks
static int Opcode_Snd_CdPlay(OpenBGI_t* o, Thread_t* t) // A0 84: track, loop → r
{
	int loop = (int)Thread_Pop(t), track = (int)Thread_Pop(t);
	Thread_Push(t, (uint32_t)Cd_Play(o, track, loop));
	return 0;
}

static int Opcode_Snd_CdStop(OpenBGI_t* o, Thread_t* t) // A0 85: →
{
	Cd_Stop(o);
	return 0;
}

/* The device's mode into *out: 0 not ready, 1 stopped, 2 playing, 3
 * recording, 4 seeking, 5 paused, 6 open, -1 other; r is 1, or 0 when the
 * device is closed or does not answer (*out untouched). */
static int Opcode_Snd_CdStatus(OpenBGI_t* o, Thread_t* t) // A0 86: out → r
{
	Thread_Push(t, (uint32_t)Cd_Status(o, (int32_t*)PopPtr(o, t)));
	return 0;
}

// play a wave file of the base directory once, as the original's PlaySound (Sound_PlayWave); r is 1 when it plays
static int Opcode_Snd_PlayWave(OpenBGI_t* o, Thread_t* t) // A0 C0: name → r
{
	Thread_Push(t, (uint32_t)Sound_PlayWave(o, (const char*)PopPtr(o, t)));
	return 0;
}

/* Fill o->vm.optableA0 for the selected engine profile.  Every entry
 * names the opcode, its handler and the generations that have it; the
 * family only gained "A0 28" in 1.547, the rest cover GEN_FIRST ..
 * GEN_LAST (see Vm_FillTable). */
void Vm_OptableA0Init(OpenBGI_t* o)
{
	static const OpEntry_t ops[] = {
		{0x00, Opcode_Snd_Status, GEN_FIRST, GEN_LAST},
		{0x08, Opcode_Snd_BgmMasterVolume, GEN_FIRST, GEN_LAST},
		{0x09, Opcode_Snd_SeMasterVolume, GEN_FIRST, GEN_LAST},
		{0x10, Opcode_Snd_BgmPlayFile, GEN_FIRST, GEN_LAST},
		{0x11, Opcode_Snd_BgmPlay, GEN_FIRST, GEN_LAST},
		{0x12, Opcode_Snd_BgmPlayPair, GEN_FIRST, GEN_LAST},
		{0x14, Opcode_Snd_BgmPause, GEN_FIRST, GEN_LAST},
		{0x15, Opcode_Snd_BgmPosition, GEN_FIRST, GEN_LAST},
		{0x16, Opcode_Snd_BgmFadeVolume, GEN_FIRST, GEN_LAST},
		{0x17, Opcode_Snd_BgmPan, GEN_FIRST, GEN_LAST},
		{0x18, Opcode_Snd_BgmFadeIn, GEN_FIRST, GEN_LAST},
		{0x19, Opcode_Snd_BgmFadeOut, GEN_FIRST, GEN_LAST},
		{0x1C, Opcode_Snd_BgmChannelVolume, GEN_1_553, GEN_LAST},
		{0x20, Opcode_Snd_SeLoad, GEN_FIRST, GEN_LAST},
		{0x21, Opcode_Snd_SeLoadEx, GEN_FIRST, GEN_LAST},
		{0x22, Opcode_Snd_SeUnload, GEN_FIRST, GEN_LAST},
		{0x24, Opcode_Snd_SePlay, GEN_FIRST, GEN_LAST},
		{0x25, Opcode_Snd_SeStop, GEN_FIRST, GEN_LAST},
		{0x26, Opcode_Snd_SeFadeOut, GEN_FIRST, GEN_LAST},
		{0x27, Opcode_Snd_SeLoadRate, GEN_1_547, GEN_LAST},
		{0x28, Opcode_Snd_SeLoadMem, GEN_1_547, GEN_LAST},
		{0x2C, Opcode_Snd_SeChannelVolume, GEN_1_588, GEN_LAST},
		{0x2F, Opcode_Snd_SeLength, GEN_1_573, GEN_LAST},
		{0x80, Opcode_Snd_CdOpen, GEN_FIRST, GEN_LAST},
		{0x81, Opcode_Snd_CdClose, GEN_FIRST, GEN_LAST},
		{0x84, Opcode_Snd_CdPlay, GEN_FIRST, GEN_LAST},
		{0x85, Opcode_Snd_CdStop, GEN_FIRST, GEN_LAST},
		{0x86, Opcode_Snd_CdStatus, GEN_FIRST, GEN_LAST},
		{0xC0, Opcode_Snd_PlayWave, GEN_FIRST, GEN_LAST},
	};
	Vm_FillTable(o, o->vm.optableA0, OPFAM_A0, ops, BGI_COUNTOF(ops));
}
