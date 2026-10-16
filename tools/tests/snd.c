/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/snd.c - unit test of the BW sound container, its codecs and the
 *               Wave Master API (src/snd/bw/ and src/snd/wm/, declared in
 *               inc/bgi/snd/bw.h and inc/bgi/snd/wavemaster.h); built as
 *               bin/test_snd by `make test`
 *
 * The codec tests encode with the inverse of each decoder's arithmetic and
 * check that the decoder reproduces the encoder's own reconstruction, in
 * one go (a whole-file stream, as sound effects are loaded) and in
 * odd-sized chunks (a music stream decoding on demand; the 4-bit codec
 * keeps a pending sample across odd mono requests).  A failure in a codec
 * group means the decoder's arithmetic or its chunking differs from the
 * one reproduced here.
 *
 * The groups of tests, in the order they run:
 *
 *   pcm     - codec 1: the samples come back unchanged, the gain
 *             multiplies and clamps, the burnt-in fade-in, decoding in
 *             chunks, the loop override of the pair openers and the
 *             header's own loop specification with its restart frame
 *   adpcm4  - codec 0 (4-bit ADPCM) in mono with even and odd frame
 *             counts and in stereo
 *   huffman - codec 2 (Huffman-coded 8-bit ADPCM) with a tree built here,
 *             mono and stereo, several step parameters, a file longer than
 *             the decoder's coded window, and a loop restart
 *   api     - the Wave Master entry points and every result code they
 *             return before and after Wm_Init, on bad slot numbers,
 *             unloaded slots, a corrupt file and a missing one; a sound
 *             effect and a music channel are played, faded and paused
 *
 * The api group writes a file under bin/, so the test runs from the
 * project root (as `make test` does).
 */
#include "bgi/snd/bw.h"
#include "bgi/snd/wavemaster.h"
#include "bgi/os.h"

#include <stdio.h>
#include <math.h>

static int gFails; // the number of failed checks so far
// evaluate a condition; print it with its location and count a failure when it is false
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

/* write a 0x40-byte BW header (BwHeader_t) with the given data size, frame count, rate,
 * channel count and codec; the loop fields stay 0 */
static void PutHeader(uint8_t* b, uint32_t dataSize, uint32_t frames, uint32_t rate, uint32_t ch, uint32_t codec)
{
	memset(b, 0, 0x40);
	b[0] = 0x40;
	memcpy(b + 4, "bw  ", 4);
	memcpy(b + 8, &dataSize, 4);
	memcpy(b + 12, &frames, 4);
	memcpy(b + 16, &rate, 4);
	memcpy(b + 20, &ch, 4);
	memcpy(b + 0x30, &codec, 4);
}

// a test signal: two tones, slightly different per channel
static int16_t Signal(uint32_t i, uint32_t ch)
{
	double v = 9000.0 * sin(i * 0.05 + ch) + 4000.0 * sin(i * 0.31);
	return (int16_t)v;
}

// ---- PCM --------------------------------------------------------------------

/* Codec 1 on a stereo file of 1001 frames: a whole-file stream returns
 * the header's parameters and the samples unchanged; a gain of 4
 * multiplies and clamps; WmStream_FadeIn scales sample i by i / frames
 * over the fade and refuses a fade longer than the sound.  A music stream
 * decodes the same samples in chunks of 64 frames, reports whether it
 * continues (0 for a plain file, the SetLoop override, the header's loop
 * flag), restarts at the loop start and counts its restarts.  A failure
 * means the PCM path, the fade, or the loop logic shared by every codec
 * changed. */
