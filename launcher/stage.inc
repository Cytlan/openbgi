// This file is part of OpenBGI (https://openbgi.net).
// SPDX-License-Identifier: GPL-2.0-only
//
// stage.inc - what moves behind the launcher (launcher.bpc).
//
// Two looks, one of them at a time, each in one of several colour schemes - which ones is
// decided by chance at every start, unless the player fixed the look ("Look" on the first
// screen):
//
//   waves    a dark gradient with two soft glows, ribbons of light drifting across it and
//            rising sparks
//   shapes   a bright gradient with slanted bands and a field of dots, a band sliding over
//            it and small figures - triangles, squares, rings - that drift, turn and fade
//
// The pictures are made here, out of gray patterns (paint.inc); what moves them is the
// engine as the games use it: every piece is a sprite with a blend mode (added light for
// the waves, plain blending for the shapes), and a thread of its own sends it on its way
// with the engine's tweens.  The ribbons are plain sprites that go to and fro
// (gfx0.obj_move with an easing curve); the other pieces are transformed sprites
// (gfx0.sprite_set_transform) of which only the "progress" moves (gfx0.obj_tween_full):
// a figure's turning and growing, and the place of a piece, which the engine can so put
// between two pixels ("Gliding" below).
//
// A change of the whole picture - the launcher appearing, another look, a game starting -
// goes through a wipe as the games' scene changes do (curtain): the picture that leaves
// is a sprite that the engine takes away through a gray pattern, the darker parts first
// (gfx0.sprite_set_wipe), and the pattern is made for the occasion - panes of glass,
// crystals, hexagons, a clock hand, blinds, diamonds, rays, a mosaic.
//
// The stage is kept cheap: what does not move is drawn into the background's own bitmap
// once, the moving pieces are few and mostly empty, and the picture is presented as often
// as the look can afford: the waves 60 times a second, the shapes - whose figures are all
// drawn anew for every picture - 30 times (WAVES_FPS, SHAPES_FPS).

#define THEME_WAVES     0
#define THEME_SHAPES    1
#define WAVE_PALETTES   8
#define SHAPE_PALETTES  6

#define RIBBONS         3
#define SPARKS          10
#define SHAPES          14
#define SHAPE_KINDS     8
#define WAVES_FPS       60          // the pictures a second while a look shows (gfx0.set_fps)
#define SHAPES_FPS      30
#define STAGE_FPS       60          // the rate the tweens are told (they go by the clock)
#define GLIDE_STEPS     16          // the legs of a glide that is slow at both ends
#define RIB_STEPS       80          // the pieces a ribbon's length is drawn in
#define WIPES           8           // the kinds of pattern a curtain goes through

#define BMP_LOW         10          // scratch: the lower half of the background's colours; the stage as it was
#define BMP_RIBBON      204         // + ribbon
#define BMP_SPARK       208         // + 0 .. 2: three sizes
#define BMP_BAND        212
#define BMP_SHAPE       216         // + kind
#define BMP_CURTAIN     230         // the picture that leaves in a change of the whole picture,
#define BMP_WIPE        231         // .. and the pattern it leaves through

#define PRIO_BAND       12
#define PRIO_RIBBON     20
#define PRIO_SPARK      30
#define PRIO_SHAPE      40
#define PRIO_STAGE      59          // up to here it is the stage,
#define PRIO_CURTAIN    60          // .. over which a curtain of the stage alone lies;
#define PRIO_VEIL       60000       // one over everything

