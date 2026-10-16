/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * prep.h - the preprocessor of the source languages: `#include` and
 *          `#define` for the program sources (docs/bpc.md) and the
 *          scenes of the 1.64 game (docs/scenario.md); implemented in
 *          src/asm/prep.c
 *
 * A compiler hands its source text to Prep_Expand and compiles what comes
 * back: the text with every included file in the place of its `#include`
 * line and every defined name replaced by what it stands for.  The
 * preprocessor works on the text, not on a compiler's tokens, so both
 * languages share it; it knows only what the two have in common - string
 * and character literals and the two comment forms, inside which nothing
 * is replaced - and leaves every other `#` line to the compiler.
 *
 * The lines of a source stay lines: a directive becomes an empty line
 * and a replacement stays on the line of the name.  Prep_Where tells for
 * a line of the expanded text which file it came from and which line of
 * it, which is how a compiler's messages name the right place.
 *
 *   #include "name"    the file, looked for beside the file that includes
 *                      it and then in the directories of PrepIncludes_t;
 *                      a file is read once however often it is named
 *   #define NAME text  NAME stands for the rest of the line (without its
 *                      comment) from here on; the text is read again where
 *                      the name is used, so it may hold other names
 *   #undef NAME        NAME is an ordinary word again
 */
#ifndef BGI_PREP_H_
#define BGI_PREP_H_

#include "bgi/common.h"

/* Where `#include "name"` finds its files (all of it optional: NULL in
 * place of the struct, or a zeroed one, looks beside the source only).  A
 * name is looked for in the directory of the file that includes it, then
 * in `dirs` in order.  The paths are native ones (os.h); the names in the
 * source are UTF-8 with '/' between the directories. */
typedef struct PrepIncludes
{
	const char* baseDir;     // the directory of the source text itself (NULL: the directory part of its file name)
	const char* const* dirs; // further directories to look in
	int dirCount;
	/* reads a file instead of OS_ReadNativeFile (the tests keep theirs in
	 * memory): the bytes in a malloc'ed buffer with a NUL after them and
	 * their number in *size, NULL when there is no such file */
	uint8_t* (*read)(void* ctx, const char* path, uint32_t* size);
	void* ctx;
} PrepIncludes_t;

// where a line of the expanded text came from
typedef struct PrepLine
{
	int file; // the index into PrepText_t.files
	int line; // the line there, from 1
} PrepLine_t;

// what Prep_Expand hands back (Prep_Free when the compiler is done with it)
typedef struct PrepText
{
	const char* text; // what to compile: `len` bytes with a NUL after them
	size_t len;
	int errors; // messages went to the log: `text` is not worth compiling
	int warnings;
	// the rest belongs to the preprocessor
	const char* name;  // the source's name as it was given
	char* expanded;    // the buffer behind `text`, NULL when the source needed no change and `text` is the source itself
	PrepLine_t* lines; // one per line of the expanded text
	int lineCount;
	char** files; // the names the messages use; files[0] is the source
	int fileCount;
} PrepText_t;

/* Expand `text` (`len` bytes; a NUL has to follow them) into *out.
 * `fileName` names the text in the messages - which go to `log` (NULL:
 * stderr) as "<file>:<line>: error: ..." - and is, unless `inc` says
 * otherwise, the path its `#include` lines are relative to.  A source
 * without a directive of the preprocessor comes back as it is, at no
 * cost.  A byte order mark at the start of a file is dropped. */
void Prep_Expand(const char* text, size_t len, const char* fileName, const PrepIncludes_t* inc, FILE* log, PrepText_t* out);

/* The file and the line that line `line` (from 1) of the expanded text
 * came from: the file's name is returned, the line stored in *srcLine. */
const char* Prep_Where(const PrepText_t* t, int line, int* srcLine);

void Prep_Free(PrepText_t* t);

#endif // BGI_PREP_H_
