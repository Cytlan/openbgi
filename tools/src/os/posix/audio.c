/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * audio.c - the POSIX back end's sound output, CD audio and movies
 *           (inc/bgi/os.h)
 *
 * The sound library mixes in software and pulls 16-bit stereo frames
 * through OS_AudioStart()'s callback.  With ALSA available at build time
 * (BGI_HAVE_ALSA, see the Makefile) a thread feeds the default PCM device
 * in 20 ms periods; without it the same thread runs the mixer against the
 * clock and discards the frames, so that fades, loops and the "is it still
 * playing" bookkeeping keep their timing on a silent machine.  The
 * callback therefore always runs on this file's thread, never on the
 * engine's.
 *
 * CD audio through MCI, PlaySound and the DirectShow movie graph have no
 * counterpart here: every call reports failure, which the engine handles
 * (a CD that is not there, a movie that could not be rendered).
 */
#include "bgi/os.h"

#include <pthread.h>
#include <time.h>
#include <errno.h>

#ifdef BGI_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

#define AUDIO_PERIOD_FRAMES 882 // 20 ms at 44.1 kHz; the code below derives the period from the rate instead

static struct
{
	int running;        // between OS_AudioStart and OS_AudioStop
	int rate;           // frames per second
	OsAudioFill_t fill; // the mixer's callback and its argument
	void* user;
	pthread_t thread;  // the thread that pulls the frames
	volatile int stop; // set by OS_AudioStop: the thread ends after its current period
#ifdef BGI_HAVE_ALSA
	snd_pcm_t* pcm; // the playback device, NULL when it could not be opened (the silent path)
#endif
} gAudio;

// nanosleep that resumes after a signal
static void SleepNs(long ns)
{
	struct timespec ts;
	ts.tv_sec = ns / 1000000000L;
	ts.tv_nsec = ns % 1000000000L;
	while(nanosleep(&ts, &ts) == -1 && errno == EINTR)
	{
	}
}

#ifdef BGI_HAVE_ALSA
/* open the default playback device for interleaved 16-bit stereo at
 * `rate` (or the nearest rate it offers) with periods of about
 * `periodFrames` and a buffer of four of them; 0 when any step fails,
 * with the device closed again */
