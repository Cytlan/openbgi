# Coding conventions

The formatting and naming scheme of OpenBGI.  Formatting is enforced by
the `.clang-format` at the project root (`clang-format -i --style=file`);
`inc/bgi/msg.h` is generated and excluded via `.clang-format-ignore`.

## Formatting

* Allman braces (the brace on its own line), also for `struct` bodies.
* Tabs for indentation (4 columns), spaces for alignment.
* `Type_t* ptr` - the star binds to the type.
* No space between a keyword and its parenthesis: `if(`, `for(`, `while(`,
  `switch(`.  `case` labels are indented one level inside the `switch`.
* No column limit; a statement is broken where it reads best.
* `//` comments inside functions and after declarations; `/* ... */` only
  for the file header and for multi-line comments in front of a function.
* Opcode handlers carry the opcode bytes and the stack effect
  (`inputs → outputs`, the inputs in the order the script pushed them, the
  last one on top of the stack) in a trailing comment on their signature line:
  `static int Opcode_Sys_CountFiles(Thread_t* t) // 80 24: pattern, recurse → n`;
  what the instruction does goes in a comment above when the name and the
  stack effect do not say it.

## Comments

Comments describe usage, not the reverse engineering: what a function
does, what its arguments mean, what it returns (every result code of the
original is kept, so say what each value stands for), and anything a
caller has to know (ownership, locking, which instruction of the scripts
reaches it).  The addresses of the routines of `tayutama.exe` that the code
reproduces are not in the sources; a lookup table outside the code base
maps every function, global and type to its address and offsets.

* Every file starts with a header comment.  Its first two lines name the
  project and the licence, the same in every file (in the file's own
  comment syntax):

      This file is part of OpenBGI (https://openbgi.net).
      SPDX-License-Identifier: GPL-2.0-only

  After an empty line come the file's name, what it holds, the header that
  declares its interface, and - in a paragraph when it helps - how the
  parts fit together.  Generated files get the same two lines from their
  generators.
* Every function that is not trivially named has a comment in front of it.
  One line (`//`) when one sentence says it; a block (`/* ... */`) when the
  arguments or the result codes need explaining.  Trivial getters and
  constructors may go without.
* Inline comments explain the "why" of a non-obvious step, the meaning of a
  magic number, or a quirk of the original that is reproduced on purpose
  (which looks like a bug otherwise).
* Struct members carry a trailing comment with their meaning (and unit:
  pixels, 16.16, milliseconds) unless the name says it all.
* Instruction references use the opcode bytes in quotes: `"91 55"`,
  `"80 3F" of 1.69 build 472 on`.

## Naming

| Kind | Scheme | Examples |
|------|--------|----------|
| Types | `PascalCase_t`, the `struct` tag without the suffix | `Window_t`, `struct Window`, `DispObjVtbl_t` |
| Methods / module functions | `Module_PascalCase` | `Window_SetFont`, `Gfx_ObjSetPos`, `Thread_Push` |
| Acronym modules | upper-case module | `BGI_Alloc`, `OS_FileAttrs` |
| Opcode handlers | `Opcode_<Group>_<Name>` (static) | `Opcode_PushI8`, `Opcode_Sys_CountFiles` |
| Forwarders to the global graphics manager | `GfxCall_<Method>` | `GfxCall_SpriteShow` |
| File-local helpers | `PascalCase`, `static` | `NewPixBuf`, `ShowBracket` |
| Globals | `gPascalCase` | `gGfx` |
| Vtable instances | `<Class>_Vtbl` | `Window_Vtbl` |
| Struct fields | `camelCase` | `int32_t textLevel; // the level of the text layer` |
| Macros / constants | `UPPER_SNAKE` | `WIN_ITEMS`, `H_SPRITE`, `MSG_ASK_ABORT` |
| Header guards | `BGI_<PATH>_H_` | `BGI_GFX_WINDOW_H_` |

Names describe what the reverse engineering established a routine or field
does.  Where a value's meaning is still unknown the name says so
(`unknown2A4`, `pad2A4`) rather than guessing; the names of the earlier
OpenBGI sources were chosen with less information and are not copied when
they disagree with the disassembly.

## Semantics

* C99; the original is C++.  Objects are plain structs with the vtable
  pointer first (`obj.vt`), constructors are `Class_Ctor`, destructors
  `Class_Dtor`; `Class_New` / `Class_Delete` allocate as well.  Objects are
  allocated zeroed (`BGI_Calloc`) where the original relied on `new` plus
  explicit zeroing.
* Method result codes keep the original values (`0x8000000n` from objects,
  small integers from the manager wrappers, `0xFF` for an unknown handle).
* Rectangles are inclusive (`r`, `b` are the last pixel), fixed point is
  16.16 unless a comment says otherwise.
* Japanese literals never appear in the sources: they live in
  `tools/messages.txt` (with an English translation) and are emitted into
  `inc/bgi/msg.h` as `MSG_*` macros.  Japanese text that is only quoted in
  a comment is accompanied by its translation.
