/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/ffmovie.c - the movie decoder beside the core (include/openbgi_ffmovie.h,
 *                   src/movie/ffmovie.c) on movies made for it; built as
 *                   test_ffmovie by `make test` in the builds that have
 *                   the decoder
 *
 * The movies are a second or two long and lie in bin/test_ffmovie/.  The
 * ffmpeg program makes those that are not there yet; where it cannot be
 * run (a Windows build under wine) the ones a Linux build's test left
 * there are used, and a movie that is still missing skips its checks
 * with a line that says so.  The groups, in the order they run:
 *
 *   has     - the decoders and the file formats the games' movies need
 *             are in this build's ffmpeg
 *   mpeg    - a red MPEG-1 movie with a tone: the rate its pictures are
 *             numbered at (twice the frame rate, a number without a
 *             picture after each), its length, every picture red, at
 *             most four decoded ahead, the tone's length and loudness,
 *             both ends; at another size the pictures are scaled
 *   take    - what taking does: a number not decoded yet gives nothing
 *             and loses nothing, a picture is given once, taking one
 *             lets go of the older ones, a number without a picture
 *             gives the one before it
 *   order   - pictures that get brighter with their number come out in
 *             that order, each once
 *   formats - WMV with WMA in ASF, Microsoft MPEG-4 with PCM in AVI,
 *             H.264 with AAC in MP4, a bare MPEG-2 stream: each plays to
 *             its end in its colour, with its sound
 *   held    - a movie that leaves pictures out: their numbers have none
 *             and give the one before; none is lost.  One whose time
 *             stamps jump ahead: its pictures go on at the usual
 *             distance, and a hole after the jump is one still
 *   sound   - a tone that begins half a second after the pictures has
 *             that much silence in front of it; one that begins half a
 *             second before them has that much cut; a movie opened
 *             without sound gives none and its sound is over at once;
 *             asking for nothing decodes nothing; a tone at another
 *             rate is resampled, and when it is said to be over none of
 *             it is left
 *   files   - a file that cannot be sought plays all the same; a file
 *             that ends early, or fails in the middle, plays what is
 *             there and ends; a text file, an empty one and one that
 *             fails at once are no movies; a file struct without its
 *             size is refused
 */
#include "openbgi_ffmovie.h"
#include "host/os.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

#define DIR_ "bin/test_ffmovie"
#define W    32
#define H    24

// ---- the movies ------------------------------------------------------------------------------

/* The movie DIR_/name, made by `ffmpeg ARGS DIR_/name` when it is not
 * there yet (ARGS: the words of `args`, separated by '|'); 0 when it is
 * not there and cannot be made. */
static int Movie(const char* name, const char* args, char* path, size_t n)
{
	const char* argv[48];
	char words[1024];
	char* w;
	int count = 0;
	FILE* f;
	snprintf(path, n, DIR_ "/%s", name);
	f = fopen(path, "rb");
	if(!f)
	{
		OsPipeProc_t* p;
		char buf[256];
		argv[count++] = "ffmpeg";
		argv[count++] = "-v";
		argv[count++] = "quiet";
		argv[count++] = "-nostdin";
		argv[count++] = "-y";
		snprintf(words, sizeof words, "%s", args);
		for(w = strtok(words, "|"); w && count < 46; w = strtok(NULL, "|"))
			argv[count++] = w;
		argv[count++] = path;
		argv[count] = NULL;
		OS_NativeMkdir("bin");
		OS_NativeMkdir(DIR_);
		p = OS_PipeProcStart(argv);
		while(p && OS_PipeProcRead(p, buf, (int)sizeof buf) > 0)
			;
		OS_PipeProcStop(p);
		f = fopen(path, "rb");
	}
	if(f)
	{
		int empty = fgetc(f) == EOF;
		fclose(f);
		if(!empty)
			return 1;
		remove(path); // (what a failed run left)
	}
	printf("  (%s is not there and the ffmpeg program did not make it: skipped)\n", name);
	return 0;
}

