# This file is part of OpenBGI (https://openbgi.net).
# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile - builds OpenBGI
#
#   make                 native Linux build         -> bin/linux/openbgi-<host arch>
#   make win             MinGW cross build, x86     -> bin/windows/openbgi-x86.exe
#   make win64           MinGW cross build, x86_64  -> bin/windows/openbgi-x86_64.exe
#   make haiku           Haiku cross build, x86_64  -> bin/haiku/openbgi-x86_64 (docs/dev/building.md)
#   make macos           macOS build (on a Mac with MACOS_CC=cc, or osxcross) -> bin/macos/openbgi-<arch>; `make freetype PLATFORM=macos` first
#   make android         Android NDK build, x86_64  -> bin/android/openbgi-x86_64 (command line, no display yet)
#   make wasm            Emscripten build           -> bin/wasm/openbgi.html (+ .js, .wasm; docs/dev/wasm.md)
#   make ARCH=x86        any of them for another architecture: x86 (32-bit),
#                        x86_64, arm64 (macOS, Android), arm (Android)
#                        -> bin/<platform>/openbgi-<arch>, objects in obj/<platform>-<arch>/
#   make CC=clang        any other C99 compiler
#   make EMBED_FONTS="gothic=A.ttf mincho=B.otf"
#                        other fonts built into the engine than those of fonts/ (opened for
#                        faces that are not installed); EMBED_FONTS= for none
#   make ICON=file.ico   another icon for the desktop programs than icons/openbgi.ico (the window's,
#                        and the Windows program file's; docs/dev/building.md); ICON= builds none in
#   make icon            icons/openbgi.ico and the icon's other files again from its drawing, icons/openbgi.svg
#   make GL=0            Linux and Windows: the program without the OpenGL output (docs/dev/os_layer.md);
#                        SHADERS=0 with it but without the shader files of shaders/ built in
#   make LAUNCHER=0      the program without the launcher built in (launcher/, docs/dev/launcher.md):
#                        started without a game it asks with the system's dialog, as the builds for
#                        Android and the browser do, which never carry it
#   make TOOLS=0         the program without the command-line tools in it (docs/modding/README.md: `openbgi dump`, `pack`,
#                        `scn`, `asm`, `bpc`), as the builds for Android and the browser are; with them, the
#                        tools' old names are put beside the program: bin/<platform>/bgidump-<arch>, bgipack-,
#                        bgiscn-, bpasm-, bpc- (links to it; on Windows one-line .cmd files that call
#                        openbgi-<arch>.com, the program linked for the console)
#   make test            build and run the unit tests (tests/; Windows builds under wine)
#   make check           syntax-check every source with -pedantic
#   make check-core      the core alone: plain ISO C99, its includes, linked and run without a host's code
#   make lib             the core as a library: bin/<platform>/libopenbgi-<arch>.a and the shared one (.so, .dll)
#   make lib-example     examples/minihost.c, a host in ISO C, built with each -> bin/<platform>/minihost-<arch>, minihost-shared-<arch>
#   make check-lib       both of them: what the shared library lets out, and the example run on an archive made for it
#   make ffmpeg          fetch and build ffmpeg's libraries for the movie decoder, stripped to the games' codecs (MOVIES=bundled)
#   make check-win       syntax-check src/host/win32 against stub SDK headers (no MinGW needed)
#   make check-macos     syntax-check the macOS back end (src/host/macos, src/host/posix) against stub Apple headers (no SDK needed)
#   make check-wasm      syntax-check src/host/wasm against a stub Emscripten header (no emcc needed)
#   make wasm-test       the asset server's protocol test (node)
#   make wasm-sim        the WebAssembly back end as a native binary (bin/linux/openbgi-wasmsim-<arch>, no browser)
#   make headless        the test host: a game run on a clock of its own, pictures of its frames and its sound
#                        written or compared (bin/linux/openbgi-headless-<arch>, no display)
#   make clean
#
# Layout: include/ the API (openbgi.h), src/ the sources with their own headers in src/include/, obj/ objects, bin/ binaries.
# The core is src/ but src/host/, src/movie/ and src/tools/, with the headers of src/include/ and the API of include/; it is compiled
# with those two alone.  src/movie/ is the movie decoder beside it (ffmpeg's libraries; MOVIES, below).
# The hosts are src/host/: their shared files (os_common.c, os_files.c, os_host.c, os_window.c,
# os_machine.c, os_media.c, os_ffmpeg.c, os_preview.c) and headers (os.h, os_common.h, ...: "host/os.h") plus
# src/host/posix/*.c, src/host/win32/*.c or src/host/wasm/*.c; Haiku builds src/host/posix/*.c with
# src/host/haiku/*.cpp, Android and the test host the POSIX system files with src/host/headless/ in place
# of X11.  src/tools/ is the tools' own code (the commands of the program, the dump and pack module;
# TOOLS, below); the hosts, the tools and the tests see the hosts' headers (HOST_INC, -Isrc).
# Only plain make is used; no autotools / CMake.

CC      ?= gcc
# the optional libraries are looked up with pkg-config; a cross build uses
# its toolchain's own (see the win32 platform below), never the host's.
# FreeType is the engine's own rasteriser on the POSIX platforms
# (BGI_HAVE_FREETYPE, src/core/ftfont.c); Windows and the page draw the
# text with GDI and the canvas instead
# PKGCONF is the pkg-config program itself, which the platforms below run
# with their own directories.  Where there is none - a Mac with the Command
# Line Tools alone - PKGCONF="python3 tools/pkgconfig.py" stands in.
PKGCONF ?= pkg-config
PKG_CONFIG ?= $(PKGCONF)
CSTD    := -std=c99
# (two switches more are added below, once the compiler is known: "compiler-
# dependent warnings")
WARN    := -Wall -Wextra -pedantic -Wno-unused-parameter
OPT     ?= -O2 -g
# SAN=address (or address,undefined, ...): a build watched by the compiler's
# sanitizers, in directories of its own (obj/<platform>-<arch>-san/,
# bin/san/) - "make check-core SAN=address" runs the core's reload loop
# under AddressSanitizer, "make headless SAN=address" builds the test host
# for a game's (docs/dev/building.md)
SAN     ?=
ifneq ($(SAN),)
  OPT     := -O1 -g -fsanitize=$(SAN) -fno-omit-frame-pointer
  LDFLAGS += -fsanitize=$(SAN)
endif
API_INC := -Iinclude
INC     := $(API_INC) -Isrc/include
# the hosts' headers (src/host/: "host/os.h"), for the hosts, the tools and the tests - never the core
HOST_INC := -Isrc
CFLAGS  += $(CSTD) -Wall -Wextra -pedantic -Wno-unused-parameter $(OPT) $(INC) -D_POSIX_C_SOURCE=200809L
LDFLAGS +=

# ---- platform -------------------------------------------------------------
# PLATFORM is "posix", "win32", "haiku", "macos", "android", "wasm" (and the
# simulators "wasmsim" and "headless");
# "make win" selects the MinGW toolchain, "make haiku" the Haiku cross
# toolchain, "make macos" osxcross, "make android" the NDK, "make wasm"
# Emscripten (emcc on the PATH, e.g. from emsdk).
PLATFORM ?= posix

# ---- architecture -----------------------------------------------------------
# ARCH is "x86" (32-bit), "x86_64", "arm" (32-bit) or "arm64".  Windows and
# Haiku take x86 and x86_64, macOS x86_64 and arm64, Android all four, a
# native Linux build its host's (and the other x86 one).  Each platform has
# its default - the host's for Linux, x86 for Windows (the original's),
# x86_64 for the rest - and picks the toolchain for the others below.  The
# objects go to obj/<platform>-<arch>/, the binaries to one directory per
# platform, bin/<platform>/, with the architecture in their names:
# bin/linux/openbgi-x86_64, bin/windows/openbgi-x86.exe, bin/haiku/openbgi-x86
# (the browser build, which has none, makes bin/wasm/openbgi.html).
ARCH_OF = $(if $(filter x86_64 amd64,$(1)),x86_64,$(if $(filter i386 i486 i586 i686,$(1)),x86,$(if \
          $(filter aarch64 arm64,$(1)),arm64,$(if $(filter arm%,$(1)),arm,$(1)))))
HOST_ARCH := $(call ARCH_OF,$(firstword $(subst -, ,$(shell $(CC) -dumpmachine 2>/dev/null))))
ifeq ($(PLATFORM),win32)
  DEFAULT_ARCH := x86
else ifneq ($(filter haiku macos android,$(PLATFORM)),)
  DEFAULT_ARCH := x86_64
else
  DEFAULT_ARCH := $(HOST_ARCH)
endif
ARCH ?= $(DEFAULT_ARCH)
ARCHES_win32   := x86 x86_64
ARCHES_haiku   := x86 x86_64
ARCHES_macos   := x86_64 arm64
ARCHES_android := x86 x86_64 arm arm64
ifneq ($(ARCHES_$(PLATFORM)),)
  ifeq ($(filter $(ARCHES_$(PLATFORM)),$(ARCH)),)
    $(error ARCH=$(ARCH): the $(PLATFORM) build takes $(ARCHES_$(PLATFORM)))
  endif
