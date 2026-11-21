# Sound files (BW)

Every sound of a game - music, effects, voices - is a BW file, loose or
in an archive. Code: `src/snd/bw/`. The library that plays them:
[../sound.md](../sound.md).

## Header

0x40 bytes, little-endian.

| Offset | Size | Content |
|---|---|---|
| 0x00 | 4 | Header size: 0x40 |
| 0x04 | 4 | `bw  ` (two spaces) |
| 0x08 | 4 | Coded bytes |
| 0x0C | 4 | Frames (samples per channel) |
| 0x10 | 4 | Sample rate |
| 0x14 | 4 | Channels: 1 or 2 |
| 0x18 | 4 | Loop flag |
| 0x1C | 4 | Loop start, in frames: the stream restarts there at its end |
| 0x20 | 16 | Two {predictor, step} pairs: the ADPCM states for a loop restart |
| 0x30 | 4 | Codec |
| 0x38 | 4 | Codec 2: bit position of the loop restart |
| 0x3C | 4 | Codec 2: step shift parameter |

The coded data follows the header.

## Codecs

| Codec | Format | File |
|---|---|---|
| 0 | 4-bit ADPCM | `adpcm.c` |
| 1 | 16-bit PCM | `pcm.c` |
| 2 | Huffman-coded 8-bit ADPCM | `huffman.c` |
| 3 | Ogg Vorbis: an ordinary Ogg stream after the header | `vorbis.c` |

Every game examined uses codec 3 only. Every decoder multiplies its
output by a gain and clamps to 16 bits.

### Codec 0

One nibble per sample, low nibble of a byte first. Stereo alternates
the channel state per sample.

    code  = sign (bit 3) + magnitude m (bits 0..2)
    delta = ((2m + 1) * step) >> 3
    sample = pred +/- delta
    step  = (T[m] * step) >> 6,  T = {57, 57, 57, 57, 77, 102, 128, 153}
    step is clamped to 0x7f .. 0x6000

The predictor keeps the unclamped sample. Initial state: {0, 0x7f}. A
loop restart reloads the states from the header.

### Codec 2

After the header: 8 unused bytes, a 0x400-byte Huffman tree, then the
bit stream, most significant bit first, read in 0x400-byte windows.

- The tree: a 32-bit root, then for each inner node `i >= 0x100` two
  int16 children at byte `4 + (i - 0x100) * 4`. Values below 0x100 are
  codes.
- A code is sign (bit 7) + magnitude m.

      unit  = 1 << ((param + 1) / 2)
      delta = ((2m + 1) * step) / (2 * unit)
      step  = step * (m < unit ? 57 : 77 + 25.6 * (m - unit)) / 64

- A restart seeks to byte `0x448 + loopBit / 8`.

### Codec 3

The Ogg Vorbis stream starts at offset 0x40. Rate, channels and length
in the header repeat what the stream says. Loop flag and loop start
are the only information an Ogg file cannot carry; `openbgi dump`
writes them to a `.json` beside the `.ogg`.

## Looping

Background music carries `loop = 1` and its loop start (an intro, then
the repeated part). Voices and full-length songs carry 0. The games'
sound scripts read the flag from the file themselves (bytes 0x18 to
0x1B) to refuse playing a looping file as an effect.
