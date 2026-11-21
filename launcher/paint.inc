// This file is part of OpenBGI (https://openbgi.net).
// SPDX-License-Identifier: GPL-2.0-only
//
// paint.inc - how the launcher (launcher.bpc) draws: with gray patterns.
//
// The program has no pictures of its own.  What it shows it makes when it starts, the way a
// game makes a transition: a gray bitmap says where, and how much.  OpenBGI's
// obgi.generate_pattern draws such a bitmap by rule - a ramp, rings, a clock sweep, noise,
// tiles, a polygon, a line, shaded triangles (docs/modding/obgi.md) - and from there on everything is
// the engine's own:
//
//   gfx1.apply_gray        the gray bitmap becomes the alpha channel of a bitmap filled with
//                          one colour: a shape in that colour (ink)
//   gfx0.bmp_blit          .. which goes over another bitmap (stamp)
//   gfx0.sprite_set_wipe   a sprite appears through a pattern, the darker parts first
//   gfx0.bg_blend          the background changes through one
//
// A shape is drawn into the scratch bitmap BMP_MASK (mask_new, pat_...), given a colour
// (ink_to) and put where it belongs (stamp).  mask_in makes the pattern in a bitmap of its
// own instead, for the ones that are kept.

#define PM_GRAY         3           // a pixel mode of "90 11": one byte, a gray level

#define BMP_MASK        8           // scratch: the pattern being made
#define BMP_INK         9           // scratch: a colour with that pattern as its alpha

// the kinds of pattern
#define PAT_LINEAR      0           // angle, v0, v1, repeat, flags
#define PAT_RADIAL      1           // cx, cy, rx, ry, v0, v1, repeat, flags
#define PAT_SWEEP       2           // cx, cy, angle, v0, v1, repeat, flags
#define PAT_NOISE       3           // seed, cellW, cellH, v0, v1, octaves, flags
#define PAT_TILES       4           // shape, w, h, jitter, inner, angle, seed, flags
#define PAT_POLYGON     5           // value, x0, y0, x1, y1, ...     (1/256 pixel)
#define PAT_LINE        6           // value, width, closed, x0, y0, ...
#define PAT_TRIANGLES   7           // x, y, v of each corner, three corners to a triangle

// how a pattern meets what the bitmap holds
#define PAT_SET         0
#define PAT_LIGHTEN     1
#define PAT_DARKEN      2
#define PAT_ADD         3
#define PAT_SUBTRACT    4
#define PAT_MULTIPLY    5

// flags
#define PAT_MIRROR      1           // a ramp runs up and down again
#define PAT_SMOOTH      2           // .. eased at both ends
#define PAT_DITHER      4           // .. its steps broken up
#define PAT_POLAR       8           // noise around the centre: rays
#define PAT_TURNED      16          // every tile's own ramp runs its own way

// the tiles
#define TILE_RECT       0
#define TILE_COLUMNS    1
#define TILE_ROWS       2
#define TILE_BRICKS     3
#define TILE_TRIANGLES  4
#define TILE_DIAMONDS   5
#define TILE_HEXAGONS   6
#define TILE_CELLS      7

#define G_PAINT         0x110000
global int gPat[16] @ G_PAINT;                  // the numbers of a ramp, of noise, of tiles
global int gLineHead[3] @ G_PAINT + 0xff4;      // a line: value, width, closed - and then its points
global int gPolyHead[1] @ G_PAINT + 0xffc;      // a polygon: value - and then its points
global int gPts[4096] @ G_PAINT + 0x1000;       // points: x, y in 1/256 pixel (or a triangle's x, y, v)
global int gMask @ G_PAINT + 0x40;              // the bitmap the pattern is made in,
global int gMaskW @ G_PAINT + 0x44;             // .. and its size
global int gMaskH @ G_PAINT + 0x48;

// A new pattern of w x h, black, in bitmap bmp.
mask_in(bmp, w, h) {
    gMask = bmp;
    gMaskW = imax(w, 2);
    gMaskH = imax(h, 2);
    gfx0.bmp_create(bmp, gMaskW, gMaskH, PM_GRAY);
    gfx0.bmp_clear(bmp, 0);
}

// .. in the scratch bitmap.
mask_new(w, h) {
    mask_in(BMP_MASK, w, h);
}

// The polygon of the first n points of gPts into the pattern.
pat_poly(mode, value, n) {
    gPolyHead[0] = value;
    obgi.generate_pattern(gMask, PAT_POLYGON, mode, gPolyHead, 1 + 2 * n);
}

// The line through the first n points of gPts, `width` wide (1/256 pixel).
pat_line(mode, value, width, closed, n) {
    gLineHead = {value, width, closed};
    obgi.generate_pattern(gMask, PAT_LINE, mode, gLineHead, 3 + 2 * n);
}

// The first n triangles of gPts (nine numbers each).
pat_triangles(mode, n) {
    obgi.generate_pattern(gMask, PAT_TRIANGLES, mode, gPts, 9 * n);
}

// A ramp across the pattern in the direction `angle` (degrees).
pat_linear(mode, angle, v0, v1, repeat, flags) {
    gPat = {angle * 65536, v0, v1, repeat, flags};
    obgi.generate_pattern(gMask, PAT_LINEAR, mode, gPat, 5);
}

// v0 at (cx, cy), v1 from the ellipse of the radii rx, ry on.
pat_radial(mode, cx, cy, rx, ry, v0, v1, flags) {
    gPat = {cx, cy, rx, ry, v0, v1, 1, flags};
    obgi.generate_pattern(gMask, PAT_RADIAL, mode, gPat, 8);
}

