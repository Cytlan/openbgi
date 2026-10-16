/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * moviefx.c - movie playback through ffmpeg (inc/bgi/gfx.h: MovieFx_*)
 *
 * The original plays its opening movies (MPEG files beside the archives)
 * through DirectShow.  Where no native player is at hand - always on
 * POSIX, and on Windows when DirectShow cannot render the file - the
 * movie is decoded by the ffmpeg program, which must be installed (on
 * the PATH, or dropped next to the engine's binary): one ffmpeg process
 * writes the picture as raw BGRA frames at the size of the rectangle the
 * script asked for and at the source's frame rate, a second one the
 * sound as 16-bit stereo at the mixer's rate, both through pipes
 * (OS_PipeProc*).  Two threads fill a frame ring and a sample ring;
 * the frames are presented from the engine's pass (MovieFx_Tick, called
 * every pass from the scheduler through Movie_Tick) on a clock that
 * starts with the first frame, dropping late ones, and the samples are
 * mixed into the sound library's output (Wm_SetExternalMix) with the
 * movie volume as the gain.  ffprobe, when present, supplies the length
 * the script is told and the frame rate; without it the rate is taken as
 * 30 and the length as unknown.  The movie is over when both pipes have
 * ended and everything read was shown or heard; Movie_OnEvent then does
 * what the DirectShow EC_COMPLETE did.
 */
#include "bgi/gfx.h"
#include "bgi/display.h"
#include "bgi/snd/wavemaster.h"
#include "bgi/os.h"

#include <math.h>

#define FX_FRAMES      4                   // frames in the ring
#define FX_AUDIO_RATE  44100               // the mixer's rate (WM_MIX_RATE)
#define FX_RING_FRAMES (FX_AUDIO_RATE * 2) // two seconds of sound
#define FX_CMD_MAX     0x400

typedef struct MovieFx
{
	OsPipeProc_t* video;
	OsPipeProc_t* audio;
	OsThread_t* vThread;
	OsThread_t* aThread;
	int x, y, w, h;                 // the rectangle on the picture
	double fps;                     // frames per second of the raw video stream
	int32_t lengthMs;               // from ffprobe, 0 unknown
	uint8_t* frame[FX_FRAMES];      // the ring: w * h * 4 bytes each
	volatile int32_t vWrite, vRead; // frames produced / consumed (monotonic; the slot is the count modulo FX_FRAMES)
	volatile int vEnd;              // the video pipe ended
	int16_t* ring;                  // the sample ring, FX_RING_FRAMES stereo frames
	volatile int32_t aWrite, aRead; // frames produced / consumed (monotonic)
	volatile int aEnd;              // the audio pipe ended
	volatile int stop;              // the threads are to leave
	int started;                    // the clock runs
	uint32_t startTick;             // when the first frame was shown
	int32_t shown;                  // index of the frame shown last, -1 for none
	volatile int32_t gain16;        // the volume as a 16.16 gain, read by the mixer's thread
	int hasAudio;                   // the sound track is mixed
} MovieFx_t;

static MovieFx_t* gFx; // the running movie, NULL for none

// ---- the decoder threads -----------------------------------------------------------------------

static int ReadFully(OsPipeProc_t* p, uint8_t* buf, int n, volatile int* stop)
{
	int got = 0;
	while(got < n && !*stop)
	{
		int r = OS_PipeProcRead(p, buf + got, n - got);
		if(r <= 0)
			return 0;
		got += r;
	}
	return got == n;
}

static void VideoThread(void* arg)
{
	MovieFx_t* fx = (MovieFx_t*)arg;
	int frameBytes = fx->w * fx->h * 4;
	while(!fx->stop)
	{
		int slot;
		if(fx->vWrite - fx->vRead >= FX_FRAMES)
		{ // the ring is full: wait for the presenter
			OS_SleepMs(2);
			continue;
		}
		slot = fx->vWrite % FX_FRAMES;
		if(!ReadFully(fx->video, fx->frame[slot], frameBytes, &fx->stop))
			break;
		fx->vWrite++;
	}
	fx->vEnd = 1;
}

static void AudioThread(void* arg)
{
	MovieFx_t* fx = (MovieFx_t*)arg;
	uint8_t chunk[4096 * 4];
	while(!fx->stop)
	{
		int free = FX_RING_FRAMES - (fx->aWrite - fx->aRead), n, got, i;
		if(free < 4096)
		{
			OS_SleepMs(5);
			continue;
		}
		n = OS_PipeProcRead(fx->audio, chunk, (int)sizeof chunk);
		if(n <= 0)
			break;
		got = n / 4; // whole stereo frames (ffmpeg writes whole samples; a torn tail is dropped)
		for(i = 0; i < got; i++)
		{
			int slot = (fx->aWrite + i) % FX_RING_FRAMES;
			memcpy(fx->ring + (size_t)slot * 2, chunk + (size_t)i * 4, 4);
		}
		fx->aWrite += got;
	}
	fx->aEnd = 1;
}

