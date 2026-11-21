# Engine versions and profiles

The games use builds of the same engine from 1.58 (2005) to 1.669
(2023). One OpenBGI binary runs all of them. What differs between
builds is kept in an engine profile (`src/include/bgi/version.h`,
`o->engine.profile`), selected once at start.

## The builds

| Executable | Product name ("80 E8") | Linked | Engine version (build) | Compat | Compiler |
|---|---|---|---|---|---|
| NurseryRhymeTrial.exe | NurseryRhyme (trial) | 2005-08-02 | 1.58/389.2 | - | VC6 |
| NurseryRhyme.exe | NurseryRhyme | 2005-11-09 | 1.64/398.3 | - | VC6 |
| Itsusora.exe | ItsukaTodokuAnoSorani | 2007-01-11 | 1.66/428.6 | - | VC6 |
| Fortune-Arterial.exe | FortuneArterial (August) | 2008-01-05 | 1.69/438.1 | - | VC6 |
| tayutama.exe | Tayutama (trial; the reference of this reimplementation) | 2008-04-12 | 1.69/444.2 | - | VC6 |
| TayutamaFull.exe | Tayutama (the retail game, `Tayutama.exe` of the disc) | 2008-06-16 | 1.69/451.3 | - | VC6 |
| TayutamaHD.exe | TayutamaHD | 2009-05-18 | 1.69/472.2 | - | VC6 |
| BGI.exe (Kira Kira Curtain Call, English edition) | KiraKiraCurtainCallENG | 2010-04-21 | 1.492.1 | 1.69 | VC6 |
| PrismRhythm.exe | PrismRhythm | 2010-04-27 | 1.494 | 1.69 | VC6 |
| DiamicDays.exe | DiamicDays | 2011-07-29 | 1.529.2 | 1.69 | VC6 |
| Gackoh.exe | Gackoh | 2012-01-10 | 1.535.1 | 1.69 | VC6 |
| Gackoh_HD.exe | Gackoh_HD | 2012-07-11 | 1.547 | 1.69 | VC6 |
| Heptagram.exe | HanairoHeptagram | 2012-10-12 | 1.553.3 | 1.69 | VC6 |
| magicha.exe | MagicalCharming! | 2013-05-14 | 1.573.2 | 1.71 | VC6 |
| sekachu.exe | SekaiToSekaiNoMannakaDe | 2014-01-20 | 1.588.3 | 1.72 | VS2010 |
| unmei.exe | UnmeisenjouNoPhi | 2014-10-13 | 1.599 | 1.72 | VS2010 (LTCG) |
| kodomo.exe | KodomoNoAsobi | 2015-11-09 | 1.616.3 | 1.72 | VS2010 (LTCG) |
| senmomo.exe | SenNoHatouTsukisomeNoKouki (trial) | 2016-09-01 | 1.636.1 | 1.72 | VS2010 (LTCG) |
| Tayutama2AS.exe | Tayutama2AS | 2017-04-05 | 1.640.10 | 1.72 | VS2010 (LTCG) |
| WakabaironoQuartet.exe | WakabaironoQuartet | 2019-05-17 | 1.653.3 | 1.72 | VS2017 |
| Nekotsuku.exe | NekoTsukuSakura | 2019-11-21 | 1.654.3 | 1.72 | VS2017 |
| yoake.exe | YoakemaeyoriRuriironaWin10 (August) | 2020-06-25 | 1.658.2 | 1.72 | VS2017 |
| yoake2.exe | YoakemaeyoriRuriironaPS2Win10 (August) | 2020-06-25 | 1.658.2 | 1.72 | VS2017 |
| yoake3.exe | YoakemaeyoriRuriironaMCWin10 (August) | 2020-06-25 | 1.658.2 | 1.72 | VS2017 |
| madokino.exe | MadoiShirokinoKamikakushi | 2020-11-04 | 1.659.2 | 1.72 | VS2017 |
| yumahorome.exe | Yumahorome | 2021-09-26 | 1.662.1 | 1.72 | VS2017 |
| arcana.exe | ArcanaAlchemia | 2022-08-19 | 1.667 | 1.72 | VS2017 |
| haruyome.exe | HarukaAonoHanayomeni | 2023-04-26 | 1.669.1 | 1.72 | VS2017 |

