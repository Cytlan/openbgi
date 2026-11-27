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
 * other's with acquire order.  While the ring has what a read asks for,
 * nothing else is shared and neither side waits for the other.
 *
 * A read that finds the ring short mixes the missing frames itself, under
 * the lock that guards the voices (SndOut_Enter): the sound then goes on
 * whatever holds the engine's thread, and that thread makes up for it at
 * its next run by mixing that much less.
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

/* The lock between the engine's thread and the reader, with what it
 * takes to use it: a copy of the host's table and its `user`, so that the
 * reader needs nothing of the engine's state before it holds the lock
 * (that state is cleared when the engine starts again). */
typedef struct SndGate
{
	OpenBGI_Threads_t t;
	void* user;
	void* lock;
} SndGate_t;

static SndGate_t* GateNew(OpenBGI_t* o)
{
	const OpenBGI_Threads_t* t = o->host.threads;
	SndGate_t* g;
	if(!t || !t->lockNew || !t->lockFree || !t->lock || !t->unlock)
		return NULL;
	g = (SndGate_t*)BGI_Calloc(sizeof *g);
	memcpy(&g->t, t, t->size < sizeof g->t ? t->size : sizeof g->t);
	g->user = o->host.user;
	g->lock = t->lockNew(o->host.user);
	if(!g->lock)
	{
		BGI_Free(g);
		g = NULL;
	}
	return g;
}

void SndOut_Enter(OpenBGI_t* o)
{
	SndOut_t* s = &o->snd.out;
	SndGate_t* g = (SndGate_t*)s->gate;
	if(s->held++ == 0 && g)
		g->t.lock(g->user, g->lock);
}

void SndOut_Leave(OpenBGI_t* o)
{
	SndOut_t* s = &o->snd.out;
	SndGate_t* g = (SndGate_t*)s->gate;
	if(s->held > 0 && --s->held == 0 && g)
		g->t.unlock(g->user, g->lock);
}

void SndOut_LeaveAll(OpenBGI_t* o)
{
	SndOut_t* s = &o->snd.out;
	if(s->held > 0)
	{
		s->held = 1;
		SndOut_Leave(o);
	}
}

void* SndOut_Renew(OpenBGI_t* o)
{
	SndOut_t* s = &o->snd.out;
	SndGate_t* g = (SndGate_t*)s->gate;
	SndOut_LeaveAll(o);
	if(g)
	{
		g->t.lock(g->user, g->lock); // (a reader that is mixing finishes first; the next finds no gate, or waits and finds the mixer stopped)
		STORE(&s->gate, NULL);
	}
	return g;
}

void SndOut_Renewed(OpenBGI_t* o, void* gate)
{
	SndOut_t* s = &o->snd.out;
	SndGate_t* g = (SndGate_t*)gate;
	if(g)
	{
		STORE(&s->gate, gate);
		g->t.unlock(g->user, g->lock);
	}
}

void SndOut_Free(OpenBGI_t* o)
{
	SndOut_t* s = &o->snd.out;
	SndGate_t* g = (SndGate_t*)s->gate;
	if(!g)
		return;
	SndOut_LeaveAll(o);
	STORE(&s->gate, NULL);
	g->t.lockFree(g->user, g->lock);
	BGI_Free(g);
}

void SndOut_Start(OpenBGI_t* o)
{
	SndOut_t* s = &o->snd.out;
	if(!s->gate)
	{
		SndGate_t* g = GateNew(o);
		if(g && s->held > 0)
			g->t.lock(g->user, g->lock); // (the caller entered before there was a lock, and leaves with one)
		STORE(&s->gate, g);
	}
	s->lastMs = Clock_HostMs(o);
	s->carry = (uint32_t)((uint64_t)s->lastMs * SNDOUT_RATE % 1000u);
	s->noteMs = s->lastMs;
	s->noteMin = 0xffffffffu;
	s->noteMax = 0;
	STORE(&s->running, 1);
}

void SndOut_Stop(OpenBGI_t* o)
{
	STORE(&o->snd.out.running, 0);
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
	f = (uint64_t)span * SNDOUT_RATE + s->carry; // the frames the time on the grid holds
	n = (uint32_t)(f / 1000u);
	s->carry = (uint32_t)(f % 1000u);
	Perf_PartBegin(o, PERF_PART_SOUND);
	SndOut_Enter(o);
	if((int32_t)(s->written - LOAD(&s->taken)) < 0)
		STORE(&s->written, LOAD(&s->taken)); // (the reader is ahead: the ring was cleared under it)
	if(s->extra)
	{ // what the reader mixed while this thread was away is that much of the time
		n = n > s->extra ? n - s->extra : 0;
		s->extra = 0;
	}
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
	SndOut_Leave(o);
	Perf_PartEnd(o, PERF_PART_SOUND);
	if(o->snd.sndDebug)
		Note(s, now);
}

// ---- the host's calls --------------------------------------------------------------------------

int OpenBGI_AudioRead(OpenBGI_t* o, int16_t* out, int frames)
{
	SndOut_t* s = &o->snd.out;
	SndGate_t* g = (SndGate_t*)LOAD(&s->gate);
	uint32_t w = LOAD(&s->written), t = s->taken, d = LOAD(&s->dropTo), n, at;
	if(frames <= 0)
		return 0;
	if(g && LOAD(&s->running) && ((int32_t)(w - t) < 0 || w - t < (uint32_t)frames))
	{ // the engine's thread is busy and the ring runs dry: the missing frames are mixed here
		g->t.lock(g->user, g->lock);
		if(s->running && LOAD(&s->gate) == g)
		{
			uint32_t have = (int32_t)(s->written - t) > 0 ? s->written - t : 0, need = have < (uint32_t)frames ? (uint32_t)frames - have : 0;
			if((int32_t)(s->written - t) < 0)
				STORE(&s->written, t); // (the ring was cleared under the reader)
			s->reader = 1;
			Mix(o, s, need);
			s->reader = 0;
			s->extra += need;
			STORE(&s->helped, s->helped + need);
		}
		g->t.unlock(g->user, g->lock);
		w = LOAD(&s->written);
	}
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
	uint32_t unread = LOAD(&s->written) - LOAD(&s->taken);
	*ahead = s->running && (int32_t)unread > 0 ? unread : 0;
	*dropped = s->dropped;
	*starved = LOAD(&s->starved);
}

uint32_t SndOut_Helped(OpenBGI_t* o)
{
	return LOAD(&o->snd.out.helped);
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
	SndOut_Enter(o);
	if(lead && !s->lead)
		STORE(&s->dropTo, s->written); // what was mixed for nobody is not played
	s->lead = lead;
	SndOut_Leave(o);
}
