/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * moviefx.c - the movies the host decodes and the engine shows
 *             (src/include/bgi/gfx.h: MovieFx_*)
 *
 * The original plays its opening movies (MPEG files beside the archives)
 * through DirectShow.  Where the host has no player of its own for a file
 * but a decoder (OpenBGI_MovieDecoder_t, openbgi.h: the movie library
 * of openbgi_ffmovie.h in the hosts here), the decoder reads the file through
 * the engine's file layer - in place, also in a zip archive or a disc
 * image - and hands over the frames
 * - 32-bit, at the size of the rectangle the script asked for, numbered
 * at a rate the decoder names -
 * and the sound, 16-bit stereo at the mixer's rate.  The frames are
 * presented from the engine's pass (MovieFx_Tick, called every pass from
 * the scheduler through Movie_Tick) on a clock that starts with the first
 * frame, dropping late ones, and the sound is mixed into the sound
 * library's output (Wm_SetExternalMix) with the movie volume as the gain,
 * held back until the first frame shows.  The movie is over when both
 * streams have ended and everything decoded was shown or heard;
 * Movie_OnEvent then does what the DirectShow EC_COMPLETE did.
 */
#include "bgi/gfx.h"
#include "bgi/display.h"
#include "bgi/snd/wavemaster.h"
#include "bgi/host.h"
#include "bgi/fs.h"
#include "bgi/core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FX_MIX_CHUNK 1024 // the stereo frames taken from the decoder at a time while mixing

typedef struct MovieFx
{
	void* m;            // the host's decoder of the file
	FsFile_t* file;     // the file it reads: the engine's, wherever the game has it
	int x, y, w, h;     // the rectangle on the picture
	double fps;         // frames per second
	int32_t lengthMs;   // 0 unknown
	uint8_t* frame;     // the frame being shown: w * h * 4 bytes
	int32_t taken;      // the frames taken from the decoder (shown or dropped)
	int started;        // the clock runs
	uint32_t startTick; // when the first frame was shown
	int32_t shown;      // index of the frame shown last, -1 for none
	int32_t gain16;     // the volume as a 16.16 gain
	int hasAudio;       // the sound track is mixed
	OpenBGI_t* o;       // the engine, for the mixer's tap
} MovieFx_t;

// the external mix: the decoder's samples, scaled by the gain, into the accumulators (silence when there are none yet)
static void MixTap(void* user, int32_t* acc, int frames)
{
	MovieFx_t* fx = (MovieFx_t*)user;
	int16_t buf[FX_MIX_CHUNK * 2];
	int ended;
	if(!fx->started)
		return; // not started yet: the sound stays aligned with the picture
	while(frames > 0)
	{
		int want = frames < FX_MIX_CHUNK ? frames : FX_MIX_CHUNK, n, i;
		if(fx->gain16 == 0)
		{ // silent: consumed without mixing
			n = Host_DecoderSamples(fx->o, fx->m, NULL, want, &ended);
			if(n <= 0)
				return;
			frames -= n;
			continue;
		}
		n = Host_DecoderSamples(fx->o, fx->m, buf, want, &ended);
		if(n <= 0)
			return;
		for(i = 0; i < n; i++)
		{
			acc[i * 2] += (int32_t)(((int64_t)buf[i * 2] * fx->gain16) >> 16);
			acc[i * 2 + 1] += (int32_t)(((int64_t)buf[i * 2 + 1] * fx->gain16) >> 16);
		}
		acc += n * 2;
		frames -= n;
	}
}

static int32_t GainOf(int32_t volume) // IBasicAudio hundredths of a decibel (-10000 .. 0) as a 16.16 factor
{
	if(volume <= -10000)
		return 0;
	if(volume >= 0)
		return 0x10000;
	return (int32_t)(pow(10.0, volume / 2000.0) * 65536.0);
}

static void Release(OpenBGI_t* o, MovieFx_t* fx)
{
	if(fx->hasAudio)
		Wm_SetExternalMix(o, NULL, NULL);
	Host_DecoderClose(o, fx->m);
	Fs_FileClose(fx->file); // (after the decoder, which may read it to the last)
	free(fx->frame);
	free(fx);
}

// the movie's file for the decoder: read through the file layer, so that one in an archive or a disc image is read where it is
static int FileRead(void* ctx, uint8_t* buf, int n)
{
	return n > 0 ? (int)Fs_FileRead((FsFile_t*)ctx, buf, (uint32_t)n) : 0;
}

static int FileSeek(void* ctx, int64_t pos)
{
	return pos >= 0 && pos <= 0xffffffff && Fs_FileSeek((FsFile_t*)ctx, (uint32_t)pos);
}