#define RED_MPG   "-f|lavfi|-i|color=c=red:s=32x24:r=25:d=1|-f|lavfi|-i|sine=frequency=440:sample_rate=44100:duration=1|-c:v|mpeg1video|-c:a|mp2"
#define STEPS     "nullsrc=s=32x24:r=25:d=1,geq=lum='16+N*8':cb=128:cr=128"
#define STEPS_MPG "-f|lavfi|-i|" STEPS "|-c:v|mpeg1video|-q:v|2"
#define TONE(d)   "-f|lavfi|-i|sine=frequency=440:sample_rate=44100:duration=" d

// ---- a file of the host's ----------------------------------------------------------------------

typedef struct File
{
	FILE* fp;
	long limit;  // bytes it gives before it ends, -1 for all
	long failAt; // the read position from which it fails, -1 for never
	long reads;  // the calls of its read
	int noSeek;  // it cannot be sought
	void* movie; // the movie opened on it
} File_t;

static int FileRead(void* ctx, uint8_t* buf, int n)
{
	File_t* f = (File_t*)ctx;
	long at = ftell(f->fp);
	f->reads++;
	if(f->failAt >= 0 && at >= f->failAt)
		return -1;
	if(f->limit >= 0 && at + n > f->limit)
		n = at < f->limit ? (int)(f->limit - at) : 0;
	return (int)fread(buf, 1, (size_t)n, f->fp);
}

static int FileSeek(void* ctx, int64_t pos)
{
	return fseek(((File_t*)ctx)->fp, (long)pos, SEEK_SET) == 0;
}

// the movie at `path` opened at w x h (f->limit, failAt and noSeek set beforehand); NULL when it is none
static void* Open(File_t* f, const char* path, int w, int h, int audio, OpenBGI_MovieInfo_t* info)
{
	OpenBGI_MovieFile_t file = {.size = sizeof file};
	long size;
	f->fp = fopen(path, "rb");
	f->reads = 0;
	if(!f->fp)
		return NULL;
	fseek(f->fp, 0, SEEK_END);
	size = ftell(f->fp);
	fseek(f->fp, 0, SEEK_SET);
	file.ctx = f;
	file.read = FileRead;
	file.seek = f->noSeek ? NULL : FileSeek;
	file.length = f->noSeek ? -1 : (f->limit >= 0 && f->limit < size ? f->limit : size);
	memset(info, 0, sizeof *info);
	f->movie = OpenBGI_FfMovieOpen(&file, w, h, audio, info);
	if(!f->movie)
	{
		fclose(f->fp);
		f->fp = NULL;
	}
	return f->movie;
}

static void Close(File_t* f)
{
	OpenBGI_FfMovieClose(f->movie);
	if(f->fp)
		fclose(f->fp);
	memset(f, 0, sizeof *f);
	f->limit = f->failAt = -1;
}

// ---- playing -----------------------------------------------------------------------------------

// what a movie played to its end gave
typedef struct Played
{
	int32_t numbers;          // the frames it counted
	int pictures;             // the pictures taken, one per number that had one
	int most;                 // the most numbers decoded ahead of the one taken next
	int32_t first, last;      // the first and the last number with a picture
	int b, g, r;              // the middle pixel of the last picture
	int sameColour;           // every picture's middle pixel was within 40 of the first one's
	int opaque;               // every picture's alpha was 255
	int rising;               // every picture was brighter than the one before
	long sound;               // the stereo frames of sound
	long loudFirst, loudLast; // the first and the last one louder than a tenth of full scale, -1 for none
	long sides;               // the ones whose left and right differ
	long afterEnd;            // the ones that still came after the sound was said to be over
	int vEnded, aEnded;
} Played_t;

/* Play a movie to its end as a host does: the frames asked for, every
 * number taken in turn, the sound read a hundred frames at a time (and
 * asked for once more after it was said to be over). */
