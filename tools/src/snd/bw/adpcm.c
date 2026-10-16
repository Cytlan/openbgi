/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * adpcm.c - the ADPCM arithmetic of the BW codecs and codec 0, the 4-bit
 *           ADPCM of sound effects (bw_internal.h)
 *
 * Both ADPCM variants keep a predictor and a step per channel; a code's
 * magnitude scales the step for the next sample.  The 4-bit codec packs
 * two codes per byte, the 7-bit codes of codec 2 come out of the Huffman
 * decoder (huffman.c) and use Adpcm7_Step.
 */
#include "bw_internal.h"

// step multipliers of the 4-bit codec by magnitude, in 1/64
static const int32_t kStepTable4[8] = {0x39, 0x39, 0x39, 0x39, 0x4d, 0x66, 0x80, 0x99};

/* one 4-bit code (bit 3 sign, bits 0..2 magnitude): the sample is the
 * predictor plus (2 * magnitude + 1) / 8 of the step, the step is scaled
 * by the table and kept in 0x7f .. 0x6000; the clamped 16-bit sample */
int16_t Adpcm4_Step(uint32_t nibble, AdpcmState_t* st)
{
	int32_t mag = (int32_t)(nibble & 7);
	int32_t sign = ((nibble >> 2) & 2) ? -1 : 1;
	int32_t delta = ((2 * mag + 1) * st->step) >> 3;
	int32_t sample = st->pred + sign * delta;
	int32_t step = (int32_t)(((uint32_t)(kStepTable4[mag] * st->step)) >> 6);
	if(step < 0x7f)
		step = 0x7f;
	else if(step > 0x6000)
		step = 0x6000;
	st->pred = sample;
	st->step = step;
	if(sample > 0x7fff)
		return 0x7fff;
	if(sample < -0x8000)
		return -0x8000;
	return (int16_t)sample;
}

/* one 8-bit code (bit 7 sign, bits 0..6 magnitude) of the Huffman codec;
 * `param` is the header's step shift, which sets the magnitude unit
 * (2 ^ ((param + 1) / 2)).  The step is scaled by 57/64 for a magnitude
 * below the unit, by 77/64 at the unit and 25.6/64 more per magnitude
 * step above it, and kept in 0x7f .. 0x6000; the clamped 16-bit sample */
int16_t Adpcm7_Step(uint32_t code, AdpcmState_t* st, uint32_t param)
{
	uint32_t unit = 1u << ((param + 1) >> 1);
	uint32_t mag = code & 0x7f;
	int32_t sign = (code & 0x80) ? -1 : 1;
	int32_t delta = (int32_t)(((2 * mag + 1) * (uint32_t)st->step) / (2 * unit));
	int32_t sample = st->pred + sign * delta;
	int32_t step;
	if(mag < unit)
		step = 57 * st->step;
	else
	{
		// (mag - unit) * -25.6 truncated toward zero, so the factor grows by 25.6 per magnitude step
		int32_t k = BGI_Ftol((double)(int64_t)(mag - unit) * -25.6);
		step = (0x4d - k) * st->step;
	}
	step = (step + ((step >> 31) & 0x3f)) >> 6; // /64 toward zero
	if(step < 0x7f)
		step = 0x7f;
	else if(step > 0x6000)
		step = 0x6000;
	st->pred = sample;
	st->step = step;
	if(sample > 0x7fff)
		return 0x7fff;
	if(sample < -0x8000)
		return -0x8000;
	return (int16_t)sample;
}

/* `count` frames of nibbles into samples; the low nibble of a
 * byte comes first, stereo alternates the channel state per sample */
static void Adpcm4_DecodeBlock(WmStream_t* s, int16_t* dst, const uint8_t* nibbles, uint32_t count)
{
	uint32_t n = count * s->channels, i;
	int ch = 0;
	for(i = 0; i < n; i++)
	{
		uint32_t nib = (i & 1) ? (nibbles[i >> 1] >> 4) : (nibbles[i >> 1] & 0xf);
		dst[i] = Adpcm4_Step(nib, &s->adpcm[ch]);
		if(s->channels == 2)
			ch ^= 1;
	}
}

/* Decode up to `frames` frames of codec 0 into dst; the frames delivered
 * (fewer at the end of the stream).  Mono data packs two frames per
 * byte, so an odd request decodes one frame too many: it is kept as the
 * pending sample and delivered first by the next call. */
uint32_t Adpcm4_Decode(WmStream_t* s, int16_t* dst, uint32_t frames)
{
	uint32_t left, n, perChunk, done = 0, extra = 0;
	int16_t* out = dst;
	if(s->hasPending)
	{ // the sample decoded beyond the previous odd request goes out first
		*out++ = s->pending;
		frames--;
		extra = 1;
	}
	left = s->hdr.frames - s->pos - extra;
	n = frames <= left ? frames : left;
	perChunk = (WM_CHUNK_BYTES & ~3u) / s->blockAlign; // frames per pass through the scratch buffer
	s->hasPending = (s->channels == 1 && (frames & 1)) ? 1 : 0;
	while(done < n)
	{
		uint32_t remaining = n - done, decodeN;
		if(perChunk > remaining)
			decodeN = remaining + ((s->channels & 1) * (remaining & 1)); // a whole byte for an odd mono tail
		else
			remaining = decodeN = perChunk;
		WmSource_Read(s->src, s->codeBuf, (decodeN * s->blockAlign) >> 2); // half a byte per sample
		Adpcm4_DecodeBlock(s, s->chunk, s->codeBuf, decodeN);
		Wm_ApplyGain(s, s->chunk, decodeN * s->channels);
		memcpy(out, s->chunk, (size_t)remaining * s->blockAlign);
		out += remaining * s->channels;
		done += remaining;
		if(s->hasPending && remaining != decodeN)
			s->pending = s->chunk[remaining];
	}
	// the original counts neither the position nor the result with the pending
	// sample, which is harmless with its even-sized requests; counting it keeps
	// odd-sized requests consistent
	s->pos += done + extra;
	if(s->pos == s->hdr.frames)
		s->hasPending = 0;
	return done + extra;
}
