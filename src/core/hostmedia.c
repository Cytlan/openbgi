/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * hostmedia.c - the host's CD audio, movie player and movie decoder as
 *               the engine calls them (src/include/bgi/host.h)
 *
 * Thin calls of OpenBGI_CdAudio_t, OpenBGI_MoviePlayer_t and
 * OpenBGI_MovieDecoder_t (openbgi.h).  Without the host's table or
 * member there is no CD drive, no player (OPENBGI_MOVIE_FAILED: nothing
 * can be played) and no decoder.
 */
#include "bgi/host.h"
#include "bgi/core.h"

#include <string.h>

static const OpenBGI_CdAudio_t* Cd(OpenBGI_t* o)
{
	return o->host.cdAudio;
}

static const OpenBGI_MoviePlayer_t* Player(OpenBGI_t* o)
{
	return o->host.moviePlayer;
}

static const OpenBGI_MovieDecoder_t* Decoder(OpenBGI_t* o)
{
	return o->host.movieDecoder;
}

// ---- CD audio ---------------------------------------------------------------------------------

uint32_t Host_CdOpen(OpenBGI_t* o, const char* element)
{
	const OpenBGI_CdAudio_t* c = Cd(o);
	return c && c->open ? c->open(o->host.user, element) : 0;
}

int Host_CdSetTimeFormatTmsf(OpenBGI_t* o, uint32_t device)
{
	const OpenBGI_CdAudio_t* c = Cd(o);
	return c && c->setTimeFormatTmsf ? c->setTimeFormatTmsf(o->host.user, device) != 0 : 0;
}

int Host_CdStatus(OpenBGI_t* o, uint32_t device, uint32_t item, uint32_t* out)
{
	const OpenBGI_CdAudio_t* c = Cd(o);
	return c && c->status ? c->status(o->host.user, device, item, out) != 0 : 0;
}

int Host_CdPlay(OpenBGI_t* o, uint32_t device, uint32_t from, uint32_t to, int notify)
{
	const OpenBGI_CdAudio_t* c = Cd(o);
	return c && c->play ? c->play(o->host.user, device, from, to, notify) != 0 : 0;
}

int Host_CdStop(OpenBGI_t* o, uint32_t device)
{
	const OpenBGI_CdAudio_t* c = Cd(o);
	return c && c->stop ? c->stop(o->host.user, device) != 0 : 0;
}

int Host_CdClose(OpenBGI_t* o, uint32_t device)
{
	const OpenBGI_CdAudio_t* c = Cd(o);
	return c && c->close ? c->close(o->host.user, device) != 0 : 0;
}

// ---- the movie player -------------------------------------------------------------------------

int Host_HasPlayer(OpenBGI_t* o)
{
	const OpenBGI_MoviePlayer_t* p = Player(o);
	return p && p->open;
}

int Host_MovieOpen(OpenBGI_t* o, const char* path)
{
	const OpenBGI_MoviePlayer_t* p = Player(o);
	return p && p->open ? p->open(o->host.user, path) : OPENBGI_MOVIE_FAILED;
}

int Host_MovieStart(OpenBGI_t* o, int x, int y, int w, int h, int32_t volume, int32_t* lengthMs)
{
	const OpenBGI_MoviePlayer_t* p = Player(o);
	return p && p->start ? p->start(o->host.user, x, y, w, h, volume, lengthMs) != 0 : 0;
}

void Host_MovieStop(OpenBGI_t* o)
{
	const OpenBGI_MoviePlayer_t* p = Player(o);
	if(p && p->stop)
		p->stop(o->host.user);
}

int Host_MovieRunning(OpenBGI_t* o)
{
	const OpenBGI_MoviePlayer_t* p = Player(o);
	return p && p->running ? p->running(o->host.user) != 0 : 0;
}

void Host_MovieSetVolume(OpenBGI_t* o, int32_t volume)
{
	const OpenBGI_MoviePlayer_t* p = Player(o);
	if(p && p->setVolume)
		p->setVolume(o->host.user, volume);
}

// ---- the movie decoder ------------------------------------------------------------------------

int Host_HasDecoder(OpenBGI_t* o)
{
	const OpenBGI_MovieDecoder_t* d = Decoder(o);
	return d && d->open;
}

void* Host_DecoderOpen(OpenBGI_t* o, const OpenBGI_MovieFile_t* file, const char* path, int w, int h, int audio, OpenBGI_MovieInfo_t* info)
{
	const OpenBGI_MovieDecoder_t* d = Decoder(o);
	memset(info, 0, sizeof *info);
	info->fps = 30.0;
	return d && d->open && file ? d->open(o->host.user, file, path, w, h, audio, info) : NULL;
}

int32_t Host_DecoderFrames(OpenBGI_t* o, void* m, int* ended)
{
	const OpenBGI_MovieDecoder_t* d = Decoder(o);
	*ended = !d || !d->frames;
	return *ended ? 0 : d->frames(o->host.user, m, ended);
}

int Host_DecoderTake(OpenBGI_t* o, void* m, int32_t index, uint8_t* out)
{
	const OpenBGI_MovieDecoder_t* d = Decoder(o);
	return d && d->take ? d->take(o->host.user, m, index, out) != 0 : 0;
}

int Host_DecoderSamples(OpenBGI_t* o, void* m, int16_t* out, int frames, int* ended)
{
	const OpenBGI_MovieDecoder_t* d = Decoder(o);
	*ended = !d || !d->samples;
	return *ended ? 0 : d->samples(o->host.user, m, out, frames, ended);
}

void Host_DecoderClose(OpenBGI_t* o, void* m)
{
	const OpenBGI_MovieDecoder_t* d = Decoder(o);
	if(m && d && d->close)
		d->close(o->host.user, m);
}
