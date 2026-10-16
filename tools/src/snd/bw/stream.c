/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * stream.c - a BW stream: opening a file (or an intro / loop pair of
 *            files), decoding frames, and the loop and restart
 *            bookkeeping the player relies on (inc/bgi/snd/bw.h)
 *
 * The original models these as a class tree: a buffered PCM base with one
 * subclass per codec, each in a "whole file" flavour for sound effects
 * (decode everything into a memory source at open) and a streaming
 * flavour for music (decode on demand), plus a two-source subclass for an
 * intro / loop pair.  Here one WmStream_t carries the codec state and a
 * few flags instead; the codecs are in pcm.c, adpcm.c, huffman.c and
 * vorbis.c.
 */
#include "bw_internal.h"

// the header of a source into *hdr; 0, or the WM_STREAM_ERR_* code of the first check that fails
static int ReadHeader(WmSource_t* src, BwHeader_t* hdr)
{
	if(WmSource_Size(src) < 0x40)
		return WM_STREAM_ERR_SHORT;
	WmSource_Seek(src, 0);
	if(WmSource_Read(src, hdr, 0x40) != 0x40)
		return WM_STREAM_ERR_SHORT;
	if(memcmp(hdr->magic, "bw  ", 4) != 0)
		return WM_STREAM_ERR_MAGIC;
	if(hdr->codec > 3)
		return WM_STREAM_ERR_CODEC;
	return 0;
}

// take the format (16-bit, channels, rate) and the loop fields from a header
static void SetFormat(WmStream_t* s, const BwHeader_t* hdr)
{
	s->hdr = *hdr;
	s->rate = hdr->rate;
	s->channels = hdr->channels;
	s->blockAlign = 2 * hdr->channels;
	s->loopFlag = (int)hdr->loopFlag;
	s->loopStart = (int32_t)hdr->loopStart;
}

/* set the codec's state up for decoding from the start of the data:
 * buffers, fresh ADPCM states, the Huffman tree and the first window of
 * coded bytes, or the Vorbis decoder; 0, WM_STREAM_ERR_DECODER when the
 * Vorbis decoder cannot be opened */
static int CodecOpen(WmStream_t* s)
{
	switch(s->hdr.codec)
	{
		case BW_CODEC_ADPCM4:
			s->codeBufSize = (WM_CHUNK_BYTES + 2) >> 2; // a nibble per sample of the scratch buffer
			s->codeBuf = (uint8_t*)BGI_Alloc(s->codeBufSize);
			s->adpcm[0].pred = s->adpcm[1].pred = 0;
			s->adpcm[0].step = s->adpcm[1].step = 0x7f;
			s->hasPending = 0;
			WmSource_Seek(s->src, 0x40);
			return 0;
		case BW_CODEC_PCM:
			WmSource_Seek(s->src, 0x40);
			return 0;
		case BW_CODEC_HUFFMAN:
		{
			uint8_t extra[8];
			s->codeBufSize = WM_CHUNK_BYTES >> 1; // a code per sample of the scratch buffer
			s->codeBuf = (uint8_t*)BGI_Alloc(s->codeBufSize);
			s->coded = (uint8_t*)BGI_Alloc(WM_CODE_CHUNK);
			WmSource_Seek(s->src, 0x40);
			WmSource_Read(s->src, extra, 8); // 8 unused bytes precede the tree
			WmSource_Read(s->src, s->huff.tree, 0x400);
			s->codedBytes = WmSource_Read(s->src, s->coded, WM_CODE_CHUNK);
			Huffman_SetData(&s->huff, s->coded, 0);
			s->adpcm[0].pred = s->adpcm[1].pred = 0;
			s->adpcm[0].step = s->adpcm[1].step = 0x7f;
			return 0;
		}
		case BW_CODEC_VORBIS:
			return Vorbis_Open(s, 0) ? 0 : WM_STREAM_ERR_DECODER;
		default:
			return WM_STREAM_ERR_CODEC;
	}
}

