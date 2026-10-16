# This file is part of OpenBGI (https://openbgi.net).
# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile - builds OpenBGI
#
#   make                 native Linux build        -> bin/bgi
#   make win             MinGW cross build          -> bin/bgi.exe
#   make wasm            Emscripten build           -> bin/wasm/openbgi.html (+ .js, .wasm; docs/wasm.md)
#   make CC=clang        any other C99 compiler
#   make tools           the command-line tools -> bin/bpasm, bin/bgiscn, bin/bgidump, bin/bgipack
#   make test            build and run the unit tests (tests/)
#   make check           syntax-check every source with -pedantic
#   make check-win       syntax-check src/os/win32 against stub SDK headers (no MinGW needed)
#   make check-wasm      syntax-check src/os/wasm against a stub Emscripten header (no emcc needed)
#   make wasm-test       the asset server's protocol test (node)
#   make wasm-sim        the WebAssembly back end as a native binary (bin/openbgi-wasmsim, no browser)
#   make clean
#
# Layout: inc/ headers, src/ sources, obj/ objects, bin/ binaries.
# The OS layer is src/os/os_common.c plus src/os/posix/*.c, src/os/win32/*.c
# or src/os/wasm/*.c.  Only plain make is used; no autotools / CMake.

CC      ?= gcc
# the optional libraries are looked up with pkg-config; a cross build uses
# its toolchain's own (see the win32 platform below), never the host's
PKG_CONFIG ?= pkg-config
CSTD    := -std=c99
# -Wno-format-truncation / -Wno-format-overflow: the engine formats paths and
# messages into fixed MAX_PATH-sized buffers as the original does; GCC's
# estimates assume unbounded %s arguments (the inputs are archive and file
# names, which are bounded by the formats that produce them)
WARN    := -Wall -Wextra -pedantic -Wno-unused-parameter \
           -Wno-format-truncation -Wno-format-overflow
OPT     ?= -O2 -g
INC     := -Iinc
CFLAGS  += $(CSTD) $(WARN) $(OPT) $(INC) -D_POSIX_C_SOURCE=200809L
LDFLAGS +=

