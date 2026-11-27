# Testing

What checks a change, and how to watch a game without a desktop.

## Checks

| Command | Checks |
|---|---|
| `make test` | The unit tests of `tests/` (one program per area, `obj/<platform>-<arch>/tests/test_*`), and that the launcher's sources compile |
| `make PLATFORM=win32 test` | The same, built with MinGW and run under wine |
| `make check` | Every source compiles with `-pedantic` |
| `make check-core` | The core compiles as strict ISO C99 without any host header, links with `tests/core.c` and runs ([embedding.md](embedding.md)) |
| `make check-lib` | The library exports exactly the functions of `openbgi.h`; `examples/minihost.c` plays a small game with it |
| `make check-libretro` | The core exports exactly libretro's functions and plays a small game in a frontend without a window (`tools/retrofront.c`) |
| `make check-win`, `check-macos`, `check-wasm` | The platform back ends compile against stand-in headers (`tools/win32stub/`, `macstub/`, `wasmstub/`); the macOS one's Objective-C with clang |
| `make test SAN=address` | The unit tests under the address sanitizer |

A change should pass `make test`, `make check`, `make check-core` and
`make check-lib` at least, with no compiler warning.

The unit tests cover: the assembler and the compilers, the codecs
(DSC, CompressedBG, BW sound, BF_Movie), the blitters against
reference pictures, archives, zip and disc images, the file layer,
game detection, fonts and the text engine, the VM's base instructions
and OpenBGI's own, the sound library, input mapping (touch, pad), the
dump and pack tools, the host tables, the Win32 string conversions,
icons, the reader of the shader files, the picture drawn through OpenGL
and through OpenGL ES (compared on Mesa's EGL; skipped where there is
none), the performance display, and the core driven through
`openbgi.h` alone.

## The performance display

`src/sys/perf.c`, `src/include/bgi/perf.h`. The engine measures its own
run and says what it found: as a panel over the picture, and as a line
on standard error every five seconds.

| Turned on by | Where |
|---|---|
| The row "Performance display" of the settings over a game | Hosts that say a surface |
| `"perf": "on"` in the `"launcher"` object of `openbgi.json` | Any host. The row writes it |
| `BGI_PERF=1` in the environment | Any host, for that run |

| Host | Shows |
|---|---|
| One that said a surface | The panel in the screen layer, and the line |
| Any other | The line |

    openbgi: perf: 59.9 fps, 24.1 drawn; frame 16.6 ms, longest 33.4; engine 3.2 ms, longest 12.0, 19 %; memory 182 MB (most 190), bitmaps 64 MB; sound 3 voices, 42 to 58 ms ahead, 0 dropped, 128 starved; disk 1234 KB/s, 14 reads, 3.1 ms

| Figure | Is |
|---|---|
| `fps` | Calls of `OpenBGI_RunFrame` a second: the host's loop |
| `drawn` | Those of them whose picture changed |
| `frame` | From one call's start to the next one's: the mean, and the longest |
| `engine` | From a call's start until the engine has its picture: the mean, the longest, and its share of the time. The screen layer and the host's own drawing come after it and are not in it |
| `memory` | What the process holds, from the host (`OpenBGI_Machine_t.processMemory`), and the most so far. Linux and Android say it. Elsewhere: `not known` |
| `bitmaps` | The pixels of the engine's bitmaps. A part of the memory |
| `voices` | Sounds that play now |
| `ms ahead` | Sound that is mixed and not yet read by the host: the least and the most at a frame's end. Near 0 means the output is about to run dry |
| `dropped` | Sound frames mixed that found no room in the ring |
| `starved` | Sound frames the host asked for and the ring did not have: silence was played. A host that asks for more than it plays (the test host does) shows a large number |
| `disk` | Read from files through the host: the rate, the reads and the time spent in them |

- The panel shows the same figures of the last half second, and two
  graphs: the last 240 frames' periods (green up to a sixtieth of a
  second, yellow up to a thirtieth, red beyond; lines at both), and the
  memory over the last minute.
- The panel is painted twice a second, not with every frame, and only
  that part of the layer is handed over again.
- Its texts are English only.
- An engine that has it off pays one test a frame and one a read.
- A run for a first picture of the launcher's and the settings' own
  engine never have it on.
- The last line of the panel is the engine itself: `OpenBGI 1.0  SIMD
  NEON  threads 1 of 8`. The log has it once, when the display comes on.