endif
# The targets that only start make again for a platform of their own
# (`make android ARCH=arm64`): when nothing else is asked for, nothing is
# built for the platform of this run, and an ARCH it could not build is no
# error (ONLY_RESTART: yes).
RESTART_GOALS := win win64 haiku macos android wasm wasm-sim headless
ONLY_RESTART := $(if $(MAKECMDGOALS),$(if $(filter-out $(RESTART_GOALS),$(MAKECMDGOALS)),,yes))
OBJDIR := obj/$(PLATFORM)-$(ARCH)$(if $(SAN),-san)
BINDIR_posix   := bin/linux
BINDIR_wasmsim := bin/linux
BINDIR_headless := bin/linux
BINDIR_win32   := bin/windows
BINDIR_haiku   := bin/haiku
BINDIR_macos   := bin/macos
BINDIR_android := bin/android
BINDIR_wasm    := bin/wasm
BINDIR ?= $(if $(SAN),bin/san,$(or $(BINDIR_$(PLATFORM)),bin/$(PLATFORM)))
SUFFIX := -$(ARCH)
# C++ sources (the Haiku platform's only) and the flags they compile with
CXX_SRC  :=
CXXFLAGS += $(OPT) $(INC) -Wall -Wextra -Wno-unused-parameter
ifeq ($(PLATFORM),wasm)
  CC      := emcc
  EXE     := .html
  OS_SRC  := $(wildcard src/host/wasm/*.c)
  CFLAGS  += -DBGI_WASM -DBGI_NO_DEBUGGER
  OPT     := -O2
  # Asyncify lets the engine's blocking calls (file reads over the websocket,
  # sleeps) suspend into the browser's event loop; its stack holds the locals
  # of the suspended frames.  IDBFS persists the overlay (what the engine
  # writes) in IndexedDB.  The page is tools/wasm/shell.html, the websocket
  # client tools/wasm/client.js.
  LDFLAGS += $(OPT) -sASYNCIFY -sASYNCIFY_STACK_SIZE=262144 \
             -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=268435456 -sMAXIMUM_MEMORY=2147483648 \
             -sENVIRONMENT=web -sEXIT_RUNTIME=0 -sINVOKE_RUN=1 \
             -sEXPORTED_FUNCTIONS=_main,_malloc,_free \
             -sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8,HEAPU16,HEAP16,HEAPU32,FS,IDBFS \
             -lidbfs.js --pre-js tools/wasm/client.js --shell-file tools/wasm/shell.html
  OBJDIR  := obj/wasm
  SUFFIX  :=
  # no host libraries: the BW codec 3 (Vorbis) needs the single-file decoder here
  VORBIS  := $(if $(wildcard src/snd/stb_vorbis.c),stb,none)
else ifeq ($(PLATFORM),wasmsim)
  # "make wasm-sim": the WebAssembly back end on the host, with
  # tools/wasm/fakebrowser.c and hostmock.c in place of the browser and the
  # asset server (docs/dev/wasm.md); a native binary that runs a game directory
  # through the back end's own file layer, overlay and presentation
  EXE     :=
  OS_SRC  := $(wildcard src/host/wasm/*.c)
  SIM_SRC := tools/wasm/hostmock.c tools/wasm/fakebrowser.c
  CFLAGS  += -DBGI_WASM -DBGI_POSIX -DBGI_NO_DEBUGGER -I. -Itools/wasmstub
  LDLIBS  += -lm
  TARGET  := $(BINDIR)/openbgi-wasmsim$(SUFFIX)
  VORBIS  := none
else ifeq ($(PLATFORM),headless)
  # "make headless": the test host (src/host/headless/main.c, docs/dev/building.md)
  # on the build machine - the POSIX back end's system services without its
  # X11 files: a window nobody sees, a clock of its own, no sound output (it
  # reads the engine's sound after each frame).  It runs a game for a given
  # time with the input of a script and writes or compares pictures of its
  # frames and its sound.
  EXE     :=
  X11_FILES := child dbgwin dialogs events glx main msgbox uikit window x11 xdnd
  OS_SRC  := $(filter-out $(patsubst %,src/host/posix/%.c,$(X11_FILES)),$(wildcard src/host/posix/*.c)) \
             $(wildcard src/host/headless/*.c)
  CFLAGS  += -DBGI_POSIX -DBGI_HEADLESS -DBGI_HAVE_FREETYPE -D_FILE_OFFSET_BITS=64 $(shell $(PKG_CONFIG) --cflags freetype2 2>/dev/null)
  LDLIBS  += -lfreetype -lpthread -lm
  ifneq ($(shell $(PKG_CONFIG) --exists fontconfig 2>/dev/null && echo yes),)
    CFLAGS += -DBGI_HAVE_FONTCONFIG $(shell $(PKG_CONFIG) --cflags fontconfig)
    LDLIBS += $(shell $(PKG_CONFIG) --libs fontconfig)
  endif
  TARGET  := $(BINDIR)/openbgi-headless$(SUFFIX)
else ifeq ($(PLATFORM),win32)
  # CROSS is the toolchain prefix (by ARCH: i686- or x86_64-w64-mingw32-); it
  # must not be given a value (not even an empty one) before this line, or
  # "?=" keeps that and the host's gcc builds
  CROSS   ?= $(if $(filter x86_64,$(ARCH)),x86_64,i686)-w64-mingw32-
  CC      := $(CROSS)gcc
  # MINGW_DEPS: a prefix with <arch>/ subdirectories of static libraries
  # built for MinGW (libogg, libvorbis; docs/dev/building.md); pkg-config looks
  # there instead of in $(CROSS)pkg-config's directories
  MINGW_DEPS ?=
  ifneq ($(MINGW_DEPS),)
    PKG_CONFIG := PKG_CONFIG_LIBDIR=$(MINGW_DEPS)/$(ARCH)/lib/pkgconfig $(PKGCONF)
    PKG_STATIC := --static
  else
    PKG_CONFIG := $(CROSS)pkg-config
  endif
  EXE     := .exe
  OS_SRC  := $(wildcard src/host/win32/*.c)
  LDLIBS  += -lgdi32 -luser32 -lwinmm -limm32 -lcomctl32 -lole32 -loleaut32 -lshell32 -ladvapi32 -lcomdlg32 -luuid -lstrmiids
  CFLAGS  += -DBGI_WIN32
  LDFLAGS += -mwindows
else ifeq ($(PLATFORM),haiku)
  # Haiku, cross-compiled (docs/dev/building.md): the POSIX back end, its X11
  # window code on Xlibe (Haiku's Xlib, no X server), with the Media Kit
  # sound and the pointer services of src/host/haiku/*.cpp.  CROSS is the
  # toolchain prefix (the same caveat as for win32 applies); HAIKU_CC and
  # HAIKU_CXX replace the compilers outright (e.g. clang with
  # --target=x86_64-unknown-haiku).  HAIKU_SYSROOT, when given, is the tree
  # laid out as Haiku's /boot (system headers and libraries plus the
  # extracted _devel packages); it is passed as --sysroot and is where
  # pkg-config looks.  Empty: the toolchain's own sysroot and pkg-config.
  # The 32-bit Haiku is a hybrid whose modern compiler and libraries are
  # its secondary architecture: ARCH=x86 builds with i586-pc-haiku-gcc
  # against the "x86" subdirectories of develop/headers and develop/lib.
  ifeq ($(ARCH),x86)
    CROSS   ?= i586-pc-haiku-
    HAIKU_SUB := /x86
  else
    CROSS   ?= x86_64-unknown-haiku-
    HAIKU_SUB :=
  endif
  HAIKU_CC  ?= $(CROSS)gcc
  HAIKU_CXX ?= $(CROSS)g++
  # HAIKU_SYSROOT_x86 / HAIKU_SYSROOT_x86_64 (from the environment) stand in
  # for HAIKU_SYSROOT when it is not given
  HAIKU_SYSROOT ?= $(HAIKU_SYSROOT_$(ARCH))
  CC      := $(HAIKU_CC)
  CXX     := $(HAIKU_CXX)
  ifneq ($(HAIKU_SYSROOT),)
    HAIKU_SR := --sysroot=$(HAIKU_SYSROOT)
    PKG_CONFIG := PKG_CONFIG_SYSROOT_DIR=$(HAIKU_SYSROOT) \
                  PKG_CONFIG_LIBDIR=$(HAIKU_SYSROOT)/boot/system/develop/lib$(HAIKU_SUB)/pkgconfig:$(HAIKU_SYSROOT)/boot/system/data/pkgconfig \
                  $(PKGCONF)
  else
    PKG_CONFIG := $(CROSS)pkg-config
  endif
  EXE     :=
  OS_SRC  := $(wildcard src/host/posix/*.c)
  CXX_SRC := $(wildcard src/host/haiku/*.cpp)
  # pthreads and the maths are part of libroot;
  # be / media / game are the kits haiku.cpp uses, which need the C++ runtime.
  # Xlibe's headers are in the default search path; FreeType's directory
  # comes from pkg-config, else from where its _devel package puts it ("="
  # stands for the sysroot); the secondary architecture's directories are
  # named outright, for a compiler that does not search them itself
  FT_CFLAGS := $(shell $(PKG_CONFIG) --cflags freetype2 2>/dev/null)
  HAIKU_INC := $(if $(HAIKU_SUB),-I=/boot/system/develop/headers$(HAIKU_SUB))
  CFLAGS  += $(HAIKU_SR) $(HAIKU_INC) -DBGI_POSIX -DBGI_HAIKU -DBGI_HAVE_FREETYPE \
             $(if $(FT_CFLAGS),$(FT_CFLAGS),-I=/boot/system/develop/headers$(HAIKU_SUB)/freetype2)
  CXXFLAGS += $(HAIKU_SR) $(HAIKU_INC) -DBGI_POSIX -DBGI_HAIKU
  LDFLAGS += $(HAIKU_SR)
  LDLIBS  += $(if $(HAIKU_SUB),-L=/boot/system/develop/lib$(HAIKU_SUB)) -lX11 -lfreetype
  # fontconfig resolves the face names; without it the font directories are searched
  ifneq ($(shell $(PKG_CONFIG) --exists fontconfig 2>/dev/null && echo yes),)
    CFLAGS += -DBGI_HAVE_FONTCONFIG $(shell $(PKG_CONFIG) --cflags fontconfig)
    LDLIBS += $(shell $(PKG_CONFIG) --libs fontconfig)
  endif
  # (libgcc_s: the unwinder the C++ code needs, which the gcc driver adds
  # only when it links as g++)
  HAIKU_LIBS := -lbe -lmedia -lgame -lstdc++ -lgcc_s
else ifeq ($(PLATFORM),macos)
  # macOS (docs/dev/building.md): the POSIX back end's system files with
  # the Objective-C files of src/host/macos/ in the place of its X11 ones -
  # the window is AppKit's, the picture Metal's, the sound AudioToolbox's.
  # FreeType is the tree's own, built once by `make freetype` (below) into
  # FREETYPE_PREFIX: a Mac has none.  MACOS_CC is the compiler ("cc" on a
  # Mac; osxcross's o64-clang for x86_64 and oa64-clang for arm64
  # elsewhere).  MACOS_MIN is the oldest macOS the binary runs on (10.13;
  # 11.0, the first on arm64).
  MACOS_CC  ?= $(if $(filter arm64,$(ARCH)),oa64-clang,o64-clang)
  MACOS_MIN ?= $(if $(filter arm64,$(ARCH)),11.0,10.13)
  CC      := $(MACOS_CC)
  EXE     :=
  X11_FILES := child dbgwin dialogs events glx msgbox uikit window x11 xdnd
  OS_SRC  := $(filter-out $(patsubst %,src/host/posix/%.c,$(X11_FILES)),$(wildcard src/host/posix/*.c))
  OBJC_SRC := $(wildcard src/host/macos/*.m)
  FREETYPE_PREFIX ?= deps/freetype/macos-$(ARCH)
  # (the library by its place in the tree, not by its pkg-config file: that names the directory it was built
  # into, and another FreeType the Mac may have - XQuartz's, Homebrew's - is not the one meant)
  FREETYPE_BUILT := $(FREETYPE_PREFIX)/lib/libfreetype.a
  # _DARWIN_C_SOURCE: the BSD calls (flock, ...) that _POSIX_C_SOURCE alone hides
  CFLAGS  += -mmacosx-version-min=$(MACOS_MIN) -DBGI_POSIX -DBGI_MACOS -DBGI_HAVE_FREETYPE -D_DARWIN_C_SOURCE \
             -I$(FREETYPE_PREFIX)/include/freetype2
  LDFLAGS += -mmacosx-version-min=$(MACOS_MIN)
  LDLIBS  += $(FREETYPE_BUILT) -lpthread -lm -lobjc \
             -framework Cocoa -framework Metal -framework QuartzCore -framework CoreGraphics -framework AudioToolbox \
             -framework CoreFoundation
else ifeq ($(PLATFORM),android)
  # Android (docs/dev/building.md), with the NDK: for now the engine and the
  # tools as command-line programs (adb shell) - the POSIX back end's system
  # services with src/host/headless/headless.c in place of its X11 files: a
  # window that is never seen, no input.  ANDROID_NDK is the NDK, ANDROID_API the oldest
  # API level (28: posix_spawn), ANDROID_DEPS a prefix with <arch>/
  # subdirectories of the static libraries the build needs and the NDK
  # lacks (FreeType, libogg / libvorbis; docs/dev/building.md).
  ANDROID_NDK ?=
  ANDROID_API ?= 28
  ANDROID_DEPS ?=
  ANDROID_TRIPLE_x86    := i686-linux-android
  ANDROID_TRIPLE_x86_64 := x86_64-linux-android
  ANDROID_TRIPLE_arm    := armv7a-linux-androideabi
  ANDROID_TRIPLE_arm64  := aarch64-linux-android
  NDK_HOST := $(if $(filter Darwin,$(shell uname -s)),darwin-x86_64,linux-x86_64)
  CC      := $(ANDROID_NDK)/toolchains/llvm/prebuilt/$(NDK_HOST)/bin/$(ANDROID_TRIPLE_$(ARCH))$(ANDROID_API)-clang
  ifeq ($(wildcard $(CC)),)
    $(warning PLATFORM=android: no compiler at "$(CC)": ANDROID_NDK=<the directory of the NDK> is needed, and ARCH one of $(ARCHES_android) (docs/dev/building.md, "Android"))
  endif
  ifeq ($(ANDROID_DEPS),)
    $(warning PLATFORM=android: ANDROID_DEPS=<prefix> is not set: FreeType and libvorbis, which the NDK lacks, are looked for there (docs/dev/building.md, "Android"))
  endif
  ANDROID_PREFIX := $(if $(ANDROID_DEPS),$(ANDROID_DEPS)/$(ARCH))
  PKG_CONFIG := PKG_CONFIG_LIBDIR=$(ANDROID_PREFIX)/lib/pkgconfig $(PKGCONF)
  PKG_STATIC := --static
  EXE     :=
  X11_FILES := child dbgwin dialogs events glx msgbox uikit window x11 xdnd
  OS_SRC  := $(filter-out $(patsubst %,src/host/posix/%.c,$(X11_FILES)),$(wildcard src/host/posix/*.c)) \
             src/host/headless/headless.c
  CFLAGS  += -DBGI_POSIX -DBGI_ANDROID -DBGI_HAVE_FREETYPE -D_FILE_OFFSET_BITS=64 $(if $(ANDROID_PREFIX),-I$(ANDROID_PREFIX)/include) \
             $(shell $(PKG_CONFIG) --cflags freetype2 2>/dev/null)
  LDLIBS  += $(if $(ANDROID_PREFIX),-L$(ANDROID_PREFIX)/lib) \
             $(shell $(PKG_CONFIG) --libs --static freetype2 2>/dev/null || echo -lfreetype) -lm
else
  # another architecture than the host's: CROSS names a cross toolchain
  # (e.g. i686-linux-gnu-, with its own pkg-config); without one an x86
  # host builds the other x86 flavour with -m32 / -m64 and the libraries of
  # that architecture, which pkg-config finds in the multiarch directories
  ifneq ($(ARCH),$(HOST_ARCH))
    ifneq ($(CROSS),)
      CC := $(CROSS)gcc
      PKG_CONFIG := $(CROSS)pkg-config
    else ifeq ($(ARCH),x86)
      CC := $(CC) -m32
      PKG_CONFIG := PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib32/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig $(PKGCONF)
    else ifeq ($(ARCH),x86_64)
      CC := $(CC) -m64
      PKG_CONFIG := PKG_CONFIG_LIBDIR=/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib64/pkgconfig:/usr/share/pkgconfig $(PKGCONF)
    else ifeq ($(ONLY_RESTART),)
      $(error ARCH=$(ARCH) on a $(HOST_ARCH) host needs CROSS=<toolchain prefix>)
    endif
  endif
  EXE     :=
  OS_SRC  := $(wildcard src/host/posix/*.c)
  LDLIBS  += -lX11 -lXext -lfreetype -lpthread -lm -ldl
  # 64-bit file offsets on 32-bit systems too (the tools' archives can pass 2 GB)
  CFLAGS  += -DBGI_POSIX -DBGI_HAVE_FREETYPE -D_FILE_OFFSET_BITS=64 $(shell $(PKG_CONFIG) --cflags freetype2 2>/dev/null)
  # ALSA output when the development files are present; silence otherwise
  ifneq ($(shell $(PKG_CONFIG) --exists alsa 2>/dev/null && echo yes),)
    CFLAGS += -DBGI_HAVE_ALSA $(shell $(PKG_CONFIG) --cflags alsa)
    LDLIBS += $(shell $(PKG_CONFIG) --libs alsa)
  endif
  # fontconfig resolves the face names; without it well-known font files are searched
  ifneq ($(shell $(PKG_CONFIG) --exists fontconfig 2>/dev/null && echo yes),)
    CFLAGS += -DBGI_HAVE_FONTCONFIG $(shell $(PKG_CONFIG) --cflags fontconfig)
    LDLIBS += $(shell $(PKG_CONFIG) --libs fontconfig)
  endif
endif

# ---- Vorbis (BW codec 3) -------------------------------------------------------
# VORBIS=auto (default) uses libvorbisfile when pkg-config finds it, else
# the decoder in the tree, src/snd/stb_vorbis.c.  VORBIS=vorbisfile / stb /
# none force one; with none the sounds of that codec play as silence.
VORBIS ?= auto
ifeq ($(VORBIS),auto)
  ifneq ($(shell $(PKG_CONFIG) --exists vorbisfile 2>/dev/null && echo yes),)
    VORBIS := vorbisfile
  else ifneq ($(wildcard src/snd/stb_vorbis.c),)
    VORBIS := stb
  else
    VORBIS := none
  endif
endif
ifeq ($(VORBIS),vorbisfile)
  CFLAGS += -DBGI_HAVE_VORBISFILE $(shell $(PKG_CONFIG) --cflags vorbisfile 2>/dev/null)
  # (PKG_STATIC: a prefix of static libraries, which need their dependencies named too)
  LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKG_STATIC) vorbisfile 2>/dev/null || echo -lvorbisfile -lvorbis -logg)
endif
ifeq ($(VORBIS),stb)
  CFLAGS += -DBGI_USE_STB_VORBIS
endif
# ---- the movies (ffmpeg's libraries) -------------------------------------------
# The hosts' movie decoder (src/movie/ffmovie.c, include/openbgi_ffmovie.h) is made
# of ffmpeg's libavformat, libavcodec, libswscale, libswresample and
# libavutil.  MOVIES says where they come from:
#   system   the system's development files, through pkg-config - the
#            default of the native Linux build when it has them
#   bundled  ffmpeg built from its source with only the decoders the
#            games' movies need (tools/mkffmpeg.py; docs/dev/building.md,
#            "Movies"), which `make ffmpeg` does once for a platform and
#            an architecture, into FFMPEG_PREFIX
#            (deps/ffmpeg/<platform>-<arch>/) - the default of the cross
#            builds, and of the native one without the system's files
#   none     no decoder: a movie is skipped (macOS and the browser, for now)
MOVIES ?= auto
comma := ,
FFMPEG_MODULES := libavformat libavcodec libswscale libswresample libavutil
FFMPEG_PREFIX  ?= deps/ffmpeg/$(if $(filter headless wasmsim,$(PLATFORM)),posix,$(PLATFORM))-$(ARCH)
ifeq ($(MOVIES),auto)
  ifneq ($(filter macos wasm wasmsim,$(PLATFORM)),)
    MOVIES := none
  else ifneq ($(filter posix headless,$(PLATFORM)),)
    MOVIES := $(if $(shell $(PKG_CONFIG) --exists $(FFMPEG_MODULES) 2>/dev/null && echo yes),system,bundled)
  else
    MOVIES := bundled
  endif
endif
ifeq ($(MOVIES),system)
  FF_PKG := $(PKG_CONFIG)
else ifeq ($(MOVIES),bundled)
  # (its .pc files name the directory it was built into: wherever the tree is now, it is this one)
  FF_PKG := PKG_CONFIG_LIBDIR=$(FFMPEG_PREFIX)/lib/pkgconfig $(PKGCONF) --static --define-variable=prefix=$(abspath $(FFMPEG_PREFIX))
  FFMPEG_BUILT := $(FFMPEG_PREFIX)/lib/libavcodec.a
else ifneq ($(MOVIES),none)
  $(error MOVIES=$(MOVIES): system, bundled or none)
endif
ifneq ($(MOVIES),none)
  # (-isystem: the libraries' headers are not held to this project's warnings)
  FF_CFLAGS := $(patsubst -I%,-isystem %,$(shell $(FF_PKG) --cflags $(FFMPEG_MODULES) 2>/dev/null))
  CFLAGS    += -DBGI_HAVE_FFMPEG
  LDLIBS    += $(shell $(FF_PKG) --libs $(FFMPEG_MODULES) 2>/dev/null)
  MOVIE_SRC := src/movie/ffmovie.c
  # (Haiku: ffmpeg's assembly objects say what stack they need, Haiku's own start files do not, and the
  # linker takes the silence of those for a wish for an executable one - in every program linked with them)
  LDLIBS    += $(if $(filter haiku,$(PLATFORM)),-Wl$(comma)-z$(comma)noexecstack)
endif
# the Haiku kits come last: they need the C++ runtime after everything else
LDLIBS += $(HAIKU_LIBS)

# ---- compiler-dependent warnings ----------------------------------------------
# A switch that turns a warning off is passed only to a compiler that knows
# it: clang reports one it does not know, which stops the builds that take
# warnings for errors (the core, the library, the libretro core).  cc-knows
# is the switch when $(CC) takes it, else nothing.
cc-knows = $(shell $(CC) -Werror $(1) -fsyntax-only -x c /dev/null >/dev/null 2>&1 && echo $(1))
# -Wno-format-truncation / -Wno-format-overflow: the engine formats paths and
# messages into fixed MAX_PATH-sized buffers as the original does; GCC's
# estimates assume unbounded %s arguments (the inputs are archive and file
# names, which are bounded by the formats that produce them).  GCC has both,
# clang from 18 on (the NDK from r27).
WARN_KNOWN := $(call cc-knows,-Wno-format-truncation) $(call cc-knows,-Wno-format-overflow)
WARN   += $(WARN_KNOWN)
CFLAGS += $(WARN_KNOWN)
# Newer clang (Emscripten's among them) reports the globals that only keep
# what a script set: the original keeps them the same way and nothing reads
# them yet.  Older compilers do not know the switch, so it is probed.
ifneq ($(shell $(CC) -Werror -Wunused-but-set-global -fsyntax-only -x c /dev/null >/dev/null 2>&1 && echo yes),)
  CFLAGS += -Wno-unused-but-set-global
endif
# The browser build keeps its JavaScript inside the C sources (EM_JS), one
# string per function: the longest is past the 4095 characters ISO C99
# promises, which is all -Woverlength-strings reports.
ifeq ($(PLATFORM),wasm)
  CFLAGS += -Wno-overlength-strings
endif

# ---- the launcher --------------------------------------------------------
# Started without a game, the desktop programs show the launcher: a
# Buriko program of OpenBGI's own (launcher/launcher.bpc, docs/dev/launcher.md)
# whose sources are packed into them - LAUNCHER=1, the default for Linux,
# Windows, Haiku, macOS and the test host.  The engine writes the sources
# into the user's data folder and compiles them there itself (and keeps the
# result until a source or the engine changes), so
# no compiled program is kept in the source tree or in the binary.
# LAUNCHER=0 leaves the sources out: the program then asks for a game with
# the system's dialog as before.  The builds whose game comes from
# somewhere else never have them: the browser's, the command-line one for
# Android, the library and the libretro core (a host of the library hands
# the engine a launcher of its own if it wants one, OpenBGI_SetLauncher).
# `make test` says when the sources do not compile.
LAUNCHER ?= $(if $(filter posix win32 haiku macos headless,$(PLATFORM)),1,0)
ifeq ($(LAUNCHER),1)
  CFLAGS += -DBGI_LAUNCHER
endif

# The picture through OpenGL where the system has a graphics processor
# (GL=1: Linux and Windows; src/host/os_gl.c, docs/dev/os_layer.md).
# Nothing of OpenGL is needed to build or to run: its library is looked
# for when the window is made.  The shader files under shaders/ go into
# the program as a table, as the launcher's sources do (SHADERS=0 leaves
# them out: no filters beyond the three then).
GL ?= $(if $(filter posix win32,$(PLATFORM)),1,0)
SHADERS ?= $(GL)
ifeq ($(GL),1)
  CFLAGS += -DBGI_GL
  ifeq ($(SHADERS),1)
    CFLAGS += -DBGI_SHADERS
  endif
endif

# ---- the program's icon ---------------------------------------------------------
# icons/openbgi.ico (docs/dev/building.md; made from the drawing icons/openbgi.svg
# by `make icon`) is the icon of the programs that have a window: built into
# them as a table, which the window takes while the launcher runs and for a
# game without an icon of its own, and into the Windows program as its
# resource 101, the icon of the file.  ICON=FILE names another .ico file,
# ICON= (empty) leaves the icon out: the window then has the smaller set of
# the same icon that the engine carries itself (src/base/ownicon.inc), and
# the Windows file none.
ICON ?= $(if $(filter posix win32 haiku macos,$(PLATFORM)),icons/openbgi.ico)
ifneq ($(ICON),)
  CFLAGS += -DBGI_ICON
endif

# ---- the command-line tools ---------------------------------------------------
# The tools (docs/modding/README.md) are commands of the program - `openbgi dump`,
# `pack`, `scn`, `asm`, `bpc`, `help` - and the program is a tool when it is
# started by a tool's own name (bgidump, bgipack, bgiscn, bpasm, bpc); the
# names are put beside it, below.  TOOLS=0 builds the program without
# them.  The same builds as for the launcher have them: the desktop
# programs and the test host.
TOOLS ?= $(if $(filter posix win32 haiku macos headless,$(PLATFORM)),1,0)
ifeq ($(TOOLS),1)
  CFLAGS += -DBGI_TOOLS
endif

# ---- sources ----------------------------------------------------------------
# src/base/ needs nothing of the engine (`make check-core` and tests/base.c hold it to that)
BASE_SRC := $(wildcard src/base/*.c)
FS_SRC   := $(wildcard src/fs/*.c)
CORE_SRC := $(wildcard src/core/*.c)
ASM_SRC  := $(wildcard src/asm/*.c)
SCN_SRC  := $(wildcard src/scn/*.c)
BPC_SRC  := $(wildcard src/bpc/*.c)
# the tools (TOOLS, above): the commands and the module of dump and pack (docs/modding/dump-pack.md)
TOOLS_SRC := $(if $(filter 1,$(TOOLS)),$(wildcard src/tools/*.c) $(wildcard src/tools/dump/*.c))
VM_SRC   := $(wildcard src/vm/*.c)
WAIT_SRC := $(wildcard src/wait/*.c)
SYS_SRC  := $(wildcard src/sys/*.c) $(wildcard src/sys/install/*.c) $(wildcard src/sysobj/*.c)
GFX_SRC  := $(wildcard src/gfx/*.c) $(wildcard src/gfx/background/*.c) $(wildcard src/gfx/mgr/*.c) \
            $(wildcard src/gfx/particle/*.c) $(wildcard src/gfx/window/*.c) $(wildcard src/gfx/text/*.c)
# (stb_vorbis.c only when it is the Vorbis decoder, above)
SND_SRC  := $(filter-out src/snd/stb_vorbis.c,$(wildcard src/snd/*.c)) $(wildcard src/snd/bw/*.c) $(wildcard src/snd/wm/*.c) \
            $(if $(filter stb,$(VORBIS)),src/snd/stb_vorbis.c)
# the debugger (docs/modding/debugger.md) is not part of the WebAssembly build: the
# hooks in the engine compile to nothing there (BGI_NO_DEBUGGER, src/include/bgi/dbg.h)
DBG_SRC  := $(if $(filter wasm wasmsim,$(PLATFORM)),,$(wildcard src/dbg/*.c) $(wildcard src/dbg/views/*.c))
UI_SRC   := $(wildcard src/ui/*.c)
# the core: the engine and the compilers and decompilers of its formats
ENGINE_SRC := $(BASE_SRC) $(FS_SRC) $(CORE_SRC) $(ASM_SRC) $(SCN_SRC) $(BPC_SRC) $(VM_SRC) $(WAIT_SRC) $(SYS_SRC) $(GFX_SRC) $(SND_SRC) $(DBG_SRC) $(UI_SRC)
# the hosts' shared files; OS_SRC (above) is the platform's back end
HOST_COMMON_SRC := $(wildcard src/host/*.c)

SRC := $(ENGINE_SRC) $(TOOLS_SRC) $(HOST_COMMON_SRC) $(OS_SRC) $(MOVIE_SRC)
OBJ := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRC)) $(patsubst src/%.cpp,$(OBJDIR)/%.o,$(CXX_SRC)) \
       $(patsubst src/%.m,$(OBJDIR)/%.o,$(OBJC_SRC)) $(patsubst tools/%.c,$(OBJDIR)/tools/%.o,$(SIM_SRC))
# the fonts built into the engine (EMBED_FONTS, below): a generated table
OBJ += $(OBJDIR)/gen/embfonts.o
# the launcher's sources built into the desktop programs (LAUNCHER, below): a generated table too
ifeq ($(LAUNCHER),1)
  OBJ += $(OBJDIR)/gen/launcher.o
endif
# the shader files built into the programs that show the picture through OpenGL (SHADERS, above)
ifeq ($(GL)$(SHADERS),11)
  OBJ += $(OBJDIR)/gen/shaders.o
endif
# the program's icon (ICON, above): a generated table, and on Windows a resource
ifneq ($(ICON),)
  OBJ += $(OBJDIR)/gen/icon.o
  ifeq ($(PLATFORM),win32)
    RES_OBJ := $(OBJDIR)/gen/resource.o
  endif
endif
DEP := $(OBJ:.o=.d)
LIB_OBJ := $(filter-out $(OBJDIR)/host/%/main.o,$(OBJ)) # the engine without its entry point (src/host/<platform>/main.c)
# the same for the tests, which draw no game's text and show no launcher: with an empty table of fonts,
# and without the launcher's sources (only the entry points name them)
TOOL_OBJ := $(filter-out $(OBJDIR)/gen/embfonts.o $(OBJDIR)/gen/launcher.o $(OBJDIR)/gen/shaders.o $(OBJDIR)/gen/icon.o,$(LIB_OBJ)) $(OBJDIR)/gen/nofonts.o

# the engine: bin/<platform>/openbgi-<arch>[.exe] (above)
TARGET ?= $(BINDIR)/openbgi$(SUFFIX)$(EXE)

.PHONY: all win win64 haiku macos android wasm wasm-sim headless tools check check-core check-win check-macos check-wasm wasm-test \
        lib lib-example check-lib libretro check-libretro ffmpeg freetype test clean msg icon check-launcher FORCE

# ---- the tools' names beside the program ---------------------------------------
# With the tools in it (TOOLS, above) the program is a tool when it is started
# by a tool's name: links to it on the systems that have links.  On Windows
# the program has no console, so it is linked a second time as a console
# program, openbgi-<arch>.com - the one a command line finds first for
# "openbgi-<arch>" - and each name is a one-line .cmd file beside it that
# calls it with the tool's command.  The test host gets no names: they are
# the program's (it takes the commands all the same).
TOOL_NAMES := bgidump bgipack bgiscn bpasm bpc
TOOL_CMD_bgidump := dump
TOOL_CMD_bgipack := pack
TOOL_CMD_bgiscn  := scn
TOOL_CMD_bpasm   := asm
TOOL_CMD_bpc     := bpc
CONSOLE    :=
TOOL_LINKS :=
ifeq ($(TOOLS),1)
  ifneq ($(filter posix haiku macos,$(PLATFORM)),)
    TOOL_LINKS := $(foreach t,$(TOOL_NAMES),$(BINDIR)/$(t)$(SUFFIX)$(EXE))
  else ifeq ($(PLATFORM),win32)
    CONSOLE    := $(BINDIR)/openbgi$(SUFFIX).com
    TOOL_LINKS := $(foreach t,$(TOOL_NAMES),$(BINDIR)/$(t)$(SUFFIX).cmd)
  endif
endif

all: $(TARGET) $(CONSOLE) $(TOOL_LINKS)

# (the name `make tools` is from when the tools were programs of their own)
tools: all

ifneq ($(CONSOLE),)
$(CONSOLE): $(OBJ) $(RES_OBJ) | bin
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -mwindows,$(LDFLAGS)) -mconsole -o $@ $(OBJ) $(RES_OBJ) $(LDLIBS)

# (%~dp0 is the folder of the .cmd file, %* its arguments; a line of text, so nothing is copied)
$(BINDIR)/%$(SUFFIX).cmd: $(CONSOLE)
	@printf '@"%%~dp0$(notdir $(CONSOLE))" $(TOOL_CMD_$*) %%*\r\n' > $@
else ifneq ($(TOOL_LINKS),)
$(TOOL_LINKS): $(TARGET)
	ln -sf $(notdir $(TARGET)) $@
endif

win:
	$(MAKE) PLATFORM=win32

win64:
	$(MAKE) PLATFORM=win32 ARCH=x86_64

macos:
	$(MAKE) PLATFORM=macos

android:
	$(MAKE) PLATFORM=android

haiku:
	$(MAKE) PLATFORM=haiku

wasm:
	$(MAKE) PLATFORM=wasm

wasm-sim:
	$(MAKE) PLATFORM=wasmsim

headless:
	$(MAKE) PLATFORM=headless

$(TARGET): $(OBJ) $(RES_OBJ) | bin
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(RES_OBJ) $(LDLIBS)

$(OBJDIR)/%.o: src/%.c | src/include/bgi/msg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(HOT_OPT) -MMD -MP -c -o $@ $<

# the movie decoder sees ffmpeg's headers (and nothing of a host's)
$(OBJDIR)/movie/%.o: CFLAGS += $(FF_CFLAGS)
# the picture's scaler is a few loops over every pixel: with -O3 the
# compilers do several pixels at a time, which halves its time (the last
# -O of a command line counts)
PRESENT_OPT := $(if $(filter -O0 -O1 -Og,$(OPT)),,-O3)
$(OBJDIR)/host/os_present.o: CFLAGS += $(PRESENT_OPT)
# so are the blitters, where most of a game's processor time goes: in every build of the core (HOT_OPT in the
# compile rules of the program, the library and the libretro core)
HOT_OPT = $(if $(filter %/gfx/blit.o %/gfx/blit_rows.o %/gfx/blit_modes.o %/gfx/blit_xform.o,$@),$(PRESENT_OPT))
ifneq ($(FFMPEG_BUILT),)
$(OBJDIR)/movie/ffmovie.o: $(FFMPEG_BUILT)
$(FFMPEG_BUILT):
	@echo "openbgi: the movie decoder needs ffmpeg's libraries, which are not in $(FFMPEG_PREFIX) yet:"
	@echo "  make ffmpeg$(if $(filter-out posix,$(PLATFORM)), PLATFORM=$(PLATFORM))$(if $(filter-out $(DEFAULT_ARCH),$(ARCH)), ARCH=$(ARCH))   fetches ffmpeg's source and builds them, once (docs/dev/building.md, \"Movies\")"
	@echo "  make MOVIES=none ...   builds without movies; MOVIES=system uses the system's ffmpeg (pkg-config)"
	@exit 1
endif

# the hosts and the tools' code see the hosts' headers; the core does not, so
# that a core file that includes one fails to compile
$(OBJDIR)/host/%.o: CFLAGS += $(HOST_INC)
$(OBJDIR)/host/%.o: CXXFLAGS += $(HOST_INC)
$(OBJDIR)/tools/%.o: CFLAGS += $(HOST_INC)

$(OBJDIR)/%.o: src/%.cpp | src/include/bgi/msg.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# the macOS back end's Objective-C (src/host/macos/): the C compiler's flags, with the objects' lifetimes kept by the compiler
# (and without the short names of the system's assertion macros - check, verify, require - which are plain words elsewhere)
OBJCFLAGS := -fobjc-arc -D__ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES=0
$(OBJDIR)/%.o: src/%.m | src/include/bgi/msg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OBJCFLAGS) -MMD -MP -c -o $@ $<
ifneq ($(FREETYPE_BUILT),)
$(OBJ): | $(FREETYPE_BUILT)
$(FREETYPE_BUILT):
	@echo "openbgi: the macOS build needs FreeType, which is not in $(FREETYPE_PREFIX) yet:"
	@echo "  make freetype PLATFORM=macos$(if $(filter-out $(DEFAULT_ARCH),$(ARCH)), ARCH=$(ARCH))   fetches its source and builds it, once (docs/dev/building.md)"
	@exit 1
endif

$(OBJDIR)/tools/%.o: tools/%.c | src/include/bgi/msg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# third-party single-file libraries are compiled with the platform's flags
# (a cross build's sysroot and target) but without the project's warnings
$(OBJDIR)/snd/stb_vorbis.o: src/snd/stb_vorbis.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c -o $@ $<

bin:
	@mkdir -p bin

# ---- fonts built into the engine -------------------------------------------------
# The four fonts of fonts/ (BIZ UD Gothic and BIZ UD Mincho, fixed pitch and
# proportional; docs/dev/building.md) are compiled into the engine and opened in
# place of a face the game asks for that is not installed (src/include/bgi/embfonts.h,
# docs/dev/building.md).  EMBED_FONTS="gothic=FILE mincho=FILE pgothic=FILE
# pmincho=FILE" names other TrueType / OpenType fonts, EMBED_FONTS= (empty)
# builds without any.  The table is generated on every build and rewritten
# only when it changes; the tools and the tests are linked with an empty one.
EMBED_FONTS ?= gothic=fonts/BIZUDGothic-Regular.ttf pgothic=fonts/BIZUDPGothic-Regular.ttf \
               mincho=fonts/BIZUDMincho-Regular.ttf pmincho=fonts/BIZUDPMincho-Regular.ttf
$(OBJDIR)/gen/embfonts.c: FORCE
	@mkdir -p $(dir $@)
	@python3 tools/mkembfonts.py $@ $(EMBED_FONTS)

$(OBJDIR)/gen/nofonts.c: tools/mkembfonts.py
	@mkdir -p $(dir $@)
	@python3 tools/mkembfonts.py $@

# (the fonts are long string literals, which -pedantic would report)
$(OBJDIR)/gen/embfonts.o: $(OBJDIR)/gen/embfonts.c src/include/bgi/embfonts.h
	$(CC) $(CFLAGS) -w -c -o $@ $<

$(OBJDIR)/gen/nofonts.o: $(OBJDIR)/gen/nofonts.c src/include/bgi/embfonts.h
	$(CC) $(CFLAGS) -w -c -o $@ $<

FORCE:

# ---- the launcher's sources ---------------------------------------------------
# (the program first: it is the file the engine compiles)
LAUNCHER_SRC := launcher/launcher.bpc $(sort $(wildcard launcher/*.inc))

# The engine keeps the program it compiled of them and compiles again when a source changed - or the
# engine: the pack says which build carries it, by a checksum of everything the engine is built from
# (the file "build.id" of the pack; src/sys/launcher_src.c).
BUILD_ID_SRC := $(ENGINE_SRC) $(filter-out src/host/% src/tools/%,$(wildcard include/*.h src/include/bgi/*.h src/include/bgi/*/*.h \
                src/*/*.h src/*/*/*.h src/*/*.inc src/*/*/*.inc))

