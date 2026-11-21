# Child windows, the text entry and the dialogs ("B0 xx")

Code: `src/sys/child.c`, `src/sys/edit.c`, `src/sys/dialog.c`,
`src/vm/ops_ext0.c` (the handlers); the dialog toolkit `src/ui/`; the
dialogs drawn in the picture `src/sys/inframe.c`. Host side:
`OpenBGI_ChildWindows_t`, `OpenBGI_TextEntry_t` (`include/openbgi.h`).

A host that has windows, a text control or dialogs of its own shows
them as the original did. Every other host, and any host while a pad
has the turn (dialogs) or with `--child-windows=frame` /
`--dialogs=frame`, gets them drawn in the game's picture by the engine.

## Child windows ("B0 10" .. "B0 1C")

Up to eight pop-up windows owned by the main window
(`o->child.children`). A handle is `0xff000000 | slot`. Games use them
for tools, such as a debug window.

| Opcode | Arguments | Does |
|---|---|---|
| `B0 10` | title, x, y, w, h | Create, hidden. Client size 0x20 .. 0x400 each way (else 0x80000001); needs a free slot (else 0x80000002). |
| `B0 11` | | Destroy. The player cannot close a child window. |
| `B0 14` | | Show (without taking the focus) / hide |
| `B0 15`, `B0 16`, `B0 17` | | Title; move; position (pushes x and y) |
| `B0 18` | colour | Fill |
| `B0 19` | x, y, bitmap, effect, level | Draw a bitmap. Codes: 0x80000003 no bitmap, 4 mode, 5 effect, 6 level, 7 no overlap, 8 other. |
| `B0 1A` | x, y, text, font no, size, width %, bold, proportional, colour | Draw text; pushes the widest line's width. Codes: 9 size, 0xa width, 0xb font. |
| `B0 1C` (1.69/472 on) | text | The text that Ctrl+C in the window copies |

Drawing goes into a back buffer, which is then presented. While display
scaling is on, the original blits child windows through the main
window's mapping (offset, stretched, clipped); that quirk is kept.

The engine draws the windows in the picture where the host has none of
its own, with `--child-windows=frame`, and where the host fits the
game's window to its screen ([os_layer.md](os_layer.md)): a window of
the host's would be of the scripts' size and at their place there.

Drawn in the picture (`Child_Frame`), a window has the border and
caption of the engine's dialogs. A press on it makes it the active
window and the main window inactive, as with the original's pop-up.

## The text entry ("B0 20" .. "B0 2A")

In the original: an EDIT control over the main window, subclassed so
that Tab and Enter end the entry, drawn transparently over the picture.

| Build | Behaviour |
|---|---|
| Up to 1.69/451 | One single-line, centred control, created with the main window. Tab and Enter select all and return the focus. |
| From 1.69/472 | Each set-up makes a new control: left-aligned, multi-line when the rectangle is higher than one line. Showing it opens the input method's status window. |
| From 1.529 | Tab hides it |
| From 1.636 | "B0 2A" sets the alignment |

| Opcode | Arguments | Does |
|---|---|---|
| `B0 20` | x, y, w, h, font no, size, max length, focus | Set up and show. The rectangle must lie inside the picture and be at least 8 pixels; size 8 .. 0x40; length 1 .. 0x100. |
| `B0 21` (1.69/472 on) | | Destroy the control |
| `B0 22` (1.529 on) | | The next set-up's glyph width, in percent of half-width |
| `B0 23` | | Whether it is shown |
| `B0 24`, `B0 25`, `B0 26` | | Show / hide; colour; set text |
| `B0 27` | pointer | Get text; pushes the length (-1 without a control) |
| `B0 28`, `B0 29` | | Enter hides it too; single-byte printable characters are refused |
| `B0 2A` (1.636 on) | 0 left, 1 centred, 2 right | Alignment; pushes 1 when taken |

The Windows host keeps the EDIT control (`src/host/win32/edit.c`) and
reports the keys typed into it as taken (`OPENBGI_KEY_TAKEN`). Where
the picture is shown at another size, the control's place, its size and
its font's are the engine's times that. Every
other host gets the engine's entry (`src/sys/edit.c`): text in the
colour of "B0 25" over the picture, a selection, a caret, the same
keys.

## Dialogs ("B0 80" .. "B0 8F") and the wallpaper

| Opcode | Arguments | Does |
|---|---|---|
| `B0 80` | text | Message box with an information icon |
| `B0 81` | text, default | Yes / no; pushes 1 for Yes |
| `B0 82` | text, style | Style 1: OK / Cancel, pushes 1 for OK; else yes / no |
| `B0 84` | | One text field, OK / Cancel; pushes 1 when confirmed and stores the text |
| `B0 85` | | Two labelled fields |
| `B0 8C` (1.616 on) | | List selection: answers "cancelled" (placeholder) |
| `B0 8F` | | The player profile: surname, given name, nickname, pronoun (10 bytes each) and a birthday. Month and day are 0-based list selections. OK is the only button; a field with a single-byte character is rejected. |
| `B0 F0` | path, stretch, tile | Set the desktop wallpaper (Windows only; elsewhere it reports failure) |

Every dialog clears the input states afterwards.

The dialog toolkit (`src/ui/`) lays the boxes out as the original's
resources do. The Win32 back end rebuilds the original's dialog
templates in memory (`src/host/win32/dlgtemplate.c`); the X11 back end
shows the toolkit's dialogs in windows of its own; the engine draws
them over its picture where the host has no dialogs.

The display settings (the settings key, F9) are a box of the toolkit
too, always drawn over the picture (`Inframe_Settings`,
`Ui_BuildDisplaySettings`). They answer no request of the scripts';
their pickers only step (`UiFieldW_t.pick`), and what they say holds
at once (`MainWin_ViewApply`).
