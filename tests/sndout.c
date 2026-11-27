/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/sndout.c - unit test of the engine's sound out (src/snd/sndout.c,
 *                  bgi/snd/sndout.h; the host's side in openbgi.h:
 *                  OpenBGI_AudioRead, OpenBGI_RunAudio,
 *                  OpenBGI_SetAudioLead) and of the wave player behind
 *                  "A0 C0" (Wm_PlayWave); built as test_sndout by
 *                  `make test`
 *
 * The engine is made with a host whose clock the test sets, so every
 * amount is exact.  The groups of tests, in the order they run:
 *
 *   grid    - nobody reading (lead 0): the mixer runs for the time that
 *             passed, on the clock's grid of frames however the time is
 *             cut up; a full ring drops what does not fit and keeps the
 *             time
 *   lead    - a reader at the clock's pace, one faster and one slower:
 *             between L and 2 L frames unread after every run, nothing
 *             dropped; what was mixed for nobody is skipped once a lead
 *             is set
 *   restart - the engine started again under a host that goes on reading
 *             (the launcher starting a game): the lead the host set still
 *             holds, so a frame that takes long leaves no more than 2 L
 *             unread behind it; a read that ended after the ring was
 *             cleared does not stop the mixer
 *   wave    - a 16-bit stereo and an 8-bit mono RIFF WAVE come out sample
 *             for sample (level 0, the same rate); compressed and broken
 *             files are refused; a new wave cuts off the one before
 *   threads - a reader on a thread of its own and the engine on the real
 *             clock: what comes out is the wave without a frame lost or
 *             torn, gaps of silence aside
 */
#include "bgi/core.h"
#include "bgi/snd/wavemaster.h"
#include "bgi/snd/sndout.h"
#include "bgi/engine.h"
#include "host/os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"

// ---- the host: a clock the test sets ----------------------------------------------------------

static uint32_t gMs;  // the clock (ms)
static int gRealTime; // the threads group: the OS layer's clock instead

static uint32_t TestTicksMs(void* user)
{
	BGI_UNUSED(user);
	return gRealTime ? OS_TicksMs() : gMs;
}

static int TestPerfNs(void* user, int64_t* out)
{
	BGI_UNUSED(user);
	*out = (int64_t)TestTicksMs(NULL) * 1000000;
	return 1;
}

static void TestLocalTime(void* user, OpenBGI_DateTime_t* out)
{
	BGI_UNUSED(user);
	memset(out, 0, sizeof *out);
	out->year = 2026;
	out->month = 1;
	out->day = 1;
}

static const OpenBGI_Host_t kTestHost = {.size = sizeof(OpenBGI_Host_t), .api = OPENBGI_API_VERSION, .ticksMs = TestTicksMs, .perfNs = TestPerfNs, .localTime = TestLocalTime, .caps = OPENBGI_CAP_DIALOGS}; // (no fonts, no files)

static uint32_t Grid(uint32_t ms) // floor(ms * 44100 / 1000)
{
	return (uint32_t)((uint64_t)ms * SNDOUT_RATE / 1000u);
}

static uint32_t Unread(OpenBGI_t* o)
{
	return o->snd.out.written - o->snd.out.taken;
}

// read and drop up to `frames` frames; how many there were
static uint32_t Drain(OpenBGI_t* o, uint32_t frames)
{
	static int16_t buf[SNDOUT_RING * 2];
	uint32_t got = 0;
	while(got < frames)
	{
		int n = OpenBGI_AudioRead(o, buf, (int)(frames - got > SNDOUT_RING ? SNDOUT_RING : frames - got));
		if(n <= 0)
			break;
		got += (uint32_t)n;
	}
	return got;
}

