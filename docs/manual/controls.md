# Controls

Keyboard, mouse, game pad and touch screen.

## OpenBGI's own keys

| Key | What it does |
|---|---|
| Alt+Enter | Full screen on and off |
| Alt+F4 | Closes the game |
| F9 | Opens and closes the display settings (Windows, Linux and macOS; [below](#the-window-and-the-display-settings)) |
| F12 | Opens and closes the debugger |

On a Mac:

| Key | What it does |
|---|---|
| Option | The Alt key: Option+Return is full screen on and off |
| Control | The Ctrl key |
| Command+Q | Closes the game, as the window's close button does |
| Control+Command+F | Full screen on and off |
| fn+F12, and fn with the other F keys | The F keys, unless the keyboard settings say that they work without fn |

## The window and the display settings

On Windows, Linux and macOS:

- The window follows the desktop's scale: on a desktop set to 150 %, a
  game of 1280 x 720 gets a window of 1920 x 1080 pixels.
- A window that would not fit beside the task bar, the panels or the
  Dock is made smaller, and the picture is scaled into it.
- Drag the window's border to give it any size. The picture keeps its
  proportions; where the window does not, it has black bars beside it.
- A game that sets another window size itself gets that size.
- The window's size is not remembered: the next start has the size the
  game asks for again.

F9 opens the display settings over the picture. Click a setting to
step through its values, or use the arrow keys; F9, Escape or "Close"
puts them away.

| Setting | Values |
|---|---|
| Window size | Automatic (the game's size at the desktop's scale), 100 %, 150 %, 200 %, Fit the screen |
| Filter | The game's (what the game chooses, else bilinear), Bicubic (sharper), Bilinear (softer), Nearest (hard pixels); with a graphics processor also Anime4K A, B, C, A+A, B+B, C+A ([below](#the-graphics-processor-and-the-anime4k-filters)) |
| Filter quality | Fast or High: the size of the Anime4K filters' networks. Only where those filters are |
| Whole steps only | On: the picture is enlarged 1, 2, 3 ... times and no size between |
| Full screen | As Alt+Enter. The window covers the screen and the picture is scaled onto it; on Windows without a graphics processor the display's mode is switched instead, as the original engine does it |
| Reset window size | Back to the size the game asks for |

The filter, its quality and the whole steps are remembered for each
game, in `openbgi.json` ([files.md](files.md)).

### The graphics processor and the Anime4K filters

On Linux and Windows the picture is scaled to the window by the
graphics processor, through OpenGL 3.0 or later, when the system has
one. Nothing else changes: the game is drawn as before. Without a
graphics processor (or with OpenGL drawn by software) the processor
scales, as before. `BGI_GL=off` keeps the graphics processor out
([command-line.md](command-line.md)).

With the graphics processor the Filter setting also has the six modes
of [Anime4K](https://github.com/bloc97/Anime4K), which enlarge an
anime-style picture with small neural networks and keep its lines
sharp:

| Mode | What it does |
|---|---|
| Anime4K A | Restores lines, then enlarges. The strongest; made for pictures that are blurred or smeared |
| Anime4K B | The same with a softer restoring: fewer false edges on a picture that is only a little blurred |
| Anime4K C | Enlarges and takes noise out; restores least. For a picture that is clean already |
| Anime4K A+A, B+B, C+A | A, B or C with a second restoring pass: stronger, and about twice the work |

- They work where the picture is enlarged. A picture shown at its own
  size or smaller is hardly changed.
- Filter quality: Fast uses the smaller networks and suits a laptop's
  graphics; High uses the larger ones and wants a graphics card.
- A mode that the graphics processor cannot run is replaced by
  Bicubic, and the terminal says why.
- Which mode looks best differs from game to game.

macOS scales with the graphics processor too (Metal), with the three
plain filters.

A game that uses F9 itself: give the settings another key, or none,
with `"settingsKey"` ([below](#changing-what-the-pad-does)).

## In a game

Every other key belongs to the game, and each game explains its own
controls. Most BGI games share these:

| Input | Usual meaning |
|---|---|
| Left click, Enter, Space | Next line; press a button |
| Right click | The game's menu, or back |
| Mouse wheel up | The backlog |
| Ctrl, held | Skip |

## In the launcher

| Key | What it does |
|---|---|
| Arrows | Move through the list |
| Enter | Start the game, open the folder, press the button |
| Escape | Back |
| Tab | Between the list and the buttons |
| Page Up, Page Down, Home, End | Move through the list faster |
| Backspace | The folder above (in the folder browser) |
| F5 | Search for games again |

A double click starts a game or opens a folder.

## Game pad

A pad plays the games through the mouse and keys they already know.

| Control | What it does |
|---|---|
| Bottom face button | Confirm: next line, or click the focused button |
| Right face button, START | Cancel: the right mouse button |
| D-pad | Moves a focus between the buttons on the screen |
| Left stick | Moves the pointer |
| Right stick | The mouse wheel |
| L1, R1 | Wheel up and down: the backlog and back |
| R2, held | Skip |
| Top face button | The on-screen keyboard, in a text field |

- Up to four pads are read; any of them plays. A pad can be plugged in
  while the game runs.
- Message boxes and dialogs are drawn inside the game's picture while a
  pad is in use, so the pad reaches them.
- Some later games give the pad buttons of their own. Those win over
  the table above; `--own-pad-keys=off` turns that off.

| System | Pads that work |
|---|---|
| Windows | XInput pads (Xbox and compatible); other pads through the joystick API |
| Linux | Any pad the kernel knows. If none is found, add your user to the group `input`. |
| RetroArch | The frontend's pad ([libretro.md](libretro.md)) |
| macOS, Haiku | None yet |

## Touch screen

| Gesture | What it does |
|---|---|
| Tap | Left click. A tap close to a button presses the button. |
| Long press, or tap with two fingers | Right click |
| Flick up or down | The mouse wheel: the backlog and back |
| Two fingers moved up or down | The mouse wheel, step by step |
| Drag a slider's knob | Moves the knob |

## On-screen keyboard

Where a game asks for a name, the on-screen keyboard types hiragana,
katakana, letters, digits and signs. It types kana as they are; there
is no conversion to kanji.

| Pad control | In the keyboard |
|---|---|
| D-pad | Moves over the keys |
| Confirm | Types the key |
| Cancel | Deletes |
| L1, R1 | Move the caret |
| R2 | Next table |
| Top face button | Puts the keyboard away |

## Changing what the pad does

Edit `openbgi.json` ([files.md](files.md)). An `"input"` object at the
top applies to every game; one inside a game's entry applies to that
game.

    "input": {
        "pad": { "west": "key:A", "select": "key:F1", "r2": "none" },
        "ownPadKeys": false,
        "stickSpeed": 150
    }

Controls: `south`, `east`, `west`, `north` (the face buttons by
position), `l1`, `r1`, `l2`, `r2`, `l3`, `r3`, `select`, `start`,
`guide`, `up`, `down`, `left`, `right`, and `button0` to `button31` for
a button by number.

| Value | The control then |
|---|---|
| `confirm`, `cancel`, `skip`, `backlog`, `forward`, `keyboard`, `up`, `down`, `left`, `right` | does that pad action |
| `key:NAME` | holds a key: `RETURN`, `SPACE`, `ESCAPE`, `BACK`, `TAB`, `SHIFT`, `CONTROL`, `ALT`, the arrows, `PRIOR`, `NEXT`, `HOME`, `END`, `INSERT`, `DELETE`, `F1` to `F12`, a letter, a digit, or a code (`key:0x41`) |
| `mouse:left`, `mouse:right`, `mouse:middle` | holds a mouse button |
| `wheel:up`, `wheel:down` | turns the wheel one notch |
| `none` | does nothing |

| Setting | Meaning |
|---|---|
| `"ownPadKeys": false` | Ignore the pad keys a game sets itself |
| `"stickSpeed"` | Pointer speed of the left stick in percent, 25 to 400 |
| `"tapDelay"` | Milliseconds between moving the pointer and clicking, 0 to 2000, for games that miss quick clicks |
| `"pointer"` | When OpenBGI draws its own arrow: `"pad"`, `"always"`, `"never"` |
| `"pointerSize"` | `"small"`, `"normal"`, `"large"` |
| `"onScreenKeyboard": false` | The on-screen keyboard appears only when asked for |
| `"settingsKey"` | The key that opens the display settings: `"F9"`, any key name of `key:NAME` above, or `"none"` |
