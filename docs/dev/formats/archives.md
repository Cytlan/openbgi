# Archives and the compression layers

Where a game keeps its files, and the layers the loader removes on the
way. All numbers are little-endian.

## Archives (`*.arc`)

Two index formats with the same structure. Reader: `src/fs/arc.c` (the
engine), `src/fs/arcread.c` (the tools); writer: `src/fs/arcwrite.c`.

| Offset | Size | Content |
|---|---|---|
| 0 | 12 | Magic: `PackFile    ` (four spaces) or `BURIKO ARC20` |
| 12 | 4 | Entry count |
| 16 | count x record | The index |
| 16 + count x record | | The entries' data |

| | `PackFile` | `BURIKO ARC20` (1.573 on) |
|---|---|---|
| Record size | 32 | 0x80 |
| Name | 16 bytes at +0 | 0x60 bytes at +0 |
| Data offset | 4 bytes at +16 | 4 bytes at +0x60 |
| Size | 4 bytes at +20 | 4 bytes at +0x64 |
| Rest | Padding | Padding |

- The data offset counts from the end of the index.
- Names are NUL-padded Shift-JIS. They are lower-cased when the index
  is read, so lookups ignore case. At most 15 characters in `PackFile`,
  95 in `BURIKO ARC20`.
- An entry is stored as it is or under the DSC layer (below).

Usual archives:

| Archive | Content |
|---|---|
| `system.arc` | The boot program `ipl._bp` and early system files |
| `sysprg.arc` | The system programs |
| `sysgrp.arc` | System graphics |
| `autoload.arc` | Programs loaded at start |
| `data01xxx.arc` | Scenario files and their tables |
| `data02xxx.arc` and up | Pictures, frame sequences, sounds, voices; the numbering differs per game |

## How a file is found

Every load goes through `LoadFile` (`src/fs/loadfile.c`):

1. A loose file of that name in the game's folder.
2. The entry in the named archive in the game's folder.
3. The same in the alternative (disc) folder.
4. A prompt for the disc, then again from 1.

So a loose file wins over an archive entry of the same name. DSC and
CompressedBG are decoded on the way; callers see decoded data only.

For the data archives the engine takes the first archive, in directory
order, that has an entry of the name asked for.

## DSC: general compression

`DSC FORMAT 1.00`, in every build. Programs, scenario files, pictures
and sounds may all be stored this way. Decoder `src/core/dsc.c`,
encoder `src/core/dscenc.c`.

| Offset | Size | Content |
|---|---|---|
| 0x00 | 16 | `DSC FORMAT 1.00`, NUL |
| 0x10 | 4 | Key: seed of the generator for the code lengths |
| 0x14 | 4 | Size of the decompressed data |
| 0x18 | 4 | Number of Huffman symbols in the stream |
| 0x1C | 4 | Not read |
| 0x20 | 512 | Code lengths, one per symbol, obfuscated |
| 0x220 | | The bit stream, most significant bit first |

1. Each code length has one output of the generator (below) subtracted
   from it, the generator seeded with the key.
2. A canonical Huffman tree is built from the non-zero lengths, sorted
   by (length, symbol).
3. Symbols 0x000 .. 0x0FF are literal bytes. Symbols 0x100 .. 0x1FF are
   back references: the low byte + 2 is the length, 12 further bits
   give the distance, and the copy starts at `out - distance - 2`.

## The generator

One pseudo-random generator scrambles every obfuscated format. A
decoder seeds it with the key in the file and steps it once per byte,
subtracting the output. `src/base/hash.c`.

    lo  = 20021 * (key & 0xffff)
    hi  = 20021 * (key >> 16) + 346 * key
    hi  = (hi + (lo >> 16)) & 0xffff
    key = (hi << 16) + (lo & 0xffff) + 1
    out = hi & 0x7fff

## SDC and DCFS: save data

`src/core/sdc.c`. SDC is used for the global database `BGI.gdb`, for
some tables in the archives (`bsvcd`, `evdb`), and by the instructions
"80 C0" / "80 C1".

| Offset | Size | Content |
|---|---|---|
| 0x00 | 16 | `SDC FORMAT 1.00`, NUL |
| 0x10 | 4 | Key: a 16-bit seed |
| 0x14 | 4 | Bytes of LZ data after the header |
| 0x18 | 4 | Size of the original data |
| 0x1C | 2 | 16-bit sum of the scrambled payload bytes |
| 0x1E | 2 | 16-bit xor of the scrambled payload bytes |
| 0x20 | | Payload: each LZ byte plus one generator output |

LZ tokens, window 0x801 bytes, match length 2 .. 17:

| Token | Meaning |
|---|---|
| `0xxxxxxx` | A literal run of x + 1 bytes follows |
| `1LLLLDDD dddddddd` | Copy L + 2 bytes from (D:d) + 2 back |

`DCFS FORMAT 1.00` ("80 C4" / "80 C5") is a sequence of equally sized
frames. Every frame after the first is stored as alternating runs of
"same as the previous frame" and "literal bytes", run lengths as
base-128 varints. It is wrapped in an SDC layer.

## The global database (`BGI.gdb`)

SDC-encoded, in three layouts. Code: `src/sysobj/database.c`.

| Builds | Layout |
|---|---|
| Up to 1.494 | Fixed sections with 0x1000 name slots |
| 1.529 .. 1.599 | The same with 0x4000 name slots and the flag groups behind them |
| 1.616 on | "BURIKO GDB 3.00": sized sections, names and flag groups of any number and length |

1.529 and later also read the older fixed layout; 1.616 and later read
both. Each build writes its own.

## Other loose files

| File | Builds | Content |
|---|---|---|
| `BGI.gdb` | All | The global database |
| `BGI.hvl` | 1.66 on | Known by name only |
| `*.bss` | 1.653 on | Known by name only |
| `comap.dat` | Older games | The registration written by the game's `reg.exe` |
| A 16-byte file named like the product, holding `0123456789ABCDEF` | | The disc marker: a boot program quits when it finds it. OpenBGI hides it unless `--disc-marker` is given. |
