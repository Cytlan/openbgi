# Building

Plain GNU `make`. No autotools, no CMake.

- Objects: `obj/<platform>-<arch>/`
- Binaries: `bin/<platform>/`, with the architecture in the name
  (`bin/linux/openbgi-x86_64`, `bin/windows/openbgi-x86.exe`)

## Linux

Required:

| What | Debian / Ubuntu package |
|---|---|
| A C99 compiler (gcc, or `make CC=clang`), GNU make | `build-essential` |
| pkg-config | `pkg-config` |
| X11, Xext | `libx11-dev`, `libxext-dev` |
| FreeType | `libfreetype-dev` |
| Python 3 (generated tables) | `python3` |

Optional, found with pkg-config:

| What | Package | Without it |
|---|---|---|
| ALSA | `libasound2-dev` | No sound is heard; timing is kept |
| fontconfig | `libfontconfig-dev` | Font folders are searched for known Japanese fonts |
| libvorbisfile | `libvorbis-dev` | The decoder in the tree is used (`src/snd/stb_vorbis.c`) |
| FFmpeg's libraries, 6.1 or later | `libavformat-dev libavcodec-dev libswscale-dev libswresample-dev libavutil-dev` | `make ffmpeg` builds a private copy ("Movies") |
| nasm | `nasm` | The private FFmpeg is built without x86 assembly (slower) |
| The `ffmpeg` program | `ffmpeg` | The movie tests are skipped |

    make            # bin/linux/openbgi-<arch>
    make test       # the unit tests
    sudo make install

## Targets and settings

| Target | Does |
|---|---|
| `make` | The Linux program |
| `make test` | Builds and runs the unit tests ([testing.md](testing.md)) |
| `make check` | Syntax-checks every source with `-pedantic` |
| `make check-core` | Builds the core alone, without any host, and runs it ([embedding.md](embedding.md)) |
| `make lib`, `make lib-example`, `make check-lib` | The core as a library; `examples/minihost.c`; checks of both |
| `make libretro`, `make check-libretro` | The libretro core ([libretro.md](libretro.md)) |
| `make ffmpeg` | Builds FFmpeg's libraries for the movie decoder |
| `make win`, `make win64` | Windows through MinGW, 32-bit and 64-bit |
| `make haiku`, `make macos`, `make android`, `make wasm` | The other systems (below) |
| `make headless` | The test host ([testing.md](testing.md)) |
| `make check-win`, `check-macos`, `check-wasm` | Syntax checks against stand-in headers, without the toolchains |
| `make install`, `install-lib`, `install-libretro`, `install-strip`, `uninstall` | Installing (below) |
| `make msg` | Regenerates `src/include/bgi/msg.h` from `tools/messages.txt` |
| `make icon` | Regenerates the icon's files from its drawing, `icons/openbgi.svg`: `icons/openbgi.ico`, the two large pictures for the Mac, and the icon the engine carries (`src/base/ownicon.inc`). Needs `rsvg-convert` |
| `make clean` | Removes `obj/` and `bin/`; `deps/` stays |

| Setting | Meaning |
|---|---|
| `PLATFORM=` | `posix`, `win32`, `haiku`, `macos`, `android`, `wasm`, `headless` |
| `ARCH=` | `x86`, `x86_64`, `arm`, `arm64` (table below) |
| `CC=`, `CROSS=` | The compiler; the prefix of a cross toolchain |
| `OPT=` | Optimisation flags (default `-O2 -g`) |
| `SAN=address` | Builds with a sanitizer (native targets) |
| `MOVIES=system` / `bundled` / `none` | Where FFmpeg comes from |
| `VORBIS=auto` / `vorbisfile` / `stb` / `none` | The Vorbis decoder |
| `EMBED_FONTS=` | The fonts built in ("Fonts") |
| `ICON=file.ico` | Another icon for the program; `ICON=` builds none in (the window then has the icon the engine carries, the Windows file none) |
| `TOOLS=0` | A program without the command-line tools |
| `LAUNCHER=0` | A program without the launcher |
| `GL=0` | Linux and Windows: a program that never shows the picture through OpenGL |
| `SHADERS=0` | .. one with OpenGL but without the Anime4K shader files (`shaders/`) built in |
| `PKGCONF=` | Another pkg-config program; `"python3 tools/pkgconfig.py"` where there is none |

