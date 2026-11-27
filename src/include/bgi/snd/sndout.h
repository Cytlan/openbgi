/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sndout.h - the engine's sound out: the ring of mixed frames a host
 *            drains (OpenBGI_AudioRead, openbgi.h) and how much the
 *            mixer (Wm_Mix, bgi/snd/wavemaster.h) puts into it
 *            (src/snd/sndout.c); not part of the original engine
 *
 * The original hands its sound to DirectSound, which plays it on its own.
 * Here the engine runs the mixer itself, on its own thread: at the start
 * and the end of every frame and whenever the host calls
 * OpenBGI_RunAudio, for the time that passed on the host's clock, in
 * pieces of SNDOUT_PIECE frames.  What it mixes goes into a ring of
 * SNDOUT_RING frames, the only thing another thread touches: the host
 * takes the frames out of it from its audio device's thread.
 *
 * The frames follow the host's clock on a grid of its own: the frame for
 * millisecond t is floor(t * 44100 / 1000), so the mixer always lands on
 * the same frame for the same clock, however the time was cut into
 * calls.  How far ahead of the reader it mixes is the host's choice
 * (OpenBGI_SetAudioLead):
 *
 *   * 0 - nobody plays the sound (the default; tools, tests, the test
 *     host): the mixer runs for the time that passed and what does not
 *     fit into the ring is dropped, so fades, loops and the scripts' "is
 *     it still playing" keep their timing.
 *   * a lead L > 0 - the host plays it: after a run the ring holds at
 *     least L frames that were not read and at most 2 L.  Within that the
 *     mixer runs for the time that passed; an output that reads faster
 *     than the clock runs gets more, one that reads slower gets less, so
 *     the voices move at the output's pace and the ring neither runs dry
 *     nor fills up.
 */
#ifndef BGI_SND_SNDOUT_H_
#define BGI_SND_SNDOUT_H_

#include "bgi/common.h"

#define SNDOUT_RATE     44100 // frames per second (OPENBGI_AUDIO_RATE)
#define SNDOUT_RING     32768 // frames the ring holds, a power of two (0.74 s)
#define SNDOUT_PIECE    1024  // frames the mixer runs for at a time (about the 20 ms a sound device asks for)
#define SNDOUT_MAX_LEAD 300   // ms, the longest lead OpenBGI_SetAudioLead takes (2 L fits the ring)
#define SNDOUT_MAX_SPAN 60000 // ms, the most time one run makes up for (a clock that jumped)

typedef struct SndOut
{
	int16_t ring[SNDOUT_RING * 2];   // interleaved stereo; frame i at i % SNDOUT_RING
	int16_t piece[SNDOUT_PIECE * 2]; // the mixer's output before it goes into the ring
	volatile uint32_t written;       // frames put into the ring so far (wrapping); written by the engine's thread
	volatile uint32_t taken;         // frames read out of it so far (wrapping); written by the reader
	volatile uint32_t dropTo;        // `written` when a lead was set after none: the reader skips what lies before
	uint32_t lead;                   // frames (OpenBGI_SetAudioLead); 0 when nobody plays the sound
	volatile int running;            // the mixer runs (SndOut_Start .. SndOut_Stop)
	uint32_t lastMs;                 // the host's clock at the last run
	uint32_t carry;                  // lastMs * SNDOUT_RATE modulo 1000: the frames' grid
	uint32_t dropped;                // frames mixed that did not fit into the ring (for the tests, and the performance display)
	volatile uint32_t starved;       // frames the reader asked for while the mixer ran and the ring did not have; written by the reader
	uint32_t noteMs;                 // BGI_SND_DEBUG: the clock at the last line about the ring,
	uint32_t noteMin, noteMax;       // .. and the fewest and the most frames unread after a run since
	/* The reader's own mixing: a read that finds the ring short mixes the
	 * missing frames itself, whatever the engine's thread is doing, so the
	 * sound does not wait for a load, a picture or a slow frame.  The
	 * voices and their streams are therefore behind a lock: `gate` holds
	 * one of the host's (OpenBGI_Host_t.threads; NULL: there is none, and
	 * the reader only reads), and whoever mixes or touches a voice holds
	 * it - the engine's thread between SndOut_Enter and SndOut_Leave. */
	void* volatile gate; // (a block of its own, which stays when the engine starts again: the reader may be at it)
	int held;            // the engine's thread: SndOut_Enter calls not left yet
	int reader;          // the reader is mixing (under the lock): a movie's sound stays out, since its decoder is the engine's thread's alone
	uint32_t extra;      // frames the reader mixed since the engine's last run: the time they stand for has passed
	volatile uint32_t helped; // .. and all of them so far (the performance display, the tests)
} SndOut_t;

void SndOut_Start(OpenBGI_t* o); // the mixer runs from the host's clock now on (Wm_Init)
void SndOut_Stop(OpenBGI_t* o);  // it runs no more: the ring keeps what it has, then the host reads silence (Wm_Shutdown)
void SndOut_Run(OpenBGI_t* o);   // run the mixer up to the host's clock now (OpenBGI_RunFrame, OpenBGI_RunAudio)
/* The engine's thread is about to touch a voice, a stream a voice plays
 * or the ring, and is done with it: the host's reader (OpenBGI_AudioRead)
 * mixes only outside the two.  The calls nest.  Whatever takes long - a
 * file read, a sound decoded whole - is done before SndOut_Enter, since a
 * reader that needs frames waits for SndOut_Leave.  LeaveAll: whatever is
 * entered (an error leaves the work by a jump). */
void SndOut_Enter(OpenBGI_t* o);
void SndOut_Leave(OpenBGI_t* o);
void SndOut_LeaveAll(OpenBGI_t* o);
/* The engine starts again (Engine_Renew): Renew before its state is
 * cleared, Renewed with Renew's result once the host's tables are back.
 * The reader stays out between the two. */
void* SndOut_Renew(OpenBGI_t* o);
void SndOut_Renewed(OpenBGI_t* o, void* gate);
void SndOut_Free(OpenBGI_t* o); // the lock goes (the engine is deleted)
// for the performance display: the frames mixed and not yet read, those lost to a full ring so far, those asked for in vain so far
void SndOut_Stats(OpenBGI_t* o, uint32_t* ahead, uint32_t* dropped, uint32_t* starved);
uint32_t SndOut_Helped(OpenBGI_t* o); // the frames the reader mixed itself so far

#endif // BGI_SND_SNDOUT_H_
