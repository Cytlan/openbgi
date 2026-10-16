/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sound.c - the Sound view of the debugger (dbg_internal.h)
 *
 * The Wave Master's voices (Wm_DebugVoice): the music channels in the
 * left pane, the effect slots in the right one, each with its state,
 * sample rate, channel count, length in frames, position (effects),
 * DirectSound volume (hundredths of a dB), pan and loop count (music).
 * Voices without a sound loaded are dimmed.  The view takes no input.
 */
#include "../dbg_internal.h"
#include "bgi/snd/wavemaster.h"

// the two panes, one row per voice (effect rows beyond the pane are left out)
static void SoundDraw(int x, int y, int w, int h)
{
	int i, ly;
	int half = (w - 3 * DBG_PAD) / 2;
	WmVoiceInfo_t v;
	DbgPane_Frame(x + DBG_PAD, y + DBG_PAD, half, h - 2 * DBG_PAD, "ch  state    rate xc   frames volume  pan  lp   (music channels)");
	ly = y + DBG_PAD + DBG_CELL_H + 6;
	for(i = 0; i < WM_BGM_CHANNELS; i++)
	{
		Wm_DebugVoice(1, i, &v);
		DbgDraw_TextF(x + 2 * DBG_PAD, ly + i * DBG_CELL_H, v.loaded ? DBG_TEXT : DBG_DIM, "%2d  %-7s %5u x%u %8u %6d %4d %3d", i,
			!v.loaded ? "-" : v.paused ? "paused"
				: v.playing            ? "playing"
									   : "stopped",
			(unsigned)v.rate, (unsigned)v.channels,
			(unsigned)v.frames, (int)v.dsVolume, (int)v.pan, v.loops);
	}
	DbgPane_Frame(x + 2 * DBG_PAD + half, y + DBG_PAD, half, h - 2 * DBG_PAD, "no  state    rate xc   frames      pos volume  pan   (effect slots)");
	ly = y + DBG_PAD + DBG_CELL_H + 6;
	for(i = 0; i < WM_SE_SLOTS; i++)
	{
		Wm_DebugVoice(0, i, &v);
		if(ly + (i + 1) * DBG_CELL_H > y + h - DBG_PAD)
			break;
		DbgDraw_TextF(x + 3 * DBG_PAD + half, ly + i * DBG_CELL_H, v.loaded ? DBG_TEXT : DBG_DIM, "%2d  %-7s %5u x%u %8u %8u %6d %4d", i,
			!v.loaded ? "-" : v.paused ? "paused"
				: v.playing            ? "playing"
									   : "loaded",
			(unsigned)v.rate, (unsigned)v.channels,
			(unsigned)v.frames, (unsigned)v.pos, (int)v.dsVolume, (int)v.pan);
	}
}

static int SoundKey(int vk, int ch)
{
	return 0;
}

static int SoundClick(int x, int y, int button)
{
	return 0;
}

static int SoundWheel(int x, int y, int delta)
{
	return 0;
}

const DbgViewOps_t kDbgViewSound = {"Sound", SoundDraw, SoundKey, SoundClick, SoundWheel};