// the external mix: the ring's samples, scaled by the gain, into the accumulators (silence when the ring is empty)
static void MixTap(void* user, int32_t* acc, int frames)
{
	MovieFx_t* fx = (MovieFx_t*)user;
	int32_t avail = fx->aWrite - fx->aRead, gain = fx->gain16;
	int n = avail < frames ? (int)avail : frames, i;
	if(!fx->started || gain == 0)
	{ // not started yet: keep the sound aligned with the picture; silent: consume without mixing
		if(fx->started)
			fx->aRead += n;
		return;
	}
	for(i = 0; i < n; i++)
	{
		int slot = (fx->aRead + i) % FX_RING_FRAMES;
		acc[i * 2] += (int32_t)(((int64_t)fx->ring[(size_t)slot * 2] * gain) >> 16);
		acc[i * 2 + 1] += (int32_t)(((int64_t)fx->ring[(size_t)slot * 2 + 1] * gain) >> 16);
	}
	fx->aRead += n;
}

// ---- probing -----------------------------------------------------------------------------------

/* ffprobe: the frame rate and the length (lengthMs 0 when unknown).  The
 * output is "key=value" lines; a missing ffprobe leaves the defaults. */
static void Probe(const char* native, double* fps, int32_t* lengthMs)
{
	const char* argv[] = {"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
		"stream=r_frame_rate:format=duration", "-of", "default=noprint_wrappers=1", native, NULL};
	OsPipeProc_t* p = OS_PipeProcStart(argv);
	char out[0x800];
	int len = 0, r;
	char* line;
	*fps = 30.0;
	*lengthMs = 0;
	if(!p)
		return;
	while(len < (int)sizeof out - 1 && (r = OS_PipeProcRead(p, out + len, (int)sizeof out - 1 - len)) > 0)
		len += r;
	out[len] = 0;
	OS_PipeProcStop(p);
	for(line = out; line && *line;)
	{
		char* nl = strchr(line, '\n');
		if(nl)
			*nl = 0;
		if(strncmp(line, "r_frame_rate=", 13) == 0)
		{
			double num = atof(line + 13), den = 1.0;
			const char* slash = strchr(line + 13, '/');
			if(slash)
				den = atof(slash + 1);
			if(num > 0 && den > 0 && num / den >= 1.0 && num / den <= 240.0)
				*fps = num / den;
		}
		else if(strncmp(line, "duration=", 9) == 0)
		{
			double d = atof(line + 9);
			if(d > 0)
				*lengthMs = (int32_t)(d * 1000.0 + 0.5);
		}
		line = nl ? nl + 1 : NULL;
	}
}

// ---- the player --------------------------------------------------------------------------------

static int32_t GainOf(int32_t volume) // IBasicAudio hundredths of a decibel (-10000 .. 0) as a 16.16 factor
{
	if(volume <= -10000)
		return 0;
	if(volume >= 0)
		return 0x10000;
	return (int32_t)(pow(10.0, volume / 2000.0) * 65536.0);
}

static void Release(MovieFx_t* fx)
{
	int i;
	fx->stop = 1;
	if(fx->hasAudio)
		Wm_SetExternalMix(NULL, NULL);
	OS_PipeProcStop(fx->video); // the readers wake with an error or the end
	OS_PipeProcStop(fx->audio);
	OS_ThreadJoin(fx->vThread);
	OS_ThreadJoin(fx->aThread);
	for(i = 0; i < FX_FRAMES; i++)
		free(fx->frame[i]);
	free(fx->ring);
	free(fx);
}

/* Start `path` (an engine path) in the picture rectangle (x, y, w, h)
 * at `volume`; *lengthMs receives the length ffprobe reports (0
 * unknown).  1 when ffmpeg is running on the file, 0 when it could not
 * be started (ffmpeg missing, or the file unreadable).  An earlier movie
 * is stopped first. */
