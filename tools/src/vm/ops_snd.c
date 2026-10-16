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
 * effect loads (A0 20, A0 21) install a wait object and return scheduler
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

VmHandler_t vm_optable_A0[256]; // the "A0 xx" dispatch table, filled by Vm_OptableA0Init

// raise a script error with a formatted message; never returns
#define SND_ERROR(t, ...)           \
	do                              \
	{                               \
		char msg_[0x104];           \
		sprintf(msg_, __VA_ARGS__); \
		ScriptError(msg_, (t));     \
	} while(0)

static int Opcode_Snd_Status(Thread_t* t) // A0 00: → 0x14 (the library's "not running" code, always)
{
	Thread_Push(t, 0x14);
	return 0;
}

// the channel's master volume (0 .. 0x80), applied on top of what the scripts set per track
static int Opcode_Snd_BgmMasterVolume(Thread_t* t) // A0 08: ch, vol →
{
	uint32_t vol = Thread_Pop(t);
	int ch = (int)Thread_Pop(t);
	CheckVolume(vol, t);
	CheckBgmChannel(ch, t);
	Bgm_SetMasterVolume(ch, vol);
	return 0;
}

// the slot's master volume (0 .. 0x80)
static int Opcode_Snd_SeMasterVolume(Thread_t* t) // A0 09: slot, vol →
{
	uint32_t vol = Thread_Pop(t);
	int slot = (int)Thread_Pop(t);
	CheckVolume(vol, t);
	CheckSeSlot(slot, t);
	Se_SetMasterVolume(slot, vol);
	return 0;
}

// stream a loose BW file from the base (then the disc) directory on the channel; a missing or non-BW file is a script error
static int Opcode_Snd_BgmPlayFile(Thread_t* t) // A0 10: ch, name, vol →
{
	int vol = (int)Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	int ch = (int)Thread_Pop(t);
	CheckVolume((uint32_t)vol, t);
	CheckBgmChannel(ch, t);
	switch(Bgm_PlayFile(ch, name, vol))
	{
		case WM_FILE_NOT_FOUND: SND_ERROR(t, MSG_BW_NOT_FOUND, name); break;
		case WM_NOT_BW: SND_ERROR(t, MSG_NOT_BW, name); break;
		default: break;
	}
	return 0;
}

/* Stream an archive entry (a null arc: a loose file through the search
 * sub-directories) on the channel at `vol` and `pan` (0x40 centred).  The
 * manager asks the user to retry while the file is missing; giving up, or
 * a file that is not a BW file, is a script error. */
static int Opcode_Snd_BgmPlay(Thread_t* t) // A0 11: ch, arc, name, vol, pan →
{
	int pan = (int)Thread_Pop(t), vol = (int)Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	int ch = (int)Thread_Pop(t);
	CheckPan((uint32_t)pan, t);
	CheckVolume((uint32_t)vol, t);
	CheckBgmChannel(ch, t);
	switch(Bgm_PlayArc(ch, arc, name, vol, pan))
	{
		case WM_FILE_NOT_FOUND: SND_ERROR(t, MSG_BW_ARC_NOT_FOUND, arc, name); break;
		case WM_NOT_BW: SND_ERROR(t, MSG_SE_NOT_BW, arc, name); break;
		default: break;
	}
	return 0;
}

/* As "A0 11" with an intro / loop pair: `intro` plays once, then `loop`
 * repeats while `flag` is non-zero (the same name twice makes a single
 * looping file). */
static int Opcode_Snd_BgmPlayPair(Thread_t* t) // A0 12: ch, arc, intro, loop, flag, vol, pan →
{
	int pan = (int)Thread_Pop(t), vol = (int)Thread_Pop(t), flag = (int)Thread_Pop(t);
	const char* loop = (const char*)PopPtr(t);
	const char* intro = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	int ch = (int)Thread_Pop(t);
	CheckPan((uint32_t)pan, t);
	CheckVolume((uint32_t)vol, t);
	CheckBgmChannel(ch, t);
	switch(Bgm_PlayArcPair(ch, arc, intro, loop, flag, vol, pan))
	{
		case WM_FILE_NOT_FOUND: SND_ERROR(t, MSG_BW_PAIR_NOT_FOUND, arc, intro, loop); break;
		case WM_NOT_BW: SND_ERROR(t, MSG_NOT_BW_PAIR, arc, intro, loop); break;
		default: break;
	}
	return 0;
}

static int Opcode_Snd_BgmPause(Thread_t* t) // A0 14: ch, run → (run 0 pauses, else resumes)
{
	int run = (int)Thread_Pop(t);
	int ch = (int)Thread_Pop(t);
	CheckBgmChannel(ch, t);
	Bgm_Pause(ch, run == 0);
	return 0;
}