static void Play(void* m, int w, int h, Played_t* p)
{
	uint8_t* px = (uint8_t*)malloc((size_t)w * (size_t)h * 4);
	static int16_t snd[1024 * 2];
	int32_t next = 0;
	int first[3] = {0, 0, 0}, spins, lum = -1;
	memset(p, 0, sizeof *p);
	p->first = p->last = -1;
	p->loudFirst = p->loudLast = -1;
	p->sameColour = p->opaque = p->rising = 1;
	for(spins = 0; spins < 100000 && !(p->vEnded && p->aEnded && next >= p->numbers); spins++)
	{
		int n, i;
		p->numbers = OpenBGI_FfMovieFrames(m, &p->vEnded);
		if(p->numbers - next > p->most)
			p->most = p->numbers - next;
		if(next < p->numbers)
		{
			if(OpenBGI_FfMovieTake(m, next, px))
			{
				const uint8_t* mid = px + ((size_t)(h / 2) * (size_t)w + (size_t)(w / 2)) * 4;
				int now = mid[0] + mid[1] + mid[2];
				if(p->first < 0)
					p->first = next;
				for(i = 0; i < 3; i++)
				{
					if(p->pictures == 0)
						first[i] = mid[i];
					p->sameColour &= abs(mid[i] - first[i]) <= 40;
				}
				p->opaque &= mid[3] == 255;
				p->rising &= now > lum;
				lum = now;
				p->b = mid[0];
				p->g = mid[1];
				p->r = mid[2];
				p->last = next;
				p->pictures++;
			}
			next++;
		}
		if(p->aEnded)
		{ // (over: nothing more may come)
			p->afterEnd += OpenBGI_FfMovieSamples(m, snd, 1024, &p->aEnded);
			continue;
		}
		n = OpenBGI_FfMovieSamples(m, snd, 100, &p->aEnded);
		for(i = 0; i < n; i++)
		{
			if(abs(snd[i * 2]) > 3276 || abs(snd[i * 2 + 1]) > 3276)
			{
				if(p->loudFirst < 0)
					p->loudFirst = p->sound + i;
				p->loudLast = p->sound + i;
			}
			p->sides += snd[i * 2] != snd[i * 2 + 1];
		}
		p->sound += n;
	}
	p->afterEnd += OpenBGI_FfMovieSamples(m, snd, 1024, &p->aEnded);
	free(px);
	if(getenv("BGI_TEST_VERBOSE"))
		printf("  [%d numbers, %d pictures (%d .. %d), %d ahead, B G R %d %d %d, same %d opaque %d rising %d, %ld sound frames, loud %ld .. %ld, ended %d %d]\n",
			(int)p->numbers, p->pictures, (int)p->first, (int)p->last, p->most, p->b, p->g, p->r, p->sameColour, p->opaque, p->rising, p->sound,
			p->loudFirst, p->loudLast, p->vEnded, p->aEnded);
}

// ---- the groups --------------------------------------------------------------------------------

static void TestHas(void)
{
	static const char* const kNeeded[] = {"mpeg1video", "mpeg2video", "mp2float", "mp3float", "wmv1", "wmv2", "wmv3", "vc1", "msmpeg4",
		"wmav1", "wmav2", "wmapro", "h264", "aac", "pcm_s16le", "pcm_u8", "mpeg", "mpegvideo", "asf", "avi", "mov"};
	size_t i;
	printf("has\n");
	CHECK(strncmp(OpenBGI_FfMovieVersion(), "Lavc", 4) == 0);
	for(i = 0; i < sizeof kNeeded / sizeof kNeeded[0]; i++)
		if(!OpenBGI_FfMovieHas(kNeeded[i]))
		{
			printf("  FAIL: this build's ffmpeg has no \"%s\"\n", kNeeded[i]);
			gFails++;
		}
	CHECK(!OpenBGI_FfMovieHas("no-such-decoder"));
}