static void TestPcm(void)
{
	enum
	{
		N = 1001
	};
	uint8_t* file = (uint8_t*)malloc(0x40 + N * 4);
	int16_t* pcm = (int16_t*)(file + 0x40);
	WmSource_t* src;
	WmStream_t* s;
	int err = 0;
	uint32_t i;
	printf("pcm\n");
	PutHeader(file, N * 4, N, 22050, 2, BW_CODEC_PCM);
	for(i = 0; i < N; i++)
	{
		pcm[i * 2] = Signal(i, 0);
		pcm[i * 2 + 1] = Signal(i, 1);
	}
	src = WmSource_FromMemory(file, 0x40 + N * 4);
	s = WmStream_Open(src, 1.0, 1, &err);
	CHECK(s != NULL && err == 0);
	if(s)
	{
		const int16_t* out = WmStream_Samples(s);
		CHECK(WmStream_Rate(s) == 22050 && WmStream_Channels(s) == 2 && WmStream_Frames(s) == N);
		CHECK(memcmp(out, pcm, N * 4) == 0);
		// the gain multiplies and clamps
		WmStream_Close(s);
		src = WmSource_FromMemory(file, 0x40 + N * 4);
		s = WmStream_Open(src, 4.0, 1, &err);
		out = WmStream_Samples(s);
		CHECK(out[0] == (int16_t)(pcm[0] * 4 > 32767 ? 32767 : pcm[0] * 4 < -32768 ? -32768
																				   : pcm[0] * 4));
		// the burnt-in fade: frames = ms * rate / 1000, sample i scaled by i / frames
		WmStream_Close(s);
		src = WmSource_FromMemory(file, 0x40 + N * 4);
		s = WmStream_Open(src, 1.0, 1, &err);
		CHECK(WmStream_FadeIn(s, 10) == 0); // 220 frames
		out = WmStream_Samples(s);
		CHECK(out[0] == 0 && out[1] == 0);
		CHECK(out[100 * 2] == (int16_t)(int)((double)pcm[100 * 2] * 100.0 / 220.0));
		CHECK(out[220 * 2] == pcm[220 * 2]);
		CHECK(WmStream_FadeIn(s, 1000) == -1); // longer than the sound
		WmStream_Close(s);
	}
	// a streaming open decodes on demand through the same path
	src = WmSource_FromMemory(file, 0x40 + N * 4);
	s = WmStream_Open(src, 1.0, 0, &err);
	CHECK(s != NULL);
	if(s)
	{
		int16_t buf[64 * 2];
		uint32_t got = WmStream_Decode(s, buf, 64), total = got;
		CHECK(got == 64 && memcmp(buf, pcm, 64 * 4) == 0);
		while((got = WmStream_Decode(s, buf, 64)) != 0)
			total += got;
		CHECK(total == N);
		CHECK(WmStream_Continues(s) == 0); // a plain file ends
		WmStream_SetLoop(s, 2, 0);         // the override: continues, restarting at frame 0
		CHECK(WmStream_Continues(s) == 2);
		WmStream_Restart(s);
		CHECK(WmStream_LoopCount(s) == 0); // one restart, but inside the 4-second grace
		got = WmStream_Decode(s, buf, 64);
		CHECK(got == 64 && memcmp(buf, pcm, 64 * 4) == 0);
		WmStream_Close(s);
	}
	// the header's own loop specification (flag, restart frame) is what makes a
	// single-file background music loop: the openers set no flag of their own
	{
		uint32_t one = 1, start = 300;
		memcpy(file + 0x18, &one, 4);   // loopFlag
		memcpy(file + 0x1c, &start, 4); // loopStart, frames
		src = WmSource_FromMemory(file, 0x40 + N * 4);
		s = WmStream_Open(src, 1.0, 0, &err);
		CHECK(s != NULL);
		if(s)
		{
			int16_t buf[64 * 2];
			uint32_t got, total = 0;
			CHECK(WmStream_Continues(s) == 1);
			while((got = WmStream_Decode(s, buf, 64)) != 0)
				total += got;
			CHECK(total == N);
			WmStream_Restart(s); // the pump's reaction to a short decode of a continuing stream
			got = WmStream_Decode(s, buf, 64);
			CHECK(got == 64 && memcmp(buf, pcm + 300 * 2, 64 * 4) == 0); // playback resumes at the restart frame
			WmStream_SetLoop(s, 0, 0);                                   // the pair openers' override wins over the header
			CHECK(WmStream_Continues(s) == 0);
			WmStream_Close(s);
		}
		memset(file + 0x18, 0, 8);
	}
	free(file);
}

// ---- codec 0: 4-bit ADPCM ------------------------------------------------------

// an ADPCM decoder state, as the stream keeps one per channel
typedef struct
{
	int32_t pred, step; // the predicted sample and the current step size
} St;