$(OBJDIR)/gen/launcher.c: $(LAUNCHER_SRC) tools/mkpack.py $(BUILD_ID_SRC)
	@mkdir -p $(dir $@)
	@python3 tools/mkpack.py $@ gOsLauncherSources $(LAUNCHER_SRC) --build-id $(BUILD_ID_SRC)

$(OBJDIR)/gen/launcher.o: $(OBJDIR)/gen/launcher.c
	$(CC) $(CFLAGS) -w -c -o $@ $<

# ---- the shader files ---------------------------------------------------------
SHADER_SRC := $(sort $(wildcard shaders/*/*.glsl))

$(OBJDIR)/gen/shaders.c: $(SHADER_SRC) tools/mkpack.py
	@mkdir -p $(dir $@)
	@python3 tools/mkpack.py $@ gOsShaders $(SHADER_SRC)

$(OBJDIR)/gen/shaders.o: $(OBJDIR)/gen/shaders.c
	$(CC) $(CFLAGS) -w -c -o $@ $<

# ---- the program's icon -------------------------------------------------------
$(OBJDIR)/gen/icon.c: $(ICON) tools/mkicon.py
	@mkdir -p $(dir $@)
	@python3 tools/mkicon.py --table $(ICON) $@ gOsIcon

$(OBJDIR)/gen/icon.o: $(OBJDIR)/gen/icon.c
	$(CC) $(CFLAGS) -w -c -o $@ $<

# the Windows program's resources: its icon, which Windows shows for the file
# and the window's class takes (resource 101; src/host/win32/window.c)
$(OBJDIR)/gen/resource.o: $(ICON)
	@mkdir -p $(dir $@)
	@printf '101 ICON "%s"\n' "$(abspath $(ICON))" > $(OBJDIR)/gen/resource.rc
	$(CROSS)windres -O coff -o $@ $(OBJDIR)/gen/resource.rc

# whether the sources compile, with the compiler of this build (part of `make test` in a build with the tools)
check-launcher: $(TARGET)
	@mkdir -p $(OBJDIR)/gen
	@$(if $(SAN),env ASAN_OPTIONS=detect_leaks=0) $(TEST_RUN) $(TARGET) bpc launcher/launcher.bpc -o $(OBJDIR)/gen/launcher._bp || { echo "launcher/launcher.bpc does not compile"; exit 1; }
	@echo "  launcher/launcher.bpc compiles"

# the Shift-JIS message table is generated from tools/messages.txt
src/include/bgi/msg.h: tools/messages.txt tools/mkmsg.py
	python3 tools/mkmsg.py

msg: src/include/bgi/msg.h

# the icon's files from its drawing, icons/openbgi.svg: the .ico file of the
# programs, the two large pictures the Mac's application has besides, and the
# table the engine carries (tools/mkicon.py).  They are kept in the tree, so
# only this target needs rsvg-convert (librsvg).
ICON_SIZES := 16 24 32 48 64 128 256
icon:
	@t=$$(mktemp -d) && for s in $(ICON_SIZES); do rsvg-convert -w $$s -h $$s icons/openbgi.svg -o $$t/$$s.png || exit 1; done && \
	    python3 tools/mkicon.py --ico icons/openbgi.ico $(patsubst %,$$t/%.png,$(ICON_SIZES)) && rm -rf $$t
	rsvg-convert -w 512 -h 512 icons/openbgi.svg -o icons/openbgi-512.png
	rsvg-convert -w 1024 -h 1024 icons/openbgi.svg -o icons/openbgi-1024.png
	python3 tools/mkicon.py --own icons/openbgi.ico src/base/ownicon.inc

# syntax check of every translation unit (the platforms share all but src/host;
# "make check PLATFORM=haiku" checks the Haiku build with its toolchain); the
# hosts' headers only for the hosts and the tools' code, as in the build
check:
	@for f in $(SRC); do \
	    echo "  CHECK $$f"; \
	    case $$f in src/host/*|src/tools/*) i="$(HOST_INC)";; src/movie/*) i="$(FF_CFLAGS)";; *) i="";; esac; \
	    $(CC) $(CFLAGS) $$i -fsyntax-only $$f || exit 1; \
	done
	@for f in $(CXX_SRC); do \
	    echo "  CHECK $$f"; \
	    $(CXX) $(CXXFLAGS) $(HOST_INC) -fsyntax-only $$f || exit 1; \
	done

# syntax check of the Win32 back end without MinGW, against the stub SDK
# headers in tools/win32stub/ (see docs/dev/building.md)
check-win:
	python3 tools/win32stub/gen.py

# syntax check of the macOS back end without the SDK: the POSIX system
# files it is built of, with the host's headers and stand-ins for the
# Apple ones in tools/macstub/ (AudioToolbox, dyld, Mach, sysctl, and the
# names stat has there for a file's times; docs/dev/building.md), without
# the compiler's __linux__, so that what is written for Linux alone is not
# seen - and its Objective-C files (src/host/macos/) against stand-ins
# for Cocoa, Metal and QuartzCore, with clang (OBJC_CHECK_CC), the one
# compiler here that reads the language; left out with a word when there
# is none
MACOS_POSIX_SRC := $(filter-out $(patsubst %,src/host/posix/%.c,child dbgwin dialogs events msgbox uikit window x11 xdnd),$(wildcard src/host/posix/*.c))
OBJC_CHECK_CC ?= clang
check-macos:
	@for f in $(MACOS_POSIX_SRC); do \
	    echo "  CHECK $$f"; \
	    $(CC) $(CFLAGS) $(HOST_INC) -DBGI_MACOS -U__linux__ -Itools/macstub -fsyntax-only $$f || exit 1; \
	done
	@if command -v $(OBJC_CHECK_CC) > /dev/null 2>&1; then \
	    for f in src/host/macos/*.m; do \
	        echo "  CHECK $$f"; \
	        $(OBJC_CHECK_CC) -x objective-c $(CSTD) -Wall -Wextra -pedantic -Wno-unused-parameter -fobjc-arc -fblocks -fobjc-runtime=gnustep-2.0 -fsyntax-only \
	            -Iinclude -Isrc/include $(HOST_INC) -D_POSIX_C_SOURCE=200809L -DBGI_POSIX -DBGI_MACOS -U__linux__ -Itools/macstub $$f || exit 1; \
	    done; \
	else echo "  check-macos: no $(OBJC_CHECK_CC): src/host/macos/*.m not checked"; fi

# syntax check of the WebAssembly back end without emcc, against the stub
# Emscripten header in tools/wasmstub/ (the JavaScript bodies are dropped)
check-wasm:
	@for f in src/host/wasm/*.c; do \
	    echo "  CHECK $$f"; \
	    $(CC) $(CFLAGS) $(HOST_INC) -DBGI_WASM -DBGI_NO_DEBUGGER -Itools/wasmstub -fsyntax-only $$f || exit 1; \
	done

# ---- the core alone --------------------------------------------------------------
# check-core builds the core - ENGINE_SRC, all of src/ but src/host/,
# src/movie/ and src/tools/ - on its own as plain ISO C99: include/ and src/include/ alone on the include path,
# no feature or platform macro (no _POSIX_C_SOURCE, no BGI_POSIX, ...; nor
# the C library's fortification, whose wrappers declare POSIX calls), the
# warnings errors.  Its #include lines may name only its own headers and
# those of ISO C, FreeType and Vorbis, and its #if lines test no platform
# (tools/checkcore.py).  It is then linked with tests/core.c - a host made
# of its clocks and a few files in memory - and the libraries the core may
# use, without a line of a host's code, and run where the platform's
# programs run here (natively; under wine for Windows).  The objects go to
# obj/core-<platform>-<arch>/.
CORE_DIR    := obj/core-$(PLATFORM)-$(ARCH)$(if $(SAN),-san)
CORE_CFLAGS := $(CSTD) $(WARN) $(filter-out $(WARN),$(filter -Wno-%,$(CFLAGS))) -Werror $(OPT) -U_FORTIFY_SOURCE $(INC) \
               $(sort $(filter -DBGI_HAVE_FREETYPE -DBGI_HAVE_VORBISFILE -DBGI_USE_STB_VORBIS -DBGI_NO_DEBUGGER -m% --sysroot% -isystem%,$(CFLAGS))) \
               $(sort $(filter-out -Iinclude -Isrc/include -I. -Itools/%,$(filter -I%,$(CFLAGS))))
CORE_LIBS   := $(filter -L% -lfreetype -lvorbisfile -lvorbis -logg -lm,$(LDLIBS))
CORE_OBJ    := $(patsubst src/%.c,$(CORE_DIR)/%.o,$(filter-out src/snd/stb_vorbis.c,$(ENGINE_SRC))) $(CORE_DIR)/gen/embfonts.o \
               $(if $(filter stb,$(VORBIS)),$(CORE_DIR)/snd/stb_vorbis.o)
# where the probe runs: the build machine's own programs, and Windows' under wine
CORE_RUNS   := $(if $(filter posix headless wasmsim,$(PLATFORM)),yes,$(if $(filter win32,$(PLATFORM)),$(if $(shell command -v wine 2>/dev/null),yes)))

check-core:
	@python3 tools/checkcore.py $(filter-out src/snd/stb_vorbis.c,$(ENGINE_SRC)) $(wildcard src/include/bgi/*.h src/include/bgi/*/*.h) tests/core.c
	@$(MAKE) --no-print-directory $(CORE_DIR)/test_core$(EXE)
	@if [ "$(CORE_RUNS)" = yes ]; then echo "  RUN $(CORE_DIR)/test_core$(EXE)"; $(TEST_RUN) $(CORE_DIR)/test_core$(EXE) || exit 1; \
	else echo "  $(CORE_DIR)/test_core$(EXE) built; programs of $(PLATFORM) do not run here"; fi

$(CORE_DIR)/%.o: src/%.c | src/include/bgi/msg.h
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) $(HOT_OPT) -MMD -MP -c -o $@ $<

