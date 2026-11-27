# Changing the launcher

The launcher is a Buriko program whose sources OpenBGI carries and
compiles itself. You can edit them.

## Where the sources are

At a start without a game, OpenBGI writes every source that is not
there yet into `launcher/` in your data folder and compiles
`launcher.bpc` from there:

| System | Folder |
|---|---|
| Windows | `%APPDATA%\OpenBGI\launcher\` |
| Linux | `~/.local/share/openbgi/launcher/` |
| macOS | `~/Library/Application Support/OpenBGI/launcher/` |
| Haiku | `~/config/settings/OpenBGI/launcher/` |

| File | Content |
|---|---|
| `launcher.bpc` | The program: screens, lists, layout, input, settings, the folder browser |
| `paint.inc` | Drawing with gray patterns |
| `stage.inc` | The moving background, its colour schemes, the wipes |
| `settings.inc` | The settings over a running game: the program's second way to run |
| `strings.inc`, `strings_ja.inc` | The texts, English and Japanese |
| `obgi_launcher.inc` | Names for OpenBGI's own instructions |
| `written.txt` | OpenBGI's note of what it wrote. Leave it alone. |
| `compiled.cache`, `builtin.cache` | The compiled program, kept so that a start does not compile again. Made anew whenever a source changes. They can be deleted. |

The language is described in [programs.md](programs.md), the `obgi`
instructions in [obgi.md](obgi.md).

## Editing

1. Change a file in that folder.
2. Start OpenBGI again. It compiles the sources again and the change
   shows.

- If the files do not compile, a box names file, line and error (the
  first five; all of them on standard error), and OpenBGI runs its
  built-in launcher until the mistake is fixed.
- To get a file back, delete it. Delete the folder to get all of them
  back.
- To check by hand: `openbgi bpc launcher.bpc -o launcher._bp`.

## Updates

A newer OpenBGI may carry newer sources.

| Your sources | A newer OpenBGI |
|---|---|
| Unchanged | Replaces them silently |
| Changed | Asks: keep or replace. "Replace" keeps your files in `backup-<date>-<time>`. "Keep" is not asked again until the built-in sources change. |

## Adding a language

1. Copy `strings_ja.inc` to a new file and translate the entries.
2. Include it in `launcher.bpc` and call its function in `init` when
   the engine's language matches (`obgi.cfg_get("", "language", ...)`).

OpenBGI's own messages exist in English and Japanese only.

## The settings over a game

The same program is the settings screen that comes up over a running
game (the settings button of the on-screen controls). The game's
engine runs it in a second engine, where `obgi.cfg_get("", "mode", ...)`
reads `settings`; `run()` in `launcher.bpc` then calls `settings_run()`
of `settings.inc` and never reaches the launcher's own screens.

| In that mode | How |
|---|---|
| The picture's size | `"surface"`: the width, the height and the pixels an inch of what the host draws on. Read every turn: a turned phone changes them |
| A see-through picture | `obgi.see_through(1)` before the display mode is set |
| Sizes | `gScale` is the density over 160, so a row is a finger high on any screen |
| What a row changes | Settings of the path `""` that reach the game's engine: `"filter"`, `"quality"`, `"whole"`, `"pad"`, `"through"`, `"perf"`, `"game"` ([obgi.md](obgi.md#settings)) |
| Which rows there are | The three of the picture only where `"filters"` has a value. `Close the game` only where `"game"` reads `game` |
| The list under a finger | `obgi.touch_drag(1)`: a finger is the left button held. `settings_input` tells a tap from a drag, moves the list with the finger, and lets it run on after a flick |
| Closing | `obgi.cfg_set("", "panel", 0, "close")` |
| No files | The second engine has none. Do not load pictures there or read the launcher's own settings: `cfg_get` of an ordinary key finds nothing |

An edit to `settings.inc` shows the next time the launcher itself has
started: the game's engine uses the program the launcher last ran with.

To add a row:

1. Give it a number (`ROW_...`) and a text (`TXT_SET_...` in
   `strings.inc` and `strings_ja.inc`).
2. Put it into the list in `settings_read`, and name it in
   `settings_label`.
3. A switch: say whether it is on in `settings_on`, and change it in
   `settings_switch`.
4. Anything else: draw its value in `settings_list`, and say what a tap
   and the keys do to it in `settings_activate`.

## How it draws

The launcher has no picture files. It uses the same instructions as a
game:

| Element | Made with |
|---|---|
| Every piece on the screen | A sprite |
| Shapes (boxes, rings, ribbons, glows) | `obgi.generate_pattern` as the alpha of a filled bitmap (`gfx1.apply_gray`) |
| Motion | The engine's tweens (`gfx0.obj_move`, `gfx0.obj_tween_full`), one thread per piece (`sys81.spawn_borrower`) |
| Wipes | `gfx0.sprite_set_wipe` through a generated pattern |
| A game's picture | `gfx0.bmp_load_picture`, `gfx0.half_size`, `gfx0.fit_bicubic` |
| Text | `gfx2.text` in the built-in font |
| Window size | `sys81.set_resizable`, `sys81.get_window_size` |
| Dropped files | `obgi.cfg_get("", "dropped", ...)` after `sys.dragdrop_enable(1)` |

Frame rates are `WAVES_FPS` and `SHAPES_FPS` in `stage.inc`.
