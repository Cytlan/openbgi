/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bpc.h - the decompiler and compiler of the programs ("*._bp"): the
 *         C-like source the system programs were written in, recovered
 *         from the bytecode and compiled back to it; implemented in
 *         src/bpc/ (bpdec.c, bpcomp.c)
 *
 * The programs come from one compiler whose output follows fixed
 * shapes - a frame of locals below the frame pointer, the arguments of a
 * function pushed right to left and of an engine instruction left to
 * right, conditions as chains of conditional jumps, every return through
 * one exit - and the decompiler turns those shapes back into functions,
 * declarations, expressions and the usual statements (docs/bpc.md
 * describes the source).  What it cannot express exactly, or what does
 * not come back identical when compiled, is written as an `__asm` block
 * of listing lines, so that the source always compiles to the input: the
 * compiler produces an assembler listing (docs/asm.md) and assembles it,
 * and the disassembler's analysis (Dis_Analyze, asm.h) is what the
 * decompiler starts from.
 */
#ifndef BGI_BPC_H_
#define BGI_BPC_H_

#include "bgi/asm.h"
#include "bgi/prep.h"

// what Bpc_Decompile writes (zero the struct for the defaults)
typedef struct BpcDecOptions
{
	EngineGen_t gen;  // the opcode set and pointer layout of the program (GEN_COUNT: any)
	const char* name; // the program's name for the heading (may be NULL)
	int noVerify;     // 1: skip the compile-and-compare check of every function
	int verbose;      // 1: report on stderr why a function fell back to a token block; 2: trace the statements
} BpcDecOptions_t;

/* Decompile a program file (`size` bytes, header included) into `out`.
 * Returns the number of functions written as `__asm` blocks (0: the whole
 * program is source), or -1 when the file has no usable header. */
int Bpc_Decompile(const uint8_t* file, uint32_t size, const BpcDecOptions_t* opt, FILE* out);

// what Bpc_Compile produces
typedef struct BpcResult
{
	uint8_t* file; // the program file, header included (BGI_Free when done); NULL when errors != 0
	uint32_t size; // its size in bytes
	int errors;    // messages went to `log`
	int warnings;
	AsmLabel_t* labels; // the labels of the listing the compiler assembled: every function by its name (BGI_Free when done)
	int labelCount;
} BpcResult_t;

/* Compile source text (`len` bytes, a NUL after them) for a generation
 * (`#engine NAME` in the text overrides `gen`) into *out.  The text goes
 * through the preprocessor (prep.h: `#include`, `#define`) first.
 * Messages go to `log` (NULL: stderr) as "<file>:<line>: error: ...";
 * `fileName` names the text in them and, unless `inc` (may be NULL) says
 * otherwise, is the path its `#include` lines are relative to. */
void Bpc_Compile(const char* text, size_t len, const char* fileName, EngineGen_t gen, const PrepIncludes_t* inc, FILE* log, BpcResult_t* out);

#endif // BGI_BPC_H_
