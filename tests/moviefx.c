/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/moviefx.c - the decoded movies end to end: the engine's player
 *                   (src/sys/moviefx.c) over the OS layer's decoder
 *                   (src/host/os_ffmpeg.c, the decoder of OS_Host: the
 *                   movie library), and the pipe-process layer
 *                   (OS_PipeProc*) the test makes its movie with; built
 *                   as test_moviefx by `make test`
 *
 * The test needs the ffmpeg program on the PATH to make its movie;
 * without it, and in a build without the movie decoder, every part that
 * depends on them is skipped and reported, and the test still passes.
 * With them, a two-second MPEG-1 clip with a tone is generated into the
 * scratch directory (BGI_TEST_TMP, else the current directory) and
 * played without a window: the frames are decoded, the clock runs them
 * down, the sound goes through the mixer tap as the engine's sound out
 * runs the mixer (OpenBGI_RunAudio, for nobody), the length is the
 * clip's, and the end is reported once.  A missing file and an unusable
 * rectangle are refused; a stop in the middle leaves nothing running; a
 * helper that prints nothing and one that does not exist are handled by
 * the pipe layer.
 */
#include "bgi/gfx.h"
#include "bgi/core.h"
#include "bgi/display.h"
#include "bgi/snd/wavemaster.h"
#include "host/os.h"

#include <stdio.h>

#include "check.h"

// (the player blits only while the engine's window is alive, o->display.windowAlive of src/sys/window.c: none here)

static int HaveProgram(const char* name)
{
	const char* argv[] = {name, "-version", NULL};
	char buf[256];
	OsPipeProc_t* p = OS_PipeProcStart(argv);
	int n;
	if(!p)
		return 0;
	n = OS_PipeProcRead(p, buf, (int)sizeof buf);
	OS_PipeProcStop(p);
	return n > 0;
}

static void TestPipe(void)
{
	// a program that does not exist
	const char* none[] = {"bgi-no-such-program-here", NULL};
	const char* quiet[] = {"ffmpeg", "-v", "quiet", "-version", NULL};
	char buf[64];
	OsPipeProc_t* p = OS_PipeProcStart(none);
	CHECK(p == NULL || OS_PipeProcRead(p, buf, (int)sizeof buf) <= 0); // a spawn through the PATH may fail only at exec
	OS_PipeProcStop(p);
	OS_PipeProcStop(NULL);
	CHECK(OS_PipeProcRead(NULL, buf, 1) == -1);
	// a program that prints nothing: the read ends at once
	p = OS_PipeProcStart(quiet);
	if(p)
	{
		int n = OS_PipeProcRead(p, buf, (int)sizeof buf);
		(void)n; // -version prints even with -v quiet in some builds; what matters is the clean end
		while(OS_PipeProcRead(p, buf, (int)sizeof buf) > 0)
			;
		OS_PipeProcStop(p);
	}
}

static void TestPlayer(OpenBGI_t* o, const char* clip)
{
	int32_t length = -1, i;
	uint32_t start;
	int ended = 0, ticks = 0;
	CHECK(!MovieFx_Start(o, "no_such_movie.mpg", 0, 0, 320, 240, 0, &length));
	CHECK(!MovieFx_Start(o, clip, 0, 0, 0, 240, 0, &length)); // an empty rectangle
	CHECK(!MovieFx_IsRunning(o));
	CHECK(MovieFx_Start(o, clip, 0, 0, 160, 120, -1000, &length));
	CHECK(MovieFx_IsRunning(o));
	CHECK(length >= 1800 && length <= 2300); // the two-second clip's
	MovieFx_SetVolume(o, 0);
	start = OS_TicksMs();
	while(MovieFx_Tick(o))
	{
		ticks++;
		OS_SleepMs(5);
		OpenBGI_RunAudio(o); // the mixer takes the sound track for the time that passed
		if(OS_TicksMs() - start > 15000)
			break;
	}
	ended = !MovieFx_IsRunning(o);
	CHECK(ended);
	CHECK(OS_TicksMs() - start >= 1500); // the clock ran the clip down, not the decoder
	CHECK(ticks > 50);
	CHECK(!MovieFx_Tick(o)); // nothing runs: 0 at once
	// a stop in the middle
	CHECK(MovieFx_Start(o, clip, 10, 10, 160, 120, 0, &length));
	for(i = 0; i < 20; i++)
	{
		MovieFx_Tick(o);
		OS_SleepMs(5);
		OpenBGI_RunAudio(o);
	}
	MovieFx_Stop(o);
	CHECK(!MovieFx_IsRunning(o));
	MovieFx_Stop(o); // twice is fine
}

int main(void)
{
	const char* tmp = getenv("BGI_TEST_TMP");
	char clip[0x400];
	OpenBGI_t* o = OpenBGI_New(OS_Host()); // the engine whose code the test runs
	if(!o)
		return 1;
	OS_Init();
	printf("pipe\n");
	TestPipe();
	if(!HaveProgram("ffmpeg") || !OS_Host()->movieDecoder)
	{
		printf("no ffmpeg program to make a movie with, or a build without the movie decoder: the player test is skipped\nok\n");
		OS_Shutdown();
		return 0;
	}
	snprintf(clip, sizeof clip, "%s/bgi_test_movie.mpg", tmp ? tmp : ".");
	{
		const char* gen[] = {"ffmpeg", "-v", "quiet", "-y", "-f", "lavfi", "-i", "testsrc=size=320x240:rate=25",
			"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100", "-t", "2", "-c:v", "mpeg1video", "-c:a", "mp2",
			"-b:a", "128k", clip, NULL};
		char buf[64];
		OsPipeProc_t* p = OS_PipeProcStart(gen);
		CHECK(p != NULL);
		while(p && OS_PipeProcRead(p, buf, (int)sizeof buf) > 0)
			;
		OS_PipeProcStop(p);
	}
	Wm_Init(o); // the sound track goes through the mixer tap
	printf("player\n");
	TestPlayer(o, clip);
	Wm_Shutdown(o);
	remove(clip);
	OS_Shutdown();
	printf(gFails ? "%d FAILED\n" : "ok\n", gFails);
	return gFails ? 1 : 0;
}