With `TOOLS=1` (the default for desktop programs) a build also puts the
tools' old names beside the program: `bgidump`, `bgipack`, `bgiscn`,
`bpasm`, `bpc`. They are links to it (on Windows: `.cmd` files that
call `openbgi-<arch>.com`). The program started under such a name is
that tool.

On Windows the same objects are linked twice: `openbgi-<arch>.exe`
(desktop program, no console) and `openbgi-<arch>.com` (console
program; a command prompt prefers `.com`).

## Architectures

| Platform | Default | Others |
|---|---|---|
| Linux | The host's | `ARCH=x86` on x86_64 (`-m32`, i386 libraries), or `CROSS=aarch64-linux-gnu- ARCH=arm64` |
| Windows | x86 (`i686-w64-mingw32-`) | `x86_64` (`x86_64-w64-mingw32-`) |
| Haiku | x86_64 (`x86_64-unknown-haiku-`) | `x86` (`i586-pc-haiku-`, the secondary architecture) |
| macOS | x86_64 | `arm64` |
| Android | x86_64 | `x86`, `arm`, `arm64` |

32-bit Linux on Debian or Ubuntu:

    dpkg --add-architecture i386 && apt update
    apt install gcc-multilib libx11-dev:i386 libxext-dev:i386 libfreetype-dev:i386 \
        libfontconfig-dev:i386 libasound2-dev:i386 libvorbis-dev:i386

Nothing depends on the architecture except the CPUID / RDTSC checks,
which answer "not available" off x86.

## Installing

    make && sudo make install                  # under /usr/local
    make install prefix=/usr DESTDIR=/tmp/pkg  # into a folder for a package
    sudo make uninstall

| Installed | Where under `prefix` |
|---|---|
| The program | `bin/openbgi` |
| Desktop entry, icons (the sizes of the `.ico` file and the drawing as the scalable one), metainfo | `share/applications/`, `share/icons/hicolor/`, `share/metainfo/` |
| Manual page | `share/man/man1/openbgi.1` |
| The manual, `README.md`, `LICENSE`, `NOTICES` | `share/doc/openbgi/` |
| `make install-lib`: the library | `lib/libopenbgi.so.<major>`, `lib/libopenbgi.a`, `lib/libopenbgi-movie.a`, `include/openbgi.h`, `include/openbgi_ffmovie.h`, `lib/pkgconfig/openbgi.pc` |
| `make install-libretro`: the core | `lib/libretro/openbgi_libretro.so`, `share/libretro/info/openbgi_libretro.info` |

`bindir`, `libdir`, `includedir`, `datadir`, `mandir`, `docdir` and
`DESTDIR` can each be set.

## Movies

Movies in common formats are decoded by FFmpeg's libraries behind
`src/movie/ffmovie.c` (`include/openbgi_ffmovie.h`). The engine shows
the frames and mixes the sound itself.

| `MOVIES=` | Source | Default for |
|---|---|---|
| `system` | The system's development files | Native Linux when it has them |
| `bundled` | Static libraries in `deps/ffmpeg/<platform>-<arch>/`, built by `make ffmpeg` | Cross builds; native Linux otherwise |
| `none` | No decoder; movies are skipped | macOS, the browser |

`make ffmpeg [PLATFORM=...] [ARCH=...]` runs `tools/mkffmpeg.py`: it
fetches FFmpeg 7.1.5 from ffmpeg.org (or takes `FFMPEG_TARBALL=file`),
checks its SHA-256, and builds static libraries with these decoders
only:

| | |
|---|---|
| Video | MPEG-1, MPEG-2, WMV 7/8/9, VC-1, Microsoft MPEG-4 v1-v3, H.264 |
| Audio | MP2, MP3, WMA 1/2/Pro, AAC, PCM |
| Files | MPEG program streams, ASF, AVI, MP4 |

Such a build of FFmpeg is under the LGPL 2.1 or later. A Windows
binary with the decoder needs Windows Vista or later; `MOVIES=none`
builds one for older systems, whose movies go to DirectShow.

## Fonts

Four fonts are compiled into every build and stand in for the Windows
fonts the games ask for by name:

| File in `fonts/` | Family | Stands in for | Version |
|---|---|---|---|
| `BIZUDGothic-Regular.ttf` | BIZ UDGothic | MS Gothic and every face of no other class | 1.051 |
| `BIZUDPGothic-Regular.ttf` | BIZ UDPGothic | MS PGothic, MS UI Gothic | 1.051 |
| `BIZUDMincho-Regular.ttf` | BIZ UDMincho | MS Mincho | 1.06 |
| `BIZUDPMincho-Regular.ttf` | BIZ UDPMincho | MS PMincho | 1.06 |

