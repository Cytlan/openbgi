# The OS layer

`src/host/` is everything between the core and a system: the hosts
(programs that own an engine) and the platform back ends they are built
on. The core never includes a platform header and never calls this
layer; it asks its host through the tables of `OpenBGI_Host_t`
(`include/openbgi.h`), which this layer fills in.

Writing a host of your own without this layer:
[embedding.md](embedding.md).

## Files

| Path | Content |
|---|---|
| `src/host/os.h` | The layer's interface (`OS_*`), used by the hosts, the tools and the tests |
| `src/host/os_host.c` | `OS_Host()`: the host tables an engine is made with; `OS_RunMain`, `OS_RunEngine`: the main loop |
| `src/host/os_common.c` | Platform-independent helpers: the registry text file, the window title |
| `src/host/os_files.c`, `os_window.c`, `os_machine.c`, `os_media.c` | The file, registry, window, debugger-window, machine, shell, CD audio and movie-player tables |
| `src/host/os_ffmpeg.c` | The movie decoder as the host's table |
| `src/host/os_present.c` | The game's picture laid on a canvas of another size (the libretro core) |
| `src/host/os_gl.c`, `os_glhook.c` | The picture shown through OpenGL, and the shader files it can be scaled with ([below](#the-picture-through-opengl)) |
| `src/host/os_preview.c` | The launcher's jobs and first pictures, on two threads ([launcher.md](launcher.md)) |
| `src/host/os_pads.c` | Device reports into the engine's pad state, the same on every system |
| `src/host/posix/` | Linux; also built for Haiku, Android and the test host, and its system files for macOS |
| `src/host/haiku/` | Haiku's sound and pointer services (the only C++ in the tree) |
| `src/host/macos/` | macOS: the application, the window, Metal, the boxes and pickers (the only Objective-C in the tree) |
| `src/host/headless/` | A back end without a display: Android's command line and the test host |
| `src/host/win32/` | Windows |
| `src/host/wasm/` | The browser ([wasm.md](wasm.md)) |
| `src/host/libretro/` | The libretro core ([libretro.md](libretro.md)) |

The Makefile picks the folders from `PLATFORM`. `make check-core`
builds the core without any of them.

## Conventions

- Paths arrive as the engine builds them: Shift-JIS, `\` separators,
  optionally a drive. Functions that mirror a Win32 call keep its
  return convention.
- Virtual-key codes, window messages and dialog ids are those of
  Windows, because the scripts use them.
- The code page 932 table is the engine's own (`src/base/sjis.c`). No
  back end needs iconv or a system code page.
- The layer keeps state of its own (one window, one audio device per
  process) and none of the engine's.

## The main loop

Each platform's `main.c` joins the command line, names the launcher's
packed sources (`OS_UseLauncher`) and calls `OS_RunMain`. One turn of
`OS_RunEngine`:

1. `OS_AnswerRequests`: message boxes, dialogs and pickers the engine
   asked for, each modal. The back end's modal loop calls
   `OpenBGI_RunAudio` every 20 ms so that sound plays on.
2. The pointer's place into `OpenBGI_Pointer`.
3. `OpenBGI_RunFrame`.
4. `OS_ShowFrame`: the rectangles that changed, blitted to the window.
5. `OS_PumpMessages` until the queue is empty; each message's input or
   event is handed to the engine. Then `OpenBGI_PumpEvents`.
6. Pads: `OS_PadPoll`, `OpenBGI_Pad` for those that changed.
7. `OS_IdleSleep`, or `OS_IdleSleepMs` with the time the frame asked
   for.

The sound output's callback reads `OpenBGI_AudioRead` on its own
thread. The engine starts no other program on any system.

## What a back end provides

| Area | Functions | Notes |
|---|---|---|
| Files | `OS_File*`, `OS_Find*`, attributes, copy, move, delete, drives; `OS_Native*` for native paths; `OS_UserDataDir`, `OS_BinaryDir` | The engine goes through `src/fs/fs.c`, which mounts zip archives and disc images over the game's folder in the user's data |
| Time | `OS_TicksMs`, `OS_PerfCounterNs`, `OS_LocalTime`, `OS_SleepMs`, the 0.5 ms idle sleep | The engine reads no clock of its own |
| Threads | Threads with a priority | For the movie decoder's pipes and the preview runs; the engine has none |
| Machine | CPUID and the time stamp counter (x86 only), memory, names, the user's language, the display adapter | |
| Window | Create, show, move, title, icon, display modes, full screen, the picture's size and scaling rule, the cursor, drop acceptance | `OS_WindowScales`, `OS_WindowHasIcons` say what a back end cannot do. `OS_WindowPresent` ends a frame's blits: a back end that draws once a frame draws there. `OS_WindowFilterName` names filters of the back end's own, or none |
| The screen layer | `OS_WindowLayer`; the `surface` and `surfaceTouch` handlers | For a back end whose picture does not fill what it draws on. It says its surface (`OpenBGI_SetSurface`), sends touches in the surface's pixels, and draws the layer of each frame over everything ([input.md](input.md#on-screen-controls)). No back end of this tree says one yet: each does nothing in `OS_WindowLayer` |
| The window's screen | `OS_WindowFits`, `OS_WindowDpi`, `OS_WindowWorkArea`, `OS_WindowSetAspect`; the `screenChanged` handler | With `OS_WindowFits` 1 the engine fits the window to its screen ([below](#fitting-the-window-to-its-screen)) |
| Child windows | `OS_ChildWindows`, or NULL | NULL: the engine draws them in its picture |
| Text entry | `OS_TextEntry`, or NULL | NULL: the engine draws it |
| Dialogs | `OS_MessageBox`, `OS_DialogInput1/2/Profile`, file and folder pickers, the installer's dialogs | `OS_HostCaps` reports `OPENBGI_CAP_DIALOGS` (or `_MESSAGES`: the boxes alone), `_POINTER`, `_KEYBOARD`, `_JOBS` |
| Fonts | `OS_FontFiles` (font files for the engine's FreeType rasteriser) or `OS_FontRenderer` (a renderer of the system's) | Windows and the browser hand over a renderer |
| Pads | `OS_PadPoll`, `OS_PadState`, `OS_PadShutdown` | Up to four ports ([input.md](input.md)) |
| Debugger | `OS_DebugWindowOpen/Close/Present` | X11 and Win32; else the debugger draws over the picture |
| Audio | `OS_AudioStart/Stop`: a pull callback | CD audio (`OS_Mci*`) on Windows only |
| Movies | The decoder table (builds with FFmpeg); on Windows also the system's player | |
| Registry | `OS_RegReadString`, `OS_RegWriteString`, ... | A text file except on Windows |
| Shell | The instance lock, IPC to a running instance, `OS_OpenDocument`, clipboard, wallpaper | No program is started for a game |
| Tools | `OS_NativeList`, `OS_NativeStat`, `OS_RunWorkers`, `OsCommon_PrepFiles` | For `openbgi dump`, `pack`, `bpc`, `scn` |

Input goes to the engine as `OpenBGI_Key` (Windows virtual keys, with
repeat and "system" flags), `OpenBGI_Button`, `OpenBGI_Wheel`,
`OpenBGI_Pointer`, `OpenBGI_Focus`, `OpenBGI_Text` (UTF-8),
`OpenBGI_Touch`, `OpenBGI_TouchScale` - or, from a back end that says
its surface, `OpenBGI_SetSurface` and `OpenBGI_SurfaceTouch`. The
engine keeps the key state itself.

## Fitting the window to its screen

A back end whose `OS_WindowFits` answers 1 (X11, macOS, Win32) takes part in
"the view" (`src/sys/mainwin.c`):

| The back end | The engine |
|---|---|
| Reports the desktop's scale (`OS_WindowDpi`; 96 is none) and the part of the window's screen a window may take (`OS_WindowWorkArea`) | Gives the scripts a desktop, a frame and window positions divided by the scale, and makes the window their client size times the scale |
| | Makes a window that does not fit the work area smaller in proportion and keeps it inside |
| Has a sizing border on every window and reports each size the player gives it (`resized`) | Keeps that size while the scripts ask for the client size they had, and tells them nothing; a game that asked for the border itself ("81 66") is told the size, divided by the scale - also when a size it asked for had to be made smaller, so that it lays its picture out for the room there is |
| Keeps the proportions it is told while the player sizes (`OS_WindowSetAspect`), as far as the system lets it | Names the scripts' client size, or none for a window the game sizes |
| Shows a picture of another size than the client area scaled, by `OPENBGI_SCALE_*` with the `_WHOLE` and `_CHOSEN` flags | Passes the player's filter and steps from the display settings |
| Says when the scale, the work area or the frame becomes another (`screenChanged`) | Fits the window again |

The scripts never learn of the scale. Child windows are drawn in the
picture on such a host, because a window of the host's would be of the
scripts' size and at their place (`--child-windows=host` has the
host's all the same). The back end's own windows follow the scale
themselves (`OsCommon_Dpi`). `BGI_VIEW_TRACE=1` prints what
the scripts asked for and what the host was given.

## The picture through OpenGL

Linux and Windows show the picture through OpenGL when the system has
a graphics processor for it. The engine and its compositor are as
without it: only the last step, the picture scaled into the window,
moves from the processor to the graphics processor.

| File | Content |
|---|---|
| `src/host/os_gl.c` | The presenter. No OpenGL header and no library: the back end hands over how functions are found by name. The picture is a texture that the frame's blits update (`OsGl_Blit`); `OsGl_Draw` draws it once a frame, by the nearest pixel, bilinearly, bicubically (Catmull-Rom) or through a chain of shader files. A layer (`OsGl_Layer`) is a second texture, blended over all of the target after the picture |
| `src/host/os_glhook.c` | Reads shader files in the format of mpv's user shaders, as far as Anime4K uses it, and plans their passes for a picture and a window size: which run, at which size, into which texture. No OpenGL in it; `tests/glhook.c` tests it |
| `src/host/posix/glx.c` | Loads `libGL.so.1` at run time and makes a context for the window's own visual: OpenGL 3.3 core, else what the server makes |
| `src/host/win32/wgl.c` | Loads `opengl32.dll` at run time. Asks once, on a hidden window, whether OpenGL will do, because the window's styles, the text entry and full screen depend on the answer before the game's window exists |
| `shaders/anime4k/` | The Anime4K shader files, unchanged ([NOTICES](../../NOTICES)) |
| `tools/mkpack.py` | Packs `shaders/*/*.glsl` into the program (`gOsShaders`); the entry points hand the pack over with `OS_UseShaders` |

- OpenGL is used from version 3.0 on, and not when software draws it
  (llvmpipe, Windows' own renderer): the back end's scaler is faster
  than that. `BGI_GL=off`, `force` and `old` override
  ([command-line.md](../manual/command-line.md)). One line on stderr
  says what was found.
- A filter of the back end's own is a chain of shader files
  (`kChains` in `os_gl.c`): the six Anime4K modes, each with smaller
  and larger networks (`OPENBGI_SCALE_HIGH`). The passes draw into
  16-bit float textures; the last step scales the result into its
  place bicubically. A chain that does not compile or run is given up
  for the session and the picture is shown bicubically.
- The buffers are swapped without waiting for the display. The engine
  paces its frames itself.
- A context of OpenGL ES 3.0 or later is taken as well; its version
  string says what it is. Differences there:

  | OpenGL | OpenGL ES |
  |---|---|
  | `#version 330 core` (130 on OpenGL 3.0) | `#version 300 es`, with the highest precision for numbers and textures |
  | The picture's pixels are uploaded as they are (`GL_BGRA`) | Uploaded as `GL_RGBA`; the texture is read with red and blue changed over (a swizzle) and its alpha as 1 |
  | 15-bit pictures are uploaded as they are | Made 32-bit first |
  | A software renderer is refused | Taken: there is no other scaler |
  | Shader chains always | Only where the context draws into textures of floats: OpenGL ES 3.2, or `EXT_color_buffer_half_float` / `EXT_color_buffer_float` |

  `tests/gl.c` draws the same pictures both ways on Mesa and compares
  them.
- With OpenGL the Win32 back end has no EDIT control (the engine draws
  the text entry) and switches no display mode: in full screen the
  window covers its monitor without a frame and the mode is scaled
  onto it, as on X11. Without OpenGL both are as the original has
  them.
- `make GL=0` builds without all of it; `make SHADERS=0` leaves the
  shader files out and keeps the three plain filters.

## POSIX (Linux)

| Area | Files | Notes |
|---|---|---|
| Paths, files | `paths.c`, `files.c` | The game directory is reported as `C:\`. On the way back the drive maps to the game directory, `\` becomes `/`, and a component that does not exist as spelled is matched without regard to case. |
| Process, registry | `process.c`, `registry.c` | Settings in `~/.config/bgi/registry.txt` as `key\value=data` lines; the instance lock is a file lock there |
| X11 | `x11.c`, `window.c`, `events.c`, `child.c`, `dbgwin.c` | A top-level window with a sizing border and the picture's proportions as its aspect hints. Full screen uses `_NET_WM_STATE_FULLSCREEN` and scales the picture onto the screen, mapping coordinates back. `BGI_INPUT_DEBUG=1` prints the key and button events |
| The screen | `window.c` | The scale from the `Xft.dpi` line of the root window's resources; the work area from `_NET_WORKAREA`, cut to the monitor the window is on (libXrandr, loaded at run time when the system has it); the frame asked for before the window is shown (`_NET_REQUEST_FRAME_EXTENTS`) |
| Dialogs | `uikit.c`, `msgbox.c`, `dialogs.c` | The engine's own dialog toolkit (`src/ui/`) in windows of their own, their pictures enlarged by the desktop's scale |
| Fonts | `font.c` | fontconfig when built with it, else a search of the usual font folders; `BGI_FONT_GOTHIC`, `BGI_FONT_MINCHO`, `BGI_FONT` name a file |
| Audio | `audio.c` | ALSA, 20 ms periods, 44.1 kHz. Without ALSA a silent thread reads the sound at the same pace. |
| Pads | `pad.c` | The kernel's event devices in `/dev/input`, without a library; `BGI_PAD_DIR` names another folder |

## Haiku

The POSIX back end with `BGI_HAIKU`. The X11 files run on Xlibe,
Haiku's Xlib on its own window system. Differences:

- Settings in `~/config/settings/bgi`, data in
  `~/config/settings/OpenBGI`.
- Sound through the Media Kit (`src/host/haiku/haiku.cpp`).
- Xlibe has no input method: text entry takes characters as typed.
- `X11_PutImage` sets 32 bits per pixel itself, because Xlibe derives
  24 from the depth.
- No pads yet.

## macOS

The POSIX back end's system files with `BGI_MACOS` (files, time,
threads, fonts, sound), and `src/host/macos/` in the place of its X11
files. No X server is involved.

| File | Content |
|---|---|
| `app.m` | The application object and its menu, made when a window or a box first needs one. `OS_PumpMessages` takes one event at a time and hands it to AppKit. What AppKit reports from within other calls (focus, resize, the close box, dropped files) waits in a queue for the pump, so the host's handlers are never entered from inside one of the engine's calls. A timer in AppKit's own loops gives the modal tick. |
| `window.m` | One window with one view. Keys as Windows virtual keys: by what the key types in the layout, else by its place. Option is Alt, Control is Ctrl, Command stays the system's. The input method through `NSTextInputClient` while the engine takes text, its unfinished text in a label at the field. |
| `present.m` | The picture as a Metal texture the blits fill; drawn over the view's layer, scaled by the graphics processor |
| `dialogs.m` | Message boxes as alerts, files and folders in the system's panels |
| `dbgwin.m` | The debugger's window |

How it differs from the X11 back end:

- **Points, not pixels.** Lengths and positions are points with the
  origin at the top left. A window of the picture's size shows it at
  two pixels a pixel on a display that has them.
- **Scaling.** Nearest, bilinear or bicubic (Catmull-Rom) in the
  fragment shader. Where only the display's own factor enlarges the
  picture: bicubic. In a window of another size or full screen: the
  rule of `OS_WindowSetScaling`. `BGI_MAC_FILTER=nearest`, `bilinear`
  or `bicubic` overrides both.
- **Full screen.** The window without its frame over the screen, the
  Dock and the menu bar hidden; no display mode is switched and no
  Space is made. The picture stays below a camera housing.
- **Dialogs.** `OS_HostCaps` reports `OPENBGI_CAP_MESSAGES`, not
  `OPENBGI_CAP_DIALOGS`: the message boxes are the system's, the
  scripts' input dialogs, the text entry and the pop-up windows are
  drawn by the engine in the picture.
- **The icon** is the application's in the Dock.
- **Presenting.** A frame's blits go into the texture; the next pump
  draws once. Nothing is drawn while nobody can see the window.
- Sound through an AudioQueue (AudioToolbox): three 20 ms buffers.
- Settings in `~/Library/Application Support/bgi`, data in
  `~/Library/Application Support/OpenBGI`.
- Fonts from `/System/Library/Fonts`, `/Library/Fonts`,
  `~/Library/Fonts`; FreeType is linked in.
- No pads yet. No game chooser dialog: the launcher chooses.

`make check-macos` syntax-checks all of it against `tools/macstub/`
([building.md](building.md), "macOS").

## Android and the test host

Both build the POSIX back end's system files without X11, with
`src/host/headless/headless.c` in their place: no window table (the
engine keeps a window nobody sees), no input, child windows and text
entry drawn by the engine.

- Android (`BGI_ANDROID`): command-line programs for now; settings and
  data under `$TMPDIR`; no sound output.
- The test host (`BGI_HEADLESS`, `src/host/headless/main.c`): a clock
  of its own, scripted input, pictures and sound written to files
  ([testing.md](testing.md)).

## Windows

`src/host/win32/` is the original's windowing code with the engine's
decisions taken out. `make check-win` checks it without MinGW against
`tools/win32stub/`.

| Area | Files | Notes |
|---|---|---|
| Strings | `wide.c` | No ANSI function is called. Every string is converted from code page 932 (or UTF-8) to UTF-16 by the engine's table, so no Japanese locale is needed. NT line only (Windows XP and later). |
| Window | `window.c`, `wndproc.c`, `child.c`, `edit.c`, `dbgwin.c`, `wgl.c` | The original's classes; the text entry is a subclassed EDIT control. With OpenGL ([above](#the-picture-through-opengl)): no EDIT control, no mode switch |
| The screen | `window.c`, `wndproc.c` | The process declares per-monitor DPI awareness at its start (version 2, else what the system has: the functions are looked for at run time; `BGI_DPI=off` leaves it out). The scale from `GetDpiForWindow`, the work area from the window's monitor, the frame from `AdjustWindowRectExForDpi`. The window always has a sizing border: `WM_SIZING` keeps the proportions, `WM_DPICHANGED` and a changed work area refit, a timer keeps the sound going while the player drags (`modal`) |
| Dialogs | `dlgtemplate.c`, `dialogs.c`, `instdlg.c`, `pickers.c` | Dialog templates assembled in memory from the original's layouts |
| Fonts | `font.c` | GDI: `CreateFont` and `TextOut` into an 8-bit or 1-bit DIB, with the original's parameters |
| Audio | `audio.c` | waveOut, four 20 ms buffers; MCI for CD audio |
| Movies | `../os_media.c` | DirectShow as the system's player; the FFmpeg decoder is preferred |
| Pads | `pad.c` | XInput, then the joystick API for other pads |

A windowed client of another size than the picture ("81 64" of 1.529
on, a screen with a scale, a size the player gave the window) has the
picture scaled into it with black bars, and pointer coordinates mapped
between the two. The EDIT control and the system player's video window
lie on the picture: their place, their size and the control's font
follow the mapping (`Win32Edit_Remap`, `OsWin32_MovieRemap`). Full
screen switches the display's mode, as the original does, and maps
nothing.
