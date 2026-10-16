/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * movie.c - movie playback of "90 F0" .. "90 F3".  Interface: bgi/gfx.h
 *           (Movie_*, gMoviePlaying).
 *
 * The original plays a video file through a DirectShow filter graph shown
 * in a child window over the picture; the graph's event notifications
 * reach the main window procedure as WM_APP.  All of the COM work is in the
 * OS layer (OS_Movie*); this file keeps the engine-visible state: whether
 * a movie runs (the window then paints black and the display style cannot
 * be toggled), the volume, the error reporting and the length result.
 * When the OS layer has no player for the file (POSIX always; Windows
 * when DirectShow cannot render it) the ffmpeg player of
 * src/sys/moviefx.c takes over: it draws its frames from the engine's
 * pass (Movie_Tick) and its end is reported the way the graph's was.
 */
#include "bgi/gfx.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/display.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/os.h"

int gMoviePlaying;           // a movie is running: read by the window procedure and the display style toggle
static int gMovieGraph;      // a graph exists
static int gMovieLoaded;     // a file is rendered in it
static int gMovieFx;         // the ffmpeg player runs the movie instead of the OS layer's graph
static int32_t gMovieVolume; // IBasicAudio units, -10000 .. 0; applied to every movie started

// "90 F1" (and engine shutdown): release the graph, if any.  1 when there was one, else 0
int Movie_Stop(void)
{
	int had = gMovieGraph || gMovieFx;
	if(!had)
		return 0;
	if(gMovieFx)
		MovieFx_Stop();
	else
		OS_MovieStop(); // IMediaControl::Stop when playing, hide and disown the video window, release all
	gMoviePlaying = 0;
	gMovieGraph = 0;
	gMovieFx = 0;
	gMovieLoaded = 0;
	return had;
}

/* "90 F0": stop whatever runs, build a graph for `path` and start it in
 * the (x, y, w, h) rectangle of the picture (mapped through the display
 * scaling) at the current volume.  1 when playback started, *outLength
 * then holding the length in ms as the player reports it.  A missing
 * audio device or renderer is a script error (thrown; the graph is kept
 * for the next stop); any other failure returns 0. */
int Movie_Play(int32_t* outLength, const char* path, int x, int y, int w, int h)
{
	Bmp_t scaled;
	Rect_t r, m;
	int32_t volume = gMovieVolume;

	Movie_Stop();
	// build the graph with its interfaces and render the file into it (IGraphBuilder::RenderFile)
	switch(OS_MovieOpen(path))
	{
		case OS_MOVIE_OK: gMovieGraph = 1; break;
		case OS_MOVIE_FAILED:
			// no native player for the file: ffmpeg, which scales the frames to the rectangle itself
			if(!MovieFx_Start(path, x, y, w, h, volume, outLength))
				return 0;
			gMovieFx = 1;
			gMovieLoaded = 1;
			gMoviePlaying = 1;
			return 1;
		case OS_MOVIE_NO_AUDIO_DEVICE:
			gMovieGraph = 1;
			ThrowScriptError(MSG_MOVIE_NO_AUDIO_DEVICE);
			return 0;
		case OS_MOVIE_VIDEO_NOT_RENDERED:
			gMovieGraph = 1;
			ThrowScriptError(MSG_MOVIE_VIDEO_NOT_RENDERED);
			return 0;
		case OS_MOVIE_AUDIO_NOT_RENDERED:
			gMovieGraph = 1;
			ThrowScriptError(MSG_MOVIE_AUDIO_NOT_RENDERED);
			return 0;
		default: return 0; // the original keeps a failed graph until the next stop; the OS layer releases it at once
	}
	r.l = x;
	r.t = y;
	r.r = x + w - 1;
	r.b = y + h - 1;
	if(Gfx_GetScaledBmp(gGfx, &scaled)) // a scaled display: map the picture rectangle to window coordinates
	{
		Gfx_MapRectScaled(gGfx, &m, &r);
		x = m.l;
		y = m.t;
		w = m.r - m.l + 1;
		h = m.b - m.t + 1;
	}
	if(!OS_MovieStart(x, y, w, h, volume, outLength))
		return 0;
	gMovieLoaded = 1;
	gMoviePlaying = 1;
	return 1;
}

// "90 F2": a movie was started and the player still runs it
int Movie_IsPlaying(void)
{
	if(gMovieFx)
		return gMoviePlaying && MovieFx_IsRunning();
	return gMoviePlaying && OS_MovieIsRunning();
}

/* "90 F3": the volume, 0 .. 0x80, for the running movie and the ones to
 * come; 0 is silence (-10000), otherwise the attenuation
 * -(12800 - 100 v) * 0.375 in hundredths of a decibel.  1 ok, 0 for a
 * value above 0x80 (nothing changes). */
int Movie_SetVolume(uint32_t v)
{
	int32_t vol;
	if(v > 0x80)
		return 0;
	if(v == 0)
		vol = -10000;
	else
		vol = (int32_t) - ((double)(int64_t)(0x3200 - (int32_t)(v * 100)) * 0.375000000009375); // the original's constant: 0.375 with rounding noise
	OS_MovieSetVolume(vol);                                                                     // the original applies it only when a graph exists; the OS layer ignores the call without one
	MovieFx_SetVolume(vol);
	gMovieVolume = vol;
	return 1;
}

// every pass of the scheduler: the ffmpeg player shows its due frame; its end is the graph's EC_COMPLETE
void Movie_Tick(void)
{
	if(gMovieFx && gMoviePlaying && !MovieFx_Tick())
		Movie_OnEvent(1);
}

/* the WM_APP notification of the main window: the OS layer has drained
 * the graph's events, `completed` says the movie reached its end.  A
 * movie that is over (or was stopped meanwhile) is released and the
 * picture restored. */
void Movie_OnEvent(int completed)
{
	if(completed)
		gMoviePlaying = 0;
	if(!gMoviePlaying)
	{
		Movie_Stop();
		Window_Repaint();
	}
}
