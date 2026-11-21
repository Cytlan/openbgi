/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * movie.c - movie playback of "90 F0" .. "90 F3".  Interface: bgi/gfx.h
 *           (Movie_*, o->movie.moviePlaying).
 *
 * The original plays a video file through a DirectShow filter graph shown
 * in a child window over the picture; the graph's event notifications
 * reach the main window procedure as WM_APP.  That player is the host's
 * (OpenBGI_MoviePlayer_t, Host_Movie*; its events come as
 * OpenBGI_MovieEvent); this file keeps the engine-visible state: whether
 * a movie runs (the window then paints black and the display style cannot
 * be toggled), the volume, the error reporting and the length result.
 * When the host has no player for the file (none at all but on Windows,
 * or DirectShow cannot render it) and a decoder, src/sys/moviefx.c shows
 * what the decoder hands over: it draws the frames from the engine's
 * pass (Movie_Tick) and its end is reported the way the graph's was.
 */
#include "bgi/gfx.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/display.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/host.h"
#include "bgi/fs.h"
#include "bgi/core.h"

// "90 F1" (and engine shutdown): release the graph, if any.  1 when there was one, else 0
int Movie_Stop(OpenBGI_t* o)
{
	int had = o->movie.movieGraph || o->movie.movieFx;
	if(!had)
		return 0;
	if(o->movie.movieFx)
		MovieFx_Stop(o);
	else
		Host_MovieStop(o); // IMediaControl::Stop when playing, hide and disown the video window, release all
	o->movie.moviePlaying = 0;
	o->movie.movieGraph = 0;
	o->movie.movieFx = 0;
	o->movie.movieLoaded = 0;
	return had;
}

// the host's decoder decodes `path` and the engine shows it (MovieFx); 0 when it cannot
static int PlayDecoded(OpenBGI_t* o, int32_t* outLength, const char* path, int x, int y, int w, int h)
{
	if(!MovieFx_Start(o, path, x, y, w, h, o->movie.movieVolume, outLength))
		return 0;
	o->movie.movieFx = 1;
	o->movie.movieLoaded = 1;
	o->movie.moviePlaying = 1;
	return 1;
}

/* "90 F0": stop whatever runs, build a graph for `path` and start it in
 * the (x, y, w, h) rectangle of the picture (mapped through the display
 * scaling) at the current volume.  1 when playback started, *outLength
 * then holding the length in ms as the player reports it.  A missing
 * audio device or renderer is a script error (thrown; the graph is kept
 * for the next stop); any other failure returns 0.
 *
 * Of the host's two ways to play a movie the decoder goes first, whose
 * frames the engine shows itself, the same on every system.  The host's
 * own player (the original's way: DirectShow) plays when the host has no
 * decoder, or when it was asked for - "movies": "player" in openbgi.json,
 * --movies=player - and then the decoder gets what the player cannot
 * play. */
int Movie_Play(OpenBGI_t* o, int32_t* outLength, const char* path, int x, int y, int w, int h)
{
	Bmp_t scaled;
	Rect_t r, m;
	int32_t volume = o->movie.movieVolume;
	char local[0x400];

	Movie_Stop(o);
	if(Host_HasDecoder(o) && !(o->engine.optMovies ? o->engine.optMovies == 2 : o->engine.moviesPlayer))
		return PlayDecoded(o, outLength, path, x, y, w, h);
	if(!Host_HasPlayer(o)) // (nothing plays it: an archive's file is not taken out for nobody)
		return PlayDecoded(o, outLength, path, x, y, w, h);
	// build the graph with its interfaces and render the file into it (IGraphBuilder::RenderFile)
	Fs_LocalPath(o, path, local, sizeof local); // the player opens the file by its name: a zip game's is extracted
	switch(Host_MovieOpen(o, local))
	{
		case OPENBGI_MOVIE_OK: o->movie.movieGraph = 1; break;
		case OPENBGI_MOVIE_FAILED:
			// no player for the file: the host's decoder, which scales the frames to the rectangle itself
			return PlayDecoded(o, outLength, path, x, y, w, h);
		case OPENBGI_MOVIE_NO_AUDIO_DEVICE:
			o->movie.movieGraph = 1;
			ThrowScriptError(o, MSG_MOVIE_NO_AUDIO_DEVICE);
			return 0;
		case OPENBGI_MOVIE_VIDEO_NOT_RENDERED:
			o->movie.movieGraph = 1;
			ThrowScriptError(o, MSG_MOVIE_VIDEO_NOT_RENDERED);
			return 0;
		case OPENBGI_MOVIE_AUDIO_NOT_RENDERED:
			o->movie.movieGraph = 1;
			ThrowScriptError(o, MSG_MOVIE_AUDIO_NOT_RENDERED);
			return 0;
		default: return 0; // the original keeps a failed graph until the next stop; the host releases it at once
	}
	r.l = x;
	r.t = y;
	r.r = x + w - 1;
	r.b = y + h - 1;
	if(Gfx_GetScaledBmp(o->gfx.gfx, &scaled)) // a scaled display: map the picture rectangle to window coordinates
	{
		Gfx_MapRectScaled(o->gfx.gfx, &m, &r);
		x = m.l;
		y = m.t;
		w = m.r - m.l + 1;
		h = m.b - m.t + 1;
	}
	if(!Host_MovieStart(o, x, y, w, h, volume, outLength))
		return 0;
	o->movie.movieLoaded = 1;
	o->movie.moviePlaying = 1;
	return 1;
}

// "90 F2": a movie was started and the player still runs it
int Movie_IsPlaying(OpenBGI_t* o)
{
	if(o->movie.movieFx)
		return o->movie.moviePlaying && MovieFx_IsRunning(o);
	return o->movie.moviePlaying && Host_MovieRunning(o);
}

/* "90 F3": the volume, 0 .. 0x80, for the running movie and the ones to
 * come; 0 is silence (-10000), otherwise the attenuation
 * -(12800 - 100 v) * 0.375 in hundredths of a decibel.  1 ok, 0 for a
 * value above 0x80 (nothing changes). */
int Movie_SetVolume(OpenBGI_t* o, uint32_t v)
{
	int32_t vol;
	if(v > 0x80)
		return 0;
	if(v == 0)
		vol = -10000;
	else
		vol = (int32_t) - ((double)(int64_t)(0x3200 - (int32_t)(v * 100)) * 0.375000000009375); // the original's constant: 0.375 with rounding noise
	// the original applies it only when a graph exists; the host ignores the call without one
	Host_MovieSetVolume(o, vol);
	MovieFx_SetVolume(o, vol);
	o->movie.movieVolume = vol;
	return 1;
}

// every pass of the scheduler: the decoded movie shows its due frame; its end is the graph's EC_COMPLETE
void Movie_Tick(OpenBGI_t* o)
{
	if(o->movie.movieFx && o->movie.moviePlaying && !MovieFx_Tick(o))
		Movie_OnEvent(o, 1);
}

/* the WM_APP notification of the main window (OpenBGI_MovieEvent): the
 * host has drained the player's events, `completed` says the movie
 * reached its end.  A
 * movie that is over (or was stopped meanwhile) is released and the
 * picture restored. */
void Movie_OnEvent(OpenBGI_t* o, int completed)
{
	if(completed)
		o->movie.moviePlaying = 0;
	if(!o->movie.moviePlaying)
	{
		Movie_Stop(o);
		Window_Repaint(o);
	}
}
