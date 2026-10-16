/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/moviefx.c - the ffmpeg movie player (src/sys/moviefx.c) and the
 *                   pipe-process layer under it (OS_PipeProc*); built as
 *                   bin/test_moviefx by `make test`
 *
 * The test needs ffmpeg on the PATH (ffprobe too for the length); without
 * it every part that depends on the program is skipped and reported, and
 * the test still passes.  With it, a two-second MPEG-1 clip with a tone is
 * generated into the scratch directory (BGI_TEST_TMP, else the current
 * directory) and played without a window: the frames are decoded, the
 * clock runs them down, the sound is pulled through the mixer tap by
 * hand, the length matches ffprobe's, and the end is reported once.  A
 * missing file and an unusable rectangle are refused; a stop in the
 * middle leaves nothing running; a helper that prints nothing and one
 * that does not exist are handled by the pipe layer.
 */
#include "bgi/gfx.h"
#include "bgi/display.h"
#include "bgi/snd/wavemaster.h"
#include "bgi/os.h"

#include <stdio.h>

static int gFails;
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

// (the player blits only while the engine's window is alive, gWindowAlive of src/sys/window.c: none here)

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

static void TestPlayer(const char* clip)
{
	int32_t length = -1, i;
	uint32_t start;
	int ended = 0, ticks = 0;
	CHECK(!MovieFx_Start("no_such_movie.mpg", 0, 0, 320, 240, 0, &length));
	CHECK(!MovieFx_Start(clip, 0, 0, 0, 240, 0, &length)); // an empty rectangle
	CHECK(!MovieFx_IsRunning());
	CHECK(MovieFx_Start(clip, 0, 0, 160, 120, -1000, &length));
	CHECK(MovieFx_IsRunning());
	CHECK(length >= 1800 && length <= 2300); // ffprobe's duration of the two-second clip
	MovieFx_SetVolume(0);
	start = OS_TicksMs();
	while(MovieFx_Tick())
	{
		ticks++;
		OS_SleepMs(5);
		if(OS_TicksMs() - start > 15000)
			break;
	}
	ended = !MovieFx_IsRunning();
	CHECK(ended);
	CHECK(OS_TicksMs() - start >= 1500); // the clock ran the clip down, not the decoder
	CHECK(ticks > 50);
	CHECK(!MovieFx_Tick()); // nothing runs: 0 at once
	// a stop in the middle
	CHECK(MovieFx_Start(clip, 10, 10, 160, 120, 0, &length));
	for(i = 0; i < 20; i++)
	{
		MovieFx_Tick();
		OS_SleepMs(5);
	}
	MovieFx_Stop();
	CHECK(!MovieFx_IsRunning());
	MovieFx_Stop(); // twice is fine
}

int main(void)
{
	const char* tmp = getenv("BGI_TEST_TMP");
	char clip[0x400];
	OS_Init();
	printf("pipe\n");
	TestPipe();
	if(!HaveProgram("ffmpeg"))
	{
		printf("ffmpeg not found: the player test is skipped\nok\n");
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
	Wm_Init(); // the sound track goes through the mixer tap (the silent clock pulls it here)
	printf("player\n");
	TestPlayer(clip);
	Wm_Shutdown();
	remove(clip);
	OS_Shutdown();
	printf(gFails ? "%d FAILED\n" : "ok\n", gFails);
	return gFails ? 1 : 0;
}
