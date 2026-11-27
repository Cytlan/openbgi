# OpenBGI's own instructions (`obgi`, "EF xx")

Instructions the original engine does not have: UTF-8 text, right-to-left
text, word wrap, scrolling messages, and what a program needs to list
and start games. A game's programs call them when they are rebuilt for
OpenBGI.

- They exist in every engine profile, with the same numbers.
- In a source: `obgi.text_scroll(1, 150);`
  ([programs.md](programs.md)).
- A program that uses one does not run on the original engine, which
  stops at the first `EF` byte.
- Every switch is off until a program turns it on.

| Code | Name | Arguments -> result | Does |
|---|---|---|---|
| `EF 00` | `obgi.version` | -> level | The level of the extensions: 3 |
| `EF 01` | `obgi.text_direction` | mode | Layout of rich text |
| `EF 02` | `obgi.text_encoding` | mode | Encoding of every string: 0 Shift-JIS, 1 UTF-8 |
| `EF 03` | `obgi.text_word_wrap` | on | Break lines between words |
| `EF 08` | `obgi.text_scroll` | on, msPerLine | A message longer than its window scrolls |
| `EF 09` | `obgi.text_scroll_arrows` | bmpUp, upX, upY, bmpDown, downX, downY | Pictures and places of the scroll arrows |
| `EF 0A` | `obgi.text_fit` | h, dst, src, lines, maxBytes, rubyOn, hang -> used | How much of a string fills so many lines |
| `EF 0B` | `obgi.text_width` | str, fontNo, size, widthPct, bold, prop -> width | Width of a string in pixels |
| `EF 0C` | `obgi.text_scroll_pos` | h -> y | How far a window's text is scrolled |
| `EF 20` | `obgi.dir_open` | path, flags -> h | Read a folder of the computer |
| `EF 21` | `obgi.dir_count` | h -> n | Number of entries |
| `EF 22` | `obgi.dir_entry` | h, i, dst, size -> kind | Name of entry i; 1 file, 2 folder, 0 none |
| `EF 23` | `obgi.dir_close` | h | Free the listing |
| `EF 28` | `obgi.game_probe` | path, info -> found | Is this folder, `.zip` or `.iso` a game |
| `EF 29` | `obgi.launch_game` | path, profile, patches, options -> ok | Start that game in place of this program |
| `EF 2C` | `obgi.cfg_get` | path, key, i, dst, size -> len | Read a setting of `openbgi.json` |
| `EF 2D` | `obgi.cfg_set` | path, key, i, value -> ok | Change, add or delete one |
| `EF 30` | `obgi.generate_pattern` | bmp, kind, mode, params, count -> ok | Draw a gray pattern by rule |
| `EF 31` | `obgi.see_through` | on | Give the picture an alpha channel, so that it can lie over another |
| `EF 32` | `obgi.touch_drag` | on | A finger on a touch screen is the left button, held where the finger is |

Arguments are in source order. Check `obgi.version()` before using an
instruction of a later level:

| Level | Adds |
|---|---|
| 1 | `EF 00` to `EF 0C` |
| 2 | `EF 20` to `EF 30` |
| 3 | `EF 31`, `EF 32` |

OpenBGI 1.0 is level 2.

## Text encoding

    obgi.text_encoding(1);      // every string from here on is UTF-8

Put it at the very start of the boot program (`ipl._bp`). Nothing
converts strings: all programs and all scenes of the game must then be
compiled as UTF-8 (`openbgi bpc -u`, `openbgi scn -c -u`, or
`#encoding utf-8` in the source). The engines from 1.653 on have
`81 00` for the same purpose.

In UTF-8 mode, a code from U+FF01 to U+FFFF that has no picture and no
cell in the game's glyph sheet is drawn from the font.

## Text direction

    obgi.text_direction(0);     // the original layout: Japanese, Chinese
    obgi.text_direction(1);     // right to left: Arabic, Hebrew
    obgi.text_direction(2);     // left to right, for every other language

Modes 1 and 2 apply to message windows and to text drawn into bitmaps
(name plates, the backlog):

- A character that is not East Asian is as wide as its font makes it.
- Lines break between words.
- Direction follows the Unicode bidirectional algorithm (without
  explicit embedding controls and bracket pairing).
- Arabic letters are joined, using the Arabic Presentation Forms; the
  font must have them.
- Mode 1 starts lines at the right edge. Mode 2 keeps paragraphs left
  to right; a Hebrew or Arabic phrase inside is still set correctly.

Write strings in reading order, without direction marks. Right-to-left
scripts need UTF-8.