// a RIFF WAVE file of `frames` frames (the samples at `pcm`, `bits` 8 or 16), format tag `tag`; *size its bytes
static uint8_t* MakeWave(const void* pcm, uint32_t frames, int channels, int bits, uint32_t rate, int tag, uint32_t* size)
{
	uint32_t data = frames * (uint32_t)channels * (uint32_t)(bits / 8), align = (uint32_t)channels * (uint32_t)(bits / 8);
	uint8_t* w = (uint8_t*)calloc(1, 44 + data + 1);
	uint32_t v;
	memcpy(w, "RIFF", 4);
	v = 36 + data;
	memcpy(w + 4, &v, 4);
	memcpy(w + 8, "WAVEfmt ", 8);
	v = 16;
	memcpy(w + 16, &v, 4);
	w[20] = (uint8_t)tag;
	w[22] = (uint8_t)channels;
	memcpy(w + 24, &rate, 4);
	v = rate * align;
	memcpy(w + 28, &v, 4);
	w[32] = (uint8_t)align;
	w[34] = (uint8_t)bits;
	memcpy(w + 36, "data", 4);
	memcpy(w + 40, &data, 4);
	memcpy(w + 44, pcm, data);
	*size = 44 + data;
	return w;
}

// ---- the groups -----------------------------------------------------------------------------

static void TestGrid(void)
{
	OpenBGI_t* o = OpenBGI_New(&kTestHost);
	uint32_t total, t;
	printf("grid\n");
	gMs = 1001; // a millisecond whose frames do not end on a whole one
	CHECK(Wm_Init(o) == 0 && Wm_StartTimer(o) == 0);
	CHECK(o->snd.out.lead == 0 && Unread(o) == 0);
	for(t = 1001; t <= 1500; t += 7)
	{ // the time in odd steps: always the grid's frames since the start
		gMs = t;
		OpenBGI_RunAudio(o);
		CHECK(o->snd.out.written == Grid(t) - Grid(1001));
	}
	gMs = 1500;
	OpenBGI_RunAudio(o); // no time: nothing
	total = o->snd.out.written;
	CHECK(total == Grid(1500) - Grid(1001));
	CHECK(Drain(o, total) == total && Unread(o) == 0);
	// nobody reads: the ring fills, the rest is mixed and dropped, the time kept
	gMs = 1500 + 2000;
	OpenBGI_RunAudio(o);
	CHECK(Unread(o) == SNDOUT_RING);
	CHECK(o->snd.out.dropped == Grid(3500) - Grid(1500) - SNDOUT_RING);
	gMs = 3500 + 10;
	OpenBGI_RunAudio(o);
	CHECK(o->snd.out.dropped == Grid(3510) - Grid(1500) - SNDOUT_RING);
	// a clock that goes back mixes nothing; then it goes on from there
	gMs = 3000;
	OpenBGI_RunAudio(o);
	CHECK(o->snd.out.dropped == Grid(3510) - Grid(1500) - SNDOUT_RING);
	Wm_Shutdown(o);
	gMs = 4000;
	OpenBGI_RunAudio(o); // stopped: nothing
	CHECK(o->snd.out.dropped == Grid(3510) - Grid(1500) - SNDOUT_RING);
	OpenBGI_Delete(o);
}

/* `steps` steps of 20 ms with a reader that takes `perStep` frames after
 * each: the ring stays between the lead and twice the lead, nothing is
 * dropped */
static void LeadRun(OpenBGI_t* o, int steps, uint32_t perStep)
{
	uint32_t lead = o->snd.out.lead;
	int i, inside = 1;
	for(i = 0; i < steps; i++)
	{
		gMs += 20;
		OpenBGI_RunAudio(o);
		inside &= Unread(o) >= lead && Unread(o) <= 2 * lead;
		Drain(o, perStep);
	}
	CHECK(inside);
	CHECK(o->snd.out.dropped == 0);
}