static const int32_t kTab4[8] = {0x39, 0x39, 0x39, 0x39, 0x4d, 0x66, 0x80, 0x99}; // the decoder's step multipliers by code magnitude, in 1/64

/* The decoder's step for one 4-bit code (bit 3 the sign, bits 0..2 the
 * magnitude): the sample moves by (2 * magnitude + 1) * step / 8, the
 * step is scaled by kTab4 and clamped to 0x7f .. 0x6000.  Returns the
 * clamped 16-bit sample and advances the state. */
static int16_t Dec4(uint32_t nib, St* st)
{
	int32_t mag = nib & 7, sign = (nib & 8) ? -1 : 1;
	int32_t delta = ((2 * mag + 1) * st->step) >> 3;
	int32_t sample = st->pred + sign * delta;
	int32_t step = (int32_t)(((uint32_t)(kTab4[mag] * st->step)) >> 6);
	if(step < 0x7f)
		step = 0x7f;
	if(step > 0x6000)
		step = 0x6000;
	st->pred = sample;
	st->step = step;
	return (int16_t)(sample > 32767 ? 32767 : sample < -32768 ? -32768
															  : sample);
}

/* greedy encoder: the nibble whose reconstruction is closest to the target; advances the
 * state as the decoder will */
static uint32_t Enc4(int16_t target, St* st)
{
	uint32_t best = 0, n;
	int32_t bestErr = 0x7fffffff;
	for(n = 0; n < 16; n++)
	{
		St t = *st;
		int32_t v = Dec4(n, &t), e = abs(v - target);
		if(e < bestErr)
		{
			bestErr = e;
			best = n;
		}
	}
	Dec4(best, st);
	return best;
}

/* Codec 0 with `channels` channels and `frames` frames: the signal is
 * encoded nibble by nibble (stereo alternating the channel state per
 * sample, the low nibble of a byte first) and the reconstruction the
 * encoder saw is the reference.  A whole-file stream has to reproduce it
 * exactly, and so does a music stream decoded in chunks of 7, 3, 11 and
 * 5 frames, which in mono splits bytes between requests. */
static void TestAdpcm4(int channels, uint32_t frames)
{
	uint32_t samples = frames * channels, nibbles = samples, bytes = (nibbles + 1) / 2;
	uint8_t* file = (uint8_t*)calloc(0x40 + bytes + 1, 1);
	int16_t* ref = (int16_t*)malloc(samples * 2);
	St st[2] = {{0, 0x7f}, {0, 0x7f}}; // the initial state of the decoder: prediction 0, the smallest step
	WmSource_t* src;
	WmStream_t* s;
	int err = 0;
	uint32_t i;
	printf("adpcm4 ch=%d frames=%u\n", channels, frames);
	PutHeader(file, bytes, frames, 44100, (uint32_t)channels, BW_CODEC_ADPCM4);
	for(i = 0; i < samples; i++)
	{
		int ch = channels == 2 ? (int)(i & 1) : 0;
		St before = st[ch];
		uint32_t nib = Enc4(Signal(i / channels, (uint32_t)ch), &st[ch]);
		St t = before;
		ref[i] = Dec4(nib, &t);
		// the low nibble of a byte comes first
		if(i & 1)
			file[0x40 + i / 2] |= (uint8_t)(nib << 4);
		else
			file[0x40 + i / 2] |= (uint8_t)nib;
	}
	src = WmSource_FromMemory(file, 0x40 + bytes);
	s = WmStream_Open(src, 1.0, 1, &err);
	CHECK(s != NULL);
	if(s)
	{
		CHECK(memcmp(WmStream_Samples(s), ref, samples * 2) == 0);
		WmStream_Close(s);
	}
	// streaming in odd chunks exercises the pending sample of mono files
	src = WmSource_FromMemory(file, 0x40 + bytes);
	s = WmStream_Open(src, 1.0, 0, &err);
	CHECK(s != NULL);
	if(s)
	{
		int16_t* out = (int16_t*)calloc(samples + 8, 2);
		uint32_t done = 0, sizes[4] = {7, 3, 11, 5}, k = 0, got;
		while(done < frames && (got = WmStream_Decode(s, out + done * channels, sizes[k++ & 3])) != 0)
			done += got;
		CHECK(done == frames);
		CHECK(memcmp(out, ref, samples * 2) == 0);
		free(out);
		WmStream_Close(s);
	}
	free(file);
	free(ref);
}

