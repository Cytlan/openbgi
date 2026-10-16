# Building

Plain `make` only - no autotools, no CMake.  Objects go to
`obj/<platform>/`, binaries to `bin/`.  The Makefile's header comment is
the short form of this file.

## Prerequisites on Linux

* a C99 compiler - gcc (the default) or clang (`make CC=clang`) - and GNU
  make;
* `pkg-config`, the X11 and Xext development files, FreeType
  (`freetype2`), pthreads;
* `python3` for the generated message table (`inc/bgi/msg.h` is produced
  from `tools/messages.txt` by `tools/mkmsg.py` when either is newer) and
  for the Win32 stub check.

Optional, detected with `pkg-config` at build time:

* **ALSA** (`alsa`): sound output.  Without it the sound library runs
  against a silent clock - the mixer is still pulled at 20 ms intervals so
  that fades, loop counters and the "is it still playing" answers keep
  their timing, but nothing is heard.
* **fontconfig** (`fontconfig`): resolves the GDI face names the engine
  asks for ("ＭＳ ゴシック" = MS Gothic, "ＭＳ 明朝" = MS Mincho) to font
  files.  Without it the usual font directories are searched for
  well-known Japanese fonts.  Either way `BGI_FONT_GOTHIC`,
  `BGI_FONT_MINCHO` or `BGI_FONT` in the environment name a font file to
  use instead.
* **libvorbisfile** (`vorbisfile`): the Vorbis codec of the BW sound
  container (codec 3, which the games use for their music and many of
  their effects).  `VORBIS=auto` (the default) takes libvorbisfile when
  `pkg-config` finds it, else `src/snd/stb_vorbis.c` when that single-file
  decoder (public domain, github.com/nothings/stb) has been dropped into
  `src/snd/`, else none.  `VORBIS=vorbisfile`, `VORBIS=stb` and
  `VORBIS=none` force one.  Without a decoder codec-3 sounds play as
  silence of the right length, so that games whose every sound is Vorbis
  still run.  `docs/sound.md` describes the container.

Optional at run time, not linked:

* **ffmpeg and ffprobe**: the opening movies in common formats (`.mpg`,
  `.wmv`, ...), which the Windows original hands to DirectShow.  The
  engine starts the two programs as child processes when a movie is to be
  played - found on the PATH or next to `bgi` / `bgi.exe` - and reads the
  decoded frames and sound through pipes (`src/sys/moviefx.c`,
  `docs/os_layer.md`).  Without them the movie is skipped, as before.  On
  Windows DirectShow is tried first and the programs take over only when
  it cannot render the file.  `make test` generates a short clip with
  `ffmpeg` for `bin/test_moviefx` and skips that part when the program is
  missing.

`OPT=...` replaces the optimisation flags (`-O2 -g` by default).

## Targets

| target | what it builds or does |
|--------|------------------------|
| `make` | the Linux engine, `bin/bgi` |
| `make test` | builds and runs the unit tests of `tests/` (`bin/test_*`): the assembler, the BF_Movie decoder, the blitters, the archive reader and the DSC / CBG decoders, the extraction options, the games table, the ffmpeg movie player, the sound codecs and library, the text engine, the VM's base instructions, the WebAssembly file layer against a mock server and the Win32 string conversions |
| `make check` | syntax-checks every translation unit of the Linux build with `-pedantic` |
| `make check-win` | syntax-checks the Win32 back end against the stub SDK headers (below; no MinGW needed) |
| `make check-wasm` | syntax-checks the WebAssembly back end against a stub Emscripten header (below; no emcc needed) |
| `make win` | the Windows engine through MinGW, `bin/bgi.exe` |
| `make wasm` | the browser build through Emscripten, `bin/wasm/openbgi.html` with its `.js` and `.wasm` |
| `make wasm-sim` | the WebAssembly back end as a native Linux binary, `bin/openbgi-wasmsim`, with no browser involved |
| `make wasm-test` | the asset server's protocol test under node |
| `make tools` | the command-line tools: `bin/bpasm`, the assembler / disassembler (`docs/asm.md`) and decompiler / compiler (`docs/bpc.md`) of the game's programs; `bin/bgiscn`, the decompiler / compiler of the scenario files (`docs/scenario.md`); `bin/bgidump` and `bin/bgipack`, which dump a game's archives into editable files and pack them again (`docs/dump.md`).  `make PLATFORM=win32 tools` builds them as `.exe` through MinGW |
| `make msg` | regenerates `inc/bgi/msg.h` from `tools/messages.txt` |
| `make clean` | removes `obj/` and `bin/` |

