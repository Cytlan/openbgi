/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * audio.c - the POSIX back end's sound output (src/host/os.h)
 *
 * The host's sound output pulls 16-bit stereo frames through
 * OS_AudioStart()'s callback, which reads the engine's sound (os.h).
 * With ALSA available at build time (BGI_HAVE_ALSA, see the Makefile) a
 * thread feeds the default PCM device in 20 ms periods; without it the
 * same thread pulls the frames at the clock's pace and discards them, so
 * that the engine, which follows the pace of the reads, keeps its fades,
 * loops and "is it still playing" on time on a silent machine.  On Haiku
 * (BGI_HAIKU) a BSoundPlayer of the Media Kit pulls the frames instead
 * (src/host/haiku/haiku.cpp), on macOS (BGI_MACOS) an AudioQueue of
 * AudioToolbox, and the silent thread only runs when the player could not
 * be started.  Android has the silent thread alone until its own back end
 * (AAudio) exists.  The callback always runs on a thread of the sound
 * output, never on the engine's.  (The test host starts no output: it
 * reads the engine's sound itself, src/host/headless/main.c.)
 *
 * There is no CD audio and no movie player of the system's here: the
 * movies go to the movie decoder (src/host/os_ffmpeg.c).
 */
#include "host/os.h"

#include <pthread.h>
#include <time.h>
#include <errno.h>

#ifdef BGI_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif
#ifdef BGI_HAIKU
#include "host/os_haiku.h"
#endif
#ifdef BGI_MACOS
#include <AudioToolbox/AudioToolbox.h>
#endif


