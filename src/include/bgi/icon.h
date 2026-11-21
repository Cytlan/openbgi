/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * icon.h - a program's icon (src/base/icon.c): the one a game's Windows
 *          executable carries, as an .ico file and as pictures, and
 *          OpenBGI's own; not part of the original engine
 *
 * The launcher keeps the icon of a game's own program and the window
 * takes it while the game runs (src/sys/launcher.c, OpenBGI_Window_t's
 * setIcon).  An .ico file is what Windows keeps an icon in: several
 * pictures of the same thing, one for each size it is shown at.
 */
#ifndef BGI_ICON_H_
#define BGI_ICON_H_

#include "bgi/common.h"

/* The icon of the Windows program `exe` (`n` bytes): its first group of
 * icons - the one Windows shows for the file - with the pictures the
 * group names, as the bytes of an .ico file (BGI_Free them), `*size` of
 * them.  NULL when the bytes are no such program or it has no icon. */
uint8_t* Icon_FromExe(const uint8_t* exe, size_t n, size_t* size);

// 1 when the bytes begin as a Windows program does ("MZ" and, where it says, "PE")
int Icon_IsExe(const uint8_t* exe, size_t n);

// a picture of an icon
typedef struct IconImage
{
	int w, h;
	uint8_t* bgra; // h rows of w pixels: blue, green, red, alpha (not multiplied in); BGI_Free
} IconImage_t;

/* The pictures of the .ico file `ico` (`n` bytes): one for each size it
 * has, the one with the most colours where a size comes twice, the
 * largest first, at most `max` of them into `out`; their number.  The
 * pictures may be bitmaps of 1, 4, 8, 24 or 32 bits or PNG files. */
int Icon_Decode(const uint8_t* ico, size_t n, IconImage_t* out, int max);
void Icon_Free(IconImage_t* images, int count);

/* OpenBGI's own icon, which the engine carries for a window whose game
 * and whose host bring none: its pictures (256, 48, 32 and 16 pixels),
 * the largest first, at most `max` of them into `out`; their number. */
int Icon_Own(IconImage_t* out, int max);

#endif // BGI_ICON_H_