# (third-party, and the generated fonts' long literals: no warnings, as in the build)
$(CORE_DIR)/snd/stb_vorbis.o: src/snd/stb_vorbis.c
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) -w -c -o $@ $<

$(CORE_DIR)/gen/embfonts.o: $(OBJDIR)/gen/embfonts.c src/include/bgi/embfonts.h
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) -w -c -o $@ $<

$(CORE_DIR)/test_core$(EXE): tests/core.c $(CORE_OBJ)
	$(CC) $(CORE_CFLAGS) -o $@ tests/core.c $(CORE_OBJ) $(CORE_LIBS)

# ---- ffmpeg for the movie decoder ----------------------------------------------
# make ffmpeg builds the bundled libraries (MOVIES, above) for PLATFORM and
# ARCH with this build's compiler: tools/mkffmpeg.py fetches the pinned
# release (or takes FFMPEG_TARBALL), checks its checksum and builds it
# with the decoders the games' movies need and nothing else.
FFMPEG_TARBALL ?=
FFMPEG_OS_posix    := linux
FFMPEG_OS_headless := linux
FFMPEG_OS_win32    := mingw32
FFMPEG_OS_haiku    := haiku
FFMPEG_OS_macos    := darwin
FFMPEG_OS_android  := android
# what tells ffmpeg's configure about a cross toolchain: its tools' prefix (the NDK's are llvm-*), its sysroot
FFMPEG_CROSS := $(if $(filter android,$(PLATFORM)),--cross-prefix=$(dir $(CC))llvm-,$(if $(CROSS),--cross-prefix=$(CROSS))) \
                $(if $(HAIKU_SYSROOT),--sysroot=$(HAIKU_SYSROOT))
