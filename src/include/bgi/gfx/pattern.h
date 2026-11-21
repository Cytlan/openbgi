/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pattern.h - gray patterns made by rule ("EF 30", obgi.generate_pattern)
 *
 * The games ship their masks and wipe patterns as gray pictures: ramps at
 * an angle, rings, clock sweeps, mosaics, crystal cells, bursts of rays,
 * cut-out shapes.  A program without pictures of its own has them made
 * here, into a GRAY8 bitmap, and uses them wherever the engine takes a
 * gray bitmap: as an alpha channel ("91 1E"), as the pattern of a wipe
 * ("90 19", "90 43", "90 5A"), as a mask ("90 55").
 *
 * A pattern is drawn into the bitmap over what it holds: `mode` says how a
 * pattern's value p meets the value d that is there (PAT_SET .. PAT_MULTIPLY),
 * and the shapes with an edge (polygon, line) do so by how much of a pixel
 * they cover.  Several patterns in a row build up a picture.
 *
 * What a kind reads at `p` (count values; all numbers are 32-bit integers):
 *
 *   PAT_LINEAR     angle, v0, v1, repeat, flags
 *       a ramp from v0 to v1 across the whole bitmap in the direction
 *       `angle` (16.16 degrees: 0 left to right, 90 top to bottom)
 *   PAT_RADIAL     cx, cy, rx, ry, v0, v1, repeat, flags
 *       v0 at (cx, cy), v1 on the ellipse with the radii rx, ry around it
 *       and beyond
 *   PAT_SWEEP      cx, cy, angle, v0, v1, repeat, flags
 *       a clock hand's turn around (cx, cy): v0 at `angle`, v1 after a full
 *       turn clockwise
 *   PAT_NOISE      seed, cellW, cellH, v0, v1, octaves, flags
 *       soft noise between v0 and v1 whose grain is cellW x cellH pixels
 *       (a long cell gives streaks), `octaves` (1 .. 6) layers of it, each
 *       half as coarse and half as strong; with PAT_POLAR the grain runs
 *       around the bitmap's centre - cellW is then in degrees - which gives
 *       rays
 *   PAT_TILES      shape, w, h, jitter, inner, angle, seed, flags
 *       cuts what the bitmap holds into tiles of about w x h pixels
 *       (PAT_TILE_*): every tile takes the value at its centre, plus a
 *       chance amount up to +-jitter, plus a ramp of `inner` gray levels
 *       across the tile in the direction `angle` - or, with PAT_TURNED, in
 *       a direction of the tile's own.  `mode` is not used.
 *   PAT_POLYGON    value, x0, y0, x1, y1, ...
 *       a filled polygon of at least 3 points (PAT_ONE to a pixel), smooth
 *       edged; where it winds around itself it stays filled
 *   PAT_LINE       value, width, closed, x0, y0, x1, y1, ...
 *       a line through the points, `width` wide (PAT_ONE to a pixel), with
 *       round ends and corners; `closed` joins the last point to the first
 *   PAT_TRIANGLES  x0, y0, v0, x1, y1, v1, x2, y2, v2, ...
 *       triangles whose value runs smoothly between those of their corners;
 *       their edges are not smoothed, so that triangles side by side leave
 *       no seam
 *
 * `repeat` (at least 1) runs a ramp that many times; the flags PAT_MIRROR
 * (up and down again within each run), PAT_SMOOTH (eased at both ends) and
 * PAT_DITHER (the steps between two gray levels broken up) apply to the
 * ramps, PAT_SMOOTH also to the noise.
 */
#ifndef BGI_GFX_PATTERN_H_
#define BGI_GFX_PATTERN_H_

#include "bgi/gfx/bitmap.h"

enum
{
	PAT_LINEAR = 0,
	PAT_RADIAL = 1,
	PAT_SWEEP = 2,
	PAT_NOISE = 3,
	PAT_TILES = 4,
	PAT_POLYGON = 5,
	PAT_LINE = 6,
	PAT_TRIANGLES = 7
};

enum // how a pattern's value p meets the value d in the bitmap
{
	PAT_SET = 0,      // p
	PAT_LIGHTEN = 1,  // the larger of the two
	PAT_DARKEN = 2,   // the smaller
	PAT_ADD = 3,      // d + p, at most 255
	PAT_SUBTRACT = 4, // d - p, at least 0
	PAT_MULTIPLY = 5  // d * p / 255
};

#define PAT_MIRROR 1
#define PAT_SMOOTH 2
#define PAT_DITHER 4
#define PAT_POLAR  8  // PAT_NOISE
#define PAT_TURNED 16 // PAT_TILES

enum
{
	PAT_TILE_RECT = 0,      // w x h rectangles
	PAT_TILE_COLUMNS = 1,   // columns w wide: only the place across is cut
	PAT_TILE_ROWS = 2,      // rows h high
	PAT_TILE_BRICKS = 3,    // rectangles, every other row shifted by half
	PAT_TILE_TRIANGLES = 4, // the rectangles cut along a diagonal, this way and that in turn
	PAT_TILE_DIAMONDS = 5,  // diamonds w wide and h high
	PAT_TILE_HEXAGONS = 6,  // hexagons w wide (h is not used)
	PAT_TILE_CELLS = 7      // crystal cells, about one to every w x h pixels
};

#define PAT_ONE        256  // a pixel, in the units of the points
#define PAT_POINTS_MAX 4096 // the most points a polygon or a line takes

// 1 when the pattern was drawn; 0 for a bitmap that is not GRAY8, an unknown kind or mode, or too few values
int Pattern_Generate(Bmp_t* b, int kind, int mode, const int32_t* p, int count);

#endif // BGI_GFX_PATTERN_H_