static void TestMpeg(void)
{
	File_t f = {.limit = -1, .failAt = -1};
	OpenBGI_MovieInfo_t info;
	Played_t p;
	char path[256];
	printf("mpeg\n");
	if(!Movie("red.mpg", RED_MPG, path, sizeof path))
		return;
	CHECK(Open(&f, path, W, H, 1, &info) != NULL);
	if(!f.movie)
		return;
	CHECK(info.fps == 50.0 && info.lengthMs >= 950 && info.lengthMs <= 1100 && info.audio == 1);
	Play(f.movie, W, H, &p);
	CHECK(p.vEnded && p.aEnded);
	CHECK(p.pictures == 25 && p.first == 0 && p.last == 48 && p.numbers == 49); // every other number, from the first
	CHECK(p.most >= 1 && p.most <= 8);                                          // four pictures ahead: their numbers and the ones between
	CHECK(p.r > 200 && p.g < 60 && p.b < 60 && p.sameColour && p.opaque);
	CHECK(p.sound >= 44100 - 2205 && p.sound <= 44100 + 2205);
	CHECK(p.loudFirst >= 0 && p.loudFirst < 2205 && p.loudLast > 44100 - 4410); // the tone from its start to its end
	CHECK(p.sides == 0 && p.afterEnd == 0);                                     // its one channel on both sides; none after the end
	Close(&f);
	// at another size, larger and of another shape: the pictures are scaled
	CHECK(Open(&f, path, 80, 30, 0, &info) != NULL);
	if(!f.movie)
		return;
	Play(f.movie, 80, 30, &p);
	CHECK(p.pictures == 25 && p.r > 200 && p.g < 60 && p.b < 60 && p.sameColour && p.opaque);
	Close(&f);
	// no size, and one too large
	CHECK(Open(&f, path, 0, H, 0, &info) == NULL);
	CHECK(Open(&f, path, 0x2000, 0x2000, 0, &info) == NULL);
	Close(&f);
}

static void TestTake(void)
{
	File_t f = {.limit = -1, .failAt = -1};
	OpenBGI_MovieInfo_t info;
	static uint8_t px[W * H * 4];
	char path[256];
	int32_t n;
	int ended, i;
	printf("take\n");
	if(!Movie("steps.mpg", STEPS_MPG, path, sizeof path))
		return;
	CHECK(Open(&f, path, W, H, 0, &info) != NULL);
	if(!f.movie)
		return;
	// asked again and again without taking: four pictures (the numbers 0, 2, 4, 6) and no more
	for(i = 0; i < 10; i++)
		n = OpenBGI_FfMovieFrames(f.movie, &ended);
	CHECK(n == 7 && !ended);
	// a number that is not decoded yet: nothing, and nothing is lost by asking
	CHECK(!OpenBGI_FfMovieTake(f.movie, 7, px) && !OpenBGI_FfMovieTake(f.movie, 1000, px));
	CHECK(OpenBGI_FfMovieFrames(f.movie, &ended) == 7);
	// the first picture, once
	memset(px, 0xff, sizeof px);
	CHECK(OpenBGI_FfMovieTake(f.movie, 0, px) && px[1] < 40);
	CHECK(!OpenBGI_FfMovieTake(f.movie, 0, px));
	// the number after it has no picture of its own, and the one that stands there was taken
	CHECK(!OpenBGI_FfMovieTake(f.movie, 1, px));
	// taking the picture of number 4 (the third: about 18 of 255) lets go of number 2's
	CHECK(OpenBGI_FfMovieTake(f.movie, 4, px) && px[1] > 12 && px[1] < 26);
	CHECK(!OpenBGI_FfMovieTake(f.movie, 2, px) && !OpenBGI_FfMovieTake(f.movie, 3, px) && !OpenBGI_FfMovieTake(f.movie, 4, px));
	// number 6's still waits; room for three more: the numbers up to 12
	CHECK(OpenBGI_FfMovieFrames(f.movie, &ended) == 13);
	// a number between two pictures gives the one before it (number 8's at 9: about 37), and number 6's goes with it
	memset(px, 0, sizeof px);
	CHECK(OpenBGI_FfMovieTake(f.movie, 9, px) && px[1] > 30 && px[1] < 45);
	CHECK(!OpenBGI_FfMovieTake(f.movie, 6, px) && !OpenBGI_FfMovieTake(f.movie, 8, px) && !OpenBGI_FfMovieTake(f.movie, 9, px));
	CHECK(OpenBGI_FfMovieTake(f.movie, 10, px) && px[1] > 40 && px[1] < 55);
	Close(&f); // (in the middle)
}

