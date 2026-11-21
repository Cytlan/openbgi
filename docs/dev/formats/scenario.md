# Scenario files

The compiled story files. The engine does not execute them: the game's
own system programs interpret them (`scrdrv._bp` fetches tokens,
`scrctrl._bp` is the stack machine, `scrmsg`, `scrsnd`, `scrgrp1` .. are
the commands). `src/scn/` decompiles and compiles them
([../../modding/scenario.md](../../modding/scenario.md)).

## Container (1.494 and later)

All numbers are little-endian.

| Offset | Size | Content |
|---|---|---|
| 0 | 28 | `BurikoCompiledScriptVer1.00`, NUL |
| 0x1c | 4 | Size of the rest of the header |
| 0x20 | 4 | Import count |
| | | Import names, NUL-terminated |
| | 4 | Export count |
| | | Exports: name (NUL-terminated), dword offset |
| | 1..16 | Zero bytes; the token stream starts at a multiple of 16 |
| | | Token stream: 32-bit words |
| | | String table: NUL-terminated Shift-JIS strings |

A string or code operand is a byte offset from the start of the token
stream. The string table begins where the lowest referenced string
lies. `StringsDB` and `ScenarioDB` in the same archive are tables, not
scenario files.

## Headerless files (1.66, 1.69)

The same token stream from offset 0, strings after it. Operands are
file offsets. No imports or exports. Recognised by the frame prologue
of the first function (`push_fp; push N; add; set_fp`). Scenes call
functions of the game's `main` by address (`push ADDR; 0xf2`).

## Tokens

Tokens below 0x100 are the base machine; commands start at 0x100 and
differ per game. `openbgi scn --scan sysprg.arc` reads a game's
dispatch table and prints each command's module, name, argument count
and result.

| Token | Meaning |
|---|---|
| 0x00 `push v` | Immediate |
| 0x01 `addr off` | Code address |
| 0x02 `local n` | Address of the local at frame - n |
| 0x03 `str off` | String |
| 0x08 `load s` | Pop address, push byte (s = 0) or dword |
| 0x09 `store s` | Pop value, pop address, store |
| 0x0a `store2 s` | Pop address, pop value, store |
| 0x10, 0x11 | Push, set the frame pointer |
| 0x18 `jmp` | Pop target |
| 0x19 `jcc c` | Pop target and value; jump if zero (c = 0) or nonzero (c = 1) |
| 0x1a `call`, 0x1b `ret` | |
| 0x1c `callfn` | Call an imported function by name (string on top) |
| 0x1d `setret`, 0x1e `try`, 0x1f `endtry` | Return value, exit frame |
| 0x20 - 0x2b | add sub mul div mod and or xor shl shr sar |
| 0x30 - 0x35, 0x38 - 0x3a | eq ne le ge lt gt, land lor lnot |
| 0x3f `args n` | Reverse the top n values |
| 0x40 `memcpy_r`, 0x48 `strcpy_r` | Copies with the source on top |
| 0x7b `srcinfo`, 0x7f `line`, 0xfe `lineinfo` | Source position markers |
| 0x80 - 0x83 | memcpy, memclr, memset, memcmp |
| 0x90 - 0x99 | strcpy, strcat, strcpy_lower, strreplace, strlen, streq, itoa, substr |
| 0xa0 - 0xc1 | random, atoi, bool, mulshift, atan2, vec_len |
| 0xe0 - 0xea | Work and flag variables: set_lwork, lwork, set_gwork, gwork, set_lflag, lflag, set_gflag, gflag, lstr, set_gflag2, gflag2 |
| 0xec, 0xf0 - 0xff | regstr, call_scenario, jump_alias, (0xf2: call by address), jump_scenario, quit, quit_if_idle, set_label, test_label, msgbox_num, msgbox, debug_dump, file_dialog, debug_pc |

A command pops its first argument first. Arguments are either pushed
right to left, or left to right followed by `args n`.

## Compiler styles

The files come from several compilers. The decompiler reproduces each
one's habits so that a file compiles back to identical bytes; the
source's `#style` line says which.

| Style | Markers | Arguments | Unary minus | Notes |
|---|---|---|---|---|
| `library` | `line` before every statement and closing brace | Right to left, no `args` | `x * 0xffffffff` | `main` first, then functions sorted by upper-cased name; string table starts with the source name and headers |
| `scenario` | `str F; push L; args 2; lineinfo` before simple statements | Left to right, then `args n`; `msg` without `args` | `0 - x` | Ends with a label dispatcher; `#srcinfo` variant prefixes messages with `srcinfo` |
| `scenario-line` | `line` tokens, also before `if` | `args` form for `msg` too | | `goto` goes through the dispatcher |
| `library-raw` | As `library` | Left to right, `args n` for two or more | `x * 0xffffffff` | Functions named `fn_0001` .. in layout order; `#file` switches the source name |

## 16-bit scenes (1.64)

No stack and no expressions: a flat stream of 16-bit opcodes, each
followed by its operands (dwords and NUL-terminated strings) as its
handler reads them.

- A jump target and a message's text are file offsets.
- Texts lie after the code, in order of reference, from the next
  multiple of 16.
- A variable operand is a dword: high word 1 = scene work variables,
  2 = global ones; low word = index. High word 0 = literal.

The opcode table is built in (`src/scn/scn16.c`); it covers the one
1.64 game. Another game of that build needs its handlers read the same
way.

## Code

| File | Content |
|---|---|
| `src/include/bgi/scn.h` | The interface: `Scn_Load`, `Scn_Save`, `Scn_ScanCommands`, `Scn_ListFunctions`, `Scn_Decompile`, `Scn_Compile` |
| `src/scn/scnfile.c` | The containers |
| `src/scn/scncmds.c` | The command table scan |
| `src/scn/scndec.c`, `scncomp.c` | Decompiler, compiler |
| `src/scn/scntok.c` | Token and operator tables |
| `src/scn/scn16.c` | The 16-bit scenes |
| `src/fs/arcwrite.c` | Archive writer for `--pack` |
| `tests/scn.c` | Containers, code generation, round trip |

## Limits

- The command scan counts the pops of each handler; a handler that pops
  in a deeper subroutine is counted short. The decompiler compensates at
  the end of a statement.
- Nested switches that share a slot, and a `default:` in last position,
  are told apart by heuristics. The round trip guards them.
- The string order of a raw library is found by trying every position
  of its own functions among its includes.