/* The names are historical: what is pushed is 1 while the channel plays
 * and 0 otherwise, and *outLength receives the stream's loop count (only
 * while it plays). */
static int Opcode_Snd_BgmPosition(Thread_t* t) // A0 15: ch, outLength → position
{
	uint32_t* outLength = (uint32_t*)PopPtr(t);
	int ch = (int)Thread_Pop(t);
	CheckBgmChannel(ch, t);
	Thread_Push(t, Bgm_GetPosition(ch, outLength));
	return 0;
}

// fade the channel's volume towards vol (0 .. 0x80) over ms milliseconds
static int Opcode_Snd_BgmFadeVolume(Thread_t* t) // A0 16: ch, vol, ms →
{
	int ms = (int)Thread_Pop(t), vol = (int)Thread_Pop(t);
	int ch = (int)Thread_Pop(t);
	CheckVolume((uint32_t)vol, t);
	CheckBgmChannel(ch, t);
	Bgm_FadeVolume(ch, vol, ms);
	return 0;
}

// the channel's pan, 0 .. 0x80 with 0x40 centred
static int Opcode_Snd_BgmPan(Thread_t* t) // A0 17: ch, pan →
{
	int pan = (int)Thread_Pop(t);
	int ch = (int)Thread_Pop(t);
	CheckPan((uint32_t)pan, t);
	CheckBgmChannel(ch, t);
	Bgm_SetPan(ch, pan);
	return 0;
}

// fade the channel's fade level up to full over ms milliseconds
static int Opcode_Snd_BgmFadeIn(Thread_t* t) // A0 18: ch, ms →
{
	int ms = (int)Thread_Pop(t);
	int ch = (int)Thread_Pop(t);
	CheckBgmChannel(ch, t);
	Bgm_FadeIn(ch, ms);
	return 0;
}

// fade the channel's fade level down to silence over ms milliseconds
static int Opcode_Snd_BgmFadeOut(Thread_t* t) // A0 19: ch, ms →
{
	int ms = (int)Thread_Pop(t);
	int ch = (int)Thread_Pop(t);
	CheckBgmChannel(ch, t);
	Bgm_FadeOut(ch, ms);
	return 0;
}

/* Load the BW file `name` of archive `arc` into effect slot `slot` through
 * the background loader; the thread waits for the load (no fade-in, gain
 * 1.0). */
static int Opcode_Snd_SeLoad(Thread_t* t) // A0 20: slot, arc, name →
{
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	int slot = (int)Thread_Pop(t);
	CheckSeSlot(slot, t);
	Thread_SetWait(t, WaitSeLoad_New(t, slot, arc, name, 0, 1.0));
	return 2;
}

/* A0 21: slot, arc, name, fadeInMs, gain16 →
 * "A0 20" with a fade-in, which the library burns into the decoded
 * samples, and a 16.16 gain that multiplies them (both 0 / 1.0 for
 * "A0 20"). */
static int Opcode_Snd_SeLoadEx(Thread_t* t)
{
	int32_t gain16 = (int32_t)Thread_Pop(t);
	int fadeInMs = (int)Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	int slot = (int)Thread_Pop(t);
	CheckSeSlot(slot, t);
	Thread_SetWait(t, WaitSeLoad_New(t, slot, arc, name, fadeInMs, gain16 / 65536.0));
	return 2;
}

/* A0 28 (1.547 on): slot, data, mode, gain16, rate16 →
 * "A0 21" for a BW file the script holds in memory (bsctrl2._bp reads a
 * voice file into a heap block and plays it as an effect): the data is
 * decoded whole before the wait ends, so the script may free it after.
 * `mode` (1 in every script) and the 16.16 playback rate (1.0 there; the
 * original's "A0 21" of these builds passes 1.0 for it as well) are
 * accepted and left alone - the effect plays at the file's rate. */
static int Opcode_Snd_SeLoadMem(Thread_t* t)
{
	int32_t gain16;
	const void* data;
	int slot;
	Thread_Pop(t); // rate16
	gain16 = (int32_t)Thread_Pop(t);
	Thread_Pop(t); // mode
	data = PopPtr(t);
	slot = (int)Thread_Pop(t);
	CheckSeSlot(slot, t);
	Thread_SetWait(t, WaitSeMem_New(t, slot, data, gain16 / 65536.0));
	return 2;
}

static int Opcode_Snd_SeUnload(Thread_t* t) // A0 22: slot →
{
	int slot = (int)Thread_Pop(t);
	CheckSeSlot(slot, t);
	Se_Unload(slot);
	return 0;
}

