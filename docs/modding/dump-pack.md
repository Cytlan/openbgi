# Dumping a game and packing it again

`openbgi dump` turns every archive (`*.arc`) of a game into files that
ordinary tools open. `openbgi pack` builds the archives again.

    openbgi dump [options] GAMEDIR OUTDIR
    openbgi pack [options] DUMPDIR OUTDIR

    openbgi dump "/games/Tayutama" tayutama.dump
    (edit files under tayutama.dump/)
    openbgi pack --changed tayutama.dump tayutama.new

Neither command writes into the game's folder. The result of `pack` is
read by OpenBGI and by the original engine alike. To hand it out, make a
patch of it: [patches.md](patches.md).

## What a dump holds

Each archive becomes a folder of its name under OUTDIR.

| Entry | File | Beside it |
|---|---|---|
| Program (`title._bp`) | `title.bpc`, source ([programs.md](programs.md)) | |
| Scenario file (`_01`) | `_01.bsc`, source ([scenario.md](scenario.md)) | |
| Picture (raw or CompressedBG) | `name.png` | `name.json` if the picture has extra header words |
| Sound | `name.ogg` (the Vorbis stream as stored), or `name.wav` for other codecs | `name.json`: the loop |
| Frame sequence (BF_Movie) | `name/0000.png`, `0001.png`, ... | `name.json`: size, depth, frame rate |
| Table (`StringsDB`, `ScenarioDB`, `BitmapNameDB`) | `name.json` | |
| SDC-coded block (`bsvcd`, `evdb`) | `name.bin`, decoded | `name.json` if bytes precede the SDC header |
| Anything else | the file itself, DSC layer removed | |

`OUTDIR/bgidump.json` describes the dump. It is written last: an
interrupted dump has none and cannot be packed.

File names are the entry names in UTF-8, with characters a file system
refuses replaced by `_`. A second entry that would get the same file
name has `~2` appended.

## openbgi dump

| Option | Meaning |
|---|---|
| `-e`, `--engine NAME` | Engine profile instead of the detected one; `any` for none |
| `-g`, `--games FILE` | The `openbgi.json` to identify the game with |
| `--only NAME` | Dump this archive only; may be repeated |
| `-j N` | Worker processes (default: one per processor, at most 16; always 1 on Windows) |
| `--png-level N` | PNG compression, 1 (fast) to 9 (small); default 4 |
| `-f`, `--force` | Write into a folder that already holds a dump |
| `-v` / `-q` | A line per entry / problems and the summary only |

Exit status: 0 everything converted; 1 the dump is complete but a
message names an entry; 2 no dump.

The decompiler needs the game's engine version. `dump` detects it like
the engine does and prints it:

    bgidump: product name "HanairoHeptagram": engine profile 1.553

If detection fails, pass `--engine NAME` (`openbgi --list-engines`).

Messages:

| Message | Meaning |
|---|---|
| does not compile back to the original bytes | The source is written anyway. Untouched, the entry packs to its original bytes; edited, it will differ in more than the edit. |
| does not decode: kept as it is | The entry is dumped as a plain file. |
| not an archive: left out | A `*.arc` without an archive header; listed under `"skipped"`. |

## Editing

**Programs and scenario files.** UTF-8 text. Strings become Shift-JIS
(CP932) when compiled; a character CP932 lacks becomes `?` with a
warning, unless the game is switched to UTF-8 ([obgi.md](obgi.md)). A
source may `#include` another file of the dump by a relative path; such
a source is compiled on every pack.

**Pictures.** PNG in 8-bit grey, RGB or RGBA for a picture of 8, 24 or
32 bits. Alpha is not premultiplied.

- The size may change. The depth is kept: transparency packed into a
  24-bit picture is lost, colour packed into an 8-bit picture becomes
  grey; `pack` says so.
- Keep the colour of fully transparent pixels when your editor offers
  the choice. Scaled sprites otherwise get a dark fringe.
