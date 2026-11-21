# Architecture

What OpenBGI is made of and the rules its parts keep.

## Principles

- **The game is the authority.** OpenBGI plays a game from its own
  files and reproduces what the original engine does with them,
  quirks included. Where the original has a bug the games depend on,
  OpenBGI has it too, with a comment.
- **One binary, every engine version.** Differences between engine
  builds are data (a profile) and per-version handler variants, not
  separate builds ([versions.md](versions.md)).
- **Self-contained.** Fonts, the launcher, the movie decoder and a
  Vorbis decoder are in the program. The few system libraries it uses
  are ones a desktop has anyway.
- **The core is portable.** It is ISO C99, includes no system header,
  reads no clock, starts no thread and never blocks. Everything it
  needs of a system it asks its host for.
- **No surprises for the player.** OpenBGI starts no other program for
  a game. It does not write into a game that is an archive, a disc
  image or a read-only folder.
- **No DRM circumvention.** A game protected by DRM is not supported
  and is not worked around.
- **Few extensions.** OpenBGI's own instructions exist for what the
  original cannot do; each is off until a program uses it
  ([../modding/obgi.md](../modding/obgi.md)).
- **Not a development platform.** The goal is to run existing games,
  not to be an engine for new ones.

## How a game runs

1. The engine opens the game: a folder, a zip archive or a disc image,
   mounted as one volume (`src/fs/`), with patches and the user's data
   laid over it.
2. It detects the engine version from the game's programs and selects
   a profile (`src/core/detect.c`).
3. It loads the boot program `ipl._bp` and starts the script machine
   (`src/vm/`). A program is bytecode: [formats/programs.md](formats/programs.md).
4. The game's system programs do everything else by calling engine
   instructions: load pictures into bitmap slots, create display
   objects (sprites, windows, backgrounds), play sounds, read input.
   They also interpret the story's scenario files themselves
   ([formats/scenario.md](formats/scenario.md)).
5. Each frame the host calls `OpenBGI_RunFrame`: every script thread
   gets a turn, then the frame services run (effects, panels, input,
   compositing), and the host is handed the picture and the sound
   ([vm.md](vm.md)).

## Layers

| Layer | Folders | May use |
|---|---|---|
| Base | `src/base/` | ISO C only. No engine state. |
| Core | `src/` except `host/`, `movie/`, `tools/` | Base, `include/`, `src/include/`, FreeType, Vorbis |
| Movie decoder | `src/movie/` | FFmpeg's libraries; handed to the core by a host |
| Hosts and OS layer | `src/host/` | The core through `include/openbgi.h`; system headers |
| Tools | `src/tools/` | The core and the OS layer |

`make check-core` enforces the boundary: the core compiles with
`-std=c99 -pedantic -Werror` and `-Iinclude -Isrc/include` alone, and
no `#if` in it may test a platform.

All state of an engine is in `OpenBGI_t` (`src/include/bgi/core.h`).
Several engines can run in one process; the launcher's previews and
the tools rely on that.

## The source tree

