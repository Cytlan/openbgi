# Coding conventions

The formatting and naming scheme of OpenBGI.  Formatting is enforced by
the `.clang-format` at the project root (`clang-format -i --style=file`);
the generated files (`src/include/bgi/msg.h` and the instruction tables) and
`src/host/libretro/libretro.h`, which is libretro's own header kept as
it came, are excluded via `.clang-format-ignore`.

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
| The engine as a host drives it (`include/openbgi.h`) | `OpenBGI_PascalCase`, `OpenBGI_PascalCase_t`, `OPENBGI_UPPER_SNAKE` | `OpenBGI_RunFrame`, `OpenBGI_Frame_t`, `OPENBGI_RUNNING` |
| Opcode handlers | `Opcode_<Group>_<Name>` (static) | `Opcode_PushI8`, `Opcode_Sys_CountFiles` |
| Forwarders to the global graphics manager | `GfxCall_<Method>` | `GfxCall_SpriteShow` |
| File-local helpers | `PascalCase`, `static` | `NewPixBuf`, `ShowBracket` |
| Engine state (the original's globals) | `o-><group>.<camelCase>`, a field of `OpenBGI_t` | `o->gfx.gfx`, `o->files.baseDir` |
| Globals (constant tables, the OS layer) | `gPascalCase`, `kPascalCase` for constants | `gMsgEnglish`, `kStdKeys` |
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

* C99; the original is C++.  (The exceptions are
  `src/host/haiku/haiku.cpp`, because Haiku's kits have no C interface; it
  stays behind the C header `src/host/os_haiku.h` - and the Objective-C
  files of `src/host/macos/`, because AppKit and Metal have none either;
  they stay behind `src/host/os.h`.)  The core - all of
  `src/` but `src/host/`, `src/movie/` and `src/tools/` - is plain ISO C99: it includes
  its own headers (`src/include/bgi/`) and the API's (`include/`), the ISO
  C library's and FreeType's and Vorbis's, nothing of a host's
  (`src/host/`, "host/os.h"), and asks for no feature or platform macro;
  it is compiled with those two folders alone, and `make check-core`
  builds it so on its own and runs it without a line of a host's code.
  Within the core, `src/base/` is what needs no engine - the codecs, the
  text tables, memory - and includes neither the engine's state
  (`bgi/core.h`) nor the API; the same check holds it to that, and
  `tests/base.c` is linked with it alone.  The hosts (`src/host/`), the tools' code
  (`src/tools/`) and the tests use the systems' headers as they need
  them; the movie decoder (`src/movie/`) uses ffmpeg's and ISO C's,
  nothing of a system or of the hosts.  Objects are plain
  structs with the vtable pointer first (`obj.vt`), constructors are
  `Class_Ctor`, destructors `Class_Dtor`; `Class_New` / `Class_Delete`
  allocate as well.  Objects are allocated zeroed (`BGI_Calloc`) where the
  original relied on `new` plus explicit zeroing.
* Method result codes keep the original values (`0x8000000n` from objects,
  small integers from the manager wrappers, `0xFF` for an unknown handle).
* Rectangles are inclusive (`r`, `b` are the last pixel), fixed point is
  16.16 unless a comment says otherwise.
* The engine keeps no state in global or static variables.  Everything it
  keeps between calls is a field of one struct, `OpenBGI_t`
  (`src/include/bgi/core.h`), grouped by the part of the engine it belongs to
  (`o->vm`, `o->text`, `o->snd`, ...), so that several engines can share
  a process and one can be dropped and started again.  Every function
  that reads or changes that state, itself or through what it calls,
  takes the engine as its first parameter, `OpenBGI_t* o`: opcode
  handlers (`int op(OpenBGI_t* o, Thread_t* t)`), the methods of the
  engine's objects and every callback the engine is called back through
  (the OS layer's event handlers, the launcher, the installer's dialogs
  carry the engine to pass back; the host's audio callback and the
  workers get it through their user pointer).  A macro that reads the state takes `o`
  as a parameter too (`VM_TAG_CODE(o)`).  A field is named after the
  original's global without the `g` (gBaseDir is `o->files.baseDir`), the
  name the address table outside the code base uses; a new piece of state
  is a new field.  Starting values that are not zero are set in
  `OpenBGI_New` (`src/core/openbgi.c`).  What may stay global: constant
  tables, tables computed once from nothing (the CRC table, the DCT
  cosines), and the OS layer's own state.  A program that runs engine
  code makes an engine first (`OpenBGI_New`, with the host's services:
  `OS_Host()` over the OS layer) and frees it at the end
  (`OpenBGI_Delete`).
* The host owns the main loop and the clock (`include/openbgi.h`): the
  engine runs one frame per `OpenBGI_RunFrame`, never sleeps, and reads
  time only through the host's services (`Clock_HostMs`, `Clock_PerfNs`,
  `Clock_LocalTime`, and `GetTicks`, the scripts' clock built on them),
  never from the OS layer.  The same holds for the input: keys, buttons,
  the wheel, the pointer and the focus are what the host hands in
  (`OpenBGI_Key` and the rest), and the engine's own pointer moves go
  out with the frame; the engine asks the OS layer for none of it.  The
  fonts too: the engine rasterises its text itself
  (`src/core/ftfont.c`) from the font files its host finds, or draws with
  the renderer its host brings (`src/gfx/typeface.c`, `openbgi.h` "The
  fonts").  And the files: every file, directory, drive, shell folder and
  registry value is reached through the host's tables (`Host_*`,
  `src/include/bgi/host.h`, usually under `Fs_*`), with the game's paths as the
  scripts build them or native paths of the host's (`openbgi.h` "The
  files"), the font files FreeType reads included.  And the window: the
  main window and the debugger's are the host's tables (`MainWin_*`,
  `src/sys/mainwin.c`; without one the engine keeps a window nobody
  sees), what the window's procedure got comes in as the host's calls
  (`OpenBGI_Close` and the rest), and a message the engine sends itself
  (the posted close, the quit) is its own, taken by `OpenBGI_PumpEvents`;
  the engine pumps no messages of the system's.  And the rest of the
  machine: the processor (CPUID, the time stamp counter), the memory, the
  names, the Windows version, other programs, the instance lock, the
  clipboard, the desktop, CD audio and the movies are the host's tables
  too (`Host_*`, `openbgi.h` "The machine" to "The movies"; without one
  an ordinary machine of the original's time, no shell, no CD drive, no
  movies).  The engine has no threads and no locks: what a host makes on
  threads of its own (a movie's frames and sound) the engine takes from it
  on its own; and a file the compilers include comes from their caller
  (`PrepIncludes_t`, `src/include/bgi/prep.h`).
* The engine never waits inside a frame.  What the original showed and
  waited for - a message box, a dialog, a picker - is a request to the
  host (`MsgBox`, `MsgBox_Ask`, `Request_Post`; `src/include/bgi/request.h`),
  never a call of the OS layer's window; what it waited for besides is a
  step taken once a frame (`WaitStep_t`, `src/include/bgi/waitobj.h`).  An
  instruction that waits gives its thread such a step, which holds the
  engine as the original's wait did (docs/dev/vm.md, "Requests and
  steps").  Engine code that runs while the host shows a window (the
  game chooser's and the installer's callbacks) uses the window's own
  callbacks for its boxes and pauses.
* Japanese literals never appear in the sources: they live in
  `tools/messages.txt` (with an English translation) and are emitted into
  `src/include/bgi/msg.h` as `MSG_*` macros.  Japanese text that is only quoted in
  a comment is accompanied by its translation.