Not in modes 1 and 2: ruby, the Japanese line-breaking rules, the
inline tags of 1.494 and later, vertical windows. Keep Japanese and
Chinese in mode 0.

Fonts: the engine draws with the font the game asks for. For a
character that font lacks, the Linux build takes a glyph from another
installed font; Windows leaves it to GDI. For a release in another
script, ship a font with the game. Japanese fonts draw Cyrillic, Greek
and typographic quotes a full cell wide; use ASCII quotes in English.

Right-to-left text and font fallback have been tried on Linux only.

## Word wrap

    obgi.text_word_wrap(1);

In mode 0, keeps a Latin word together and drops the space a line would
end with. The engines from 1.494 on do this by themselves.

## Scrolling messages

    obgi.text_scroll(1, 150);   // on; a line scrolls in 150 ms

A message longer than its window scrolls instead of being cut off:

- Printed at text speed: the view moves up a line when the next glyph
  lies below it.
- Hurried (a click, instant text): one view is shown; if more follows,
  the down arrow blinks and a click turns the page.
- While waiting for the click: mouse wheel, cursor up/down and the two
  arrows scroll by a line. Scrolling past the end is "go on".
- Skipping: the view jumps to the end.

A message that fits behaves as before. Vertical windows do not scroll.
`msPerLine` of 0 or less keeps the current time (120 at start).

    obgi.text_scroll_arrows(BMP_UP, 600, 4, BMP_DOWN, 600, 70);
    obgi.text_scroll_arrows(-1, -1, -1, -1, -1, -1);    // the built-in ones

Each arrow: a bitmap number (-1: the built-in triangle) and a position
in the window (negative: the default place). The pictures are copied.

`obgi.text_scroll_pos(window)` is 0 at the top, else the scroll offset
in pixels. A program that uses the wheel for the backlog should open it
only at 0.

## Measuring text

    used = obgi.text_fit(hMsgWnd, rec.text, msg, 3, 255, 1, 1);

Lays `msg` out in window `hMsgWnd` and copies what fills 3 lines to
`rec.text` (at most 255 bytes plus terminator). Returns the bytes of
`msg` used. In a loop it cuts a long message into backlog entries.

    x = (204 - obgi.text_width(name, 0, 24, 100, 0, 1)) / 2;

The width of a string on one line: font number, size in pixels, width
in percent, bold, proportional.

## Folders

Paths here are the system's own (`/data/games`, `C:\Games`), in the
encoding of the program's strings.

    h = obgi.dir_open(path, 0);
    n = obgi.dir_count(h);
    for (i = 0; i < n; i = i + 1) {
        kind = obgi.dir_entry(h, i, name, 256);   // 1 a file, 2 a folder
    }
    obgi.dir_close(h);

- `dir_open` returns 0 when the folder cannot be read or eight listings
  are open. Folders come first, then files, sorted by name.
- The path `""` is the computer: its drives; or the folders of the
  setting `"roots"` where the system names some, each under its whole
  path and in the system's order; or the root.

| Flag | Meaning |
|---|---|
| 1 | List hidden entries (names starting with `.` or `$`) too |
| 2 | The program will call `game_probe` on what it finds here |
| 4 | ... and will list the folders below |

Flags 2 and 4 change nothing in the result; they let the engine read
ahead. On the desktop programs `dir_open` and `game_probe` do not block
the engine: the calling thread waits, the program's other threads run
on. Call them from a thread of their own (`sys81.spawn_borrower`) to
keep the program responsive.

## Games

    struct GameInfo {
        char name[128];     // the game's name, "" when unknown
        char profile[16];   // the engine profile detected
        char product[64];   // the product name its programs check for
        int kind;           // 1 folder, 2 zip archive, 3 disc image
    };
    if (obgi.game_probe(path, info)) ...

    obgi.launch_game(path, profile, patches, options);

- `launch_game` ends this program and starts the game in the same
  engine. It returns 0 only when there is no game at the path.
- When the game ends, this program is started again from its
  beginning. Keep what it needs to find again in its settings
  (`cfg_set`), as the launcher keeps the selected game.
- `profile`: `""` for detection. `patches`: strings one after the
  other, ended by an empty string; 0 for none. `options`: more
  command-line options, `""` for none.

## Settings

    len = obgi.cfg_get(path, key, i, dst, size);    // -1: no such value
    ok  = obgi.cfg_set(path, key, i, value);

A key holds a list of string values. `cfg_set` replaces value `i`, adds
one when `i` is the count, deletes it when `value` is 0.

