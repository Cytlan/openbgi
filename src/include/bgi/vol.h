/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * vol.h - a volume: files of some origin as one read-only tree (vol.c),
 *          for the games played from an archive, a disc image or a folder
 *          and for the patches laid over them (fs.h); not part of the
 *          original engine
 *
 * Three origins are read, each by a reader of its own that knows nothing
 * of the others:
 *
 *   * a zip archive (zip.c): PKWARE's format with zip64, entries stored
 *     or deflated and checked against their CRC-32 when they are read;
 *   * a disc image (iso.c, udf.c): ISO 9660 with or without Joliet
 *     names, or UDF, in 2048-byte sectors, whose files are read where
 *     they lie;
 *   * a folder (voldir.c): its files and sub-folders as they are when it
 *     is opened, each file read from itself.
 *
 * Whatever the origin, the entries are a tree of paths relative to the
 * volume's root:
 *
 *   * names are UTF-8, '/' separates the components, and there are no
 *     "." or ".." components (each reader says how it gets there);
 *   * every folder a path implies is an entry of its own;
 *   * the lookups ignore the case of ASCII letters: each entry has a key,
 *     its path with those letters in lower case, and the entries are sorted
 *     by it (byte order).  Two names that differ only in case leave the one
 *     found last;
 *   * in an archive or an image that holds a game (Vol_Open), a single
 *     folder at the top that holds everything is the root, and when the
 *     root is not a game's but a folder below it is, that folder is.
 */
#ifndef BGI_VOL_H_
#define BGI_VOL_H_

#include "bgi/common.h"

typedef struct VolEntry
{
	const char* path; // UTF-8 relative to the root, '/'-separated, no trailing '/'; "" never occurs
	const char* key;  // the path with ASCII letters in lower case
	const char* name; // the last component of `path`
	uint64_t size;    // bytes
	uint32_t dosTime; // last modification: the MS-DOS date in the high half, the time in the low half (local time)
	uint64_t mtime;   // the same as a FILETIME (UTC) when the origin has one, else 0
	uint32_t check;   // a check value of the bytes where the origin has one (a zip's CRC-32), else 0
	uint8_t isDir;    // a folder (found as one, or implied by a path)
	uint8_t readable; // its bytes can be read (a zip's entry that is encrypted or compressed some other way cannot)
	uint8_t stored;   // .. and lie in a file as they are: Vol_OpenStored reads them in place
	char* file;       // a folder's file: its own native path; NULL for every other origin
	struct Vol* from; // an entry of a volume laid over the one it is listed in (Vol_Overlay): that volume; NULL otherwise
	// the origin's own: where and how the bytes are kept
	uint64_t csize;  // bytes as they are kept
	uint64_t local;  // a zip: the offset of the entry's local header
	uint64_t data;   // the offset of the kept bytes in the volume's file (a zip's is found when it is first wanted)
	uint16_t method; // a zip: 0 stored, 8 deflated, anything else not readable
} VolEntry_t;

typedef struct Vol Vol_t;

/* Open what is at a native path, through the host's files (bgi/host.h):
 * a folder, a zip archive or a disc image.  NULL when it cannot be read
 * or is none of them (a zip spanning several disks, say). */
Vol_t* Vol_Open(OpenBGI_t* o, const char* nativePath);
/* The same for a volume that is no game but files for one (a patch): its
 * root is taken as it is - no folder at its top is made the root, and no
 * game is looked for in it. */
Vol_t* Vol_OpenPlain(OpenBGI_t* o, const char* nativePath);
/* Lay the volume `over` on `v`: from then on v lists the entries of both,
 * and where both have a file of a key it is over's (a folder of v's stays
 * a folder).  When all of over's entries are in one folder at its top
 * and none of its files meets a file of v's that way, while some do
 * without that folder, the folder is left out of their paths (a patch
 * zipped as a folder).  Several may be laid on, the later over the
 * earlier; `over` is v's then, closed with it.  It is done before the
 * entries are used: the ones handed out before are gone.  0 when memory
 * ran out (`over` is still the caller's). */
int Vol_Overlay(Vol_t* v, Vol_t* over);
void Vol_Close(Vol_t* v);                             // accepts NULL
const char* Vol_Path(const Vol_t* v);                 // the native path it was opened from
uint32_t Vol_Count(const Vol_t* v);                   // the entries, folders included
const VolEntry_t* Vol_At(const Vol_t* v, uint32_t i); // by index, in key order
/* the entry of a key (lower case, as VolEntry_t.key), NULL when there is
 * none; "" is the root, which is no entry */
const VolEntry_t* Vol_Find(const Vol_t* v, const char* key);
/* the index of the first entry whose key is not below `key` (Vol_Count
 * when there is none): the entries under a folder "a/b" are the run from
 * Vol_Lower(v, "a/b/") whose keys start with "a/b/" */
uint32_t Vol_Lower(const Vol_t* v, const char* key);

/* An entry's bytes, checked where the origin can check them, in a
 * malloc'ed buffer (one spare NUL after them; free() it), *size bytes.
 * NULL when the entry is a folder or not readable, its bytes are damaged
 * or memory runs out. */
uint8_t* Vol_Load(Vol_t* v, const VolEntry_t* e, size_t* size);
/* The same, handed to `write` piece by piece (1 when it took them), so an
 * entry of any size needs little memory.  1 when the whole entry was
 * written and its check value, if it has one, is right. */
int Vol_Extract(Vol_t* v, const VolEntry_t* e, int (*write)(void* ctx, const uint8_t* data, size_t n), void* ctx);
/* A stored entry read in place: a native stream of its own (Host_FileClose
 * it), with *offset the position of the entry's first byte in it.  NULL
 * when the entry is not stored or the file cannot be opened. */
struct HostFile* Vol_OpenStored(Vol_t* v, const VolEntry_t* e, uint64_t* offset);

#endif // BGI_VOL_H_
