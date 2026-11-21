# Frame sequences (BF_Movie)

Animated pictures decoded by the engine into a bitmap slot, from
1.69/472 on: `*.bm` entries of the picture archives. The games use them
for animated character pictures, sprite groups and small movies.
Decoder: `src/base/bfmovie.c`. Instructions: [../bfmovie.md](../bfmovie.md).

## Header

Little-endian.

| Offset | Size | Content |
|---|---|---|
| 0x00 | 16 | `BF_Movie_______`, NUL |
| 0x10 | 4 | Type: 0, or 0x10000 / 0x10001 for the streamed form |
| 0x14 | 4 | Width |
| 0x18 | 4 | Height |
| 0x1C | 4 | Bits per pixel: 24 or 32 |
| 0x20 | 4 | Pixel mode the target slot must have: 1 (RGB32) for 24 bits, 2 (ARGB32) for 32 |
| 0x24 | 4 | Frames per second |
| 0x28 | 4 | Frame count |
| 0x2C | 20 | Zero (type 0) |
| 0x40 | 4 x count | Offset of each frame from the start of the file; a frame runs to the next offset, the last to the end |

## A frame of type 0

Three stages:

1. **Huffman.** A varint N (7 bits per byte, low group first, bit 7 =
   more), 256 varint symbol weights, then a bit stream of N symbols.
   The tree is built by repeatedly joining the two lightest nodes (the
   first in index order on ties); parents are allocated from the top of
   the 511-node array downwards. Bits are taken from the low end of
   each byte; bit 1 goes to the lighter child.
2. **Runs.** The N bytes are a 32-bit length L, then pairs of varints
   (fill, copy): `fill` bytes of 0x80, then `copy` literal bytes, until
   L bytes are out.
3. **Pixels.**
   - Frame 0: rows of pixels padded to 4 bytes. Each byte is the
     difference to the rounded-down average of its left and upper
     neighbours (the one available on the first row or column, 0 at the
     origin). A 24-bit movie writes alpha 0.
   - Later frames: a delta against the previous frame. One bit per
     8 x 8 block (row-major, low bit first) says whether the block
     changes. Each changed block has eight row-mask bytes. In a changed
     block a pixel with its mask bit set is the previous pixel plus one
     signed delta byte per channel; with the bit clear it is either the
     byte 0x80 (a transparent pixel) or two signed bytes (dx, dy) that
     copy the previous frame's pixel at that displacement.

## The streamed form (types 0x10000, 0x10001)

Another codec in the same container; the original reads its frames
from the archive on demand.

| Offset | Size | Content |
|---|---|---|
| 0x2C | 4 | Every how many frames a whole one comes |
| 0x40 | 64 + 64 | Quantisation tables: luma, chroma |
| 0xC0 | 4 x count | Offset of each frame |

A frame is a frame of the DCT codec of CompressedBG version 2
([pictures.md](pictures.md); `Cbg2_Frame`, `src/base/cbg2.c`):

- 16 varint weights of the DC tree, 176 of the AC tree.
- A 32-bit offset for every band of eight pixel rows, and one for the
  alpha plane (0xFFFFFFFF in a 24-bit movie).
- A band: one bit per 8 x 8 block (set: the block is in this frame), a
  varint coefficient count, the DC bit stream, then the AC bit stream
  from the next byte on. Blocks not in the frame stay as they are.
- The alpha plane of a 32-bit movie. Type 0x10001 starts it with a
  32-bit word: 1 = the whole plane as literal and copy tokens; 2 = some
  blocks only (a count, a Huffman stream with one bit per block, then
  64 alpha bytes per marked block). Type 0x10000 has form 1 without the
  word.

## Tools and tests

- `openbgi dump` writes a movie as one PNG per frame and encodes it
  again from them (`src/base/bfmovieenc.c`; larger than the original,
  since it does not search for motion).
- `tests/bfmovie.c` checks the decoder with an encoder written as its
  inverse, and a streamed movie written out byte by byte.
  `BGI_BM_FILE=<file>` decodes a real one.
