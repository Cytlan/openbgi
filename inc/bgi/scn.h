/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * scn.h - the compiled scenario files of the games ("BurikoCompiledScript
 *         Ver1.00" from 1.494, the headerless files of 1.66 / 1.69 and the
 *         16-bit scenes of 1.64) and the source language they are
 *         decompiled to and compiled from; implemented in src/scn/
 *
 * A scenario file is a container (Scn_Load / Scn_Save: the imports, the
 * exported functions, a stream of 32-bit tokens and a string table) whose
 * tokens the system programs of the game interpret (scrdrv._bp runs the
 * dispatch loop, scrctrl._bp implements the base tokens 0x00..0xff and
 * scrsys / scrmsg / scrslct / scrsnd / scrgrp* the game commands from
 * 0x100).  The base tokens are a small stack machine (push, load, store,
 * arithmetic, jumps, calls); everything a game does is a command token
 * that takes its arguments from that stack.  Which commands a game has is
 * read from its sysprg.arc (Scn_ScanCommands); the names and the format
 * are described in docs/scenario.md.
 */
#ifndef BGI_SCN_H_
#define BGI_SCN_H_

#include "bgi/common.h"
#include "bgi/prep.h"

// ---- the container ------------------------------------------------------------------------------

#define SCN_MAGIC    "BurikoCompiledScriptVer1.00" // 27 characters and a NUL: the first 28 bytes
#define SCN_NAME_MAX 128                           // an import or export name, NUL included
#define SCN_IMPORTS  32                            // imports a file may list

typedef struct ScnExport
{
	char name[SCN_NAME_MAX];
	uint32_t off; // the function's offset in the token stream, bytes
} ScnExport_t;

typedef struct ScnFile
{
	char imports[SCN_IMPORTS][SCN_NAME_MAX]; // the programs loaded with this one, in file order
	int importCount;
	ScnExport_t* exports; // the exported functions, in file order (the layout order)
	int exportCount;
	uint32_t* code; // the token stream (`codeLen` dwords); string operands are byte offsets into the stream + strings
	uint32_t codeLen;
	uint8_t* strings; // the string table: NUL-terminated Shift-JIS strings, starting at byte offset codeLen * 4
	uint32_t stringsLen;
	uint32_t base;  // the file offset of the token stream (after the header), as loaded or as Scn_Save lays it out
	int raw;        // 1: the headerless layout of the 1.66 / 1.69 games - the token stream at offset 0, no imports or exports
	uint8_t* image; // a 16-bit scene of the 1.64 games (src/scn/scn16.c): the whole file, no token stream (`code` is NULL)
	uint32_t imageSize;
} ScnFile_t;

/* Parse a scenario file (`size` bytes): the Buriko container, the
 * headerless layout of the 1.66 / 1.69 games (a token stream from offset
 * 0 that starts with the frame prologue of its first function, `raw`), or
 * a 16-bit scene of the 1.64 games (kept as its `image`).  The token
 * stream ends where the first string any token refers to begins; a file
 * no token of which refers to a string is all tokens.  Returns 1 on
 * success; 0 with a message in `err` (`errSize` bytes) when the magic,
 * the header or the token stream is malformed. */
int Scn_Load(const uint8_t* data, uint32_t size, ScnFile_t* out, char* err, size_t errSize);
/* The file image of a container (BGI_Free when done), `*size` bytes: the
 * header is laid out as the original compiler does (the token stream
 * starts at a multiple of 16, with 1 to 16 bytes of padding after the
 * export list); a `raw` file is the stream and the strings alone; a
 * 16-bit scene is its image. */
uint8_t* Scn_Save(const ScnFile_t* f, uint32_t* size);
void Scn_Free(ScnFile_t* f);
// the string at byte offset `off` of the stream + strings, or NULL when it is out of range
const char* Scn_String(const ScnFile_t* f, uint32_t off);

// ---- the token set ------------------------------------------------------------------------------

