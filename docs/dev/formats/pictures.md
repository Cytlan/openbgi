# Pictures

A picture is stored raw or as CompressedBG, and either may be under
the DSC layer ([archives.md](archives.md)). After loading, the engine
always holds the raw form. All numbers are little-endian.

## Raw

| Offset | Size | Content |
|---|---|---|
| 0 | 2 | Width |
| 2 | 2 | Height |
| 4 | 4 | Bits per pixel: 8, 24 or 32 |
| 8 | 8 | Two extra words. The character pictures of later games put flags and `x \| y << 16` here. |
| 0x10 | width x height x bpp / 8 | Pixel rows, top first, no row padding. 24 and 32 bits are B, G, R(, A). |

## CompressedBG, version 1 (lossless)

From 1.64 on. Decoder `src/base/cbg.c`, encoder `src/base/cbgenc.c`
(it produces the original's bytes from the same pixels and key).

| Offset | Size | Content |
|---|---|---|
| 0x00 | 16 | `CompressedBG___`, NUL |
| 0x10 | 2 | Width |
| 0x12 | 2 | Height |
| 0x14 | 4 | Bits per pixel: 8, 24 or 32 |
| 0x18 | 8 | Two words copied into the raw header |
| 0x20 | 4 | Size of the Huffman-decoded data |
| 0x24 | 4 | Key: seed of the generator for the weight table |
| 0x28 | 4 | Length of the encrypted weight table |
| 0x2C | 1 | Check: byte sum of the decrypted table |
| 0x2D | 1 | Check: byte xor of the decrypted table |
| 0x2E | 2 | Version: 1 or 2 |
| 0x30 | | The encrypted weight table, then the Huffman bit stream |

Decoding:

1. Decrypt the weight table: subtract one generator output per byte
   ([archives.md](archives.md), "The generator"). Verify sum and xor.
2. Read 256 leaf weights as varints (7 bits per byte).
3. Build the Huffman tree: repeatedly join the two lightest nodes, the
   first in index order on ties. 511 nodes.
4. Huffman-decode the bit stream.
5. Expand the zero runs: alternating varint counts of literal bytes
   and of zeros.
6. Undo the filter: each byte is the difference to the rounded-down
   average of its left and upper neighbours, per channel.

The decoder trusts the header as the original does.

## CompressedBG, version 2 (DCT)

From 1.573 on, for backgrounds, event pictures and sprites; system
graphics stay version 1. `src/base/cbg2.c`. There is no encoder: an
edited version 2 picture is written back as version 1.

A JPEG-like codec:

- YCbCr with full-resolution chroma, 8 x 8 blocks.
- Two quantisation tables (luma, chroma), pre-scaled by the AAN
  factors; the float IDCT of libjpeg's `jidctflt`.
- DC differences and run / size AC pairs, coded with weight-built
  Huffman trees: 16 DC categories; 176 AC symbols (run in the low
  nibble, size in the high one, 0 = end of block, 0x0F = sixteen
  zeros). Bits are taken most significant first.
- The alpha plane of a 32-bit picture is a separate LZ-coded stream.

After the 0x30-byte header:

| Part | Content |
|---|---|
| 132 encrypted bytes | The two quantisation tables and four unused bytes, decrypted and checked like the version 1 weight table |
| Weights | 16 + 176 varints |
| Offsets | (rows / 8 + 1) 32-bit offsets from the stream's start: one per band of eight pixel rows, and one for the alpha stream |
| A band | One bit per block column (all set in a still picture); a varint coefficient count (columns x 3 x 64); the DC bit stream (one predictor across the band: the Y blocks, then Cb, then Cr); the AC bit stream from the next byte boundary |
| Alpha | A 32-bit mode: 1 = literal and copy tokens for the whole plane |

The original decodes such a picture as a one-frame BF_Movie of the
streamed type ([bfmovie.md](bfmovie.md)).

## In the engine

Loaded pictures go into numbered bitmap slots of the bitmap manager
(`src/gfx/`). Pixel modes: 1 = RGB32, 2 = ARGB32, 3 = 8-bit gray.
`openbgi dump` writes pictures as PNG and reads them back
([../../modding/dump-pack.md](../../modding/dump-pack.md)).