#define G_STAGE         0x104000
global int gTheme @ G_STAGE + 0;            // the look in effect
global int gThemeSet @ G_STAGE + 4;         // the player's choice: 0 by chance, 1 waves, 2 shapes
global int gPal @ G_STAGE + 8;              // the colour scheme's number
global int gPalA @ G_STAGE + 12;            // its colours (0xRRGGBB): the ribbons, the bands
global int gPalB @ G_STAGE + 16;            // .. the lines on them, the figures
global int gPalC @ G_STAGE + 20;            // .. the glow, a second colour of figures
global int gAccent @ G_STAGE + 24;          // .. and what the launcher marks things with
global int gAccentHi @ G_STAGE + 28;        // .. lighter
global int gRibMargin @ G_STAGE + 32;       // how far a ribbon reaches beyond the picture on each side
global int gNextRib @ G_STAGE + 40;         // the numbers the threads take when they start
global int gStageGen @ G_STAGE + 44;        // counts the times the pieces were put in their places (stage_build)
global int gNextSpk @ G_STAGE + 48;
global int gNextShape @ G_STAGE + 56;
global int gShapeSize @ G_STAGE + 60;       // the figures' bitmaps are this many pixels square
global int gStageDue @ G_STAGE + 64;        // when the stage is made again for a new picture size (0: it is as it should be)
global int gStageUp @ G_STAGE + 68;         // the stage was made once
global int gPalBg[4] @ G_STAGE + 0x80;      // the background's four corners (0xAARRGGBB)
global int gRibY[RIBBONS] @ G_STAGE + 0x90;
global int gRibSpr[RIBBONS] @ G_STAGE + 0xa0;
global int gBandSpr @ G_STAGE + 0xc0;
global int gBandW @ G_STAGE + 0xd0;
global int gSpkSpr[SPARKS] @ G_STAGE + 0xe0;
global int gShapeSpr[SHAPES] @ G_STAGE + 0x110;
global int gCurtainSpr @ G_STAGE + 0x150;   // the curtain: its sprite,
global int gCurtainAt @ G_STAGE + 0x154;    // .. when it began to move (0: there is none; -1: at the next frame),
global int gCurtainMs @ G_STAGE + 0x158;    // .. for how long,
global int gCurtainFrom @ G_STAGE + 0x15c;  // .. and how much of it is there at first and at last (0 .. 0x100)
global int gCurtainTo @ G_STAGE + 0x160;
global int gBandAtX @ G_STAGE + 0x170;      // where the band stands between two glides
global int gRibTop[130] @ G_STAGE + 0x200;  // a ribbon's upper edge along its length (1/256 pixel),
global int gTh[130] @ G_STAGE + 0x500;      // .. and its thickness

// ---- the colours --------------------------------------------------------------------------

// The colour scheme gPal of the look gTheme into the globals.
palette() {
    if (gTheme == THEME_WAVES) {
        if (gPal == 0) {            // blue
            gPalBg = {0xff0a1a3a, 0xff12356b, 0xff050d1f, 0xff1a1040};
            gPalA = 0x3a7bff; gPalB = 0xa8ccff; gPalC = 0x2a60ff; gAccent = 0x4c8df6;
        } else if (gPal == 1) {     // teal
            gPalBg = {0xff06252b, 0xff0d4a4f, 0xff03141a, 0xff0a2a3a};
            gPalA = 0x2ed3c0; gPalB = 0xb0fff0; gPalC = 0x18a8a0; gAccent = 0x2ec4b6;
        } else if (gPal == 2) {     // violet
            gPalBg = {0xff1a0f3a, 0xff3a1c6b, 0xff0d081f, 0xff2a0f40};
            gPalA = 0x9b6bff; gPalB = 0xdcc8ff; gPalC = 0x7040e0; gAccent = 0x9b7bff;
        } else if (gPal == 3) {     // crimson
            gPalBg = {0xff3a0a14, 0xff6b1228, 0xff1f050a, 0xff40101a};
            gPalA = 0xff4a6a; gPalB = 0xffc0cc; gPalC = 0xe02848; gAccent = 0xf0506e;
        } else if (gPal == 4) {     // amber
            gPalBg = {0xff3a2206, 0xff6b420d, 0xff1f1203, 0xff40280a};
            gPalA = 0xffb030; gPalB = 0xffe6b0; gPalC = 0xe08a18; gAccent = 0xf0a030;
        } else if (gPal == 5) {     // green
            gPalBg = {0xff0a2a14, 0xff12562a, 0xff05160a, 0xff0a3020};
            gPalA = 0x48d878; gPalB = 0xc8ffd8; gPalC = 0x20a850; gAccent = 0x3fc468;
        } else if (gPal == 6) {     // pink
            gPalBg = {0xff3a0a2a, 0xff6b1250, 0xff1f0516, 0xff401038};
            gPalA = 0xff6ac0; gPalB = 0xffd0ec; gPalC = 0xe03898; gAccent = 0xf060b0;
        } else {                    // slate
            gPalBg = {0xff141a24, 0xff283444, 0xff0a0d12, 0xff1a2030};
            gPalA = 0x9ab0d0; gPalB = 0xe0e8f4; gPalC = 0x6080a8; gAccent = 0x7f9fd0;
        }
    } else {
        gPalB = 0xffffff;
        if (gPal == 0) {            // cyan
            gPalBg = {0xff16c7d9, 0xff7be0e6, 0xff0aa5c9, 0xff37d6c2};
            gPalA = 0xffffff; gPalC = 0x0b6f9c; gAccent = 0x12b8d0;
        } else if (gPal == 1) {     // pink
            gPalBg = {0xffff6fae, 0xffffa3c4, 0xffff4d8d, 0xffffb86b};
            gPalA = 0xffffff; gPalC = 0xc2256b; gAccent = 0xf0508e;
        } else if (gPal == 2) {     // lime
            gPalBg = {0xff7bdc5a, 0xffc8f06a, 0xff22b573, 0xff4fd6b4};
            gPalA = 0xffffff; gPalC = 0x157a4c; gAccent = 0x3fc45a;
        } else if (gPal == 3) {     // violet
            gPalBg = {0xff8b6cff, 0xffc59bff, 0xff5a4bdc, 0xffff8bd0};
            gPalA = 0xffffff; gPalC = 0x3b2aa8; gAccent = 0x9478ff;
        } else if (gPal == 4) {     // orange
            gPalBg = {0xffffa53c, 0xffffd35c, 0xffff6a3c, 0xffffc04a};
            gPalA = 0xffffff; gPalC = 0xb8401a; gAccent = 0xf08a2a;
        } else {                    // sky
            gPalBg = {0xff4aa8ff, 0xff9ad4ff, 0xff2f6fe0, 0xff6fe0ff};
            gPalA = 0xffffff; gPalC = 0x1c4aa8; gAccent = 0x4a9cf0;
        }
    }
    gAccentHi = rgb_mix(gAccent, 0xffffff, 110);
}