- "Compat" is the `Compatibility` of the executable's version resource.
- Profile names (`--engine=`, `--list-engines`) are the version without
  the minor build number: `1.69/444`, `1.69/451`, `1.69/472`, `1.529`.
- Fortune Arterial (build 438) runs on profile `1.69/444`. The three
  Yoake builds (1.658.2) run on `1.659`.
- The default profile is `1.69/444`, the build OpenBGI was first
  verified against.

## What a profile decides

| Item | Where |
|---|---|
| Which opcodes exist | `src/vm/opset.c` (generated), one bit mask per generation |
| Which handler variant serves an opcode | The `{op, fn, GEN_from, GEN_to}` lists in `src/vm/ops_*.c`, filled by `Vm_FillTable` |
| The tagged-pointer layout and the heap limit | The profile's numbers ([vm.md](vm.md)) |
| Behaviour switches inside services | `EngineGen_t` comparisons in the source ("from 1.529 on") |

A script that uses an opcode its engine did not have gets the
original's "undefined instruction" error. An opcode the engine had but
OpenBGI lacks gets "not implemented", naming it.

| Builds | Offset bits | Heap limit |
|---|---|---|
| 1.58 .. 1.66 | 24 | 16 MB |
| 1.69 builds 438 .. 451 | 25 | 32 MB |
| 1.69 build 472 .. 1.669 | 26 | 64 MB |

## openbgi.json

Profiles and known games are built in (`src/core/version.c`) and
written to `openbgi.json` at the first run. From then on the file is
read, so a profile can be changed or a game added without rebuilding.

Search order: `--games=` / `BGI_GAMES`, the game's folder, beside the
binary, the user's data folder. A file that cannot be written is
skipped unless it has `"staticConfig": true` at the top (then it is
used and never written). A file named on the command line is always
used.

| Top-level key | Content |
|---|---|
| `"openbgi"` | The version that wrote the file |
| `"profiles"` | `{"name", "generation", "compat", "tagShift", "tagCode", "tagData", "tagHeap", "dynFirst", "dynCount", "heapLimit", "bootCode", "bootData"}`. `generation` names a built-in profile and selects opcodes and switches; a number left out takes that generation's value. |
| `"games"` | `{"product", "profile", "hash"}` for known games; `{"directory", ...}` for what the launcher keeps ([launcher.md](launcher.md)) |
| `"launcher"` | The launcher's settings |
| `"input"` | Pad and touch settings, here or in a game's entry ([input.md](input.md)) |
| `"movies": "player"` | Use the system's movie player where there is one (Windows) |
| `"staticConfig": true` | Never write this file |

Built-in profiles and games missing from an older file are added in
memory when it is read.

## Detecting the game

`src/core/detect.c` looks at the game's own programs: `ipl._bp` and the
programs of `sysprg.arc`. The first step that decides wins.
`openbgi --detect GAME` prints all three.

1. **Product name.** Most boot programs from 1.529 on compare the
   result of "80 E8" (`sys.get_product_name`) with a literal and quit
   when it differs. That literal is the product name. If the games
   table has it with one profile, that profile is used. An unknown name
   is still what "80 E8" returns afterwards.
2. **Hash.** FNV-1a (64 bits) over `ipl._bp`, then the programs of
   `sysprg.arc` in order of their lower-cased names; each contributes
   its name, size and DSC-decoded bytes. The launcher records the hash
   of every game it starts.
3. **Opcodes.** The programs are read as the disassembler reads them,
   and every build is scored: opcodes it lacks, arguments it would
   reject ("90 38", "91 38", "91 9A", "C0 1F" constants), the 1.667
   jump encoding. The closest build lacks the fewest and, among those,
   has the fewest opcodes the programs never use.