They are Morisawa's BIZ UD fonts, unchanged, from
github.com/googlefonts/morisawa-biz-ud-gothic (tag `v1.051`) and
morisawa-biz-ud-mincho (tag `v1.06`), under the SIL Open Font License
1.1 (`fonts/OFL-*.txt`). They have the proportions of the fonts they
replace and every character of CP932.

    7d2b48d84ef4e65c9f85bcf65fb1fb41c92b134be641fab6a7141d597c9a92b0  BIZUDGothic-Regular.ttf
    258d7156c165f2ff774b6efee637c22c3b950de0d8a10e501137061bc8085d01  BIZUDPGothic-Regular.ttf
    468ee6d9b149ca144809e03841bf18740ecf014e055a00da6ecaf1aaf4165af2  BIZUDMincho-Regular.ttf
    dbcea04578ac1e9d3484525e870ce491bd04361768f4d2ba4b827d96e20f891d  BIZUDPMincho-Regular.ttf

Another set, or none:

    make EMBED_FONTS="gothic=A.ttf mincho=B.otf pgothic=C.ttf pmincho=D.ttf"
    make EMBED_FONTS=

`tools/mkembfonts.py` writes them into a generated C table. A face the
game asks for is opened as: the face itself if installed; its fallback
if the game set one and it is installed; the built-in font of its class
(`--stand-in-font=` forces the design); otherwise whatever the platform
substitutes.

## Windows

Debian / Ubuntu: `apt install gcc-mingw-w64-i686 gcc-mingw-w64-x86-64`.

    make ffmpeg PLATFORM=win32            # once; add ARCH=x86_64 for 64-bit
    make win                              # bin/windows/openbgi-x86.exe
    make win64

- Optional libraries are looked up with `$(CROSS)pkg-config`, never the
  host's. Without a libvorbis for MinGW the tree's decoder is used.
- `MINGW_DEPS=<prefix>` names static libraries built for MinGW, in
  `<prefix>/x86/` and `<prefix>/x86_64/`. libogg and libvorbis build
  with `./configure --host=i686-w64-mingw32 --prefix=<prefix>/x86
  --disable-shared --enable-static`. The libretro core for Windows also
  needs a static FreeType there.
- `make PLATFORM=win32 test` runs the tests under wine when it is on
  the PATH (`TEST_RUN=` names another runner).
- Remove `obj/win32-*` when changing the compiler.
- The binary runs on Windows XP and later (Vista and later with the
  movie decoder).

## Haiku

The toolchain is Haiku's own cross compiler:

    git clone https://review.haiku-os.org/buildtools
    git clone https://review.haiku-os.org/haiku
    mkdir haiku/generated.x86_64 && cd haiku/generated.x86_64
    ../configure --cross-tools-source ../../buildtools --build-cross-tools x86_64

For 32-bit Haiku, in a second build folder:

    ../configure --cross-tools-source ../../buildtools --build-cross-tools x86_gcc2 --build-cross-tools x86

32-bit Haiku is a hybrid: its system is built with GCC 2, and modern
programs use its secondary architecture `x86`. OpenBGI is built for
that one (`i586-pc-haiku-gcc`).

The compiler needs a sysroot laid out like Haiku's `/boot`. On a Haiku
installation:

    pkgman install haiku_devel xlibe_devel freetype_devel fontconfig_devel libvorbis_devel

(32-bit: the same names with `_x86` before `_devel`.) Copy
`/boot/system/develop`, `/boot/system/lib` and
`/boot/system/data/pkgconfig` into `<sysroot>/boot/system/`.

Without a Haiku installation: unpack the `.hpkg` files of those
packages and what they require (from eu.hpkg.haiku-os.org) with Haiku's
`package extract -C <sysroot>/boot/system <file>`. `package` is built
by `configure --host-only` and `jam '<build>package'` in a Haiku
checkout. Each package's `.pc` files name
`/packages/<name>-<version>/.self`; make that a link to
`../../boot/system` in the sysroot.

    make ffmpeg PLATFORM=haiku HAIKU_SYSROOT=/path/to/sysroot
    make haiku HAIKU_SYSROOT=/path/to/sysroot
    make haiku ARCH=x86 HAIKU_SYSROOT=/path/to/sysroot-x86

