# Engine versions and profiles

The games run the same engine in different builds, from ver 1.58 of 2005
to 1.669 of 2023.  One binary runs all of them: the differences between
the builds are kept in an engine profile (`inc/bgi/version.h`, `gEngine`)
selected once at start-up - from the game's `ipl._bp` through
`games.json`, from the command line (`--engine=NAME`, `--list-engines`),
the environment (`BGI_ENGINE=NAME`) or the launcher's dialog.  The
default is the build the reimplementation was verified against,
Tayutama ver 1.69 build 444.

Most differences between the builds are additional opcodes; which
opcodes a build defines is tabulated in `src/vm/opset.c` (generated from
a survey of 22 executables), and the VM's handler tables are filled at
start-up with the handlers of the selected generation (`Vm_FillTable`,
`src/vm/ops_base.c`).  A script using an opcode its engine did not have
gets the original's "undefined instruction" error; an opcode the engine
had but the reimplementation lacks yet gets a "not implemented" error
naming it.  A few differences are parameters of the virtual machine
itself that every pointer-handling service depends on - the layout of
the tagged script pointers and the size of the thread-local heap - and a
few more are behaviour switches inside the services ("1.529 on", "1.69
build 472 on" in the source comments), all keyed on the generation
(`EngineGen_t`, `GEN_1_58` .. `GEN_1_669`).

## The builds

| executable | product name ("80 E8") | linked | engine version (build) | compat | compiler |
|---|---|---|---|---|---|
| NurseryRhymeTrial.exe | NurseryRhyme (trial) | 2005-08-02 | 1.58/389.2 | - | VC6 |
| NurseryRhyme.exe | NurseryRhyme | 2005-11-09 | 1.64/398.3 | - | VC6 |
| Itsusora.exe | ItsukaTodokuAnoSorani | 2007-01-11 | 1.66/428.6 | - | VC6 |
| tayutama.exe | Tayutama (trial; the reference of this reimplementation) | 2008-04-12 | 1.69/444.2 | - | VC6 |
| TayutamaFull.exe | Tayutama (the retail game, `Tayutama.exe` of the disc) | 2008-06-16 | 1.69/451.3 | - | VC6 |
| TayutamaHD.exe | TayutamaHD | 2009-05-18 | 1.69/472.2 | - | VC6 |
| PrismRhythm.exe | PrismRhythm | 2010-04-27 | 1.494 | 1.69 | VC6 |
| DiamicDays.exe | DiamicDays | 2011-07-29 | 1.529.2 | 1.69 | VC6 |
| Gackoh.exe | Gackoh | 2012-01-10 | 1.535.1 | 1.69 | VC6 |
| Gackoh_HD.exe | Gackoh_HD | 2012-07-11 | 1.547 | 1.69 | VC6 |
| Heptagram.exe | HanairoHeptagram | 2012-10-12 | 1.553.3 | 1.69 | VC6 |
| magicha.exe | MagicalCharming! | 2013-05-14 | 1.573.2 | 1.71 | VC6 |
| sekachu.exe | SekaiToSekaiNoMannakaDe | 2014-01-20 | 1.588.3 | 1.72 | VS2010 |
| unmei.exe | UnmeisenjouNoPhi | 2014-10-13 | 1.599 | 1.72 | VS2010 (LTCG) |
| kodomo.exe | KodomoNoAsobi | 2015-11-09 | 1.616.3 | 1.72 | VS2010 (LTCG) |
| Tayutama2AS.exe | Tayutama2AS | 2017-04-05 | 1.640.10 | 1.72 | VS2010 (LTCG) |
| WakabaironoQuartet.exe | WakabaironoQuartet | 2019-05-17 | 1.653.3 | 1.72 | VS2017 |
| Nekotsuku.exe | NekoTsukuSakura | 2019-11-21 | 1.654.3 | 1.72 | VS2017 |
| madokino.exe | MadoiShirokinoKamikakushi | 2020-11-04 | 1.659.2 | 1.72 | VS2017 |
| yumahorome.exe | Yumahorome | 2021-09-26 | 1.662.1 | 1.72 | VS2017 |
| arcana.exe | ArcanaAlchemia | 2022-08-19 | 1.667 | 1.72 | VS2017 |
| haruyome.exe | HarukaAonoHanayomeni | 2023-04-26 | 1.669.1 | 1.72 | VS2017 |

"compat" is the `Compatibility : x.xx` of the version resource (the
script-level compatibility the engine declares); the ver 1.58 .. 1.69
builds carry `ver x.xx ( build : n )` in their text instead.  Tayutama
(trial and retail) and TayutamaHD are the same engine version in three
builds: the retail game's build 451 is build 444 plus a dozen opcodes
that build 472 also has (see below), on the build 444 pointer layout.

The profile names of `--engine` are the version numbers of the table
without the minor build number (`1.69/444`, `1.69/451`, `1.69/472`, `1.529`, ...);
`--list-engines` prints them.

## games.json

The profiles and the games are built into the binary (`src/core/
version.c`) and written out as `games.json` the first time the engine
runs without one, next to the binary; from then on that file is what
the engine reads, so a profile can be adjusted or added and a game
recorded without rebuilding.  The file is looked for in the game's
directory first (a game may carry its own), then next to the binary;
`--games=PATH` or `BGI_GAMES=PATH` name another one.  Its own header
describes the format: `"profiles"` is a list of `{"name", "generation",
"compat", "tagShift", "tagCode", "tagData", "tagHeap", "dynFirst",
"dynCount", "heapLimit", "bootCode", "bootData"}` - the generation (one
of the built-in profiles' names) selects the opcode set and the
behaviour switches, the numbers are the virtual machine's parameters,
and a profile that leaves a number out takes its generation's built-in
value, so `{"name": "test", "generation": "1.553", "heapLimit":
8388608}` is a complete profile; `"games"` is the list of `{"match",
"profile", "product"}` entries described in the header, to which the
launcher adds `"directory"`, `"name"` and `"options"` for the games it
started.  When the file cannot be written (the binary runs from a
read-only place) the built-in lists serve in memory, silently.  A file
that lacks some of the built-in profiles (by name) or games (by match
string) - one an older build wrote - gets them appended in memory when
it is read, so a newer binary still detects every game it knows; the
file itself is rewritten only when the launcher records a game.

## File formats by version

Magic strings found in the executables: `PackFile` archives, `DSC FORMAT
1.00` compression and `bw` audio in every build; `CompressedBG___`
pictures from 1.64 on (1.58 has no CBG decoder) - version 1, the
lossless one of `src/core/cbg.c`, in every build with a decoder, and from
1.573 on version 2 as well (the word at +0x2E), the DCT codec of
`src/core/cbg2.c` that the backgrounds, event pictures and sprites of
Magical Charming!, Sekachu and the later games are stored in (the system
graphics stay version 1 / DSC); `BURIKO ARC20` archives
from 1.573 on next to `PackFile` (a 16-byte header - the 12-byte magic
and the entry count - then 0x80-byte entries: the name, lower-cased on
loading, at +0, the data offset at +0x60 and the size at +0x64, with the
data starting at 16 + count * 0x80); the
Visual Studio 2017 builds compare the archive magics as dword immediates
(`Pack` / `File`), so the strings no longer appear as text.  Loose file
names: `BGI.gdb` (the global database) everywhere, `BGI.hvl` from 1.66,
`.bss` from 1.653.  Ogg Vorbis is linked in every build.

## Tagged pointers

Script pointers are 32-bit values with an area tag in the top bits; the
layout changed twice, which every pointer-handling service and the
dynamic-block allocator ("80 20") depend on:

| builds | offset bits | global | code | data (locals) | local heap | dynamic blocks | heap limit |
|---|---|---|---|---|---|---|---|
| 1.58 .. 1.66 | 24 | tag 0x00 | 0x11 | 0x10 | 0x12 | 0x40.. (tag - 0x40) | 16 MB |
| 1.69 builds 444, 451 (Tayutama) | 25 | 0x00 | 0x08 | 0x09 | 0x0A | 0x10..0x3F (tag - 0x10) | 32 MB |
| 1.69 build 472 .. 1.669 | 26 | 0 | 1 | 2 | 3 | 16..63 (tag - 16); 4..15 are errors | 64 MB |

Compiled scripts embed such pointers - `syswnd._bp`
of Tayutama does `push32 0x20220400`, dynamic block 0 of the 1.69 build
444 layout - so the layout is part of the script ABI and a game needs the
profile of its own build.  The reimplementation keeps it in
`inc/bgi/version.h` (`gEngine`, `Engine_SelectProfile`).

## Structural changes between versions

* 1.69 build 451 (the retail Tayutama): the main table gains the `81`
  dispatcher ("未定義のシステム制御命令 $81%02X") with `81 10` and `81 1F`;
  `80 39` `80 6E` `90 F4..F6` `91 F0..F2` `B0 03` `B0 C0` appear and `80 80`
  takes its build 472 form (the database in the "80 39" directory, the
  window position in the database).  `90 F4` reads the file on the
  calling thread and `B0 03` pushes no result in this build.
* 1.69 build 472 (TayutamaHD): `43` (atan2 in 16.16 degrees), `50..54`
  (64-bit add / sub / mul / div / mod on pointer operands), `7E`; the
  pointer layout changes (above).
* 1.529 (DiamicDays): the second system family `81 xx` gets its first 21 opcodes.
* 1.535 (Gackoh): the `D0 xx` family appears (13 opcodes) and the main table
  gains the `D0` dispatcher.
* 1.588 (sekachu): first build with Visual Studio 2010; `D0 1C..1F` are
  replaced by `D0 17, 20..23, 28`; `76` of the main table is dropped; `E0`
  appears in the main table.
* 1.599 (unmei): link-time code generation (the handlers use a register
  calling convention for the VM helpers from here on; same source, different
  code shape).
* 1.616 (kodomo): `80 EC..EF` are dropped (`EC..EE` return in 1.640).
* 1.640 (Tayutama2AS): the base instruction set grows by nine opcodes
  `56..5F` (except 5C); `D0` gains 20 opcodes (`40 41 60..6A 70 72 74 75 78..7A`).
* 1.653 (WakabaironoQuartet): first Visual Studio 2017 build; the `E0 xx`
  family gets its own table (9 opcodes); the prohibited-opcode table appears.
* 1.667 (arcana): seventeen new base opcodes `0F 12 18..1F 2C..2F 3B 3C 73`;
  `7F` (a build-dependent debugging hook before: a no-op in most builds, a
  "検証結果" message box in 1.653) becomes a family dispatcher with a single
  opcode `7F 00` ("未定義のシステム制御命令 $7F%02X"); the conditional jump
  `15` reads an extra 16-bit immediate.

## What the reimplementation does about it

* The base instruction set 00..7E is stable from 1.58 to 1.662: no base
  opcode changes its argument protocol in that range (the only differences
  are the debugging opcodes 79..7B gaining message-box titles in 1.494 and
  7F, the build-dependent hook).  Versions only add: 67 / FF (1.66), 42 74
  75 (1.69), 81 (1.69 build 451), 43 50..54 7E (1.69 build 472), 77 7C (1.494), 66 (1.529),
  44 76 (1.573; 76 dropped again in 1.588), 64 65 (1.588), 56..5F (1.640),
  55 (1.653), 45 (1.654), 46 47 (1.662).  1.667 is the first break: the
  seventeen compact-encoding opcodes (`0F 12 18..1F 2C..2F 3B 3C 73`, with
  inline 8/16/32-bit immediates) and the conditional jump `15`, which
  reads an 8-bit condition and a 16-bit immediate before its operands.
  One VM core covers everything with a version switch for 1.667 on.
* The families only grow (the appendix lists every opcode with the version
  it appeared in), so the newest build's opcode set is a superset of
  every older one except for the few removed opcodes: `80 63` (1.64; the
  number is reused by a different handler from 1.69), `90 8A..8D` and
  `91 8F` (1.66), `92 37` (1.494), `76` (1.588), `D0 1C..1F` (1.588),
  `80 EC..EF` (1.616; `EC..EE` return in 1.640), `91 F4..F6` (1.653).
  A reused number is the one case that needs a per-generation table
  entry (the `OpEntry_t` lists of `src/vm/ops_*.c` give every handler the
  range of generations it serves).
* The opcodes whose argument protocol or messages changed between builds
  have one handler per variant, again selected by generation: `80 64`
  `80 66` `90 10` `90 20..23` `90 5A` `90 5B` `91 98` (1.64); `90 56..5B`
  `90 87` `91 8C..8E` (1.66); `80 4C` (1.69); `80 80` (1.69/451); `90 F4`
  `B0 03` (1.69/472);
  `80 89..8B` `91 38` (1.494); `80 3A` `91 68` (1.529); `91 78 7A 7B`
  (1.535); `91 7B` (1.547); `81 29` (1.573); `80 25` `D0 15` `D0 16`
  (1.588); `80 DC` `D0 28` (1.599); `90 11` `92 1E` `A0 11` (1.640);
  `80 B0 B1 B4 B5 B6` (1.653); `15` (1.667).
* Changes inside the services the handlers call (file formats, the
  renderer, the sound library) are handled where they occur, with a
  comment naming the build that introduced them.
* Not every opcode of the later builds has a handler yet (`--list-missing`
  prints the gaps of a profile).  Of the ones the system programs of the
  games use, only `81 B0` / `81 B7` (1.573 on, the string distance the
  `recoveryrip._bp` of Magical Charming! computes) are known to be
  missing; the system programs of the Lump of Sugar games use no other
  missing one.  A few are placeholders: `90 5D` (1.588 on), the 3-D
  sprite of those builds - "90 5C" with rotations about the X, Y and Z
  axes - takes the rotations and ignores them (the scene-select screens
  of 1.653 on pass 0 for all three, which is the flat "90 5C" picture;
  a rotated one shows unrotated); `B0 8C` (1.616 on), the list-selection dialog
  the title of Haruka Ao no Hanayome ni offers its languages in, answers
  "cancelled"; `81 6C` (1.669), the renderer's output size, is stored
  only; the video textures of "91 F0 .. F7" / "92 F2 .. F5" have no
  player.  A missing opcode stops the game with "not implemented" naming
  it; please report the program and the opcode.

## Appendix: every opcode with the version it first appears in

Per family, the defined opcodes of the newest build (1.669) and of all the
older ones, each with the first version that has it; an opcode that was
dropped again carries the version that dropped it in brackets.

**main** (120): 00:1.58/389.2 01:1.58/389.2 02:1.58/389.2 04:1.58/389.2 05:1.58/389.2 06:1.58/389.2 08:1.58/389.2 09:1.58/389.2 0A:1.58/389.2 0B:1.58/389.2 0C:1.58/389.2 0F:1.667 10:1.58/389.2 11:1.58/389.2 12:1.667 14:1.58/389.2 15:1.58/389.2 16:1.58/389.2 17:1.58/389.2 18:1.667 19:1.667 1A:1.667 1B:1.667 1C:1.667 1D:1.667 1E:1.667 1F:1.667 20:1.58/389.2 21:1.58/389.2 22:1.58/389.2 23:1.58/389.2 24:1.58/389.2 25:1.58/389.2 26:1.58/389.2 27:1.58/389.2 28:1.58/389.2 29:1.58/389.2 2A:1.58/389.2 2B:1.58/389.2 2C:1.667 2D:1.667 2E:1.667 2F:1.667 30:1.58/389.2 31:1.58/389.2 32:1.58/389.2 33:1.58/389.2 34:1.58/389.2 35:1.58/389.2 38:1.58/389.2 39:1.58/389.2 3A:1.58/389.2 3B:1.667 3C:1.667 40:1.58/389.2 42:1.69/444.2 43:1.69/472.2 44:1.573.2 45:1.654.3 46:1.662.1 47:1.662.1 48:1.58/389.2 49:1.58/389.2 50:1.69/472.2 51:1.69/472.2 52:1.69/472.2 53:1.69/472.2 54:1.69/472.2 55:1.653.3 56:1.640.10 57:1.640.10 58:1.640.10 59:1.640.10 5A:1.640.10 5B:1.640.10 5D:1.640.10 5E:1.640.10 5F:1.640.10 60:1.58/389.2 61:1.58/389.2 62:1.58/389.2 63:1.58/389.2 64:1.588.3 65:1.588.3 66:1.529.2 67:1.66/428.6 68:1.58/389.2 69:1.58/389.2 6A:1.58/389.2 6B:1.58/389.2 6C:1.58/389.2 6D:1.58/389.2 6E:1.58/389.2 6F:1.58/389.2 70:1.58/389.2 71:1.58/389.2 73:1.667 74:1.69/444.2 75:1.69/444.2 76:1.573.2[-1.588.3] 77:1.494 78:1.58/389.2 79:1.58/389.2 7A:1.58/389.2 7B:1.58/389.2 7C:1.494 7D:1.58/389.2 7E:1.69/472.2 7F:1.58/389.2 80:1.58/389.2 81:1.69/451.3 90:1.58/389.2 91:1.58/389.2 92:1.58/389.2 A0:1.58/389.2 B0:1.58/389.2 C0:1.58/389.2 D0:1.535.1 E0:1.588.3 FF:1.66/428.6

**7F** (1): 00:1.667

**80** (183): 00:1.58/389.2 01:1.58/389.2 02:1.58/389.2 03:1.640.10 04:1.58/389.2 05:1.69/472.2 06:1.529.2 07:1.529.2 08:1.58/389.2 09:1.553.3 0A:1.66/428.6 0B:1.66/428.6 0C:1.58/389.2 0D:1.66/428.6 0E:1.64/398.3 0F:1.58/389.2 10:1.58/389.2 11:1.58/389.2 12:1.58/389.2 13:1.58/389.2 14:1.58/389.2 15:1.58/389.2 16:1.58/389.2 17:1.58/389.2 18:1.58/389.2 19:1.58/389.2 1A:1.58/389.2 1B:1.58/389.2 1C:1.58/389.2 1D:1.58/389.2 1E:1.58/389.2 1F:1.58/389.2 20:1.58/389.2 21:1.58/389.2 24:1.66/428.6 25:1.66/428.6 26:1.588.3 27:1.494 28:1.58/389.2 29:1.58/389.2 2A:1.58/389.2 2B:1.69/472.2 2C:1.58/389.2 2D:1.58/389.2 2F:1.58/389.2 30:1.58/389.2 31:1.58/389.2 32:1.58/389.2 33:1.58/389.2 34:1.58/389.2 35:1.58/389.2 36:1.58/389.2 37:1.58/389.2 38:1.58/389.2 39:1.69/451.3 3A:1.66/428.6 3B:1.58/389.2 3C:1.58/389.2 3D:1.58/389.2 3E:1.58/389.2 3F:1.58/389.2 40:1.58/389.2 41:1.58/389.2 44:1.58/389.2 45:1.58/389.2 46:1.58/389.2 47:1.58/389.2 48:1.58/389.2 49:1.58/389.2 4A:1.58/389.2 4B:1.58/389.2 4C:1.58/389.2 50:1.58/389.2 52:1.535.1 53:1.494 54:1.58/389.2 58:1.58/389.2 59:1.58/389.2 5A:1.58/389.2 5C:1.58/389.2 5D:1.66/428.6 5E:1.58/389.2 5F:1.58/389.2 60:1.58/389.2 61:1.58/389.2 62:1.58/389.2 63:1.58/389.2 64:1.58/389.2 65:1.64/398.3 66:1.58/389.2 67:1.64/398.3 68:1.58/389.2 69:1.58/389.2 6A:1.58/389.2 6B:1.58/389.2 6C:1.58/389.2 6D:1.58/389.2 6E:1.69/451.3 6F:1.69/444.2 70:1.58/389.2 71:1.58/389.2 74:1.58/389.2 78:1.58/389.2 79:1.58/389.2 7A:1.58/389.2 7B:1.58/389.2 80:1.58/389.2 81:1.58/389.2 82:1.58/389.2 83:1.58/389.2 84:1.58/389.2 85:1.58/389.2 88:1.58/389.2 89:1.58/389.2 8A:1.58/389.2 8B:1.58/389.2 90:1.58/389.2 91:1.58/389.2 94:1.58/389.2 95:1.58/389.2 96:1.58/389.2 97:1.58/389.2 98:1.66/428.6 99:1.66/428.6 9A:1.66/428.6 9C:1.66/428.6 9D:1.66/428.6 9E:1.616.3 9F:1.653.3 A0:1.58/389.2 A1:1.58/389.2 A8:1.58/389.2 A9:1.58/389.2 AC:1.58/389.2 AF:1.573.2 B0:1.58/389.2 B1:1.58/389.2 B4:1.58/389.2 B5:1.58/389.2 B6:1.58/389.2 C0:1.58/389.2 C1:1.58/389.2 C4:1.69/444.2 C5:1.69/444.2 CF:1.599 D0:1.58/389.2 D1:1.58/389.2 D2:1.58/389.2 D3:1.58/389.2 D4:1.58/389.2 D8:1.58/389.2 D9:1.58/389.2 DA:1.599 DB:1.599 DC:1.58/389.2 DD:1.58/389.2 DE:1.599 E0:1.58/389.2 E1:1.58/389.2 E2:1.58/389.2 E3:1.58/389.2 E8:1.64/398.3 E9:1.69/472.2 EA:1.529.2 EC:1.64/398.3 ED:1.69/444.2 EE:1.69/444.2 EF:1.69/444.2[-1.616.3] F0:1.58/389.2 F1:1.58/389.2 F2:1.58/389.2 F3:1.58/389.2 F4:1.58/389.2 F5:1.58/389.2 F6:1.58/389.2 F7:1.58/389.2 F8:1.58/389.2 F9:1.58/389.2 FA:1.58/389.2 FB:1.58/389.2 FC:1.58/389.2 FD:1.58/389.2 FE:1.58/389.2

**81** (91): 00:1.653.3 01:1.659.2 02:1.662.1 03:1.662.1 04:1.616.3 07:1.616.3 08:1.573.2 09:1.573.2 0A:1.529.2 0B:1.529.2 0C:1.529.2 0D:1.529.2 0E:1.529.2 0F:1.529.2 10:1.69/451.3 11:1.69/472.2 14:1.529.2 16:1.616.3 17:1.616.3 18:1.599 19:1.599 1B:1.547 1D:1.547 1E:1.529.2 1F:1.69/451.3 20:1.653.3 21:1.653.3 27:1.653.3 28:1.547 29:1.547 2A:1.547 2B:1.573.2 2C:1.573.2 2D:1.573.2 2F:1.616.3 30:1.529.2 31:1.529.2 32:1.573.2 34:1.662.1 35:1.588.3 36:1.616.3 37:1.616.3 38:1.553.3 39:1.588.3 3A:1.69/472.2 3B:1.69/472.2 3C:1.573.2 3D:1.573.2 3E:1.573.2 44:1.599 48:1.653.3 60:1.494 61:1.547 62:1.599 63:1.529.2 64:1.529.2 65:1.69/472.2 66:1.669.1 67:1.669.1 68:1.535.1 69:1.573.2 6A:1.547 6B:1.547 6C:1.669.1 6D:1.616.3 6E:1.616.3 6F:1.616.3 80:1.640.10 8C:1.667 8D:1.667 8E:1.667 8F:1.667 B0:1.573.2 B7:1.573.2 B8:1.640.10 B9:1.640.10 D0:1.640.10 D1:1.640.10 D2:1.640.10 D3:1.640.10 D4:1.640.10 D5:1.640.10 D8:1.653.3 DA:1.653.3 E0:1.573.2 E9:1.529.2 EA:1.573.2 EC:1.547 ED:1.547 F2:1.529.2 F7:1.553.3

**90** (191): 00:1.58/389.2 01:1.58/389.2 02:1.58/389.2 03:1.58/389.2 04:1.58/389.2 05:1.64/398.3 06:1.529.2 07:1.69/472.2 08:1.58/389.2 09:1.58/389.2 0A:1.58/389.2 0B:1.66/428.6 0C:1.58/389.2 0D:1.58/389.2 0E:1.58/389.2 0F:1.58/389.2 10:1.58/389.2 11:1.58/389.2 12:1.58/389.2 13:1.58/389.2 14:1.58/389.2 15:1.58/389.2 16:1.58/389.2 17:1.494 18:1.58/389.2 19:1.58/389.2 1A:1.58/389.2 1B:1.58/389.2 1C:1.58/389.2 1D:1.58/389.2 1E:1.58/389.2 1F:1.58/389.2 20:1.58/389.2 21:1.58/389.2 22:1.58/389.2 23:1.58/389.2 24:1.64/398.3 28:1.64/398.3 29:1.69/444.2 2C:1.66/428.6 30:1.58/389.2 31:1.58/389.2 32:1.58/389.2 33:1.58/389.2 34:1.58/389.2 35:1.64/398.3 36:1.494 37:1.69/444.2 38:1.58/389.2 39:1.599 3A:1.69/472.2 3C:1.58/389.2 3D:1.58/389.2 3F:1.58/389.2 40:1.58/389.2 41:1.58/389.2 42:1.58/389.2 43:1.58/389.2 44:1.58/389.2 45:1.58/389.2 46:1.58/389.2 47:1.58/389.2 48:1.58/389.2 49:1.58/389.2 4A:1.58/389.2 4C:1.58/389.2 4D:1.69/444.2 50:1.58/389.2 51:1.58/389.2 53:1.69/444.2 54:1.58/389.2 55:1.58/389.2 56:1.58/389.2 57:1.58/389.2 58:1.58/389.2 59:1.58/389.2 5A:1.58/389.2 5B:1.58/389.2 5C:1.69/444.2 5D:1.588.3 60:1.58/389.2 61:1.58/389.2 64:1.58/389.2 65:1.58/389.2 66:1.58/389.2 70:1.58/389.2 71:1.58/389.2 74:1.58/389.2 75:1.58/389.2 76:1.58/389.2 78:1.58/389.2 79:1.58/389.2 7A:1.69/444.2 80:1.58/389.2 81:1.58/389.2 82:1.494 83:1.66/428.6 84:1.58/389.2 85:1.58/389.2 86:1.58/389.2 87:1.58/389.2 88:1.58/389.2 89:1.66/428.6 8A:1.58/389.2[-1.66/428.6] 8B:1.58/389.2[-1.66/428.6] 8C:1.58/389.2[-1.66/428.6] 8D:1.58/389.2[-1.66/428.6] 90:1.58/389.2 91:1.69/444.2 92:1.529.2 94:1.58/389.2 95:1.58/389.2 96:1.58/389.2 97:1.58/389.2 98:1.58/389.2 99:1.58/389.2 9A:1.58/389.2 9B:1.58/389.2 9C:1.58/389.2 9D:1.58/389.2 9E:1.66/428.6 9F:1.58/389.2 A0:1.58/389.2 A1:1.58/389.2 A2:1.58/389.2 A3:1.58/389.2 A4:1.58/389.2 A5:1.58/389.2 A6:1.58/389.2 A7:1.58/389.2 AF:1.58/389.2 B0:1.58/389.2 B1:1.58/389.2 B4:1.58/389.2 B5:1.58/389.2 B6:1.58/389.2 B7:1.69/444.2 B8:1.58/389.2 B9:1.58/389.2 BA:1.58/389.2 BC:1.58/389.2 BD:1.58/389.2 BE:1.58/389.2 BF:1.58/389.2 C0:1.547 C1:1.640.10 C2:1.616.3 C3:1.599 C4:1.553.3 C5:1.553.3 C6:1.599 C7:1.599 C8:1.588.3 CA:1.616.3 CC:1.616.3 CD:1.616.3 CE:1.640.10 CF:1.640.10 D0:1.58/389.2 D1:1.58/389.2 D4:1.58/389.2 D5:1.58/389.2 D6:1.58/389.2 D7:1.58/389.2 D8:1.58/389.2 D9:1.58/389.2 DA:1.58/389.2 DB:1.58/389.2 DC:1.58/389.2 DD:1.58/389.2 DE:1.58/389.2 DF:1.58/389.2 E0:1.58/389.2 E1:1.58/389.2 E4:1.58/389.2 E5:1.58/389.2 E8:1.58/389.2 E9:1.58/389.2 F0:1.58/389.2 F1:1.58/389.2 F2:1.58/389.2 F3:1.58/389.2 F4:1.69/451.3 F5:1.69/451.3 F6:1.69/451.3 F7:1.69/472.2 F8:1.58/389.2 FA:1.58/389.2 FB:1.58/389.2 FC:1.58/389.2 FD:1.58/389.2

**91** (111): 00:1.653.3 03:1.616.3 04:1.659.2 05:1.669.1 06:1.616.3 09:1.653.3 0A:1.640.10 0B:1.616.3 0C:1.553.3 0D:1.547 0E:1.529.2 0F:1.529.2 10:1.58/389.2 11:1.58/389.2 12:1.58/389.2 13:1.58/389.2 14:1.58/389.2 15:1.58/389.2 16:1.494 17:1.573.2 18:1.66/428.6 19:1.66/428.6 1A:1.69/472.2 1B:1.494 1C:1.58/389.2 1D:1.529.2 1E:1.58/389.2 1F:1.66/428.6 31:1.573.2 33:1.69/444.2 36:1.494 37:1.69/472.2 38:1.69/472.2 3D:1.588.3 3E:1.494 3F:1.494 40:1.66/428.6 41:1.66/428.6 42:1.66/428.6 43:1.66/428.6 44:1.66/428.6 45:1.66/428.6 46:1.66/428.6 47:1.66/428.6 48:1.66/428.6 49:1.66/428.6 4A:1.66/428.6 55:1.529.2 60:1.58/389.2 61:1.58/389.2 64:1.58/389.2 65:1.58/389.2 66:1.58/389.2 67:1.58/389.2 68:1.69/444.2 69:1.529.2 70:1.529.2 71:1.529.2 73:1.529.2 74:1.529.2 75:1.529.2 76:1.529.2 78:1.529.2 79:1.529.2 7A:1.529.2 7B:1.529.2 7C:1.529.2 7D:1.529.2 7E:1.547 7F:1.547 88:1.66/428.6 89:1.66/428.6 8A:1.66/428.6 8B:1.66/428.6 8C:1.58/389.2 8D:1.58/389.2 8E:1.58/389.2 8F:1.58/389.2[-1.66/428.6] 90:1.58/389.2 91:1.58/389.2 92:1.58/389.2 93:1.58/389.2 94:1.58/389.2 95:1.58/389.2 96:1.66/428.6 97:1.494 98:1.58/389.2 99:1.529.2 9A:1.529.2 9B:1.494 9C:1.58/389.2 9D:1.58/389.2 9E:1.529.2 9F:1.529.2 B8:1.69/444.2 BA:1.69/444.2 BB:1.529.2 BF:1.66/428.6 DB:1.588.3 F0:1.69/451.3 F1:1.69/451.3 F2:1.69/451.3 F3:1.494 F4:1.69/472.2[-1.653.3] F5:1.69/472.2[-1.653.3] F6:1.69/472.2[-1.653.3] F7:1.494 F8:1.659.2 F9:1.659.2 FA:1.659.2 FB:1.659.2

**92** (46): 00:1.58/389.2 01:1.66/428.6 0E:1.667 10:1.58/389.2 11:1.58/389.2 12:1.535.1 13:1.529.2 14:1.494 15:1.494 16:1.529.2 17:1.529.2 18:1.58/389.2 19:1.58/389.2 1A:1.494 1B:1.640.10 1C:1.58/389.2 1D:1.58/389.2 1E:1.58/389.2 1F:1.66/428.6 31:1.640.10 37:1.69/472.2[-1.494] 3D:1.640.10 88:1.66/428.6 89:1.66/428.6 8A:1.66/428.6 8C:1.66/428.6 8D:1.66/428.6 8E:1.66/428.6 90:1.66/428.6 91:1.66/428.6 94:1.659.2 95:1.659.2 97:1.616.3 98:1.588.3 99:1.653.3 9B:1.640.10 9C:1.66/428.6 9D:1.529.2 9E:1.529.2 9F:1.529.2 F0:1.494 F1:1.547 F2:1.494 F4:1.547 F5:1.573.2 F6:1.573.2

**A0** (30): 00:1.58/389.2 08:1.58/389.2 09:1.58/389.2 10:1.58/389.2 11:1.58/389.2 12:1.58/389.2 14:1.58/389.2 15:1.58/389.2 16:1.58/389.2 17:1.58/389.2 18:1.58/389.2 19:1.58/389.2 1C:1.553.3 20:1.58/389.2 21:1.58/389.2 22:1.58/389.2 23:1.529.2 24:1.58/389.2 25:1.58/389.2 26:1.58/389.2 27:1.547 28:1.547 2C:1.588.3 2F:1.573.2 80:1.58/389.2 81:1.58/389.2 84:1.58/389.2 85:1.58/389.2 86:1.58/389.2 C0:1.66/428.6

**B0** (56): 00:1.58/389.2 02:1.69/472.2 03:1.69/451.3 04:1.58/389.2 05:1.66/428.6 06:1.529.2 08:1.58/389.2 10:1.58/389.2 11:1.58/389.2 14:1.58/389.2 15:1.58/389.2 16:1.58/389.2 17:1.58/389.2 18:1.58/389.2 19:1.58/389.2 1A:1.58/389.2 1C:1.69/472.2 20:1.58/389.2 21:1.69/472.2 22:1.529.2 23:1.69/472.2 24:1.58/389.2 25:1.66/428.6 26:1.58/389.2 27:1.58/389.2 28:1.69/472.2 29:1.69/472.2 2A:1.640.10 80:1.58/389.2 81:1.58/389.2 82:1.64/398.3 83:1.573.2 84:1.58/389.2 85:1.58/389.2 86:1.573.2 87:1.573.2 8C:1.573.2 8F:1.66/428.6 A0:1.588.3 A1:1.588.3 A2:1.588.3 A3:1.588.3 B0:1.669.1 B1:1.669.1 B4:1.669.1 B8:1.669.1 C0:1.69/451.3 C1:1.529.2 C2:1.69/472.2 C3:1.494 C4:1.529.2 C5:1.653.3 C6:1.547 C7:1.535.1 C8:1.659.2 F0:1.66/428.6

**C0** (45): 00:1.58/389.2 01:1.58/389.2 04:1.58/389.2 05:1.58/389.2 06:1.599 08:1.58/389.2 09:1.58/389.2 0A:1.58/389.2 0B:1.58/389.2 0C:1.58/389.2 0D:1.58/389.2 0F:1.58/389.2 10:1.58/389.2 18:1.69/472.2 1A:1.640.10 1B:1.640.10 1F:1.494 20:1.58/389.2 24:1.58/389.2 25:1.58/389.2 28:1.66/428.6 29:1.494 2C:1.66/428.6 2D:1.66/428.6 40:1.64/398.3 41:1.64/398.3 42:1.64/398.3 43:1.66/428.6 44:1.64/398.3 45:1.64/398.3 46:1.64/398.3 47:1.64/398.3 48:1.64/398.3 49:1.64/398.3 4A:1.64/398.3 4B:1.64/398.3 4C:1.64/398.3 4D:1.64/398.3 4E:1.64/398.3 4F:1.64/398.3 C0:1.535.1 C1:1.535.1 C2:1.535.1 C3:1.535.1 F0:1.58/389.2

**D0** (54): 00:1.535.1 01:1.535.1 04:1.535.1 05:1.535.1 10:1.535.1 11:1.535.1 12:1.535.1 14:1.535.1 15:1.535.1 16:1.535.1 17:1.588.3 18:1.599 1C:1.535.1[-1.588.3] 1D:1.535.1[-1.588.3] 1E:1.535.1[-1.588.3] 1F:1.547[-1.588.3] 20:1.588.3 21:1.588.3 22:1.588.3 23:1.588.3 28:1.588.3 2C:1.599 2D:1.616.3 40:1.640.10 41:1.640.10 60:1.640.10 61:1.640.10 62:1.640.10 63:1.640.10 64:1.640.10 65:1.640.10 66:1.640.10 67:1.640.10 68:1.640.10 69:1.640.10 6A:1.640.10 70:1.640.10 71:1.653.3 72:1.640.10 74:1.640.10 75:1.640.10 78:1.640.10 79:1.640.10 7A:1.640.10 7B:1.653.3 80:1.616.3 81:1.616.3 84:1.616.3 87:1.616.3 88:1.616.3 8A:1.616.3 8C:1.616.3 8D:1.616.3 8E:1.616.3

**E0** (10): 00:1.588.3 3F:1.667 40:1.588.3 80:1.588.3 90:1.653.3 92:1.653.3 94:1.653.3 96:1.653.3 C0:1.640.10 C2:1.640.10