The engine is run from inside a game's directory (`cd <game>;
/path/to/bin/bgi`); the README has the command line.

## The Windows build

`make win` builds with `i686-w64-mingw32-gcc` (`CROSS=` selects another
prefix, e.g. `make win CROSS=x86_64-w64-mingw32-`) and links gdi32,
user32, winmm, imm32, comctl32, ole32, oleaut32, shell32, advapi32,
comdlg32, uuid and strmiids.  The optional libraries are looked up with
the toolchain's own `$(CROSS)pkg-config`, never the host's: without a
libvorbisfile built for MinGW the Vorbis decoder is `src/snd/stb_vorbis.c`
when that file is there, else none.  The objects go to `obj/win32/`;
objects left there by another compiler have to be removed first (`rm -rf
obj/win32`), since make does not notice the change.  The Win32 back end
(`src/os/win32/`) is the original's windowing, dialog, GDI, waveOut, MCI
and DirectShow code with the engine's decisions taken out
(`docs/os_layer.md`).

The back end calls the wide-character (W) entry points of Windows only
and converts the engine's Shift-JIS strings itself (`docs/os_layer.md`),
so the binary does not depend on the system's code page; it runs on the
NT line of Windows only (Windows XP and later), not on Windows 9x.

A machine without MinGW can still check the back end: `tools/win32stub/`
is a stand-in `windows.h` (and the other headers the back end includes)
with the structures under their real member names, the constants with
dummy values generated into `autogen.h`, the functions that take or
return strings under their real prototypes and the other API functions
left implicitly declared; `make check-win` runs `python3
tools/win32stub/gen.py`, which compiles every Win32 file (and
`src/sys/main.c`) with `gcc -fsyntax-only -fshort-wchar` against it,
collects the identifiers GCC reports as undeclared into `autogen.h` and
repeats until nothing new turns up.  This catches syntax errors,
misspelt identifiers, wrong structure members and a narrow string given
to a wide-character call or the reverse; it does not check the arguments
of the functions it leaves undeclared, which is what a real `make win`
is for.  `make test` also builds `bin/test_win32wide`, the back end's
string conversions against the same stub (`tests/win32wide.c`).

Resources: the original carries an icon (id 0x65), a cursor (0x6A) and
seven dialog templates in its resource section.  The dialogs are rebuilt
in memory from the layouts of those resources
(`src/os/win32/dlgtemplate.c`); the icon and cursor are loaded with
LoadIcon / LoadCursor and are simply absent when no resource script
provides them - a resource script with `ICON` / `CURSOR` entries under
ids 0x65 / 0x6A brings them back.

## The WebAssembly build

`make wasm` needs `emcc` on the PATH (from emsdk) and produces the page,
which `node tools/wasm/server.js <game directory>` serves together with
the game's files over a websocket; `docs/wasm.md` explains the design,
the server and the page's parameters.  The Vorbis codec needs
`src/snd/stb_vorbis.c` dropped in on this platform (there are no host
libraries); without it those sounds are silent.

This build has not been run against a real Emscripten yet either.  Its
stand-ins:

* `make check-wasm` compiles `src/os/wasm/*.c` with the host compiler
  against `tools/wasmstub/emscripten/emscripten.h`, which declares the
  `EM_JS` / `EM_ASYNC_JS` functions and drops their JavaScript bodies.
  (The bodies still have to tokenise as C, which rules out
  regular-expression literals and empty `''` strings in them.)
* `make test` includes `bin/test_wasmfs` (`tests/wasmfs.c`): the back
  end's file layer, block cache and overlay on the host against
  `tools/wasm/hostmock.c`, a mock of the server.
* `make wasm-test` runs `tools/wasm/selftest.js`, which starts the server
  on a scratch directory and drives it through the real browser-side
  client (`client.js`) under node 22's WebSocket.
* `make wasm-sim` links the engine with the WebAssembly back end,
  `hostmock.c` and `tools/wasm/fakebrowser.c` (the JavaScript side on a
  host: a canvas written out as a picture, the host's clock, a WAV file
  for the sound, a scripted mouse) into a native binary that runs a game
  directory through the back end's own file layer, overlay and
  presentation.

What none of these covers is the JavaScript in the browser - the canvas
text, the `keyCode` mapping, the Web Audio scheduling, Asyncify's
behaviour - so the first real `make wasm` and page load may also need a
round of small fixes.
