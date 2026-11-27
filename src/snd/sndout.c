/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sndout.c - the engine's sound out (bgi/snd/sndout.h) and the calls a
 *            host makes for it (openbgi.h: OpenBGI_AudioRead,
 *            OpenBGI_RunAudio, OpenBGI_SetAudioLead)
 *
 * One writer, the engine's thread, and one reader, the host's audio
 * thread, share the ring through two counters: the writer publishes
 * `written` after the frames are in place, the reader publishes `taken`
 * after it copied them out, each with release order, and each reads the
 * other's with acquire order.  Nothing else is shared, so neither side
 * ever waits for the other.
 *
 * The ring is cleared when the engine starts again (Engine_Renew), which
 * the reader need not know of: a read that was under way then publishes a
 * `taken` that is ahead of the new `written`.  The reader takes nothing
 * while that is so, and the writer goes on from the reader's count.
 */
#include "bgi/snd/sndout.h"
#include "bgi/snd/wavemaster.h"
#include "bgi/core.h"

#include <stdio.h>
#include <string.h>

// the counters between the two threads: acquire loads, release stores
#if defined(__GNUC__)
#define LOAD(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#else
#define LOAD(p)     (*(p)) // (volatile: enough where stores are not reordered)
#define STORE(p, v) (*(p) = (v))
#endif

void SndOut_Start(OpenBGI_t* o)
{
	SndOut_t* s = &o->snd.out;
	s->lastMs = Clock_HostMs(o);
	s->carry = (uint32_t)((uint64_t)s->lastMs * SNDOUT_RATE % 1000u);
	s->noteMs = s->lastMs;
	s->noteMin = 0xffffffffu;
	s->noteMax = 0;
	s->running = 1;
}

void SndOut_Stop(OpenBGI_t* o)
{
	o->snd.out.running = 0;
}

// `n` frames of the mixer into the ring, a piece at a time; what does not fit is mixed all the same and dropped
static void Mix(OpenBGI_t* o, SndOut_t* s, uint32_t n)
{
	while(n > 0)
	{
		uint32_t k = n < SNDOUT_PIECE ? n : SNDOUT_PIECE, room, put, at;
		Wm_Mix(o, s->piece, (int)k);
		room = SNDOUT_RING - (s->written - LOAD(&s->taken));
		put = k < room ? k : room;
		at = s->written & (SNDOUT_RING - 1);
		if(at + put <= SNDOUT_RING)
			memcpy(s->ring + (size_t)at * 2, s->piece, (size_t)put * 4);
		else
		{ // across the end of the ring
			uint32_t first = SNDOUT_RING - at;
			memcpy(s->ring + (size_t)at * 2, s->piece, (size_t)first * 4);
			memcpy(s->ring, s->piece + (size_t)first * 2, (size_t)(put - first) * 4);
		}
		STORE(&s->written, s->written + put);
		s->dropped += k - put;
		n -= k;
	}
}

/* BGI_SND_DEBUG: once a second, how much of the sound was mixed and not
 * yet read after the runs of that second - what the ring adds to the time
 * between an instruction and its sound, before the output's own buffers */
static void Note(SndOut_t* s, uint32_t now)
{
	uint32_t unread = s->written - LOAD(&s->taken);
	if((int32_t)unread < 0)
		unread = 0;
	if(unread < s->noteMin)
		s->noteMin = unread;
	if(unread > s->noteMax)
		s->noteMax = unread;
	if(now - s->noteMs < 1000u)
		return;
	fprintf(stderr, "snd: out: %u .. %u ms mixed ahead of the output (lead %u ms), %u frames dropped\n", (unsigned)(s->noteMin * 1000u / SNDOUT_RATE),
		(unsigned)(s->noteMax * 1000u / SNDOUT_RATE), (unsigned)(s->lead * 1000u / SNDOUT_RATE), (unsigned)s->dropped);
	s->noteMs = now;
	s->noteMin = 0xffffffffu;
	s->noteMax = 0;
}