# (macOS from another system: ffmpeg's configure cannot run what it builds)
FFMPEG_MACOS_CROSS := $(if $(filter Darwin,$(shell uname -s)),,--cross)
# make freetype (PLATFORM=macos): FreeType for a system that has none,
# into FREETYPE_PREFIX: tools/mkfreetype.py fetches its release (or takes
# FREETYPE_TARBALL), checks its checksum and builds the static library
FREETYPE_TARBALL ?=
freetype:
	@test -n "$(FREETYPE_PREFIX)" || { echo "make freetype: PLATFORM=$(PLATFORM) takes the system's FreeType"; exit 1; }
	python3 tools/mkfreetype.py --prefix=$(FREETYPE_PREFIX) --cc="$(CC)" \
	    $(if $(MACOS_MIN),--extra-cflags="-mmacosx-version-min=$(MACOS_MIN)") \
	    $(if $(filter Darwin,$(shell uname -s)),,--host=$(if $(filter arm64,$(ARCH)),aarch64,x86_64)-apple-darwin) \
	    $(if $(FREETYPE_TARBALL),--tarball=$(FREETYPE_TARBALL))

ffmpeg:
	@test -n "$(FFMPEG_OS_$(PLATFORM))" || { echo "make ffmpeg: no movie decoder for PLATFORM=$(PLATFORM)"; exit 1; }
	python3 tools/mkffmpeg.py --prefix=$(FFMPEG_PREFIX) --target-os=$(FFMPEG_OS_$(PLATFORM)) --arch=$(ARCH) --cc="$(CC)" $(FFMPEG_CROSS) \
	    $(if $(HAIKU_INC),--extra-cflags="$(HAIKU_INC)") $(if $(HAIKU_SUB),--extra-ldflags="-L=/boot/system/develop/lib$(HAIKU_SUB)") \
	    $(if $(MACOS_MIN),--extra-cflags="-mmacosx-version-min=$(MACOS_MIN)" --extra-ldflags="-mmacosx-version-min=$(MACOS_MIN)" $(FFMPEG_MACOS_CROSS)) \
	    $(if $(FFMPEG_TARBALL),--tarball=$(FFMPEG_TARBALL))