// Choose the look - the player's, else one by chance - and a colour scheme by chance.
stage_pick() {
    if (gThemeSet == 1) {
        gTheme = THEME_WAVES;
    } else if (gThemeSet == 2) {
        gTheme = THEME_SHAPES;
    } else {
        gTheme = sys.random(2);
    }
    if (gTheme == THEME_WAVES) {
        gPal = sys.random(WAVE_PALETTES);
    } else {
        gPal = sys.random(SHAPE_PALETTES);
    }
    palette();
}

// ---- the pictures -------------------------------------------------------------------------

// The n corners of a regular figure of radius r around (cx, cy), the first at angle a0
// (degrees), into gPts; all in pixels.
ngon(n, cx, cy, r, a0) { int i; int a;
    for (i = 0; i < n; i = i + 1) {
        a = (a0 + 360 * i / n) * 65536;
        gPts[i * 2] = cx * 256 + muldiv(r * 256, cos(a), 65536);
        gPts[i * 2 + 1] = cy * 256 + muldiv(r * 256, sin(a), 65536);
    }
}

// The background into the picture-sized bitmap bmp (which is in the picture's mode): four
// colours, one to a corner, running into each other - and on that what stands still: the
// glows of the waves; the slanted bands and the dots of the shapes.
stage_bg(bmp) { int i; int w; int x; int slant; int step; int gx; int gy; int r;
    // the upper edge's two colours, left to right
    gfx0.bmp_clear(bmp, gPalBg[0] & 0xffffff);
    mask_new(gW, gH);
    pat_linear(PAT_SET, 0, 0, 255, 1, PAT_DITHER);
    stamp(bmp, 0, 0, gPalBg[1] & 0xffffff);
    // the lower edge's likewise, and more of them the further down
    gfx0.bmp_create(BMP_LOW, gW, gH, PM_ARGB);
    gfx0.bmp_clear(BMP_LOW, 0xff000000 | (gPalBg[2] & 0xffffff));
    stamp(BMP_LOW, 0, 0, gPalBg[3] & 0xffffff);
    pat_linear(PAT_SET, 90, 0, 255, 1, PAT_DITHER);
    gfx1.apply_gray(BMP_LOW, BMP_MASK, 0, 0);
    gfx0.bmp_blit(bmp, 0, 0, BMP_LOW, 0, 0);
    gfx0.bmp_free(BMP_LOW);

    if (gTheme == THEME_WAVES) {
        mask_new(gW * 68 / 100, gH * 104 / 100);
        pat_radial(PAT_SET, gMaskW / 2, gMaskH / 2, gMaskW / 2, gMaskH / 2, 0x4c, 0, PAT_SMOOTH | PAT_DITHER);
        stamp(bmp, gW * 72 / 100 - gMaskW / 2, gH * 22 / 100 - gMaskH / 2, gPalC);
        mask_new(gW * 52 / 100, gH * 80 / 100);
        pat_radial(PAT_SET, gMaskW / 2, gMaskH / 2, gMaskW / 2, gMaskH / 2, 0x38, 0, PAT_SMOOTH | PAT_DITHER);
        stamp(bmp, gW * 14 / 100 - gMaskW / 2, gH * 80 / 100 - gMaskH / 2, gPalA);
        return;
    }
    mask_new(gW, gH);
    slant = gH * 35 / 100;
    for (i = 0; i < 3; i = i + 1) {
        w = gW * (9 + i * 7) / 100;
        x = gW * (8 + i * 31) / 100;
        gPts[0] = (x + slant) * 256;
        gPts[1] = 0;
        gPts[2] = (x + slant + w) * 256;
        gPts[3] = 0;
        gPts[4] = (x + w) * 256;
        gPts[5] = gH * 256;
        gPts[6] = x * 256;
        gPts[7] = gH * 256;
        pat_poly(PAT_LIGHTEN, 0x1c + i * 0x0a, 4);
    }
    step = px(26);
    for (gy = 0; gy < 11; gy = gy + 1) {        // dots that grow towards the lower right corner
        for (gx = 0; gx < 16; gx = gx + 1) {
            r = step * 4 / 10 * (gx + gy + 2) / 27;
            if (r >= 1) {
                ngon(12, gW - px(8) - (16 - gx) * step + step / 2, gH - px(8) - (11 - gy) * step + step / 2, r, 0);
                pat_poly(PAT_LIGHTEN, 0x50, 12);
            }
        }
    }
    stamp(bmp, 0, 0, gPalA);
}

