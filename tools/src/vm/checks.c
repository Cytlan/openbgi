/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * checks.c - operand validators of the graphics and sound instruction
 *            families (declared in inc/bgi/vm.h, used by the ops_gfx*.c,
 *            ops_snd.c and ops_ext*.c handlers)
 *
 * Each one raises a script error (which never returns) when the operand is
 * out of range and is otherwise a no-op.  They are called with the operand
 * and the thread, so the message gets the thread's context (thread number,
 * program, instruction) prepended; the ranges are those of the original
 * and are repeated on each function.
 */
#include "bgi/vm.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/gfx/bmpops.h"

// format the message `fmt` with the offending value `v` and raise it on thread `t`
#define CHECK_ERROR(t, fmt, v)     \
	do                             \
	{                              \
		char msg_[0x100];          \
		sprintf(msg_, (fmt), (v)); \
		ScriptError(msg_, (t));    \
	} while(0)

void CheckSeSlot(uint32_t slot, Thread_t* t) // 0.. 63: a sound-effect slot
{
	if(slot >= 0x40)
		CHECK_ERROR(t, MSG_BAD_SE_SLOT, (int)slot);
}

void CheckPan(int32_t pan, Thread_t* t) // 0.. 128: 64 is the centre
{
	if(pan < 0 || pan > 0x80)
		CHECK_ERROR(t, MSG_BAD_PAN, pan);
}

void CheckVolume(int32_t volume, Thread_t* t) // 0.. 128
{
	if(volume < 0 || volume > 0x80)
		CHECK_ERROR(t, MSG_BAD_VOLUME, volume);
}

void CheckBgmChannel(uint32_t channel, Thread_t* t) // 0.. 15
{
	if(channel >= 0x10)
		CHECK_ERROR(t, MSG_BAD_BGM_CHANNEL, (int)channel);
}

void CheckDivCount(int32_t n, Thread_t* t) // 1.. 256: the step count of a text scroll or fade ("90 95", "90 96")
{
	if(n < 1 || n > 0x100)
		CHECK_ERROR(t, MSG_BAD_DIV_COUNT, n);
}

void CheckFontNo(int32_t no, Thread_t* t) // a font number FontNameByNo knows (0, 1 or a registered face)
{
	if(!FontNameByNo(no))
		CHECK_ERROR(t, MSG_BAD_FONT_NO, no);
}

// 0.. 255; raises the error without thread context (the message carries no value either)
void CheckVirtualKey(int32_t vk)
{
	if(vk > 0xff || vk < 0)
		ThrowScriptError(MSG_BAD_VKEY);
}

void CheckBitmapNo(int32_t no, Thread_t* t) // 0.. 4095; 0.. 16383 from 1.69 build 472 on (BMP_NO_LIMIT)
{
	if(no < 0 || no >= BMP_NO_LIMIT)
		CHECK_ERROR(t, MSG_BAD_BITMAP_NO, no);
}

void CheckPriority(uint32_t prio, Thread_t* t) // 0.. 4095: a display priority
{
	if(prio >= 0x1000)
		CHECK_ERROR(t, MSG_BAD_PRIORITY, (int)prio);
}

void ErrBitmapNotRegistered(int32_t no, Thread_t* t) // always raises: bitmap `no` has no image
{
	CHECK_ERROR(t, MSG_BITMAP_NOT_REGISTERED, no);
}

/* the effect modes the blitters know: 0..9, 0x20..0x27, 0x40,
 * 0x41, 0x80, 0xC0, 0xC1, 0xF0 and 0xFF; anything else is an error */
void CheckEffectMode(uint32_t mode, Thread_t* t)
{
	int ok;
	if(mode <= 9 || (mode >= 0x20 && mode <= 0x27) || mode == 0x40 || mode == 0x41)
		ok = 1;
	else
		ok = mode == 0x80 || mode == 0xc0 || mode == 0xc1 || mode == 0xf0 || mode == 0xff;
	if(!ok)
		CHECK_ERROR(t, MSG_BAD_EFFECT_MODE, (int)mode);
}

void CheckAlpha(uint32_t level, Thread_t* t) // 0.. 256: an effect level, the `level` of Bmp_Blit
{
	if(level > 0x100)
		CHECK_ERROR(t, MSG_BAD_EFFECT_LEVEL, (int)level);
}

void CheckMixRatio(uint32_t ratio, Thread_t* t) // 0.. 256
{
	if(ratio > 0x100)
		CHECK_ERROR(t, MSG_BAD_MIX_RATIO, (int)ratio);
}

void CheckTransparency(uint32_t level, Thread_t* t) // 0.. 256
{
	if(level > 0x100)
		CHECK_ERROR(t, MSG_BAD_TRANSPARENCY, (int)level);
}