static void TestOrder(void)
{
	File_t f = {.limit = -1, .failAt = -1};
	OpenBGI_MovieInfo_t info;
	Played_t p;
	char path[256];
	printf("order\n");
	if(!Movie("steps.mpg", STEPS_MPG, path, sizeof path))
		return;
	CHECK(Open(&f, path, W, H, 1, &info) != NULL);
	if(!f.movie)
		return;
	CHECK(info.audio == 0); // (it has no sound)
	Play(f.movie, W, H, &p);
	CHECK(p.pictures == 25 && p.rising && p.opaque && p.vEnded && p.aEnded && p.sound == 0);
	CHECK(p.r > 190 && p.g > 190 && p.b > 190); // the last one is the brightest
	Close(&f);
}

// one movie of the formats group: `pictures` of them in colour (r, g, b: each 0 or 255, 127 for any), and about a second and a half of tone
static void Format(const char* name, const char* args, int pictures, int r, int g, int b, int audio)
{
	File_t f = {.limit = -1, .failAt = -1};
	OpenBGI_MovieInfo_t info;
	Played_t p;
	char path[256];
	if(!Movie(name, args, path, sizeof path))
		return;
	if(!Open(&f, path, W, H, 1, &info))
	{
		printf("  FAIL: %s does not open\n", name);
		gFails++;
		return;
	}
	Play(f.movie, W, H, &p);
	if(!(p.vEnded && p.aEnded && p.afterEnd == 0 && p.pictures == pictures && p.sameColour && p.opaque && abs(p.r - r) < 60 && abs(p.g - g) < 60 && abs(p.b - b) < 60 &&
		   info.audio == audio && (audio ? p.sound > 66150 * 9 / 10 && p.sound < 66150 * 11 / 10 && p.loudFirst >= 0 && p.loudFirst < 4410 : p.sound == 0)))
	{
		printf("  FAIL: %s: %d pictures (%d wanted), R G B %d %d %d, %ld sound frames, loud from %ld, audio %d, ended %d %d\n", name, p.pictures,
			pictures, p.r, p.g, p.b, p.sound, p.loudFirst, info.audio, p.vEnded, p.aEnded);
		gFails++;
	}
	Close(&f);
}

static void TestFormats(void)
{
	printf("formats\n");
	Format("blue.wmv", "-f|lavfi|-i|color=c=blue:s=32x24:r=30:d=1.5|" TONE("1.5") "|-c:v|wmv2|-c:a|wmav2", 45, 0, 0, 255, 1);
	Format("blue1.wmv", "-f|lavfi|-i|color=c=blue:s=32x24:r=30:d=1.5|" TONE("1.5") "|-c:v|wmv1|-c:a|wmav1", 45, 0, 0, 255, 1);
	Format("green.avi", "-f|lavfi|-i|color=c=lime:s=32x24:r=15:d=1.5|" TONE("1.5") "|-c:v|msmpeg4v3|-c:a|pcm_s16le", 23, 0, 255, 0, 1);
	Format("mp3.avi", "-f|lavfi|-i|color=c=lime:s=32x24:r=25:d=1.5|" TONE("1.5") "|-c:v|mpeg2video|-c:a|libmp3lame", 38, 0, 255, 0, 1);
	Format("white.mp4", "-f|lavfi|-i|color=c=white:s=32x24:r=24:d=1.5|" TONE("1.5") "|-c:v|libx264|-pix_fmt|yuv420p|-c:a|aac", 36, 255, 255, 255, 1);
	Format("white.m2v", "-f|lavfi|-i|color=c=white:s=32x24:r=25:d=1.5|-c:v|mpeg2video|-f|mpeg2video", 38, 255, 255, 255, 0);
}

