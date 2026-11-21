# Programs (`._bp`)

The bytecode files the engine executes: the system programs of
`sysprg.arc`, the boot program `ipl._bp` of `system.arc`, the entries
of `autoload.arc`. Tools: [../../modding/programs.md](../../modding/programs.md).
The machine: [../vm.md](../vm.md).

## File

Little-endian.

| Offset | Size | Content |
|---|---|---|
| 0 | 4 | Offset of the code: 0x10 |
| 4 | 4 | Size of the code area |
| 8 | 8 | Zero |
| 0x10 | | The code area: bytecode, then string constants and data |

The engine starts a program at offset 0 of the code area. Strings are
NUL-terminated Shift-JIS (UTF-8 in a game switched to it).

## Code

- An instruction is one opcode byte, or a family byte and a second
  byte, followed by inline operands for a few base instructions.
- References inside the program (`push_code_addr`, `push_code_off`)
  are signed 16-bit distances from the instruction's own offset, so the
  original's programs stay below 32 KB. OpenBGI's compiler switches a
  larger program to 32-bit offsets (`push_i32`).
- A program may contain tagged 32-bit pointers ([../vm.md](../vm.md)).

## What the original compiler emits

The decompiler relies on these fixed shapes and the compiler reproduces
them (`src/bpc/bpc_internal.h` has the full list):

| Construct | Code |
|---|---|
| Function entry | `push_fp; push F; add; set_fp`, then the parameters popped into the frame, first parameter first |
| Function exit | One `push_fp; push F; sub; set_fp; ret`; `return v` pushes v and jumps there |
| Arguments | A function's are pushed right to left, an instruction's left to right |
| Assignment | Address, value, `store_keep`; a call's value and an initialiser are value, address, `store` |
| Immediates | `push_i8` up to 127, `push_i16` up to 32767; a negative constant is the positive one negated (`not; push_i8 1; add`) |
| Conditions | Chains of `jcc`; `&&` jumps to the false target, `\|\|` to the true target |
| Loops | `while` tests at the head; `do` at the end; `for` is a `while` with its step before the back jump |
| Dead code | Emitted as written |

No constant folding takes place.

## Finding code in a file

The disassembler (`Dis_Analyze`, `src/asm/`) follows the code from
offset 0 and from every relative branch target until a return, an
unconditional jump, an instruction that ends the thread or an undefined
opcode. A gap that decodes cleanly from its start to its end is code
nothing refers to (`; unreferenced`). The rest is data: strings where
NUL-terminated printable Shift-JIS lies, bytes elsewhere.

## Code

| File | Content |
|---|---|
| `src/include/bgi/asm.h`, `src/asm/` | `Asm_Decode`, `Asm_Format`, `Asm_OpName`, `Dis_Program`, `Dis_Analyze`, `Asm_Assemble`; `opnames.c` and `opstack.c` are the name and stack-effect tables |
| `src/include/bgi/bpc.h`, `src/bpc/` | `Bpc_Decompile`, `Bpc_Compile`, `Bpc_CompileText` (messages as text; what the launcher's start uses) |
| `src/include/bgi/prep.h`, `src/asm/prep.c` | `Prep_Expand`: `#include`, `#define`, `#undef` |
| `tests/asm.c`, `tests/bpc.c` | Encodings, names, shapes, round trips |