// the codec's decoder: up to `frames` frames into dst, the frames delivered
static uint32_t DecodeImpl(WmStream_t* s, int16_t* dst, uint32_t frames)
{
	switch(s->hdr.codec)
	{
		case BW_CODEC_ADPCM4: return Adpcm4_Decode(s, dst, frames);
		case BW_CODEC_PCM: return Pcm_Decode(s, dst, frames);
		case BW_CODEC_HUFFMAN: return Huffman_DecodeFrames(s, dst, frames);
		default: return Vorbis_Decode(s, dst, frames);
	}
}

// a zeroed stream with its decode scratch buffer
static WmStream_t* StreamAlloc(void)
{
	WmStream_t* s = (WmStream_t*)BGI_Alloc(sizeof *s);
	memset(s, 0, sizeof *s);
	s->chunk = (int16_t*)BGI_Alloc(WM_CHUNK_BYTES);
	return s;
}

/* Open a stream on `src` (see bw.h).  A whole-file stream decodes every
 * frame into a memory source right here, pads a short decode with
 * silence and releases the coded source; its reads then come from the
 * PCM.  On failure *err holds the WM_STREAM_ERR_* code and the source is
 * closed. */
WmStream_t* WmStream_Open(WmSource_t* src, double gain, int wholeFile, int* err)
{
	BwHeader_t hdr;
	WmStream_t* s;
	int e = ReadHeader(src, &hdr);
	if(e)
	{
		WmSource_Close(src);
		*err = e;
		return NULL;
	}
	s = StreamAlloc();
	s->src = src;
	s->gain = gain;
	s->wholeFile = wholeFile;
	SetFormat(s, &hdr);
	e = CodecOpen(s);
	if(e)
	{
		WmStream_Close(s);
		*err = e;
		return NULL;
	}
	if(wholeFile)
	{ // everything into a memory source
		uint32_t bytes = hdr.frames * s->blockAlign;
		WmSource_t* mem = WmSource_FromMemory(NULL, bytes);
		uint32_t got = DecodeImpl(s, (int16_t*)mem->mem, hdr.frames);
		if(got < hdr.frames)
			memset(mem->mem + (size_t)got * s->blockAlign, 0, (size_t)(hdr.frames - got) * s->blockAlign);
		s->decoded = mem;
		WmSource_Close(s->src);
		s->src = NULL;
		s->pos = 0;
	}
	*err = 0;
	return s;
}

/* Open an intro / loop pair (see bw.h): both headers are read and must
 * say codec 3, both Vorbis decoders are opened up front, and the stream
 * starts on the intro with the intro's format.  On failure *err holds
 * the WM_STREAM_ERR_* code and both sources are closed. */
WmStream_t* WmStream_OpenPair(WmSource_t* intro, WmSource_t* loop, double gain, int* err)
{
	BwHeader_t h[2];
	WmStream_t* s;
	int e = ReadHeader(intro, &h[0]);
	if(!e)
		e = ReadHeader(loop, &h[1]);
	if(!e && (h[0].codec != h[1].codec || h[0].codec != BW_CODEC_VORBIS))
		e = WM_STREAM_ERR_CODEC; // both parts must be Vorbis
	if(e)
	{
		WmSource_Close(intro);
		WmSource_Close(loop);
		*err = e;
		return NULL;
	}
	s = StreamAlloc();
	s->pair = 1;
	s->gain = gain;
	s->part[0] = intro;
	s->part[1] = loop;
	s->partHdr[0] = h[0];
	s->partHdr[1] = h[1];
	SetFormat(s, &h[0]); // the intro's format
	// both decoders are opened up front; Vorbis_Open works on `src`, so it is pointed at each part in turn
	s->src = intro;
	if(!Vorbis_Open(s, 0))
		e = WM_STREAM_ERR_DECODER;
	s->src = loop;
	if(!e && !Vorbis_Open(s, 1))
		e = WM_STREAM_ERR_DECODER;
	s->src = intro;
	s->current = 0;
	if(e)
	{
		WmStream_Close(s);
		*err = e;
		return NULL;
	}
	*err = 0;
	return s;
}

