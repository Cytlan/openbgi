# The libretro core

OpenBGI as a core for RetroArch and other libretro frontends, on Linux,
Windows and Android.

## Installing

| System | Download |
|---|---|
| Linux | `libretro-openbgi_<version>-1_amd64.deb` (Debian, Ubuntu), `libretro-openbgi-<version>-1.x86_64.rpm` (Fedora), or `openbgi_libretro-<version>-linux-x86_64.zip` |
| Windows | `openbgi_libretro-<version>-windows-x86_64.zip` (or `-x86`) |
| Android | `openbgi_libretro-<version>-android-<abi>.zip`: `arm64-v8a` for most phones |

From a zip:

1. Copy the core (`openbgi_libretro.so`, `.dll` or
   `openbgi_libretro_android.so`) into the frontend's cores folder.
2. Copy `openbgi_libretro.info` into its core-info folder.
   (RetroArch: Settings, Directory, "Cores" and "Core Info".)
3. On Android, RetroArch installs the core with Load Core, Install or
   Restore a Core.

## Loading a game

Use Load Content and pick one of:

| File | What is played |
|---|---|
| `game.zip` | The game in the archive |
| `game.iso` | The game on the disc image |
| The game's `.exe`, its `BGI.gdb`, or one of its `.arc` files | The folder that file is in |

Nothing is written into the game. Saves go to `OpenBGI/games/<name>/`
in the frontend's save folder; OpenBGI's settings (`openbgi.json`) are
in `OpenBGI/` there.

There are no save states: rewind and run-ahead do nothing.

## Patches

Put patch zips into a folder named `patches`:

| The content | Folder for patches |
|---|---|
| A file in the game's folder | `patches/` in that folder |
| `game.zip`, `game.iso` | `patches/game/` beside the file if it exists, else `patches/` beside it |

Choose them in the core options **Patch 1** to **Patch 4**; a later one
wins. A change applies after Restart. Save the options per game
(RetroArch: Quick Menu, Core Options, Manage Core Options, Save Game
Options).

## Fonts

The core has fonts built in. To use your own (`msgothic.ttc`,
`msmincho.ttc`, any `.ttf`, `.ttc`, `.otf`), put them into
`openbgi/fonts/` in the frontend's system folder.

## Input

| Device | Behaviour |
|---|---|
| Keyboard | Keys go to the game. Frontends also bind keys to the pad; turn on the frontend's keyboard focus (RetroArch: Game Focus, Scroll Lock) to type normally. |
| Mouse | Pointer, three buttons, wheel. In full screen, turn on **Draw the pointer for a mouse** to see a pointer. |
| RetroPad, port 1 | B and Start confirm, A cancels, X opens the on-screen keyboard, L backlog, R forward, R2 held skips, D-pad moves the focus, left stick the pointer, right stick the wheel |
| Touch screen | As in [controls.md](controls.md); the default on Android |

## Core options

The first value is the default.

| Group | Option | Values |
|---|---|---|
| Picture | Scaling | powers of two, whole numbers, fill the screen, none |
| | Scaling filter | bicubic, bilinear, nearest |
| | Screen size | automatic, the game's own, 1280x720 ... 3840x2160 |
| Pointer | Pointer device | automatic, mouse, touch screen |
| | Draw the pointer for a mouse | off, on |
| | Size of the drawn pointer | normal, small, large |
| | Stick speed | 100%, 50% ... 300% |
| Pad | Confirm and cancel | B confirms and A cancels, or the reverse |
| | B, A, Y, X, L, R, L2, R2, L3, R3, Select, Start | default, a pad action, a mouse button, the wheel, a key, nothing |
| | The game's own pad keys | automatic, on, off |
| Language and touch | Messages of OpenBGI | English, Japanese |
| | Tap delay | automatic, 0 to 500 ms |
| | On-screen keyboard | automatic, only when asked for |
| Compatibility | Engine profile | automatic, 1.58 ... 1.669 |
| | Disc and registration check | the game's own, skipped |
| | Disc marker file | hidden, shown |
| | Movies | on, skipped |
| Fonts | The player's fonts before the built-in ones | on, off |
| | Design of the built-in fonts | by the font's name, Gothic, Mincho |
| Patches | Patch 1 to Patch 4 | none, the zips of the `patches` folder |

Picture, Pointer and Pad options apply at once. Compatibility, Fonts,
Patches and the message language apply from the game's next start
(Restart).

Scaling: the game's picture is centred on a canvas of your display's
size and enlarged in the chosen steps. With **Screen size** set to "the
game's own", the frontend does all scaling (its integer scaling, its
shaders). On Android that is the default: the frontend scales with the
graphics processor.

## Not supported yet

- Save states
- Content that the frontend can reach only through its own file
  interface (VFS)
- A second pad, rumble, the frontend's on-screen keyboard
- Disc images other than plain `.iso`