/* The base tokens (scrctrl._bp).  Everything else is a command of the
 * game; the inline operands belong to the tokens below only. */
enum ScnToken
{
	SCN_PUSH = 0x00,   // v: push the immediate
	SCN_ADDR = 0x01,   // off: push the address of a code position (a label, a function)
	SCN_LOCAL = 0x02,  // n: push the address of the local at fp - n (the data stack)
	SCN_STR = 0x03,    // off: push the address of a string
	SCN_LOAD = 0x08,   // size (0 byte, else dword): pop an address, push what it holds
	SCN_STORE = 0x09,  // size: pop a value, pop an address, store (`addr value store`)
	SCN_STORE2 = 0x0a, // size: pop an address, pop a value, store (`value addr store2`: the argument pops)
	SCN_PUSHFP = 0x10, // push the frame pointer
	SCN_SETFP = 0x11,  // pop into the frame pointer
	SCN_JMP = 0x18,    // pop a code address, jump
	SCN_JCC = 0x19,    // c: pop a code address, pop a value, jump when (c == 0) == (value == 0)
	SCN_CALL = 0x1a,   // pop a code address, push a frame, call
	SCN_RET = 0x1b,    // return (ends the file when the frame stack is empty)
	SCN_CALLFN = 0x1c, // pop a string: call the exported function of that name (any loaded file)
	SCN_SETRET = 0x1d, // pop the return value
	SCN_TRY = 0x1e,    // pop a code address: the function's exit label (its exception frame)
	SCN_ENDTRY = 0x1f, // pop that frame; push the return value when one was set
	SCN_ADD = 0x20,
	SCN_SUB = 0x21,
	SCN_MUL = 0x22,
	SCN_DIV = 0x23,
	SCN_MOD = 0x24,
	SCN_AND = 0x25,
	SCN_OR = 0x26,
	SCN_XOR = 0x27,
	SCN_NOT = 0x28, // bitwise not
	SCN_SHL = 0x29,
	SCN_SHR = 0x2a, // logical
	SCN_SAR = 0x2b, // arithmetic
	SCN_EQ = 0x30,
	SCN_NE = 0x31,
	SCN_LE = 0x32,
	SCN_GE = 0x33,
	SCN_LT = 0x34,
	SCN_GT = 0x35,
	SCN_LAND = 0x38,
	SCN_LOR = 0x39,
	SCN_LNOT = 0x3a,
	SCN_ARGS = 0x3f,    // n: reverse the top n values (a left-to-right argument list becomes the command's order)
	SCN_SRCINFO = 0x7b, // 0x12345678, index, text: the message marker of the 1.640+ scenario compiler
	SCN_LINE = 0x7f,    // file, line: the source position marker of the library compiler
	SCN_RAWCALL = 0xf2, // the 1.66 / 1.69 games: pop a code address and call it (a scene calling into its game's main)
	SCN_LINEINFO = 0xfe // the source position command of the scenario compiler (file, line on the stack)
};

// the number of inline operands of a token (0 for the commands)
int Scn_OperandCount(uint32_t token);

// ---- the commands of a game ----------------------------------------------------------------------

// one command token as the game's system programs implement it
typedef struct ScnCmd
{
	uint32_t token;
	char module[16];  // the program that registers it ("scrmsg")
	char name[48];    // its name in the source language ("msg_140" when nothing better is known)
	int args;         // the arguments it pops, -1 when not known
	int result;       // 1 when it pushes a result
	uint32_t handler; // the handler's offset in the module (for the scan listing)
	char params[160]; // the parameter names the handler's range checks give (Shift-JIS, '|'-separated), or ""
} ScnCmd_t;

typedef struct ScnCmdTable
{
	ScnCmd_t* cmds;
	int count;
	uint32_t dispatch; // the address of the dispatch table in the engine's global memory
	int scenes16;      // 1: a 1.64 game, whose scenes are 16-bit opcodes with a built-in table (src/scn/scn16.c); no commands
} ScnCmdTable_t;

