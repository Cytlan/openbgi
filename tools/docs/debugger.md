# The debugger

`src/dbg/` is a window into the running engine: the script threads with
their stacks and frames and the disassembly around their instruction
pointers, the loaded programs as listings, the bitmap slots with their
pictures, the display objects, the sound channels, the memory areas and a
log of what happened, live while a game runs, with the VM pausable and
steppable.  Nothing of it exists in the original; nothing of the engine's
behaviour changes while it is closed (the hooks are a flag test each,
`inc/bgi/dbg.h`).  It is part of the Linux and Windows builds; the
WebAssembly build leaves it out (the hooks compile to nothing there).

## Opening it

* `bgi --debug` opens it with the game; `--debug=pause` holds the
  threads until F5, so the boot can be stepped from its first
  instruction.
* F12 in the game's window opens and closes it at any time (F12 never
  reaches the scripts).
* It opens by itself when a breakpoint is hit.

Where the OS layer has a second window (`OS_DebugWindowOpen`: X11 and
Win32), the debugger is a 1152 x 768 window of its own next to the game's;
it is placed to the right of the game's window when the screen has room,
else to its left.  `BGI_DEBUG_OVERLAY=1` makes it draw over the game's
picture instead, sized to the game's window, taking the game's keys and
mouse while open - the path a back end without a second window would use.

## Keys

    F5              run / pause
    F6              step one pass of the scheduler (every runnable thread
                    gets its turn, then the frame services run)
    F7              step one instruction (per runnable thread)
    F9              toggle a breakpoint on the selected listing line
    T               instruction trace into the log (every instruction of
                    every thread: module+offset and the opcode bytes)
    Tab, 1 .. 7     the views
    Left / Right    which pane of a two-pane view the arrow keys act on
    Up / Down, PgUp / PgDn, Home / End     move in the focused pane
    F               back to following the instruction pointer (after
                    scrolling a listing away from it)
    Esc, F12        close

The mouse selects rows and tabs, the wheel scrolls the pane under it, and
a click in the margin of a listing line (the 14 pixels left of the
offset) toggles a breakpoint there.

## The views

**Threads** - every thread of the scheduler with its id, the module and
offset of its instruction pointer, frame pointer, stack pointer and
state (`runs`, or the wait class it blocks on: `WaitTimerKey`,
`WaitTween`, `WaitMenuBmp` ...).  The selected thread (also the one the
Programs view and the thread areas of the Memory view show) has its
listing on the right, following the instruction pointer; its evaluation
stack top first, each value with its interpretation as a tagged pointer
(`code+`, `data+`, `heap+`, `dynN+`, `gmem+`); and the dwords below its
frame pointer, which is how `push_local_addr` addresses the locals.

**Programs** - the modules the selected thread has loaded (name, base in
the thread's code area, size; the one containing the instruction pointer
in yellow) and the listing of the selected module.  The listings are the
assembler's (`docs/asm.md`: labels for functions, jumps, strings and
data, the current engine's mnemonics), produced by `Dis_Program` from the
bytes in the thread's code area and cached per module while the debugger
is open.

**Bitmaps** - the bitmap manager's slots in use, with size, pixel mode
and generation (the counter of the slot's last allocation), and the
selected slot's picture scaled to fit (never enlarged; ARGB over a
checkerboard, as image viewers show transparency).  Virtual slots (no
pixels) are marked.

**Objects** - the display objects of the graphics manager: the
background, sprites, windows, maps, filters, effectors, particle and rain
screens, knobs, groups; class, slot id, visibility, priority, position
and size, with the selected one's fields (enabled, visible, priority,
level, fade, effect, position, offsets, the fixed-point position and
depth, progress, mask, owner) and its bitmap.

**Sound** - the Wave Master voices: the music channels and the effect
slots with their state, sample rate, channel count, length, position,
DirectSound volume (hundredths of a dB), pan and loop count.

