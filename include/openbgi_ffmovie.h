/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * openbgi_ffmovie.h - the movie decoder beside the core: ffmpeg's libraries
 *             behind the calls a host's OpenBGI_MovieDecoder_t is made
 *             of (openbgi.h, "The movies"; src/movie/ffmovie.c); not
 *             part of the original engine, nor of the core
 *
 * The core decodes no movies: it shows the frames and mixes the sound a
 * host's decoder hands it.  This is that decoder for the hosts to share,
 * a library of its own (libopenbgi-movie) that needs ffmpeg's libavformat,
 * libavcodec, libswscale, libswresample and libavutil and nothing of a
 * system: it reads the file through the two calls of an
 * OpenBGI_MovieFile_t (openbgi.h), runs on the thread it is called on
 * and keeps no state outside the movie it was asked to open.  The engine
 * hands a host's decoder the movie's file, which it reads itself
 * wherever the game has it; a host passes it and the five calls on:
 *
 *     static void* Open(void* user, const OpenBGI_MovieFile_t* file, const char* path, int w, int h, int audio,
 *                       OpenBGI_MovieInfo_t* info)
 *     {
 *         return OpenBGI_FfMovieOpen(file, w, h, audio, info);
 *     }
 *
 * (a program that decodes a file of its own fills such a struct with
 * its read and seek, and closes its file after OpenBGI_FfMovieClose).
 * What the calls do is
 * what openbgi.h says of the decoder's: frames of w x h pixels, 32-bit
 * B G R A, at the file's frame rate, and the sound as 16-bit stereo at
 * OPENBGI_AUDIO_RATE.  Which files it plays is the build's choice of
 * ffmpeg's decoders (docs/dev/building.md, "Movies").
 */
#ifndef OPENBGI_FFMOVIE_H_
#define OPENBGI_FFMOVIE_H_

#include "openbgi.h"

/* The five calls of OpenBGI_MovieDecoder_t (openbgi.h), without the
 * host's `user` and with the file in place of the path.  Open is NULL
 * when the file is no movie this build of ffmpeg decodes. */
OPENBGI_API void* OpenBGI_FfMovieOpen(const OpenBGI_MovieFile_t* file, int w, int h, int audio, OpenBGI_MovieInfo_t* info);
OPENBGI_API int32_t OpenBGI_FfMovieFrames(void* movie, int* ended);
OPENBGI_API int OpenBGI_FfMovieTake(void* movie, int32_t index, uint8_t* out);
OPENBGI_API int OpenBGI_FfMovieSamples(void* movie, int16_t* out, int frames, int* ended);
OPENBGI_API void OpenBGI_FfMovieClose(void* movie);

/* What this build decodes, for a host to say: the libavcodec it was
 * compiled with ("Lavc60.31.102"), and whether the ffmpeg it runs with
 * has a decoder ("h264", "wmav2") or reads a file format ("asf", "avi",
 * "mpeg", "mov") of that name. */
OPENBGI_API const char* OpenBGI_FfMovieVersion(void);
OPENBGI_API int OpenBGI_FfMovieHas(const char* name);

#endif // OPENBGI_FFMOVIE_H_
