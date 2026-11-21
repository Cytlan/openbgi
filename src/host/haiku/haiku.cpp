/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * haiku.cpp - the Haiku additions to the POSIX back end (src/host/os_haiku.h):
 *             the sound output through the Media Kit and the pointer
 *             warp of the Game Kit
 *
 * The only C++ of the project: these kits have no C interface.  Built for
 * PLATFORM=haiku alone (see the Makefile), next to the files of
 * src/host/posix/, which call these functions where Linux has ALSA or Xlib
 * calls that Xlibe does not implement.
 *
 * The sound: one BSoundPlayer asks for interleaved 16-bit stereo at the
 * engine's rate in 20 ms buffers and calls the host's callback, which
 * reads the engine's sound, from its own thread for every buffer.  Should the media server hand over another format,
 * float buffers are converted and anything else plays silence.
 */
#include "host/os_haiku.h"

#include <MediaDefs.h>
#include <SoundPlayer.h>
#include <View.h>
#include <WindowScreen.h>

#include <stdlib.h>
#include <string.h>

// ---- sound (Media Kit) -----------------------------------------------------------

static BSoundPlayer* gPlayer;    // NULL while closed
static OsHaikuAudioFill_t gFill; // the host's callback and its argument
static void* gUser;
static int16_t* gConv;     // 16-bit frames for a buffer of another format
static size_t gConvFrames; // frames gConv holds

// the player's callback: the host fills the buffer (or gConv, converted afterwards)
static void PlayBuffer(void* cookie, void* buffer, size_t size, const media_raw_audio_format& format)
{
	(void)cookie;
	memset(buffer, 0, size);
	if(format.channel_count != 2)
		return;
	if(format.format == media_raw_audio_format::B_AUDIO_SHORT)
		gFill(gUser, (int16_t*)buffer, (int)(size / (2 * sizeof(int16_t))));
	else if(format.format == media_raw_audio_format::B_AUDIO_FLOAT)
	{
		size_t frames = size / (2 * sizeof(float)), i;
		float* out = (float*)buffer;
		if(frames > gConvFrames)
			return; // larger than any buffer asked for: silence
		gFill(gUser, gConv, (int)frames);
		for(i = 0; i < frames * 2; i++)
			out[i] = gConv[i] / 32768.0f;
	}
}

int OsHaiku_AudioOpen(int rate, OsHaikuAudioFill_t fill, void* user)
{
	media_raw_audio_format fmt = media_raw_audio_format::wildcard;
	if(gPlayer || rate <= 0 || !fill)
		return 0;
	gFill = fill;
	gUser = user;
	gConvFrames = (size_t)rate / 5; // 200 ms, ten times the buffer asked for
	gConv = (int16_t*)malloc(gConvFrames * 2 * sizeof(int16_t));
	if(!gConv)
		return 0;
	fmt.frame_rate = (float)rate;
	fmt.channel_count = 2;
	fmt.format = media_raw_audio_format::B_AUDIO_SHORT;
	fmt.byte_order = B_MEDIA_HOST_ENDIAN;
	fmt.buffer_size = (size_t)(rate / 50) * 2 * sizeof(int16_t); // 20 ms
	gPlayer = new BSoundPlayer(&fmt, "OpenBGI", PlayBuffer, NULL, NULL);
	if(gPlayer->InitCheck() != B_OK || gPlayer->Start() != B_OK)
	{
		delete gPlayer;
		gPlayer = NULL;
		free(gConv);
		gConv = NULL;
		return 0;
	}
	gPlayer->SetHasData(true);
	return 1;
}

void OsHaiku_AudioClose(void)
{
	if(!gPlayer)
		return;
	gPlayer->SetHasData(false);
	gPlayer->Stop(true, false); // blocks until the callback is out
	delete gPlayer;
	gPlayer = NULL;
	free(gConv);
	gConv = NULL;
	gConvFrames = 0;
}

// ---- the pointer -----------------------------------------------------------------

void OsHaiku_SetMousePosition(int x, int y)
{
	set_mouse_position((int32)x, (int32)y);
}
