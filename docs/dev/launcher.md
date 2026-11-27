# The launcher: the engine's side

What the engine and a host do for the launcher. Using it:
[../manual/games.md](../manual/games.md). Changing its program:
[../modding/launcher.md](../modding/launcher.md).

## In the build

- The launcher's sources are `launcher/` in the tree. `tools/mkpack.py`
  packs them into a table the desktop programs link.
- A host hands the pack to the core with `OpenBGI_SetLauncher`
  (`include/openbgi.h`). Linux, Windows, macOS, Haiku and the test host
  do. The libretro core, the library, the browser and Android builds do
  not.
- `make LAUNCHER=0` builds a desktop program without it.
- Without the pack, or with `--launcher=dialog`, a start without a game
  opens the system's folder dialog (`src/sys/gamechooser.c`).
- `make check-launcher` (part of `make test`) compiles the tree's
  sources with the build's own compiler.

## At a start without a game

`src/sys/launcher_src.c`:

1. Writes each source that is missing into `launcher/` of the user's
   data folder. `written.txt` there holds a checksum of every file
   written and of the set they came from, to tell a changed file from
   an untouched one.
2. Compiles `launcher.bpc` from that folder with `Bpc_CompileText`
   (`src/include/bgi/bpc.h`), unless the program kept from an earlier
   compile still stands (below). On errors it compiles the built-in
   sources in memory instead.
3. Boots the program with the user's data folder as the game directory
   and the profile 1.669.

`src/sys/launcher.c` carries out the `obgi` instructions the program
calls. `obgi.launch_game` stops the run and starts the engine again in
the same process with the game's command line.

## Back to the launcher

`src/sys/launcher.c`, `Launcher_Return`; the frame in
`src/sys/engine.c`.

A game the launcher started does not end the run of frames. When its
run is over and its last box is answered, the engine stops it and
starts again as the launcher was started first: the same command
line, in the directory that start began in.

| The run | Ends by | Then |
|---|---|---|
| A game the launcher started | The game itself; the window's close box, as the game handles it; an error | The launcher |
| The same | The host's `OpenBGI_Quit` (a signal, the activity gone) | `OPENBGI_DONE` |
| A game named on the command line | Anything | `OPENBGI_DONE` |
| The launcher | Its program ends; its window is closed | `OPENBGI_DONE` |

- A host needs to do nothing: `OpenBGI_RunFrame` keeps answering
  `OPENBGI_RUNNING` across the change, as it does when a game is
  started.
- The game's play time and last picture are written before the
  launcher starts, so its list shows them.
- The launcher's program starts from its beginning. It finds the game
  it started through its setting `"last"`.
- The processor's clock is measured for a second at an engine's first
  start only (`Cpu_Detect`). A game started from the launcher, and the
  launcher after it, start without that second.
- `tests/launcher.c`, group "back", runs the launcher, a game and the
  launcher again through the frames.

## The settings over a game

`src/sys/settings.c`, `src/include/bgi/settings.h`. The settings are a
screen of the launcher's program (`launcher/settings.inc`). Over a
running game a second engine, the panel, runs it. The game goes on
under it.

| Piece | How |
|---|---|
| The panel's engine | Made by the game's engine (`Settings_Open`) and freed by it (`Settings_Close`). It never outlives the game's run |
| Its host | The game's clocks and fonts, and nothing else. A files table that holds no file. The OS layer keeps one text encoding and one game directory for the process, and the game has both |
| Its program | The launcher's as the game's engine keeps it since the launcher last ran (`Launcher_Program`); for a game named on the command line the pack's own sources, compiled then |
| Its start | `Engine_StartBuiltin`: no command line, no settings file, no lock, no sound, the launcher's profile |
| Its frames | One with each frame of the game's engine (`Settings_Frame`), on the same thread |
| Its picture | ARGB32 at the surface's size (`obgi.see_through`). `Overlay_Frame` copies what changed of it into the screen layer and draws the on-screen controls over it |
| Its input | Touches beside the controls, the on-screen pad, and the host's keys and buttons, while it is up. The game gets none of them |
| What it changes | Settings of the path `""`: `Settings_Value`, `Settings_Set` ([obgi.md](../modding/obgi.md)) |