// release the decoders, the sources (a pair's parts, the decoded PCM) and the buffers
void WmStream_Close(WmStream_t* s)
{
	if(!s)
		return;
	Vorbis_Close(s, 0);
	Vorbis_Close(s, 1);
	if(s->pair)
	{
		WmSource_Close(s->part[0]);
		WmSource_Close(s->part[1]);
	}
	else
		WmSource_Close(s->src);
	WmSource_Close(s->decoded);
	BGI_Free(s->codeBuf);
	BGI_Free(s->coded);
	BGI_Free(s->chunk);
	BGI_Free(s);
}

uint32_t WmStream_Rate(const WmStream_t* s)
{
	return s->rate;
}

uint32_t WmStream_Channels(const WmStream_t* s)
{
	return s->channels;
}

// the total length in frames: both parts of a pair
uint32_t WmStream_Frames(const WmStream_t* s)
{
	if(s->pair)
		return s->partHdr[0].frames + s->partHdr[1].frames;
	return s->hdr.frames;
}

const BwHeader_t* WmStream_Header(const WmStream_t* s)
{
	return &s->hdr;
}

const int16_t* WmStream_Samples(const WmStream_t* s)
{
	return s->decoded ? (const int16_t*)s->decoded->mem : NULL;
}

/* up to `frames` frames of 16-bit interleaved PCM into dst; the frames
 * delivered (fewer at the end of the current part) */
uint32_t WmStream_Decode(WmStream_t* s, int16_t* dst, uint32_t frames)
{
	if(s->decoded)
	{ // a whole-file stream: straight from the memory source
		uint32_t left = s->hdr.frames - s->pos, n = frames <= left ? frames : left;
		memcpy(dst, s->decoded->mem + (size_t)s->pos * s->blockAlign, (size_t)n * s->blockAlign);
		s->pos += n;
		return n;
	}
	return DecodeImpl(s, dst, frames);
}

/* whether a short decode means "restart" (1) or "ended" (0): a pair's
 * intro always goes on to the loop part, the loop part repeats while
 * `mode` is set, a single file restarts when its loop flag is set */
int WmStream_Continues(const WmStream_t* s)
{
	if(s->pair)
		return s->current == 0 ? 1 : s->mode != 0;
	return s->loopFlag;
}

/* override the loop flag and restart frame the header gave (the "same
 * file as intro and loop" form of the pair openers) */
void WmStream_SetLoop(WmStream_t* s, int flag, int32_t start)
{
	s->loopFlag = flag;
	s->loopStart = start;
}

// make part `idx` of a pair the current one: its source and its format
static void Pair_Switch(WmStream_t* s, int idx)
{
	if(s->current == idx)
		return;
	s->current = idx;
	s->src = s->part[idx];
	SetFormat(s, &s->partHdr[idx]);
}

/* The restart the pump performs when a decode comes up short and the
 * stream continues: a pair's intro hands over to the loop part, the
 * loop part (and a looping single file) goes back to its loop start.
 * Every restart but the intro hand-over counts as a loop and opens the
 * 4-second grace period of WmStream_LoopCount.  Per codec, the loop
 * start means: a PCM seek; a Vorbis frame seek; for the ADPCM codecs
 * the header's saved predictor / step states plus the coded position
 * (codec 0: the nibble position, with a pending high nibble when a mono
 * loop starts on an odd frame; codec 2: the loop bit position). */
void WmStream_Restart(WmStream_t* s)
{
	if(s->pair)
	{
		if(s->current == 0)
		{
			Pair_Switch(s, 1);
			s->pos = 0;
		}
		else
		{
			s->loopGrace = OS_TicksMs() + 4000;
			s->loopCount++;
			s->pos = 0;
			Vorbis_SeekFrame(s, 1, 0);
		}
		return;
	}
	s->loopGrace = OS_TicksMs() + 4000;
	s->loopCount++;
	s->pos = (uint32_t)s->loopStart;
	switch(s->hdr.codec)
	{
		case BW_CODEC_VORBIS:
			Vorbis_SeekFrame(s, 0, (uint32_t)s->loopStart);
			break;
		case BW_CODEC_PCM:
			WmSource_Seek(s->src, s->hdr.headerSize + (uint32_t)s->loopStart * s->blockAlign);
			break;
		case BW_CODEC_ADPCM4: // the header's states, the nibble position, a pending high nibble
			s->adpcm[0].pred = s->hdr.loopState[0];
			s->adpcm[0].step = s->hdr.loopState[1];
			s->adpcm[1].pred = s->hdr.loopState[2];
			s->adpcm[1].step = s->hdr.loopState[3];
			WmSource_Seek(s->src, 0x40 + ((s->channels * (uint32_t)s->loopStart) >> 2));
			if(s->channels == 1 && (s->loopStart & 1))
			{
				uint8_t b = 0;
				WmSource_Read(s->src, &b, 1);
				s->pending = Adpcm4_Step(b >> 4, &s->adpcm[0]);
				s->hasPending = 1;
			}
			break;
		case BW_CODEC_HUFFMAN:
			s->adpcm[0].pred = s->hdr.loopState[0];
			s->adpcm[0].step = s->hdr.loopState[1];
			s->adpcm[1].pred = s->hdr.loopState[2];
			s->adpcm[1].step = s->hdr.loopState[3];
			Huffman_Restart(s);
			break;
		default:
			break;
	}
}