# ---- platform -------------------------------------------------------------
# PLATFORM is "posix", "win32" or "wasm"; "make win" selects the MinGW
# toolchain, "make wasm" Emscripten (emcc on the PATH, e.g. from emsdk).
PLATFORM ?= posix
ifeq ($(PLATFORM),wasm)
  CC      := emcc
  EXE     := .html
  OS_SRC  := $(wildcard src/os/wasm/*.c)
  CFLAGS  += -DBGI_WASM
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
  TARGET  := bin/wasm/openbgi$(EXE)
  # no host libraries: the BW codec 3 (Vorbis) needs the single-file decoder here
  VORBIS  := $(if $(wildcard src/snd/stb_vorbis.c),stb,none)
else ifeq ($(PLATFORM),wasmsim)
  # "make wasm-sim": the WebAssembly back end on the host, with
  # tools/wasm/fakebrowser.c and hostmock.c in place of the browser and the
  # asset server (docs/wasm.md); a native binary that runs a game directory
  # through the back end's own file layer, overlay and presentation
  EXE     :=
  OS_SRC  := $(wildcard src/os/wasm/*.c)
  TOOL_SRC := tools/wasm/hostmock.c tools/wasm/fakebrowser.c
  CFLAGS  += -DBGI_WASM -DBGI_POSIX -I. -Itools/wasmstub
  LDLIBS  += -lm
  TARGET  := bin/openbgi-wasmsim
  VORBIS  := none
else ifeq ($(PLATFORM),win32)
  # CROSS is the toolchain prefix; it must not be given a value (not even an
  # empty one) before this line, or "?=" keeps that and the host's gcc builds
  CROSS   ?= i686-w64-mingw32-
  CC      := $(CROSS)gcc
  PKG_CONFIG := $(CROSS)pkg-config
  EXE     := .exe
  OS_SRC  := $(wildcard src/os/win32/*.c)
  LDLIBS  += -lgdi32 -luser32 -lwinmm -limm32 -lcomctl32 -lole32 -loleaut32 -lshell32 -ladvapi32 -lcomdlg32 -luuid -lstrmiids
  CFLAGS  += -DBGI_WIN32
  LDFLAGS += -mwindows
else
  EXE     :=
  OS_SRC  := $(wildcard src/os/posix/*.c)
  LDLIBS  += -lX11 -lXext -lfreetype -lpthread -lm
  CFLAGS  += -DBGI_POSIX $(shell $(PKG_CONFIG) --cflags freetype2 2>/dev/null)
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
# src/snd/stb_vorbis.c when that file has been dropped in, else the sounds
# of that codec play as silence.  VORBIS=vorbisfile / stb / none force one.
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
  LDLIBS += $(shell $(PKG_CONFIG) --libs vorbisfile 2>/dev/null || echo -lvorbisfile -lvorbis -logg)
endif
ifeq ($(VORBIS),stb)
  CFLAGS += -DBGI_USE_STB_VORBIS
endif

# ---- sources ----------------------------------------------------------------
CORE_SRC := $(wildcard src/core/*.c)
ASM_SRC  := $(wildcard src/asm/*.c)
SCN_SRC  := $(wildcard src/scn/*.c)
BPC_SRC  := $(wildcard src/bpc/*.c)
# the dump / pack tools' module (docs/dump.md) is not part of the WebAssembly
# build: it works on directories of the host
DUMP_SRC := $(if $(filter wasm wasmsim,$(PLATFORM)),,$(wildcard src/dump/*.c))
VM_SRC   := $(wildcard src/vm/*.c)
WAIT_SRC := $(wildcard src/wait/*.c)
SYS_SRC  := $(wildcard src/sys/*.c) $(wildcard src/sys/install/*.c) $(wildcard src/sysobj/*.c)
GFX_SRC  := $(wildcard src/gfx/*.c) $(wildcard src/gfx/background/*.c) $(wildcard src/gfx/mgr/*.c) \
            $(wildcard src/gfx/particle/*.c) $(wildcard src/gfx/window/*.c) $(wildcard src/gfx/text/*.c)
SND_SRC  := $(wildcard src/snd/*.c) $(wildcard src/snd/bw/*.c) $(wildcard src/snd/wm/*.c)
# the debugger (docs/debugger.md) is not part of the WebAssembly build: the
# hooks in the engine compile to nothing there (inc/bgi/dbg.h)
DBG_SRC  := $(if $(filter wasm wasmsim,$(PLATFORM)),,$(wildcard src/dbg/*.c) $(wildcard src/dbg/views/*.c))
COMMON_OS_SRC := src/os/os_common.c

SRC := $(CORE_SRC) $(ASM_SRC) $(SCN_SRC) $(BPC_SRC) $(DUMP_SRC) $(VM_SRC) $(WAIT_SRC) $(SYS_SRC) $(GFX_SRC) $(SND_SRC) $(DBG_SRC) \
       $(COMMON_OS_SRC) $(OS_SRC)
OBJ := $(patsubst src/%.c,obj/$(PLATFORM)/%.o,$(SRC)) $(patsubst tools/%.c,obj/$(PLATFORM)/tools/%.o,$(TOOL_SRC))
DEP := $(OBJ:.o=.d)
LIB_OBJ := $(filter-out obj/$(PLATFORM)/sys/main.o,$(OBJ)) # the engine without its entry point

TARGET ?= bin/bgi$(EXE)

.PHONY: all win wasm wasm-sim tools check check-win check-wasm wasm-test test clean msg

all: $(TARGET)

# ---- the command-line tools (tools/*.c linked with the engine library) ----
TOOLS := bin/bpasm$(EXE) bin/bgiscn$(EXE) bin/bgidump$(EXE) bin/bgipack$(EXE)

tools: $(TOOLS)

bin/bpasm$(EXE): tools/bpasm.c $(LIB_OBJ) | bin
	$(CC) $(CFLAGS) -o $@ $< $(LIB_OBJ) $(LDLIBS)

bin/bgiscn$(EXE): tools/bgiscn.c $(LIB_OBJ) | bin
	$(CC) $(CFLAGS) -o $@ $< $(LIB_OBJ) $(LDLIBS)

bin/bgidump$(EXE): tools/bgidump.c $(LIB_OBJ) | bin
	$(CC) $(CFLAGS) -o $@ $< $(LIB_OBJ) $(LDLIBS)

bin/bgipack$(EXE): tools/bgipack.c $(LIB_OBJ) | bin
	$(CC) $(CFLAGS) -o $@ $< $(LIB_OBJ) $(LDLIBS)

win:
	$(MAKE) PLATFORM=win32

wasm:
	$(MAKE) PLATFORM=wasm

wasm-sim:
	$(MAKE) PLATFORM=wasmsim

$(TARGET): $(OBJ) | bin
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

obj/$(PLATFORM)/%.o: src/%.c | inc/bgi/msg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

obj/$(PLATFORM)/tools/%.o: tools/%.c | inc/bgi/msg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# third-party single-file libraries are compiled without the project's warnings
obj/$(PLATFORM)/snd/stb_vorbis.o: src/snd/stb_vorbis.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(OPT) $(INC) -w -c -o $@ $<

bin:
	@mkdir -p bin

# the Shift-JIS message table is generated from tools/messages.txt
inc/bgi/msg.h: tools/messages.txt tools/mkmsg.py
	python3 tools/mkmsg.py

msg: inc/bgi/msg.h

# syntax check of every translation unit (both platforms share all but src/os)
check:
	@for f in $(SRC); do \
	    echo "  CHECK $$f"; \
	    $(CC) $(CFLAGS) -fsyntax-only $$f || exit 1; \
	done

# syntax check of the Win32 back end without MinGW, against the stub SDK
# headers in tools/win32stub/ (see docs/building.md)
check-win:
	python3 tools/win32stub/gen.py

# syntax check of the WebAssembly back end without emcc, against the stub
# Emscripten header in tools/wasmstub/ (the JavaScript bodies are dropped)
check-wasm:
	@for f in src/os/wasm/*.c; do \
	    echo "  CHECK $$f"; \
	    $(CC) $(CFLAGS) -DBGI_WASM -Itools/wasmstub -fsyntax-only $$f || exit 1; \
	done

# the asset server's protocol, exercised through the browser-side client under node
wasm-test:
	node tools/wasm/selftest.js

# ---- tests ---------------------------------------------------------------------
TEST_SRC := $(filter-out tests/wasmfs.c tests/win32wide.c,$(wildcard tests/*.c))
TEST_BIN := $(patsubst tests/%.c,bin/test_%$(EXE),$(TEST_SRC))

bin/test_%$(EXE): tests/%.c $(LIB_OBJ) | bin
	$(CC) $(CFLAGS) -o $@ $< $(LIB_OBJ) $(LDLIBS)

# the WebAssembly file layer on the host, against the mock server of the test
# (built apart: it defines the OS_File* functions the platform's back end has too)
WASMFS_SRC := tests/wasmfs.c src/os/wasm/fs.c src/os/wasm/sjis.c src/os/wasm/host.c src/os/os_common.c \
              tools/wasm/hostmock.c
bin/test_wasmfs$(EXE): $(WASMFS_SRC) inc/bgi/os_wasm.h | bin
	$(CC) $(CFLAGS) -DBGI_WASM -I. -Itools/wasmstub -o $@ $(WASMFS_SRC)

# the Win32 back end's string conversions on the host: wide.c against the stub
# windows.h, with the 16-bit wchar_t of Windows, the code pages mocked by the
# test with iconv (built apart for the same reason as the test above)
WIN32WIDE_SRC := tests/win32wide.c src/os/win32/wide.c src/os/os_common.c
bin/test_win32wide$(EXE): $(WIN32WIDE_SRC) inc/bgi/os_win32.h tools/win32stub/windows.h | bin
	$(CC) $(CFLAGS) -DBGI_WIN32 -fshort-wchar -Itools/win32stub -o $@ $(WIN32WIDE_SRC)

ifeq ($(PLATFORM),posix)
TEST_BIN += bin/test_wasmfs$(EXE) bin/test_win32wide$(EXE)
endif

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do echo "  RUN $$t"; $$t || exit 1; done

clean:
	rm -rf obj bin

-include $(DEP)