Step 3 is a heuristic. Of 31 games at hand it finds the exact profile
for 16 and the neighbouring one for 10. Add a game's product name or
hash to the table when it is wrong.

## Changes between versions

| Build | Change |
|---|---|
| 1.64 | CompressedBG pictures (1.58 has none) |
| 1.66 | `67`, `FF`; `BGI.hvl` |
| 1.69 | `42`, `74`, `75`; 25-bit pointer layout |
| 1.69/451 | The `81` family begins (`81 10`, `81 1F`); `80 39`, `80 6E`, `90 F4..F6`, `91 F0..F2`, `B0 03`, `B0 C0` |
| 1.69/472 | `43`, `50..54` (64-bit arithmetic), `7E`; 26-bit pointer layout |
| 1.492 | `47` (a * b / c), `81 3F` (path of the engine's program: [program_file.md](program_file.md)); both gone in 1.494 |
| 1.494 | Scenario container; movies from archives (`92 F0`, `92 F2`); word wrap |
| 1.529 | `81` gets 21 opcodes; `66`; priorities 0 .. 0xFFFF; second database layout |
| 1.535 | The `D0` family; dynamic blocks from pools; clock can stop while inactive (`81 68`) |
| 1.547 | Kept script errors (`81 6A`, `81 6B`); a game's own pad keys (`81 1B`) |
| 1.573 | `BURIKO ARC20` archives; CompressedBG version 2; `44`, `76`; the clock ignores jumps |
| 1.588 | `64`, `65`, `E0`; `76` dropped; `D0 1C..1F` replaced |
| 1.599 | Borrower threads (`81 44`) |
| 1.616 | `80 EC..EF` dropped; database "BURIKO GDB 3.00" |
| 1.636 | `56..5F` (not 5C); 20 more `D0` opcodes; `80 EC..EE` return as plug-in DLL calls |
| 1.640 | `81 B9`, `90 CF`, `92 1B`, `92 31` |
| 1.653 | `55`; `E0` family table; UTF-8 strings (`81 00`); `.bss` files |
| 1.654, 1.662 | `45`; `46`, `47` (another meaning than in 1.492) |
| 1.667 | Seventeen compact base opcodes (`0F 12 18..1F 2C..2F 3B 3C 73`); `7F` family; the conditional jump `15` reads a condition byte and a 16-bit target inline |

Rules that follow:

- The base set `00..7E` only grows from 1.58 to 1.662. 1.667 is the
  first break and has a version switch in the VM core.
- A reused number needs a per-generation handler entry: `80 63` (from
  1.69), `80 EC..EE` (from 1.636), `47`.
- Opcodes whose arguments changed have one handler per variant, for
  example `90 10`, `90 5A`, `80 80`, `91 38`, `91 7B`, `80 25`, `15`.
- Removed opcodes: `80 63` (1.64), `90 8A..8D`, `91 8F` (1.66),
  `92 37` (1.492), `47`, `81 3F` (1.494), `76`, `D0 1C..1F` (1.588),
  `80 EC..EF` (1.616), `91 F4..F6` (1.653).

## Not implemented, or placeholders

`openbgi --list-missing GAME` prints the opcodes of a game's profile
that have no handler.

| Opcode | State |
|---|---|
| `81 B0`, `81 B7` (1.573 on) | Missing; used by `recoveryrip._bp` of one game |
| `90 5D` (1.588 on) | The 3-D sprite: rotations are ignored |
| `B0 8C` (1.616 on) | The list-selection dialog answers "cancelled" |
| `81 6C` (1.669) | The renderer's output size is stored only |
| `91 F0..F7`, `92 F2..F5` | Video textures: frames decode, no player ([bfmovie.md](bfmovie.md)) |
| `80 EC..EE` (1.636 on) | Plug-in DLLs: answered as "no such plug-in" |
| `77` | No handler in any generation |

The full list of opcodes by version: [opcodes.md](opcodes.md).