// A ribbon of w x h: a band under a wavy line, thicker here and thinner there, bright along
// that line and fading out below it, with a fine line on its edge.  Every one comes out
// differently.
ribbon_make(bmp, w, h) { int n; int k; int p1; int f1; int a1; int p2; int f2; int a2; int p3; int f3; int t0; int t1; int m; int x0; int x1; int at;
    n = RIB_STEPS;
    p1 = sys.random(360);
    f1 = 200 + sys.random(260);
    a1 = h * (14 + sys.random(10)) / 100;
    p2 = sys.random(360);
    f2 = 500 + sys.random(500);
    a2 = h * (3 + sys.random(5)) / 100;
    p3 = sys.random(360);
    f3 = 300 + sys.random(400);
    t0 = h * (24 + sys.random(10)) / 100;
    t1 = t0 * (30 + sys.random(40)) / 100;
    m = (h - t0) / 2;
    for (k = 0; k <= n; k = k + 1) {
        gRibTop[k] = m * 256 + ((a1 * sin((p1 + f1 * k / n) * 65536)) >> 8) + ((a2 * sin((p2 + f2 * k / n) * 65536)) >> 8);
        gTh[k] = t0 * 256 + ((t1 * sin((p3 + f3 * k / n) * 65536)) >> 8);
    }
    // the band: two triangles to a piece, their value falling from the upper edge to the lower
    mask_new(w, h);
    for (k = 0; k < n; k = k + 1) {
        x0 = w * k / n * 256;
        x1 = w * (k + 1) / n * 256;
        at = k * 18;
        gPts[at] = x0;
        gPts[at + 1] = gRibTop[k];
        gPts[at + 2] = 0x74;
        gPts[at + 3] = x1;
        gPts[at + 4] = gRibTop[k + 1];
        gPts[at + 5] = 0x74;
        gPts[at + 6] = x1;
        gPts[at + 7] = gRibTop[k + 1] + gTh[k + 1];
        gPts[at + 8] = 0;
        gPts[at + 9] = x0;
        gPts[at + 10] = gRibTop[k];
        gPts[at + 11] = 0x74;
        gPts[at + 12] = x1;
        gPts[at + 13] = gRibTop[k + 1] + gTh[k + 1];
        gPts[at + 14] = 0;
        gPts[at + 15] = x0;
        gPts[at + 16] = gRibTop[k] + gTh[k];
        gPts[at + 17] = 0;
    }
    pat_triangles(PAT_SET, 2 * n);
    ink_to(bmp, gPalA);
    // the line on its edge
    mask_new(w, h);
    for (k = 0; k <= n; k = k + 1) {
        gPts[k * 2] = w * k / n * 256;
        gPts[k * 2 + 1] = gRibTop[k];
    }
    pat_line(PAT_SET, 0xb0, imax(256, gScale * 4), 0, n + 1);
    stamp(bmp, 0, 0, gPalB);
}