- `BGI_PERF=2` adds to the log what had the picture painted again in
  those five seconds, a line each: how often, the object's class and
  priority, the rectangle's size and where it was last. `the whole
  picture` counts the full repaints; `others` what found no room among
  16 lines.
- `tests/perf.c` runs it on clocks the test moves.

## The blitters' fast rows

`src/gfx/blit_rows.c`, `src/gfx/blit_simd.h`. The rows of the commonest
blits exist twice: in word lanes, and four pixels at a time with the
processor's vector instructions. The build decides which the blitters
call. Nothing is asked of the processor at run time.

| Build | Fast rows |
|---|---|
| x86, 64-bit | SSE2 |
| x86, 32-bit | SSE2. None when built with `X86_SSE2=0` ([building.md](building.md)) |
| ARM, 64-bit | NEON |
| ARM, 32-bit | NEON when the compiler has it on (Android's has) |
| The browser, anything else | None |

| Row loop | The blit |
|---|---|
| Alpha copy | A picture with alpha onto the screen: sprites, the message window (effect `0x00`) |
| Blend with alpha | The same at a level: a sprite that fades (`0x01`, `0x20`) |
| Blend without alpha | Two pictures without alpha, mixed (`0x01`, `0xF0`) |
| Cross fade | Two pictures with alpha, mixed onto the screen: one sprite turning into another |
| Add with alpha, add without alpha | Light (`0x02`, `0x21`) |

Every other blit is a word-lane or byte loop as before.

- Both forms give the same bytes. `test_blitrows` compares them over
  40 000 random rows a loop: every length from 0 to 70, any alignment,
  every level.
- `test_blitgold` holds the blitters to the sums the first code gave,
  whichever rows they call.
- `test_blitrows time` prints what a 1920 x 1080 pass of each loop
  takes in both forms.
- `make OPT="-O2 -g -DBGI_NO_SIMD"` builds without the fast rows.
- The performance display names the rows in its last line (`SIMD NEON`),
  after the version and before the threads a frame is painted on.

To add a fast row:

1. Move the row of the blit from `blit.c` into `blit_rows.c` as a
   `Plain_` function. Add its pointer to `BlitRows_t`
   (`src/include/bgi/gfx/bitmap.h`).
2. Write the `Fast_` function with the names of `blit_simd.h`. They are
   the same in SSE2 and NEON, so one function serves both.
3. Say in a comment which values of its arguments it takes. In `blit.c`
   choose the table with `ROWS_FOR(condition)`.
4. Add the loop to `tests/blitrows.c`.

To run the NEON rows without an ARM machine, with a cross compiler and
QEMU's user mode:

```
aarch64-linux-gnu-gcc -std=c99 -O3 -static -Iinclude -Isrc/include -Isrc/gfx \
    -D_POSIX_C_SOURCE=200809L -o test_blitrows_arm64 tests/blitrows.c src/gfx/blit_rows.c
qemu-aarch64 ./test_blitrows_arm64
```

## The test host

`make headless` builds `bin/linux/openbgi-headless-<arch>`: the engine
with no window, on a clock of its own. It runs a game for a set time,
with scripted input, and writes pictures of its frames and a summary
of its sound. A run is deterministic: the clock starts at 0 and moves
only between frames, the date is fixed, and the sound is mixed on the
clock's grid.

    openbgi-headless [options] [--] <the engine's command line>

    openbgi-headless --run=60000 --every=3000 --input=title.txt --shots=pictures GAME
    openbgi-headless --run=60000 --every=3000 --input=title.txt --compare=pictures --shots=new GAME

| Option | Meaning |
|---|---|
| `--run=MS` | How long the game runs on the host's clock (30000) |
| `--step=MS` | How far the clock moves after a frame (1) |
| `--refresh=HZ` | The display's refresh rate (60); 0: no wait for the vertical blank |
| `--every=MS` | A picture every MS (1000; 0: none) |
| `--shots=DIR` | Write the pictures as `DIR/<ms>.png` |
| `--compare=DIR` | Compare pictures and sound with an earlier run; exit code 1 on a difference |
| `--input=FILE` | The player's input, from a script |
| `--wav=FILE` | Write the sound as a WAVE file |
| `--targets` | Also record what a click reaches at each picture |
| `--reload=N` | Run N more times in the same process, on a restarted and on a new engine in turn; each must equal the first |
| `--preview=FILE` | Make the launcher's first picture of the game and nothing else |

The input script has one action per line, `MS ACTION [ARGUMENTS]`.
`FROM-TO/EVERY` repeats an action. `#` starts a comment.

    # the first choice of the title, then the text clicked on
    12000 click 70 35
    20000-60000/1500 click 400 300

| Action | Does |
|---|---|
| `key K`, `down K`, `up K` | A key press (100 ms), or press and release separately |
| `click X Y`, `rclick X Y`, `move X Y` | The mouse, in the picture's pixels |
| `wheel N` | N notches, positive away from the player |
| `resize W H` | A new client size for a resizable window |
| `type TEXT` | Typed text, UTF-8 |
| `tap X Y`, `longpress X Y`, `tap2 X Y` | Touch gestures |
| `swipe X Y X2 Y2 [MS]`, `drag ...`, `scroll2 X Y DY [MS]` | Moving touches |
| `touch ID down\|move\|up\|cancel X Y`, `touchscale PPI` | Single contacts; the screen's density |
| `pad C`, `paddown C`, `padup C` | A pad control: `south`, `east`, `west`, `north`, `l1`, `r1`, `l2`, `r2`, `l3`, `r3`, `select`, `start`, `guide`, `up`, `down`, `left`, `right` |
| `stick left\|right X Y [MS]` | A stick at X Y (-100 .. 100) |
| `action A` | A pad action directly: `confirm`, `cancel`, `skip`, `backlog`, `forward`, `keyboard`, `up`, `down`, `left`, `right` |

With the sanitizer:

    make headless SAN=address
    BGI_DATA=/tmp/data bin/san/openbgi-headless-x86_64 --run=30000 --reload=3 game.zip

The test host speaks English on every machine. Without a game it runs
the launcher, with the `openbgi.json` that `BGI_GAMES` names.

## Watching the scripts

`BGI_TRACE`, `BGI_OPSTAT` and the other switches are listed in
[vm.md](vm.md). The debugger: [../modding/debugger.md](../modding/debugger.md).
`BGI_MSGBOX_STDERR=1` prints message boxes to standard error and takes
their default button, so an unattended run never blocks.
