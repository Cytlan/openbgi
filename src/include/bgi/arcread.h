/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * arcread.h - a stand-alone reader and writer of the game archives for
 *             the tools (implemented in src/fs/arcread.c and arcwrite.c)
 *
 * The engine reads its archives through the archive manager (src/fs/
 * arc.c) on top of its file layer (fs.h); the command-line tools (`openbgi asm`, the
 * extraction options) want the same index without an engine: this reads a
 * "PackFile    " (16-byte names, 32-byte records) or "BURIKO ARC20"
 * (1.573 on: 0x60-byte names, 0x80-byte records) archive with stdio (the
 * tools), through the host's files or through a source of the caller's,
 * and hands out its entries, decoded from DSC FORMAT 1.00 when they are.
 * Unlike the engine's index, names are kept in their stored case and
 * offsets are absolute.
 */
#ifndef BGI_ARCREAD_H_
#define BGI_ARCREAD_H_

#include "bgi/common.h"

typedef struct ArcReadEntry
{
	char name[0x61]; // Shift-JIS, as stored (not lower-cased), NUL-terminated
	uint32_t offset; // from the start of the file, bytes
	uint32_t size;   // stored size, bytes
} ArcReadEntry_t;

/* where an archive that is not a stdio stream is read from (ArcRead_OpenReader):
 * `read` fills `buf` with `n` bytes at `off` (1 when it could), `close` lets
 * the source go; both get `ctx` */
typedef struct ArcSource
{
	int (*read)(void* ctx, uint32_t off, void* buf, uint32_t n);
	void (*close)(void* ctx);
	void* ctx;
} ArcSource_t;
typedef struct ArcRead
{
	FILE* f;                 // the open archive (NULL for one read from `src`)
	ArcSource_t src;         // the archive when it is not a stdio stream
	int arc20;               // 1 for BURIKO ARC20, 0 for PackFile
	uint32_t count;          // entries
	ArcReadEntry_t* entries; // the index, in file order (count + 1 slots, the last zeroed)
} ArcRead_t;

// open and index an archive; NULL with a message on stderr when it is not one
ArcRead_t* ArcRead_Open(const char* path);
/* the same for a file the caller opened ("rb"), for a path stdio cannot
 * take as it is (the tools' OS_NativeOpen); `name` is what the
 * messages call it.  The file is the reader's from here on: ArcRead_Close
 * closes it, and so does a failure. */
ArcRead_t* ArcRead_OpenFile(FILE* f, const char* name);
// the same for a native path, read through the host's files (bgi/host.h): the engine's own way
ArcRead_t* ArcRead_OpenNative(OpenBGI_t* o, const char* path);
// the same for an archive read through a source, which is the reader's from here on as well
ArcRead_t* ArcRead_OpenReader(const ArcSource_t* src, const char* name);
void ArcRead_Close(ArcRead_t* a);
// `n` bytes at `off` of the archive; 1 when they were all read
int ArcRead_ReadAt(const ArcRead_t* a, uint32_t off, void* buf, uint32_t n);
// the entry of a name (ASCII case-insensitive), or NULL
const ArcReadEntry_t* ArcRead_Find(const ArcRead_t* a, const char* name);
/* the entry's data, decoded from DSC when stored that way (`*wasDsc` says
 * whether it was); *size receives the length.  The buffer has one spare byte
 * after the data; BGI_Free when done.  NULL on a read error. */
uint8_t* ArcRead_Load(OpenBGI_t* o, const ArcRead_t* a, const ArcReadEntry_t* e, uint32_t* size, int* wasDsc);

// ---- writing (src/fs/arcwrite.c) -------------------------------------------

// one entry to write: its name (Shift-JIS, at most 15 / 0x5f characters) and its stored bytes
typedef struct ArcWriteEntry
{
	const char* name;
	const uint8_t* data;
	uint32_t size;
} ArcWriteEntry_t;

/* The image of an archive holding `entries` in order (free() it), `*size`
 * bytes: a PackFile, or a BURIKO ARC20 when `arc20` is set.  The bytes are
 * stored as given; a name longer than the format's field is cut. */
uint8_t* ArcWrite_Build(const ArcWriteEntry_t* entries, int count, int arc20, uint32_t* size);

#endif // BGI_ARCREAD_H_