static void TestLead(void)
{
	OpenBGI_t* o = OpenBGI_New(&kTestHost);
	uint32_t before;
	printf("lead\n");
	gMs = 0;
	CHECK(Wm_Init(o) == 0 && Wm_StartTimer(o) == 0);
	gMs = 300;
	OpenBGI_RunAudio(o); // mixed for nobody: to be skipped
	before = o->snd.out.written;
	CHECK(before == Grid(300));
	OpenBGI_SetAudioLead(o, 60);
	CHECK(o->snd.out.lead == Grid(60));
	gMs += 1;
	OpenBGI_RunAudio(o); // the lead at once
	CHECK(o->snd.out.written - before == o->snd.out.lead);
	CHECK(Drain(o, SNDOUT_RING) == o->snd.out.lead); // the stale frames were skipped
	LeadRun(o, 200, 882);                            // the clock's pace
	LeadRun(o, 200, 1000);                           // a faster output: it gets more than the clock says
	before = o->snd.out.written;
	LeadRun(o, 200, 700); // a slower one: held back
	CHECK(o->snd.out.written - before < Grid(200 * 20));
	// a reader that stops: the voices stop with it, nothing is dropped
	gMs += 1000;
	OpenBGI_RunAudio(o);
	CHECK(Unread(o) <= 2 * o->snd.out.lead && o->snd.out.dropped == 0);
	// the lead's bounds
	OpenBGI_SetAudioLead(o, 100000);
	CHECK(o->snd.out.lead == Grid(SNDOUT_MAX_LEAD));
	OpenBGI_SetAudioLead(o, -5);
	CHECK(o->snd.out.lead == 0);
	Wm_Shutdown(o);
	OpenBGI_Delete(o);
}

static void TestRestart(void)
{
	OpenBGI_t* o = OpenBGI_New(&kTestHost);
	static int16_t buf[882 * 2];
	uint32_t lead, worst = 0;
	int i;
	printf("restart\n");
	gMs = 0;
	CHECK(Wm_Init(o) == 0 && Wm_StartTimer(o) == 0);
	OpenBGI_SetAudioLead(o, 60);
	lead = o->snd.out.lead;
	LeadRun(o, 50, 882);
	// the run ends and the next one begins on the same engine, as when the launcher starts a game
	Wm_Shutdown(o);
	Engine_Renew(o);
	CHECK(o->snd.out.lead == lead); // what the host said of its output still holds
	CHECK(Wm_Init(o) == 0 && Wm_StartTimer(o) == 0);
	/* frames of 20 ms, and every eighth one takes 400 ms (a picture is
	 * loaded): the output read silence meanwhile, and the sound of those
	 * 400 ms is not queued behind it */
	for(i = 0; i < 40; i++)
	{
		int slow = i % 8 == 7;
		gMs += slow ? 400 : 20;
		Drain(o, slow ? 882 * 20 : 882);
		OpenBGI_RunAudio(o);
		if(Unread(o) > worst)
			worst = Unread(o);
	}
	CHECK(worst <= 2 * lead);
	CHECK(o->snd.out.dropped == 0);
	/* a read that was under way when the ring was cleared ends after it:
	 * the reader's count is ahead of the writer's.  Nothing is read until
	 * the mixer has caught up, which it does at its next run. */
	Drain(o, SNDOUT_RING);
	o->snd.out.taken = o->snd.out.written + 882;
	CHECK(OpenBGI_AudioRead(o, buf, 882) == 0);
	gMs += 20;
	OpenBGI_RunAudio(o);
	CHECK(Unread(o) >= lead && Unread(o) <= 2 * lead);
	LeadRun(o, 50, 882);
	Wm_Shutdown(o);
	OpenBGI_Delete(o);
}

