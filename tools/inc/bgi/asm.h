/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * asm.h - the assembler and disassembler of Buriko programs ("*._bp");
 *         implemented in src/asm/decode.c, dis.c, as.c and the generated
 *         opnames.c
 *
 * A program file is a 16-byte header (the code offset 0x10, the size of
 * what follows, two zero words) and one area holding the bytecode and the
 * data it refers to (strings, tables), all addressed by offsets from the
 * area's start (docs/vm.md).  This module turns such a file into an
 * assembler listing with labels and back:
 *
 *   * Asm_Decode: one instruction with its operands, for the listing, the
 *     debugger and the analysis;
 *   * Dis_Program: the whole file - the code is found by following the
 *     flow from the entry and every jump target, the data by what the
 *     code refers to - as text the assembler accepts;
 *   * Asm_Assemble: the text back into a file, in as many passes as the
 *     relative operands need to settle.
 *
 * Every function takes the engine generation (EngineGen_t, version.h) to
 * work under: it decides which opcodes exist, what a reused number means,
 * how the conditional jump is encoded (1.667+) and the layout of tagged
 * pointers.  GEN_COUNT stands for "any": every opcode of every build is
 * accepted and a reused number has its newest meaning.
 *
 * The listing's syntax is described in docs/asm.md.  Mnemonics come from
 * the opcode tables of the reimplementation (opnames.c, generated from
 * them); an opcode without a name is written as
 * `family.0xHH` (`sys.0x5F`), and an opcode the selected engine
 * generation does not define is still assembled, with a warning.
 */
#ifndef BGI_ASM_H_
#define BGI_ASM_H_

#include "bgi/common.h"
#include "bgi/version.h"

// ---- mnemonics --------------------------------------------------------------------------

// one entry of the mnemonic table: a name and the opcode it stands for in a range of builds
typedef struct AsmOpName
{
	uint16_t fam;         // 0 for the base instructions, else the family byte (0x80, 0x90, ..)
	uint8_t op;           // the opcode (the second byte of a family instruction)
	EngineGen_t from, to; // the generations the name applies to (a reused number has several)
	const char* name;     // the mnemonic without the family prefix ("yield", not "sys.yield")
} AsmOpName_t;
extern const AsmOpName_t kAsmOpNames[]; // the mnemonic table (opnames.c, generated)
extern const size_t kAsmOpNameCount;    // its number of entries

// the family byte's mnemonic prefix ("sys" for 0x80 ..), NULL for a byte that is not a family
const char* Asm_FamilyName(int fam);
// the family byte of a prefix (`len` bytes of `name`: "sys" → 0x80); -1 when it is not one
int Asm_FamilyByte(const char* name, size_t len);
/* The name of an opcode in a generation, NULL when it has none.  Under
 * GEN_COUNT a number that was reused gets its newest name. */
const char* Asm_OpName(int fam, int op, EngineGen_t gen);
/* The opcode of a mnemonic (`len` bytes of `name`): "push_i8", "sys.yield",
 * or the numeric forms "sys.0x5f" and "0x3c" (a bare number is a base
 * opcode and must be below 0x80).  The name is looked up in `gen` first;
 * a name only another generation knows still resolves, to its newest
 * number (the assembler then warns about the opcode).  Returns 1 with
 * `*fam` (0 for a base instruction, else the family byte) and `*op` set,
 * 0 when the name is unknown. */
int Asm_LookupName(const char* name, size_t len, EngineGen_t gen, int* fam, int* op);

// ---- stack effects --------------------------------------------------------------------------

// one entry of the stack-effect table: what an opcode pops and pushes in a range of builds
typedef struct AsmOpStack
{
	uint16_t fam;         // 0 for the base instructions, else the family byte
	uint8_t op;           // the opcode
	EngineGen_t from, to; // the generations the entry applies to
	int8_t pops;          // values taken from the evaluation stack; -1 when it depends on the values ("6F" sprintf)
	int8_t pushes;        // values left on it
} AsmOpStack_t;
extern const AsmOpStack_t kAsmOpStack[]; // the table (opstack.c, generated from the handler comments)
extern const size_t kAsmOpStackCount;

/* The stack effect of an opcode in a generation (under GEN_COUNT: the
 * newest build's).  1 with `*pops` and `*pushes` set, 0 when the table
 * has no entry (an opcode the reimplementation does not have). */
int Asm_StackEffect(int fam, int op, EngineGen_t gen, int* pops, int* pushes);

// ---- one instruction ------------------------------------------------------------------------

/* The operand kinds of an instruction; `AsmInsn_t.kinds` lists them in
 * order.  All operands sit inline in the code after the opcode byte(s) -
 * what an instruction takes from the evaluation stack is not an operand
 * here - and the family instructions have none.  The opcodes in quotes are
 * the base instructions that use each kind. */
