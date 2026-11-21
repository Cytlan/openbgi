/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bgm.c - the music channels of the Wave Master library
 *         (src/include/bgi/snd/wavemaster.h): a BW file or archive entry - or an
 *         intro / loop pair of them - streamed through the decoder pump
 *
 * The four openers share Bgm_OpenSource / Bgm_OpenPairSources, which
 * turn a source into a stream, and Bgm_Start, which puts the stream on
 * the channel and starts it.  The other entry points work on a loaded
 * channel after Wm_BgmCheck.
 */
#include "wm_internal.h"
#include "bgi/core.h"

/* Attach a freshly opened stream to a channel and start it: the channel
 * volume is `volume`, the fade level full with no fades, the pan `pan`
 * (0 .. 0x80 clamped, 0x40 centred).  Whether the music loops is the
 * file's business: the BW header's loop flag and restart position are
 * what the stream's "continues" query reports to the pump; the
 * single-file openers pass no loop flag of their own, and the override
 * of Bgm_OpenSource serves the "same file as intro and loop" case of the
 * pair openers.  Always 0. */
static int Bgm_Start(OpenBGI_t* o, int ch, WmStream_t* s, int volume, int pan)
{
	WmRecord_t* r = &o->snd.bgm[ch];
	Voice_Attach(r->voice, s);
	r->stream = s;
	r->fadeVol.active = 0;
	r->volume = volume;
	r->fadeLevel = 0x80;
	r->fade.active = 0;
	r->active = 1;
	Voice_SetVolume(r->voice, Record_Level(o, r));
	// the pan of Wm_BgmSetPan: 0 .. 0x80 clamped, then (pan - 0x40) * 2
	if(pan > 0x80)
		pan = 0x80;
	else if(pan < 0)
		pan = 0;
	Voice_SetPan(r->voice, BGI_Ftol((double)(pan - 0x40) * 0.015625 * 128.0));
	r->voice->playing = 1; // the streaming buffer starts with its first fill
	return 0;
}

/* open a music stream on `src` and start it on the channel; `loopFlag`
 * bit 0 set overrides the header's loop specification with bit 1 (loop
 * from frame 0 when set, play once when clear).  0, or WM_E_NOT_BW when
 * the source is not a playable BW file (the channel is then marked
 * empty; the source is released by the failed open) */
static int Bgm_OpenSource(OpenBGI_t* o, int ch, WmSource_t* src, int volume, int pan, double gain, int loopFlag)
{
	int err;
	WmStream_t* s = WmStream_Open(o, src, gain, 0, &err);
	if(!s)
	{
		o->snd.bgm[ch].active = 0; // what the catch block of the original's openers does
		return WM_E_NOT_BW;
	}
	if(loopFlag & 1)
		WmStream_SetLoop(s, loopFlag & 2, 0);
	if(o->snd.sndDebug)
	{
		const BwHeader_t* h = WmStream_Header(s);
		SND_DEBUG(o, "music channel %d: codec %u, %u Hz x %u, %u frames, loop %s from frame %u%s\n", ch, (unsigned)h->codec,
			(unsigned)h->rate, (unsigned)h->channels, (unsigned)h->frames, WmStream_Continues(s) ? "on" : "off",
			(unsigned)h->loopStart, loopFlag & 1 ? " (pair form override)" : "");
	}
	return Bgm_Start(o, ch, s, volume, pan);
}

// stream the loose file at `path` on the channel; 0, 0x14, 0x15, 0xc missing, 0xe not a BW file
int Wm_BgmOpenFile(OpenBGI_t* o, int ch, const char* path, int volume, int pan, double gain)
{
	int res = 0;
	WmSource_t* src;
	if(!Wm_Running(o))
		res = WM_E_NOT_RUNNING;
	else if((uint32_t)ch >= WM_BGM_CHANNELS)
		res = WM_E_BAD_CHANNEL;
	else if((src = WmSource_OpenFile(o, path)) == NULL)
		res = WM_E_NOT_FOUND;
	else
		res = Bgm_OpenSource(o, ch, src, volume, pan, gain, 0);
	return res;
}

