/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * huffman.c - codec 2 of the BW format: 8-bit ADPCM codes compressed with
 *             a Huffman tree stored in the file (bw_internal.h)
 *
 * The file holds a 0x400-byte tree after the header (a 32-bit root, then
 * two int16 children per inner node; values below 0x100 are the symbols)
 * and the bit stream, consumed MSB first.  The stream keeps a window of
 * coded bytes (WM_CODE_CHUNK) that is topped up as the reader drains it.
 */
#include "bw_internal.h"

void Huffman_SetData(Huffman_t* h, const uint8_t* data, uint32_t bitPos)
{
	h->data = data;
	h->bitPos = bitPos;
}

// the next symbol: walk the tree from the root, one bit per inner node; the bit position advances
int32_t Huffman_Decode(Huffman_t* h)
{
	const uint8_t* p = h->data + (h->bitPos >> 3);
	uint32_t mask = 0x80u >> (h->bitPos & 7);
	uint32_t used = 0;
	int32_t node;
	memcpy(&node, h->tree, 4);
	while(node >= 0x100)
	{
		int bit = (*p & mask) ? 1 : 0;
		int16_t child;
		memcpy(&child, h->tree + 4 + (node - 0x100) * 4 + bit * 2, 2);
		node = child;
		mask >>= 1;
		if(mask == 0)
		{
			mask = 0x80;
			p++;
		}
		used++;
	}
	h->bitPos += used;
	return node;
}

// `count` codes out of the bit stream, refilling the coded window as it drains
static void Huffman_Codes(WmStream_t* s, uint8_t* out, uint32_t count)
{
	uint32_t i;
	int eof = 0; // the source ran dry during this call: no further refills
	for(i = 0; i < count; i++)
	{
		uint32_t bitsLeft = s->codedBytes * 8 - s->huff.bitPos;
		if(bitsLeft < 0x100 && !eof)
		{ // slide the unread tail to the front and top the window up
			uint32_t byte = s->huff.bitPos >> 3, bit = s->huff.bitPos & 7, got;
			s->codedBytes -= byte;
			memmove(s->coded, s->coded + byte, s->codedBytes);
			got = WmSource_Read(s->src, s->coded + s->codedBytes, WM_CODE_CHUNK - s->codedBytes);
			if(got < WM_CODE_CHUNK - s->codedBytes)
				eof = 1;
			s->codedBytes += got;
			Huffman_SetData(&s->huff, s->coded, bit);
		}
		out[i] = (uint8_t)Huffman_Decode(&s->huff);
	}
}

/* Decode up to `frames` frames of codec 2 into dst; the frames delivered
 * (fewer at the end of the stream).  The codes of a chunk are pulled out
 * of the bit stream first, then run through the ADPCM step, stereo
 * alternating the channel state per sample. */
uint32_t Huffman_DecodeFrames(WmStream_t* s, int16_t* dst, uint32_t frames)
{
	uint32_t left = s->hdr.frames - s->pos;
	uint32_t n = frames <= left ? frames : left;
	uint32_t perChunk = s->codeBufSize / s->channels, done = 0; // frames per pass through the code buffer
	while(done < n)
	{
		uint32_t chunk = perChunk < n - done ? perChunk : n - done, i;
		int ch = 0;
		Huffman_Codes(s, s->codeBuf, chunk * s->channels);
		for(i = 0; i < chunk * s->channels; i++)
		{
			s->chunk[i] = Adpcm7_Step(s->codeBuf[i], &s->adpcm[ch], s->hdr.stepParam);
			if(s->channels == 2)
				ch ^= 1;
		}
		Wm_ApplyGain(s, s->chunk, chunk * s->channels);
		memcpy(dst + (size_t)done * s->channels, s->chunk, (size_t)chunk * s->blockAlign);
		done += chunk;
	}
	s->pos += done;
	return done;
}

/* the codec-2 restart: the bit reader at the header's loop position with
 * a fresh window (the ADPCM states are the caller's business) */
void Huffman_Restart(WmStream_t* s)
{
	WmSource_Seek(s->src, 0x448 + (s->hdr.loopBit >> 3)); // header, 8 bytes, the 0x400-byte tree, then the data
	s->codedBytes = WmSource_Read(s->src, s->coded, WM_CODE_CHUNK);
	Huffman_SetData(&s->huff, s->coded, s->hdr.loopBit & 7);
}
