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
   (`src/include/bgi/bpc.h`). On errors it compiles the built-in sources
   in memory instead.
3. Boots the program with the user's data folder as the game directory
   and the profile 1.669.

`src/sys/launcher.c` carries out the `obgi` instructions the program
calls. `obgi.launch_game` stops the run and starts the engine again in
the same process with the game's command line.

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
| `launcher.preview` | By hand | `"none"`: no first pictures |
| game `name`, `profile`, `options`, `patches`, `removed` | Launcher | What "Edit game" sets |
| game `played`, `seconds`, `picture`, `icon`, `preview` | Engine | Last start, total run time, file names, `"none"` after a failed first picture |
| game `filter`, `filterQuality`, `wholeSteps` | Engine | The display settings (F9) of that game: `"bicubic"`, `"bilinear"`, `"nearest"` or the name of a filter of the host's (`"Anime4K A"`; kept while the host has none of that name, and used again when it has); `"high"`; `"true"`. A game started without the launcher has them in the entry of its folder or archive; the launcher's own are `launcher.filter`, `launcher.filterQuality` and `launcher.wholeSteps` |

A games entry with `"directory"` and no `"product"` is what is kept of
the game in that folder. Where the file is looked for and when it is
written: [versions.md](versions.md).
