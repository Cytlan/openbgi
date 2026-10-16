/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bw.h - the "BW" sound container of the Wave Master library: its header,
 *        the sources a stream reads from and the stream that decodes it
 *        (src/snd/bw/: source.c, stream.c and one file per codec).
 *
 * A BW file is a 0x40-byte header (BwHeader_t below) followed by the coded
 * data of one of four codecs: 0 = 4-bit ADPCM, 1 = 16-bit PCM, 2 =
 * Huffman-coded 8-bit ADPCM, 3 = Ogg Vorbis.  The header carries the
 * sample rate and channel count, the length in frames, the loop flag and
 * restart frame with the ADPCM states at that point, and - for codec 2 -
 * the bit position of the loop restart and the step shift parameter.
 *
 * The library reads the data through a "source" (a loose file, an archive
 * entry, or a block of memory) and decodes it with a "stream" object that
 * hands out 16-bit frames.  Sound effects are decoded completely into
 * memory when they are loaded; music is decoded on demand.
 */
#ifndef BGI_SND_BW_H_
#define BGI_SND_BW_H_

#include "bgi/common.h"

typedef struct BwHeader
{
	uint32_t headerSize;  // 0x40
	char magic[4];        // "bw  " (two spaces)
	uint32_t dataSize;    // coded bytes after the header
	uint32_t frames;      // decoded length in frames (samples per channel)
	uint32_t rate;        // sample rate in Hz
	uint32_t channels;    // 1 or 2
	uint32_t loopFlag;    // non-zero: the stream restarts at `loopStart` when its end is reached
						  //     (how background music loops; the loop specification that sound
						  //     scripts test by reading the header with "81 30")
	uint32_t loopStart;   // the restart position in frames
	int32_t loopState[4]; // the ADPCM states at `loopStart`: ch0 predictor, ch0 step, ch1 predictor, ch1 step
	uint32_t codec;       // BW_CODEC_*
	uint32_t reserved1;   // unused
	uint32_t loopBit;     // codec 2: bit offset of the loop restart in the coded data
	uint32_t stepParam;   // codec 2: shift parameter of the step adaption
} BwHeader_t;

#define BW_CODEC_ADPCM4  0 // 4-bit ADPCM, two codes per byte (sound effects)
#define BW_CODEC_PCM     1 // 16-bit little-endian PCM
#define BW_CODEC_HUFFMAN 2 // 8-bit ADPCM codes behind a Huffman tree stored in the file
#define BW_CODEC_VORBIS  3 // an Ogg Vorbis stream behind the header

// ---- sources (a file, a block of memory, an archive entry) ----------------------
typedef struct WmSource WmSource_t;
WmSource_t* WmSource_OpenFile(const char* path);                     // NULL when the file cannot be opened
WmSource_t* WmSource_OpenArc(const char* arcPath, const char* name); // NULL when the archive or the entry is missing
WmSource_t* WmSource_FromMemory(const void* data, uint32_t size);    // a copy of `data` (NULL: `size` zero bytes)
void WmSource_Close(WmSource_t* s);                                  // NULL is accepted
uint32_t WmSource_Size(const WmSource_t* s);                         // bytes in the source
uint32_t WmSource_Read(WmSource_t* s, void* dst, uint32_t n);        // bytes read; fewer than `n` at the end
void WmSource_Seek(WmSource_t* s, uint32_t pos);                     // clamped to the size
uint32_t WmSource_Tell(const WmSource_t* s);                         // the read position in bytes

// ---- streams --------------------------------------------------------------------
typedef struct WmStream WmStream_t;

/* errors thrown (as ints) by the stream constructors; the library turns
 * them into its return codes */
#define WM_STREAM_ERR_SHORT   0x10000003 // source shorter than a header
#define WM_STREAM_ERR_MAGIC   0x11000001 // not "bw  "
#define WM_STREAM_ERR_CODEC   0x11000004 // the codec does not match the class
#define WM_STREAM_ERR_DECODER 0x10000000 // the decoder could not be set up

/* Create the stream for a source whose header says which codec it holds;
 * `gain` multiplies every decoded sample (the engine passes 1.0).  A
 * sound-effect stream (`wholeFile`) decodes everything into memory at
 * once; a music stream decodes as it is read.  The source is owned by the
 * stream from here on.  Returns NULL with *err set to a WM_STREAM_ERR_*
 * code on failure (the source is then released). */
WmStream_t* WmStream_Open(WmSource_t* src, double gain, int wholeFile, int* err);
/* An intro / loop pair of Vorbis streams (both headers must say codec 3,
 * else WM_STREAM_ERR_CODEC): the intro plays once, then the loop part
 * repeats while `mode` is non-zero (WmStream_SetMode).  Both sources are
 * owned by the stream, and released on failure. */
WmStream_t* WmStream_OpenPair(WmSource_t* intro, WmSource_t* loop, double gain, int* err);
void WmStream_Close(WmStream_t* s); // NULL is accepted

uint32_t WmStream_Rate(const WmStream_t* s);            // Hz
uint32_t WmStream_Channels(const WmStream_t* s);        // 1 or 2
uint32_t WmStream_Frames(const WmStream_t* s);          // total length (both parts of a pair)
const BwHeader_t* WmStream_Header(const WmStream_t* s); // the header of the current part

/* decode up to `frames` frames of 16-bit interleaved samples; fewer means
 * the end of the (current part of the) stream was reached */
uint32_t WmStream_Decode(WmStream_t* s, int16_t* dst, uint32_t frames);
/* what the decoder pump does when a decode comes up short: 1 = the stream
 * continues after WmStream_Restart() (a looping file, the loop part of a
 * pair, or a pair's intro handing over to its loop), 0 = it has ended */
int WmStream_Continues(const WmStream_t* s);
void WmStream_Restart(WmStream_t* s);           // back to the loop start (a pair's intro: on to the loop part)
void WmStream_Rewind(WmStream_t* s);            // everything back to the start (stop / reload)
void WmStream_SetMode(WmStream_t* s, int mode); // pair loop mode: non-zero repeats the loop part
/* override the header's loop flag / start of a single stream (the
 * "same file as intro and loop" form of the pair openers) */
void WmStream_SetLoop(WmStream_t* s, int flag, int32_t start);
/* the number of loop restarts, minus one while the last restart
 * is less than 4 seconds old */
int WmStream_LoopCount(const WmStream_t* s);

/* a linear fade-in over `ms` applied to the decoded samples of a
 * whole-file stream; 0 ok, -1 when the stream is not a whole-file one or
 * the fade is longer than the sound */
int WmStream_FadeIn(WmStream_t* s, uint32_t ms);
/* the decoded samples of a whole-file stream (16-bit interleaved,
 * Frames() frames); NULL for a music stream */
const int16_t* WmStream_Samples(const WmStream_t* s);

#endif // BGI_SND_BW_H_