static void TestHeld(void)
{
	File_t f = {.limit = -1, .failAt = -1};
	OpenBGI_MovieInfo_t info;
	static uint8_t px[W * H * 4];
	Played_t p;
	char path[256];
	int ended, before;
	printf("held\n");
	// the pictures 8 to 15 of 25 are left out: the one before them stands for a third of a second
	if(!Movie("held.wmv", "-f|lavfi|-i|" STEPS ",select='not(between(n,8,15))'|-fps_mode|passthrough|-c:v|wmv2|-q:v|2", path, sizeof path))
		return;
	CHECK(Open(&f, path, W, H, 0, &info) != NULL);
	if(!f.movie)
		return;
	CHECK(info.fps == 25.0);
	Play(f.movie, W, H, &p);
	CHECK(p.numbers == 25 && p.pictures == 17 && p.first == 0 && p.last == 24 && p.rising && p.vEnded);
	Close(&f);
	// taken in the middle of the hole: the picture before it, once
	CHECK(Open(&f, path, W, H, 0, &info) != NULL);
	if(!f.movie)
		return;
	OpenBGI_FfMovieFrames(f.movie, &ended);
	CHECK(OpenBGI_FfMovieTake(f.movie, 3, px));
	CHECK(OpenBGI_FfMovieFrames(f.movie, &ended) == 8);  // (the numbers 4 to 7 wait)
	CHECK(OpenBGI_FfMovieTake(f.movie, 7, px));          // which leaves room for the picture after the hole, and three more
	CHECK(OpenBGI_FfMovieFrames(f.movie, &ended) == 20); // 16 to 19
	before = px[1];
	CHECK(!OpenBGI_FfMovieTake(f.movie, 12, px) && px[1] == before);    // number 7's stands there, and was taken
	CHECK(OpenBGI_FfMovieTake(f.movie, 16, px) && px[1] > before + 40); // the first one after the hole
	Close(&f);
	// the time stamps jump half a minute ahead at the eleventh picture, and the pictures 15 to 19 are left out: the
	// pictures go on at their usual distance over the jump, counted from there, so the hole after it is one still
	if(!Movie("jump.wmv", "-f|lavfi|-i|" STEPS ",setpts='if(gte(N,10),PTS+30/TB,PTS)',select='not(between(n,15,19))'|-fps_mode|passthrough|-c:v|wmv2|-q:v|2",
		   path, sizeof path))
		return;
	CHECK(Open(&f, path, W, H, 0, &info) != NULL);
	if(!f.movie)
		return;
	Play(f.movie, W, H, &p);
	CHECK(p.numbers == 25 && p.pictures == 20 && p.first == 0 && p.last == 24 && p.rising && p.vEnded);
	Close(&f);
	CHECK(Open(&f, path, W, H, 0, &info) != NULL);
	if(!f.movie)
		return;
	OpenBGI_FfMovieFrames(f.movie, &ended);
	CHECK(OpenBGI_FfMovieTake(f.movie, 3, px));
	OpenBGI_FfMovieFrames(f.movie, &ended);
	CHECK(OpenBGI_FfMovieTake(f.movie, 7, px));
	CHECK(OpenBGI_FfMovieFrames(f.movie, &ended) == 12); // 8 to 11: the jump is no hole
	CHECK(OpenBGI_FfMovieTake(f.movie, 11, px));
	CHECK(OpenBGI_FfMovieFrames(f.movie, &ended) == 21); // 12, 13, 14 and, after the hole, 20
	Close(&f);
}

