# The script virtual machine

The conventions the VM sources (`src/vm/`) rely on, in one place.  The
instruction-by-instruction reference is the handlers themselves: every
opcode handler of `src/vm/ops_*.c` carries its opcode bytes and stack
effect on its signature line, and `docs/versions.md` lists which opcodes
each build of the engine has; this file is about the machinery around
them.

## Threads and the scheduler

A thread (`Thread_t`, `thread.c`) owns a code area (the program and the
modules appended to it by "80 40"-style loaders), a data area (the
evaluation stack and the locals, addressed from the frame pointer), a
local heap ("70" / "71") and a wait object when it is blocked.  `VmMain`
(`sched.c`) runs the threads round-robin: each turn executes up
to 0x100000 instructions of one thread, or fewer when a handler asks.
Every handler is `int op(Thread_t* t)` and returns a scheduler code:

| code | meaning |
|---|---|
| 0 | next instruction |
| 1, 2 | the turn ends (2: the thread was put into a wait) |
| 3 | switch to the thread in `gSwitchThreadId` |
| 4 | the thread ends ("17" at the outermost frame) |
| 5 | reboot the machine |
| 6 | quit |

Script errors (`ScriptError`, `error.c`) are raised with a longjmp to the
frame `VmMain` holds, after a message box; the original throws a C++
`int` the same way.

## Instructions

The opcode is one byte; `80`, `90`, `91`, `92`, `A0`, `B0`, `C0` read a
second byte and dispatch through their family table (`ops_sys.c`,
`ops_gfx0/1/2.c`, `ops_snd.c`, `ops_ext0/1.c`); `FF` is the user-defined
instruction mechanism (`userop.c`).  The base instructions `00 .. 7F`
(`ops_base.c`) take their operands from the evaluation stack and, for a
few, from the code stream after the opcode (`00` i8, `01` i16, `02` i32,
`04` / `05` / `06` i16, `08` / `09` / `0A` / `0B` / `0C` a size byte).  The
trailing comment on a handler's signature line gives its stack effect
`inputs → outputs`: the inputs in the order the script pushed them (the
last one is on top of the stack and is popped first), then what the
handler pushes; `tests/vm.c` exercises the base set through a small
assembler.

Other builds of the engine have more families (`81`, `D0`, `E0`, `7F`)
and more opcodes; `docs/versions.md` lists them.  `EF` is not the
original's: it is the family of OpenBGI's own instructions
(`ops_obgi.c`, `docs/obgi.md`), present in every profile.

## Tagged pointers

Script pointers are 32-bit values made of an area tag in the top bits and
an offset.  In the 1.69 build 444 this reimplementation follows, the tag
is the top 7 bits: 0 = global memory, 8 = the thread's code area, 9 = its
data area, 0xA = its local heap, 0x10 .. 0x3F = the dynamic global blocks
of "80 20".  Other builds lay the tags out differently (24-bit offsets
before 1.69, 26-bit from 1.69 build 472 on), and compiled scripts embed
such pointers (e.g. `push32 0x20220400` in `syswnd._bp` addresses dynamic
block 0), so the layout is part of the script ABI.  The layout and the
local-heap limit come from the selected engine profile (`inc/bgi/version.h`,
`Engine_SelectProfile`): detected from the game's `ipl._bp` through
`games.json`, or forced with `--engine=NAME` / `BGI_ENGINE=<name>`, default
`1.69/444`.  The profile's generation also decides which opcodes exist
and which handler variant serves them (`OpEntry_t` lists, `Vm_FillTable`,
the generation bits of `opset.c`).  `ResolvePtr` (`dynmem.c`) turns a tagged
value into a host pointer and raises a script error for an unmapped
region.

## Programs and scenario files

A program (`*._bp`, in `sysprg.arc` / `system.arc` / `autoload.arc`)
starts with a 16-byte header - the code offset (0x10), the code size and
two zero words - and holds bytecode followed by its string constants
(`caddr` operands point into the same area).  The scenario files are
interpreted by the system programs, not by the engine: 16-bit opcodes
with inline operands in 1.64, token streams without a header in 1.66 /
1.69 (`docs/scenario.md` has both).  From 1.494 the scenario (`data01000.arc`:
`_01`, `a01`, ..) is a `BurikoCompiledScriptVer1.00` file: a 28-byte
magic, the header size, the count and names of the programs it imports
(`framework`, a character module, `emotion` ..), an export count with
(name, offset) pairs, then a stream of 32-bit tokens that the system
programs interpret themselves (`script._bp` loads the file and its
imports into the thread's code area, `scrdrv._bp` walks the tokens).
Those files contain no engine opcodes, so they do not bear on which
instructions a game needs.

## Development aids

None of these exists in the original; the tracing lives in `src/vm/trace.c`.
`BGI_TRACE=1` or `BGI_TRACE=<program>[,<program>]` prints every executed
instruction (thread, program, program-relative and absolute IP, the opcode
bytes, the frame pointer and the three values on top of the evaluation
stack); `BGI_TRACE_FROM=<ms>` delays it, `BGI_TRACE_LAST=<n>` keeps a
ring instead and dumps it when a message box opens, `BGI_TRACE_OPS=
FAM:OP,..` follows chosen instructions with their stack arguments and
results; `BGI_OPSTAT=<file>` writes a histogram of the executed opcodes
("family opcode count" lines) when the machine stops; `--list-missing`
prints the opcodes of the selected profile that have no handler yet, and
`internal/re_tools/missingops.py` (not part of the release) tells which
of them a game's programs use.  `BGI_PANEL_DEBUG=1` prints the buttons of
every panel as it starts and the events it queues (`docs/panel_system.md`).
`bin/bpasm -d`
disassembles a program from a file or an archive into a listing with
labels, names and strings (`docs/asm.md`), and the debugger (`--debug`,
`docs/debugger.md`) shows the threads, their listings and stacks live.
