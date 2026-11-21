/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_present.h - the game's picture laid on a canvas of another size, for
 *                a host whose screen is not the picture's (os_present.c)
 *
 * A host that shows the picture on a screen larger or smaller than the
 * game's makes a presenter, tells it the canvas - the screen, or whatever
 * the host hands on - and how to scale, and draws each frame through it:
 * the picture comes out in the middle of the canvas, on black, scaled by
 * the largest factor of the steps chosen that fits.
 *
 *     OsPresent_t* p = OsPresent_New();
 *     OsPresent_Setup(p, 2560, 1440, OS_PRESENT_POW2, OS_PRESENT_BICUBIC);
 *     ... each frame of the engine:
 *     OsPresent_Changed(p, y, h);            what the frame says changed
 *     ... each picture of the screen:
 *     canvas = OsPresent_Draw(p, pixels, w, h, pitch, bpp, &changed);
 *
 * Only what changed is scaled again: a frame that changed nothing costs
 * nothing, one that changed a line of text costs that line.  A picture of
 * another size, or another set-up, redraws everything.
 */
#ifndef BGI_HOST_OS_PRESENT_H
#define BGI_HOST_OS_PRESENT_H

#include <stdint.h>

// the steps the picture is scaled in
#define OS_PRESENT_POW2     0 // by powers of two: 1/8 .. 1/2, 1, 2, 4, 8
#define OS_PRESENT_WHOLE    1 // by whole numbers: 1, 2, 3, ..., and 1/2, 1/3, ... for a picture larger than the canvas
#define OS_PRESENT_FILL     2 // by whatever fills the canvas in one direction, the picture's proportions kept
#define OS_PRESENT_NONE     3 // not at all: the middle of a picture larger than the canvas
#define OS_PRESENT_STRETCH  4 // to the canvas exactly, whatever its proportions (a canvas made for the picture)

// how the pixels between the picture's are made
#define OS_PRESENT_BICUBIC  0 // Catmull-Rom, four pixels each way (more when shrinking)
#define OS_PRESENT_BILINEAR 1 // the two nearest each way, by their distance
#define OS_PRESENT_NEAREST  2 // the nearest one: hard edges

typedef struct OsPresent OsPresent_t;

OsPresent_t* OsPresent_New(void);
void OsPresent_Delete(OsPresent_t* p); // accepts NULL

/* the canvas (pixels) and the scaling of the pictures drawn from now on;
 * values out of range are taken as the nearest that makes sense */
void OsPresent_Setup(OsPresent_t* p, int canvasW, int canvasH, int steps, int filter);

/* rows y .. y + h - 1 of the picture changed since the last
 * OsPresent_Draw (h < 0: all of it); rows outside the picture are left
 * out when it is drawn */
void OsPresent_Changed(OsPresent_t* p, int y, int h);

/* The canvas, canvasW x canvasH pixels of 32 bits (0x00RRGGBB, a row after
 * the other without gaps), with the picture on it: `pixels` are its w x h
 * pixels, `pitch` bytes a row, `bpp` 32 (blue, green, red and a byte that
 * is not looked at) or 16 (5-5-5); NULL pixels give a black canvas.
 * *changed (when not NULL) is 0 when the canvas is as the call before left
 * it.  The canvas stays valid until the next call with this presenter;
 * NULL when memory ran out or nothing was set up. */
const uint32_t* OsPresent_Draw(OsPresent_t* p, const uint8_t* pixels, int w, int h, int pitch, int bpp, int* changed);

/* the rows of the canvas the last OsPresent_Draw wrote: *h of them from
 * *y on (none: 0; all of them after a new set-up, a picture of another
 * size or a canvas gone black) */
void OsPresent_Drawn(const OsPresent_t* p, int* y, int* h);

// where the last picture drawn lies on the canvas (its left, top, width and height there; 0 x 0 before the first)
void OsPresent_Place(const OsPresent_t* p, int* x, int* y, int* w, int* h);

/* a place on the canvas as a place in the picture, kept inside it; 1
 * when the place was on the picture, 0 beside it (or before a picture) */
int OsPresent_ToPicture(const OsPresent_t* p, int canvasX, int canvasY, int* x, int* y);

/* the size a w x h picture is drawn at on a canvasW x canvasH canvas with
 * those steps (larger than the canvas only with OS_PRESENT_NONE) */
void OsPresent_Size(int w, int h, int canvasW, int canvasH, int steps, int* outW, int* outH);

#endif