| Key | Reaches |
|---|---|
| `"pad"`, `"through"` | The on-screen controls: `Overlay_SetPad`, `Overlay_SetThrough` |
| `"perf"` | The performance display: `Perf_Set` ([testing.md](testing.md#the-performance-display)) |
| `"limit"` | The frames held to the screen's rate: `Perf_SetLimit`, which tells the host (`OpenBGI_Window_t.setFrameLimit`) and keeps `"frames": "free"` in the settings file. No value where the host has no such function |
| `"filters"`, `"filter"`, `"quality"`, `"whole"` | The display settings of `mainwin.c`: `MainWin_ViewSettings`, `MainWin_ViewApply`, `MainWin_ViewSetHigh`. They are on where `MainWin_ViewScales` says so: the host fits a window to its screen (`workArea`), or it said a surface and scales the picture (`setScaling`) |
| `"game"` set to `close` | `OpenBGI_Close` of the game's engine, after the panel is gone (`closeGame`). The game asks what it asks at the window's close button. When it ends, the launcher follows |
| `"panel"` set to `close` | `Settings_Close` when the panel's frame is over |

- A host that said no surface has no layer, so no panel. No host of
  this tree says one yet; the desktop keeps the display settings box
  (F9).
- The engine says on standard error how long the panel took to appear:
  `openbgi: the settings were up after 47 ms`.
- A request for the host from the panel (it should make none) is
  logged and answered with 0.
- `tests/settings.c` runs a small panel program over an engine without
  a host's window.

## The compiled program is kept

Compiling the launcher takes about half a second on a desktop. The
compiled program is kept in the folder as `compiled.cache`, with a note
of what it was made from:

| Noted | A start compiles again when |
|---|---|
| Every file the compile read, with a checksum of its bytes | One of them has other bytes, or is gone |
| Every file the compile looked for and did not find | One of them is there now |
| The engine: its release, the launcher's profile and the pack's `build.id` | Another build of OpenBGI runs |

- `build.id` is a file of the pack that is no source. `tools/mkpack.py
  --build-id` makes it: a checksum of everything the engine is built
  from (`BUILD_ID_SRC` in the Makefile). A host with a pack of its own
  may leave it out; the release alone tells builds apart then.
- `build.id` is not written into the folder and does not make a pack
  "newer".
- The built-in sources, which run while the folder's do not compile,
  are kept the same way as `builtin.cache`.
- Without a data folder, or with one that cannot be written to,
  nothing is kept and every start compiles.
- A damaged cache file is not used: a checksum closes it.
- `o->launcher.cached` says whether this start's program was taken
  from the cache (`tests/launcher.c`).

## Jobs: reading without blocking

`obgi.dir_open` and `obgi.game_probe` can take seconds on a slow disk.
A host with `OPENBGI_CAP_JOBS` runs them beside the frames:

1. The instruction makes a job and its thread waits, as for a file
   load.
2. The host takes jobs with `OpenBGI_JobNext`, runs each with a second
   engine on a thread of its own (`OpenBGI_JobRun`) and hands it back
   (`OpenBGI_JobDone`).
3. A listing opened with the flags `DIR_GAMES` / `DIR_DEEPER` queues
   the probes and listings the program will ask for next. Answers never
   asked for are dropped at the next such listing.

When a game starts or the launcher ends, pending jobs are abandoned
without waiting for the drive. A host without the capability reads on
the engine's thread.

## First pictures and icons

A game that was never played has no picture. For each game the
launcher names with the `"preview"` setting, the host may run it out of
sight (`OpenBGI_PreviewNext`, `OpenBGI_PreviewIcon`,
`OpenBGI_PreviewDone`; desktop hosts: `src/host/os_preview.c`):

- Ctrl is held and the picture's corner clicked four times a second
  until the picture has three things to click (a title menu), or
  fifteen seconds of the game's time have passed.
- The picture is sampled once a second and taken when three samples in
  a row are equal, after five seconds when two are, after eight as it
  is. A picture of one colour is never taken.
- The game's clock is counted, not measured: ten to twenty game seconds
  take about one real second. Nothing is written; the game's files are
  only read. Message boxes get their default button; movies are
  skipped.
- A run that showed nothing leaves `"preview": "none"` in the game's
  entry.

The same run looks for the icon of the game's own program among the
`.exe` files of its folder, skipping set-up and uninstall programs
(names containing "setup", "install", "unins", "bhvc", "update",
"patch", "config", "redist"), and stores it as `icons/<name>.ico`.

**The window's icon.** While a game runs, its window has the game's
own icon where one was found, on the systems that take a window icon:
through the window manager on Linux and Haiku, in the title bar and
the task bar on Windows, as the application's icon in the Dock on
macOS. A game without one, and the launcher
itself, have OpenBGI's icon. A game that was not started by the
launcher gets the icon found in its folder at that moment.

While a game the launcher started runs, the engine keeps a small copy
of its picture now and then and writes the last one to `pictures/` when
the game ends.

## The folder browser's top

The listing of the path `""` is what is above every folder:

| Host | Listing of `""` |
|---|---|
| Windows | The drives (`logicalDrives`) |
| One that names roots (`OpenBGI_Files_t.nativeRoots`; a host on Android, with its storage volumes) | Those folders, each under its whole path, in the host's order. The program reads their names as the setting `"root_names"` and takes each to have `""` above it |
| Any other | The root folder |

Drives and roots are known at once; no job is made for them.

## openbgi.json

    {
        "launcher": {
            "folders": ["/data/games", "/data/discs/tayutama.iso"],
            "last": "/data/games/Tayutama",
            "sort": "recent",
            "look": "waves",
            "width": "1600", "height": "900"
        },
        "games": [
            { "directory": "/data/games/Tayutama", "name": "Tayutama",
              "profile": "1.69/444", "options": "--no-disc-check",
              "patches": ["/data/patches/english.zip"],
              "played": "2026-11-18 21:30", "seconds": "5400",
              "picture": "0f3c2a9e51d07b44.png",
              "icon": "0f3c2a9e51d07b44.ico" }
        ]
    }

| Key | Written by | Meaning |
|---|---|---|
| `launcher.folders` | Launcher | The places searched |
| `launcher.last`, `sort`, `look`, `width`, `height` | Launcher | Its own state |
| `launcher.touchPad`, `touchThrough` | The on-screen controls | Whether the pad is shown, and whether touches beside it reach the picture ([input.md](input.md#on-screen-controls)) |
| `launcher.preview` | By hand | `"none"`: no first pictures |
| game `name`, `profile`, `options`, `patches`, `removed` | Launcher | What "Edit game" sets |
| game `played`, `seconds`, `picture`, `icon`, `preview` | Engine | Last start, total run time, file names, `"none"` after a failed first picture |
| game `filter`, `filterQuality`, `wholeSteps` | Engine | The display settings (F9) of that game: `"bicubic"`, `"bilinear"`, `"nearest"` or the name of a filter of the host's (`"Anime4K A"`; kept while the host has none of that name, and used again when it has); `"high"`; `"true"`. A game started without the launcher has them in the entry of its folder or archive; the launcher's own are `launcher.filter`, `launcher.filterQuality` and `launcher.wholeSteps` |

A games entry with `"directory"` and no `"product"` is what is kept of
the game in that folder. Where the file is looked for and when it is
written: [versions.md](versions.md).