static void TestWave(void)
{
	OpenBGI_t* o = OpenBGI_New(&kTestHost);
	enum
	{
		N = 3000
	};
	static int16_t pcm[N * 2], out[N * 2];
	static uint8_t pcm8[N];
	uint8_t *w, *w8, *bad;
	uint32_t size, size8, sizeBad, i;
	int same = 1;
	printf("wave\n");
	for(i = 0; i < N; i++)
	{
		pcm[i * 2] = (int16_t)(i * 7 - 9000);
		pcm[i * 2 + 1] = (int16_t)(12000 - i * 5);
		pcm8[i] = (uint8_t)(i * 3);
	}
	w = MakeWave(pcm, N, 2, 16, 44100, 1, &size);
	w8 = MakeWave(pcm8, N, 1, 8, 44100, 1, &size8);
	bad = MakeWave(pcm, N, 2, 16, 44100, 2, &sizeBad); // MS ADPCM's tag
	gMs = 0;
	CHECK(Wm_PlayWave(o, w, size) == 0); // the library is not running
	CHECK(Wm_Init(o) == 0 && Wm_StartTimer(o) == 0);
	CHECK(Wm_PlayWave(o, bad, sizeBad) == 0);
	CHECK(Wm_PlayWave(o, "RIFF", 4) == 0);
	CHECK(Wm_PlayWave(o, w, 40) == 0); // cut off before its data
	// 16-bit stereo: the samples as they are
	CHECK(Wm_PlayWave(o, w, size) == 1);
	gMs = 100; // 4410 frames: the wave and silence after it
	OpenBGI_RunAudio(o);
	CHECK(OpenBGI_AudioRead(o, out, N) == N);
	for(i = 0; i < N * 2; i++)
		same &= out[i] == pcm[i];
	CHECK(same);
	CHECK(OpenBGI_AudioRead(o, out, 10) == 10 && out[0] == 0 && out[19] == 0);
	CHECK(!o->snd.waveVoice.playing);
	Drain(o, SNDOUT_RING);
	// 8-bit mono: centred, widened, on both sides
	CHECK(Wm_PlayWave(o, w8, size8) == 1);
	gMs += 100;
	OpenBGI_RunAudio(o);
	CHECK(OpenBGI_AudioRead(o, out, N) == N);
	for(i = 0, same = 1; i < N; i++)
		same &= out[i * 2] == (int16_t)((pcm8[i] - 128) * 256) && out[i * 2 + 1] == out[i * 2];
	CHECK(same);
	Drain(o, SNDOUT_RING);
	// a new one cuts the one before off
	CHECK(Wm_PlayWave(o, w, size) == 1);
	gMs += 10;
	OpenBGI_RunAudio(o);
	CHECK(Wm_PlayWave(o, w8, size8) == 1);
	gMs += 10;
	OpenBGI_RunAudio(o);
	Drain(o, Grid(10)); // the first wave's beginning
	CHECK(OpenBGI_AudioRead(o, out, 1) == 1 && out[0] == (int16_t)((pcm8[0] - 128) * 256));
	Wm_Shutdown(o);
	free(w);
	free(w8);
	free(bad);
	OpenBGI_Delete(o);
}

// ---- threads ------------------------------------------------------------------------------------

#define RAMP_FRAMES (44100 * 3)

typedef struct Reader
{
	OpenBGI_t* o;
	volatile int stop;
	uint32_t heard;  // frames of the ramp heard
	uint32_t broken; // frames that did not follow the one before
} Reader_t;

// the reader: 441 frames every 5 ms, as a sound device would take them
static void ReaderThread(void* arg)
{
	Reader_t* r = (Reader_t*)arg;
	int16_t buf[441 * 2];
	int32_t last = -1;
	while(!r->stop)
	{
		int n = OpenBGI_AudioRead(r->o, buf, 441), i;
		for(i = 0; i < n; i++)
		{
			int32_t v = buf[i * 2];
			if(v == 0 && buf[i * 2 + 1] == 0)
				continue; // silence (before the wave, or a gap)
			if(buf[i * 2 + 1] != (int16_t)~v || (last >= 0 && v != last % 32767 + 1))
				r->broken++;
			last = v;
			r->heard++;
		}
		OS_SleepMs(5);
	}
}