int MovieFx_Start(const char* path, int x, int y, int w, int h, int32_t volume, int32_t* lengthMs)
{
	MovieFx_t* fx;
	char native[0x400], size[64], rate[32];
	int i;
	MovieFx_Stop();
	if(w <= 0 || h <= 0 || (size_t)w * (size_t)h > 0x1000000u || OS_FileAttrs(path) == OS_INVALID_ATTRS)
		return 0;
	OS_PathNative(path, native, sizeof native);
	fx = (MovieFx_t*)calloc(1, sizeof *fx);
	fx->x = x;
	fx->y = y;
	fx->w = w;
	fx->h = h;
	fx->shown = -1;
	fx->gain16 = GainOf(volume);
	Probe(native, &fx->fps, &fx->lengthMs);
	snprintf(size, sizeof size, "scale=%d:%d", w, h);
	snprintf(rate, sizeof rate, "%.4f", fx->fps);
	{
		const char* argv[] = {"ffmpeg", "-v", "quiet", "-nostdin", "-i", native, "-an", "-f", "rawvideo", "-pix_fmt", "bgra",
			"-vf", size, "-r", rate, "pipe:1", NULL};
		fx->video = OS_PipeProcStart(argv);
	}
	if(!fx->video)
	{
		static int said; // once per run: the usual cause is no ffmpeg on the PATH
		if(!said++)
			fprintf(stderr, "bgi: ffmpeg could not be started for \"%s\": the movie is skipped (ffmpeg and ffprobe must be on the PATH or next to the executable)\n", native);
		free(fx);
		return 0;
	}
	for(i = 0; i < FX_FRAMES; i++)
		fx->frame[i] = (uint8_t*)malloc((size_t)w * (size_t)h * 4);
	if(Wm_MixRunning())
	{
		const char* argv[] = {"ffmpeg", "-v", "quiet", "-nostdin", "-i", native, "-vn", "-f", "s16le", "-ac", "2", "-ar",
			"44100", "pipe:1", NULL};
		fx->audio = OS_PipeProcStart(argv);
		if(fx->audio)
		{
			fx->ring = (int16_t*)calloc((size_t)FX_RING_FRAMES * 2, sizeof(int16_t));
			fx->hasAudio = 1;
		}
	}
	if(!fx->audio)
		fx->aEnd = 1;
	fx->vThread = OS_ThreadStart(VideoThread, fx, 0);
	if(fx->audio)
		fx->aThread = OS_ThreadStart(AudioThread, fx, 0);
	if(fx->hasAudio)
		Wm_SetExternalMix(MixTap, fx);
	gFx = fx;
	*lengthMs = fx->lengthMs;
	return 1;
}

void MovieFx_Stop(void)
{
	MovieFx_t* fx = gFx;
	if(!fx)
		return;
	gFx = NULL;
	Release(fx);
}

// a movie is loaded and not yet over
int MovieFx_IsRunning(void)
{
	return gFx != NULL;
}

void MovieFx_SetVolume(int32_t volume)
{
	if(gFx)
		gFx->gain16 = GainOf(volume);
}

// blit frame `slot` of the ring into the window at the movie's rectangle (through the display scaling)
static void ShowFrame(MovieFx_t* fx, int slot)
{
	Bmp_t b;
	OsDc_t* dc;
	if(!gWindowAlive)
		return;
	b.pixels = fx->frame[slot];
	b.pitch = fx->w * 4;
	b.w = fx->w;
	b.h = fx->h;
	b.mode = PM_RGB32;
	b.bpp = 4;
	dc = OS_WindowGetDc();
	Window_DcBlit(dc, fx->x, fx->y, &b);
	OS_WindowReleaseDc(dc);
}

/* Every pass: show the frame that is due (the newest one not later than
 * the clock, so a slow machine drops frames rather than falling behind),
 * and end the movie once both streams are exhausted.  1 while the movie
 * runs, 0 when it just ended (the caller then does what the end
 * notification did) or none runs. */
int MovieFx_Tick(void)
{
	MovieFx_t* fx = gFx;
	int32_t avail, due, idx;
	uint32_t now;
	if(!fx)
		return 0;
	avail = fx->vWrite - fx->vRead;
	if(!fx->started)
	{
		if(avail <= 0 && !fx->vEnd)
			return 1; // the first frame is still being decoded
		fx->started = 1;
		fx->startTick = OS_TicksMs();
	}
	now = OS_TicksMs();
	due = (int32_t)((double)(now - fx->startTick) * fx->fps / 1000.0);
	// the newest produced frame whose index is at most `due`
	idx = fx->vWrite - 1;
	if(idx > due)
		idx = due;
	if(idx > fx->shown && idx >= fx->vRead)
	{
		ShowFrame(fx, idx % FX_FRAMES);
		fx->shown = idx;
		fx->vRead = idx + 1; // frees this and every older slot
	}
	if(fx->vEnd && fx->vRead >= fx->vWrite)
	{ // every frame shown; the sound may still be draining
		int32_t left = fx->aWrite - fx->aRead;
		if(fx->aEnd && (left <= 0 || !fx->hasAudio))
		{
			MovieFx_Stop();
			return 0;
		}
	}
	return 1;
}
