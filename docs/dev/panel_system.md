# The panel system

Buttons on message windows: "90 B6" .. "90 BF", "91 B6" .. "91 BF",
"80 A8" .. "80 AC". Nearly every menu of a game is one.

Code: `src/include/bgi/panel.h` (every record, field by field),
`src/sys/objproc.c` (the controller base, the registry, key maps),
`src/sys/panel.c` (ButtonPanel), `src/sys/sprpanel.c` (SpritePanel).

## Controllers

A display object has a `proc` slot for a controller.

| Opcode | Does |
|---|---|
| `90 B8`, `91 B8` | Create a controller for the object behind a handle: a ButtonPanel for a window (kind 0) or a SpritePanel (kind 1). Returns an id. |
| `90 B9` | Delete it |
| `80 A8`, `80 A9` | Read, set its enable flag |
| `80 AC` | Post a message of up to 0x100 dwords, processed at the next poll |
| `90 BC` | Read the decision, the focus and the current group |
| `90 BD`, `90 BE` | The current group; the selection of every group |
| `90 BF` | Pop an event (three dwords) |
| `91 BF` | Install key map 4 .. 7 |
| `90 B6`, `90 B7` | Paint a description's buttons once, without a controller |

`Panel_PollAll` polls every controller once per scheduler pass.

## ButtonPanel

A description (`PanelDesc_t`) names 1 .. 256 groups of 1 .. 256 buttons.
The script's tables are copied with their pointers resolved.

On start the panel:

- paints every button that has a bitmap into the window's text layer;
- creates one virtual child of the window per button, carrying its hit
  mask (a pixel mask bitmap, the rectangle, or nothing);
- installs input layers: a modal panel pushes its own key and mouse
  layers and takes the focus.

Each pass, while started:

1. Read this pass's input bits through the panel's layers.
2. Hit-test the pointer against the buttons. Report a change (event
   0x10000001); with `hoverSelects`, select the button.
3. Hotkeys: a button whose `hotkey` is pressed is decided.
4. The key map turns standard keys into actions: decide, cancel, move
   the selection in the group's grid, change the group.
5. A decision records (group, index, flag) and, with `finishOnDecide`,
   ends the panel. A cancel records (-1, -1).

Groups that share a `linkId` are mutually exclusive.

From 1.69 build 472 a pointer move on a `hoverSelects` button reports a
selection (0x10000004) only when the selection changed; before, on
every move. The scripts play their hover sound on that event.

## Events

| Event | a | b |
|---|---|---|
| 0x10000001 Button under the pointer changed | group | index (or -1, -1) |
| 0x10000002 Focus changed | group << 16 \| index (or -1) | The button has a focus bitmap |
| 0x10000003 Current group changed | group | direction |
| 0x10000004 Selection changed | group << 16 \| index | direction |
| 0x10000005 The cursor hit the grid's edge | group << 16 \| index | direction |
| 0x10000006 (sprite panel) A button was decided | group << 16 \| index | flag |
| 0x10000007 (sprite panel, 1.69/472 on) Click position of a mouse decision | group << 16 \| index | y << 16 \| x, relative to the button |
| 0x10000007 (sprite panel, 1.494 on) A click beside every button | 0xffffffff | y << 16 \| x, relative to the window |

Direction: -1 / 1 previous / next; 0xffff / 1 left / right;
0xffff0000 / 0x10000 up / down; 0 for the mouse.

## Messages ("80 AC")

| Message | Arguments | Does |
|---|---|---|
| 0x10000000 | group, index, flag | Decide a button |
| 0x10000001 | group | Set the current group |
| 0x10000002 | group, index | Select |
| 0x10000003 | on | Enable or disable input |

## Key maps

A key map gives one of 18 actions to each of 24 standard keys (mouse
buttons, wheel, Enter, Space, the cursor keys, digits, Tab). In the
built-in maps the left button clicks, the right button cancels, Enter
decides and Space cancels. The cursor keys differ:

| Map | Up / down | Left / right |
|---|---|---|
| 0 | Previous / next group | Previous / next button |
| 1 | Left / right in the grid | Up / down in the grid |
| 2 | Previous / next button | Previous / next group |
| 3 | Up / down in the grid | Left / right in the grid |

Actions for maps 4 .. 7: 1 click, 2 cancel, 3 decide, 4 cancel key,
5 / 6 previous / next button, 7 / 8 previous / next group, 9 .. 12
left / right / up / down in the grid (wrapping), 13 .. 18 the same
without wrapping (the edge is reported). Tab is outside the map: next
group, previous with Shift. Only the lowest input bit set in a pass
produces an action.

## SpritePanel

Uses the window's rotatable sub-sprites as buttons:

- A button is a sprite with four bitmaps (normal, focus, selected,
  selected + focus), an optional frame animation and a "swing" of up to
  8 phases (angle, duration).
- `noRedecide` refuses a second decision of the selected button;
  `inputDisabled` starts the panel with input off.
- Decisions are also reported as event 0x10000006.

Groups, links, key maps and the event queue are the ButtonPanel code,
through a shared vtable (`PanelVtbl_t`).

`BGI_PANEL_DEBUG=1` prints the buttons of every panel as it starts and
the events it queues.
