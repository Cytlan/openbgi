# BF_Movie sequences and the video textures (1.69/472 on)

The builds from TayutamaHD (1.69/472) on carry a second kind of movie
next to the DirectShow player of `90 F0`: a *frame sequence* in the
engine's own "BF_Movie" container, decoded straight into a bitmap slot by
the engine.  The system programs use it for everything that moves inside
a still picture - the animated bust-shots of `bsctrl._bp` (blinking,
mouth movement), the motion manager (`mtnmngrsfo._bp`), the sprite groups
(`scrgrp2 .. 4._bp`), the "touch" reactions (`touches._bp` of
Tayutama2AS) and a frame-by-frame movie manager (`moviemngrbm._bp`).  The
files are `*.bm` entries of the `data02xxx.arc` picture archives
(PrismRhythm's `data02007.arc` holds seven of them, 300 KB .. 4 MB each;
the samples examined are 744x432 .. 800x600, 15 .. 30 frames at 30 fps).

The decoder is `src/core/bfmovie.c`, the registry and the instructions
`src/gfx/bmseq.c` / `src/vm/ops_gfx0.c`, the loader waits are in
`src/wait/wait_load.c`; `tests/bfmovie.c` checks all of it with an encoder
written from the decoder's inverse (and, with `BGI_BM_FILE=<file>`,
decodes a real movie).

## The container

    +00  "BF_Movie_______\0"
    +10  type: 0 (all files seen); 0x10000 / 0x10001 the streamed form
    +14  width          +18 height
    +1C  bits per pixel (24 or 32)
    +20  pixel mode the target slot must have (1 RGB32 for 24 bpp, 2 ARGB32 for 32)
    +24  frames per second    +28 frame count
    +2C .. +3F  zero
    +40  one 32-bit offset per frame, from the start of the file; frame i
         runs to the offset of frame i + 1, the last one to the end

### A frame

Three stages (`BfMovie_DecodeFrame` and its helpers in
`src/core/bfmovie.c`):

1. **Huffman** (`Huffman`).  A varint N (7 bits per byte, low group first,
   bit 7 = more), then 256 varint symbol weights, then the bit stream of
   N symbols.  The tree is built like CompressedBG's - repeatedly join the
   two lightest nodes, the first in index order on ties - but the parents
   are allocated from the top of the 511-node array downwards (so among
   equal weights a leaf beats a parent and a later parent beats an earlier
   one), and the bits are taken from the *low* end of each byte; bit 1
   goes to the lighter child, bit 0 to the heavier.  A node without a left
   child is a leaf (a parent that got only one child decodes as symbol 0).
2. **Runs** (`Unrle`).  The N bytes are a 32-bit length L followed by
   pairs of varints (fill, copy): `fill` bytes of 0x80, then `copy`
   literal bytes, until L bytes are out.
3. **Pixels**.
   * Frame 0 (`KeyFrame`): the picture as rows of bpp/8-byte pixels padded
     to 4 bytes, each byte the difference to the rounded-down average of
     its left and upper neighbours (the single available one on the first
     row / column, 0 at the origin) - the CompressedBG filter.  A 24-bit
     movie writes alpha 0 into its RGB32 slot.
   * Frame i > 0 (`DeltaFrame`): a delta against the previous frame, which
     the bitmap slot still holds.  First one bit per 8 x 8 block
     (row-major, low bit first) saying whether the block changes, then
     eight *row mask* bytes per changed block, then the pixel stream.
     In a changed block each pixel is, with its row-mask bit set, the
     previous pixel plus one signed delta byte per channel (3 or 4), and
     with the bit clear either the byte 0x80 - a transparent (zero)
     pixel - or two signed bytes (dx, dy) that copy the previous frame's
     pixel at that displacement.  The reference is a copy of the slot
     taken before the frame is written, so displacements may point at
     pixels the frame has already changed.

The frames of the "streamed" types 0x10000 / 0x10001 have a 0xC0-byte
header with the offsets at +0xC0 and go through another decoder, with
the frame data read from the archive on demand.  No game examined ships
such a file; the engine here reports them as code 0x8000000A (and once on
stderr) if one turns up.  Their frame codec is known, though: the DCT
codec of `src/core/cbg2.c` - a version 2 CompressedBG is decoded by the
original as a one-frame movie of type 0x10001 - with the per-band block
bitmap marking the blocks a frame changes.

## The registry and the instructions

Loaded movies are records on a list (`src/gfx/bmseq.c`) numbered from a
global counter; a record may have one *clone* sharing its data under a
second number, and the data goes with the last of the pair.