/* Read the commands of a game from its sysprg.arc: the handlers the
 * scenario programs register in the dispatch table and what each pops and
 * pushes.  Returns 1 on success (the table is sorted by token; empty with
 * `scenes16` set for a 1.64 game), 0 with a message on stderr when the
 * archive cannot be read or has no scenario programs. */
int Scn_ScanCommands(const char* sysprgPath, ScnCmdTable_t* out);
/* the same for an archive that is open already (it stays open); `sysprgPath`
 * is what the messages call it */
struct ArcRead;
int Scn_ScanCommandsArc(struct ArcRead* a, const char* sysprgPath, ScnCmdTable_t* out);
void Scn_FreeCommands(ScnCmdTable_t* t);
const ScnCmd_t* Scn_FindCommand(const ScnCmdTable_t* t, uint32_t token);
const ScnCmd_t* Scn_FindCommandByName(const ScnCmdTable_t* t, const char* name, size_t len);
// write the table as text ("token module name args result handler params" lines; the opcodes of a 1.64 game)
void Scn_WriteCommands(const ScnCmdTable_t* t, FILE* out);

// ---- the source language ---------------------------------------------------------------------------

// a function of an imported module: its name and parameter count (Scn_ListFunctions)
typedef struct ScnFuncInfo
{
	char name[SCN_NAME_MAX];
	int params;
} ScnFuncInfo_t;

/* The exported functions of a container with their parameter counts
 * (from the prologues), appended to `*list` (realloc'ed; `*count`
 * updated).  The decompiler uses them for the arguments of calls into
 * imported modules.  A raw file has no exports: its functions are listed
 * by address (`@0x1234`), the way the raw scenes call those of their
 * game's main file. */
void Scn_ListFunctions(const ScnFile_t* f, ScnFuncInfo_t** list, int* count);

typedef struct ScnDecOptions
{
	const ScnCmdTable_t* cmds;  // the game's commands (NULL: token names only)
	const ScnFuncInfo_t* funcs; // the functions of the imported modules (NULL: arities unknown)
	int funcCount;
	const char* name; // the file's name in the archive, for the heading and the default source name
	int rawTokens;    // 1: write the token listing (one token per line) instead of source
	int noVerify;     // 1: skip the round-trip check of each function (write source whatever it compiles to)
	int verbose;      // 1: report on stderr what made a function fall back to tokens
} ScnDecOptions_t;

/* Decompile a container to source text on `out`.  Every function (every
 * file, for the scenario style) is compiled back and compared while it is
 * written: one that does not reproduce its tokens is written as a token
 * block (`__asm { }`) instead, so that the text always compiles to the
 * original bytes.  Returns the number of such fallbacks (0: everything is
 * source).  A 16-bit scene (`image`) is written as `#style inline` source
 * (or as a listing with `rawTokens`): 0 when it reproduces the file, 1
 * when it does not (a note says so), -1 when the image does not decode. */
int Scn_Decompile(const ScnFile_t* f, const ScnDecOptions_t* opt, FILE* out);

typedef struct ScnCompileResult
{
	ScnFile_t file; // the compiled container (Scn_Free when done)
	int errors;     // messages went to `log`
	int warnings;
} ScnCompileResult_t;

/* Compile source text (`len` bytes, a NUL after them) into a container.
 * Messages go to `log` (NULL: stderr) as "<file>:<line>: error: ...".
 * `cmds` names the game's commands; a command may also be written by its
 * number (`cmd_0x140`).  `#style inline` source compiles to a 16-bit scene
 * (`file.image`); such a source goes through the preprocessor first
 * (prep.h: `#include "file"` relative to `fileName` or as `inc` says,
 * `#define`).  The other styles do not: their `#include` records a header
 * of the original source in the file.  `inc` may be NULL. */
void Scn_Compile(const char* text, size_t len, const char* fileName, const ScnCmdTable_t* cmds, const PrepIncludes_t* inc, FILE* log,
	ScnCompileResult_t* out);

#endif // BGI_SCN_H_
