# The assembler and disassembler

`src/asm/` turns a Buriko program (`*._bp`) into an assembler listing and
back; `bin/bpasm` (`make tools`) is the command.  Every program of every
game examined (1856 of the 21 games' `system.arc` / `sysprg.arc` /
`autoload.arc`) disassembles and assembles back to the identical bytes
(`bpasm -r`), which is the module's acceptance test; `tests/asm.c` checks
the encodings, the names and the error paths.  It is the ground of the
decompiler and compiler of the programs (`docs/bpc.md`, the same
binary's `-D` and `-C`): the decoder (`Asm_Decode`) and the flow analysis
(`Dis_Analyze`) are what the decompiler builds on, and the listing is
what the compiler targets.  The scenario files the programs interpret
are a different format with their own tool, `bin/bgiscn`
(`docs/scenario.md`); its command-table scan uses `Asm_Decode` on the
system programs.

    bpasm -d [-e ENGINE] [-b] [-O] [-o OUT.s] FILE._bp
    bpasm -d [-e ENGINE] [-b] [-O] --arc ARCHIVE NAME
    bpasm -a [-e ENGINE] FILE.s -o OUT._bp
    bpasm -r [-e ENGINE] (FILE._bp... | --arc ARCHIVE [NAME...])
    bpasm -l ARCHIVE
    bpasm -D / -C / -R: the decompiler and compiler (docs/bpc.md)

`-e` is an engine profile (`bgi --list-engines`) or `any` (the default).
The profile decides which opcodes exist and what a reused number means
(`90 8B` is `window_set_punch` up to 1.64, nothing afterwards, and the
same instruction is `90 87` from 1.66), how the conditional jump is
encoded (1.667+), and the layout of tagged pointers for `code(label)`.
Under `any` every opcode of every build is accepted, a name means its
newest number, and an opcode whose name would assemble back to another
number is written numerically (`gfx0.0x8b`), so that a listing made under
`any` still round-trips.  `-b` adds the instruction bytes and `-O` the
offsets in columns ended by `|`; the assembler skips such columns.

## The listing

    ; openbgi disassembly of ipl._bp
    .engine 1.64
    ; 1904 bytes of code area; the entry is offset 0

            push_fp
            push_i16 600
            add
            set_fp
            sys.get_launcher_mode
            push_code_off loc_055f
            jcc 0
            push_code_addr str_0580              ; "実験リソース"
            sys.push_search_name
            ...
    sub_0a1c:
            push_fp
            ...
            ret
    sub_0b40: ; unreferenced
            ...
    str_0580:
            .str "実験リソース"
    dat_0640:
            .db 0x01, 0x00, 0x00, 0x00

One statement per line; a label is an identifier followed by `:` (several
may precede a statement); `;` starts a comment.  Mnemonics:

* the base instructions by the names of the reimplementation's handlers
  (`push_i8`, `push_local_addr`, `store_multi`, `jcc`, `sys.yield` ...;
  the tables of `src/asm/opnames.c`, generated from the opcode tables of
  `src/vm/ops_*.c`).  A base opcode without a name is written as its
  number (`0x73`).
* the family instructions as `family.name` or `family.0xHH`: `sys` (80),
  `sys81` (81), `sys7f` (7F, 1.667+), `gfx0` / `gfx1` / `gfx2` (90 / 91 /
  92), `snd` (A0), `ext0` / `ext1` / `ext2` (B0 / C0 / E0), `eval` (D0),
  `user` (FF, the user-defined instructions: `user.0x12`).

Operands are expressions of numbers (decimal, `0x` hex, `'c'`), labels,
`+` / `-`, and `code(label)` - a tagged pointer into the code area, as
`push_i32` carries them.  The operand kinds, as the instructions read
them inline (everything else comes from the evaluation stack):

| instruction | operands |
|---|---|
| `push_i8 v`, `push_i16 v`, `push_i32 v` | an immediate of that size |
| `push_local_addr n` | `fp - n` (u16) |
| `push_code_addr L` | a data address: L, encoded relative to the instruction's own offset (s16) |
| `push_code_off L` | a code target, likewise |
| `load s`, `store s`, `store_keep s` | the size code 0 / 1 / 2 (byte / word / dword) |
| `store_inline b, b, "text"` | up to 255 bytes copied inline (strings allowed, no NUL added) |
| `store_multi s, n` | the size code and the count |
| `jcc c` | the condition (0 `!=0`, 1 `==0`, 2 `>0`, 3 `>=0`, 4 `<=0`, 5 `<0`); the target is popped |
| 1.667+: `jcc c, L` with `c & 8` | the inline form: condition and a relative target |
| 1.667+: `store_local n, s`, `push_local n, s`, `mul_local`, `div_local` | a local reference: offset below the frame pointer and size code |
| 1.667+: `push_local_add n, s, v`, `push_local_index n, s, v` | ... and a varint immediate |
| 1.667+: `push_local_shl n, s, b` | ... and a byte |
| 1.667+: `jmp_rel L` | a varint offset from the end of the instruction |
| 1.667+: `load_abs v` (u32), `load_rel v`, `add_imm v`, `mul_imm v`, `muladd_imm v`, `div_imm v` (varints) | |
| 1.667+: `jcmp c, L` | a condition byte and a relative target |
| 1.667+: `shift_imm b` | |

Convenience forms the assembler expands: `push V` (`push_i8` /
`push_i16` / `push_i32` by the value), `jmp L`, `call L`, `jcc C, L` (a
`push_code_off L` before the instruction; in 1.667+ a `jcc` whose
condition has bit 3 is the inline form instead).  The relative operands
have a 16-bit range, so a program stays below 32 KB - every program of
the games does, and the compiler never uses an absolute target.

Directives: `.engine NAME` (overrides `-e`), `.str "text"[, "text"]`
(Shift-JIS, NUL-terminated), `.db` / `.dw` / `.dd` lists (strings in
`.db`, without a NUL), `.align N[, fill]`.  Strings are UTF-8 in the
listing and become Shift-JIS (CP932) in the file; `\xNN` inserts a byte,
`\n \r \t \\ \"` as usual.  The disassembler writes a string in UTF-8 only
when the conversion there and back is exact (CP932 has duplicate codes
for a few characters), else as escaped bytes.  For a game whose text is
UTF-8, `-u` (or a line `#encoding utf-8` in a source) keeps the strings as
they are written (`docs/bpc.md`, "Strings in UTF-8").

The second byte of the families is named after the family: `sys.yield`,
`gfx0.sprite_create`, `snd.se_play`, and `obgi.text_scroll` for OpenBGI's
own instructions, the "EF xx" family, which every engine profile has
(`docs/obgi.md`).

## How the disassembler finds the code

The code is followed from offset 0 (where the engine starts a program)
and from every relative branch target (`push_code_off`, the 1.667+
relative jumps), instruction by instruction, until a return, an
unconditional jump, an instruction that ends the thread (`sys.exit_thread`,
`sys.quit`, `sys.reboot`) or an undefined opcode - computed jumps (`load`
then `jmp`, the jump tables) lead nowhere by themselves, but their tables
are built from `push_code_off` instructions, so their targets are found
all the same.  A `push_code_off` followed by `call` names its target
`sub_`, other targets `loc_`; `push_code_addr` and tagged `push_i32`
values name data `str_` (a string lies there) or `dat_`.

The compiler leaves code nothing refers to - functions without a caller,
the tail of a function after its last jump.  A gap between what the
flows reached is taken as code when it decodes cleanly from its start to
its end (every opcode defined, the last instruction ending its flow where
the gap ends or where a real string begins); such a block is labelled
and marked `; unreferenced`.  What remains is data: strings where
NUL-terminated printable Shift-JIS lies, bytes elsewhere.  A tagged value
that points into the middle of an instruction is a constant and is
printed as a number.

The analysis never ran into an undefined opcode or an overlapping
instruction in the 1856 programs, which is some evidence that the
operand formats are complete; the 1.667+ superinstructions, which no
system program of the two 1.667+ games uses, are implemented from
`docs/versions.md` and checked only by the unit test.

## The API (inc/bgi/asm.h)

`Asm_Decode` decodes one instruction with its operands and resolved
target; `Asm_Format` writes it as the assembler reads it; `Asm_OpName` /
`Asm_LookupName` map opcodes and mnemonics for a generation;
`Dis_Program` writes the listing of a file and `Dis_Analyze` gives the
analysis behind it (the code / data map with its labels, which the
decompiler reads); `Asm_Assemble` assembles text into a file, listing
the labels it defined.  A family instruction another build defines but
the generation lacks decodes with `foreign` set (a program may carry one
in code its engine never runs); the listing notes it and the assembler
warns.  `src/core/arcread.c` reads an archive's index and entries
with stdio for the tools (the engine has its own reader on the OS layer).
