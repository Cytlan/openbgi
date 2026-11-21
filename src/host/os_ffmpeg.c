/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_ffmpeg.c - the OS layer's movie decoder for the host's table
 *               (OpenBGI_MovieDecoder_t of openbgi.h, for OS_Host;
 *               declared in host/os_common.h): the movie library
 *
 * The decoding is the movie library's (openbgi_ffmovie.h: ffmpeg's libraries,
 * linked into the binary), and the file is the engine's, which reads it
 * for the library wherever the game has it: the calls are passed on as
 * they are.  A build without ffmpeg's libraries
 * (MOVIES=none: macOS and the page, for now) has no decoder, and the
 * engine skips the movies.
 */
#include "host/os.h"
#include "host/os_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(BGI_HAVE_FFMPEG)

const OpenBGI_MovieDecoder_t* OsCommon_MovieDecoder(void)
{
	return NULL;
}

#else

#include "openbgi_ffmovie.h"

static void* Open(void* user, const OpenBGI_MovieFile_t* file, const char* path, int w, int h, int audio, OpenBGI_MovieInfo_t* info)
{
	void* movie = OpenBGI_FfMovieOpen(file, w, h, audio, info);
	BGI_UNUSED(user);
	if(!movie)
	{
		char native[0x400];
		OS_PathNative(path, native, sizeof native);
		fprintf(stderr, "openbgi: \"%s\" is no movie this build decodes (%s): it is skipped\n", native, OpenBGI_FfMovieVersion());
	}
	return movie;
}

static int32_t Frames(void* user, void* movie, int* ended)
{
	BGI_UNUSED(user);
	return OpenBGI_FfMovieFrames(movie, ended);
}

static int Take(void* user, void* movie, int32_t index, uint8_t* out)
{
	BGI_UNUSED(user);
	return OpenBGI_FfMovieTake(movie, index, out);
}

static int Samples(void* user, void* movie, int16_t* out, int frames, int* ended)
{
	BGI_UNUSED(user);
	return OpenBGI_FfMovieSamples(movie, out, frames, ended);
}

static void Close(void* user, void* movie)
{
	BGI_UNUSED(user);
	OpenBGI_FfMovieClose(movie);
}

static const OpenBGI_MovieDecoder_t kDecoder = {sizeof(OpenBGI_MovieDecoder_t), Open, Frames, Take, Samples, Close};

const OpenBGI_MovieDecoder_t* OsCommon_MovieDecoder(void)
{
	return &kDecoder;
}

#endif // BGI_HAVE_FFMPEG