static void TestSound(void)
{
	File_t f = {.limit = -1, .failAt = -1};
	OpenBGI_MovieInfo_t info;
	Played_t p;
	static int16_t snd[4096 * 2];
	char path[256], video[256], tone[256], args[1024];
	int ended, i, loud = 0;
	printf("sound\n");
	// the parts the two movies below are put together of, as they are
	if(!Movie("part.m1v", "-f|lavfi|-i|color=c=red:s=32x24:r=25:d=1.5|-c:v|mpeg1video|-f|mpeg1video", video, sizeof video) ||
		!Movie("part.mp2", TONE("1.5") "|-c:a|mp2", tone, sizeof tone))
		return;
	// the tone half a second after the pictures: that much silence first
	snprintf(args, sizeof args, "-i|%s|-itsoffset|0.5|-i|%s|-c|copy", video, tone);
	if(Movie("late.mpg", args, path, sizeof path) && Open(&f, path, W, H, 1, &info))
	{
		Play(f.movie, W, H, &p);
		CHECK(p.pictures >= 36 && p.pictures <= 38 && p.aEnded);
		CHECK(p.loudFirst > 22050 - 2205 && p.loudFirst < 22050 + 2205);
		CHECK(p.sound > 88200 - 4410 && p.sound < 88200 + 4410);
		Close(&f);
	}
	// the pictures half a second after the tone: that much of it is cut
	snprintf(args, sizeof args, "-itsoffset|0.5|-i|%s|-i|%s|-c|copy", video, tone);
	if(Movie("early.mpg", args, path, sizeof path) && Open(&f, path, W, H, 1, &info))
	{
		Play(f.movie, W, H, &p);
		CHECK(p.pictures >= 36 && p.pictures <= 38 && p.first == 0 && p.aEnded);
		CHECK(p.loudFirst >= 0 && p.loudFirst < 2205);
		CHECK(p.sound > 44100 - 4410 && p.sound < 44100 + 4410);
		Close(&f);
	}
	// a tone at half the engine's rate, taken a frame at a time: resampled, and none of it left when it is said to be over
	if(Movie("low.mpg", "-f|lavfi|-i|color=c=red:s=32x24:r=25:d=1|-f|lavfi|-i|sine=frequency=440:sample_rate=22050:duration=1|-c:v|mpeg1video|-c:a|mp2", path,
		   sizeof path) &&
		Open(&f, path, W, H, 1, &info))
	{
		long total = 0, left;
		OpenBGI_FfMovieFrames(f.movie, &ended); // (the first picture: the sound is laid against it)
		ended = 0;
		for(i = 0; i < 100000 && !ended; i++)
			total += OpenBGI_FfMovieSamples(f.movie, snd, 1, &ended);
		left = OpenBGI_FfMovieSamples(f.movie, snd, 4096, &ended);
		CHECK(ended && left == 0 && total > 44100 - 2205 && total < 44100 + 2205);
		Close(&f);
	}
	if(!Movie("red.mpg", RED_MPG, path, sizeof path))
		return;
	// opened without its sound: none, and that is over from the start
	CHECK(Open(&f, path, W, H, 0, &info) != NULL && info.audio == 0);
	if(f.movie)
	{
		ended = 0;
		CHECK(OpenBGI_FfMovieSamples(f.movie, snd, 64, &ended) == 0 && ended == 1);
		Close(&f);
	}
	// with it: asking for nothing reads nothing and decodes nothing; what is asked for can be dropped
	CHECK(Open(&f, path, W, H, 1, &info) != NULL && info.audio == 1);
	if(f.movie)
	{
		long reads = f.reads;
		ended = 1;
		CHECK(OpenBGI_FfMovieSamples(f.movie, NULL, 0, &ended) == 0 && ended == 0 && f.reads == reads);
		CHECK(OpenBGI_FfMovieSamples(f.movie, NULL, 64, &ended) == 64 && ended == 0);
		CHECK(OpenBGI_FfMovieSamples(f.movie, snd, 4096, &ended) == 4096 && ended == 0);
		for(i = 0; i < 4096 * 2; i++)
			loud += abs(snd[i]) > 3276;
		CHECK(loud > 0);
		Close(&f);
	}
}

