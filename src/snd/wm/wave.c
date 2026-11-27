/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wave.c - "A0 C0": a RIFF WAVE file played once through the mixer
 *          (Wm_PlayWave, src/include/bgi/snd/wavemaster.h); not part of the
 *          original library
 *
 * The original hands the file to PlaySound (SND_FILENAME | SND_ASYNC |
 * SND_NODEFAULT | SND_NOWAIT): Windows plays it beside DirectSound at
 * full volume, and a new one cuts off the one before.  Here it is a voice
 * of the mixer of its own: the PCM data becomes a BW file of 16-bit PCM
 * in memory, opened like a sound effect and played at level 0 (no
 * attenuation) in the middle.  The library's volumes, fades and holds do
 * not reach it.  PCM of 8 bits (unsigned) or 16 bits in one or two
 * channels is taken, also behind a WAVE_FORMAT_EXTENSIBLE header; the
 * compressed formats PlaySound would hand to a codec are refused.
 */
#include "wm_internal.h"
#include "bgi/core.h"
#include "bgi/bytes.h"

#include <stdlib.h>

#define WAVE_MAX_DATA 0x4000000 // 64 MB of samples at most

/* The format and the samples of a RIFF WAVE file: 1 and the fields, 0 when
 * it is not one the mixer can play.  The data chunk may be cut short by
 * the end of the file (the frames that are there are taken). */
static int Wave_Parse(const uint8_t* b, uint32_t size, uint32_t* rate, uint32_t* channels, uint32_t* bits, const uint8_t** data,
	uint32_t* dataSize)
{
	uint32_t pos = 12, haveFmt = 0;
	*data = NULL;
	if(size < 12 || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0)
		return 0;
	while(pos + 8 <= size)
	{
		const uint8_t* c = b + pos;
		uint32_t len = Le32(c + 4), have = size - pos - 8;
		if(memcmp(c, "fmt ", 4) == 0 && len >= 16 && have >= 16)
		{
			uint16_t tag = Le16(c + 8);
			*channels = Le16(c + 10);
			*rate = Le32(c + 12);
			*bits = Le16(c + 22);
			if(tag == 0xfffe && len >= 40 && have >= 40)
				tag = Le16(c + 32); // WAVE_FORMAT_EXTENSIBLE: the sub-format GUID begins with the format tag
			if(tag != 1)
				return 0; // not PCM
			haveFmt = 1;
		}
		else if(memcmp(c, "data", 4) == 0)
		{
			*data = c + 8;
			*dataSize = len < have ? len : have;
			break;
		}
		if(len > size - pos - 8)
			break;
		pos += 8 + len + (len & 1); // chunks are padded to an even size
	}
	return haveFmt && *data && *rate > 0 && *rate <= 192000 && (*channels == 1 || *channels == 2) && (*bits == 8 || *bits == 16);
}

int Wm_PlayWave(OpenBGI_t* o, const void* file, uint32_t size)
{
	const uint8_t* data;
	uint32_t rate = 0, channels = 0, bits = 0, dataSize = 0, frames, i, n; // (Wave_Parse fills them in when it returns 1)
	uint8_t* bw;
	int16_t* pcm;
	BwHeader_t* h;
	WmSource_t* src;
	WmStream_t* s;
	int err;
	if(!o->snd.audioRunning || !Wave_Parse((const uint8_t*)file, size, &rate, &channels, &bits, &data, &dataSize))
		return 0;
	if(dataSize > WAVE_MAX_DATA)
		dataSize = WAVE_MAX_DATA;
	frames = dataSize / (channels * (bits / 8));
	n = frames * channels;
	bw = (uint8_t*)BGI_Calloc(sizeof(BwHeader_t) + (size_t)n * 2);
	h = (BwHeader_t*)bw;
	h->headerSize = sizeof(BwHeader_t);
	memcpy(h->magic, "bw  ", 4);
	h->dataSize = n * 2;
	h->frames = frames;
	h->rate = rate;
	h->channels = channels;
	h->codec = BW_CODEC_PCM;
	pcm = (int16_t*)(bw + sizeof(BwHeader_t));
	for(i = 0; i < n; i++)
		pcm[i] = bits == 8 ? (int16_t)((data[i] - 128) * 256) : (int16_t)Le16(data + (size_t)i * 2);
	src = WmSource_FromMemory(bw, h->headerSize + h->dataSize);
	BGI_Free(bw);
	s = WmStream_Open(o, src, 1.0, 1, &err);
	if(!s)
		return 0;
	SndOut_Enter(o);
	Voice_Attach(&o->snd.waveVoice, s); // the wave before stops
	Voice_Play(&o->snd.waveVoice, 0);
	SndOut_Leave(o);
	return 1;
}
