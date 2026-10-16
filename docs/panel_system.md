# The panel system ("90 B6" .. "90 BF", "91 B6" .. "91 BF", "80 A8" .. "80 AC")

Sources: `inc/bgi/panel.h` (every record and structure, with the layout
of the script's tables field by field), `src/sys/objproc.c` (the
controller base, the registry, the key maps), `src/sys/panel.c`
(ButtonPanel, the "90 B6" painter), `src/sys/sprpanel.c` (SpritePanel);
the handlers are in `src/vm/ops_gfx0.c`, `src/vm/ops_gfx1.c` and
`src/vm/ops_sys.c`.

## ObjProc: controllers hooked into display objects

A display object has a `proc` slot.  "90 B8" / "91 B8" (`SubObj_Create`)
create a controller for the object behind a handle - a ButtonPanel for a
window (kind 0) or a SpritePanel (kind 1) - give it an id from a running
serial (`gProcSerial`) and return that id; the script addresses
the controller by id from then on.  "90 B9" deletes it, "80 A8" / "80 A9"
read and set its enable flag, "80 AC" posts a message of up to 0x100
dwords that the controller processes at its next poll.  The registry walks
every display object of the graphics manager to find the id
(`SubObj_Find`), and `Panel_PollAll` polls every controller once per
scheduler pass (a poll may ask to stop the pass).

A controller that drew something marks itself dirty; a flush (after the
pass) then asks for a present (`gProcPresent`).

## ButtonPanel

A panel sits on a message window.  Its description (`PanelDesc_t`) names
1..256 groups of 1..256 buttons each; the script's tables are copied with
their tagged pointers resolved (`Panel_CopyDesc`; 2 = a bad
group count, 3 = a bad button count) and then once more into the panel on
`Start()`.

Start (`ButtonPanel_Start`) paints every button that has a
bitmap into the window's text layer (item 4 of the window), creates a
size-less virtual child of the window as the panel's own mouse-layer entry
and one virtual child per drawn button carrying its hit mask (the pixel
mask bitmap, the rectangle, or nothing for `hitMaskBmp == -2`), and
installs the input layers: a modal panel pushes its own key and mouse
layers and takes the input focus, a non-modal one with `mouse` set joins
the mouse layer stack.  The initially selected buttons are drawn in their
selected bitmap; the overlays (`ov`) of a group are drawn over window
items 0 / 1 while the group is current, those of a button over items 2 /
3 while it has the focus.

Update (`ButtonPanel_Update`, from the poll) runs once per pass
while the panel is started:

1. `animate` (nothing for ButtonPanel; the sprite panel steps frames and
   swings),
2. the input bits of this pass are read through the panel's layers and
   masked with `inputMask`,
3. the pointer is hit-tested against the virtual buttons (masked); a
   change of the button under the pointer is reported (event 0x10000001)
   and, with `hoverSelects`, selects it; the focus bitmap is swapped in
   (event 0x10000002),
4. hotkeys: a button whose `hotkey` virtual key is pressed (bit 31 also on
   auto-repeat) is decided,
5. the key map turns the standard-key bits into actions (see below):
   decide, cancel, move the selection within the current group's grid of
   `columns` x rows (wrapping, or reporting the edge with event 0x10000005
   for the `_STOP` variants), or change the current group (Tab / Shift+Tab
   by default),
6. a decision (`ButtonPanel_Decide`) records (group, index, flag), selects
   the button when `selectOnDecide`, clears the panel's layers when
   `finishOnDecide` and ends the panel; a cancel records (-1, -1).

Groups sharing a `linkId` are mutually exclusive: selecting in one clears
the others (`ButtonPanel_ApplyLink`).

The script reads the state with "90 BC" (`SubObj_GetCursor`: the decision,
the focus and the current group), "90 BD" (the current group), "90 BE"
(the selection of every group) and drains the event queue with "90 BF"
(`SubObj_PopEvent`, three dwords per event):

| event | a | b |
|-------|---|---|
| 0x10000001 button under the pointer changed | group | index (or -1, -1) |
| 0x10000002 focus changed | group<<16 \| index (or -1) | the button has a focus bitmap |
| 0x10000003 current group changed | group | direction |
| 0x10000004 selection changed | group<<16 \| index | direction |
| 0x10000005 the cursor hit the grid's edge | group<<16 \| index | direction |
| 0x10000006 (sprite panel) a button was decided | group<<16 \| index | flag |
| 0x10000007 (sprite panel, 1.69/472 on) the click position of a mouse decision, queued ahead of its 0x10000006 | group<<16 \| index | y<<16 \| x, relative to the button |
| 0x10000007 (sprite panel, 1.494 on) a click beside every button | 0xffffffff | y<<16 \| x, relative to the window |

The direction is the key's: -1 / 1 previous / next, 0xffff / 1 left /
right, 0xffff0000 / 0x10000 up / down, 0 for the mouse.

The "80 AC" messages a panel understands: 0x10000000 (group, index, flag)
decide a button, 0x10000001 (group) set the current group, 0x10000002
(group, index) select, 0x10000003 (on) enable or disable the input.

## Key maps

A key map assigns one of 18 actions to each of the 24 standard-key
entries of the input layer (left, right, middle, X1, X2, wheel up, wheel
down, Enter, Space, up, down, left, right, the digits 1 .. 9 and 0, Tab).
In every built-in map the left button clicks, the right button cancels
(flag 1), Enter decides the current group's selection and Space cancels
(flag 0); the four cursor keys differ (`ButtonPanel_Update`):

| map | up / down | left / right |
|----:|-----------|--------------|
| 0 | previous / next group | previous / next button |
| 1 | left / right in the grid | up / down in the grid |
| 2 | previous / next button | previous / next group |
| 3 | up / down in the grid | left / right in the grid |

Maps 4..7 are installed by the script with "91 BF" (`Panel_SetKeyMap`);
every entry is an action number: 1 click, 2 cancel, 3 decide, 4 cancel
key, 5 / 6 previous / next button, 7 / 8 previous / next group, 9..12
left / right / up / down in the grid (wrapping), 13..18 the same without
wrapping (the edge is reported instead).  Tab is handled outside the map:
next group, previous with Shift.  Only the lowest input bit set in a pass
produces an action.

## SpritePanel

The sprite panel (`sprpanel.c`, "91 B6" / "91 BA") uses the window's
rotatable sub-sprites as buttons instead of painted pixels: each button is
a sprite at (x + ox, y + oy) rotating about (ox, oy), with an optional
frame animation (`frames` consecutive bitmaps every `frameMs`) and a
"swing" of up to 8 phases (angle, duration) that can run only while the
pointer is on the button and either pause or restart when it leaves.  Its
four bitmaps are normal / focus / selected / selected+focus; `noRedecide`
refuses a second decision of the selected button; `inputDisabled` starts
the panel with its input off.  Decisions are additionally reported as
event 0x10000006, from 1.69/472 preceded by the click position (event
0x10000007) for a mouse decision, and from 1.494 a click beside every
button is reported the same way with -1 for the button (the `clickOutside`
hook) - the system programs of the later builds handle the buttons they
keep disabled, and the tabs of their option screens, through that event.
Everything else - groups, links, key maps, the event queue - is the
ButtonPanel code through the shared vtable (`PanelVtbl_t`).

## Painting without a panel

"90 B6" / "90 B7" (`Panel_Paint` / `SpritePanel_Paint`) draw a
description's buttons into a window once, in their
normal or selected state, without creating a controller - the same code
`Start()` uses for its initial paint.
