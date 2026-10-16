/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bw_internal.h - shared between the files of the BW stream decoder
 *                 (src/snd/bw: stream.c, source.c and the codecs); the
 *                 public interface is inc/bgi/snd/bw.h
 *
 * A BW file is a 0x40-byte header (BwHeader_t) followed by the coded data
 * of one of four codecs.  WmStream_t holds the decoder state of all of
 * them; the codec files provide an open / decode / restart set each that
 * stream.c dispatches on the header's codec number.
 */
#ifndef BGI_SND_BW_INTERNAL_H
#define BGI_SND_BW_INTERNAL_H

#include "bgi/snd/bw.h"
#include "bgi/os.h"
#include "bgi/error.h"

#if defined(BGI_HAVE_VORBISFILE)
#include <vorbis/vorbisfile.h>
#elif defined(BGI_USE_STB_VORBIS)
#define STB_VORBIS_HEADER_ONLY
#include "../stb_vorbis.c" // dropped into src/snd by the builder, see the Makefile
#endif

#define WM_CHUNK_BYTES 0x100000 // the decode scratch buffer of a stream, in bytes
#define WM_CODE_CHUNK  0x400    // codec 2: coded bytes kept in memory

// ---- source.c --------------------------------------------------------------------------------

#define WM_SRC_FILE    0 // a loose file
#define WM_SRC_MEMORY  1 // a block of memory owned by the source
#define WM_SRC_ARC     2 // an entry of a "PackFile" archive

struct WmSource
{
	int kind;       // WM_SRC_*
	OsFile_t* file; // file / archive
	uint32_t base;  // archive: entry offset in the file
	uint32_t size;  // bytes available
	uint32_t pos;   // read position inside the entry / memory
	uint8_t* mem;   // memory: the data (a whole-file stream's PCM)
};

// ---- adpcm.c ---------------------------------------------------------------------------------

typedef struct AdpcmState
{
	int32_t pred; // the last output, not clamped
	int32_t step; // the current step size, 0x7f .. 0x6000
} AdpcmState_t;

// Decode one 4-bit code (bit 3 sign, bits 0..2 magnitude); the clamped 16-bit sample.
int16_t Adpcm4_Step(uint32_t nibble, AdpcmState_t* st);
// Decode one 8-bit code (bit 7 sign, bits 0..6 magnitude) of codec 2; `param` is the header's step shift.
int16_t Adpcm7_Step(uint32_t code, AdpcmState_t* st, uint32_t param);

// ---- huffman.c -------------------------------------------------------------------------------

/* The Huffman decoder of the codec-2 files: a 0x400-byte tree stored in
 * the file - a 32-bit root, then for every inner node i >= 0x100 two
 * int16 children at byte 4 + (i - 0x100) * 4, values below 0x100 being
 * the symbols.  Bits are consumed MSB first. */
typedef struct Huffman
{
	uint8_t tree[0x400];
	const uint8_t* data; // the coded bytes being read
	uint32_t bitPos;     // the next bit of `data`
} Huffman_t;

// Point the bit reader at `data`, `bitPos` bits in.
void Huffman_SetData(Huffman_t* h, const uint8_t* data, uint32_t bitPos);
// The next symbol of the bit stream.
int32_t Huffman_Decode(Huffman_t* h);

// ---- the stream ------------------------------------------------------------------------------

struct WmStream
{
	BwHeader_t hdr;                      // the header of the current part of a pair
	uint32_t rate, channels, blockAlign; // Hz, 1 or 2, bytes per frame
	double gain;                         // multiplies every decoded sample
	int wholeFile;                       // decoded completely at open (sound effects)
	WmSource_t* src;                     // the coded data (a pair: the current part)
	WmSource_t* decoded;                 // whole-file streams: the memory source holding the PCM
	uint32_t pos;                        // frames decoded of the current part
	// the loop fields of the header: what the file says, unless the "same file
	// as intro and loop" form of the pair openers overrode them (WmStream_SetLoop);
	// the pump restarts the stream at the end when the flag is set
	// (WmStream_Continues), which is how a single-file background music loops
	int loopFlag;
	int32_t loopStart;  // frames
	uint32_t loopCount; // restarts so far
	uint32_t loopGrace; // tick (ms) until which the last restart counts as "just now"
	int16_t* chunk;     // decode scratch (WM_CHUNK_BYTES)

	// codec 0 / 2
	AdpcmState_t adpcm[2]; // per channel
	uint8_t* codeBuf;      // coded bytes (codec 0) / decoded Huffman codes (codec 2)
	uint32_t codeBufSize;  // bytes
	int16_t pending;       // codec 0: the extra sample decoded past an odd request
	int hasPending;        // `pending` is valid
	Huffman_t huff;        // codec 2
	uint8_t* coded;        // codec 2: the WM_CODE_CHUNK window of coded bytes
	uint32_t codedBytes;   // valid bytes in the window

	// codec 3: one decoder per part of a pair
#if defined(BGI_HAVE_VORBISFILE)
	OggVorbis_File vf[2];
	int vfOpen[2]; // ov_open_callbacks succeeded (ov_clear is due)
#elif defined(BGI_USE_STB_VORBIS)
	stb_vorbis* vorbis[2];
	uint8_t* vorbisData[2];                   // stb_vorbis decodes from memory: the coded data is loaded
	int vorbisPending[2];                     // unused (the reader pulls whole frames out of stb_vorbis directly)
	int16_t* vorbisBlock[2];                  // unused
	int vorbisBlockLen[2], vorbisBlockPos[2]; // reset by the open / seek, never read
#endif

	// pair
	int pair;              // the stream is an intro / loop pair
	int current;           // 0 intro, 1 loop
	int mode;              // non-zero: the loop part repeats
	WmSource_t* part[2];   // intro, loop
	BwHeader_t partHdr[2]; // their headers
};

// ---- the codecs: `frames` frames of 16-bit PCM into dst; the frames delivered ----------------

// Apply the stream's gain to `samples` 16-bit samples in place (pcm.c).
void Wm_ApplyGain(const WmStream_t* s, int16_t* p, uint32_t samples);

uint32_t Pcm_Decode(WmStream_t* s, int16_t* dst, uint32_t frames);    // codec 1
uint32_t Adpcm4_Decode(WmStream_t* s, int16_t* dst, uint32_t frames); // codec 0

uint32_t Huffman_DecodeFrames(WmStream_t* s, int16_t* dst, uint32_t frames); // codec 2
void Huffman_Restart(WmStream_t* s);                                         // back to the header's loop position

// codec 3: `which` selects the decoder (the part of a pair)
int Vorbis_Open(WmStream_t* s, int which); // 1 ok
void Vorbis_Close(WmStream_t* s, int which);
void Vorbis_SeekFrame(WmStream_t* s, int which, uint32_t frame);
uint32_t Vorbis_ReadBytes(WmStream_t* s, int which, uint8_t* buf, uint32_t n); // raw PCM bytes, 0 at the end
uint32_t Vorbis_Decode(WmStream_t* s, int16_t* dst, uint32_t frames);

#endif