// stream the entry `name` of the archive at `arcPath` on the channel; the same results
int Wm_BgmOpenArc(OpenBGI_t* o, int ch, const char* arcPath, const char* name, int volume, int pan, double gain)
{
	int res = 0;
	WmSource_t* src;
	if(!Wm_Running(o))
		res = WM_E_NOT_RUNNING;
	else if((uint32_t)ch >= WM_BGM_CHANNELS)
		res = WM_E_BAD_CHANNEL;
	else if((src = WmSource_OpenArc(o, arcPath, name)) == NULL)
		res = WM_E_NOT_FOUND;
	else
		res = Bgm_OpenSource(o, ch, src, volume, pan, gain, 0);
	return res;
}

/* open a pair stream on two sources (both must be Vorbis), the loop part
 * repeating while `mode` is non-zero, and start it; 0, or WM_E_NOT_BW
 * (the sources are released by the failed open) */
static int Bgm_OpenPairSources(OpenBGI_t* o, int ch, WmSource_t* a, WmSource_t* b, int mode, int volume, int pan, double gain)
{
	int err;
	WmStream_t* s = WmStream_OpenPair(o, a, b, gain, &err);
	if(!s)
	{
		o->snd.bgm[ch].active = 0;
		return WM_E_NOT_BW;
	}
	WmStream_SetMode(s, mode);
	return Bgm_Start(o, ch, s, volume, pan);
}

/* stream the loose files `intro` and `loop` as a pair; the same name
 * twice opens one file that loops from frame 0 when `mode` is non-zero
 * and plays once otherwise.  0, 0x14, 0x15, 0xc either file missing,
 * 0xe not (both) Vorbis BW files. */
int Wm_BgmOpenFilePair(OpenBGI_t* o, int ch, const char* intro, const char* loop, int mode, int volume, int pan, double gain)
{
	int res = 0;
	if(!Wm_Running(o))
		res = WM_E_NOT_RUNNING;
	else if((uint32_t)ch >= WM_BGM_CHANNELS)
		res = WM_E_BAD_CHANNEL;
	else if(strcmp(intro, loop) == 0)
	{ // one file: loop it when `mode` says so
		WmSource_t* src = WmSource_OpenFile(o, intro);
		res = src ? Bgm_OpenSource(o, ch, src, volume, pan, gain, mode ? 3 : 1) : WM_E_NOT_FOUND;
	}
	else
	{
		WmSource_t* a = WmSource_OpenFile(o, intro);
		WmSource_t* b = a ? WmSource_OpenFile(o, loop) : NULL;
		if(!a || !b)
		{
			WmSource_Close(a);
			res = WM_E_NOT_FOUND;
		}
		else
			res = Bgm_OpenPairSources(o, ch, a, b, mode, volume, pan, gain);
	}
	return res;
}

// Wm_BgmOpenFilePair with both parts taken from the archive at `arcPath`
int Wm_BgmOpenArcPair(OpenBGI_t* o, int ch, const char* arcPath, const char* intro, const char* loop, int mode, int volume, int pan,
	double gain)
{
	int res = 0;
	if(!Wm_Running(o))
		res = WM_E_NOT_RUNNING;
	else if((uint32_t)ch >= WM_BGM_CHANNELS)
		res = WM_E_BAD_CHANNEL;
	else if(strcmp(intro, loop) == 0)
	{
		WmSource_t* src = WmSource_OpenArc(o, arcPath, intro);
		res = src ? Bgm_OpenSource(o, ch, src, volume, pan, gain, mode ? 3 : 1) : WM_E_NOT_FOUND;
	}
	else
	{
		WmSource_t* a = WmSource_OpenArc(o, arcPath, intro);
		WmSource_t* b = a ? WmSource_OpenArc(o, arcPath, loop) : NULL;
		if(!a || !b)
		{
			WmSource_Close(a);
			res = WM_E_NOT_FOUND;
		}
		else
			res = Bgm_OpenPairSources(o, ch, a, b, mode, volume, pan, gain);
	}
	return res;
}

// stop the channel and rewind its stream (it stays loaded); 0, or the check's code
int Wm_BgmStop(OpenBGI_t* o, int ch)
{
	int res;
	res = Wm_BgmCheck(o, ch);
	if(res == 0)
		Voice_Stop(o->snd.bgm[ch].voice);
	return res;
}