// Figure k of the shapes, s pixels square.
shape_make(k, s) { int c; int r; int lw; int col;
    mask_new(s, s);
    c = s / 2;
    r = s * 42 / 100;
    lw = imax(512, s * 256 / 16);
    col = gPalB;
    if (k == 0) {                   // a triangle
        ngon(3, c, c, r, -90);
        pat_poly(PAT_SET, 0xb0, 3);
    } else if (k == 1) {            // a triangle's outline
        ngon(3, c, c, r, -90);
        pat_line(PAT_SET, 0xd0, lw, 1, 3);
    } else if (k == 2) {            // a square's outline
        ngon(4, c, c, r, 45);
        pat_line(PAT_SET, 0xd0, lw, 1, 4);
    } else if (k == 3) {            // a ring
        ngon(40, c, c, r, 0);
        pat_line(PAT_SET, 0xd0, lw, 1, 40);
    } else if (k == 4) {            // a disc
        ngon(40, c, c, r * 6 / 10, 0);
        pat_poly(PAT_SET, 0x90, 40);
    } else if (k == 5) {            // a hexagon's outline
        ngon(6, c, c, r, 0);
        pat_line(PAT_SET, 0xd0, lw, 1, 6);
    } else if (k == 6) {            // a triangle in the scheme's dark colour
        ngon(3, c, c, r, -90);
        pat_poly(PAT_SET, 0x70, 3);
        col = gPalC;
    } else {                        // a square in it
        ngon(4, c, c, r, 45);
        pat_poly(PAT_SET, 0x60, 4);
        col = gPalC;
    }
    ink_to(BMP_SHAPE + k, col);
}

// A slanted band, w wide, as high as the picture.
band_make(bmp, w, alpha) { int slant;
    mask_new(w, gH);
    slant = gH * 35 / 100;
    gPts[0] = slant * 256;
    gPts[1] = 0;
    gPts[2] = w * 256;
    gPts[3] = 0;
    gPts[4] = (w - slant) * 256;
    gPts[5] = gH * 256;
    gPts[6] = 0;
    gPts[7] = gH * 256;
    pat_poly(PAT_SET, alpha, 4);
    ink_to(bmp, gPalA);
}

// A spark: a spot of light, s pixels across.
spark_make(bmp, s) {
    mask_new(s, s);
    pat_radial(PAT_SET, s / 2, s / 2, s / 2, s / 2, 255, 0, PAT_SMOOTH);
    ink_to(bmp, gPalB);
}

// ---- the curtain --------------------------------------------------------------------------

// A pattern as large as the picture into BMP_WIPE: one of several kinds, each a little
// different every time.
wipe_make() { int k; int a; int seed; int cx; int cy;
    k = sys.random(WIPES);
    a = sys.random(360);
    seed = sys.random(30000);
    cx = gW / 2;
    cy = gH / 2;
    mask_in(BMP_WIPE, gW, gH);
    if (k == 0) {                   // panes of glass, each going its own way
        pat_linear(PAT_SET, a, 40, 215, 1, 0);
        pat_tiles(TILE_TRIANGLES, px(128), px(128), 18, 80, 0, seed, PAT_TURNED);
    } else if (k == 1) {            // crystals, from the middle outwards
        pat_radial(PAT_SET, cx, cy, gW * 6 / 10, gH * 6 / 10, 20, 235, 0);
        pat_tiles(TILE_CELLS, px(72), px(72), 28, 0, 0, seed, 0);
    } else if (k == 2) {            // hexagons
        pat_linear(PAT_SET, a, 24, 231, 1, 0);
        pat_tiles(TILE_HEXAGONS, px(88), 0, 24, 0, 0, seed, 0);
    } else if (k == 3) {            // a clock hand
        pat_sweep(PAT_SET, cx, cy, a, 0, 255, 1, 0);
    } else if (k == 4) {            // blinds, one after the other
        pat_linear(PAT_SET, 78, 0, 255, 1, 0);
        pat_tiles(TILE_COLUMNS, imax(gW / 14, 8), 0, 0, 0, 0, seed, 0);
    } else if (k == 5) {            // diamonds, towards the middle
        pat_radial(PAT_SET, cx, cy, gW * 6 / 10, gH * 6 / 10, 235, 20, 0);
        pat_tiles(TILE_DIAMONDS, px(96), px(96), 14, 0, 0, seed, 0);
    } else if (k == 6) {            // rays
        pat_noise(PAT_SET, seed, 7, px(320), 0, 150, 2, PAT_POLAR | PAT_SMOOTH);
        pat_radial(PAT_ADD, cx, cy, gW * 6 / 10, gH * 6 / 10, 0, 105, 0);
    } else {                        // a mosaic
        pat_linear(PAT_SET, a, 30, 225, 1, 0);
        pat_tiles(TILE_RECT, px(64), px(64), 30, 0, 0, seed, 0);
    }
}

// Bitmap BMP_CURTAIN - a picture as large as the launcher's - becomes a curtain at priority
// prio that goes from `from` to `to` (how much of it is there: 0 .. 0x100) within ms
// milliseconds, through a new pattern.  curtain_step moves it.
curtain(prio, from, to, ms) {
    wipe_make();
    gfx0.sprite_set_wipe(gCurtainSpr, 0, 0, BMP_CURTAIN, BMP_WIPE, 2, from, FX_BLEND, 0, prio);
    gfx0.sprite_show(gCurtainSpr, 1);
    gCurtainFrom = from;
    gCurtainTo = to;
    gCurtainMs = ms;
    gCurtainAt = -1;
}

