/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * picture.h - the picture files a script reads or writes outside the
 *             archives ("90 C4" / "90 C5", 1.553 on): BMP, PNG, JPEG and
 *             GIF in (src/base/picture.c, src/base/jpeg.c, png.h), BMP,
 *             JPEG and PNG out.  The original hands these to GDI+.
 *
 * A decoded picture is B, G, R, A per pixel, rows from the top, with its
 * kind: the pixel format GDI+ reports for such a file, which decides how
 * "90 C4" with mode -1 creates the bitmap.  Indexed pictures (8-bit
 * palettes, GIF, grey JPEG) keep their palette indices as well.
 */
#ifndef BGI_PICTURE_H_
#define BGI_PICTURE_H_

#include "bgi/common.h"

typedef enum PicKind
{
	PIC_OTHER = 0,    // a format "90 C4" mode -1 refuses (16 bpp, 1 / 4-bit indices, 16-bit samples, 32 bpp without alpha)
	PIC_RGB24 = 1,    // 24-bit colour, opaque
	PIC_ARGB32 = 2,   // 32-bit colour with alpha
	PIC_INDEXED8 = 3, // 8-bit palette indices (`index`)
} PicKind_t;

typedef struct Picture
{
	uint32_t width;  // pixels
	uint32_t height; // pixels
	PicKind_t kind;
	uint8_t* bgra;  // height rows of width * 4 bytes (BGI_Free)
	uint8_t* index; // PIC_INDEXED8: height rows of width palette indices (BGI_Free); NULL otherwise
} Picture_t;

/* Decode a BMP, PNG, JPEG (baseline or progressive, grey or YCbCr) or GIF
 * (its first frame) of `size` bytes, recognised by its content; 1 ok, 0
 * when the content is none of these or is damaged */
int Picture_Decode(const uint8_t* data, size_t size, Picture_t* out);
void Picture_Free(Picture_t* p);

/* the JPEG decoder alone (Picture_Decode uses it): 1 ok, 0 not a JPEG it
 * reads (arithmetic coding, lossless, CMYK) or damaged */
int Jpeg_Decode(const uint8_t* data, size_t size, Picture_t* out);

/* Encode `h` rows of `pitch` bytes, `w` pixels of B, G, R, x each (the
 * fourth byte ignored), as a file (BGI_Free the result, `*size` bytes):
 * a baseline JFIF JPEG with 2x2 chroma subsampling and the IJG tables
 * scaled to `quality` (1 .. 100), a 24-bit BMP, or a 24-bit PNG.  NULL
 * for an empty picture (and for a JPEG wider or higher than 65535). */
uint8_t* Jpeg_Encode(const uint8_t* bgrx, uint32_t w, uint32_t h, size_t pitch, int quality, size_t* size);
uint8_t* Bmp_Encode24(const uint8_t* bgrx, uint32_t w, uint32_t h, size_t pitch, size_t* size);
uint8_t* Png_Encode24(const uint8_t* bgrx, uint32_t w, uint32_t h, size_t pitch, size_t* size);

#endif // BGI_PICTURE_H_