// ---- codec 2: Huffman-coded 8-bit ADPCM ------------------------------------------

/* The decoder's step for one 8-bit code (bit 7 the sign, bits 0..6 the
 * magnitude) under the header's step parameter `param`: the unit is
 * 2^((param + 1) / 2), the sample moves by (2 * magnitude + 1) * step /
 * (2 * unit), and the step adapts by a factor that is 57/64 below the
 * unit and grows with the magnitude above it (the decoder's floating
 * point arithmetic is reproduced), clamped to 0x7f .. 0x6000.  Returns
 * the clamped sample and advances the state. */
static int16_t Dec7(uint32_t code, St* st, uint32_t param)
{
	uint32_t unit = 1u << ((param + 1) >> 1), mag = code & 0x7f;
	int32_t sign = (code & 0x80) ? -1 : 1;
	int32_t delta = (int32_t)(((2 * mag + 1) * (uint32_t)st->step) / (2 * unit));
	int32_t sample = st->pred + sign * delta, step;
	if(mag < unit)
		step = 57 * st->step;
	else
		step = (0x4d - (int32_t)((double)(int64_t)(mag - unit) * -25.6)) * st->step;
	step = (step + ((step >> 31) & 0x3f)) >> 6; // divide by 64 rounding toward zero
	if(step < 0x7f)
		step = 0x7f;
	if(step > 0x6000)
		step = 0x6000;
	st->pred = sample;
	st->step = step;
	return (int16_t)(sample > 32767 ? 32767 : sample < -32768 ? -32768
															  : sample);
}

/* greedy encoder: the code of the 256 whose reconstruction is closest to the target;
 * advances the state */
static uint32_t Enc7(int16_t target, St* st, uint32_t param)
{
	uint32_t best = 0, c;
	int32_t bestErr = 0x7fffffff;
	for(c = 0; c < 256; c++)
	{
		St t = *st;
		int32_t v = Dec7(c, &t, param), e = abs(v - target);
		if(e < bestErr)
		{
			bestErr = e;
			best = c;
		}
	}
	Dec7(best, st, param);
	return best;
}

/* Write the 0x400-byte tree the decoder reads: a 32-bit root, then two
 * int16 children per inner node (values of 0x100 and above are inner
 * nodes, below the symbols).  A complete binary tree over the 256 codes:
 * node 0x100 + k has the children 2k + 1 and 2k + 2 (as heap indices),
 * the leaves being the symbols; every code is 8 bits, MSB first. */
static void BuildTree(uint8_t* tree)
{
	int32_t root = 0x100;
	int k;
	memset(tree, 0, 0x400);
	memcpy(tree, &root, 4);
	for(k = 0; k < 255; k++)
	{
		int16_t l = (int16_t)(2 * k + 1 < 255 ? 0x100 + 2 * k + 1 : 2 * k + 1 - 255);
		int16_t r = (int16_t)(2 * k + 2 < 255 ? 0x100 + 2 * k + 2 : 2 * k + 2 - 255);
		memcpy(tree + 4 + k * 4, &l, 2);
		memcpy(tree + 4 + k * 4 + 2, &r, 2);
	}
}

/* the path from the root to leaf `sym` in that tree (a right child is a 1 bit); returns
 * its length */
static int CodeBits(int sym, uint32_t* bits)
{
	int n = 0, node = sym + 255; // heap index of the leaf
	uint32_t path = 0;
	while(node > 0)
	{
		int parent = (node - 1) / 2;
		path |= (uint32_t)((node == 2 * parent + 2) ? 1 : 0) << n;
		n++;
		node = parent;
	}
	*bits = path; // bit n-1 = the root's decision, bit 0 = the last
	return n;
}

