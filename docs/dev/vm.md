# The script virtual machine

The machinery around the instruction handlers (`src/vm/`). The
reference for single instructions is the handlers: each one in
`src/vm/ops_*.c` carries its opcode bytes and stack effect on its
signature line. Which engine version has which instruction:
[versions.md](versions.md).

## Threads

A thread (`Thread_t`, `thread.c`) owns:

| Area | Holds |
|---|---|
| Code | The program, and the modules loaded after it ("80 40") |
| Data | The evaluation stack and the locals, addressed from the frame pointer |
| Local heap | "70" / "71" allocations |
| Wait object | Present while the thread is blocked |

From 1.599 a thread can run in borrowed parts of another thread's code
and data areas and share its heap ("81 44", `Thread_SpawnBorrower`).
It has a stack of its own. When the lender ends, its borrowers end
first.

## The scheduler

`sched.c` runs the threads round-robin, one pass per frame. A turn
executes up to 0x100000 instructions. Every handler is
`int op(OpenBGI_t* o, Thread_t* t)` and returns:

| Code | Meaning |
|---|---|
| 0 | Next instruction |
| 1, 2 | The turn ends (2: the thread was put into a wait) |
| 3 | Switch to the thread in `o->vm.switchThreadId` |
| 4 | The thread ends |
| 5 | Reboot the machine |
| 6 | Quit |
| 7 | (OpenBGI's own) The turn was cut short: see "Requests and steps" |

A script error (`ScriptError`, `src/core/error.c`) shows a message box
and leaves the pass with a longjmp. It stops the machine, unless a
script asked with "81 6A" (1.547 on) for errors to be kept: then only
the failing thread ends.

## The main loop

The host owns the loop; the engine runs one frame per call of
`OpenBGI_RunFrame` (`include/openbgi.h`). One iteration:

1. The pass: every thread has its turn.
2. The frame services: particles, rain, panels, targets, the cursor,
   knobs, the input poll, the present, the movie player, the debugger's
   overlay.
3. The host shows the picture, hands over input (`OpenBGI_Key`,
   `OpenBGI_Button`, ...) and calls `OpenBGI_PumpEvents`.
4. The host waits as `OpenBGI_Frame_t.idleMs` says.
5. The late panel poll, the debugger's turn, the teardown of ended
   threads.

`OpenBGI_RunFrame` does 1 and 2 and returns the picture and what
changed in it; the next call starts with 5. The engine never sleeps and
reads no clock but the host's. Start-up (the game chooser, measuring
the clock, the boot) is spread over frames too (`src/sys/engine.c`).

## Requests and steps

The original waits inside an instruction for a message box, a dialog, a
file picker or a disc. The engine cannot block inside a frame, so:

- A **request** (`src/sys/request.c`) is a question to the host
  (`OpenBGI_NextRequest`, `OpenBGI_Answer`). While one is unanswered,
  `OpenBGI_RunFrame` runs nothing.
- A **step** (`WaitStep_t`, `src/wait/wait_step.c`) is a wait whose
  function runs once a frame, takes the answer and pushes the
  instruction's results. Until it is done its thread holds the engine
  (`o->vm.holder`): no other thread runs and nothing is presented, as
  in the original.

After an instruction that left a request open, the turn and the pass
end with code 7; `o->vm.resumeAt` is the thread the pass continues
with.

A load that finds its file missing while a disc is in use returns
`BGI_RETRY_VALUE`. The scheduler puts the thread back to the start of
the instruction (`Thread_t.opSP`) with a step that asks for the disc.
Memory an instruction allocates before such a load is taken with
`Vm_TempAlloc`, so that the restart frees it.

## Instructions

- One opcode byte. `80`, `81`, `90`, `91`, `92`, `A0`, `B0`, `C0`,
  `D0`, `E0`, `7F` read a second byte and dispatch through a family
  table. `FF` is the user-defined instruction mechanism (`userop.c`).
  `EF` is OpenBGI's own family (`ops_obgi.c`,
  [../modding/obgi.md](../modding/obgi.md)).
- Base instructions `00 .. 7F` (`ops_base.c`) take operands from the
  evaluation stack; a few read inline bytes after the opcode (`00` i8,
  `01` i16, `02` i32, `04`-`06` i16, `08`-`0C` a size byte).
- A handler's comment gives `inputs -> outputs`, inputs in push order.
- Handlers are registered as `{op, fn, GEN_from, GEN_to}` lists
  (`OpEntry_t`); `Vm_FillTable` fills the tables for the selected
  generation, and `src/vm/opset.c` (generated) says which opcode each
  generation defines.

## Tagged pointers

A script pointer is 32 bits: an area tag in the top bits and an offset.
`ResolvePtr` (`src/core/dynmem.c`) turns it into a host pointer and
raises a script error for an unmapped area. The layout depends on the
engine version and is part of the script ABI, because compiled scripts
contain such pointers.

| Area | 1.58 .. 1.66 | 1.69 builds 444, 451 | 1.69 build 472 on |
|---|---|---|---|
| Global memory | `A` | `A` | `A` |
| Code area of the thread | `0x11000000 + A` | `0x10000000 + A` | `0x04000000 + A` |
| Data area | `0x10000000 + A` | `0x12000000 + A` | `0x08000000 + A` |
| Local heap | `0x12000000 + A` | `0x14000000 + A` | `0x0c000000 + A` |
| Dynamic global block n | `((0x40 + n) << 24) + A` | `((0x10 + n) << 25) + A` | `((0x10 + n) << 26) + A` |

From 1.535 the dynamic blocks come from pools by size and are not
numbered.

## Followed values

A game can read the engine's own program file and compare a checksum
of it: [program_file.md](program_file.md).

## Development aids

`src/vm/trace.c`. None exists in the original.

| Setting | Effect |
|---|---|
| `BGI_TRACE=1`, `BGI_TRACE=prog[,prog]` | Prints every executed instruction: thread, program, IP, opcode bytes, frame pointer, top three stack values |
| `BGI_TRACE_FROM=ms` | Starts the trace later |
| `BGI_TRACE_LAST=n` | Keeps a ring of n instructions and dumps it when a message box opens |
| `BGI_TRACE_OPS=FAM:OP,...` | Follows chosen instructions with arguments and results |
| `BGI_OPSTAT=file` | Writes a histogram of executed opcodes when the machine stops |
| `BGI_PANEL_DEBUG=1` | Prints the buttons of every panel and the events it queues |
| `--list-missing` | Prints the profile's opcodes that have no handler |
| `--debug` | The debugger ([../modding/debugger.md](../modding/debugger.md)) |
