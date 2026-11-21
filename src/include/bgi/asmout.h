/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * asmout.h - where the text of the assembler, the disassembler and the
 *            compiler goes: a stream or the memory (src/asm/textout.c)
 *
 * The listings of asm.h and the messages of the assembler, the
 * preprocessor and the compiler are written either to a stdio stream
 * (the tools' files, stderr) or into a growing text in memory
 * (Dis_ProgramText: the debugger's listings; Bpc_CompileText: a compile
 * inside the engine, which uses no file for it).  An AsmOut_t with `fp`
 * set writes to the stream; a zeroed one collects the text, always
 * NUL-terminated once anything was written (free() it).  When memory
 * runs out the text keeps what it had and `failed` is set; nothing more
 * is added.
 */
#ifndef BGI_ASMOUT_H_
#define BGI_ASMOUT_H_

#include "bgi/asm.h"
#include "bgi/prep.h"

#include <stdarg.h>
#include <stdio.h>

typedef struct AsmOut
{
	FILE* fp;        // the stream; NULL: the text below
	char* text;      // the text written so far (malloc'ed, NUL-terminated)
	size_t len, cap; // its length and the size of the buffer
	int failed;      // memory ran out: the text is cut where it was
} AsmOut_t;

void AsmOut_Write(AsmOut_t* w, const char* s, size_t n);
void AsmOut_Puts(AsmOut_t* w, const char* s);
void AsmOut_Putc(AsmOut_t* w, int c);
void AsmOut_Printf(AsmOut_t* w, const char* fmt, ...)
#if defined(__GNUC__)
	__attribute__((format(printf, 2, 3)))
#endif
	;
void AsmOut_VPrintf(AsmOut_t* w, const char* fmt, va_list ap);
// Asm_WriteStringLiteral into either (strlit.c)
void AsmOut_StringLiteral(OpenBGI_t* o, AsmOut_t* w, const uint8_t* s, uint32_t n);

// Asm_Assemble and Prep_Expand with their messages to either (`log` may not be NULL)
void Asm_AssembleTo(OpenBGI_t* o, const char* text, size_t len, EngineGen_t gen, AsmOut_t* log, AsmResult_t* out);
void Prep_ExpandTo(const char* text, size_t len, const char* fileName, const PrepIncludes_t* inc, AsmOut_t* log, PrepText_t* out);

#endif // BGI_ASMOUT_H_
