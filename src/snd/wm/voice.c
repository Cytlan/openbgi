/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * voice.c - a playing sound of the Wave Master library and the software
 *           mixer that sums them (wm_internal.h)
 *
 * A voice stands in for a DirectSound secondary buffer: an effect plays
 * from its decoded samples, music is pulled from its decoder through a
 * 4-second ring that Voice_Pump keeps half a second ahead, restarting the
 * stream where it says it continues.  The mixer runs in the OS layer's
 * audio thread (OS_AudioStart, 44.1 kHz stereo like the original's
 * primary buffer) under the library lock; attenuations are applied as
 * linear gains and sources with another sample rate are interpolated
 * linearly.
 */
#include "wm_internal.h"

// ---- voices ------------------------------------------------------------------------------------

// the linear gains of both output channels from the DirectSound volume and pan
static void Voice_UpdateGains(Voice_t* v)
{
	double g = v->dsVolume <= -10000 ? 0.0 : pow(10.0, v->dsVolume / 2000.0);
	// DirectSound pan: the opposite channel is attenuated by |pan| hundredths of a decibel
	double attL = v->pan > 0 ? pow(10.0, -v->pan / 2000.0) : 1.0;
	double attR = v->pan < 0 ? pow(10.0, v->pan / 2000.0) : 1.0;
	v->gainL = g * attL;
	v->gainR = g * attR;
}

// the level (0 .. 0x80 dB of attenuation) in DirectSound units, never below -10000
void Voice_SetVolume(Voice_t* v, int level)
{
	int32_t ds = level * -100;
	if(ds < -10000)
		ds = -10000;
	v->dsVolume = ds;
	Voice_UpdateGains(v);
}

// pan -128 .. 128 (clamped) -> (pan / 128)^3 * 10000 in DirectSound units
void Voice_SetPan(Voice_t* v, int pan)
{
	double p;
	if(pan < -128)
		pan = -128;
	else if(pan > 128)
		pan = 128;
	p = pow((double)pan, 3.0) / pow(128.0, 3.0) * -10000.0;
	v->pan = BGI_Ftol(-p); // the original negates the product and negates it again: the sign is that of `pan`
	Voice_UpdateGains(v);
}

/* Take a freshly opened stream (closing the previous one): the format,
 * a stopped voice at the start, full volume, and - for music - a fresh
 * ring sized for 4 seconds of the stream, or - for an effect - the
 * decoded samples. */
void Voice_Attach(Voice_t* v, WmStream_t* s)
{
	if(v->stream)
		WmStream_Close(v->stream);
	v->stream = s;
	v->rate = WmStream_Rate(s);
	v->channels = WmStream_Channels(s);
	v->playing = v->paused = 0;
	v->pos = v->posFrac = 0;
	v->dsVolume = -100 * 0x80; // the original's voice holds the level 0x80 after a load
	if(v->isBgm)
	{
		BGI_Free(v->ring);
		v->ringFrames = v->rate * WM_RING_SECONDS;
		v->ring = (int16_t*)BGI_Alloc((size_t)v->ringFrames * v->channels * sizeof(int16_t));
		v->ringRead = v->ringCount = 0;
		v->done = 0;
	}
	else
	{
		v->samples = WmStream_Samples(s);
		v->frames = WmStream_Frames(s);
	}
	Voice_UpdateGains(v);
}

// close the stream and drop the samples / the ring; the voice is silent afterwards
void Voice_Detach(Voice_t* v)
{
	if(v->stream)
		WmStream_Close(v->stream);
	v->stream = NULL;
	v->samples = NULL;
	v->frames = 0;
	v->playing = v->paused = 0;
	BGI_Free(v->ring);
	v->ring = NULL;
	v->ringFrames = v->ringRead = v->ringCount = 0;
}

// stop, the play cursor back to the start, the stream rewound (and a music ring emptied)
void Voice_Stop(Voice_t* v)
{
	v->playing = 0;
	v->paused = 0;
	v->pos = v->posFrac = 0;
	if(v->stream)
		WmStream_Rewind(v->stream);
	if(v->isBgm)
	{
		v->ringRead = v->ringCount = 0;
		v->done = 0;
	}
}

// set the volume level and play from the current position
void Voice_Play(Voice_t* v, int level)
{
	Voice_SetVolume(v, level);
	v->playing = 1;
	v->paused = 0;
}

// `paused` stops a playing voice without moving its position; 0 lets a stopped one play on
void Voice_Pause(Voice_t* v, int paused)
{
	if(v->playing && paused)
	{
		v->paused = 1; // Stop() without moving the position
		v->playing = 0;
	}
	else if(!v->playing && !paused)
	{
		v->playing = 1; // Play() again
		v->paused = 0;
	}
}

int Voice_IsPlaying(const Voice_t* v)
{
	return v->playing;
}

/* the decoder pump: keep the ring at least half a second ahead,
 * decoding up to half a second at a time and restarting the stream where
 * it says it continues; a stream that has ended marks the voice done */