# ---- the library -----------------------------------------------------------------
# make lib builds the core as the library a host links, with include/openbgi.h
# its whole interface: the objects of check-core as the static
# bin/<platform>/libopenbgi-<arch>.a, and the same sources once more as
# position-independent code in which only the functions of openbgi.h can be
# seen from outside (OPENBGI_EXPORT there; obj/core-<platform>-<arch>-pic/) as
# the shared one - libopenbgi-<arch>.so.<the interface's major version>,
# with the name without the version for the linker, on the ELF platforms
# (Android: libopenbgi-<arch>.so, no version); openbgi-<arch>.dll with its
# import library libopenbgi-<arch>.dll.a on Windows.  macOS and the browser
# build get the static library alone.  A program that uses the static
# library links the core's own libraries too (CORE_LIBS: FreeType, Vorbis
# unless it is built in, the mathematics).  The movie decoder comes as a
# static library of its own beside them, libopenbgi-movie-<arch>.a, in
# the builds that have it.
#
# make lib-example builds examples/minihost.c - a host in ISO C that plays a
# game from a zip archive without a window - with each of them
# (bin/<platform>/minihost-<arch>, minihost-shared-<arch>); make check-lib
# then checks what the shared library lets out and runs both on an archive
# made for it (tools/checklib.py; under wine for Windows).
API_MAJOR   := $(shell sed -n 's/^\#define OPENBGI_API_MAJOR  *\([0-9][0-9]*\).*/\1/p' include/openbgi.h)
LIB_AR      ?= $(if $(filter android,$(PLATFORM)),$(dir $(CC))llvm-ar,$(CROSS)ar)
LIB_NM      ?= $(if $(filter android,$(PLATFORM)),$(dir $(CC))llvm-nm,$(CROSS)nm)
LIB_OBJDUMP ?= $(CROSS)objdump
LIB_STATIC  := $(BINDIR)/libopenbgi$(SUFFIX).a
PIC_DIR     := $(CORE_DIR)-pic
PIC_OBJ     := $(patsubst $(CORE_DIR)/%,$(PIC_DIR)/%,$(CORE_OBJ))
ifeq ($(PLATFORM),win32)
  LIB_SHARED  := $(BINDIR)/openbgi$(SUFFIX).dll
  # (the compiler's own support code built in: the DLL needs no libgcc DLL beside it)
  LIB_LINK    := -shared -static-libgcc -Wl,--out-implib,$(BINDIR)/libopenbgi$(SUFFIX).dll.a
  PIC_CFLAGS  := $(CORE_CFLAGS) -D'OPENBGI_EXPORT=__declspec(dllexport)'
else ifeq ($(filter macos wasm,$(PLATFORM)),)
  # (Android's packages take libraries named lib*.so alone: no version there)
  LIB_SONAME  := libopenbgi$(SUFFIX).so$(if $(filter android,$(PLATFORM)),,.$(API_MAJOR))
  LIB_SHARED  := $(BINDIR)/$(LIB_SONAME)
  # (--exclude-libs: what a static library linked in - FreeType, Vorbis - defines stays inside too)
  LIB_LINK    := -shared -Wl,-soname,$(LIB_SONAME) -Wl,--exclude-libs,ALL
  LIB_RPATH   := -Wl,-rpath,'$$ORIGIN'
  PIC_CFLAGS  := $(CORE_CFLAGS) -fPIC -fvisibility=hidden -D'OPENBGI_EXPORT=__attribute__((visibility("default")))'
endif
EXAMPLE_CFLAGS  := $(CSTD) $(WARN) -Werror $(OPT) $(API_INC) $(sort $(filter -m% --sysroot% -isystem%,$(CFLAGS)))
MINIHOST        := $(BINDIR)/minihost$(SUFFIX)$(EXE)
MINIHOST_SHARED := $(if $(LIB_SHARED),$(BINDIR)/minihost-shared$(SUFFIX)$(EXE))

# the movie decoder (src/movie/, MOVIES above) as a library of its own beside
# the core's, for a host that hands it to the engine: static, compiled so
# that it can go into a shared object too; whoever links it adds ffmpeg's
LIB_MOVIE   := $(if $(MOVIE_SRC),$(BINDIR)/libopenbgi-movie$(SUFFIX).a)

lib: $(LIB_STATIC) $(LIB_SHARED) $(LIB_MOVIE)

lib-example: $(MINIHOST) $(MINIHOST_SHARED)

check-lib: lib lib-example
	@python3 tools/checklib.py --header=include/openbgi.h --example=examples/minihost.c --nm="$(LIB_NM)" \
	    $(if $(LIB_SHARED),--shared=$(LIB_SHARED) --objdump="$(LIB_OBJDUMP)") \
	    $(if $(LIB_MOVIE),--movie=$(LIB_MOVIE) --movie-header=include/openbgi_ffmovie.h) \
	    --work=$(CORE_DIR)/checklib $(if $(filter yes,$(CORE_RUNS)),--run="$(TEST_RUN)") $(MINIHOST) $(MINIHOST_SHARED)

$(LIB_STATIC): $(CORE_OBJ) | bin
	@mkdir -p $(dir $@)
	rm -f $@
	$(LIB_AR) rcs $@ $(CORE_OBJ)

ifneq ($(LIB_MOVIE),)
$(CORE_DIR)/movie/ffmovie.o: src/movie/ffmovie.c include/openbgi_ffmovie.h include/openbgi.h $(FFMPEG_BUILT)
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) $(filter -fPIC,$(PIC_CFLAGS)) $(FF_CFLAGS) -c -o $@ $<