| `path` | Settings of |
|---|---|
| `""` | The program itself (the `"launcher"` object of `openbgi.json`) |
| A game's path | That game: `"name"`, `"profile"`, `"options"`, `"patches"`, `"removed"`, and the engine's `"played"`, `"seconds"`, `"picture"`, `"icon"` |

`"product"`, `"hash"`, `"directory"` and `"input"` cannot be set.

Read-only keys of `""`:

| Key | Value |
|---|---|
| `"profiles"` | The names of the engine profiles |
| `"games"` | The folder of every game the file knows |
| `"home"` | The user's documents folder, or `""` |
| `"separator"` | `/` or `\` |
| `"roots"` | The folders a folder browser starts from, on a system where the folders above them cannot be listed (Android's storage volumes). A list; empty on most systems |
| `"root_names"` | What each of them is called, in the same order |
| `"language"` | `en` or `ja` |
| `"version"` | The version of OpenBGI, as text: `1.0`. No value in an engine before 1.1 |
| `"previews"` | How many first pictures were made since the program began |
| `"mode"` | `settings` when the program runs as the settings over a game ([below](#a-see-through-picture)), else `launcher` |
| `"surface"` | Only in the settings over a game: what the host draws on. Value 0 its width, 1 its height, 2 its pixels an inch |

Special keys of `""`:

| Key | Use |
|---|---|
| `"dropped"` | Paths dropped on the window (after `sys.dragdrop_enable(1)`). Read index 0, then set it to 0 to remove it. |
| `"preview"` | Set to a game's path: the game is in the list; the engine finds its icon and makes a first picture if it has none |
| `"panel"` | Only in the settings over a game: set to `close`, and the settings are put away when the turn is over |

The settings over a game read and set what the player chose there. Each
of these keys of `""` exists only in that mode, and reaches the game's
engine at once:

| Key | Values | Is |
|---|---|---|
| `"pad"` | `on`, `off` | The on-screen pad |
| `"through"` | `on`, `off` | Whether a touch beside the pad reaches the game |
| `"sizes"` | A list of names, read only | The window's sizes, where the host fits a window to its screen: `auto`, `100`, `150`, `200`, `fit`. No value elsewhere, and none in full screen |
| `"size"` | One of them | The window's size |
| `"fullscreen"` | `on`, `off` | Full screen, as Alt+Enter. No value where the host fits no window to a screen |
| `"perf"` | `on`, `off` | The performance display |
| `"limit"` | `on`, `off` | The frames a second are held to the screen's refresh rate. No value where the host cannot hold them: show no row then |
| `"filters"` | A list of names, read only | The filters the picture can be scaled with: `game` (what the game chose), `bicubic`, `bilinear`, `nearest`, then the host's own by their names (`Anime4K A`). No value where the host does not scale the picture |
| `"filter"` | One name of that list | The filter in use |
| `"quality"` | `high`, `normal` | The larger networks of a filter of the host's own |
| `"whole"` | `on`, `off` | The picture only at whole multiples of its size |
| `"game"` | `game`, `launcher` | Which program runs under the settings. Set it to `close`: the settings are put away, and the game is closed as the window's close button closes it |

A value that is none of those listed changes nothing, and `cfg_set`
answers 0. The filter, the quality and the steps are kept for the game
as the display settings box (F9) keeps them.

    while (obgi.cfg_get("", "dropped", 0, path, 1024) > 0) {
        obgi.cfg_set("", "dropped", 0, 0);
        // path is a folder, an archive, or another file
    }

The engine writes the file a second after the last change, and when the
program ends or starts a game.

A game's picture is `pictures\<value of "picture">` in the program's
game folder; load it with `gfx0.bmp_load_picture`.

## A see-through picture

    obgi.see_through(1);                // before the display mode is set
    sys81.set_size_entry(0, w, h);
    sys.set_display_mode(0, 1, 0);

A picture of 32 bits is then made with an alpha channel, and no
background is drawn. What no sprite covers is clear. The settings over
a running game are such a picture: the game's engine lays it over the
game, at the size of what the host draws on, and the game is seen
around the panel.

- A sprite's own alpha is kept: a half clear sprite leaves the picture
  half clear there.
- A host that shows the picture itself shows it as ever. The fourth
  byte of a pixel is none of its colours.
- `obgi.see_through(0)` gives the picture its background back.

| Draws into a see-through picture | Draws nothing |
|---|---|
| Plain sprites with the effects 0 (alpha), 1 and 0x20 (blend), 2 and 0x21 (add), 0x40, 0x80 (copy) | The effects 3 (subtract), 4 (multiply), 6, 8, 9 and 0xC1 (tint) |
| Cross-faded, transformed and wiped sprites (through a copy, so a little slower) | The ripple sprite |
| The dim effects 5 and 0xC0, from a bitmap with alpha | The dim effects from a bitmap without alpha |
| | A background picture |

Text windows, effectors, filters, particles and maps were not tried.
An additive sprite over a clear part comes out weaker than over an
opaque one.

## A list under a finger

    obgi.touch_drag(1);

A touch screen's gestures are then off for this program. A finger is
the left mouse button:

| The finger | The program sees |
|---|---|
| Goes down | The pointer there, and from the next frame the left button held |
| Moves | The pointer with it, from the first pixel |
| Lifts, or is taken away by the system | The button released |

- No tap is snapped to a click target, a long press is no right button,
  and a flick turns no wheel. A second finger does nothing.
- The program tells a tap from a drag itself. A finger lifted where it
  went down tapped. One that moved further than a finger slips drags,
  and a list goes with it.
- `obgi.touch_drag(0)` gives the gestures back. A finger that is down
  goes on as it began.

`settings.inc` of the launcher's program scrolls its rows that way.

## Gray patterns

    gfx0.bmp_create(BMP, w, h, 3);                          // a gray bitmap
    ok = obgi.generate_pattern(BMP, kind, mode, params, count);

Draws into the gray bitmap `BMP`, over what it holds. `params` points
at `count` 32-bit numbers. Returns 0, with nothing drawn, when the
bitmap is not gray or the numbers do not fit the kind.

Use the result as an alpha channel (`gfx1.apply_gray`), a wipe
(`gfx0.sprite_set_wipe`), a blend mask (`gfx0.bg_blend`,
`gfx0.bmp_blit_mask`) or a sprite mask (`gfx0.sprite_set_mask`).

| Mode | Result for pattern value p over existing d |
|---|---|
| 0 | p |
| 1 | max(d, p) |
| 2 | min(d, p) |
| 3 | d + p, at most 255 |
| 4 | d - p, at least 0 |
| 5 | d * p / 255 |

| Kind | Name | params |
|---|---|---|
| 0 | linear | angle, v0, v1, repeat, flags |
| 1 | radial | cx, cy, rx, ry, v0, v1, repeat, flags |
| 2 | sweep | cx, cy, angle, v0, v1, repeat, flags |
| 3 | noise | seed, cellW, cellH, v0, v1, octaves, flags |
| 4 | tiles | shape, w, h, jitter, inner, angle, seed, flags |
| 5 | polygon | value, x0, y0, x1, y1, ... |
| 6 | line | value, width, closed, x0, y0, x1, y1, ... |
| 7 | triangles | x0, y0, v0, x1, y1, v1, x2, y2, v2, ... |

- Angles are 16.16 degrees: 0 right, 90 down. Values are gray levels 0
  to 255. Places are pixels, except the points of kinds 5 to 7, which
  are 1/256 pixel.
- linear, radial, sweep: a ramp from `v0` to `v1`, run `repeat` times.
- noise: grain of cellW x cellH pixels in 1 to 6 octaves; the same
  seed gives the same picture.
- tiles: cuts the existing picture into tiles; each takes the value at
  its centre, plus up to `jitter`, plus a ramp of `inner` levels.
  `mode` is not used.
- polygon (at least 3 points) and line have smooth edges; at most 4096
  points. triangles interpolates the corner values, edges not
  smoothed; at most 900 triangles.

| Flag | Applies to | Meaning |
|---|---|---|
| 1 | ramps | Mirror: up and down within each run |
| 2 | ramps, noise | Smooth |
| 4 | ramps | Dither |
| 8 | noise | The grain runs around the centre (rays); cellW in degrees |
| 16 | tiles | Each tile's ramp has a direction of its own |

| Tile shape | |
|---|---|
| 0 | Rectangles |
| 1, 2 | Columns w wide; rows h high |
| 3 | Bricks |
| 4 | Triangles |
| 5 | Diamonds |
| 6 | Hexagons w wide |
| 7 | Crystal cells |

    // a wipe through panes of glass
    int p[8];
    gfx0.bmp_create(BMP_WIPE, 1280, 720, 3);
    p = {30 * 65536, 40, 215, 1, 0};
    obgi.generate_pattern(BMP_WIPE, 0, 0, p, 5);
    p = {4, 128, 128, 18, 80, 0, seed, 16};
    obgi.generate_pattern(BMP_WIPE, 4, 0, p, 8);
    gfx0.sprite_set_wipe(h, 0, 0, BMP_PICTURE, BMP_WIPE, 2, 0, 1, 0, prio);

`launcher/paint.inc` in the source tree wraps the instruction for a
`.bpc` program.