static void TestThreads(void)
{
	OpenBGI_t* o = OpenBGI_New(&kTestHost);
	static int16_t pcm[RAMP_FRAMES * 2];
	Reader_t r;
	OsThread_t* t;
	uint8_t* w;
	uint32_t size, i, start;
	printf("threads\n");
	for(i = 0; i < RAMP_FRAMES; i++)
	{ // left counts 1 .. 32767 round and round (never silence), right is its complement
		int16_t v = (int16_t)(i % 32767 + 1);
		pcm[i * 2] = v;
		pcm[i * 2 + 1] = (int16_t)~v;
	}
	w = MakeWave(pcm, RAMP_FRAMES, 2, 16, 44100, 1, &size);
	gRealTime = 1;
	CHECK(Wm_Init(o) == 0 && Wm_StartTimer(o) == 0);
	OpenBGI_SetAudioLead(o, 60);
	memset(&r, 0, sizeof r);
	r.o = o;
	t = OS_ThreadStart(ReaderThread, &r, 0);
	CHECK(t != NULL);
	CHECK(Wm_PlayWave(o, w, size) == 1);
	start = OS_TicksMs();
	while(OS_TicksMs() - start < 4000 && o->snd.waveVoice.playing)
	{ // the engine's frames: 2 ms of work, then the loop's wait
		OpenBGI_RunAudio(o);
		OS_SleepMs(2);
	}
	start = OS_TicksMs();
	while(OS_TicksMs() - start < 300)
	{ // what is left in the ring
		OpenBGI_RunAudio(o);
		OS_SleepMs(2);
	}
	r.stop = 1;
	OS_ThreadJoin(t);
	CHECK(!o->snd.waveVoice.playing);
	CHECK(r.heard == RAMP_FRAMES);
	CHECK(r.broken == 0);
	if(r.heard != RAMP_FRAMES || r.broken)
		printf("  heard %u of %u frames, %u broken\n", (unsigned)r.heard, (unsigned)RAMP_FRAMES, (unsigned)r.broken);
	gRealTime = 0;
	Wm_Shutdown(o);
	free(w);
	OpenBGI_Delete(o);
}

// a read on a thread of its own, for the check that it waits while the engine's thread is at a voice
#if defined(__GNUC__)
#define FLAG_GET(p)    __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define FLAG_SET(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#else
#define FLAG_GET(p)    (*(p))
#define FLAG_SET(p, v) (*(p) = (v))
#endif
static volatile int gReadBegan, gReadEnded, gReadGot;
static int16_t gReadOut[2000 * 2];

static void ReadThread(void* arg)
{
	FLAG_SET(&gReadBegan, 1);
	gReadGot = OpenBGI_AudioRead((OpenBGI_t*)arg, gReadOut, 2000);
	FLAG_SET(&gReadEnded, 1);
}

// an external source (a movie's sound): how often the mixer asked it
static int gTaps;

static void CountTap(void* user, int32_t* acc, int frames)
{
	BGI_UNUSED(user);
	BGI_UNUSED(acc);
	BGI_UNUSED(frames);
	gTaps++;
}

/* The reader's own mixing: a read that finds the ring short mixes the
 * rest itself, whatever the engine's thread is at - the sound goes on,
 * sample for sample - and the engine's next run does not mix that time a
 * second time.  While the engine's thread is at a voice (SndOut_Enter ..
 * SndOut_Leave) such a read waits.  For a host without locks the reader
 * only reads. */