$(LIB_MOVIE): $(CORE_DIR)/movie/ffmovie.o | bin
	@mkdir -p $(dir $@)
	rm -f $@
	$(LIB_AR) rcs $@ $<
endif

$(PIC_DIR)/%.o: src/%.c | src/include/bgi/msg.h
	@mkdir -p $(dir $@)
	$(CC) $(PIC_CFLAGS) $(HOT_OPT) -MMD -MP -c -o $@ $<

$(PIC_DIR)/snd/stb_vorbis.o: src/snd/stb_vorbis.c
	@mkdir -p $(dir $@)
	$(CC) $(PIC_CFLAGS) -w -c -o $@ $<

$(PIC_DIR)/gen/embfonts.o: $(OBJDIR)/gen/embfonts.c src/include/bgi/embfonts.h
	@mkdir -p $(dir $@)
	$(CC) $(PIC_CFLAGS) -w -c -o $@ $<

ifneq ($(LIB_SHARED),)
$(LIB_SHARED): $(PIC_OBJ) | bin
	@mkdir -p $(dir $@)
	$(CC) $(PIC_CFLAGS) $(LIB_LINK) -o $@ $(PIC_OBJ) $(CORE_LIBS)
	$(if $(filter %.$(API_MAJOR),$(LIB_SONAME)),ln -sf $(LIB_SONAME) $(BINDIR)/libopenbgi$(SUFFIX).so)

$(MINIHOST_SHARED): examples/minihost.c include/openbgi.h $(LIB_SHARED)
	$(CC) $(EXAMPLE_CFLAGS) -o $@ $< -L$(BINDIR) -lopenbgi$(SUFFIX) $(LIB_RPATH)
endif

$(MINIHOST): examples/minihost.c include/openbgi.h $(LIB_STATIC)
	$(CC) $(EXAMPLE_CFLAGS) -o $@ $< $(LIB_STATIC) $(CORE_LIBS)

# ---- the libretro core -------------------------------------------------------------
# make libretro builds the engine as a core for RetroArch and the other
# libretro frontends (src/host/libretro/, docs/dev/libretro.md):
# bin/<platform>/libretro-<arch>/openbgi_libretro.so (.dll on Windows,
# openbgi_libretro_android.so for Android) with its description
# openbgi_libretro.info beside it, for Linux, Windows and Android.  The
# core is one file: the engine with its fonts, the OS layer's files, the
# movie decoder and ffmpeg's libraries - always the ones `make ffmpeg`
# builds, whatever MOVIES says, so that the core runs where no ffmpeg is
# installed - and, where the platform's build has them as static libraries
# (Windows, Android), FreeType and Vorbis.  Only the functions of
# libretro.h can be seen from outside.  The objects go to
# obj/libretro-<platform>-<arch>/.
RETRO_DIR     := obj/libretro-$(PLATFORM)-$(ARCH)
RETRO_NAME    := openbgi_libretro$(if $(filter android,$(PLATFORM)),_android)$(if $(filter win32,$(PLATFORM)),.dll,.so)
RETRO_BIN     := $(BINDIR)/libretro$(SUFFIX)
RETRO_CORE    := $(RETRO_BIN)/$(RETRO_NAME)
RETRO_FFMPEG  := $(FFMPEG_PREFIX)/lib/libavcodec.a
RETRO_FF_PKG  := PKG_CONFIG_LIBDIR=$(FFMPEG_PREFIX)/lib/pkgconfig $(PKGCONF) --static --define-variable=prefix=$(abspath $(FFMPEG_PREFIX))
# (looked up when the core is built, once the libraries are there)
RETRO_FF_CFLAGS = $(patsubst -I%,-isystem %,$(shell $(RETRO_FF_PKG) --cflags $(FFMPEG_MODULES) 2>/dev/null))
RETRO_FF_LIBS   = $(shell $(RETRO_FF_PKG) --libs $(FFMPEG_MODULES) 2>/dev/null)
RETRO_PIC     := $(if $(filter win32,$(PLATFORM)),,-fPIC -fvisibility=hidden)
ifeq ($(PLATFORM),win32)
  # the Windows build draws its text with GDI; the core has no window and takes FreeType (a static one for MinGW, under MINGW_DEPS)
  RETRO_FT_CFLAGS := -DBGI_HAVE_FREETYPE $(shell $(PKG_CONFIG) --cflags freetype2 2>/dev/null)
  RETRO_FT_LIBS   := $(shell $(PKG_CONFIG) --libs --static freetype2 2>/dev/null)
  RETRO_HOST_SRC  := $(patsubst %,src/host/win32/%.c,files native registry process pipeproc wide time thread)
  RETRO_LINK      := -shared -static-libgcc
  RETRO_SYSLIBS   := -lshell32 -ladvapi32 -lole32 -luuid -lbcrypt -lwinmm
else
  RETRO_HOST_SRC  := $(patsubst %,src/host/posix/%.c,files native registry process pipeproc paths time thread)
  # (-Bsymbolic: ffmpeg's assembly code refers to its own data directly)
  # (Android: pages of 16 KB on the devices that have them)
  RETRO_LINK      := -shared -Wl,-soname,$(RETRO_NAME) -Wl,--exclude-libs,ALL -Wl,-Bsymbolic -Wl,--no-undefined \
                     $(if $(filter android,$(PLATFORM)),-Wl$(comma)-z$(comma)max-page-size=16384)
  RETRO_SYSLIBS   := $(if $(filter android,$(PLATFORM)),,-lpthread)
endif
RETRO_HOST_SRC += src/host/os_common.c src/host/os_files.c src/host/os_ffmpeg.c src/host/os_present.c src/host/libretro/core.c
RETRO_CORE_CFLAGS := $(CORE_CFLAGS) $(RETRO_FT_CFLAGS) $(RETRO_PIC) -DOPENBGI_EXPORT=
RETRO_HOST_CFLAGS := $(filter-out -DBGI_HAVE_FONTCONFIG -DBGI_HAVE_ALSA -DBGI_HAVE_FFMPEG -DBGI_LAUNCHER $(FF_CFLAGS),$(CFLAGS)) $(HOST_INC) \
                     -DBGI_HAVE_FFMPEG -DBGI_LIBRETRO $(RETRO_PIC)
RETRO_OBJ     := $(patsubst src/%.c,$(RETRO_DIR)/%.o,$(filter-out src/snd/stb_vorbis.c,$(ENGINE_SRC)) $(RETRO_HOST_SRC) src/movie/ffmovie.c) \
                 $(RETRO_DIR)/gen/embfonts.o $(if $(filter stb,$(VORBIS)),$(RETRO_DIR)/snd/stb_vorbis.o)

libretro: $(RETRO_CORE) $(RETRO_BIN)/openbgi_libretro.info

$(RETRO_BIN)/openbgi_libretro.info: src/host/libretro/openbgi_libretro.info
	@mkdir -p $(dir $@)
	cp $< $@

ifneq ($(RETRO_FFMPEG),$(FFMPEG_BUILT))
$(RETRO_FFMPEG):
	@echo "openbgi: the libretro core carries ffmpeg's libraries, which are not in $(FFMPEG_PREFIX) yet:"
	@echo "  make ffmpeg$(if $(filter-out posix,$(PLATFORM)), PLATFORM=$(PLATFORM))$(if $(filter-out $(DEFAULT_ARCH),$(ARCH)), ARCH=$(ARCH))   fetches ffmpeg's source and builds them, once (docs/dev/building.md, \"Movies\")"
	@exit 1
endif

$(RETRO_DIR)/host/os_present.o: RETRO_HOST_CFLAGS += $(PRESENT_OPT)
$(RETRO_DIR)/host/%.o: src/host/%.c | src/include/bgi/msg.h
	@mkdir -p $(dir $@)
	$(CC) $(RETRO_HOST_CFLAGS) -MMD -MP -c -o $@ $<

$(RETRO_DIR)/movie/ffmovie.o: src/movie/ffmovie.c include/openbgi_ffmovie.h include/openbgi.h $(RETRO_FFMPEG)
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) $(RETRO_PIC) $(RETRO_FF_CFLAGS) -c -o $@ $<

$(RETRO_DIR)/snd/stb_vorbis.o: src/snd/stb_vorbis.c
	@mkdir -p $(dir $@)
	$(CC) $(RETRO_CORE_CFLAGS) -w -c -o $@ $<

$(RETRO_DIR)/gen/embfonts.o: $(OBJDIR)/gen/embfonts.c src/include/bgi/embfonts.h
	@mkdir -p $(dir $@)
	$(CC) $(RETRO_CORE_CFLAGS) -w -c -o $@ $<

$(RETRO_DIR)/%.o: src/%.c | src/include/bgi/msg.h
	@mkdir -p $(dir $@)
	$(CC) $(RETRO_CORE_CFLAGS) $(HOT_OPT) -MMD -MP -c -o $@ $<

$(RETRO_CORE): $(RETRO_OBJ) $(RETRO_FFMPEG) | bin
	@mkdir -p $(dir $@)
	$(CC) $(sort $(filter -m% --sysroot%,$(CFLAGS))) $(RETRO_LINK) -o $@ $(RETRO_OBJ) $(RETRO_FF_LIBS) $(RETRO_FT_LIBS) $(CORE_LIBS) $(RETRO_SYSLIBS)

-include $(RETRO_OBJ:.o=.d)

# make check-libretro: what the core lets out and needs, and - where the
# platform's programs run here - a game played with it by a frontend
# without a window (tools/retrofront.c, built into the objects' directory),
# loaded twice (tools/checkretro.py)
RETROFRONT := $(RETRO_DIR)/retrofront$(EXE)
$(RETROFRONT): tools/retrofront.c src/host/libretro/libretro.h
	@mkdir -p $(dir $@)
	$(CC) $(EXAMPLE_CFLAGS) $(HOST_INC) -o $@ $< $(if $(filter win32,$(PLATFORM)),,-ldl)

check-libretro: libretro $(RETROFRONT)
	@python3 tools/checkretro.py --header=src/host/libretro/libretro.h --core=$(RETRO_CORE) --nm="$(LIB_NM)" --objdump="$(LIB_OBJDUMP)" \
	    --work=$(RETRO_DIR)/check $(if $(filter yes,$(CORE_RUNS)),--run="$(TEST_RUN)") $(RETROFRONT)

# the asset server's protocol, exercised through the browser-side client under node
wasm-test:
	node tools/wasm/selftest.js

