# Embedding and extending the core

The core is a library. A program that brings its own host includes
`include/openbgi.h`, links the library and nothing else of OpenBGI.
The header's comments are the reference; this page is the overview.

## Building the library

    make lib            # bin/<platform>/libopenbgi-<arch>.a and .so.<major> (Windows: .dll)
    make lib-example    # examples/minihost.c built with each
    make check-lib      # exports and a run of the example
    sudo make install-lib

    cc -Iinclude -o myhost myhost.c bin/linux/libopenbgi-x86_64.a -lfreetype -lm
    cc -Iinclude -o myhost myhost.c -Lbin/linux -lopenbgi-x86_64

- The static library needs the core's own libraries on the line:
  FreeType, the maths library, and Vorbis's unless built with
  `VORBIS=stb`. The shared library brings them.
- The shared library exports the functions of `openbgi.h` and nothing
  else.
- After `make install-lib`: `pkg-config --cflags --libs openbgi`.

## The loop

The host owns the loop. The engine runs one frame per call and never
waits inside it.

    OpenBGI_t* o = OpenBGI_New(&host);
    OpenBGI_Frame_t f = {.size = sizeof f};
    OpenBGI_Request_t q = {.size = sizeof q};
    if(OpenBGI_Start(o, cmdLine) == OPENBGI_RUNNING)
    {
        for(;;)
        {
            while(OpenBGI_NextRequest(o, &q))
                OpenBGI_Answer(o, q.id, /* the answer */ 0);
            if(OpenBGI_RunFrame(o, &f) != OPENBGI_RUNNING)
                break;
            /* show the changed parts of f.pixels */
            /* hand over input: OpenBGI_Key, OpenBGI_Button, OpenBGI_Pointer, ... */
            OpenBGI_PumpEvents(o);
            /* wait f.idleMs */
        }
    }
    exitCode = OpenBGI_Stop(o);
    OpenBGI_Delete(o);