static void TestFiles(void)
{
	File_t f = {.limit = -1, .failAt = -1};
	OpenBGI_MovieFile_t file = {.size = sizeof file};
	OpenBGI_MovieInfo_t info;
	Played_t p;
	char path[256];
	FILE* fp;
	long size;
	printf("files\n");
	// no movies: a text, nothing at all, a file that cannot be read
	OS_NativeMkdir("bin");
	OS_NativeMkdir(DIR_);
	fp = fopen(DIR_ "/text.mpg", "wb");
	if(fp)
	{
		int i;
		for(i = 0; i < 400; i++)
			fputs("This is no movie, whatever its name says.\n", fp);
		fclose(fp);
		CHECK(Open(&f, DIR_ "/text.mpg", W, H, 1, &info) == NULL);
		Close(&f);
		f.limit = 0;
		CHECK(Open(&f, DIR_ "/text.mpg", W, H, 1, &info) == NULL);
		Close(&f);
		remove(DIR_ "/text.mpg");
	}
	// a struct without its size, or without a way to read
	file.read = FileRead;
	file.size = 0;
	CHECK(OpenBGI_FfMovieOpen(&file, W, H, 0, &info) == NULL);
	file.size = sizeof file;
	file.read = NULL;
	CHECK(OpenBGI_FfMovieOpen(&file, W, H, 0, &info) == NULL);
	CHECK(OpenBGI_FfMovieOpen(NULL, W, H, 0, &info) == NULL);
	OpenBGI_FfMovieClose(NULL);
	if(!Movie("red.mpg", RED_MPG, path, sizeof path))
		return;
	f.failAt = 0;
	CHECK(Open(&f, path, W, H, 1, &info) == NULL);
	Close(&f);
	// read from its start to its end without seeking: all of it
	f.noSeek = 1;
	CHECK(Open(&f, path, W, H, 1, &info) != NULL);
	if(f.movie)
	{
		Play(f.movie, W, H, &p);
		CHECK(p.pictures == 25 && p.vEnded && p.aEnded && p.sound > 44100 - 2205);
	}
	Close(&f);
	// half of it: what is there plays, and it ends
	fp = fopen(path, "rb");
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fclose(fp);
	f.limit = size / 2;
	CHECK(Open(&f, path, W, H, 1, &info) != NULL);
	if(f.movie)
	{
		Play(f.movie, W, H, &p);
		CHECK(p.pictures >= 5 && p.pictures <= 25 && p.vEnded && p.aEnded && p.sound > 4410 && p.sound < 44100 - 4410);
	}
	Close(&f);
	// it fails in the middle: the same
	f.failAt = size / 2;
	CHECK(Open(&f, path, W, H, 1, &info) != NULL);
	if(f.movie)
	{
		Play(f.movie, W, H, &p);
		CHECK(p.pictures >= 5 && p.pictures <= 25 && p.vEnded && p.aEnded && p.sound < 44100 - 4410);
	}
	Close(&f);
}

int main(void)
{
	OS_Init();
	TestHas();
	TestMpeg();
	TestTake();
	TestOrder();
	TestFormats();
	TestHeld();
	TestSound();
	TestFiles();
	OS_Shutdown();
	if(gFails)
	{
		printf("ffmovie: %d checks failed\n", gFails);
		return 1;
	}
	printf("ffmovie: all tests passed\n");
	return 0;
}