# ---- tests ---------------------------------------------------------------------
# the test programs are built into the object directory, obj/<platform>-<arch>/tests/
# (they run from the project's root and write their scratch files under bin/)
# (tests/ffmovie.c is the movie decoder's: not in a build without it, MOVIES=none; tests/dump.c and
# tests/tools.c are the tools': not in one without them, TOOLS=0)
TEST_SRC := $(filter-out tests/wasmfs.c tests/win32wide.c $(if $(MOVIE_SRC),,tests/ffmovie.c) \
              $(if $(TOOLS_SRC),,tests/dump.c tests/tools.c),$(wildcard tests/*.c))
TEST_BIN := $(patsubst tests/%.c,$(OBJDIR)/tests/test_%$(EXE),$(TEST_SRC))
# what runs a test binary: nothing for a native one, wine for the Windows
# builds when it is on the PATH (with a UTF-8 locale, without which wine
# cannot spell the Japanese file names the tests write; quiet; in the
# prefix OPENBGI_WINEPREFIX when that is set); TEST_RUN= runs them directly
ifeq ($(PLATFORM),win32)
  TEST_RUN ?= $(if $(shell command -v wine 2>/dev/null),env LC_ALL=C.UTF-8 WINEDEBUG=-all \
              $(if $(OPENBGI_WINEPREFIX),WINEPREFIX=$(OPENBGI_WINEPREFIX)) wine,)
endif

$(OBJDIR)/tests/test_%$(EXE): tests/%.c $(TOOL_OBJ) | bin
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(HOST_INC) -o $@ $< $(TOOL_OBJ) $(LDLIBS)

# (the table of the blitters' results is a file of its own)
$(OBJDIR)/tests/test_blitgold$(EXE): tests/blitgold.inc

# src/base/ alone: the link fails when one of its files calls for anything of the engine
$(OBJDIR)/tests/test_base$(EXE): tests/base.c $(patsubst src/%.c,$(OBJDIR)/%.o,$(BASE_SRC)) | bin
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ -lm

# the WebAssembly file layer on the host, against the mock server of the test
# (built apart: it defines the OS_File* functions the platform's back end has too)
WASMFS_SRC := tests/wasmfs.c src/host/wasm/fs.c src/host/wasm/host.c src/host/os_common.c src/base/sjis.c \
              tools/wasm/hostmock.c
$(OBJDIR)/tests/test_wasmfs$(EXE): $(WASMFS_SRC) src/host/os_wasm.h | bin
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(HOST_INC) -DBGI_WASM -DBGI_NO_DEBUGGER -I. -Itools/wasmstub -o $@ $(WASMFS_SRC)

# the Win32 back end's string conversions on the host: wide.c against the stub
# windows.h, with the 16-bit wchar_t of Windows, the code pages mocked by the
# test with iconv (built apart for the same reason as the test above)
WIN32WIDE_SRC := tests/win32wide.c src/host/win32/wide.c src/host/os_common.c src/base/sjis.c
$(OBJDIR)/tests/test_win32wide$(EXE): $(WIN32WIDE_SRC) src/host/os_win32.h tools/win32stub/windows.h | bin
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(HOST_INC) -DBGI_WIN32 -fshort-wchar -Itools/win32stub -o $@ $(WIN32WIDE_SRC)

ifeq ($(PLATFORM),posix)
TEST_BIN += $(OBJDIR)/tests/test_wasmfs$(EXE) $(OBJDIR)/tests/test_win32wide$(EXE)
endif

# (with SAN the tests run for the sanitizer's memory errors; they free little of
# their own, and what a run leaves behind is check-core's to find)
test: $(TEST_BIN) $(if $(filter posix headless,$(PLATFORM)),$(if $(TOOLS_SRC),check-launcher))
	@for t in $(TEST_BIN); do echo "  RUN $$t"; $(if $(SAN),env ASAN_OPTIONS=detect_leaks=0) $(TEST_RUN) $$t || exit 1; done

# ---- installing ----------------------------------------------------------------
# make install puts the program of the Linux build where a system keeps its
# programs - under prefix (/usr/local), or under DESTDIR and prefix when a
# package is put together: the program as bin/openbgi (its tools are its
# commands, `openbgi bpc`: no names of their own), the desktop entry, the
# icon's pictures, the description for the software centres, the manual
# page and the documents - README.md and the player's manual, docs/manual/
# (docs/modding/ and docs/dev/ stay with the sources) - and
# LICENSE and NOTICES, which go with whatever is installed, also with
# the library or the core alone.  make install-lib puts the
# library there - shared, with the plain name libopenbgi.so.<major> and
# libopenbgi.so for the linker, static, the movie decoder's, the headers
# and openbgi.pc - and make install-libretro the libretro core with its
# description, where the frontends of a system look for them.  make
# uninstall removes what the three put there.  bindir, libdir (a
# distribution's lib64 or lib/<triplet>), includedir, datadir, mandir and
# docdir can each be said; make install-strip installs without the
# symbols.
prefix      ?= /usr/local
exec_prefix ?= $(prefix)
bindir      ?= $(exec_prefix)/bin
libdir      ?= $(exec_prefix)/lib
includedir  ?= $(prefix)/include
datadir     ?= $(prefix)/share
mandir      ?= $(datadir)/man
docdir      ?= $(datadir)/doc/openbgi
INSTALL     ?= install
STRIP       ?= $(CROSS)strip
INSTALL_BIN ?= $(INSTALL) -m 755
REL_VERSION := $(shell sed -n 's/^\#define OPENBGI_VERSION  *"\(.*\)".*/\1/p' src/include/bgi/release.h)
INST_DIR    := $(OBJDIR)/install
INST_SONAME := libopenbgi.so.$(API_MAJOR)
# the documents that go with the program: the README and the player's manual, at the paths the
# README's links name (docs/modding/ and docs/dev/ stay with the sources)
INST_DOCS   := README.md
INST_MANUAL := $(wildcard docs/manual/*.md)
INST_LEGAL  := LICENSE NOTICES
INST_ICONS  := $(DESTDIR)$(datadir)/icons/hicolor
.PHONY: install install-strip install-lib install-libretro uninstall install-check

install-check:
	@test "$(PLATFORM)" = posix || { echo "make install: the Linux build is what is installed (PLATFORM=posix), not $(PLATFORM)"; exit 1; }

install: install-check all
	$(INSTALL) -d $(DESTDIR)$(bindir) $(DESTDIR)$(datadir)/applications $(DESTDIR)$(datadir)/metainfo $(DESTDIR)$(mandir)/man1 $(DESTDIR)$(docdir)
	$(INSTALL_BIN) $(TARGET) $(DESTDIR)$(bindir)/openbgi
	$(INSTALL) -m 644 icons/openbgi.desktop $(DESTDIR)$(datadir)/applications/openbgi.desktop
	$(INSTALL) -m 644 icons/net.openbgi.OpenBGI.metainfo.xml $(DESTDIR)$(datadir)/metainfo/net.openbgi.OpenBGI.metainfo.xml
	$(INSTALL) -m 644 docs/openbgi.1 $(DESTDIR)$(mandir)/man1/openbgi.1
	$(INSTALL) -m 644 $(INST_DOCS) $(INST_LEGAL) $(DESTDIR)$(docdir)
	$(INSTALL) -d $(DESTDIR)$(docdir)/docs/manual
	$(INSTALL) -m 644 $(INST_MANUAL) $(DESTDIR)$(docdir)/docs/manual
ifneq ($(ICON),)
	@rm -rf $(INST_DIR)/icons && mkdir -p $(INST_DIR)/icons && python3 tools/mkicon.py --png $(ICON) $(INST_DIR)/icons > /dev/null
	@for f in $(INST_DIR)/icons/openbgi-*.png; do s=$${f##*-}; s=$${s%.png}; \
	    $(INSTALL) -d $(INST_ICONS)/$${s}x$${s}/apps && $(INSTALL) -m 644 $$f $(INST_ICONS)/$${s}x$${s}/apps/openbgi.png || exit 1; done
endif
ifeq ($(ICON),icons/openbgi.ico)
	$(INSTALL) -d $(INST_ICONS)/scalable/apps
	$(INSTALL) -m 644 icons/openbgi.svg $(INST_ICONS)/scalable/apps/openbgi.svg
endif

install-strip:
	$(MAKE) install INSTALL_BIN='$(INSTALL) -m 755 -s --strip-program=$(STRIP)'

ifneq ($(LIB_SONAME),)
# the shared library once more under the name a system knows it by
$(INST_DIR)/$(INST_SONAME): $(PIC_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(PIC_CFLAGS) -shared -Wl,-soname,$(INST_SONAME) -Wl,--exclude-libs,ALL -o $@ $(PIC_OBJ) $(CORE_LIBS)
endif

install-lib: install-check $(LIB_STATIC) $(LIB_MOVIE) $(INST_DIR)/$(INST_SONAME)
	$(INSTALL) -d $(DESTDIR)$(libdir)/pkgconfig $(DESTDIR)$(includedir) $(DESTDIR)$(docdir)
	$(INSTALL) -m 644 $(INST_LEGAL) $(DESTDIR)$(docdir)
	$(INSTALL_BIN) $(INST_DIR)/$(INST_SONAME) $(DESTDIR)$(libdir)/$(INST_SONAME)
	ln -sf $(INST_SONAME) $(DESTDIR)$(libdir)/libopenbgi.so
	$(INSTALL) -m 644 $(LIB_STATIC) $(DESTDIR)$(libdir)/libopenbgi.a
	$(if $(LIB_MOVIE),$(INSTALL) -m 644 $(LIB_MOVIE) $(DESTDIR)$(libdir)/libopenbgi-movie.a)
	$(INSTALL) -m 644 include/openbgi.h $(if $(LIB_MOVIE),include/openbgi_ffmovie.h) $(DESTDIR)$(includedir)
	@{ echo 'prefix=$(prefix)'; echo 'libdir=$(libdir)'; echo 'includedir=$(includedir)'; echo; \
	   echo 'Name: OpenBGI'; echo 'Description: The engine of OpenBGI as a library (openbgi.h)'; echo 'URL: https://openbgi.net'; \
	   echo 'Version: $(REL_VERSION)'; echo 'Libs: -L$${libdir} -lopenbgi'; \
	   echo 'Libs.private: $(sort $(filter-out -L%,$(CORE_LIBS)))'; echo 'Cflags: -I$${includedir}'; } > $(DESTDIR)$(libdir)/pkgconfig/openbgi.pc
	@chmod 644 $(DESTDIR)$(libdir)/pkgconfig/openbgi.pc

install-libretro: install-check libretro
	$(INSTALL) -d $(DESTDIR)$(libdir)/libretro $(DESTDIR)$(datadir)/libretro/info $(DESTDIR)$(docdir)
	$(INSTALL) -m 644 $(INST_LEGAL) $(DESTDIR)$(docdir)
	$(INSTALL_BIN) $(RETRO_CORE) $(DESTDIR)$(libdir)/libretro/$(RETRO_NAME)
	$(INSTALL) -m 644 $(RETRO_BIN)/openbgi_libretro.info $(DESTDIR)$(datadir)/libretro/info/openbgi_libretro.info

uninstall: install-check
	rm -f $(DESTDIR)$(bindir)/openbgi $(DESTDIR)$(datadir)/applications/openbgi.desktop \
	    $(DESTDIR)$(datadir)/metainfo/net.openbgi.OpenBGI.metainfo.xml $(DESTDIR)$(mandir)/man1/openbgi.1 \
	    $(INST_ICONS)/*/apps/openbgi.png $(INST_ICONS)/scalable/apps/openbgi.svg \
	    $(DESTDIR)$(libdir)/$(INST_SONAME) $(DESTDIR)$(libdir)/libopenbgi.so $(DESTDIR)$(libdir)/libopenbgi.a \
	    $(DESTDIR)$(libdir)/libopenbgi-movie.a $(DESTDIR)$(libdir)/pkgconfig/openbgi.pc \
	    $(DESTDIR)$(includedir)/openbgi.h $(DESTDIR)$(includedir)/openbgi_ffmovie.h \
	    $(DESTDIR)$(libdir)/libretro/$(RETRO_NAME) $(DESTDIR)$(datadir)/libretro/info/openbgi_libretro.info
	rm -f $(addprefix $(DESTDIR)$(docdir)/,$(notdir $(INST_DOCS) $(INST_LEGAL)))
	rm -f $(addprefix $(DESTDIR)$(docdir)/docs/manual/,$(notdir $(INST_MANUAL)))
	-rmdir $(DESTDIR)$(docdir)/docs/manual $(DESTDIR)$(docdir)/docs $(DESTDIR)$(docdir) 2>/dev/null

clean:
	rm -rf obj bin

-include $(DEP) $(CORE_OBJ:.o=.d) $(PIC_OBJ:.o=.d)