void Voice_Pump(Voice_t* v)
{
	uint32_t half = v->rate / 2;
	while(!v->done && v->ringFrames - v->ringCount > half)
	{
		uint32_t want = half, got = 0;
		while(got < want)
		{
			uint32_t w = v->ringRead + v->ringCount;
			uint32_t n = want - got, space;
			uint32_t r;
			if(w >= v->ringFrames)
				w -= v->ringFrames;
			space = v->ringFrames - w; // contiguous frames to the end of the ring
			if(n > space)
				n = space;
			r = WmStream_Decode(v->stream, v->ring + (size_t)w * v->channels, n);
			v->ringCount += r;
			got += r;
			if(r != n)
			{
				if(WmStream_Continues(v->stream))
				{
					WmStream_Restart(v->stream);
					SND_DEBUG("music channel %d restarts (loop %d)\n", (int)(v - gBgmVoices), WmStream_LoopCount(v->stream) + 1);
					continue;
				}
				SND_DEBUG("music channel %d reached its end\n", (int)(v - gBgmVoices));
				v->done = 1;
				break;
			}
		}
	}
}

// ---- the mixer ---------------------------------------------------------------------------------

/* one source frame at `index` of a voice into *l, *r (effects: the
 * sample array; music: the ring, counted from its read end); 0 past the
 * end */
static int Voice_Frame(const Voice_t* v, uint32_t index, int16_t* l, int16_t* r)
{
	const int16_t* p;
	if(v->isBgm)
	{
		uint32_t i;
		if(index >= v->ringCount)
			return 0;
		i = v->ringRead + index;
		if(i >= v->ringFrames)
			i -= v->ringFrames;
		p = v->ring + (size_t)i * v->channels;
	}
	else
	{
		if(index >= v->frames)
			return 0;
		p = v->samples + (size_t)index * v->channels;
	}
	*l = p[0];
	*r = v->channels == 2 ? p[1] : p[0];
	return 1;
}

/* mix `frames` output frames of one voice into the 32-bit stereo
 * accumulator, resampling to the mixer's rate by linear interpolation;
 * an effect that runs out stops and rewinds itself, music stops once the
 * pump has found the end and the ring is empty */
static void Voice_Mix(Voice_t* v, int32_t* acc, int frames)
{
	uint32_t step = (uint32_t)(((uint64_t)v->rate << 16) / WM_MIX_RATE); // source frames per output frame, 16.16
	int i;
	if(!v->playing || !v->stream)
		return;
	if(v->isBgm)
		Voice_Pump(v);
	for(i = 0; i < frames; i++)
	{
		int16_t l0, r0, l1, r1;
		int32_t l, r, frac = (int32_t)(v->posFrac >> 8); // 0 .. 255
		uint32_t idx = v->isBgm ? 0 : v->pos;            // music is consumed from the ring's read end
		if(!Voice_Frame(v, idx, &l0, &r0))
		{ // nothing to play: an effect has ended, music either waits for the pump or has ended
			if(!v->isBgm)
			{
				v->playing = 0;
				v->pos = v->posFrac = 0; // the buffer's play cursor is back at the start
				WmStream_Rewind(v->stream);
			}
			else if(v->done)
				v->playing = 0;
			return;
		}
		if(!Voice_Frame(v, idx + 1, &l1, &r1))
			l1 = l0, r1 = r0;
		// linear interpolation towards the next source frame
		l = l0 + ((((int32_t)l1 - l0) * frac) >> 8);
		r = r0 + ((((int32_t)r1 - r0) * frac) >> 8);
		acc[i * 2] += (int32_t)(l * v->gainL);
		acc[i * 2 + 1] += (int32_t)(r * v->gainR);
		v->posFrac += step;
		while(v->posFrac >= 0x10000)
		{ // advance the source position: an effect by index, music by consuming the ring
			v->posFrac -= 0x10000;
			if(v->isBgm)
			{
				if(v->ringCount == 0)
					break;
				v->ringRead++;
				if(v->ringRead >= v->ringFrames)
					v->ringRead = 0;
				v->ringCount--;
			}
			else
				v->pos++;
		}
	}
}

/* the audio thread's callback: every voice summed into `frames` stereo
 * frames of `out`, clamped to 16 bits, 4096 frames at a time */
// the external source (Wm_SetExternalMix), read on the mixer's thread under the lock
static WmExternalMix_t gExternalMix;
static void* gExternalMixUser;

void Wm_SetExternalMix(WmExternalMix_t fn, void* user)
{
	OS_LockEnter(gWmLock);
	gExternalMix = fn;
	gExternalMixUser = user;
	OS_LockLeave(gWmLock);
}

int Wm_MixRunning(void)
{
	return gAudioRunning;
}

void Mixer_Fill(void* user, int16_t* out, int frames)
{
	static int32_t acc[4096 * 2];
	int i, n;
	BGI_UNUSED(user);
	OS_LockEnter(gWmLock);
	while(frames > 0)
	{
		n = frames > 4096 ? 4096 : frames;
		memset(acc, 0, (size_t)n * 2 * sizeof(int32_t));
		for(i = 0; i < WM_BGM_CHANNELS; i++)
			Voice_Mix(&gBgmVoices[i], acc, n);
		for(i = 0; i < WM_SE_SLOTS; i++)
			Voice_Mix(&gSeVoices[i], acc, n);
		if(gExternalMix)
			gExternalMix(gExternalMixUser, acc, n);
		for(i = 0; i < n * 2; i++)
		{
			int32_t v = acc[i];
			out[i] = (int16_t)(v > 0x7fff ? 0x7fff : v < -0x8000 ? -0x8000
																 : v);
		}
		out += n * 2;
		frames -= n;
	}
	OS_LockLeave(gWmLock);
}