// A black picture for the curtain.  (A wipe shows its pattern's in-between levels only for
// a picture with an alpha channel.)
curtain_black() {
    gfx0.bmp_create(BMP_CURTAIN, gW, gH, PM_ARGB);
    gfx0.bmp_clear(BMP_CURTAIN, 0xff000000);
}

// A frame of the curtain; 1 while it moves.
curtain_step() { int t;
    if (gCurtainAt == 0) return 0;
    if (gCurtainAt == -1) {         // its time begins with the first picture that shows it
        gCurtainAt = sys.get_ticks();
        if (gCurtainAt == 0 || gCurtainAt == -1) gCurtainAt = 1;
    }
    t = sys.get_ticks() - gCurtainAt;
    if (t >= gCurtainMs) {
        gCurtainAt = 0;
        gfx0.obj_set_progress(gCurtainSpr, gCurtainTo);
        if (gCurtainTo == 0) {      // gone: so are its pictures
            gfx0.sprite_show(gCurtainSpr, 0);
            gfx0.bmp_free(BMP_CURTAIN);
            gfx0.bmp_free(BMP_WIPE);
        }
        return 0;
    }
    gfx0.obj_set_progress(gCurtainSpr, gCurtainFrom + (gCurtainTo - gCurtainFrom) * t / gCurtainMs);
    return 1;
}

// ---- the stage ----------------------------------------------------------------------------

// The sprites of everything, hidden; the look and the colours.  Once, before the first picture.
stage_init() { int i; char s[16];
    for (i = 0; i < RIBBONS; i = i + 1) gRibSpr[i] = new_sprite();
    for (i = 0; i < SPARKS; i = i + 1) gSpkSpr[i] = new_sprite();
    for (i = 0; i < SHAPES; i = i + 1) gShapeSpr[i] = new_sprite();
    gBandSpr = new_sprite();
    gCurtainSpr = new_sprite();
    gThemeSet = 0;
    if (OBGI_CFG_GET("", "look", 0, s, 16) > 0) {
        if (streq(s, "waves")) gThemeSet = 1;
        if (streq(s, "shapes")) gThemeSet = 2;
    }
    stage_pick();
}

// Start the threads that move the pieces.  Once, after the first stage_build.
stage_start() { int i;
    for (i = 0; i < RIBBONS; i = i + 1) sys81.spawn_borrower(0x100, 0x100, 0x1000, ribbon_thread);
    for (i = 0; i < SPARKS; i = i + 1) sys81.spawn_borrower(0x100, 0x100, 0x1000, spark_thread);
    sys81.spawn_borrower(0x100, 0x100, 0x1000, band_thread);
    for (i = 0; i < SHAPES; i = i + 1) sys81.spawn_borrower(0x100, 0x100, 0x1000, shape_thread);
}

// Make the pieces of the look in effect for the picture as it is now and put them in their
// places; the other look's pieces are hidden.
stage_build() { int i; int w; int h;
    gStageDue = 0;
    gStageGen = gStageGen + 1;      // (a glide under way ends where this puts its piece)
    if (gTheme == THEME_WAVES) {
        gfx0.set_fps(WAVES_FPS);
        gRibMargin = gW / 5;
        w = gW + 2 * gRibMargin;
        h = gH * 45 / 100;
        for (i = 0; i < RIBBONS; i = i + 1) {
            ribbon_make(BMP_RIBBON + i, w, h);
            gRibY[i] = gH * (10 + i * 20) / 100;
            gfx0.sprite_set(gRibSpr[i], 0 - gRibMargin, gRibY[i], BMP_RIBBON + i, 2, 0xc0, PRIO_RIBBON + i);
            gfx0.sprite_show(gRibSpr[i], 1);
        }
        for (i = 0; i < 3; i = i + 1) {
            spark_make(BMP_SPARK + i, px(14 + i * 14));
        }
        for (i = 0; i < SPARKS; i = i + 1) {
            glide_set(gSpkSpr[i], 0, 0, BMP_SPARK + i % 3, 2, 0, PRIO_SPARK + i);
            gfx0.sprite_show(gSpkSpr[i], 1);
        }
        gfx0.sprite_show(gBandSpr, 0);
        for (i = 0; i < SHAPES; i = i + 1) gfx0.sprite_show(gShapeSpr[i], 0);
    } else {
        gfx0.set_fps(SHAPES_FPS);
        gBandW = gW * 30 / 100;
        band_make(BMP_BAND, gBandW, 0x26);
        gBandAtX = 0 - gBandW;
        glide_set(gBandSpr, gBandAtX, 0, BMP_BAND, 1, 0, PRIO_BAND);
        gfx0.sprite_show(gBandSpr, 1);
        gShapeSize = px(104);
        for (i = 0; i < SHAPE_KINDS; i = i + 1) shape_make(i, gShapeSize);
        for (i = 0; i < SHAPES; i = i + 1) gfx0.sprite_show(gShapeSpr[i], 0);   // (their threads show them)
        for (i = 0; i < RIBBONS; i = i + 1) gfx0.sprite_show(gRibSpr[i], 0);
        for (i = 0; i < SPARKS; i = i + 1) gfx0.sprite_show(gSpkSpr[i], 0);
    }
    if (!gStageUp) {
        gStageUp = 1;
        stage_start();
    }
}