/* Codec 2 with `channels` channels, `frames` frames and the step
 * parameter `param`: the file is the header (with fresh ADPCM restart
 * states and the parameter), 8 unused bytes, the tree from BuildTree and
 * the MSB-first bit stream of the codes chosen by Enc7.  A whole-file
 * stream and a music stream decoded in chunks of 13 frames must both
 * reproduce the encoder's reconstruction, and after WmStream_Restart (to
 * the header's loop bit position, 0 here) the music stream replays the
 * file.  A failure means the tree walk, the bit reader's window refill or
 * the ADPCM step changed. */
static void TestHuffman(int channels, uint32_t frames, uint32_t param)
{
	uint32_t samples = frames * channels, i;
	uint32_t cap = 0x448 + samples * 2 + 16;
	uint8_t* file = (uint8_t*)calloc(cap, 1);
	int16_t* ref = (int16_t*)malloc(samples * 2);
	St st[2] = {{0, 0x7f}, {0, 0x7f}};
	uint32_t bitPos = 0;
	WmSource_t* src;
	WmStream_t* s;
	int err = 0;
	printf("huffman ch=%d frames=%u param=%u\n", channels, frames, param);
	PutHeader(file, 0, frames, 44100, (uint32_t)channels, BW_CODEC_HUFFMAN);
	memcpy(file + 0x3c, &param, 4); // stepParam
	{                               // the restart states of the header: fresh decoders (loop bit 0)
		int32_t init[4] = {0, 0x7f, 0, 0x7f};
		memcpy(file + 0x20, init, 16);
	}
	BuildTree(file + 0x48); // after the header and 8 unused bytes
	for(i = 0; i < samples; i++)
	{
		int ch = channels == 2 ? (int)(i & 1) : 0;
		St before = st[ch], t;
		uint32_t code = Enc7(Signal(i / channels, (uint32_t)ch), &st[ch], param), bits;
		int n = CodeBits((int)code, &bits), b;
		t = before;
		ref[i] = Dec7(code, &t, param);
		for(b = n - 1; b >= 0; b--, bitPos++) // the root's decision first
			if(bits & (1u << b))
				file[0x448 + bitPos / 8] |= (uint8_t)(0x80 >> (bitPos & 7));
	}
	{
		uint32_t dataSize = (bitPos + 7) / 8 + 8 + 0x400; // the coded bytes after the header: the 8 bytes, the tree, the stream
		memcpy(file + 8, &dataSize, 4);
	}
	src = WmSource_FromMemory(file, 0x448 + (bitPos + 7) / 8);
	s = WmStream_Open(src, 1.0, 1, &err);
	CHECK(s != NULL);
	if(s)
	{
		CHECK(memcmp(WmStream_Samples(s), ref, samples * 2) == 0);
		WmStream_Close(s);
	}
	src = WmSource_FromMemory(file, 0x448 + (bitPos + 7) / 8);
	s = WmStream_Open(src, 1.0, 0, &err);
	CHECK(s != NULL);
	if(s)
	{
		int16_t* out = (int16_t*)calloc(samples + 8, 2);
		uint32_t done = 0, got;
		while(done < frames && (got = WmStream_Decode(s, out + done * channels, 13)) != 0)
			done += got;
		CHECK(done == frames);
		CHECK(memcmp(out, ref, samples * 2) == 0);
		// a loop restart at the header's bit position (0 here) replays the file
		WmStream_Restart(s);
		done = 0;
		while(done < frames && (got = WmStream_Decode(s, out + done * channels, 50)) != 0)
			done += got;
		CHECK(done == frames && memcmp(out, ref, samples * 2) == 0);
		free(out);
		WmStream_Close(s);
	}
	free(file);
	free(ref);
}

// ---- the library's result codes ----------------------------------------------------

/* The Wave Master entry points with a 500-frame mono PCM file: 0x14
 * before the library and its timer run, 1 for a second Wm_Init, 0x15 for
 * a slot or channel number out of range, 0x13 for an unloaded slot or
 * channel, 0xe for a file without the "bw  " magic, 0xc for a file that
 * does not exist; then a sound effect is loaded, played, stopped, faded
 * and unloaded, and a music channel opened from a file written under
 * bin/, queried, faded, paused and resumed, reopened as a looping pair of
 * the same name, panned and faded in volume while the 20 ms timer runs.
 * Every call has to answer 0 on the normal path, and Wm_Version has to
 * return the original library's version string.  A failure means a
 * wrapper maps an error to another code than the original or refuses a
 * valid call. */
