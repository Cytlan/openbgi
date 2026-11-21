# Input: touch screens and pads

How a touch screen and a pad play games made for a mouse and a
keyboard. What a player sees: [../manual/controls.md](../manual/controls.md).

## The mapper

`src/sys/inputmap.c`, `touch.c`, `pad.c`, `osk.c`, `steps.c`. It is
part of the core, so every host has it.

- It knows what a click reaches at any moment: buttons, menu entries,
  sliders, text waiting for a click (`OpenBGI_Targets`,
  `include/openbgi.h`).
- It turns gestures and pad controls into the mouse moves, clicks,
  wheel notches and keys the game expects. The game's scripts read the
  mouse as before.
- A host only hands over what its devices report: `OpenBGI_Touch`,
  `OpenBGI_Pad`, `OpenBGI_Action`, and `OpenBGI_TouchScale` when it
  knows the screen's density.

Whichever device the player used last has the turn. The pad's focus
goes away when another device is used.

## Host capabilities

| Capability | When absent, the engine |
|---|---|
| `OPENBGI_CAP_POINTER` | Draws its own arrow while a pad points (and for a mouse with `"pointer": "always"`) |
| `OPENBGI_CAP_KEYBOARD` | Brings up its on-screen keyboard when a text field is touched |
| `OPENBGI_CAP_DIALOGS` | Draws message boxes and dialogs in the picture |
| `OPENBGI_CAP_MESSAGES` (in the place of `_DIALOGS`) | Draws the dialogs in the picture; the host shows the message boxes (macOS) |

While a pad has the turn, the engine draws boxes, dialogs and the text
entry in the picture on every host, since a pad does not reach the
system's windows.

## Devices per host

| Host | Pads | Touch |
|---|---|---|
| Linux | `src/host/posix/pad.c`: the kernel's event devices in `/dev/input` (every `event*` with a gamepad's buttons, or a joystick's buttons and a stick). `BGI_PAD_DIR` names another folder; empty turns pads off. | As the mouse the X server makes of it |
| Windows | `src/host/win32/pad.c`: XInput (`xinput1_4.dll`, then `1_3`, then `9_1_0`); other pads through `joyGetPosEx`, buttons by DirectInput number | As the mouse Windows makes of it |
| Browser | `src/host/wasm/pad.c`: the Gamepad API | Touch events |
| libretro | The frontend's RetroPad of port 1 | The frontend's pointer device |
| Haiku, macOS, Android | None yet | - |

Up to four pads are read and count as one. New pads are looked for
every two seconds. `src/host/os_pads.c` translates what the back ends
read, the same on every system (`tests/hostpads.c`).

## A game's own pad keys

From 1.547 a game can give a pad's buttons and directions keys of its
own ("81 1B"), as the original did with a DirectInput pad. Such a key
is tapped when the control goes down; only controls the game left
alone follow the mapper. `"ownPadKeys": false` or `--own-pad-keys=off`
ignores the game's keys.

## Settings

The `"input"` object of `openbgi.json` is described in the manual. A
host may lay an object of its own over the file's
(`OpenBGI_InputSettings`); what it puts under `"defaults"` lies under
the file's instead. The libretro core passes its core options this way.

`"settingsKey"` names the key of the display settings (F9), which the
engine takes before the game sees it on a host that fits the window to
its screen ([os_layer.md](os_layer.md)); `"none"` leaves every key to
the game.

`"hotspots"` name places the engine cannot find because a script reads
the mouse itself (a map, a picture with places to choose):

    "hotspots": [ { "name": "door", "rect": [100, 200, 80, 60], "program": "map" } ]

Each is a rectangle in the game's picture and becomes a target like a
button. They count only while the screen has no clickable button of
its own, and with `"program"` only while that program is loaded.

## Trying it without devices

The test host takes touch and pad input from its script
([testing.md](testing.md)). The debugger's Targets view shows what a
click reaches, which device has the turn and where the pad's focus is.