// A clock hand's turn around (cx, cy) from `angle` (degrees), `repeat` times to the turn.
pat_sweep(mode, cx, cy, angle, v0, v1, repeat, flags) {
    gPat = {cx, cy, angle * 65536, v0, v1, repeat, flags};
    obgi.generate_pattern(gMask, PAT_SWEEP, mode, gPat, 7);
}

pat_noise(mode, seed, cellW, cellH, v0, v1, octaves, flags) {
    gPat = {seed, cellW, cellH, v0, v1, octaves, flags};
    obgi.generate_pattern(gMask, PAT_NOISE, mode, gPat, 7);
}

// Cut the pattern into tiles: each takes the value at its centre, up to +-jitter by chance,
// and a ramp of `inner` levels across itself.
pat_tiles(shape, w, h, jitter, inner, angle, seed, flags) {
    gPat = {shape, w, h, jitter, inner, angle * 65536, seed, flags};
    obgi.generate_pattern(gMask, PAT_TILES, 0, gPat, 8);
}

// Bitmap dst becomes the pattern in a colour (0xRRGGBB): the pattern is its alpha.
ink_to(dst, rgb) {
    gfx0.bmp_create(dst, gMaskW, gMaskH, PM_ARGB);
    gfx0.bmp_clear(dst, 0xff000000 | rgb);
    gfx1.apply_gray(dst, gMask, 0, 0);
}

// The pattern in a colour over bitmap dst, its corner at (x, y).
stamp(dst, x, y, rgb) {
    ink_to(BMP_INK, rgb);
    gfx0.bmp_blit(dst, x, y, BMP_INK, 0, 0);
}

// The outline of a box with rounded corners into gPts, the first at index `at`: the box
// (x, y, w, h) in pixels, pulled in by `inset` (1/256 pixel) on every side.  The number of
// points.
round_pts(at, x, y, w, h, r, inset) { int k; int i; int n; int steps; int a; int cx; int cy; int rr;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    n = at;
    if (r <= 0) {                   // plain corners
        gPts[n * 2] = x * 256 + inset;
        gPts[n * 2 + 1] = y * 256 + inset;
        gPts[n * 2 + 2] = (x + w) * 256 - inset;
        gPts[n * 2 + 3] = y * 256 + inset;
        gPts[n * 2 + 4] = (x + w) * 256 - inset;
        gPts[n * 2 + 5] = (y + h) * 256 - inset;
        gPts[n * 2 + 6] = x * 256 + inset;
        gPts[n * 2 + 7] = (y + h) * 256 - inset;
        return 4;
    }
    steps = iclamp(r / 3, 4, 14);
    rr = r * 256 - inset;
    for (k = 0; k < 4; k = k + 1) { // the corners: lower right, lower left, upper left, upper right
        cx = (k == 0 || k == 3) ? (x + w - r) * 256 : (x + r) * 256;
        cy = (k < 2) ? (y + h - r) * 256 : (y + r) * 256;
        for (i = 0; i <= steps; i = i + 1) {
            a = (k * 90 + i * 90 / steps) * 65536;
            gPts[n * 2] = cx + muldiv(rr, cos(a), 65536);
            gPts[n * 2 + 1] = cy + muldiv(rr, sin(a), 65536);
            n = n + 1;
        }
    }
    return n - at;
}

// A box with rounded corners in a colour (0xAARRGGBB) over what bitmap bmp holds.
box(bmp, x, y, w, h, r, col) { int n;
    if (w < 1 || h < 1) return;
    mask_new(w, h);
    n = round_pts(0, 0, 0, w, h, r, 0);
    pat_poly(PAT_SET, (col >> 24) & 255, n);
    stamp(bmp, x, y, col & 0xffffff);
}

// The outline of a w x h box with corners of radius r, t thick, just inside the box's edge.
ring(bmp, w, h, r, t, col) { int n;
    mask_new(w, h);
    n = round_pts(0, 0, 0, w, h, r, t * 128);
    pat_line(PAT_SET, (col >> 24) & 255, t * 256, 1, n);
    stamp(bmp, 0, 0, col & 0xffffff);
}

// The polygon of the first n points of gPts - in the pixels of bitmap bmp - in a colour
// (0xAARRGGBB) over what the bitmap holds.
poly(bmp, n, col) {
    gfx0.bmp_info(gBmpInfo, bmp);
    mask_new(gBmpInfo[2], gBmpInfo[3]);
    pat_poly(PAT_SET, (col >> 24) & 255, n);
    stamp(bmp, 0, 0, col & 0xffffff);
}

// The line through those points, `width` wide (1/256 pixel), likewise.
line(bmp, n, width, col, closed) {
    gfx0.bmp_info(gBmpInfo, bmp);
    mask_new(gBmpInfo[2], gBmpInfo[3]);
    pat_line(PAT_SET, (col >> 24) & 255, width, closed, n);
    stamp(bmp, 0, 0, col & 0xffffff);
}

// Bitmap bmp becomes w x h in one colour whose alpha runs from a0 to a1 in the direction
// `angle` (degrees: 0 left to right, 90 top to bottom).
shade(bmp, w, h, angle, a0, a1, rgb) {
    mask_new(w, h);
    pat_linear(PAT_SET, angle, a0, a1, 1, PAT_DITHER);
    ink_to(bmp, rgb);
}