enum AsmOperandKind
{
	ASM_OPD_NONE = 0, // an empty slot
	ASM_OPD_IMM8,     // s8 ("00")
	ASM_OPD_IMM16,    // s16 ("01")
	ASM_OPD_IMM32,    // u32 ("02", "18")
	ASM_OPD_LOCAL,    // u16: the offset below the frame pointer ("04")
	ASM_OPD_DATAREL,  // s16 from the opcode's own offset: a data address ("05")
	ASM_OPD_CODEREL,  // s16 from the opcode's own offset: a code target ("06")
	ASM_OPD_SIZE,     // u8 size code 0 / 1 / 2 ("08" .. "0A", the first of "0C")
	ASM_OPD_BYTES,    // u8 count and that many bytes ("0B")
	ASM_OPD_COUNT,    // u8 count (the second of "0C")
	ASM_OPD_COND,     // u8 condition ("15", "3B")
	ASM_OPD_LOCAL16,  // 1.667+: bits 0..13 the offset below the frame pointer, 14..15 the size code
	ASM_OPD_SLEB,     // 1.667+: a signed LEB128 immediate
	ASM_OPD_SLEBREL,  // 1.667+: a signed LEB128 offset from the end of the instruction: a code target ("12")
	ASM_OPD_REL16     // 1.667+: s16 from the opcode's own offset: a code target ("3B", "15" with cc & 8)
};

#define ASM_MAX_OPERANDS 3 // no instruction has more ("1A" / "1B": a local reference and an immediate)

// one decoded instruction (Asm_Decode)
typedef struct AsmInsn
{
	uint32_t off;                    // the instruction's offset in the area
	uint32_t len;                    // its length in bytes, operands included
	int fam;                         // 0 for a base instruction, else the family byte
	int op;                          // the opcode
	int count;                       // the number of operands in `kinds` and `v`
	uint8_t kinds[ASM_MAX_OPERANDS]; // the kind of each operand (enum AsmOperandKind)
	int64_t v[ASM_MAX_OPERANDS];     // the operand values as read (sign-extended where signed; BYTES: the count)
	uint32_t target;                 // the resolved offset of a CODEREL / DATAREL / SLEBREL / REL16 operand
	int targetKind;                  // 0 none, 1 code, 2 data
	const uint8_t* bytes;            // ASM_OPD_BYTES: the inline bytes (a pointer into the area)
	int foreign;                     // 1: a family instruction another build defines, not this generation
} AsmInsn_t;

/* Decode the instruction at `off` of an area of `size` bytes for a
 * generation into `*out` (cleared first).  1 on success; 0 when the opcode
 * is undefined in every build (or in that generation, for a base opcode -
 * whose operands may differ between builds; a family instruction another
 * build defines decodes with `foreign` set, as a program may carry one in
 * code its engine never runs) or the instruction runs past the end of the
 * area.  The targets of relative operands are resolved to area offsets
 * but not range checked. */
int Asm_Decode(const uint8_t* area, uint32_t size, uint32_t off, EngineGen_t gen, AsmInsn_t* out);
// the length in bytes of the instruction at `off`; 0 when it does not decode
uint32_t Asm_InsnLength(const uint8_t* area, uint32_t size, uint32_t off, EngineGen_t gen);

/* Format one decoded instruction as the assembler would read it, without
 * a label for its targets (the listing substitutes them; a target is
 * written as its offset): "push_i8 -1", "sys.yield", "store_multi 2, 3".
 * An opcode whose name would not assemble back to the same number under
 * `gen` is written numerically ("gfx0.0x8b", "0x73").  Writes at most
 * `n` bytes into `buf` (truncating like snprintf) and returns the length
 * of the full text. */
size_t Asm_Format(const AsmInsn_t* in, EngineGen_t gen, char* buf, size_t n);

// ---- string literals (strlit.c) ------------------------------------------------------------------

/* The encoding of the strings in a compiled file.  A source is always
 * UTF-8 text; normally a literal is converted to Shift-JIS (code page
 * 932), which is what every engine before 1.653 expects.  For a game that
 * runs with UTF-8 text ("81 00" of 1.653 on, or the OpenBGI instruction
 * "EF 02" in any generation) the literals are kept as they are instead:
 * the tools' `-u` option sets that as the default, a `#encoding utf-8`
 * (or `#encoding sjis`) line in a source sets it from that line on. */
extern int gAsmUtf8;                         // the literals are kept in UTF-8 right now
void Asm_SetDefaultStringEncoding(int utf8); // the tools' -u option
void Asm_ResetStringEncoding(void);          // back to the default: at the start of every file compiled or decompiled
/* the argument of an `#encoding` line: `p` is just behind the word
 * "encoding"; the name up to the end of the line is taken ("utf-8" /
 * "utf8", "sjis" / "shift-jis" / "cp932") and the encoding switched.
 * Returns where the lexer goes on; *ok is 0 for an unknown name. */
const char* Asm_EncodingDirective(const char* p, const char* end, int* ok);

/* Write `n` bytes of Shift-JIS as a quoted literal: UTF-8 text with the
 * escapes \n \r \t \\ \" and \xNN, chosen so that Asm_ReadStringLiteral
 * gets the same bytes back (non-ASCII bytes are written as \xNN when the
 * UTF-8 conversion would not be exact).  With gAsmUtf8 the bytes are
 * UTF-8 already and written as they are (\xNN when they are not valid). */
