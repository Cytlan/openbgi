/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * vol_internal.h - what a volume (vol.c) and the readers of its origins
 *                   (zip.c, iso.c, udf.c, voldir.c) share: the file a volume is
 *                   read from, the list a reader fills, and the table an
 *                   origin reads its entries' bytes with
 */
#ifndef BGI_VOL_INTERNAL_H_
#define BGI_VOL_INTERNAL_H_

#include "bgi/vol.h"
#include "bgi/host.h"

#define VOL_MAX_ENTRIES 0x400000u // more entries than this are taken for a damaged directory
#define VOL_NAME_MAX    0x400     // bytes of a path

// the file a volume is read from: a native stream of the host's
typedef struct VolIn
{
	OpenBGI_t* o;
	HostFile_t* f;
} VolIn_t;

int VolIn_ReadAt(const VolIn_t* f, uint64_t off, void* buf, size_t n); // `n` bytes at `off`; 1 when they were all read
/* `n` bytes at `off` in a malloc'ed buffer with one spare NUL after them
 * (free() it); NULL when they cannot all be read or memory runs out */
uint8_t* VolIn_Load(const VolIn_t* f, uint64_t off, uint64_t n);

typedef int (*VolWrite_t)(void* ctx, const uint8_t* data, size_t n); // takes a piece of an entry's bytes; 1 when it did

/* `n` bytes from `off` on handed to `write` piece by piece; 1 when all of
 * them were read and taken */
int VolIn_Copy(const VolIn_t* f, uint64_t off, uint64_t n, VolWrite_t write, void* ctx);

// a character more of a name in UTF-8, at `off` of `out` (`cap` bytes); the new length (unchanged when it does not fit)
size_t Vol_PutUtf8(char* out, size_t off, size_t cap, uint32_t cp);
/* a name recorded in UTF-16, the high byte first (`n` bytes), as UTF-8 into
 * `out`; 0 when a character of it cannot be part of a name ('/', '\\', a
 * control character) */
int Vol_PutUtf16Be(const uint8_t* raw, size_t n, char* out, size_t cap);
/* an entry's times from a date and a time of day as recorded, `zoneMin`
 * minutes east of UTC; nothing when they are no date */
void Vol_SetTime(VolEntry_t* e, int year, int month, int day, int hour, int min, int sec, int zoneMin);

// the entries a reader found, before the volume sorts them
typedef struct VolList VolList_t;

/* An entry more: `path` is UTF-8 relative to the root, '/'-separated,
 * without "." or ".." components; the entry's own path, key and name are
 * the volume's to set, and its `file` is the list's from here on.  0 when
 * memory ran out. */
int VolList_Add(VolList_t* l, const char* path, const VolEntry_t* e);
uint32_t VolList_Count(const VolList_t* l);

// how the bytes of an origin's entries are read (Vol_Load, Vol_Extract and Vol_OpenStored, for a volume of that origin)
typedef struct VolOrigin
{
	uint8_t* (*load)(Vol_t* v, const VolEntry_t* e, size_t* size);
	int (*extract)(Vol_t* v, const VolEntry_t* e, VolWrite_t write, void* ctx);
	HostFile_t* (*openStored)(Vol_t* v, const VolEntry_t* e, uint64_t* offset);
} VolOrigin_t;

const VolIn_t* Vol_In(const Vol_t* v);  // the volume's own file (no stream in it for a folder)
HostFile_t* Vol_Reopen(const Vol_t* v); // a second stream of that file; NULL when it cannot be opened

/* The readers.  Each adds the files and folders it finds to `l` and
 * returns 1, or 0 when the file is not of its kind or cannot be used. */
int Zip_Read(const VolIn_t* f, uint64_t fileSize, VolList_t* l); // zip.c: a zip archive
int Iso_Read(const VolIn_t* f, uint64_t fileSize, VolList_t* l); // iso.c: a disc image
/* udf.c: the UDF part of a disc image, for iso.c.  1 when its files were
 * added, 0 when the image has none that can be followed (nothing was
 * added), -1 when memory ran out on the way. */
int Udf_Read(const VolIn_t* f, uint64_t fileSize, VolList_t* l);
int VolDir_Read(OpenBGI_t* o, const char* native, VolList_t* l); // voldir.c: a folder (`native` without a separator at its end)

extern const VolOrigin_t kZipOrigin;    // zip.c: stored or deflated, checked
extern const VolOrigin_t kVolDirOrigin; // voldir.c: every entry a file of its own
/* vol.c: entries that are a run of bytes of the volume's file as they
 * are, `size` of them at `data` - what a disc image's files are */
extern const VolOrigin_t kVolRangeOrigin;

#endif // BGI_VOL_INTERNAL_H_