/* Start `path` (an engine path) in the picture rectangle (x, y, w, h)
 * at `volume`; *lengthMs receives the length the decoder reports (0
 * unknown).  1 when the host decodes the file, 0 when it cannot (no
 * decoder, or the file unreadable).  An earlier movie is stopped first. */
int MovieFx_Start(OpenBGI_t* o, const char* path, int x, int y, int w, int h, int32_t volume, int32_t* lengthMs)
{
	MovieFx_t* fx;
	OpenBGI_MovieInfo_t info;
	OpenBGI_MovieFile_t file;
	FsFile_t* f;
	void* m;
	MovieFx_Stop(o);
	if(w <= 0 || h <= 0 || (size_t)w * (size_t)h > 0x1000000u || !Host_HasDecoder(o) || !(f = Fs_FileOpenRead(o, path)))
		return 0;
	memset(&file, 0, sizeof file);
	file.size = sizeof file;
	file.ctx = f;
	file.read = FileRead;
	file.seek = FileSeek;
	file.length = Fs_FileSize(f);
	m = Host_DecoderOpen(o, &file, path, w, h, Wm_MixRunning(o), &info);
	if(!m)
	{
		Fs_FileClose(f);
		return 0;
	}
	fx = (MovieFx_t*)calloc(1, sizeof *fx);
	if(fx)
		fx->frame = (uint8_t*)malloc((size_t)w * (size_t)h * 4);
	if(!fx || !fx->frame)
	{
		Host_DecoderClose(o, m);
		Fs_FileClose(f);
		free(fx);
		return 0;
	}
	fx->m = m;
	fx->file = f;
	fx->o = o;
	fx->x = x;
	fx->y = y;
	fx->w = w;
	fx->h = h;
	fx->fps = info.fps >= 1.0 && info.fps <= 240.0 ? info.fps : 30.0;
	fx->lengthMs = info.lengthMs > 0 ? info.lengthMs : 0;
	fx->shown = -1;
	fx->gain16 = GainOf(volume);
	fx->hasAudio = info.audio && Wm_MixRunning(o);
	if(fx->hasAudio)
		Wm_SetExternalMix(o, MixTap, fx);
	o->movie.fx = fx;
	*lengthMs = fx->lengthMs;
	return 1;
}

void MovieFx_Stop(OpenBGI_t* o)
{
	MovieFx_t* fx = o->movie.fx;
	if(!fx)
		return;
	o->movie.fx = NULL;
	Release(o, fx);
}

// a movie is loaded and not yet over
int MovieFx_IsRunning(OpenBGI_t* o)
{
	return o->movie.fx != NULL;
}

void MovieFx_SetVolume(OpenBGI_t* o, int32_t volume)
{
	if(o->movie.fx)
		o->movie.fx->gain16 = GainOf(volume);
}

// blit the frame taken last into the window at the movie's rectangle (through the display scaling)
static void ShowFrame(OpenBGI_t* o, MovieFx_t* fx)
{
	Bmp_t b;
	if(!o->display.windowAlive)
		return;
	b.pixels = fx->frame;
	b.pitch = fx->w * 4;
	b.w = fx->w;
	b.h = fx->h;
	b.mode = PM_RGB32;
	b.bpp = 4;
	Window_Blit(o, fx->x, fx->y, &b);
}

/* Every pass: show the frame that is due (the newest one not later than
 * the clock, so a slow machine drops frames rather than falling behind),
 * and end the movie once both streams are exhausted.  1 while the movie
 * runs, 0 when it just ended (the caller then does what the end
 * notification did) or none runs. */
int MovieFx_Tick(OpenBGI_t* o)
{
	MovieFx_t* fx = o->movie.fx;
	int32_t decoded, due, idx;
	int vEnd;
	uint32_t now;
	if(!fx)
		return 0;
	decoded = Host_DecoderFrames(o, fx->m, &vEnd);
	if(!fx->started)
	{
		if(decoded <= fx->taken && !vEnd)
			return 1; // the first frame is still being decoded
		fx->started = 1;
		fx->startTick = Clock_HostMs(o);
	}
	now = Clock_HostMs(o);
	due = (int32_t)((double)(now - fx->startTick) * fx->fps / 1000.0);
	// the newest decoded frame whose index is at most `due`
	idx = decoded - 1;
	if(idx > due)
		idx = due;
	if(idx > fx->shown && idx >= fx->taken && Host_DecoderTake(o, fx->m, idx, fx->frame))
	{
		ShowFrame(o, fx);
		fx->shown = idx;
		fx->taken = idx + 1; // this and every older frame are let go
	}
	if(vEnd && fx->taken >= decoded)
	{ // every frame shown; the sound may still be draining
		int aEnd = 1;
		if(fx->hasAudio)
			Host_DecoderSamples(o, fx->m, NULL, 0, &aEnd);
		if(aEnd)
		{
			MovieFx_Stop(o);
			return 0;
		}
	}
	return 1;
}