// The "Look" button: the next of by chance, waves, shapes - and other colours in any case.
// The stage as it was leaves through a curtain.
stage_cycle() {
    gThemeSet = (gThemeSet + 1) % 3;
    if (gThemeSet == 1) {
        OBGI_CFG_SET("", "look", 0, "waves");
    } else if (gThemeSet == 2) {
        OBGI_CFG_SET("", "look", 0, "shapes");
    } else {
        OBGI_CFG_SET("", "look", 0, 0);
    }
    gfx0.snapshot_prio(BMP_LOW, PRIO_STAGE);        // the stage as it is: a picture without an alpha channel,
    gfx0.bmp_create(BMP_CURTAIN, gW, gH, PM_ARGB);  // .. which a wipe wants: a plain copy gives it one
    gfx0.bmp_blit(BMP_CURTAIN, 0, 0, BMP_LOW, 0x80, 0);
    gfx0.bmp_free(BMP_LOW);
    stage_pick();
    stage_bg(BMP_BG);
    gfx0.bg_bitmap(BMP_BG);
    stage_build();
    curtain(PRIO_CURTAIN, 0x100, 0, 900);
}

// Wait for ms milliseconds.
nap(ms) {
    sys.timer_set(ms);
    sys.timer_wait();
}

// ---- gliding ------------------------------------------------------------------------------
//
// A sprite stands at a whole pixel, and a small piece that drifts a few pixels a second
// would be seen to go in steps.  A gliding piece is therefore a transformed sprite that is
// neither turned nor scaled: the engine draws such a sprite around an origin inside its bitmap,
// the origin may lie between two pixels, and the sprite's progress moves it (parameter
// 0x80 is how far, in 1/65536 pixel, over a progress of 0x100).  So the piece stands at a
// whole pixel before a glide and after it, and anywhere on the way.

// Make sprite h a gliding piece: bitmap bmp with its corner at (x, y).  (The scale is one
// 65536th above 1: a sprite that is exactly neither turned nor scaled is drawn the quick
// way, at whole pixels.)
glide_set(h, x, y, bmp, effect, level, prio) {
    gfx0.sprite_set_transform(h, x, y, bmp, 0, 0, 0, 0x10001, 0x10001, 1, effect, level, prio);
}

// Put the gliding piece h at (x, y).
glide_at(h, x, y) {
    gfx0.obj_set_progress(h, 0);
    gfx0.obj_set_pos(h, x, y);
}

// Glide the piece h from (x, y), where it stands, to (x2, y2) in ms milliseconds at an even
// pace, while its level becomes `level`.  0 when the pieces were put in their places
// meanwhile (stage_build): then it is not at (x2, y2), and whoever moves it starts again.
glide(h, x, y, x2, y2, level, ms) { int gen;
    gen = gStageGen;
    gfx0.obj_set_param(h, 0x80, (x - x2) * 65536, (y - y2) * 65536);
    gfx0.obj_tween_full(h, x, y, 0, level, 0, 0x100, ms, STAGE_FPS, 0, 0, 0);
    if (gen != gStageGen) return 0;
    glide_at(h, x2, y2);
    return 1;
}

// The same, slow at both ends: GLIDE_STEPS glides of the same length in time, whose shares
// of the way are those of a curve that starts and ends level (3u^2 - 2u^3), from the level
// l1 to l2.  (The engine's own easing is for a sprite's place, not for its progress.)
glide_ease(h, x, y, x2, y2, l1, l2, ms) { int k; int e; int ax; int ay; int bx; int by; int n;
    n = GLIDE_STEPS;
    ax = x;
    ay = y;
    for (k = 1; k <= n; k = k + 1) {
        e = k * k * (3 * n - 2 * k) * 1024 / (n * n * n);      // 0 .. 1024
        bx = x + (x2 - x) * e / 1024;
        by = y + (y2 - y) * e / 1024;
        if (!glide(h, ax, ay, bx, by, l1 + (l2 - l1) * k / n, ms / n)) return 0;
        ax = bx;
        ay = by;
    }
    return 1;
}

