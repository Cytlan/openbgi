/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_media.c - the OS layer's CD audio and movie player as the host's
 *              tables (OpenBGI_CdAudio_t and OpenBGI_MoviePlayer_t of
 *              openbgi.h, for OS_Host; declared in host/os_common.h)
 *
 * The Windows back end has both, through MCI and DirectShow (OS_Mci*,
 * OS_Movie*); the tables hand them to the engine as they are.  The other
 * back ends have neither: no CD audio, and the movies go to the decoder
 * (os_ffmpeg.c) - as they do on Windows unless the player is asked for
 * (--movies=player, src/gfx/movie.c).
 */
#include "host/os.h"
#include "host/os_common.h"

#if defined(BGI_WIN32)

// ---- CD audio ---------------------------------------------------------------------------------

static uint32_t CdOpen(void* user, const char* element)
{
	BGI_UNUSED(user);
	return OS_MciOpenCdAudio(element);
}

static int CdSetTimeFormatTmsf(void* user, uint32_t device)
{
	BGI_UNUSED(user);
	return OS_MciSetTimeFormatTmsf(device);
}

static int CdStatus(void* user, uint32_t device, uint32_t item, uint32_t* out)
{
	BGI_UNUSED(user);
	return OS_MciStatus(device, item, out);
}

static int CdPlay(void* user, uint32_t device, uint32_t from, uint32_t to, int notify)
{
	BGI_UNUSED(user);
	return OS_MciPlay(device, from, to, notify);
}

static int CdStop(void* user, uint32_t device)
{
	BGI_UNUSED(user);
	return OS_MciStop(device);
}

static int CdClose(void* user, uint32_t device)
{
	BGI_UNUSED(user);
	return OS_MciClose(device);
}

static const OpenBGI_CdAudio_t kCdAudio = {sizeof(OpenBGI_CdAudio_t), CdOpen, CdSetTimeFormatTmsf, CdStatus, CdPlay, CdStop, CdClose};

const OpenBGI_CdAudio_t* OsCommon_CdAudio(void)
{
	return &kCdAudio;
}

// ---- the movie player -------------------------------------------------------------------------

static int PlayerOpen(void* user, const char* path)
{
	BGI_UNUSED(user);
	return OS_MovieOpen(path);
}

static int PlayerStart(void* user, int x, int y, int w, int h, int32_t volume, int32_t* lengthMs)
{
	BGI_UNUSED(user);
	return OS_MovieStart(x, y, w, h, volume, lengthMs);
}

static void PlayerStop(void* user)
{
	BGI_UNUSED(user);
	OS_MovieStop();
}

static int PlayerRunning(void* user)
{
	BGI_UNUSED(user);
	return OS_MovieIsRunning();
}

static void PlayerSetVolume(void* user, int32_t volume)
{
	BGI_UNUSED(user);
	OS_MovieSetVolume(volume);
}

static const OpenBGI_MoviePlayer_t kPlayer = {sizeof(OpenBGI_MoviePlayer_t), PlayerOpen, PlayerStart, PlayerStop, PlayerRunning, PlayerSetVolume};

const OpenBGI_MoviePlayer_t* OsCommon_MoviePlayer(void)
{
	return &kPlayer;
}

#else

const OpenBGI_CdAudio_t* OsCommon_CdAudio(void)
{
	return NULL;
}

const OpenBGI_MoviePlayer_t* OsCommon_MoviePlayer(void)
{
	return NULL;
}

#endif // BGI_WIN32
