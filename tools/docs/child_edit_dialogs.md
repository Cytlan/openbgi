# Child windows, the text entry and the dialogs ("B0 xx")

Sources: `src/sys/child.c`, `src/sys/edit.c`, `src/sys/dialog.c`,
`src/vm/ops_ext0.c` (the handlers), `inc/bgi/display.h`, `inc/bgi/edit.h`,
`inc/bgi/dialog.h`; the host side in `inc/bgi/os.h` (`OS_Child*`,
`OS_Edit*`, `OS_Dialog*`), implemented by `src/os/posix/child.c`,
`edit.c`, `field.c`, `msgbox.c`, `dialogs.c` and `src/os/win32/child.c`,
`edit.c`, `dialogs.c`, `dlgtemplate.c`.

## Child windows ("B0 10" .. "B0 1A")

Up to eight pop-up windows owned by the main window (class "BGI - Child
window", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX), kept in
the table `gChildren` of `src/sys/child.c` (slot: window, a "closing"
flag, a back buffer).  A handle is `0xff000000 | slot`; `Child_Find` only
checks the slot number, not that the slot is in use.

* "B0 10" create (title, x, y, w, h): the client size must be 0x20 .. 0x400
  in both directions (0x80000001), a free slot must exist (0x80000002);
  the window is created hidden at (x, y) in screen coordinates with the
  frame size added to the client size, and its back buffer cleared to
  black.
* "B0 11" destroy: sets the closing flag and sends WM_CLOSE; the window
  procedure refuses a WM_CLOSE while the flag is clear (the user cannot
  close a child), WM_DESTROY frees the slot.
* "B0 12" show / hide, "B0 13" move (x, y - outer origin; the size stays),
  "B0 14" position, "B0 15" title.
* "B0 16" fill (colour), "B0 17" draw bitmap (x, y, bitmap, effect, level:
  codes 0x80000003 no bitmap, 4 mode, 5 effect, 6 level, 7 no overlap, 8
  other), "B0 18" draw text (x, y, text, font no, size, width %, bold,
  proportional, colour, optional measure pointer: codes 9 size, 0xa width,
  0xb font).  Drawing goes into the back buffer, which is then presented
  with a `GetDC` / `BitBlt` through the display scaling (`Window_DcBlit`
  in `src/sys/present.c`; the child blit is clipped to the main picture's
  rectangle, a quirk of the original that is kept).
* Key presses on a child are posted to the main window; WM_PAINT presents
  the back buffer.

## The text entry ("B0 20" .. "B0 27")

A single-line EDIT control (ES_CENTER | ES_AUTOHSCROLL | ES_NOHIDESEL)
created hidden and 1 x 1 together with the main window with the text
MSG_EDIT_DEFAULT_TEXT ("標準の文字列", "default string"), subclassed
so that Tab and Enter select the whole text and return the focus to the
main window, and its WM_PAINT first re-presents the picture under the
control (the `edit_paint` event, `Present_EditPaint` in
`src/sys/present.c`) because the control draws transparently
(WM_CTLCOLOREDIT returns NULL_BRUSH with the colour of "B0 25").

* "B0 20" set-up (x, y, w, h, font no, size, max length, focus): the
  rectangle must lie inside the picture and be at least 8 pixels
  (MSG_BAD_EDIT_SIZE), the font number must exist (MSG_BAD_FONT_NO), the
  size be 8 .. 0x40 (MSG_BAD_FONT_SIZE), the length 1 .. 0x100
  (MSG_BAD_EDIT_LENGTH); then the control is shown, moved, given a
  `CreateFontA(size, size / 2, .., FW_THIN, .., SHIFTJIS_CHARSET, ..,
  FIXED_PITCH)` font, limited, filled with the current text, selected, and
  focused when asked.
* "B0 24" show / hide, "B0 25" colour, "B0 26" set text, "B0 27" get text:
  pushes the length GetWindowText returned, with the text copied to the
  pointer argument.

The X11 back end draws the control itself (see `docs/os_layer.md`).

## Dialogs ("B0 80" .. "B0 8F") and the wallpaper ("B0 F0")

* "B0 80" message box (text) with the caption MSG_NOTICE and an
  information icon; "B0 81" yes / no question (text, default) pushing 1
  for Yes (MB_DEFBUTTON2 when the default is No); "B0 82" (text, style):
  style 1 is OK / Cancel pushing 1 for OK, anything else the yes / no box.
* "B0 84" dialog 0x71: a caption (MSG_DLG_COMMENT_INPUT when empty), one
  edit field (0x3f9, limit, initial text with the caret at its end), OK /
  Cancel; pushes 1 when confirmed and stores the text.
* "B0 85" dialog 0x72: two labelled fields (labels MSG_DLG_STRING1 /
  STRING2 when empty, caption MSG_DLG_DATA_INPUT); the focus starts on the
  first field without an initial text.
* "B0 8F" dialog 0x77, the player profile (caption
  MSG_DLG_PROFILE_TITLE): surname, given name, nickname, first-person
  pronoun (10 bytes each, EM_LIMITTEXT 10) and a birthday as two combo
  boxes (months 1..12, days 1..31 cut to the month's days when the month
  changes, 29 for February).  The month and day values the script passes
  and receives are the 0-based list selections.  OK is the only button;
  its handler rejects any field containing a single-byte character with
  MSG_DLG_HALFWIDTH (caption MSG_DLG_INPUT_ERROR) and keeps the dialog
  open, and the focus starts on the OK button.
* Every dialog clears the input states afterwards (`Input_ClearStates`).
* "B0 F0" wallpaper (path, stretch, tile): writes `WallpaperStyle` and
  `TileWallpaper` under `HKCU\control panel\desktop` and calls
  `SystemParametersInfo(SPI_SETDESKWALLPAPER)`; the POSIX back end
  reports failure.

The dialog resource numbers (0x71, 0x72, 0x77) are those of the original
executable; the Win32 back end rebuilds the templates in memory
(`src/os/win32/dlgtemplate.c`) and the POSIX back end draws them with its
own toolkit (`src/os/posix/uikit.c`).