// ---- the threads --------------------------------------------------------------------------

// A ribbon: to and fro, at its own pace.  It goes sideways only, a whole pixel at a time:
// its edge is nearly level, so that a pixel sideways lifts or lowers it by a fraction of
// one, and no step is seen.  (A step up or down would be, and gliding a piece of this size
// costs three times the time the whole stage takes.)
ribbon_thread() { int me;
    me = gNextRib;
    gNextRib = gNextRib + 1;
    while (1) {
        if (gTheme != THEME_WAVES) {
            nap(300);
            continue;
        }
        gfx0.obj_move(gRibSpr[me], 0 - sys.random(2 * gRibMargin), gRibY[me], 1, 0x90 + sys.random(0x70), 12000 + sys.random(14000), STAGE_FPS, 0, 0);
    }
}

// A spark: it appears somewhere in the lower part, rises as it brightens, and fades.
spark_thread() { int me; int x; int y; int dx; int dy; int d;
    me = gNextSpk;
    gNextSpk = gNextSpk + 1;
    nap(sys.random(6000));
    while (1) {
        if (gTheme != THEME_WAVES) {
            nap(400);
            continue;
        }
        x = sys.random(gW);
        y = gH * 3 / 10 + sys.random(gH * 7 / 10);
        dx = sys.random(px(60)) - px(30);
        dy = px(30) + sys.random(px(70));
        d = 2200 + sys.random(2600);
        glide_at(gSpkSpr[me], x, y);
        gfx0.obj_set_level(gSpkSpr[me], 0);
        if (glide(gSpkSpr[me], x, y, x + dx, y - dy, 0x50 + sys.random(0xa0), d)) {
            glide(gSpkSpr[me], x + dx, y - dy, x + 2 * dx, y - 2 * dy, 0, d);
        }
        nap(sys.random(3000));
    }
}

// The band of the shapes: across the picture, and back.
band_thread() { int d; int x;
    while (1) {
        if (gTheme != THEME_SHAPES) {
            nap(300);
            continue;
        }
        d = 20000 + sys.random(14000);
        x = gW;
        if (gBandAtX > 0) x = 0 - gBandW;
        if (glide_ease(gBandSpr, gBandAtX, 0, x, 0, 0, 0, d)) {
            gBandAtX = x;
        } else {
            glide_at(gBandSpr, gBandAtX, 0);
        }
    }
}

// A figure of the shapes: it fades in somewhere, drifts while it turns and grows or shrinks,
// and fades out; then another one, elsewhere.  It drifts as the other pieces glide: its
// origin leaves the middle of its bitmap as the progress goes on, so that it swings out
// around the place it appeared at, between the pixels.
shape_thread() { int me; int h; int x; int y; int dx; int dy; int d; int s; int a; int c;
    me = gNextShape;
    gNextShape = gNextShape + 1;
    nap(sys.random(5000));
    while (1) {
        if (gTheme != THEME_SHAPES) {
            nap(300);
            continue;
        }
        h = gShapeSpr[me];
        c = gShapeSize / 2;
        s = 0x5000 + sys.random(0x16000);
        x = sys.random(gW);
        y = sys.random(gH);
        dx = sys.random(px(200)) - px(100);
        dy = sys.random(px(160)) - px(80);
        d = 4500 + sys.random(5500);
        a = (60 + sys.random(240)) * 65536;
        if (sys.random(2)) a = 0 - a;
        gfx0.sprite_set_transform(h, x, y, BMP_SHAPE + sys.random(SHAPE_KINDS), c, c, sys.random(360) * 65536, s, s, 1, 1, 0x100, PRIO_SHAPE + me);
        gfx0.obj_set_param(h, 0x80, 2 * dx * 65536 / (s / 256) * 256, 2 * dy * 65536 / (s / 256) * 256);  // (the bitmap's pixels: the way on the screen over the scale)
        gfx0.obj_set_param(h, 0x81, a, 0);
        gfx0.obj_set_param(h, 0x82, sys.random(0x8000) - 0x4000, sys.random(0x8000) - 0x4000);
        gfx0.obj_set_progress(h, 0);
        gfx0.sprite_show(h, 1);
        gfx0.obj_tween_full(h, x, y, 0, 0x20 + sys.random(0x70), 0, 0x80, d, STAGE_FPS, 0, 0, 0);
        gfx0.obj_tween_full(h, x, y, 0, 0x100, 0, 0x100, d, STAGE_FPS, 0, 0, 0);
        gfx0.sprite_show(h, 0);
        nap(sys.random(1500));
    }
}