static struct
{
	int running;        // between OS_AudioStart and OS_AudioStop
	int rate;           // frames per second
	OsAudioFill_t fill; // the host's callback and its argument
	void* user;
	pthread_t thread;  // the thread that pulls the frames
	volatile int stop; // set by OS_AudioStop: the thread ends after its current period
	int player;        // Haiku, macOS: the system's player pulls the frames, no thread runs
#ifdef BGI_MACOS
	AudioQueueRef queue;            // the output queue while `player` is set
	AudioQueueBufferRef buffers[3]; // its buffers, 20 ms each, refilled as they come back
#endif
#ifdef BGI_HAVE_ALSA
	snd_pcm_t* pcm; // the playback device, NULL when it could not be opened (the silent path)
	int debug;      // BGI_SND_DEBUG: what the device granted, and once a second what it holds
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
 * `rate` (ALSA resamples for a device that runs at another) with periods of about
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
		snd_pcm_hw_params_set_rate_resample(gAudio.pcm, hw, 1) >= 0 &&
		snd_pcm_hw_params_set_rate(gAudio.pcm, hw, r, 0) >= 0 &&
		snd_pcm_hw_params_set_period_size_near(gAudio.pcm, hw, &period, NULL) >= 0 &&
		snd_pcm_hw_params_set_buffer_size_near(gAudio.pcm, hw, &buffer) >= 0 &&
		snd_pcm_hw_params(gAudio.pcm, hw) >= 0;
	gAudio.debug = getenv("BGI_SND_DEBUG") != NULL;
	if(ok && gAudio.debug)
	{ // (a device may grant other sizes than it was asked for: a sound server's, dmix's)
		snd_pcm_hw_params_get_period_size(hw, &period, NULL);
		snd_pcm_hw_params_get_buffer_size(hw, &buffer);
		fprintf(stderr, "snd: device: ALSA \"default\" (%s): periods of %lu frames (%lu ms), a buffer of %lu (%lu ms); asked for %d and %d\n",
			snd_pcm_type_name(snd_pcm_type(gAudio.pcm)), (unsigned long)period, (unsigned long)(period * 1000u / r), (unsigned long)buffer,
			(unsigned long)(buffer * 1000u / r), periodFrames, periodFrames * 4);
	}
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

/* the pull loop: one 20 ms period at a time, filled by the callback and
 * written to the device (which blocks until it has room, pacing the
 * loop) or dropped after a 20 ms sleep when there is no device */
static void* AudioThread(void* arg)
{
	int period = gAudio.rate / 50; // 20 ms
	int16_t* buf = (int16_t*)malloc((size_t)period * 2 * sizeof(int16_t));
#ifdef BGI_HAVE_ALSA
	int count = 0, shortReads = 0;
#endif
	if(!buf)
		return NULL;
	while(!gAudio.stop)
	{
#ifdef BGI_HAVE_ALSA
		int got = gAudio.fill(gAudio.user, buf, period);
		if(gAudio.pcm)
		{
			snd_pcm_sframes_t n;
			if(gAudio.debug)
			{ // once a second: what waits in the device, and how often the engine had less than a period
				snd_pcm_sframes_t held = 0;
				shortReads += got < period;
				if(++count >= 50)
				{
					if(snd_pcm_delay(gAudio.pcm, &held) < 0)
						held = 0;
					fprintf(stderr, "snd: device: %ld ms waiting in it, %d of %d periods not full\n", (long)(held * 1000 / gAudio.rate), shortReads, count);
					count = shortReads = 0;
				}
			}
			n = snd_pcm_writei(gAudio.pcm, buf, (snd_pcm_uframes_t)period);
			if(n == -EPIPE)
				snd_pcm_prepare(gAudio.pcm); // underrun: start again
			else if(n < 0)
				snd_pcm_recover(gAudio.pcm, (int)n, 1);
			continue; // writei paces the loop
		}
#else
		gAudio.fill(gAudio.user, buf, period);
#endif
		SleepNs(20L * 1000000L); // the silent clock
	}
	free(buf);
	return NULL;
}

#ifdef BGI_MACOS
// a buffer the queue has played: filled again and queued again, until the stop
static void QueueRefill(void* user, AudioQueueRef q, AudioQueueBufferRef b)
{
	BGI_UNUSED(user);
	if(gAudio.stop)
		return;
	gAudio.fill(gAudio.user, (int16_t*)b->mAudioData, (int)(b->mAudioDataBytesCapacity / 4));
	b->mAudioDataByteSize = b->mAudioDataBytesCapacity;
	AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

/* an output queue for interleaved 16-bit stereo at `rate`, its callbacks
 * on the queue's own thread, primed with three filled buffers and started;
 * 0 when any step fails (nothing is left open) */
static int MacQueueOpen(int rate)
{
	AudioStreamBasicDescription f;
	int i;
	memset(&f, 0, sizeof f);
	f.mSampleRate = rate;
	f.mFormatID = kAudioFormatLinearPCM;
	f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
	f.mBytesPerPacket = 4;
	f.mFramesPerPacket = 1;
	f.mBytesPerFrame = 4;
	f.mChannelsPerFrame = 2;
	f.mBitsPerChannel = 16;
	if(AudioQueueNewOutput(&f, QueueRefill, NULL, NULL, NULL, 0, &gAudio.queue) != noErr)
		return 0;
	for(i = 0; i < 3; i++)
	{
		if(AudioQueueAllocateBuffer(gAudio.queue, (UInt32)(rate / 50) * 4, &gAudio.buffers[i]) != noErr)
		{
			AudioQueueDispose(gAudio.queue, true);
			return 0;
		}
		QueueRefill(NULL, gAudio.queue, gAudio.buffers[i]);
	}
	if(AudioQueueStart(gAudio.queue, NULL) != noErr)
	{
		AudioQueueDispose(gAudio.queue, true);
		return 0;
	}
	return 1;
}

// stop at once (no callback runs afterwards) and free the queue with its buffers
static void MacQueueClose(void)
{
	AudioQueueStop(gAudio.queue, true);
	AudioQueueDispose(gAudio.queue, true);
	gAudio.queue = NULL;
}
#endif

// start the system's player (Haiku, macOS), else open the device (when built with ALSA) and start the pull thread; 0 when running already, for a bad argument or without a thread
int OS_AudioStart(int rate, OsAudioFill_t fill, void* user)
{
	if(gAudio.running || rate <= 0 || !fill)
		return 0;
	gAudio.rate = rate;
	gAudio.fill = fill;
	gAudio.user = user;
	gAudio.stop = 0;
#if defined(BGI_HAIKU)
	gAudio.player = OsHaiku_AudioOpen(rate, fill, user); // else the silent path
#elif defined(BGI_MACOS)
	gAudio.player = MacQueueOpen(rate); // else the silent path
#endif
	if(gAudio.player)
	{
		gAudio.running = 1;
		return 1;
	}
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

// stop the player or the thread (joined: no callback runs afterwards), play out what is queued and close the device
void OS_AudioStop(void)
{
	if(!gAudio.running)
		return;
	gAudio.stop = 1;
	if(gAudio.player)
	{
#if defined(BGI_HAIKU)
		OsHaiku_AudioClose();
#elif defined(BGI_MACOS)
		MacQueueClose();
#endif
		gAudio.player = 0;
		gAudio.running = 0;
		return;
	}
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

// always available: without a sound device, or when it cannot be opened, the silent path still reads the engine's sound
int OS_AudioAvailable(void)
{
	return 1;
}