`HAIKU_CC` and `HAIKU_CXX` replace the compilers (for example clang
with `--target=x86_64-unknown-haiku`).

## macOS

On a Mac with Apple's Command Line Tools (`xcode-select --install`);
nothing else is needed. The window, the picture and the sound are the
system's own (AppKit, Metal, AudioToolbox). FreeType is built from its
source, since a Mac has none; `tools/pkgconfig.py` stands in for the
pkg-config a Mac does not have.

    A="PLATFORM=macos ARCH=arm64 MACOS_CC=cc MOVIES=bundled VORBIS=stb"
    make $A PKGCONF="python3 tools/pkgconfig.py" freetype
    make $A PKGCONF="python3 tools/pkgconfig.py" ffmpeg
    make $A PKGCONF="python3 tools/pkgconfig.py"
    python3 tools/mkmacapp.py --dmg OpenBGI.dmg bin/macos/openbgi-arm64 .

| Step | What it does |
|---|---|
| `make freetype` | `tools/mkfreetype.py` fetches FreeType's release (checksum checked) and builds a static library into `deps/freetype/macos-<arch>/`. Once. |
| `make ffmpeg` | The movie decoder's libraries, as on the other systems ("Movies") |
| `make` | The program. The back end's Objective-C files (`src/host/macos/*.m`) are compiled by the same compiler, with `-fobjc-arc`. |
| `tools/mkmacapp.py` | `OpenBGI.app` and, with `--dmg`, a disk image |

The application:

- Its executable is the program itself. It depends on nothing but
  macOS's own libraries.
- `NSHighResolutionCapable`: the picture is drawn at the display's own
  resolution.
- A game's folder, zip archive or disc image can be dropped on its
  icon.
- It is signed ad hoc and not notarised. A Mac that downloads it opens
  it after "Open Anyway" in System Settings
  ([../manual/install.md](../manual/install.md)).

The shaders that scale the picture are compiled when the program
starts: the compiler for them at build time comes only with Xcode.

Cross-compiling with osxcross (`make macos`, `MACOS_CC=o64-clang` or
`oa64-clang`) needs a macOS SDK, which Apple does not let others
distribute. `MACOS_MIN` is the oldest macOS the binary runs on (11.0
on arm64).

`make check-macos` checks the syntax of the back end without a Mac:
the POSIX files against stand-ins for Apple's C headers, the
Objective-C files with clang against stand-ins for Cocoa, Metal and
QuartzCore (`tools/macstub/`). It finds what does not parse or does not
fit the stand-ins, not what a Mac's own headers would say.

## Android

With the Android NDK (r27 is tested), API level 28 or later. For now
the result is a command-line program and the libretro core; there is no
app.

The NDK lacks FreeType, libogg and libvorbis. `ANDROID_DEPS=<prefix>`
names static builds of them, one folder per `ARCH` (`<prefix>/arm64/`
with `include/`, `lib/`, `lib/pkgconfig/`):

    TC=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
    ./configure --host=aarch64-linux-android --prefix=<prefix>/arm64 --disable-shared --enable-static \
        CC=$TC/aarch64-linux-android28-clang AR=$TC/llvm-ar RANLIB=$TC/llvm-ranlib CFLAGS="-O2 -fPIC"

FreeType: add `--without-zlib --without-bzip2 --without-png
--without-harfbuzz --without-brotli`. libvorbis: add
`--with-ogg=<prefix>/arm64`. Triples: `i686-linux-android`,
`x86_64-linux-android`, `armv7a-linux-androideabi`,
`aarch64-linux-android`.

    make ffmpeg  PLATFORM=android ARCH=arm64 ANDROID_NDK=... ANDROID_DEPS=<prefix>
    make android ARCH=arm64 ANDROID_NDK=... ANDROID_DEPS=<prefix>
    make libretro PLATFORM=android ARCH=arm64 ANDROID_NDK=... ANDROID_DEPS=<prefix>

## The browser (WebAssembly)

Needs `emcc` from emsdk (github.com/emscripten-core/emsdk:
`./emsdk install latest && ./emsdk activate latest`, then
`. ./emsdk_env.sh`).

    make wasm                                  # bin/wasm/openbgi.html, .js, .wasm
    node tools/wasm/server.js <game folder>    # serves the page and the game

See [wasm.md](wasm.md). `make check-wasm` syntax-checks the back end
without Emscripten; `make wasm-sim` builds it as a native Linux binary;
`make wasm-test` tests the asset server under node.