static void TestApi(void)
{
	enum
	{
		N = 500
	};
	uint8_t* file = (uint8_t*)malloc(0x40 + N * 2);
	int16_t* pcm = (int16_t*)(file + 0x40);
	uint32_t i, playing = 0;
	printf("api\n");
	PutHeader(file, N * 2, N, 44100, 1, BW_CODEC_PCM);
	for(i = 0; i < N; i++)
		pcm[i] = Signal(i, 0);
	CHECK(Wm_SePlay(0, 0x80, 0x40) == 0x14); // not running
	CHECK(Wm_Init() == 0);
	CHECK(Wm_Init() == 1);                   // already
	CHECK(Wm_SePlay(0, 0x80, 0x40) == 0x14); // the timer is not running yet
	CHECK(Wm_StartTimer() == 0);
	CHECK(Wm_SePlay(64, 0x80, 0x40) == 0x15); // slots are 0 .. 63
	CHECK(Wm_SePlay(3, 0x80, 0x40) == 0x13);  // nothing loaded
	CHECK(Wm_SeLoad(3, file, 0, 1.0) == 0);
	CHECK(Wm_SePlay(3, 0x80, 0x40) == 0);
	CHECK(Wm_SeStop(3) == 0);
	CHECK(Wm_SeFadeOut(3, 100) == 0);
	CHECK(Wm_SeUnload(3) == 0);
	CHECK(Wm_SePlay(3, 0x80, 0x40) == 0x13);
	file[5] = 'x';
	CHECK(Wm_SeLoad(3, file, 0, 1.0) == 0xe); // not a BW file
	file[5] = 'w';
	CHECK(Wm_BgmOpenFile(16, "x", 0x80, 0x40, 1.0) == 0x15); // channels are 0 .. 15
	CHECK(Wm_BgmOpenFile(0, "no-such-file.bw", 0x80, 0x40, 1.0) == 0xc);
	CHECK(Wm_BgmIsPlaying(0, &playing) == 0x13);
	{
		char path[260];
		FILE* f;
		snprintf(path, sizeof path, "bin/test_snd_tmp.bw");
		f = fopen(path, "wb");
		if(f)
		{
			fwrite(file, 1, 0x40 + N * 2, f);
			fclose(f);
			CHECK(Wm_BgmOpenFile(0, path, 0x80, 0x40, 1.0) == 0);
			CHECK(Wm_BgmIsPlaying(0, &playing) == 0 && playing == 1);
			CHECK(Wm_BgmFadeOut(0, 50) == 0);
			CHECK(Wm_BgmPause(0, 1) == 0);
			CHECK(Wm_BgmIsPlaying(0, &playing) == 0 && playing == 0);
			CHECK(Wm_BgmPause(0, 0) == 0);
			CHECK(Wm_BgmStop(0) == 0);
			// the same name twice: a single looping file
			CHECK(Wm_BgmOpenFilePair(1, path, path, 1, 0x80, 0x40, 1.0) == 0);
			CHECK(Wm_BgmSetPan(1, 0x80) == 0);
			CHECK(Wm_BgmFadeVolume(1, 0x40, 10) == 0);
			OS_SleepMs(30);
			CHECK(Wm_TimerPoll() == 1); // a 20 ms tick is due after 30 ms
			CHECK(Wm_BgmStop(1) == 0);
			remove(path);
		}
	}
	CHECK(Wm_StopTimer() == 0);
	CHECK(Wm_Shutdown() == 0);
	CHECK(strcmp(Wm_Version(), "2.0010DS8") == 0);
	free(file);
}

// run every group; exit status 1 when any check failed
int main(void)
{
	OS_Init();
	TestPcm();
	TestAdpcm4(1, 1000);
	TestAdpcm4(1, 1001);
	TestAdpcm4(2, 777);
	TestHuffman(1, 600, 3);
	TestHuffman(2, 333, 1);
	TestHuffman(1, 5000, 5); // crosses the 0x400-byte coded window
	TestApi();
	OS_Shutdown();
	printf(gFails ? "%d FAILED\n" : "ok\n", gFails);
	return gFails ? 1 : 0;
}
