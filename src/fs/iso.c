/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * iso.c - disc images, for the volumes (src/include/bgi/vol.h): the format alone;
 *         not part of the original engine
 *
 * An image of a data disc in sectors of 2048 bytes, as a plain ".iso"
 * file holds it.  Its files may be listed up to three times, and the
 * first of these that is there is taken:
 *
 *   - the Joliet tree of ISO 9660 (names of up to 64 UCS-2 characters, as
 *     written on Windows);
 *   - the UDF part (udf.c), which a disc may have beside ISO 9660 or
 *     alone;
 *   - the primary tree of ISO 9660 (short names, taken as Shift-JIS,
 *     without their ";1" version).
 *
 * The ISO 9660 volume descriptors start at sector 16.  Every file becomes
 * a stored entry at its place in the image, so the engine reads it there
 * without a copy; an image has no check values.
 *
 * Not read: files recorded interleaved or in extents that do not follow
 * each other (listed, not readable), the long names of Rock Ridge (such an
 * image shows its short names), and images with a header before the first
 * sector or with raw 2352-byte sectors.
 */
#include "vol_internal.h"

#include "bgi/bytes.h"
#include "bgi/sjis.h"

#define SECTOR     2048u
#define FIRST_DESC 16u        // the sector of the first volume descriptor
#define MAX_DESCS  64u        // volume descriptors looked at
#define MAX_DEPTH  32         // folders inside folders (the standard allows 8)
#define MAX_DIR    0x4000000u // bytes of one directory (64 MB)
#define FLAG_DIR   0x02u
#define FLAG_ASSOC 0x04u // an associated file (a resource fork): not the file itself
#define FLAG_MORE  0x80u // not the file's last extent

// the walk's state
typedef struct Iso
{
	const VolIn_t* f;
	uint64_t fileSize;
	VolList_t* l;
	int joliet;    // the names are UCS-2, big-endian
	uint32_t dirs; // directories read so far
	int ok;        // 0 once memory ran out or the tree is damaged beyond use
} Iso_t;

/* a record's name as one UTF-8 path component; 0 when nothing usable is
 * left of it ('/', '\\' and control characters cannot be part of a name;
 * "." and ".." are no names) */
static int DecodeName(const Iso_t* w, const uint8_t* raw, size_t n, int isDir, char* out, size_t cap)
{
	size_t off = 0, i;
	char* semi;
	if(w->joliet)
	{
		if(!Vol_PutUtf16Be(raw, n, out, cap))
			return 0;
	}
	else
	{
		char sjis[0x100];
		if(n >= sizeof sjis)
			return 0;
		for(i = 0; i < n; i++)
			if(raw[i] < 0x20 || raw[i] == '/')
				return 0;
		memcpy(sjis, raw, n);
		sjis[n] = 0;
		Sjis_ToUtf8(sjis, out, cap, 0);
		if(strchr(out, '\\'))
			return 0;
	}
	if(!isDir)
	{
		semi = strrchr(out, ';'); // the version: ";1"
		if(semi && semi[1] && strspn(semi + 1, "0123456789") == strlen(semi + 1))
			*semi = 0;
		off = strlen(out);
		if(off > 1 && out[off - 1] == '.' && !w->joliet) // a name without an extension is recorded as "NAME."
			out[off - 1] = 0;
	}
	return out[0] && strcmp(out, ".") != 0 && strcmp(out, "..") != 0;
}

// the recording time of a record (7 bytes: years since 1900, month, day, hour, minute, second, quarter hours east of UTC)
static void RecordTime(const uint8_t* t, VolEntry_t* e)
{
	Vol_SetTime(e, 1900 + t[0], t[1], t[2], t[3], t[4], t[5], (int8_t)t[6] * 15);
}

static void WalkDir(Iso_t* w, uint32_t lba, uint32_t size, const char* prefix, int depth);

