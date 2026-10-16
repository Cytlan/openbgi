/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * scn_internal.h - shared between the decompiler (scndec.c) and the
 *                  compiler (scncomp.c) of the scenario source language
 *
 * The two agree on the shapes the original compilers give each construct
 * (docs/scenario.md); this header holds what both sides refer to: the
 * token mnemonics of the `__asm` blocks, the operator table and the two
 * compiler styles.
 */
#ifndef BGI_SCN_INTERNAL_H
#define BGI_SCN_INTERNAL_H

#include "bgi/scn.h"
#include "bgi/msg.h"

/* The style of the compiler that produced a file.  The library compiler
 * (the modules: Framework, Yuzuriha, ..) writes `line` markers, pushes
 * command arguments right to left and sorts its functions; the scenario
 * compiler (the story files) writes `lineinfo` commands, pushes arguments
 * left to right followed by `args n`, and generates a label dispatcher. */
enum ScnStyle
{
	SCN_STYLE_LIBRARY = 0,
	SCN_STYLE_SCENARIO = 1
};
/* The scenario style has an older variant (`#style scenario-line`, the
 * 1.494 game): `line` tokens as the markers, also before an `if`, `goto`
 * through the dispatcher (`str NAME; set_label; addr 0; jmp`), and the
 * message command in the args form like every other.  The decompiler and
 * the compiler carry it as a flag beside the style.
 *
 * The library style has one too (`#style library-raw`, the 1.66 / 1.69
 * games, whose files have no header): the functions are not exported (the
 * decompiler numbers them in layout order, `fn_0001`, which the name sort
 * of the layout keeps; a scene is one function `main` with no markers;
 * it calls the functions of the game's `main` file by address, `push
 * ADDR; 0xf2`, written `@0xADDR(args)`), commands take their arguments
 * left to right with `args n` when n is at least 2, and the line markers
 * name the file a function comes from (`#file`), as the includes of such
 * a library hold functions. */

// the mnemonic of a base token in `__asm` blocks and the token listing (NULL for a command)
const char* ScnTok_Name(uint32_t token);
// the token of a mnemonic (`len` bytes), or UINT32_MAX
uint32_t ScnTok_Lookup(const char* name, size_t len);

// the binary operators: token, source spelling and precedence (higher binds tighter)
typedef struct ScnOperator
{
	uint32_t token;
	const char* text;
	int prec;
} ScnOperator_t;
extern const ScnOperator_t kScnBinaryOps[];
extern const int kScnBinaryOpCount;
const ScnOperator_t* ScnOp_ByToken(uint32_t token);

// precedence levels the printer and the parser share
#define SCN_PREC_PRIMARY    15
#define SCN_PREC_UNARY      14

// the message the scenario compiler puts into every label dispatcher ("the specified label was not found")
#define SCN_LABEL_NOT_FOUND MSG_SCN_LABEL_NOT_FOUND
// the first operand of every srcinfo token
#define SCN_SRCINFO_MAGIC   0x12345678u

// the token the scenario compiler's message statement ends with
#define SCN_MSG_TOKEN       0x140

// ---- the 16-bit scenes of the 1.64 games (scn16.c) ------------------------------------------------

/* The scenes of the 1.64 games are not token streams but 16-bit opcodes
 * with inline operands (scn16.c); Scn_Load keeps such a file as its image
 * (`ScnFile_t.image`), Scn_Decompile and Scn_Compile hand it over, and
 * the source is `#style inline`. */

// an opcode: its number, its name in the source and its operand signature (the letters in scn16.c)
typedef struct Scn16Op
{
	uint16_t op;
	const char* name;
	const char* sig;
} Scn16Op_t;

#define SCN16_MAX_ARGS 72 // operands of one instruction, the counted lists included

// one decoded operand: the kind letter and the dword, or the string (pointing into the file)
typedef struct Scn16Arg
{
	char kind; // 'd' 's' 't' 'l' 'v' 'x', or 'n' for the count of a list
	uint32_t num;
	const char* str;
} Scn16Arg_t;

typedef struct Scn16Insn
{
	uint32_t off, len;
	uint16_t op;
	const Scn16Op_t* def;
	int argc;
	Scn16Arg_t args[SCN16_MAX_ARGS];
} Scn16Insn_t;

// a decoded scene: the instructions, where the code ends and where the texts begin
typedef struct Scn16Scene
{
	const uint8_t* data;
	uint32_t size;
	Scn16Insn_t* insns;
	uint32_t count, cap;
	uint32_t codeEnd, textStart;
} Scn16Scene_t;

const Scn16Op_t* Scn16_FindOp(uint32_t op);
const Scn16Op_t* Scn16_FindOpByName(const char* name, size_t len);
// the opcode table as text (what `bgiscn --scan` prints for a 1.64 game)
void Scn16_WriteOps(FILE* out);
/* Decode a whole file (Scn16_FreeScene when done).  Returns 1 when it is
 * a 16-bit scene in the layout the games' compiler wrote (every opcode
 * known, the texts from the next multiple of 16 after the code), else 0
 * with a message in `err`. */
int Scn16_Parse(const uint8_t* data, uint32_t size, Scn16Scene_t* out, char* err, size_t errSize);
void Scn16_FreeScene(Scn16Scene_t* s);
// 1 when the data is a 16-bit scene (Scn16_Parse succeeds)
int Scn16_Check(const uint8_t* data, uint32_t size);
/* Write the source of a scene (or its listing, `opt->rawTokens`), verified
 * by compiling it back unless `opt->noVerify`.  Returns 0 when the source
 * reproduces the file, 1 when it does not (it is still written, with a
 * note), -1 when the file does not decode. */
int Scn16_Decompile(const uint8_t* data, uint32_t size, const ScnDecOptions_t* opt, FILE* out);
// 1 when the text begins with `#style inline` (after comments and blank lines)
int Scn16_IsSource(const char* text, size_t len);
/* Compile `#style inline` source into a scene image (`*outData`, malloc'ed,
 * `*outSize` bytes); the text goes through the preprocessor first
 * (prep.h; `inc` says where its `#include` looks, may be NULL).  Returns
 * the number of errors, written to `log` as "<file>:<line>: error: ..."
 * (0: success). */
int Scn16_Compile(const char* text, size_t len, const char* fileName, const PrepIncludes_t* inc, FILE* log, uint8_t** outData,
	uint32_t* outSize);

#endif