// the channel's master volume, applied at once; 0, 0x14 not running, 0x15 bad channel
int Wm_BgmSetMasterVolume(OpenBGI_t* o, int ch, uint32_t vol)
{
	int res = 0;
	if(!Wm_Running(o))
		res = WM_E_NOT_RUNNING;
	else if((uint32_t)ch >= WM_BGM_CHANNELS)
		res = WM_E_BAD_CHANNEL;
	else
	{
		// the original does not clamp on the music side (the effect side does): a value above 0x80 is passed through
		o->snd.bgm[ch].master = (int32_t)vol;
		Voice_SetVolume(o->snd.bgm[ch].voice, Record_Level(o, &o->snd.bgm[ch]));
	}
	return res;
}

/* the channel's own volume ("A0 1C", 1.553 on; clamped to 0x80), applied
 * at once whether or not anything is loaded and kept across loads; 0,
 * 0x14 not running, 0x15 bad channel */
int Wm_BgmSetChannelVolume(OpenBGI_t* o, int ch, uint32_t vol)
{
	int res = 0;
	if(!Wm_Running(o))
		res = WM_E_NOT_RUNNING;
	else if((uint32_t)ch >= WM_BGM_CHANNELS)
		res = WM_E_BAD_CHANNEL;
	else
	{
		o->snd.bgm[ch].aux = (int32_t)(vol > 0x80 ? 0x80 : vol);
		Voice_SetVolume(o->snd.bgm[ch].voice, Record_Level(o, &o->snd.bgm[ch]));
	}
	return res;
}

// fade the channel's fade level from where it is to silence over `ms`; 0, or the check's code
int Wm_BgmFadeOut(OpenBGI_t* o, int ch, int ms)
{
	int res;
	res = Wm_BgmCheck(o, ch);
	if(res == 0)
		Fade_Start(&o->snd.bgm[ch].fade, Clock_HostMs(o), ms, o->snd.bgm[ch].fadeLevel, 0);
	return res;
}

// fade the fade level from where it is up to full (0x80) over `ms`; 0, or the check's code
int Wm_BgmFadeIn(OpenBGI_t* o, int ch, int ms)
{
	int res;
	res = Wm_BgmCheck(o, ch);
	if(res == 0)
		Fade_Start(&o->snd.bgm[ch].fade, Clock_HostMs(o), ms, o->snd.bgm[ch].fadeLevel, 0x80);
	return res;
}

// `paused` non-zero stops the channel in place, 0 lets it play on; 0, or the check's code
int Wm_BgmPause(OpenBGI_t* o, int ch, int paused)
{
	int res;
	res = Wm_BgmCheck(o, ch);
	if(res == 0)
		Voice_Pause(o->snd.bgm[ch].voice, paused);
	return res;
}

/* fade the channel volume from where it is to `volume` (clamped to 0x80)
 * over `ms`; 0, or the check's code */
int Wm_BgmFadeVolume(OpenBGI_t* o, int ch, int volume, int ms)
{
	int res;
	res = Wm_BgmCheck(o, ch);
	if(res == 0)
	{
		if((uint32_t)volume > 0x80)
			volume = 0x80;
		Fade_Start(&o->snd.bgm[ch].fadeVol, Clock_HostMs(o), ms, o->snd.bgm[ch].volume, volume);
	}
	return res;
}

/* the channel's pan: 0 .. 0x80 clamped, 0x40 centred, given to the voice
 * as (pan - 0x40) * 2; 0, or the check's code */
int Wm_BgmSetPan(OpenBGI_t* o, int ch, int pan)
{
	int res;
	res = Wm_BgmCheck(o, ch);
	if(res == 0)
	{
		if(pan > 0x80)
			pan = 0x80;
		else if(pan < 0)
			pan = 0;
		Voice_SetPan(o->snd.bgm[ch].voice, BGI_Ftol((double)(pan - 0x40) * 0.015625 * 128.0));
	}
	return res;
}

// *out = the channel's voice is playing (1 / 0); 0, or the check's code (*out untouched)
int Wm_BgmIsPlaying(OpenBGI_t* o, int ch, uint32_t* out)
{
	int res;
	res = Wm_BgmCheck(o, ch);
	if(res == 0)
		*out = (uint32_t)Voice_IsPlaying(o->snd.bgm[ch].voice);
	return res;
}

// *out = the stream's loop count (WmStream_LoopCount); 0, or the check's code (*out untouched)
int Wm_BgmLoopCount(OpenBGI_t* o, int ch, uint32_t* out)
{
	int res;
	res = Wm_BgmCheck(o, ch);
	if(res == 0)
		*out = (uint32_t)WmStream_LoopCount(o, o->snd.bgm[ch].stream);
	return res;
}
