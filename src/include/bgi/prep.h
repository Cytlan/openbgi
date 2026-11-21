/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * prep.h - the preprocessor of the source languages: `#include` and
 *          `#define` for the program sources (docs/modding/programs.md) and the
 *          scenes of the 1.64 game (docs/modding/scenario.md); implemented in
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

/* Where `#include "name"` finds its files, and how: the files are the
 * caller's.  A name is looked for in the directory of the file that
 * includes it, then in `dirs` in order, and read with `read` - without it
 * (or without the struct) no file can be included.  The paths are the
 * caller's own (a host's native ones, the tests' names in memory); the
 * names in the source are UTF-8 with '/' between the directories, which
 * become `sep` and go through `fromUtf8`.  A host has these filled in for
 * its native files (OsCommon_PrepFiles, src/host/os_files.c). */
typedef struct PrepIncludes
{
	const char* baseDir;     // the directory of the source text itself (NULL: the directory part of its file name)
	const char* const* dirs; // further directories to look in
	int dirCount;
	/* the bytes of a file in a malloc'ed buffer with a NUL after them and
	 * their number in *size; NULL when there is no such file */
	uint8_t* (*read)(void* ctx, const char* path, uint32_t* size);
	void (*absolute)(void* ctx, const char* path, char* out, size_t n); // the full path (NULL: the path as it is)
	void (*fromUtf8)(void* ctx, const char* utf8, char* out, size_t n); // a name in the paths' encoding (NULL: as it is)
	char sep;                                                           // the separator of the paths besides '/' (0: none)
	int sjis;                                                           // the paths are Shift-JIS: a trail byte that looks like a separator is none
	int foldCase;                                                       // two names of one file may differ in the case of their letters (Windows)
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