**Memory** - the global memory, the system area and the selected
thread's code, data and local-heap areas as hex dumps with an ASCII
column; PgUp / PgDn and the wheel move through the dump.

**Log** - what happened: module loads (thread, name, base, size),
message boxes (script errors included, the caption in brackets),
breakpoints set and hit, the instruction trace while it is on.  The
wheel scrolls back, End follows the newest line again.  The log is kept
while the debugger is closed, so opening it later shows the history.

## Breakpoints and stepping

A breakpoint is a module name and an offset inside it (so it survives a
module being reloaded at another base, and applies to every thread that
loaded the module).  When a thread is about to execute an instruction at
a breakpoint, the scheduler ends that thread's turn before the
instruction, pauses the VM, selects the thread in the debugger and logs
it; F5 resumes, passing the breakpoint once (the same thread at the same
offset runs through it until it comes around again).

Pausing stops the scheduler's passes over the threads; the frame
services (the message pump, the panels, the presentation) keep running,
so the game's window stays responsive and the picture is kept.  A step
by pass runs exactly one pass; a step by instruction limits every
runnable thread's turn to one instruction for that pass.  Threads
blocked in a wait object do not move while their wait is pending in
either case: a wait on a timer or a key completes on its own terms.

## The pieces

    inc/bgi/dbg.h        the interface the engine uses: Dbg_Init / Shutdown /
                         RequestOpen / Toggle, the scheduler hooks (Dbg_Pass,
                         Dbg_GateRun, gDbgInsnLimit, Dbg_OnInsn behind
                         gDbgHooked), the window's events, Dbg_Log, the overlay
    src/dbg/dbg.c        the window: opening, events, the tabs and status bar,
                         run control, breakpoints, the log ring
    src/dbg/dbgdraw.c    the 32-bit surface and its primitives; text through
                         the OS layer's monochrome glyph rasteriser
                         (OS_FontOpenMono / OS_FontRenderMono, the path of the
                         engine's own GDI text), "MS Gothic" at 8 x 16 where the
                         platform has it, else the monospace face at the largest
                         size whose glyphs fit (OS_FontCharAdvance measures)
    src/dbg/dbg_listing.c   a module's bytes through Dis_Program, split into
                         lines with their offsets
    src/dbg/dbg_pane.c   what the views share: the framed panes, the list
                         areas and the keyboard focus, the listing pane, the
                         description of a tagged script value
    src/dbg/views/*.c    the seven views, one file each (threads.c,
                         programs.c, bitmaps.c, objects.c, sound.c, memory.c,
                         log.c)
    src/os/posix/dbgwin.c   OS_DebugWindowOpen / Close / Alive / Present and
    src/os/win32/dbgwin.c   the debug_* event handlers (X11: a second
                         top-level window; Win32: a window of its own class)

The hooks in the engine: `src/vm/sched.c` (`Dbg_GateRun` before the pass,
`Dbg_Pass` after the frame services, the instruction limit and
`Dbg_OnInsn` in the thread's turn, `Dbg_OverlayPresent` after the frame is
presented), `src/sys/window.c` (F12, the overlay's input, the handlers of
the debugger's window), `src/sys/engine.c` (`--debug`, init and shutdown),
`src/core/error.c` (message boxes into the log), `src/vm/thread.c` (module
loads into the log), `src/wait/wait.c` (`Wait_ClassName`, the names of the
wait classes for the thread list).

## Driving it without a desktop

The Linux build runs under a virtual X server (Xvfb) like any other X11
program, so the debugger can be driven from a script: the game's window
carries the X resource name `bgi` and the debugger's `bgi-debug`, which
is what window-automation tools (xdotool and the like) address keys and
clicks to, and screenshots of either can be taken with the usual X
utilities.  `BGI_MSGBOX_STDERR=1` makes message boxes print to stderr and
take their default button, so an unattended run never blocks on one;
`--debug=pause` holds the scripts at their first instruction until F5
arrives.  An instruction trace without the window at all is
`BGI_TRACE=1` (`docs/vm.md`, "Development aids").