static int AlsaOpen(int rate, int periodFrames)
{
	snd_pcm_hw_params_t* hw;
	unsigned r = (unsigned)rate;
	snd_pcm_uframes_t period = (snd_pcm_uframes_t)periodFrames, buffer = period * 4;
	int ok;
	if(snd_pcm_open(&gAudio.pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0)
		return 0;
	// (snd_pcm_hw_params_alloca() expands to alloca(), which strict C99 does
	// not declare; the heap variant avoids it)
	if(snd_pcm_hw_params_malloc(&hw) < 0)
	{
		snd_pcm_close(gAudio.pcm);
		gAudio.pcm = NULL;
		return 0;
	}
	snd_pcm_hw_params_any(gAudio.pcm, hw);
	ok = snd_pcm_hw_params_set_access(gAudio.pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED) >= 0 &&
		snd_pcm_hw_params_set_format(gAudio.pcm, hw, SND_PCM_FORMAT_S16_LE) >= 0 &&
		snd_pcm_hw_params_set_channels(gAudio.pcm, hw, 2) >= 0 &&
		snd_pcm_hw_params_set_rate_near(gAudio.pcm, hw, &r, NULL) >= 0 &&
		snd_pcm_hw_params_set_period_size_near(gAudio.pcm, hw, &period, NULL) >= 0 &&
		snd_pcm_hw_params_set_buffer_size_near(gAudio.pcm, hw, &buffer) >= 0 &&
		snd_pcm_hw_params(gAudio.pcm, hw) >= 0;
	snd_pcm_hw_params_free(hw);
	if(!ok)
	{
		snd_pcm_close(gAudio.pcm);
		gAudio.pcm = NULL;
		return 0;
	}
	snd_pcm_prepare(gAudio.pcm);
	return 1;
}
#endif

/* the pull loop: one 20 ms period at a time, filled by the mixer and
 * written to the device (which blocks until it has room, pacing the
 * loop) or dropped after a 20 ms sleep when there is no device */
static void* AudioThread(void* arg)
{
	int period = gAudio.rate / 50; // 20 ms
	int16_t* buf = (int16_t*)malloc((size_t)period * 2 * sizeof(int16_t));
	if(!buf)
		return NULL;
	while(!gAudio.stop)
	{
		memset(buf, 0, (size_t)period * 2 * sizeof(int16_t));
		gAudio.fill(gAudio.user, buf, period);
#ifdef BGI_HAVE_ALSA
		if(gAudio.pcm)
		{
			snd_pcm_sframes_t n = snd_pcm_writei(gAudio.pcm, buf, (snd_pcm_uframes_t)period);
			if(n == -EPIPE)
				snd_pcm_prepare(gAudio.pcm); // underrun: start again
			else if(n < 0)
				snd_pcm_recover(gAudio.pcm, (int)n, 1);
			continue; // writei paces the loop
		}
#endif
		SleepNs(20L * 1000000L); // the silent clock
	}
	free(buf);
	return NULL;
}

// open the device (when built with ALSA) and start the pull thread; 0 when running already, for a bad argument or without a thread
int OS_AudioStart(int rate, OsAudioFill_t fill, void* user)
{
	if(gAudio.running || rate <= 0 || !fill)
		return 0;
	gAudio.rate = rate;
	gAudio.fill = fill;
	gAudio.user = user;
	gAudio.stop = 0;
#ifdef BGI_HAVE_ALSA
	AlsaOpen(rate, rate / 50); // a failure leaves pcm NULL: the silent path
#endif
	if(pthread_create(&gAudio.thread, NULL, AudioThread, NULL) != 0)
	{
#ifdef BGI_HAVE_ALSA
		if(gAudio.pcm)
			snd_pcm_close(gAudio.pcm);
		gAudio.pcm = NULL;
#endif
		return 0;
	}
	gAudio.running = 1;
	return 1;
}

// stop the thread (joined: no callback runs afterwards), play out what is queued and close the device
void OS_AudioStop(void)
{
	if(!gAudio.running)
		return;
	gAudio.stop = 1;
	pthread_join(gAudio.thread, NULL);
#ifdef BGI_HAVE_ALSA
	if(gAudio.pcm)
	{
		snd_pcm_drain(gAudio.pcm);
		snd_pcm_close(gAudio.pcm);
		gAudio.pcm = NULL;
	}
#endif
	gAudio.running = 0;
}

// always available: without ALSA, or when the device cannot be opened, the silent path still runs the mixer
int OS_AudioAvailable(void)
{
#ifdef BGI_HAVE_ALSA
	return 1;
#else
	return 1; // the silent device still "exists": the library initialises and runs
#endif
}

// ---- CD audio (MCI) ---------------------------------------------------------------
// No CD audio: every call fails, so the engine sees a drive without a CD.

uint32_t OS_MciOpenCdAudio(const char* element)
{
	return 0;
}

int OS_MciSetTimeFormatTmsf(uint32_t dev)
{
	return 0;
}

int OS_MciClose(uint32_t dev)
{
	return 0;
}

int OS_MciStatus(uint32_t dev, uint32_t item, uint32_t* out)
{
	return 0;
}

int OS_MciPlay(uint32_t dev, uint32_t from, uint32_t to, int notify)
{
	return 0;
}

int OS_MciStop(uint32_t dev)
{
	return 0;
}

// PlaySound: not accepted
int OS_PlaySoundFile(const char* path)
{
	return 0;
}

// ---- movies ----------------------------------------------------------------------
// No movie playback: the open fails, and the rest reports a graph that is not there.

int OS_MovieOpen(const char* path)
{
	return OS_MOVIE_FAILED;
}

int OS_MovieStart(int x, int y, int w, int h, int32_t volume, int32_t* lengthMs)
{
	return 0;
}

void OS_MovieStop(void)
{
}

int OS_MovieIsRunning(void)
{
	return 0;
}

void OS_MovieSetVolume(int32_t vol)
{
}