| | | |
|---|---|---|
| `90 F4` | `&no, &info, name, arc →` | the file is read by the loader thread (`WaitSeqLoad` in `src/wait/wait_load.c`); when it is in, the record is made (`BmSeq_Register`) and `info[5]` receives width, height, pixel mode, frame rate, frame count; the wait pushes 0 ok / 1 / 2 not a movie when it ends |
| `90 F5` | `no → r` | release: 0 ok, 3 no such number |
| `90 F6` | `bmp, no, frame → r` | decode the frame into the slot: 0 ok, 3 no such number, 4 no such frame, 5 the slot's size or pixel mode differs, 6; other codes raw |
| `90 F7` | `no, &out → r` | a clone of `no` under a new number: 0 ok, 3, 7 already cloned |
| `80 53` | `→` | the next `90 F6` runs inside a wait object (`WaitSeqDecode`; the original hands the frame to a decoder pool and polls the job, here the frame is decoded on the first poll) which pushes the result instead: 0 ok, 3, 5, 8 decode failed |
| `92 F1` | `&no, &info, name, arc, streamed →` | the `90 F4` load with a flag choosing the streamed form (only the header and index are read and the frames come from the archive); the whole file is loaded here either way |
| `92 F0` | `arc, name, x, y, w, h → length` | a DirectShow movie played out of an archive entry; without video playback the answer is 0 like `90 F0`'s |

The scripts drive the playback themselves: `moviemngrbm._bp` keeps a
table of 21 records (312 bytes each: the number, the five info words, the
target slot, the current frame, a per-frame delay list) and calls `80 53`
+ `90 F6` for every frame on its timer, then invalidates the sprite that
shows the slot.  The decode writes the slot's pixels in place and tells
nobody; a sprite showing the slot is redrawn only when the script asks.

## The video textures (`92 F2 / F4`, `91 F0 .. F7`)

A bitmap slot can also carry a *video texture*: a player object attached
to the slot's record in the bitmap manager that decodes a movie into the
slot's pixels on its own - the streamed BF_Movie form again, through the
attach by name (`92 F2`: name, arc, a, b, bmp → 0 ok, 1 the player could
not be made, 2 .. 4 its failures) or `91 F0`.  The bust-shot control uses
it for its kinds 3 .. 6 and falls back when the attach answers 1.  The
manager's calls all answer 0x80000004 (pushed as 4) for a slot without a
bitmap and 0x80000001 (1) for one without a texture:

| | | |
|---|---|---|
| `91 F1` | `bmp, &state → r` | the player's state |
| `91 F2`, `91 F6` | `bmp → r` | release |
| `91 F3` | `bmp, v → r` | control |
| `91 F4` | `bmp, &p, a, b, c → r` | control (0 / 1 / 2 / 4) |
| `91 F5` | `bmp → r` | advance (0 / 1 / 2 / 4) |
| `91 F7` | `bmp, &frame → r` | 1 with the slot's last frame number (0 without a texture), 0 for a bad slot |
| `92 F4` | `bmp, v → r` | control (1: no texture) |

Without the streamed decoder every attach fails with 1 here and the
other calls answer as for a slot without a texture, which is what the
scripts expect of a machine that cannot play video.  The handlers are in
`src/vm/ops_gfx1.c` and `src/vm/ops_gfx2.c`.

## The other 1.494+ bitmap instructions met on the way

* `91 16` `slot, periodX, phaseX, ampX, periodY, phaseY, ampY →`
  (`Vec_Sine` in `src/gfx/vecmaps.c`): a sine-wave vector field - the x
  displacement of row y is `16 sin(2π (y + phaseY) / periodY) ampY`, the
  y displacement of column x is `16 sin(2π (x + phaseX) / periodX) ampX`;
  a zero period counts as 1 with its amplitude dropped.  Errors: no
  bitmap, not a vector map.
* `91 1D` `dst, src, mode, colour, level →` (`BmpOp_ColourMode` in
  `src/gfx/bmpops.c`): `src` into `dst` through a colour mode - 0 copy, 1
  blend toward `src XOR colour` by `level / 256` (`Blit_FilterKind3`), 2
  the luminance tint of the filter objects (`Blit_FilterKind2`: `luma =
  (28 B + 151 G + 77 R) >> 9` indexes a table of `2 (((luma + 1) level
  colour_c) >> 16)` added to `src (256 - level) / 256`), 3 blend toward
  the colour by `level / 256` keeping the alpha (`Blit_TintKeepAlpha`), 4
  add `colour level / 256` saturating (`Blit_AddColour`).  RGB32 or
  ARGB32, the same on both sides; `level` 0 .. 0x100.  This is what the
  bust-shot control draws its characters through (mode 0 normally).
* `91 1B` `dst, src, cx, cy, level →` (`BmpOp_GatherAdd`, the blitter
  `Blit_GatherAdd` in `src/gfx/blit_scale.c`): the "gathering add" of the
  screen-group transition - every pixel of `src` is drawn toward the
  picture's centre by the 16.16 rates `cx`, `cy` (0 leaves it in place,
  1.0 collapses it onto the centre) and added to `dst` with saturation,
  weighted bilinearly and attenuated by `level` (0 .. 0x100).  RGB32 on
  both sides.  Used once, in `scrgrp2._bp`.
