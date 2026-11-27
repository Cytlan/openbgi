# Input: touch screens and pads

How a touch screen and a pad play games made for a mouse and a
keyboard. What a player sees: [../manual/controls.md](../manual/controls.md).

## The mapper

`src/sys/inputmap.c`, `touch.c`, `pad.c`, `osk.c`, `steps.c`,
`overlay.c`. It is part of the core, so every host has it.

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

## Clicks

What a gesture or a control does goes through one queue
(`src/sys/steps.c`), a step a pass: the pointer moves, a pass, the
press, a pass, the release.

A pass can be much shorter than a turn of a program's own loop. A
program that reads the buttons itself ("80 11") once a turn would miss
a press and a release that both fall between two readings. So:

- A step that changes the left or right button waits until the program
  has read what the step before did to that button.
- Only a program that read the button within 250 ms before that step
  is waited for, and for 250 ms at most.
- A program that counts presses, or has the engine watch its click
  targets, loses nothing and is not waited for.

`"tapDelay"` is still there for a game that needs more time between
the move and the click.

A program can ask for the finger itself in the gestures' place
(`obgi.touch_drag`, `Touch_SetDrag`; mode `HOLD` of `touch.c`): the
pointer where a finger goes down, the left button held until it lifts,
the pointer with it between. The settings over a game scroll their rows
under a finger that way ([obgi.md](../modding/obgi.md#a-list-under-a-finger)).

## On-screen controls

`src/sys/overlay.c`, `src/include/bgi/overlay.h`. For a host whose
picture does not fill what it draws on: a phone, with bars beside the
picture.

1. The host says its surface with `OpenBGI_SetSurface`: its size, where
   the picture lies on it, the part no cutout covers, its pixels an
   inch. It says it again whenever one of these changes.
2. The host sends its touches with `OpenBGI_SurfaceTouch`, in the
   surface's pixels, in the place of `OpenBGI_Touch`.
3. Each frame carries a layer of the surface's size
   (`OpenBGI_Frame_t.layer`): four bytes a pixel, blue, green, red,
   alpha, the colours not multiplied by the alpha. The host draws it
   over the scaled picture and the bars, a pixel a pixel.
   `layerChanges` says whether it changed, and where.

| Control | Size | Where | As a pad |
|---|---|---|---|
| Pad button | 7 mm | Top right corner | - |
| Settings button | 7 mm | Top left corner | - |
| D-pad | 24 mm | Bottom left | The D-pad |
| Stick | 20 mm | Over the D-pad | The left stick |
| Confirm | 13 mm | Bottom right | Bottom face button |
| Cancel | 10 mm | Over Confirm | Right face button |
| Skip | 8 mm | Over Cancel | R2 |
| Forward | 8 mm | Over Skip | R1 |
| Backlog | 8 mm | Over Forward | L1 |

- The places are in the table `kPlace` of `overlay.c`, in tenths of a
  millimetre from the edges of the surface's free part. The right
  column is 16 mm wide, so it lies in the bar beside a 16:9 picture on
  a 20:9 screen. The D-pad and the stick reach into such a picture.
- A surface too low for the pad at full size has it smaller, down to
  half. The two buttons at the top keep their size.
- The pad is a pad to the mapper. Its state goes to a port of the
  engine's own (`Pad_Port`, `PAD_PORT_OWN`), after the host's four.
  The bindings of `"input"` and a game's own pad keys hold for it. It
  does not count as a pad that is plugged in: until one of its controls
  is used, nothing changes on its account.
- A contact that goes down on a control belongs to the control until it
  lifts. Any other is a touch on the picture; the engine finds the
  picture's pixel from the surface's `picture` rectangle.
- The gestures' distances are physical without `OpenBGI_TouchScale`:
  the engine works the picture's pixels an inch out of the surface.
- The pad button is always shown. It shows and hides the pad, and its
  sign is filled while the pad is shown.
- The settings button is always shown. It brings the settings up over
  the game and puts them away ([launcher.md](launcher.md#the-settings-over-a-game)).
  While they are up, the touches beside the controls and the pad are
  theirs.
- The performance display's panel lies in the layer too, while it is
  on: to the right of the settings button and of the pad's left hand,
  under the settings' picture and the controls. It takes no touch
  ([testing.md](testing.md#the-performance-display)).
- The pointer the engine draws for the stick is 4.5 mm high at least
  where the density is known (`ArrowHeight`, `pad.c`); `"pointerSize"`
  scales it.

| Setting (`"launcher"` object of `openbgi.json`) | Meaning |
|---|---|
| `"touchPad": "on"` | The pad is shown. Absent: off. The pad button writes it |
| `"touchThrough": "off"` | While the pad is on, a touch beside the controls does nothing. Absent: it reaches the picture. The settings over a game write it |

The settings are read when a run's programs start, so a game the
launcher started and the launcher after it show the pad as it was left.
`tests/overlay.c` tests the module without a host.

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

Up to four pads are read and count as one, the on-screen pad with them. New pads are looked for
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
