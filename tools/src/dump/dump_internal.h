/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dump_internal.h - what the files of src/dump/ share: the text buffer
 *                   the description is written with, the file helpers,
 *                   the two contexts and the converters of each kind of
 *                   entry (interface of the module: inc/bgi/dump.h)
 *
 * dump.c and pack.c are the two drivers.  Every kind of entry has its
 * pair of converters in one file - image.c, sound.c, movie.c, script.c
 * (programs and scenario files), table.c (the tables, the SDC layer and
 * plain bytes): Dump_<Kind> turns the entry's data into files and
 * describes them in the entry's JSON object, Pack_<Kind> makes the data
 * again from that object and the files.
 *
 * Paths: a "native" path (os.h) names a place on the host; the files of an
 * entry are recorded relative to the archive's directory in UTF-8 with '/'
 * between the components, and DumpFs_Join makes a native path of the two.
 */
#ifndef BGI_DUMP_INTERNAL_H_
#define BGI_DUMP_INTERNAL_H_

#include "bgi/dump.h"
#include "bgi/arcread.h"
#include "bgi/json.h"
#include "bgi/scn.h"
#include "bgi/version.h"
#include "bgi/deflate.h"
#include "bgi/os.h"
#include "bgi/os_common.h"

#define DUMP_PATH_MAX 0x1000 // a native path
#define DUMP_NAME_MAX 0x200  // an entry name or a file name in UTF-8

// ---- util.c: text, files, names ------------------------------------------------------------------

// a growing string, for the JSON the tools write
typedef struct DumpText
{
	char* p; // NUL-terminated once anything was added (BGI_Free via DumpText_Free)
	size_t len, cap;
} DumpText_t;

void DumpText_Add(DumpText_t* t, const char* s);
void DumpText_AddN(DumpText_t* t, const char* s, size_t n);
void DumpText_Printf(DumpText_t* t, const char* fmt, ...);
void DumpText_AddString(DumpText_t* t, const char* utf8); // as a JSON string, quotes included
void DumpText_Free(DumpText_t* t);

/* `dir` (native) and `rel` (UTF-8, '/' between components; "" for the
 * directory itself) as one native path */
void DumpFs_Join(char* out, size_t n, const char* dir, const char* rel);
// the whole file (free() it; a NUL follows the data), NULL when it cannot be read or is 4 GB or more
uint8_t* DumpFs_Read(const char* path, uint32_t* size);
int DumpFs_Write(const char* path, const void* data, size_t size); // 1 when written
// the CRC-32 of a file read in pieces; 0 with *ok = 0 when it cannot be read
uint32_t DumpFs_Crc(const char* path, int* ok);

void Dump_Hex(char* out, const uint8_t* data, size_t n);    // 2 n characters and a NUL
int Dump_Unhex(const char* hex, uint8_t* out, size_t cap);  // the bytes written, -1 when it is not hex or too long
int Dump_EqualNoCase(const char* a, const char* b);         // ASCII letters compared without regard to case
int Dump_EndsWithNoCase(const char* s, const char* suffix); // likewise
/* a file name for the entry name `utf8`: the characters a file system may
 * refuse ('<' '>' ':' '"' '/' '\\' '|' '?' '*', control characters, a dot
 * or blank at the end) become '_', and a reserved device name of Windows
 * gets one in front */
void Dump_SafeName(const char* utf8, char* out, size_t n);
/* 1 when `rel` is a path a description may name: relative, without a drive,
 * a backslash or a ".." component - nothing that leads out of the dump */
int Dump_IsInside(const char* rel);
// 1 when the name is one of `names` (ASCII case-insensitive) or the list is empty
int Dump_Wanted(const char* name, const char* const* names, int count);

// ---- the kinds of entries ------------------------------------------------------------------------

typedef enum DumpKind
{
	DUMP_DATA,     // bytes as they are
	DUMP_PROGRAM,  // a "*._bp" program -> .bpc source
	DUMP_SCENARIO, // a scenario file -> .bsc source
	DUMP_IMAGE,    // a raw or CompressedBG image -> .png
	DUMP_SOUND,    // a BW sound -> .ogg / .wav
	DUMP_MOVIE,    // a BF_Movie -> a directory of .png frames
	DUMP_TABLE,    // StringsDB, ScenarioDB, BitmapNameDB -> .json
	DUMP_SDC,      // an SDC-coded block -> .bin
	DUMP_KIND_COUNT
} DumpKind_t;

extern const char* const kDumpKindNames[DUMP_KIND_COUNT]; // the "kind" member: "data", "program", ...

// ---- dumping ---------------------------------------------------------------------------------------

// the functions of a scenario file other files call into, kept per archive (script.c)
typedef struct DumpFuncCache
{
	char name[SCN_NAME_MAX]; // the entry they were read from
	ScnFuncInfo_t* funcs;
	int count;
} DumpFuncCache_t;

