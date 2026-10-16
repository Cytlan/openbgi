# Dumping a game and packing it again: bgidump and bgipack

A game keeps everything in its archives (`*.arc`), and almost everything
in them is in a format of the engine's own: compiled programs, compiled
scenario files, CompressedBG pictures, BW sounds, BF_Movie sequences.
`bgidump` turns all the archives of a game into a directory of files that
ordinary tools open - source text, PNG, Ogg Vorbis, JSON - and `bgipack`
builds the archives again from that directory.  Together they are the
working copy of a translation: the story and the texts of the system
programs are edited as text, the buttons and other pictures with text
burned into them in an image editor, and the result goes back into
archives the game (the original engine or this one) reads.

`bin/bgidump` and `bin/bgipack` are built by `make tools`:

    bgidump [options] GAMEDIR OUTDIR
    bgipack [options] DUMPDIR OUTDIR

    bgidump "/games/Tayutama" tayutama.dump        # every *.arc of the game
    (edit files under tayutama.dump/)
    bgipack --changed tayutama.dump tayutama.new   # the archives that changed
    (copy tayutama.new/*.arc over a copy of the game)

Neither tool writes into the game directory: `bgidump` only reads it, and
`bgipack` refuses an output directory that holds the originals.

## bgidump

Every `*.arc` of `GAMEDIR` gets a directory of its name (without `.arc`)
under `OUTDIR`, and every entry becomes one or more files there:

| The entry is | It becomes | Beside it |
|---|---|---|
| a program (`title._bp`) | `title.bpc`, the decompiled source (`docs/bpc.md`) | |
| a scenario file (`_01`) | `_01.bsc`, the decompiled source (`docs/scenario.md`) | |
| an image, stored raw or as CompressedBG of either version (`sys_title_101`) | `sys_title_101.png` | `sys_title_101.json` when the image has extra header words |
| a sound (`bgm01`) | `bgm01.ogg`, the Ogg Vorbis stream inside the BW file; `bgm01.wav` for the other codecs | `bgm01.json`: the loop |
| a frame sequence (`ci_01_06.bm`) | `ci_01_06.bm/0000.png`, `0001.png`, ... | `ci_01_06.bm.json`: size, depth, frame rate |
| a table (`StringsDB`, `ScenarioDB`, `BitmapNameDB`) | `StringsDB.json` | |
| an SDC-coded block (`bsvcd`, `evdb`) | `bsvcd.bin`, the decoded bytes | `evdb.json` when bytes precede the SDC header |
| anything else (`movie.mpg`, a font, a text list) | the file itself, without the DSC layer if it had one | |

`OUTDIR/bgidump.json` describes the dump (below); it is written last, so
an interrupted dump has none and `bgipack` will not take it for a whole
one.  A directory that already holds a dump is not written into unless
`--force` is given - the files in it may have been edited.

Options:

| | |
|---|---|
| `-e`, `--engine NAME` | the engine profile, instead of the detected one; `any` for none |
| `-g`, `--games FILE` | the `games.json` to identify the game with |
| `--only NAME` | dump this archive only; may be repeated (the dump then describes just those) |
| `-j N` | worker processes (default: one per processor, at most 16; always 1 on Windows) |
| `--png-level N` | PNG compression, 1 (fast) to 9 (small); default 4 |
| `-f`, `--force` | write into a directory that already holds a dump |
| `-v` / `-q` | a line per entry / problems and the summary only |

The exit status is 0 when everything converted, 1 when the dump is
complete but a message names an entry (below), 2 when there is no dump.

### The engine build

The programs of the builds differ in their opcodes (`docs/versions.md`),
so the decompiler has to know the build.  `bgidump` finds it the way the
engine does: the boot program - a loose `ipl._bp` in the game directory,
else the one in `system.arc` - is matched against `games.json` (the one
given with `--games`, else the game directory's own, else the one beside
the tool's binary, else the built-in table), and the entry that matches
names the profile.  The first line of output says what was found:

    bgidump: Tayutama, engine profile 1.69/444

A game that is not in the table is dumped with the opcodes of every build
(`any`), which the decompiler handles but which names fewer instructions;
`--engine NAME` settles it (`bgi --list-engines` prints the names).  The
profile is recorded in every `.bpc` file (`#engine`) and in the
description.

The scenario files need a second thing from the game: the table of its
scenario commands, which `bgidump` reads from `sysprg.arc` in the game
directory (`docs/scenario.md`, "The interpreter") and writes into the
description, so that `bgipack` can compile without the game at hand.

### What each kind holds

**Programs and scenario files.**  The sources are UTF-8; the strings
become Shift-JIS (CP932) again when compiled, and a character CP932 lacks
turns into `?` with a warning.  Both documents say what the languages
look like and where the texts are: a message of the story is a line like
`msg("...", "name", 0, 1, 1);` in a `.bsc` file, the texts of the menus,
dialogs and window titles are string literals in the `.bpc` files of
`sysprg.arc` and `system.arc`.  A `.bpc` - and a `.bsc` of the 1.64
game - may `#include` another file of the dump by a path relative to
itself: the declarations of a game's global variables, or its texts as
`#define`d constants, so that a translation is one file
(`docs/bpc.md`; `#include "../strings.inc"` for a file beside the
archive directories, `#include "strings.inc"` for one among the
sources).  Such a source is compiled on every pack, so a change of the
included file alone is picked up.  The dump compiles every source once and
compares the result with the entry.  When they differ the entry's record
says `"roundtrip": false` and a message names it: the entry still packs
to its original bytes while its source is untouched, but an edited one
would differ from the original in more than the edit.  (Of the games
examined only `main` of the retail Tayutama does this.)

**Images.**  A PNG of 8-bit grey, RGB or RGBA for an image of 8, 24 or 32
bits, the alpha channel as stored (not premultiplied).  The size may be
changed; the depth is kept - a PNG with transparency packed into a 24-bit
image loses it, colours packed into an 8-bit image become grey, and
`bgipack` says so.  Many sprites have colour in their fully transparent
pixels, which image editors tend to discard on export; where a sprite is
drawn scaled this can show as a dark fringe, so keep the colour values
when the editor offers the choice ("save colour values from transparent
pixels").  The character pictures of the later games carry two extra
words in their header, `"extra": [flags, x | y << 16]` in the `.json`
beside the PNG - they look like a reference point of the picture; they
are put back as they are.

A CompressedBG file of version 2 (the lossy codec of 1.573 on:
backgrounds, event pictures and sprites of those games) is decoded to
the PNG like any other; there is no encoder for that codec here, so an
*edited* one is written as version 1, the lossless codec, which every
build reads and whose files are larger.  Untouched ones are copied as
they are.

**Sounds.**  Every sound of the games examined is codec 3: an ordinary
Ogg Vorbis stream behind a 0x40-byte header (`docs/sound.md`).  The
`.ogg` is that stream, byte for byte, so nothing is decoded or lost.  The
header's rate, channel count and length are read back from the stream
when packing; the two things an Ogg file cannot say are in the `.json`:

    {
      "loop": true,
      "loop_start": 441000
    }

`loop_start` is in frames (samples per channel).  A replacement has to be
Ogg Vorbis with one or two channels.  A sound of another codec (ADPCM,
PCM, Huffman-coded ADPCM; none in these games) is decoded to a 16-bit
WAV, and an edited WAV is packed as codec 1, plain PCM.

**Frame sequences.**  The frames of a BF_Movie (`docs/bfmovie.md`) as
the engine shows them, each decoded on top of the one before: RGBA for a
32-bit movie, RGB for a 24-bit one.  The `.json` holds what no PNG does:

    {
      "width": 800,
      "height": 600,
      "bpp": 32,
      "fps": 30
    }

When a frame was edited the whole movie is encoded again from its frames
in the order the description lists them.  The encoder stores exactly the
pictures given but does not search for motion as the original's does, so
its files are larger.

**Tables.**  `StringsDB` (`BitmapNameDB` in the oldest games) and
`ScenarioDB` of the scenario archive are a count and fixed-size records
of strings and numbers; the record size decides the layout:

    {"table": "ScenarioDB", "record_size": 64, "records": [
        {"name": "プロローグ", "values": [5, 16, 2, 0]}, ...

32 bytes: `name` (28 bytes) and `value`; 64 bytes: `name` (48 bytes) and
four `values`; 128 bytes: `name` (96 bytes), `label` (28 bytes) and
`value`.  The names of `ScenarioDB` include the chapter titles some games
show.  A string that does not fit its field in Shift-JIS is an error when
packing.  A table is written as JSON only if that JSON makes the same
bytes again; otherwise it stays a plain file.

**SDC blocks.**  The tables of the bust-shot system in the later games
(`bsvcd`, `bsvsrcd`, `evdb`) are stored in the save-data coding; the
`.bin` is the decoded content, and bytes in front of the SDC header go
into the `.json` as `"prefix"` (hex).  An edited `.bin` is coded again.

### Names

A file is named after its entry, with the characters a file system may
refuse replaced by `_`; the entry names are Shift-JIS and the file names
UTF-8.  Where two entries of an archive would want the same file (`pic`
and `pic.png`), the later one gets `~2` appended.  The description
records every entry's own name, so the file names never have to be
converted back.

### Messages

`bgidump` names an entry when it had to treat it as less than it is:

* *the decompiled program / scenario does not compile back to the
  original bytes* - see above; the source is written all the same;
* *does not decode: kept as it is* - a CompressedBG, sound, movie or SDC
  block the decoders refuse is dumped as a plain file;
* *not an archive: left out* - a `*.arc` without an archive header (some
  games ship an empty or unrelated file under such a name) is listed
  under `"skipped"` and otherwise ignored.

## bgipack

`bgipack` reads `DUMPDIR/bgidump.json` and writes every archive it lists
into `OUTDIR`, each entry in its original place.  For every entry it
compares the files with what the dump recorded - the size, then the
modification time, and the CRC-32 when the time differs:

* **unchanged** - the entry is copied from the original archive, byte for
  byte.  A dump that was not touched therefore packs to archives
  identical to the originals;
* **changed** - the entry is made from its files (compiled, encoded),
  DSC-compressed if the original was, and stored in its place.

The original archives are read from the directory the dump was made from
(recorded in the description) or from `--source DIR`.

| | |
|---|---|
| `-s`, `--source DIR` | the directory of the original archives |
| `--only NAME` | build this archive only; may be repeated |
| `-c`, `--changed` | write only the archives in which something changed |
| `-a`, `--rebuild-all` | make every entry from its files; the originals are not read |
| `--checksum` | compare by CRC even when size and time are those of the dump |
| `-v` / `-q` | a line per entry built / problems and the summary only |

With `--changed` the output is just what has to be copied into the game;
without it every archive is written, the unchanged ones as copies.

A file that does not build - a source with an error, a damaged PNG - does
not stop the archive: the compiler's messages are shown (with the file
and line), the *original* entry is put in its place, a line in capitals
says so, and the exit status is 1.  The exit status is 2 when an archive
could not be written at all: a file is missing and there is no original
to take the entry from, or the output directory is the source directory.

### Without the originals

`--rebuild-all`, or a source directory that does not hold the archive
that was dumped, makes every entry from its files.  The result is
equivalent rather than identical: sounds, tables and version 1
CompressedBG pictures come back byte for byte, scripts as their
compilers write them, DSC streams from another compressor (usually a
little smaller than the originals), version 2 CompressedBG pictures as
version 1 (larger), movies from the encoder here (larger).  It is the
slow way, and it loses the exactness of the copy for the scripts marked
`"roundtrip": false`; it exists so that a dump is complete in itself.

### Adding and removing entries

The description is plain JSON and `bgipack` follows it: an entry removed
from an archive's `"entries"` is left out of the archive, and an entry
added - `{"name": "new_pic", "kind": "image", "codec": "raw", "bpp": 32,
"dsc": true, "file": {"path": "new_pic.png"}}` - is built from its file.
Whether the game then asks for the new entry is a matter of its scripts.

## The description (bgidump.json)

    {
      "format": "openbgi-dump",
      "version": 1,
      "source": "/games/Tayutama",
      "game": {"match": "...", "product": "Tayutama", "profile": "1.69/444", "generation": "1.69/444"},
      "scenario": {"scenes16": false, "dispatch": 57140, "commands": [
        {"token": 320, "module": "scrmsg", "name": "msg", "args": 5, "result": 0, "handler": 6833, "params": ""}, ...]},
      "archives": [
        {"name": "sysgrp.arc", "dir": "sysgrp", "type": "PackFile", "size": 8258466, "entries": [
          {"name": "sys_title_101", "kind": "image", "offset": 5361724, "size": 1876, "crc": "0a1b2c3d", "dsc": true,
           "file": {"path": "sys_title_101.png", "size": 1304, "mtime": 1791633431854, "crc": "ffc42fbf"},
           "codec": "raw", "bpp": 32}, ...]}, ...],
      "skipped": ["data99000.arc"]
    }

| member | meaning |
|---|---|
| `source` | the game directory the dump was made from (UTF-8) |
| `game` | the `games.json` entry that matched and the profile used; `generation` is the engine build's name (`any` when unknown) |
| `scenario` | the game's scenario commands as `Scn_ScanCommands` found them; absent when the game has no `sysprg.arc` |
| archive `type` | `PackFile` (16-byte names) or `BURIKO ARC20` (96-byte names, 1.573 on) |
| archive `size` | the original file's size; `bgipack` takes another size for another archive |
| entry `name` | the entry's name in UTF-8; `name_sjis` (hex) is added when the name does not survive the conversion |
| entry `kind` | `program`, `scenario`, `image`, `sound`, `movie`, `table`, `sdc` or `data` |
| entry `offset`, `size`, `crc` | where the stored bytes lie in the original archive, and their CRC-32 |
| entry `dsc` | the entry is stored DSC-compressed |
| `file`, `info` | the entry's file and the JSON beside it: `path` relative to the archive's directory, `size`, `mtime` (milliseconds since 1970) and `crc` as dumped |
| `frames` | a movie's frame files, in order |
| `codec` | an image: `raw`, `cbg1`, `cbg2`; a sound: `vorbis`, `adpcm`, `pcm`, `huffman` |
| `bpp`, `key` | an image's depth; the key a version 1 CompressedBG's table is encrypted with (so that the same pixels give the same file) |
| `roundtrip`, `blocks` | a script: `false` when its source did not compile back to the entry; the number of functions the decompiler wrote as `__asm` blocks |

## How it is built

`src/dump/` holds the two drivers and one file per kind of entry, each
with both directions (`inc/bgi/dump.h` is the interface, `Dump_Run` and
`Pack_Run`; the tools in `tools/` only parse their command lines):

| | |
|---|---|
| `dump.c` | the run of `bgidump`: listing, game detection, the file names, the worker processes, the description |
| `pack.c` | the run of `bgipack`: what changed, copying, building, the archive's index |
| `script.c` | programs and scenario files through `src/bpc/` and `src/scn/` |
| `image.c`, `sound.c`, `movie.c`, `table.c` | the other kinds |
| `util.c` | the JSON text buffer, native paths, file names |

The decoders are the engine's own.  What the engine never needed and the
tools add, all in `src/core/`: `deflate.c` (zlib streams and CRC-32) and
`png.c` (a PNG reader for every colour type, bit depth and interlacing,
and a writer), so that no library is required; `dscenc.c`, a DSC
compressor; `cbgenc.c`, the version 1 CompressedBG encoder (it produces
the original's bytes from the same pixels and key); `bfmovieenc.c`, a
BF_Movie encoder.  The OS layer gained what a tool working on
directories needs (`OS_NativeStat`, `OS_NativeList`, `OS_NativeMkdir`,
`OS_RunWorkers` and the rest, `docs/os_layer.md`).

An archive's entries are shared out among worker processes (`fork`, so
not on Windows); the file names are settled before they start, from the
entry names alone.  Dumping is bounded by decoding the pictures and
compressing the PNG files, some 20 MB of pixels per second and
processor; packing an edited dump is a matter of seconds, since
everything unchanged is copied.

`tests/dump.c` checks the codecs against reference data and each other,
and the two tools on a small game made for the purpose: every kind of
entry, both index formats, the untouched round trip, edits of every
kind and the failure paths.  Against the real games: every archive up to
45 MB and every `sysgrp.arc` of twenty games (289 archives, 41,703
entries) was dumped and packed back to the identical file; the same
dumps packed with `--rebuild-all` give archives whose every entry
decodes to the original's content (but for `main` of the retail
Tayutama, as noted); and an edited title button and an edited message
of the Tayutama trial show in the game run from the packed archives.
