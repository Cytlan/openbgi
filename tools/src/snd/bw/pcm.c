/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pcm.c - codec 1 of the BW format: plain 16-bit PCM, and the gain every
 *         codec applies to its output (bw_internal.h)
 */
#include "bw_internal.h"

// scale `samples` 16-bit samples in place by the stream's gain, truncated and clamped
void Wm_ApplyGain(const WmStream_t* s, int16_t* p, uint32_t samples)
{
	uint32_t i;
	for(i = 0; i < samples; i++)
	{
		int32_t v = BGI_Ftol((double)p[i] * s->gain);
		if(v < -0x8000)
			v = -0x8000;
		else if(v > 0x7fff)
			v = 0x7fff;
		p[i] = (int16_t)v;
	}
}

/* up to `frames` frames of codec 1 into dst, a scratch buffer at a time;
 * the frames delivered (fewer at the end of the stream) */
uint32_t Pcm_Decode(WmStream_t* s, int16_t* dst, uint32_t frames)
{
	uint32_t left = s->hdr.frames - s->pos;
	uint32_t bytes = (frames <= left ? frames : left) * s->blockAlign;
	uint32_t chunkBytes = WM_CHUNK_BYTES - WM_CHUNK_BYTES % s->blockAlign; // whole frames per pass
	uint32_t done = 0;
	while(bytes > 0)
	{
		uint32_t want = bytes < chunkBytes ? bytes : chunkBytes;
		uint32_t got = WmSource_Read(s->src, s->chunk, want);
		Wm_ApplyGain(s, s->chunk, got / 2);
		memcpy((uint8_t*)dst + done, s->chunk, got);
		done += got;
		bytes -= got;
		if(got != want)
			break;
	}
	s->pos += done / s->blockAlign;
	return done / s->blockAlign;
}
