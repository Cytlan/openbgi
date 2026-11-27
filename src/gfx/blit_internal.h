/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * blit_internal.h - what the blitter files share (src/gfx/blit*.c)
 *
 * The public interface is src/include/bgi/gfx/bitmap.h.  The blitters read and
 * write pixels as 16-bit RGB 5-5-5 or 32-bit BGRx / BGRA words by the
 * bitmap's mode, and never clip: the callers (Bmp_Blit and the display
 * objects) hand them rectangles that fit.
 */
#ifndef BGI_GFX_BLIT_INTERNAL_H_
#define BGI_GFX_BLIT_INTERNAL_H_

#include "bgi/gfx/bitmap.h"

// pack a lane to a byte with unsigned saturation (packuswb)
static inline uint8_t Sat8(int v)
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255
										 : v);
}

// the first byte of row y of a bitmap
#define ROW(b, y) ((b)->pixels + (size_t)(y) * (size_t)(b)->pitch)

/* ---- several lanes in one word ----------------------------------------------------------------
 *
 * The blitters' arithmetic is the original's, lane by lane: a byte of
 * colour widened to 16 bits, multiplied, shifted, packed back.  The
 * commonest of those sums can be worked on two or three lanes at once in
 * an ordinary 32-bit word, bit for bit the same, because no lane's
 * intermediate value outgrows its 16 bits:
 *
 *     x + (((y - x) * w) >> n)  =  (x * (2^n - w) + y * w) >> n
 *
 * for bytes x, y and 0 <= w <= 2^n (the shift of a negative difference
 * rounds down, as the shift of the whole sum does, and x * 2^n has no
 * bits below 2^n), and the right side is at most 255 * 2^n: no lane
 * borrows from its neighbour and none needs the saturation.  tests/
 * blitgold.c holds every loop written this way to the bytes the
 * lane-by-lane loop gave.
 */

// the red and blue lanes of a pixel (bits 0 .. 7 and 16 .. 23), and the green one
#define LANES_RB  0x00ff00ffu
#define LANES_G   0x0000ff00u

#if UINTPTR_MAX > 0xffffffffu

// the three colour bytes of a pixel 16 bits apart in a 64-bit word (blue, red, green from the lowest), and back
#define LANES_3 0x000000ff00ff00ffull
static inline uint64_t Spread3(uint32_t x)
{
	return (x & LANES_RB) | ((uint64_t)(x & LANES_G) << 24);
}
static inline uint32_t Pack3(uint64_t v)
{
	return (uint32_t)v | (uint32_t)(v >> 24);
}

/* x + (((y - x) * w) >> 7) on the blue, green and red bytes of two pixels,
 * 0 <= w <= 128; the top byte comes out 0 */
static inline uint32_t Lerp7(uint32_t x, uint32_t y, uint32_t w)
{
	return Pack3(((Spread3(x) * (128 - w) + Spread3(y) * w) >> 7) & LANES_3);
}

// the same with eight bits of weight: x + (((y - x) * w) >> 8), 0 <= w <= 256
static inline uint32_t Lerp8(uint32_t x, uint32_t y, uint32_t w)
{
	return Pack3(((Spread3(x) * (256 - w) + Spread3(y) * w) >> 8) & LANES_3);
}

#else // a machine of 32-bit words: two lanes to a word, and the third

static inline uint32_t Lerp7(uint32_t x, uint32_t y, uint32_t w)
{
	uint32_t v = 128 - w;
	return ((((x & LANES_RB) * v + (y & LANES_RB) * w) >> 7) & LANES_RB) | ((((x & LANES_G) * v + (y & LANES_G) * w) >> 7) & LANES_G);
}

static inline uint32_t Lerp8(uint32_t x, uint32_t y, uint32_t w)
{
	uint32_t v = 256 - w;
	return ((((x & LANES_RB) * v + (y & LANES_RB) * w) >> 8) & LANES_RB) | ((((x & LANES_G) * v + (y & LANES_G) * w) >> 8) & LANES_G);
}

#endif

// (x * w) >> n on the three colour bytes, for a product of at most 16 bits a lane (w <= 2^n + 1); the top byte comes out 0
static inline uint32_t Scale3(uint32_t x, uint32_t w, int n)
{
	return ((((x & LANES_RB) * w) >> n) & LANES_RB) | ((((x & LANES_G) * w) >> n) & LANES_G);
}

// x + y on each of the four bytes, 255 at most (paddusb)
static inline uint32_t AddSat4(uint32_t x, uint32_t y)
{
	uint32_t sum = ((x & 0x7f7f7f7fu) + (y & 0x7f7f7f7fu)) ^ ((x ^ y) & 0x80808080u); // each byte's sum, the carries cut off
	uint32_t over = ((x & y) | ((x | y) & ~sum)) & 0x80808080u;                       // the bytes that carried
	return sum | ((over >> 7) * 0xffu);
}

// the rows of a blit: the build's fast loops when the arguments are in the range they ask for, the plain ones otherwise (blit_rows.c)
#define ROWS_FOR(inRange) ((inRange) ? &kBlitRows : &kBlitRowsPlain)

#endif // BGI_GFX_BLIT_INTERNAL_H_
