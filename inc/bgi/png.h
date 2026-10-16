/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * png.h - reading and writing PNG files, for the tools that turn the
 *         games' pictures into files an image editor opens and back
 *         (dump.h); not part of the original engine.  Implemented in
 *         src/core/png.c on the zlib streams of deflate.h.
 *
 * Pictures are exchanged in the engine's own layout: rows from the top,
 * one byte per channel, a pixel being 1 byte (grey), 3 bytes (blue,
 * green, red) or 4 bytes (blue, green, red, alpha) - what the bit depths
 * 8, 24 and 32 of a decoded image file hold.  The writer produces 8-bit
 * greyscale, RGB or RGBA files without interlacing; the reader takes any
 * PNG (every colour type and bit depth, palettes, transparency chunks,
 * Adam7 interlacing) and reduces it to that layout.
 */
#ifndef BGI_PNG_H_
#define BGI_PNG_H_

#include "bgi/common.h"

typedef struct PngImage
{
	uint32_t width;  // pixels
	uint32_t height; // pixels
	int channels;    // bytes per pixel: 1 grey, 3 B G R, 4 B G R A
	uint8_t* pixels; // height rows of width * channels bytes, no padding (free() it)
} PngImage_t;

/* The PNG file of a picture (free() the result), `*size` bytes.  `pixels`
 * holds `height` rows of `pitch` bytes of which the first width * channels
 * are the pixels; `level` is the compression level of Deflate_Compress.
 * NULL when memory runs out or `channels` is not 1, 3 or 4. */
uint8_t* Png_Encode(const uint8_t* pixels, uint32_t width, uint32_t height, int channels, size_t pitch, int level, size_t* size);

/* Decode a PNG file of `size` bytes into `out` (free() out->pixels).  The
 * channels follow the file: greyscale gives 1, colour (or a palette) 3,
 * anything with an alpha channel or a transparency chunk 4; samples of 16
 * bits keep their high byte, those of fewer than 8 are scaled up.  Returns
 * 1 on success; 0 with a message in `err` (`errSize` bytes) when the file
 * is not a PNG, is damaged or uses something the format does not define. */
int Png_Decode(const uint8_t* data, size_t size, PngImage_t* out, char* err, size_t errSize);

/* Convert a picture to `channels` bytes per pixel in place (the pixel
 * buffer is replaced): grey spreads into the colour channels, colour
 * becomes grey by its luma, a new alpha channel is opaque and a dropped
 * one is discarded.  Returns 0 when nothing was lost, or the sum of
 * PNG_LOST_COLOUR (pixels that were not grey became grey) and
 * PNG_LOST_ALPHA (pixels that were not opaque lost their alpha). */
#define PNG_LOST_COLOUR 1
#define PNG_LOST_ALPHA  2
int Png_Convert(PngImage_t* img, int channels);

#endif // BGI_PNG_H_
