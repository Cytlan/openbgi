# Changing the launcher

The launcher is a Buriko program whose sources OpenBGI carries and
compiles at every start. You can edit them.

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
| `strings.inc`, `strings_ja.inc` | The texts, English and Japanese |
| `obgi_launcher.inc` | Names for OpenBGI's own instructions |
| `written.txt` | OpenBGI's note of what it wrote. Leave it alone. |

The language is described in [programs.md](programs.md), the `obgi`
instructions in [obgi.md](obgi.md).

## Editing

1. Change a file in that folder.
2. Start OpenBGI again. The change shows.

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