| Folder | Content |
|---|---|
| `include/` | The public API: `openbgi.h` (the engine as a host drives it), `openbgi_ffmovie.h` (the movie decoder) |
| `src/include/bgi/` | The core's own headers, one per part; `core.h` is `OpenBGI_t` |
| `src/base/` | Memory, Shift-JIS (CP932) and bidirectional text, JSON, zlib, PNG / JPEG / BMP / GIF, CompressedBG and BF_Movie with their encoders, font-file names, Windows icons |
| `src/fs/` | The game directory as the game sees it (`fs.c`) over a volume: folder (`voldir.c`), zip (`zip.c`), disc image (`iso.c`); the engine's archives (`arc.c`); loading (`loadfile.c`); save files; listing and extracting |
| `src/core/` | `openbgi.c` (the API), calls of the host (`host*.c`), errors, clocks, script memory, profiles and `openbgi.json` (`version.c`), game detection (`detect.c`), DSC and SDC, the FreeType rasteriser (`ftfont.c`) |
| `src/vm/` | Threads, the scheduler, the opcode families (`ops_*.c`), tracing, followed values |
| `src/wait/` | Wait objects: text output, tweens, menus, loads; `wait_step.c` for waits taken a step per frame |
| `src/sys/` | Start-up (`engine.c`), requests to the host, the main window, display and presentation, input mapping (`inputmap.c`, `touch.c`, `pad.c`, `osk.c`), panels, child windows, the text entry, dialogs, movies shown by the engine (`moviefx.c`), the launcher's engine side, the installer |
| `src/sysobj/` | The system objects of the "80 8x .. 80 Dx" instructions: names, flags, the global database, message history, rings, locks, dictionaries, string tables, events |
| `src/gfx/` | Bitmaps and blitters, the bitmap manager, fonts (`typeface.c`), display objects, `background/`, `mgr/` (the graphics manager), `window/` (the message window), `text/` (the text engine and layout), `particle/`, rain, gray patterns |
| `src/snd/` | The sound manager, the sound out, `wm/` (the library and mixer), `bw/` (the container and codecs) |
| `src/asm/`, `src/bpc/`, `src/scn/` | Assembler and disassembler; program decompiler and compiler; scenario decompiler and compiler |
| `src/dbg/` | The debugger: `dbg.c` (window, run control, breakpoints), `dbgdraw.c`, `views/` (one file per view) |
| `src/ui/` | The dialog toolkit: message box and script dialogs in the classic grey look |
| `src/host/` | The hosts and platform back ends ([os_layer.md](os_layer.md)) |
| `src/movie/` | `ffmovie.c`: FFmpeg behind `openbgi_ffmovie.h` |
| `src/tools/` | The command-line tools: `tools.c` (dispatch, help), `bgidump.c`, `bgipack.c`, `bgiscn.c`, `bpasm.c`, `dump/` (one file per kind of entry) |
| `tools/` | What the build runs: `mkmsg.py` (messages), `mkpack.py` (launcher sources), `mkembfonts.py`, `mkicon.py`, `mkffmpeg.py`, `mkfreetype.py`, `pkgconfig.py`, `mkmacapp.py`, the `check*.py` scripts, `retrofront.c`, the stand-in headers (`win32stub/`, `macstub/`, `wasmstub/`), `wasm/` (the asset server and page) |
| `launcher/` | The launcher's sources, a Buriko program |
| `fonts/`, `icons/` | The built-in fonts and their licence; the program's icon (its drawing `openbgi.svg` and what `make icon` makes of it) and Linux desktop files |
| `shaders/` | The Anime4K shader files the Linux and Windows programs carry ([os_layer.md](os_layer.md#the-picture-through-opengl)) |
| `tests/` | Unit tests |
| `examples/` | `minihost.c`, the smallest host |

## Subsystem pages

| Page | Subsystem |
|---|---|
| [vm.md](vm.md) | Threads, scheduler, main loop, requests, pointers |
| [versions.md](versions.md), [opcodes.md](opcodes.md) | Engine builds, profiles, detection, opcodes by version |
| [os_layer.md](os_layer.md) | Hosts and platform back ends |
| [sound.md](sound.md) | The sound library, mixer and output |
| [input.md](input.md) | Touch and pad mapping |
| [launcher.md](launcher.md) | The launcher's engine side |
| [panel_system.md](panel_system.md) | Buttons and menus |
| [particle_rain.md](particle_rain.md) | Particles and rain |
| [child_edit_dialogs.md](child_edit_dialogs.md) | Child windows, text entry, dialogs |
| [bfmovie.md](bfmovie.md) | Frame sequences and video textures |
| [program_file.md](program_file.md) | A game's check of the engine's executable |
| [formats/](formats/README.md) | File formats |

## Messages and languages

Every string of the engine is in `tools/messages.txt`, in Japanese (the
original's) and English; `tools/mkmsg.py` generates
`src/include/bgi/msg.h`. The engine picks the language from the system,
`--lang=` or `BGI_LANG`. Message boxes raised by a game's own scripts
carry the script's text and are marked "(script)" in their caption.
