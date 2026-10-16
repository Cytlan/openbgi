/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * blit_internal.h - what the blitter files share (src/gfx/blit*.c)
 *
 * The public interface is inc/bgi/gfx/bitmap.h.  The blitters read and
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

#endif // BGI_GFX_BLIT_INTERNAL_H_