void SndOut_Run(OpenBGI_t* o)
{
	SndOut_t* s = &o->snd.out;
	uint32_t now, span, n;
	uint64_t f;
	if(!s->running)
		return;
	now = Clock_HostMs(o);
	span = now - s->lastMs;
	if(span > 0x7fffffffu)
		span = 0; // the clock went back
	else if(span > SNDOUT_MAX_SPAN)
		span = SNDOUT_MAX_SPAN;
	s->lastMs = now;
	if((int32_t)(s->written - LOAD(&s->taken)) < 0)
		STORE(&s->written, LOAD(&s->taken));     // (the reader is ahead: the ring was cleared under it)
	f = (uint64_t)span * SNDOUT_RATE + s->carry; // the frames the time on the grid holds
	n = (uint32_t)(f / 1000u);
	s->carry = (uint32_t)(f % 1000u);
	if(s->lead)
	{ // between L and 2 L frames not yet read
		uint32_t t = LOAD(&s->taken), unread;
		if((int32_t)(s->dropTo - t) > 0)
			t = s->dropTo; // (frames the reader is going to skip)
		unread = s->written - t;
		if(unread + n < s->lead)
			n = s->lead - unread; // the output reads faster than the clock runs (or this is the first run)
		else if(unread + n > 2 * s->lead)
			n = unread < 2 * s->lead ? 2 * s->lead - unread : 0; // slower
	}
	Mix(o, s, n);
	if(o->snd.sndDebug)
		Note(s, now);
}

// ---- the host's calls --------------------------------------------------------------------------

int OpenBGI_AudioRead(OpenBGI_t* o, int16_t* out, int frames)
{
	SndOut_t* s = &o->snd.out;
	uint32_t w = LOAD(&s->written), t = s->taken, d = LOAD(&s->dropTo), n, at;
	if(frames <= 0)
		return 0;
	if((int32_t)(d - t) > 0 && (int32_t)(w - d) >= 0)
		t = d;                            // what was mixed before the host began to play the sound
	n = (int32_t)(w - t) > 0 ? w - t : 0; // (nothing while the writer is behind: the ring was cleared)
	if(n > (uint32_t)frames)
		n = (uint32_t)frames;
	at = t & (SNDOUT_RING - 1);
	if(at + n <= SNDOUT_RING)
		memcpy(out, s->ring + (size_t)at * 2, (size_t)n * 4);
	else
	{
		uint32_t first = SNDOUT_RING - at;
		memcpy(out, s->ring + (size_t)at * 2, (size_t)first * 4);
		memcpy(out + (size_t)first * 2, s->ring, (size_t)(n - first) * 4);
	}
	STORE(&s->taken, t + n);
	if(n < (uint32_t)frames)
	{
		memset(out + (size_t)n * 2, 0, (size_t)((uint32_t)frames - n) * 4);
		if(s->running)
			STORE(&s->starved, s->starved + ((uint32_t)frames - n)); // (silence where sound was due)
	}
	return (int)n;
}

void SndOut_Stats(OpenBGI_t* o, uint32_t* ahead, uint32_t* dropped, uint32_t* starved)
{
	SndOut_t* s = &o->snd.out;
	uint32_t unread = s->written - LOAD(&s->taken);
	*ahead = s->running && (int32_t)unread > 0 ? unread : 0;
	*dropped = s->dropped;
	*starved = LOAD(&s->starved);
}

void OpenBGI_RunAudio(OpenBGI_t* o)
{
	if(Request_Pending(o))
		Wm_TimerPoll(o); // a box is up: the original's modal loop dispatches the fade timer
	SndOut_Run(o);
}

void OpenBGI_SetAudioLead(OpenBGI_t* o, int ms)
{
	SndOut_t* s = &o->snd.out;
	uint32_t lead;
	if(ms < 0)
		ms = 0;
	else if(ms > SNDOUT_MAX_LEAD)
		ms = SNDOUT_MAX_LEAD;
	lead = (uint32_t)ms * SNDOUT_RATE / 1000u;
	if(lead && !s->lead)
		STORE(&s->dropTo, s->written); // what was mixed for nobody is not played
	s->lead = lead;
}
