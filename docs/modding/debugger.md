# The debugger

A window into the running engine: script threads, loaded programs,
bitmaps, display objects, sound, memory, a log, and what a click
reaches. It can pause and step the scripts. In the Linux and Windows
programs; not in the browser build.

## Opening it

| How | Result |
|---|---|
| `openbgi --debug GAME` | Opens with the game |
| `openbgi --debug=pause GAME` | Opens with the scripts held at their first instruction, until F5 |
| F12 in the game's window | Opens and closes it at any time |
| A breakpoint is hit | Opens by itself |

It is a second window beside the game's. `BGI_DEBUG_OVERLAY=1` draws it
over the game's picture instead.

## Keys

| Key | Does |
|---|---|
| F5 | Run / pause |
| F6 | Step one pass of the scheduler (every runnable thread gets a turn) |
| F7 | Step one instruction per runnable thread |
| F9 | Toggle a breakpoint on the selected listing line |
| T | Instruction trace into the log |
| Tab, 1 to 8 | The views |
| Left, Right | Which pane the arrow keys act on |
| Up, Down, PgUp, PgDn, Home, End | Move in the focused pane |
| F | Follow the instruction pointer again |
| Esc, F12 | Close |

The mouse selects rows and tabs, the wheel scrolls. A click in the
margin left of a listing line toggles a breakpoint.

## Views

| View | Shows |
|---|---|
| Threads | Every thread: id, module and offset, frame and stack pointers, state (`runs`, or the wait it blocks on). For the selected thread: its listing, its stack (values decoded as pointers: `code+`, `data+`, `heap+`, `dynN+`, `gmem+`), its frame. |
| Programs | The modules the selected thread loaded (name, base, size) and the listing of one ([programs.md](programs.md), "openbgi asm") |
| Bitmaps | The bitmap slots in use: size, pixel mode, generation, and the picture |
| Objects | The display objects: class, slot, visibility, priority, position, size; the selected one's fields and bitmap |
| Sound | Music channels and effect slots: state, rate, channels, length, position, volume, pan, loops |
| Memory | Hex dumps of the global memory, the system area, and the selected thread's code, data and heap |
| Log | Module loads, message boxes (script errors included), breakpoints, the trace. Kept while the debugger is closed. |
| Targets | What a click reaches now: kind, owner, rectangle, flags; the picture with the targets outlined; the last touch; which input device has the turn |

Target flags: A anywhere, D disabled, C covered, H hover, S selected,
G held, P probed by a script lately.

## Breakpoints and stepping

- A breakpoint is a module name and an offset. It survives a reload of
  the module and applies to every thread that loaded it.
- At a breakpoint the thread stops before the instruction and the
  scripts pause. F5 resumes.
- While paused, the window stays responsive and the picture is kept.
- A thread blocked in a wait does not move when stepping; a wait on a
  timer or a key completes on its own terms.

## Without the window

| Variable | Effect |
|---|---|
| `BGI_TRACE=1` | Instruction trace on standard error; `BGI_TRACE=prog._bp,other._bp` limits it to programs |
| `BGI_TRACE_FROM=ms` | Trace from this time on |
| `BGI_MSGBOX_STDERR=1` | Message boxes print to standard error and take their default button |

On Linux the windows carry the X resource names `openbgi` and
`openbgi-debug`, so window-automation tools can drive them under a
virtual X server.