/* Everything back to the start (a stop or a reload): the position, and
 * for a music stream the decoder state - a pair back on its intro with
 * both decoders at frame 0 and the loop count cleared, the ADPCM codecs
 * with fresh states (and codec 2's window refilled from the start of the
 * data), PCM and Vorbis at their first frame.  A whole-file stream only
 * resets its position. */
void WmStream_Rewind(WmStream_t* s)
{
	s->pos = 0;
	if(s->decoded)
		return;
	if(s->pair)
	{
		s->loopCount = 0;
		s->loopGrace = 0;
		Pair_Switch(s, 1);
		Vorbis_SeekFrame(s, 1, 0);
		Pair_Switch(s, 0);
		Vorbis_SeekFrame(s, 0, 0);
		return;
	}
	switch(s->hdr.codec)
	{
		case BW_CODEC_VORBIS: Vorbis_SeekFrame(s, 0, 0); break;
		case BW_CODEC_ADPCM4: // fresh states, pending cleared
			s->adpcm[0].pred = s->adpcm[1].pred = 0;
			s->adpcm[0].step = s->adpcm[1].step = 0x7f;
			s->pending = 0;
			s->hasPending = 0;
			WmSource_Seek(s->src, s->hdr.headerSize);
			break;
		case BW_CODEC_HUFFMAN: // states, the window from the start of the data
			s->adpcm[0].pred = s->adpcm[1].pred = 0;
			s->adpcm[0].step = s->adpcm[1].step = 0x7f;
			WmSource_Seek(s->src, 0x448); // header, 8 bytes, the tree
			s->codedBytes = WmSource_Read(s->src, s->coded, WM_CODE_CHUNK);
			Huffman_SetData(&s->huff, s->coded, 0);
			break;
		default: WmSource_Seek(s->src, s->hdr.headerSize); break;
	}
}

void WmStream_SetMode(WmStream_t* s, int mode)
{
	s->mode = mode;
}

/* the loop restarts so far, less one while the last one is under 4
 * seconds old (what "A0 15" reports as the length) */
int WmStream_LoopCount(const WmStream_t* s)
{
	int n = (int)s->loopCount;
	if(OS_TicksMs() < s->loopGrace)
		n--;
	return n;
}

/* A linear fade-in over `ms` burnt into the decoded samples of a
 * whole-file stream.  The fade length in frames is (ms / 1000) * rate +
 * ((ms % 1000) * rate) / 1000; each of the first `frames` frames is
 * scaled by i / frames with the division done in floating point and
 * truncated.  0 ok; -1 when the stream is not a whole-file one or the
 * fade is longer than the sound (nothing is changed then). */
int WmStream_FadeIn(WmStream_t* s, uint32_t ms)
{
	uint32_t frames, i, c;
	int16_t* p;
	if(!s->decoded)
		return -1;
	frames = (ms / 1000) * s->rate + ((ms % 1000) * s->rate) / 1000;
	if(frames > s->hdr.frames)
		return -1;
	p = (int16_t*)s->decoded->mem;
	for(i = 0; i < frames; i++)
		for(c = 0; c < s->channels; c++, p++)
			*p = (int16_t)BGI_Ftol((double)*p * (double)(int64_t)i / (double)(int64_t)frames);
	return 0;
}