| Step | Calls |
|---|---|
| Make and start | `OpenBGI_New`, `OpenBGI_SetLauncher`, `OpenBGI_SetIcon`, `OpenBGI_Start` |
| Questions the engine asks (boxes, dialogs, pickers) | `OpenBGI_NextRequest`, `OpenBGI_Answer` |
| A frame | `OpenBGI_RunFrame` fills `OpenBGI_Frame_t`: the picture, the rectangles that changed, `idleMs`, where the pointer was moved; for a host that said its surface, the layer drawn over everything ([input.md](input.md#on-screen-controls)) |
| Input | `OpenBGI_Key`, `OpenBGI_Text`, `OpenBGI_Button`, `OpenBGI_Wheel`, `OpenBGI_Pointer`, `OpenBGI_PointerPlace`, `OpenBGI_Focus`, `OpenBGI_Touch`, `OpenBGI_TouchScale`, `OpenBGI_SetSurface`, `OpenBGI_SurfaceTouch`, `OpenBGI_Pad`, `OpenBGI_Action` |
| Window events | `OpenBGI_Close`, `OpenBGI_Quit`, `OpenBGI_Minimized`, `OpenBGI_WindowSize`, `OpenBGI_DisplayChanged`, `OpenBGI_ScreenChanged`, `OpenBGI_DropFile`, `OpenBGI_PumpEvents` |
| Sound | `OpenBGI_AudioRead` (from any thread), `OpenBGI_SetAudioLead`, `OpenBGI_RunAudio` ([sound.md](sound.md)) |
| What a click reaches | `OpenBGI_Targets` ([input.md](input.md)) |
| Launcher jobs and previews | `OpenBGI_JobNext`, `_JobRun`, `_JobDone`, `_JobBack`; `OpenBGI_PreviewNext`, `_PreviewIcon`, `_PreviewDone` ([launcher.md](launcher.md)) |
| Stop | `OpenBGI_Stop`, `OpenBGI_Delete` |

A stopped engine holds no memory of its run and can be started again.

## The host

`OpenBGI_Host_t` is what the host provides. Only the clocks are
required; a NULL table means "the host has none" and the engine does
without or draws the thing itself.

| Member | Provides | NULL means |
|---|---|---|
| `ticksMs`, `perfNs`, `localTime` | The clocks | (required) |
| `files` | Opening, reading, listing files | No files |
| `registry` | The game's settings store | None |
| `fontFiles` or `fontRenderer` | Font files for the engine's FreeType rasteriser, or a renderer of the host's | Only fonts built in or handed over |
| `window` | The game's window | A window nobody sees |
| `window->dpi`, `workArea`, `setAspect` | The screen's scale and work area: the engine fits the window to them, lets the player size it, and offers the display settings (F9) | The window is of the size the scripts ask for |
| `window->filterName` | Names of scaling filters of the host's own, beyond nearest, bilinear and bicubic: the display settings list them, and the player's choice comes back in `setScaling` as `OPENBGI_SCALE_OWN + n`, with `OPENBGI_SCALE_HIGH` for the better of two qualities | The three rules alone |
| `OpenBGI_SetSurface` with `window->setScaling` | The settings over a game offer the filter, its quality and the whole steps, without `workArea`. The player's choice comes through `setScaling` as it does for a host with `workArea` | The game's own rule alone |
| `textEntry`, `childWindows` | Controls of the host's own | The engine draws them in the picture |
| `debugWindow` | A second window for the debugger | The debugger draws over the picture |
| `machine`, `shell`, `cdAudio` | Machine facts; clipboard, documents, links; CD audio | An ordinary machine; none |
| `moviePlayer`, `movieDecoder` | A system player; a decoder whose frames the engine shows | Movies are skipped |
| `caps` | `OPENBGI_CAP_DIALOGS` (or `_MESSAGES`: the message boxes alone), `_POINTER`, `_KEYBOARD`, `_JOBS` | The engine draws dialogs, a pointer, an on-screen keyboard; reads folders on its own thread |

`src/host/os_host.c` is the complete host of the `openbgi` program.
`examples/minihost.c` is a whole host in about 600 lines of ISO C: a
clock that moves only between frames, the game read from a zip with
stdio, requests answered, each frame written as a PPM file.

    make lib-example
    bin/linux/minihost-x86_64 --run=20000 --every=2000 game.zip /tmp/shot-

## Versions of the interface

`openbgi.h` has a version of its own, `OPENBGI_API_MAJOR.MINOR`, apart
from OpenBGI's release number. A minor step only adds; a major step
may change. A host states the header it was built with
(`OpenBGI_Host_t.api`); the library reports its own
(`OpenBGI_ApiVersion`). A host works with every library of its major
version whose minor is not older. Structures carry their `size`, so a
host built with an older header keeps working.

| Version | Added |
|---|---|
| 1.0 | OpenBGI 1.0 |
| 1.1 | `OpenBGI_Files_t.nativeRoots`: the folders a folder browser starts from where the folders above them cannot be listed |
| 1.1 | `OpenBGI_SetSurface`, `OpenBGI_SurfaceTouch`, and the layer at the end of `OpenBGI_Frame_t` (`layer`, `layerWidth`, `layerHeight`, `layerPitch`, `layerChanges`, `layerChange`): the engine's on-screen controls, for a host whose picture does not fill what it draws on. A host that calls neither gets no layer |
| 1.1 | `OpenBGI_Machine_t.processMemory`: the bytes of memory the process holds, for the performance display ([testing.md](testing.md#the-performance-display)). A host without it shows no memory there |
| 1.1 | `OpenBGI_Window_t.setFrameLimit`: a host that can hold its frames to its screen's refresh rate has it, and holds them until told otherwise. The row "Frame limiter" of the settings over a game comes through it. A host without it has no such row |
| 1.1 | Not an addition, a change for hosts that hand over a launcher: a game the launcher started is followed by the launcher. The frames go on until the launcher ends or the host calls `OpenBGI_Quit` |

What a newer library adds may reach an older host. The header says
what the host does then: it ignores flags it does not know, answers a
request of a kind it does not know with 0, leaves out a target of a
kind it does not know, and takes an unknown `expose` for the picture.

The interface is 1.0 and is frozen when OpenBGI 1.0 is released. From
then on:

| | What | Promise |
|---|---|---|
| Stable | `include/openbgi.h` and `include/openbgi_ffmovie.h`: their functions, types and constants, and what their comments say they do | A host built for 1.0 keeps working with every newer OpenBGI whose interface is of major version 1 |
| Not stable | Everything else: the sources and the private headers under `src/` (`src/include/bgi/`), the OS layer (`src/host/os.h`), the layout of the tree. Also the descriptions of the installer's dialogs behind `OPENBGI_ASK_INSTALLER`, which only a host built with the sources shows; a host of the library answers that request with 0 | None. Any version may change them |

v0.1 had no interface, so a fork of it changes these internals
directly. v1.0 rebuilt them (all of the engine's state in one object,
the system behind the host's tables), and such a fork may not be
compatible with it. Work that is to outlast a version belongs on the
interface, as a host of its own, or in the tree
([CONTRIBUTING.md](../../CONTRIBUTING.md)).

## What the core guarantees

`make check-core` checks these on every build:

- Every core source includes only `include/`, `src/include/`, ISO C99
  headers, FreeType and Vorbis (`tools/checkcore.py`), and tests no
  platform macro.
- `src/base/` reaches neither the engine's state nor `openbgi.h`.
- The core links with `tests/core.c` and no object of `src/host/`, and
  runs: a host of clocks and a few files in memory boots a program,
  answers its box, restarts the engine repeatedly, and plays a host
  built with shorter and longer structures.

`make check-core SAN=address` runs the same under the address
sanitizer.

## Adding a host or a platform

- A new host on an existing platform (like the libretro core): write
  `src/host/<name>/` on the OS layer's tables ([os_layer.md](os_layer.md)).
- A new platform: implement the `OS_*` functions of `src/host/os.h` in
  `src/host/<platform>/`, add it to the Makefile's `PLATFORM` section,
  and give it a stand-in check if its toolchain is not always at hand.
  Start from `src/host/headless/` (no display) or `src/host/posix/`.

## Adding an instruction of OpenBGI's own

The `EF` family is kept small on purpose. Propose a new instruction on
the issue tracker before writing it.

1. The handler in `src/vm/ops_obgi.c`, and its line in
   `Vm_OptableEFInit`.
2. Its number in the "family EF" row of `src/vm/opset.c`.
3. Its name in `src/asm/opnames.c` and its stack effect in
   `src/asm/opstack.c`.
4. `OBGI_VERSION` one higher.
5. A test, and a section in [../modding/obgi.md](../modding/obgi.md).

## Implementing an original instruction

1. Find which builds have it ([opcodes.md](opcodes.md)) and what the
   games do with it (`openbgi asm -d`, `BGI_TRACE_OPS`).
2. Write the handler in the family's `src/vm/ops_*.c`, with its opcode
   bytes and stack effect on the signature line, and register it with
   the generations it serves: `{op, fn, GEN_from, GEN_to}`.
3. Give it a name in `src/asm/opnames.c` and a stack effect in
   `src/asm/opstack.c`.
4. If its behaviour differs between builds, write one handler per
   variant.
5. Add a unit test where the instruction can be driven without a game
   (`tests/vm.c` and the subsystem's test).