void Asm_WriteStringLiteral(FILE* out, const uint8_t* s, uint32_t n);
/* Read the literal at `p` (its opening quote; the text ends at `end`) into
 * a malloc'ed buffer (`*outBytes`, `*outLen` bytes, no NUL added) and set
 * `*next` past the closing quote.  NULL on success, else the error text. */
const char* Asm_ReadStringLiteral(const char* p, const char* end, uint8_t** outBytes, uint32_t* outLen, const char** next);

// ---- the disassembler -----------------------------------------------------------------------

// what Dis_Program writes (zero the struct for the defaults)
typedef struct DisOptions
{
	EngineGen_t gen;  // the opcode set and pointer layout to read the program with (GEN_COUNT: any)
	int showBytes;    // 1: the instruction bytes in a column before the mnemonic
	int showOffsets;  // 1: the offset of every line in a column
	const char* name; // the program's name for the heading (may be NULL)
} DisOptions_t;

/* The analysis behind the listing, for the decompiler (bpc.h): the code
 * area of a program file with a classification of every byte and the
 * labels the listing would give.  Dis_FreeMap releases the maps. */
#define DIS_K_CODE   1 // kind: the first byte of an instruction
#define DIS_K_INSIDE 2 // kind: a later byte of an instruction
#define DIS_K_DATA   3 // kind: data (nothing the flows reached)
#define DIS_L_JUMP   1 // label: a branch target ("loc_")
#define DIS_L_CALL   2 // label: a call target ("sub_")
#define DIS_L_DATA   4 // label: referenced data ("str_" / "dat_")
#define DIS_L_DEAD   8 // label: a block of code nothing refers to

typedef struct DisMap
{
	const uint8_t* area; // the code area (the file after its 16-byte header)
	uint32_t size;       // its size in bytes
	uint8_t* kind;       // per byte: DIS_K_* (one entry more than the area)
	uint8_t* label;      // per byte: DIS_L_* bits
	int problems;        // the problems the analysis found (undefined opcodes, overlapping flows)
} DisMap_t;

/* Analyse a program file (`size` bytes, header included) for a
 * generation; problems are written to `log` (may be NULL) and counted.
 * Returns 1 with the maps in *out, 0 when the header is unusable. */
int Dis_Analyze(const uint8_t* file, uint32_t size, EngineGen_t gen, FILE* log, DisMap_t* out);
void Dis_FreeMap(DisMap_t* m);
/* Write the listing of the bytes [start, end) of an analysed area: labels,
 * instructions and data lines as Dis_Program writes them (`opt` may be
 * NULL for the plain form). */
void Dis_WriteRange(const DisMap_t* m, uint32_t start, uint32_t end, EngineGen_t gen, const DisOptions_t* opt, FILE* out);
// the label of an offset ("sub_0123", "loc_0123", "str_0123", "dat_0123") as the listing names it
void Dis_LabelName(const DisMap_t* m, uint32_t off, char* buf, size_t n);
// the length of the string at a data offset (without its NUL), 0 when no string lies there
uint32_t Dis_StringLength(const DisMap_t* m, uint32_t off);

/* Disassemble a program file (`size` bytes, header included) into `out`.
 * Returns 0 on success, else the number of problems found (an unreadable
 * header, code that runs into an undefined opcode or into another
 * instruction); the listing is written either way with the problems noted
 * in it as "; problem: ..." lines, and still assembles back to the same
 * bytes.  The columns of showBytes / showOffsets end with '|', which the
 * assembler skips. */
int Dis_Program(const uint8_t* file, uint32_t size, const DisOptions_t* opt, FILE* out);

// ---- the assembler ----------------------------------------------------------------------------

// a defined label of an assembled listing and its offset in the area
typedef struct AsmLabel
{
	char name[64];
	uint32_t off;
} AsmLabel_t;

// what Asm_Assemble produces
typedef struct AsmResult
{
	uint8_t* file;      // the program file, header included (BGI_Free when done); NULL when errors != 0
	uint32_t size;      // its size in bytes
	int errors;         // the number of errors (each written to `log` with its line number)
	int warnings;       // the number of warnings: opcodes outside the generation's set, and the like
	AsmLabel_t* labels; // the labels defined, with their offsets (BGI_Free when done); NULL when errors != 0
	int labelCount;
} AsmResult_t;

/* Assemble `text` (`len` bytes of listing, docs/asm.md) for a generation
 * into `*out`; `.engine NAME` in the text overrides `gen`, and under
 * GEN_COUNT every opcode of every build is accepted.  Messages go to
 * `log` (NULL: stderr) as "<line>: error: ..." / "<line>: warning: ...".
 * Parsing continues past an error so that one run reports every syntax
 * error; the encoding (operand ranges, undefined labels) is attempted
 * only when the whole text parsed. */
void Asm_Assemble(const char* text, size_t len, EngineGen_t gen, FILE* log, AsmResult_t* out);

#endif // BGI_ASM_H_