typedef struct DumpCtx
{
	const DumpOptions_t* opt;
	FILE* log;
	EngineGen_t gen;           // the game's generation (GEN_COUNT: unknown, every opcode of every build)
	const ScnCmdTable_t* cmds; // the game's scenario commands (NULL: none found)
	int pngLevel;
	FILE* scratch; // a temporary file the decompilers write into
	FILE* sink;    // one that swallows the compilers' messages

	// the archive being dumped
	ArcRead_t* arc;
	const char* arcName;        // UTF-8, for the messages
	char arcDir[DUMP_PATH_MAX]; // its directory in the dump (native)
	DumpFuncCache_t* funcCache;
	int funcCacheCount;

	// the entry being dumped
	const char* entryName; // UTF-8
	const char* base;      // the stem of its file names (UTF-8)
	DumpText_t* meta;      // the members of its JSON object, each written as `, "key": value`
	int failed;            // a file could not be written: the dump stops
	uint32_t problems;     // entries that should have converted and did not, scripts that do not round-trip
} DumpCtx_t;

/* Write `size` bytes as the file `rel` of the archive's directory and
 * describe it in the entry's object: `, "key": {"path": ..}`, or with a
 * NULL key the object alone (an array element).  Returns 1 when written;
 * 0 sets c->failed. */
int Dump_EmitFile(DumpCtx_t* c, const char* key, const char* rel, const void* data, size_t size);
// create the directory `rel` of the archive's directory (for an entry that becomes several files); 0 sets c->failed
int Dump_MakeDir(DumpCtx_t* c, const char* rel);
// a message about the entry on the log: "<archive>/<entry>: ..."
void Dump_Note(DumpCtx_t* c, const char* fmt, ...);

/* The converters.  Each looks at the entry's data (after the DSC layer):
 * when it is theirs and converts, they write its files, add the kind's
 * members to the object and return 1; they return 0 with nothing written
 * when it is not theirs or does not convert (the caller then tries the
 * next kind, ending with Dump_Data). */
int Dump_Image(DumpCtx_t* c, const uint8_t* data, uint32_t size);
int Dump_Sound(DumpCtx_t* c, const uint8_t* data, uint32_t size);
int Dump_Movie(DumpCtx_t* c, const uint8_t* data, uint32_t size);
int Dump_Program(DumpCtx_t* c, const uint8_t* data, uint32_t size);
int Dump_Scenario(DumpCtx_t* c, const uint8_t* data, uint32_t size);
int Dump_Table(DumpCtx_t* c, const uint8_t* data, uint32_t size);
int Dump_Sdc(DumpCtx_t* c, const uint8_t* data, uint32_t size);
int Dump_Data(DumpCtx_t* c, const uint8_t* data, uint32_t size);
void Dump_FreeFuncCache(DumpCtx_t* c); // at the end of an archive

// ---- packing ---------------------------------------------------------------------------------------

typedef struct PackCtx
{
	const PackOptions_t* opt;
	FILE* log;
	EngineGen_t gen;    // the generation the dump recorded (GEN_COUNT: unknown)
	ScnCmdTable_t cmds; // the scenario commands the dump recorded
	int haveCmds;

	const char* arcName;        // the archive being built (UTF-8)
	char arcDir[DUMP_PATH_MAX]; // its directory in the dump (native)
	const char* entryName;      // the entry being built (UTF-8)
} PackCtx_t;

// a message about the entry on the log: "<archive>/<entry>: ..."
void Pack_Note(PackCtx_t* c, const char* fmt, ...);
/* the file the member `key` of the entry describes (free() it; a NUL
 * follows the data); NULL with a message when the member or the file is
 * missing */
uint8_t* Pack_ReadMember(PackCtx_t* c, const JsonNode_t* entry, const char* key, uint32_t* size);
// the same for a file object itself (an element of "frames")
uint8_t* Pack_ReadFileObject(PackCtx_t* c, const JsonNode_t* file, uint32_t* size);

/* The converters back: the entry's data as the engine reads it after the
 * DSC layer (BGI_Free it), `*size` bytes, from the entry's object and its
 * files; NULL with a message when the files do not build. */
uint8_t* Pack_Image(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size);
uint8_t* Pack_Sound(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size);
uint8_t* Pack_Movie(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size);
uint8_t* Pack_Program(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size);
uint8_t* Pack_Scenario(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size);
uint8_t* Pack_Table(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size);
uint8_t* Pack_Sdc(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size);
uint8_t* Pack_Data(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size);

/* the table converters work on memory, so that the dump can check that a
 * table's JSON makes the same bytes again before it relies on it (table.c) */
uint8_t* DumpTable_FromJson(const char* text, size_t len, uint32_t* size, char* err, size_t errSize);

#endif // BGI_DUMP_INTERNAL_H_