// what one record names: a folder to walk, or a file's (first) extent
static void TakeRecord(Iso_t* w, const uint8_t* r, const char* prefix, int depth, VolEntry_t* pending, char* pendingPath,
	uint64_t* pendingNext)
{
	uint32_t lba = Le32(r + 2), size = Le32(r + 10);
	unsigned flags = r[25], nameLen = r[32];
	char name[VOL_NAME_MAX], path[VOL_NAME_MAX];
	int isDir = (flags & FLAG_DIR) != 0;
	if(nameLen == 1 && (r[33] == 0 || r[33] == 1)) // the folder itself, its parent
		return;
	if(flags & FLAG_ASSOC)
		return;
	if(!DecodeName(w, r + 33, nameLen, isDir, name, sizeof name))
		return;
	if((size_t)snprintf(path, sizeof path, "%s%s%s", prefix, prefix[0] ? "/" : "", name) >= sizeof path)
		return;
	if(isDir)
	{
		VolEntry_t e;
		memset(&e, 0, sizeof e);
		e.isDir = 1;
		RecordTime(r + 18, &e);
		if(!VolList_Add(w->l, path, &e))
			w->ok = 0;
		else
			WalkDir(w, lba, size, path, depth + 1);
		return;
	}
	if(pendingPath[0] && strcmp(pendingPath, path) == 0)
	{ // a further extent of the file before: readable in place only when it follows the one before
		if((uint64_t)lba * SECTOR != *pendingNext)
			pending->readable = 0;
		pending->size += size;
	}
	else
	{
		memset(pending, 0, sizeof *pending);
		snprintf(pendingPath, VOL_NAME_MAX, "%s", path);
		pending->size = size;
		pending->data = (uint64_t)(lba ? lba : FIRST_DESC) * SECTOR; // (an empty file may have no extent: any place will do)
		pending->readable = !(r[26] || r[27]);                       // not interleaved
		pending->stored = 1;
		RecordTime(r + 18, pending);
	}
	*pendingNext = (uint64_t)lba * SECTOR + (((uint64_t)size + SECTOR - 1) / SECTOR) * SECTOR;
	if(flags & FLAG_MORE)
		return; // the next record goes on
	pending->csize = pending->size;
	if(pending->data + pending->size > w->fileSize)
		pending->readable = 0; // the image ends before the file does
	if(!VolList_Add(w->l, pendingPath, pending))
		w->ok = 0;
	pendingPath[0] = 0;
}

// the records of the directory at sector `lba`, `size` bytes long, whose path is `prefix` ("" for the root)
static void WalkDir(Iso_t* w, uint32_t lba, uint32_t size, const char* prefix, int depth)
{
	uint8_t* buf;
	uint32_t off = 0;
	VolEntry_t pending;
	char pendingPath[VOL_NAME_MAX];
	uint64_t pendingNext = 0;
	if(!w->ok || depth > MAX_DEPTH || !size || size > MAX_DIR || w->dirs++ > VOL_MAX_ENTRIES ||
		(uint64_t)lba * SECTOR + size > w->fileSize)
		return;
	buf = (uint8_t*)malloc(size);
	if(!buf || !VolIn_ReadAt(w->f, (uint64_t)lba * SECTOR, buf, size))
	{
		if(!buf)
			w->ok = 0;
		free(buf);
		return;
	}
	pendingPath[0] = 0;
	memset(&pending, 0, sizeof pending);
	while(off < size && w->ok && VolList_Count(w->l) < VOL_MAX_ENTRIES)
	{
		unsigned len = buf[off];
		if(!len)
		{ // no record crosses a sector: the rest of this one is empty
			off = (off / SECTOR + 1) * SECTOR;
			continue;
		}
		if(len < 34 || off + len > size || 33u + buf[off + 32] > len)
			break; // damaged: what was read so far stays
		TakeRecord(w, buf + off, prefix, depth, &pending, pendingPath, &pendingNext);
		off += len;
	}
	free(buf);
}

int Iso_Read(const VolIn_t* f, uint64_t fileSize, VolList_t* l)
{
	uint8_t d[SECTOR], primary[34], joliet[34];
	int havePrimary = 0, haveJoliet = 0;
	uint32_t i;
	Iso_t w;
	for(i = 0; i < MAX_DESCS; i++)
	{
		if((uint64_t)(FIRST_DESC + i + 1) * SECTOR > fileSize || !VolIn_ReadAt(f, (uint64_t)(FIRST_DESC + i) * SECTOR, d, SECTOR))
			break;
		if(memcmp(d + 1, "CD001", 5) != 0)
			break;
		if(d[0] == 255) // the set's terminator
			break;
		if(d[0] == 1 && !havePrimary)
		{
			memcpy(primary, d + 156, 34);
			havePrimary = 1;
		}
		else if(d[0] == 2 && !haveJoliet && d[88] == 0x25 && d[89] == 0x2f && (d[90] == 0x40 || d[90] == 0x43 || d[90] == 0x45))
		{ // a supplementary descriptor whose escape sequence says UCS-2 (levels 1 to 3)
			memcpy(joliet, d + 156, 34);
			haveJoliet = 1;
		}
	}
	if(!haveJoliet)
	{ // no long names here: the disc's UDF part has them, when there is one
		int r = Udf_Read(f, fileSize, l);
		if(r)
			return r > 0;
	}
	if(!havePrimary)
		return 0;
	memset(&w, 0, sizeof w);
	w.f = f;
	w.fileSize = fileSize;
	w.l = l;
	w.ok = 1;
	w.joliet = haveJoliet;
	{
		const uint8_t* root = haveJoliet ? joliet : primary;
		WalkDir(&w, Le32(root + 2), Le32(root + 10), "", 0);
	}
	return w.ok;
}