static void TestHelp(void)
{
	enum
	{
		N = 30000
	};
	static OpenBGI_Host_t host;
	static int16_t pcm[N * 2], out[N * 2], whole[SNDOUT_RING * 2];
	OpenBGI_t* o;
	uint8_t* w;
	uint32_t size, i, starved, written;
	int same = 1, pass;
	printf("help\n");
	for(i = 0; i < N; i++)
	{
		pcm[i * 2] = (int16_t)(i * 7 - 9000);
		pcm[i * 2 + 1] = (int16_t)(12000 - i * 5);
	}
	w = MakeWave(pcm, N, 2, 16, 44100, 1, &size);
	for(pass = 0; pass < 2; pass++)
	{ // with the host's locks, and without
		host = kTestHost;
		host.threads = pass == 0 ? OS_Threads() : NULL;
		o = OpenBGI_New(&host);
		gMs = 0;
		CHECK(Wm_Init(o) == 0 && Wm_StartTimer(o) == 0);
		CHECK((o->snd.out.gate != NULL) == (pass == 0) && o->snd.out.held == 0);
		OpenBGI_SetAudioLead(o, 60);
		CHECK(Wm_PlayWave(o, w, size) == 1);
		gMs = 1;
		OpenBGI_RunAudio(o); // the lead: 60 ms of the wave are in the ring
		CHECK(OpenBGI_AudioRead(o, out, 2000) == 2000);
		// an external source is asked by the engine's own mixing, never by the reader's
		gTaps = 0;
		Wm_SetExternalMix(o, CountTap, NULL);
		gMs = 2;
		OpenBGI_RunAudio(o);
		CHECK(gTaps > 0);
		gTaps = 0;
		// the engine's thread goes away for 400 ms; the reader asks for its sound as ever
		gMs = 401;
		starved = o->snd.out.starved;
		for(i = 2000; i + 441 <= 2000 + 17640; i += 441)
		{
			int n = OpenBGI_AudioRead(o, out + (size_t)i * 2, 441);
			if(pass == 0)
				CHECK(n == 441);
		}
		if(pass == 0)
		{ // the wave went on without a gap, and nothing was missed
			for(i = 0; i < (2000 + 17640) * 2; i++)
				same &= out[i] == pcm[i];
			CHECK(same && o->snd.out.starved == starved && o->snd.out.helped > 0 && gTaps == 0);
		}
		else
			CHECK(o->snd.out.starved > starved && o->snd.out.helped == 0);
		Wm_SetExternalMix(o, NULL, NULL);
		CHECK(SndOut_Helped(o) == o->snd.out.helped);
		// back: the time the reader mixed is not mixed again - the ring holds no more than twice the lead
		written = o->snd.out.written;
		OpenBGI_RunAudio(o);
		CHECK(o->snd.out.written - o->snd.out.taken <= 2 * o->snd.out.lead && o->snd.out.extra == 0);
		CHECK(pass != 0 || o->snd.out.written - written <= 2 * o->snd.out.lead);
		// a read of more than the ring holds: mixed with the locks, what there is without
		starved = o->snd.out.starved;
		if(pass == 0)
			CHECK(OpenBGI_AudioRead(o, whole, SNDOUT_RING) == SNDOUT_RING && o->snd.out.starved == starved);
		else
			CHECK(OpenBGI_AudioRead(o, whole, SNDOUT_RING) < SNDOUT_RING && o->snd.out.starved > starved);
		if(pass == 0)
		{ // while the engine's thread is at a voice, a read that needs frames waits for it
			OsThread_t* th;
			clock_t c0;
			SndOut_Enter(o);
			SndOut_Enter(o); // (nested)
			SndOut_Leave(o);
			FLAG_SET(&gReadBegan, 0);
			FLAG_SET(&gReadEnded, 0);
			th = OS_ThreadStart(ReadThread, o, 0);
			CHECK(th != NULL);
			while(th && !FLAG_GET(&gReadBegan))
				;
			for(c0 = clock(); clock() - c0 < CLOCKS_PER_SEC / 20;)
				;
			CHECK(!FLAG_GET(&gReadEnded));
			SndOut_Leave(o);
			OS_ThreadJoin(th);
			CHECK(FLAG_GET(&gReadEnded) && gReadGot == 2000 && o->snd.out.held == 0);
		}
		// an error that jumps out of such work lets the lock go as well
		SndOut_Enter(o);
		SndOut_Enter(o);
		SndOut_LeaveAll(o);
		CHECK(o->snd.out.held == 0);
		CHECK(OpenBGI_AudioRead(o, out, 100) == (pass == 0 ? 100 : 0) || pass != 0);
		// the mixer stopped: the reader mixes nothing any more
		Wm_Shutdown(o);
		i = o->snd.out.helped;
		OpenBGI_AudioRead(o, whole, SNDOUT_RING);
		OpenBGI_AudioRead(o, whole, SNDOUT_RING);
		CHECK(o->snd.out.helped == i);
		OpenBGI_Delete(o);
	}
	free(w);
}

int main(void)
{
	OS_Init();
	TestGrid();
	TestLead();
	TestRestart();
	TestWave();
	TestThreads();
	TestHelp();
	OS_Shutdown();
	if(gFails)
	{
		printf("sndout: %d checks failed\n", gFails);
		return 1;
	}
	printf("sndout: all tests passed\n");
	return 0;
}
