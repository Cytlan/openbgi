# OpenBGI

OpenBGI is an open-source reimplementation, in C99, of Ethornell's
"Buriko General Interpreter" (BGI), the visual-novel engine.  It is
reverse-engineered function by function from the `tayutama.exe` build
(ver 1.69, build 444.2, with the "Wave Master" sound library) and
extended to run the scripts of the engine's other builds, from ver 1.58
to ver 1.669 (`docs/versions.md`).  Every function keeps the original's
results for the same inputs, including its result codes and quirks.  The
Windows specifics sit behind an OS layer (`inc/bgi/os.h`) with a Win32
back end, a Linux back end (X11, FreeType, ALSA) and a WebAssembly back
end (a canvas, the game's files over a websocket; `docs/wasm.md`).

* Website: [OpenBGI.net](https://openbgi.net)
* Discord: <https://discord.gg/3zFTJWUNt7>

**OpenBGI is a work in progress.**  Expect bugs and missing pieces;
please report bugs and issues in our
[Discord](https://discord.gg/3zFTJWUNt7).

**OpenBGI is not a Visual Novel development platform.**  If you wish to
develop a VN on the Ethornell/BGI platform, then you must contact Buriko
Co., Ltd.

## Games

The games we have been working on, with the engine build each one needs
(`docs/versions.md` has the details of the builds):

| Game | Product name | Engine version | Status |
|---|---|---|---|
| Nursery Rhyme (trial) | `NurseryRhymeTrial` | 1.58 | In progress |
| Nursery Rhyme | `NurseryRhyme` | 1.64 | In progress |
| Itsuka, Todoku, Ano Sora ni. | `ItsukaTodokuAnoSorani` | 1.66 | In progress |
| Tayutama -Kiss on my Deity- (trial; the reference build) | `Tayutama` | 1.69 build 444 | In progress |
| Tayutama -Kiss on my Deity- | `Tayutama` | 1.69 build 451 | In progress |
| Tayutama -It's happy days- | `TayutamaHD` | 1.69 build 472 | In progress |
| Prism Rhythm | `PrismRhythm` | 1.494 | In progress |
| Diamic Days | `DiamicDays` | 1.529 | In progress |
| Gakuou -THE ROYAL SEVEN STARS- | `Gackoh` | 1.535 | In progress |
| Gakuou -It's Heartful Days!!- | `Gackoh_HD` | 1.547 | In progress |
| Hanairo Heptagram | `HanairoHeptagram` | 1.553 | In progress |
| Magical Charming! | `MagicalCharming!` | 1.573 | In progress |
| Sekai to Sekai no Mannaka de | `SekaiToSekaiNoMannakaDe` | 1.588 | In progress |
| Unmei Senjou no Phi | `UnmeisenjouNoPhi` | 1.599 | In progress |
| Kodomo no Asobi | `KodomoNoAsobi` | 1.616 | In progress |
| Tayutama 2 -After Stories- | `Tayutama2AS` | 1.640 | In progress |
| Wakabairo no Quartet | `WakabaironoQuartet` | 1.653 | In progress |
| Nekotsuku, Sakura. | `NekoTsukuSakura` | 1.654 | In progress |
| Madohi Shiroki no Kamikakushi | `MadoiShirokinoKamikakushi` | 1.659 | In progress |
| Yumahorome | `Yumahorome` | 1.662 | In progress |
| Arcana Alchemia | `ArcanaAlchemia` | 1.667 | In progress |
| Haruka Ao no Hanayome ni | `HarukaAonoHanayomeni` | 1.669 | In progress |

Only Lump of Sugar games have been tested so far.  We are interested in
testing other BGI-based games: if you have one, tell us how it goes in
our [Discord](https://discord.gg/3zFTJWUNt7).

## Building

    make              Linux (gcc or clang; X11, Xext, FreeType, optionally
                      ALSA, fontconfig and libvorbisfile via pkg-config)
    make win          Windows through MinGW (i686-w64-mingw32-gcc)
    make wasm         the browser through Emscripten (emcc) -> bin/wasm/openbgi.html;
                      `node tools/wasm/server.js <game dir>` serves it (docs/wasm.md)
    make tools        the command-line tools: bin/bpasm, the assembler /
                      disassembler of the game's programs (docs/asm.md);
                      bin/bgiscn, the decompiler / compiler of the scenario
                      files (docs/scenario.md); bin/bgidump and bin/bgipack,
                      which turn a game's archives into editable files and
                      back, for translations (docs/dump.md)
    make check        syntax-check every translation unit with -pedantic
    make test         build and run tests/
    make clean

Plain `make` only, no autotools / CMake.  Objects go to `obj/<platform>/`,
binaries to `bin/`.  `VORBIS=vorbisfile|stb|none` selects the Vorbis
decoder, `OPT=...` the optimisation flags; `docs/building.md` has the
details, the optional dependencies and the state of the Windows and
WebAssembly builds.

## Running

    cd <the game's directory>   # ipl._bp, system.arc, sysprg.arc, ...
    /path/to/bin/bgi [arguments as for the original]

or just `bin/bgi` from anywhere: started outside a game directory (no
`system.arc` or `ipl._bp` where it runs, no path given) the engine opens
a "Select game" window - the games it has run before, or a directory
to pick, with the game it finds there named; "Advanced" uncovers the
engine profile and extra options, Launch starts it and records the
directory in `games.json` for the next time.

The engine build a game needs is chosen from the game's `ipl._bp` through
`games.json`, which also holds the profiles themselves: the engine writes
the default one next to `bgi` on its first run (a game's own directory
may carry one too) - see `docs/versions.md`.  `--engine=NAME` forces a
profile, `--list-engines` prints them, `--games=PATH` names another
table, `--enable=FAM:OP` / `--disable=FAM:OP` override single opcodes;
these options are removed before the rest of the command line reaches
the original's parser.  The original's own options such as `"Do not use
mutex."` (needed for a second instance in the same directory) work as
before.

A game copied straight from its disc carries the disc marker, a 16-byte
file named like the product (`Tayutama`, `HanairoHeptagram`, ...), and
every boot program quits at once when it finds that file next to the
game - the disc is meant to be installed, not run.  The engine hides the
marker from the scripts, so such a copy starts; `--disc-marker` gives
the original's behaviour back.

Movies: the frame-sequence movies of the later builds (`docs/bfmovie.md`)
are decoded by the engine itself.  The opening movies in common formats
(`.mpg`, `.wmv`, ...) go through DirectShow on Windows, as in the
original; on Linux, and on Windows when DirectShow cannot render a file,
the engine plays them through the `ffmpeg` and `ffprobe` programs when it
finds them on the PATH or next to `bgi` (no library is linked: the video
comes in through a pipe, the sound through the engine's own mixer).
Without them the movie is skipped (a note on stderr says so) and the
game goes on, as before.

The later builds (1.529 on) open their window at the size their scripts
ask for, which is the game's own resolution scaled down when the
picture would not fit the screen - the picture is scaled into the window
as the original does with the "81 64" instruction.

The engine's own messages (script errors, notices, the installer) are
shown in English; `--lang=ja` or `BGI_LANG=ja` selects the Japanese
originals, which stay in the binary (`tools/messages.txt` holds both, the
text engine's character tables and the player-facing dialog labels are
never translated).  Message boxes raised by the game's own scripts (the
"B0 80 .. 82" instructions and the installer prompts of the `_bp`
programs) carry whatever text the script passes and are marked with a
"(script)" suffix in their caption, so that an untranslated box can be
told from an engine message.  The window title is the game's own
followed by " - OpenBGI".

The same binary looks inside the game's archives: `--list=ARCHIVE`
prints every entry with what it is (a CompressedBG image and its size, a
BW sound and its codec, a program, a compiled scenario, a BF_Movie);
`--extract=ARCHIVE` writes the entries into a directory (`--out=DIR`,
default the archive's name without its extension) as the engine sees
them after the DSC layer, with `--raw` as stored, with `--convert` turned
into common formats (images as .bmp, sounds as .wav - Vorbis only with a
decoder built in -, BF_Movie frames as one .bmp each); `--entry=NAME`
picks one entry.  `bin/bpasm` (`make tools`) disassembles and assembles
the programs (`docs/asm.md`) and decompiles them to the C-like source
they were written in and compiles that back (`docs/bpc.md`; an edited
source may name the engine's memory through declared variables -
`global int gAbort @ 1124;` - and records - `stSprite[i].x` -, keep its
texts as `#define`d constants and share all of it through `#include`);
`bin/bgiscn` does the same for the scenario files of every build (the
1.494+ containers, the headerless files of 1.66 / 1.69, the 16-bit
scenes of 1.64) and repacks the archive (`docs/scenario.md`).

For what the original cannot do - a message longer than its window,
UTF-8 text in every generation, languages written from right to left -
OpenBGI has instructions of its own, the "EF xx" family, which a game's
programs call when they are rebuilt for it (`docs/obgi.md`).

To translate a game, `bin/bgidump GAMEDIR DUMPDIR` dumps every archive at
once - the programs and scenario files decompiled, the pictures as PNG,
the sounds as Ogg Vorbis, the tables as JSON - and `bin/bgipack DUMPDIR
OUTDIR` builds the archives again from the dump, compiling and encoding
what was edited and copying the rest from the originals unchanged
(`docs/dump.md`).

`--debug` opens the debugger with the game (`--debug=pause` holds the
scripts until F5); F12 in the game's window opens and closes it at any
time.  It shows the threads with their stacks and listings, the loaded
programs, the bitmap slots, the display objects, the sound channels, the
memory areas and a log, and pauses, steps and breaks the VM
(`docs/debugger.md`).  Not in the WebAssembly build.

The Windows build needs no Japanese system locale and no locale
emulator: it calls the wide-character API of Windows and converts the
games' Shift-JIS file names, titles, messages and font names itself, and
a game may sit in a directory whose name Shift-JIS cannot spell
(`docs/os_layer.md`, "Strings").

The Linux build keeps its settings (the original's registry values) in
`~/.config/bgi/`.  Fonts: `BGI_FONT_GOTHIC`, `BGI_FONT_MINCHO` or
`BGI_FONT` name a font file to use instead of what fontconfig / the font
directories offer for "ＭＳ ゴシック" / "ＭＳ 明朝".

## Layout

    inc/bgi/        headers, one per module (gfx/ for graphics, snd/ for
                    sound, sysobj/ for the script-visible system objects)
    src/core/       archives, decoders (DSC, CBG, SDC, BF_Movie), files and
                    paths, save files, strings, the loader queue, JSON;
                    arcread.c, the tools' archive reader, and what only
                    the tools use: the DSC, CBG and BF_Movie encoders, PNG
                    and zlib streams
    src/asm/        the assembler / disassembler of the programs (bin/bpasm); the
                    preprocessor of the source languages (#include, #define)
    src/bpc/        the decompiler / compiler of the programs (bin/bpasm -D / -C)
    src/scn/        the scenario files: container, command table scan,
                    decompiler and compiler (bin/bgiscn)
    src/dump/       dumping a game's archives into editable files and
                    packing them again (bin/bgidump, bin/bgipack)
    src/vm/         the script machine: threads, the scheduler, the opcode
                    families (ops_base, ops_sys, ops_sys81, ops_gfx0 .. 2,
                    ops_snd, ops_ext0, ops_ext1), the tracing aids
    src/wait/       wait objects (text output, tweens, menus, loads, ...)
    src/sys/        the engine object, main window, display and presentation,
                    input, cursor, panels, child windows, text entry,
                    dialogs, CPU identification, the game chooser
                    (launcher.c); install/ the installer
    src/sysobj/     the system objects of the "80 8x .. 80 Dx" instructions:
                    names, flags, the global database, message history,
                    rings, locks, dicts, string tables, hit targets, events
    src/gfx/        bitmaps and blitters (blit*.c), the bitmap manager, fonts,
                    display objects (sprite, map, filter, knob, effector),
                    background/ (the twelve background types), mgr/ (the
                    graphics manager), window/ (the message window), text/
                    (the text engine and layout), particle/ and rain, movies
    src/snd/        the sound manager, wm/ (the Wave Master library), bw/
                    (the BW container and its codecs)
    src/dbg/        the debugger (--debug / F12), views/ its panes
    src/os/         the OS layer: os_common.c, posix/, win32/, wasm/
    tools/          messages.txt -> inc/bgi/msg.h (every Shift-JIS string of
                    the binary, with English translations); win32stub/ and
                    wasmstub/ (the syntax-check stand-ins); wasm/ (the asset
                    server, the page, the browser-less simulator)
    tests/          unit tests (make test)
    docs/           Markdown documentation of the subsystems

`CONVENTIONS.md` has the naming, formatting and commenting rules.  Of the
docs, `building.md` covers the builds and their
optional dependencies, `vm.md` the VM, `versions.md` the engine builds
and the profiles, `bfmovie.md` the frame-sequence movies of the later
builds, `sound.md` the Wave Master library and the BW container,
`os_layer.md` the OS interface, `wasm.md` the browser build, `asm.md` the
assembler / disassembler and its listing format, `scenario.md` the
scenario files, their source language and the translation workflow,
`dump.md` the dump and pack tools and the files of a dump,
`debugger.md` the debugger, `panel_system.md`, `particle_rain.md` and
`child_edit_dialogs.md` the subsystems they name.

Development switches (environment): `BGI_MSGBOX_STDERR=1` (message boxes
print and take their default button), `BGI_TRACE`, `BGI_TRACE_FROM`,
`BGI_TRACE_LAST`, `BGI_TRACE_OPS` (instruction traces, see
`src/vm/trace.c`), `BGI_OPSTAT=<file>` (the executed opcodes),
`BGI_INPUT_DEBUG=1`, `BGI_SND_DEBUG=1` (music opens, loop restarts),
`BGI_PANEL_DEBUG=1` (the buttons of every panel at its start and the
events it queues), `BGI_DEBUG_OVERLAY=1` (the debugger over the game's
picture instead of in a window of its own).

## License

OpenBGI is licensed under the GNU General Public License, version 2
(GPL-2.0); see `LICENSE`.