/* A0 24: slot, vol, pan → length
 * Start the effect in the slot at `vol` and `pan`.  The effect's length
 * in milliseconds (from its descriptor) is pushed when it started - or
 * when the library is not running at all - and 0 for any other library
 * result (nothing loaded, could not be started). */
static int Opcode_Snd_SePlay(Thread_t* t)
{
	int pan = (int)Thread_Pop(t), vol = (int)Thread_Pop(t);
	int slot = (int)Thread_Pop(t), r;
	CheckPan((uint32_t)pan, t);
	CheckVolume((uint32_t)vol, t);
	CheckSeSlot(slot, t);
	r = Se_Play(slot, vol, pan);
	if(r != 0 && r != WM_NOT_RUNNING)
		Thread_Push(t, 0);
	else
		Thread_Push(t, (uint32_t)Se_GetLength(slot));
	return 0;
}

static int Opcode_Snd_SeStop(Thread_t* t) // A0 25: slot →
{
	int slot = (int)Thread_Pop(t);
	CheckSeSlot(slot, t);
	Se_Stop(slot);
	return 0;
}

static int Opcode_Snd_SeFadeOut(Thread_t* t) // A0 26: slot, ms →
{
	int ms = (int)Thread_Pop(t);
	int slot = (int)Thread_Pop(t);
	CheckSeSlot(slot, t);
	Se_FadeOut(slot, ms);
	return 0;
}

// open the CD audio device (the drive the base or disc directory is on, when it is a CD-ROM)
static int Opcode_Snd_CdOpen(Thread_t* t) // A0 80: →
{
	Cd_Open();
	return 0;
}

static int Opcode_Snd_CdClose(Thread_t* t) // A0 81: →
{
	Sound_CdClose();
	return 0;
}

// play the track, replaying it when `loop` is set; r is 1 when it started, 0 when the device is closed or has no tracks
static int Opcode_Snd_CdPlay(Thread_t* t) // A0 84: track, loop → r
{
	int loop = (int)Thread_Pop(t), track = (int)Thread_Pop(t);
	Thread_Push(t, (uint32_t)Cd_Play(track, loop));
	return 0;
}

static int Opcode_Snd_CdStop(Thread_t* t) // A0 85: →
{
	Cd_Stop();
	return 0;
}

/* The device's mode into *out: 0 not ready, 1 stopped, 2 playing, 3
 * recording, 4 seeking, 5 paused, 6 open, -1 other; r is 1, or 0 when the
 * device is closed or does not answer (*out untouched). */
static int Opcode_Snd_CdStatus(Thread_t* t) // A0 86: out → r
{
	Thread_Push(t, (uint32_t)Cd_Status((int32_t*)PopPtr(t)));
	return 0;
}

// play a wave file of the base directory asynchronously through PlaySound; r is 1 when accepted
static int Opcode_Snd_PlayWave(Thread_t* t) // A0 C0: name → r
{
	Thread_Push(t, (uint32_t)Sound_PlayWave((const char*)PopPtr(t)));
	return 0;
}

/* Fill vm_optable_A0 for the selected engine profile.  Every entry names
 * the opcode, its handler and the generations that have it; the family
 * only gained "A0 28" in 1.547, the rest cover GEN_FIRST .. GEN_LAST (see
 * Vm_FillTable). */
void Vm_OptableA0Init(void)
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
		{0x20, Opcode_Snd_SeLoad, GEN_FIRST, GEN_LAST},
		{0x21, Opcode_Snd_SeLoadEx, GEN_FIRST, GEN_LAST},
		{0x22, Opcode_Snd_SeUnload, GEN_FIRST, GEN_LAST},
		{0x24, Opcode_Snd_SePlay, GEN_FIRST, GEN_LAST},
		{0x25, Opcode_Snd_SeStop, GEN_FIRST, GEN_LAST},
		{0x26, Opcode_Snd_SeFadeOut, GEN_FIRST, GEN_LAST},
		{0x28, Opcode_Snd_SeLoadMem, GEN_1_547, GEN_LAST},
		{0x80, Opcode_Snd_CdOpen, GEN_FIRST, GEN_LAST},
		{0x81, Opcode_Snd_CdClose, GEN_FIRST, GEN_LAST},
		{0x84, Opcode_Snd_CdPlay, GEN_FIRST, GEN_LAST},
		{0x85, Opcode_Snd_CdStop, GEN_FIRST, GEN_LAST},
		{0x86, Opcode_Snd_CdStatus, GEN_FIRST, GEN_LAST},
		{0xC0, Opcode_Snd_PlayWave, GEN_FIRST, GEN_LAST},
	};
	Vm_FillTable(vm_optable_A0, OPFAM_A0, ops, BGI_COUNTOF(ops));
}