- `"extra": [flags, x | y << 16]` in the `.json` is put back as it is.
- An edited CompressedBG version 2 picture (the lossy codec of 1.573 and
  later) is written as version 1 (lossless, larger). Every engine
  version reads it.

**Sounds.** The `.ogg` is the stored stream, byte for byte. A
replacement must be Ogg Vorbis with one or two channels. The `.json`
holds the loop:

    { "loop": true, "loop_start": 441000 }

`loop_start` is in frames (samples per channel). A sound of another
codec is dumped as 16-bit WAV; an edited WAV is packed as PCM.

**Frame sequences.** One PNG per frame, as shown. If any frame changes,
the whole movie is encoded again; the result is larger than the
original.

**Tables.** A count and fixed-size records:

    {"table": "ScenarioDB", "record_size": 64, "records": [
        {"name": "...", "values": [5, 16, 2, 0]}, ...

| Record size | Fields |
|---|---|
| 32 | `name` (28 bytes), `value` |
| 64 | `name` (48 bytes), four `values` |
| 128 | `name` (96 bytes), `label` (28 bytes), `value` |

A string that does not fit its field in Shift-JIS is an error when
packing. `ScenarioDB` holds the chapter titles some games show.

**SDC blocks.** The `.bin` is the decoded content; an edited one is
coded again.

## openbgi pack

`pack` compares every file with what the dump recorded (size, then
time, then CRC-32):

- unchanged: the entry is copied from the original archive, byte for
  byte. An untouched dump packs to archives identical to the originals.
- changed: the entry is built from its files and stored in its place.

| Option | Meaning |
|---|---|
| `-s`, `--source DIR` | Folder of the original archives (default: where the dump was made from) |
| `--only NAME` | Build this archive only; may be repeated |
| `-c`, `--changed` | Write only the archives in which something changed |
| `-a`, `--rebuild-all` | Build every entry from its files; the originals are not read |
| `--checksum` | Compare by CRC even when size and time match |
| `-v` / `-q` | A line per entry built / problems and the summary only |

A file that does not build does not stop the archive: the errors are
shown with file and line, the original entry is used, a line in
capitals says so, and the exit status is 1. Exit status 2: an archive
could not be written.

`--rebuild-all` gives archives that are equivalent, not identical:
scripts as the compilers write them, DSC streams from another
compressor, version 2 pictures as version 1, movies re-encoded.

## Adding and removing entries

Edit `bgidump.json`. An entry removed from an archive's `"entries"` is
left out. An entry added is built from its file:

    {"name": "new_pic", "kind": "image", "codec": "raw", "bpp": 32,
     "dsc": true, "file": {"path": "new_pic.png"}}

## bgidump.json

| Member | Meaning |
|---|---|
| `source` | The game folder the dump was made from |
| `game` | Product name, hash of the programs, how the profile was found, the profile used |
| `scenario` | The game's scenario commands, so that `pack` compiles without the game |
| archive `type` | `PackFile` (16-byte names) or `BURIKO ARC20` (96-byte names, 1.573 and later) |
| archive `size` | The original file's size |
| entry `name` | The entry's name in UTF-8; `name_sjis` (hex) when the name does not survive conversion |
| entry `kind` | `program`, `scenario`, `image`, `sound`, `movie`, `table`, `sdc`, `data` |
| entry `offset`, `size`, `crc` | Where the stored bytes lie in the original archive, and their CRC-32 |
| entry `dsc` | Stored DSC-compressed |
| `file`, `info` | The entry's file and the JSON beside it: `path`, `size`, `mtime` (ms since 1970), `crc` |
| `frames` | A movie's frame files, in order |
| `codec` | Picture: `raw`, `cbg1`, `cbg2`. Sound: `vorbis`, `adpcm`, `pcm`, `huffman` |
| `bpp`, `key` | A picture's depth; the key of a version 1 CompressedBG |
| `roundtrip`, `blocks` | A script: `false` when its source did not compile back; the number of functions written as `__asm` blocks |
