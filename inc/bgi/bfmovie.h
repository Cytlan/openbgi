/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bfmovie.h - "BF_Movie_______" frame sequences (1.69/472 on; decoded by
 *             src/core/bfmovie.c, managed by src/gfx/bmseq.c)
 *
 * The container behind the "90 F4" .. "90 F7" instructions: a short
 * animation (an animated bust-shot, a CG sequence, an eye-catch) stored as
 * one key frame and delta frames, decoded straight into a bitmap slot.  The
 * frames of the games examined (PrismRhythm's data02007.arc) are of the
 * in-memory type 0.  Types 0x10000 / 0x10001 are the streamed variant
 * (frames read from the archive on demand), which has not been met in the
 * data and is not decoded here.
 *
 * File layout (little-endian, byte offsets):
 *   0x00 "BF_Movie_______\0"       0x10 type (0 here)
 *   0x14 width   0x18 height       0x1C bits per pixel (24 / 32)
 *   0x20 pixel mode of the target slot (1 RGB32, 2 ARGB32)
 *   0x24 frame rate                0x28 frame count
 *   0x2C .. 0x3F zero
 *   0x40 frame offsets (one per frame, from the start of the file; a frame
 *        ends where the next begins, the last at the end of the file)
 *
 * A frame: varint N, 256 varint Huffman weights, then the bit stream (bits
 * taken from the least significant end of each byte) of N symbols.  The N
 * bytes decoded are a 32-bit length L followed by a run-length stream of
 * alternating (varint fill, varint copy) pairs - the fill writes 0x80 - that
 * expands to L bytes.  Frame 0 holds the whole picture as rows padded to 4
 * bytes, each byte the difference to the average of its left and upper
 * neighbours (the CompressedBG filter); the other frames hold a bit per 8 x 8
 * block (set: the block changes), eight row masks per changed block and a
 * pixel stream in which each pixel of a changed block is either (mask bit
 * set) the previous frame's pixel plus a delta per channel, or (bit clear)
 * a byte 0x80 for a transparent pixel, else a (dx, dy) displacement copying
 * the previous frame's pixel at that offset.
 */
#ifndef BGI_BFMOVIE_H_
#define BGI_BFMOVIE_H_

#include "bgi/common.h"

#define BFMOVIE_MAGIC "BF_Movie_______" // 16 bytes including the NUL

typedef struct BfMovieHeader // file layout, 0x40 bytes plus the offset table
{
	char magic[16];
	uint32_t type;      // 0: frames in the file; 0x10000 / 0x10001: streamed (not decoded)
	uint32_t width;     // pixels
	uint32_t height;    // pixels
	uint32_t bpp;       // bits per pixel, 24 or 32
	uint32_t mode;      // the pixel mode the target slot must have: 1 RGB32, 2 ARGB32
	uint32_t fps;       // frames per second
	uint32_t frames;    // frame count
	uint32_t zero[5];   // zero, not read
	uint32_t offset[1]; // frame offsets from the start of the file (`frames` entries)
} BfMovieHeader_t;

// 1 when the magic matches and the header with its offset table fits in `size`
int BfMovie_Check(const void* data, uint32_t size);
int BfMovie_IsSimple(const void* data); // 1 for type 0, the only type decodable here

/* decode frame `frame` of the movie into `dst` (width x height pixels of
 * 4 bytes, `pitch` bytes per row; the target must have the movie's size,
 * and for a frame after the first must still hold the previous frame).
 * 1 on success, 0 when the frame index is out of range or the data is
 * malformed. */
int BfMovie_DecodeFrame(const void* data, uint32_t size, uint32_t frame, uint8_t* dst, int pitch);

/* The encoder of the archive tools (bfmovieenc.c): a movie of type 0 is
 * built from its pictures, one BfMovieEnc_AddFrame per frame in order -
 * `pixels` holds height rows of `pitch` bytes, a pixel being 4 bytes (blue,
 * green, red, alpha; the alpha of a 24-bit movie is ignored, its frames
 * decode with alpha 0).  BfMovieEnc_Begin gives NULL for a size of zero or
 * a depth other than 24 or 32; BfMovieEnc_Finish returns the file (BGI_Free
 * it), `*size` bytes, and frees the encoder.  The frames decode to exactly
 * the pictures given. */
typedef struct BfMovieEnc BfMovieEnc_t;
BfMovieEnc_t* BfMovieEnc_Begin(uint32_t width, uint32_t height, uint32_t bpp, uint32_t fps);
void BfMovieEnc_AddFrame(BfMovieEnc_t* e, const uint8_t* pixels, size_t pitch);
uint8_t* BfMovieEnc_Finish(BfMovieEnc_t* e, uint32_t* size);

#endif // BGI_BFMOVIE_H_
