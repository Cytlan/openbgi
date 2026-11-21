/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * udf.c - the UDF part of a disc image, for the volumes (src/include/bgi/vol.h):
 *         the format alone; not part of the original engine
 *
 * The discs of the later games are "UDF bridge" discs: the files are
 * listed twice, once as ISO 9660 with names of eight and three characters
 * ("DATA0210.ARC" for data02100.arc) and once as UDF with the names as
 * they are.  iso.c asks here when an image has no Joliet names.
 *
 * What is followed: the anchor at sector 256, its volume descriptor
 * sequence (the partition and the logical volume), the file set and from
 * its root every directory.  The partitions may be plain ones, sparable
 * ones (read as plain: a pressed disc has nothing spared) and the
 * metadata partition of UDF 2.50, whose blocks are the bytes of a file.
 * A file's bytes may lie in one run of the image, or in its own entry.
 * Every file becomes a stored entry at its place in the image, as in
 * iso.c.
 *
 * Not followed: blocks of another size than 2048 bytes, the virtual
 * partition of a disc written in sessions, strategies other than 4.
 * Files in runs that do not follow each other, with parts not recorded,
 * or with their extents listed in further blocks are listed, not
 * readable.
 */
#include "vol_internal.h"

#include "bgi/bytes.h"

#define SECTOR        2048u
#define ANCHOR        256u       // the sector of the anchor
#define MAX_SEQ       64u        // descriptors of a volume descriptor sequence looked at
#define MAX_DEPTH     32         // folders inside folders
#define MAX_DIR       0x4000000u // bytes of one directory (64 MB)
#define MAX_PARTS     4u         // partitions and partition maps
#define MAX_RUNS      64         // runs of a directory, and of the metadata file

// the descriptors' tags
#define TAG_ANCHOR    2u
#define TAG_POINTER   3u
#define TAG_PARTITION 5u
#define TAG_LOGICAL   6u
#define TAG_END       8u
#define TAG_FILESET   256u
#define TAG_FILEID    257u
#define TAG_ENTRY     261u
#define TAG_ENTRY_EX  266u

// a file identifier's characteristics
#define FID_DIR       0x02u
#define FID_DELETED   0x04u
#define FID_PARENT    0x08u

// a run of the image's bytes
typedef struct UdfRun
{
	uint64_t at, size;
} UdfRun_t;

// a partition as the logical volume maps it
typedef struct UdfPart
{
	int known;       // its blocks can be found
	uint32_t number; // the partition descriptor's number
	uint32_t start;  // a plain partition: its first sector
	UdfRun_t* runs;  // the metadata partition: the runs of its file; NULL for a plain one
	uint32_t runCount;
} UdfPart_t;

// the walk's state
typedef struct Udf
{
	const VolIn_t* f;
	uint64_t fileSize;
	VolList_t* l;
	UdfPart_t parts[MAX_PARTS];
	uint32_t partCount;
	uint32_t dirs; // directories read so far
	int ok;        // 0 once memory ran out
} Udf_t;

// a file's or a directory's entry
typedef struct UdfEntry
{
	uint8_t block[SECTOR];
	uint64_t at;   // the block's place in the image
	unsigned part; // the partition it is in: what its short extents count in
	int isDir;
	unsigned adKind;  // how the extents are given: 0 short, 1 long, 3 none - the bytes themselves are in the entry
	uint32_t adOff;   // where in the block they are ..
	uint32_t adLen;   // .. and how many bytes of them
	uint64_t size;    // the file's bytes
	uint32_t timeOff; // where in the block the time of the last change is
} UdfEntry_t;

// the directories above the one walked, to tell a directory inside itself
typedef struct UdfUp
{
	uint64_t at;
	const struct UdfUp* up;
} UdfUp_t;

// whether `d` begins with the tag `id` of a descriptor
static int TagIs(const uint8_t* d, unsigned id)
{
	unsigned sum = 0, i;
	for(i = 0; i < 16; i++)
		if(i != 4)
			sum += d[i];
	return Le16(d) == id && (sum & 0xff) == d[4];
}

// where `bytes` bytes from block `lbn` of partition `part` on are in the image; 0 when they are not there in one piece
static int Place(const Udf_t* w, unsigned part, uint32_t lbn, uint64_t bytes, uint64_t* at)
{
	const UdfPart_t* p;
	if(part >= w->partCount || !w->parts[part].known)
		return 0;
	p = &w->parts[part];
	if(p->runs)
	{
		uint64_t off = (uint64_t)lbn * SECTOR;
		uint32_t i;
		for(i = 0; i < p->runCount; i++)
		{
			if(off < p->runs[i].size)
			{
				*at = p->runs[i].at + off;
				return off + bytes <= p->runs[i].size;
			}
			off -= p->runs[i].size;
		}
		return 0;
	}
	*at = ((uint64_t)p->start + lbn) * SECTOR;
	return *at + bytes <= w->fileSize;
}

/* the entry in the block at `at` of partition `part`; 0 when there is none
 * this reader follows.  `meta`: the entry of the metadata partition's
 * file, which is of a kind of its own. */
static int EntryAt(const Udf_t* w, uint64_t at, unsigned part, UdfEntry_t* n, int meta)
{
	const uint8_t* d = n->block;
	uint32_t eaLen, head;
	if(!VolIn_ReadAt(w->f, at, n->block, SECTOR))
		return 0;
	if(TagIs(d, TAG_ENTRY))
	{
		head = 176;
		n->timeOff = 84;
	}
	else if(TagIs(d, TAG_ENTRY_EX))
	{
		head = 216;
		n->timeOff = 92;
	}
	else
		return 0;
	if(Le16(d + 20) != 4) // the strategy: the entry itself, no chain of them
		return 0;
	n->at = at;
	n->part = part;
	n->isDir = d[27] == 4;
	if(!meta && !n->isDir && d[27] != 5 && d[27] != 249) // a plain file, or one of a video recorder; nothing else is a game's
		return 0;
	n->adKind = Le16(d + 34) & 7;
	if(n->adKind == 2)
		return 0;
	n->size = Le64(d + 56);
	eaLen = Le32(d + head - 8);
	n->adLen = Le32(d + head - 4);
	if(eaLen > SECTOR - head || n->adLen > SECTOR - head - eaLen)
		return 0;
	n->adOff = head + eaLen;
	return 1;
}

// the entry a long extent descriptor names
static int EntryOf(const Udf_t* w, const uint8_t* ad, UdfEntry_t* n)
{
	unsigned part = Le16(ad + 8);
	uint64_t at;
	return Place(w, part, Le32(ad + 4), SECTOR, &at) && EntryAt(w, at, part, n, 0);
}

/* the runs of the image that hold a file's bytes, in order: at most `cap`
 * of them into `runs` (two that follow each other count as one); their
 * number, or -1 when there are more or the entry gives them in a way this
 * reader does not follow */
static int Runs(const Udf_t* w, const UdfEntry_t* n, UdfRun_t* runs, int cap)
{
	uint64_t left = n->size;
	uint32_t off, step = n->adKind == 1 ? 16 : 8;
	int count = 0;
	if(!left)
		return 0;
	if(n->adKind == 3)
	{
		if(left > n->adLen)
			return -1;
		runs[0].at = n->at + n->adOff;
		runs[0].size = left;
		return 1;
	}
	for(off = 0; off + step <= n->adLen && left; off += step)
	{
		const uint8_t* a = n->block + n->adOff + off;
		uint32_t raw = Le32(a);
		uint64_t len = raw & 0x3fffffffu, at;
		if(!len)
			break;
		if(raw >> 30) // not recorded, or the descriptors go on in another block
			return -1;
		if(len > left)
			len = left;
		if(!Place(w, n->adKind == 1 ? Le16(a + 8) : n->part, Le32(a + 4), len, &at))
			return -1;
		if(count && runs[count - 1].at + runs[count - 1].size == at)
			runs[count - 1].size += len;
		else
		{
			if(count == cap)
				return -1;
			runs[count].at = at;
			runs[count].size = len;
			count++;
		}
		left -= len;
	}
	return left ? -1 : count;
}

/* an identifier's name as one UTF-8 path component: the first byte says
 * whether a character takes one byte or two.  0 when nothing usable is
 * left of it. */
static int DecodeName(const uint8_t* raw, size_t n, char* out, size_t cap)
{
	if(n < 2)
		return 0;
	if(raw[0] == 16 || raw[0] == 255)
	{
		if(!Vol_PutUtf16Be(raw + 1, n - 1, out, cap))
			return 0;
	}
	else if(raw[0] == 8 || raw[0] == 254)
	{
		size_t off = 0, i;
		for(i = 1; i < n; i++)
		{
			if(raw[i] < 0x20 || raw[i] == '/' || raw[i] == '\\')
				return 0;
			off = Vol_PutUtf8(out, off, cap, raw[i]);
		}
		out[off] = 0;
	}
	else
		return 0;
	return out[0] && strcmp(out, ".") != 0 && strcmp(out, "..") != 0;
}

// an entry's time of the last change (12 bytes: the zone, year, month, day, hour, minute, second, three fractions)
static void EntryTime(const uint8_t* t, VolEntry_t* e)
{
	unsigned tz = Le16(t);
	int zone = 0;
	if((tz >> 12) == 1)
	{ // local time, so many minutes east of UTC (12 bits, signed; -2047: not said)
		zone = (int)(tz & 0xfff);
		if(zone & 0x800)
			zone -= 0x1000;
		if(zone == -2047)
			zone = 0;
	}
	Vol_SetTime(e, (int16_t)Le16(t + 2), t[4], t[5], t[6], t[7], t[8], zone);
}

static void WalkDir(Udf_t* w, const UdfEntry_t* dir, const char* prefix, int depth, const UdfUp_t* up);

// what one identifier names (its long extent descriptor is `ad`): a folder to walk, or a file
static void TakeEntry(Udf_t* w, const uint8_t* ad, const char* path, int depth, const UdfUp_t* up)
{
	UdfEntry_t* n = (UdfEntry_t*)malloc(sizeof *n);
	VolEntry_t e;
	memset(&e, 0, sizeof e);
	if(!n)
	{
		w->ok = 0;
		return;
	}
	if(!EntryOf(w, ad, n))
	{ // (left out: nothing can be said of it)
		free(n);
		return;
	}
	EntryTime(n->block + n->timeOff, &e);
	if(n->isDir)
	{
		const UdfUp_t* u;
		for(u = up; u && u->at != n->at; u = u->up)
		{
		}
		e.isDir = 1;
		if(!u)
		{
			if(!VolList_Add(w->l, path, &e))
				w->ok = 0;
			else
				WalkDir(w, n, path, depth + 1, up);
		}
	}
	else
	{
		UdfRun_t run;
		int count = Runs(w, n, &run, 1);
		e.size = e.csize = n->size;
		e.stored = 1;
		e.readable = count >= 0;
		e.data = count > 0 ? run.at : (uint64_t)ANCHOR * SECTOR; // (an empty file has no place: any will do)
		if(e.data + e.size > w->fileSize)
			e.readable = 0; // the image ends before the file does
		if(!VolList_Add(w->l, path, &e))
			w->ok = 0;
	}
	free(n);
}

// a directory's bytes, `*size` of them, in a malloc'ed buffer; NULL when they cannot be read
static uint8_t* DirBytes(Udf_t* w, const UdfEntry_t* dir, uint32_t* size)
{
	UdfRun_t runs[MAX_RUNS];
	int count = Runs(w, dir, runs, MAX_RUNS), i;
	uint64_t off = 0;
	uint8_t* buf;
	if(count <= 0 || dir->size > MAX_DIR)
		return NULL;
	buf = (uint8_t*)malloc((size_t)dir->size);
	if(!buf)
	{
		w->ok = 0;
		return NULL;
	}
	for(i = 0; i < count; i++)
	{
		if(runs[i].at + runs[i].size > w->fileSize || !VolIn_ReadAt(w->f, runs[i].at, buf + off, (size_t)runs[i].size))
		{
			free(buf);
			return NULL;
		}
		off += runs[i].size;
	}
	*size = (uint32_t)dir->size;
	return buf;
}

// the identifiers of the directory `dir`, whose path is `prefix` ("" for the root)
static void WalkDir(Udf_t* w, const UdfEntry_t* dir, const char* prefix, int depth, const UdfUp_t* up)
{
	UdfUp_t here;
	uint8_t* buf;
	uint32_t size = 0, off = 0;
	if(!w->ok || depth > MAX_DEPTH || w->dirs++ > VOL_MAX_ENTRIES)
		return;
	buf = DirBytes(w, dir, &size);
	if(!buf)
		return;
	here.at = dir->at;
	here.up = up;
	while(off + 38 <= size && w->ok && VolList_Count(w->l) < VOL_MAX_ENTRIES)
	{
		const uint8_t* r = buf + off;
		uint32_t nameLen = r[19], useLen = Le16(r + 36);
		uint32_t len = (38 + useLen + nameLen + 3) & ~3u;
		char name[VOL_NAME_MAX], path[VOL_NAME_MAX];
		if(!TagIs(r, TAG_FILEID) || len > size - off)
			break; // damaged: what was read so far stays
		off += len;
		if(r[18] & (FID_DELETED | FID_PARENT))
			continue;
		if(!DecodeName(r + 38 + useLen, nameLen, name, sizeof name))
			continue;
		if((size_t)snprintf(path, sizeof path, "%s%s%s", prefix, prefix[0] ? "/" : "", name) >= sizeof path)
			continue;
		TakeEntry(w, r + 20, path, depth, &here);
	}
	free(buf);
}

// the partition descriptors of a volume descriptor sequence
typedef struct UdfDisc
{
	uint32_t number[MAX_PARTS], start[MAX_PARTS];
	uint32_t count;
	uint8_t logical[SECTOR]; // the logical volume descriptor
	int haveLogical;
} UdfDisc_t;

// the descriptors of the sequence of `bytes` bytes at sector `lba`
static void ReadSequence(const Udf_t* w, uint32_t lba, uint32_t bytes, UdfDisc_t* disc)
{
	uint8_t d[SECTOR];
	uint32_t i, left = bytes / SECTOR;
	for(i = 0; i < MAX_SEQ && left; i++, lba++, left--)
	{
		if((uint64_t)(lba + 1) * SECTOR > w->fileSize || !VolIn_ReadAt(w->f, (uint64_t)lba * SECTOR, d, SECTOR))
			return;
		if(TagIs(d, TAG_PARTITION))
		{
			uint32_t k, number = Le16(d + 22);
			for(k = 0; k < disc->count && disc->number[k] != number; k++)
			{
			}
			if(k < MAX_PARTS)
			{ // (a later descriptor of a partition replaces an earlier one)
				disc->number[k] = number;
				disc->start[k] = Le32(d + 188);
				if(k == disc->count)
					disc->count++;
			}
		}
		else if(TagIs(d, TAG_LOGICAL))
		{
			memcpy(disc->logical, d, SECTOR);
			disc->haveLogical = 1;
		}
		else if(TagIs(d, TAG_POINTER))
		{ // the sequence goes on elsewhere
			left = Le32(d + 20) / SECTOR + 1;
			lba = Le32(d + 24) - 1;
		}
		else if(TagIs(d, TAG_END) || !Le16(d))
			return;
	}
}

// the partition maps of the logical volume: where each partition's blocks are; 0 when memory ran out
static int ReadMaps(Udf_t* w, const UdfDisc_t* disc)
{
	const uint8_t* lv = disc->logical;
	uint32_t tableLen = Le32(lv + 264), mapCount = Le32(lv + 268), off = 0, i, k;
	uint32_t metaFile[MAX_PARTS];
	if(tableLen > SECTOR - 440)
		tableLen = SECTOR - 440;
	for(i = 0; i < mapCount && i < MAX_PARTS && off + 2 <= tableLen; i++)
	{
		const uint8_t* m = lv + 440 + off;
		UdfPart_t* p = &w->parts[i];
		unsigned kind = m[0], len = m[1];
		int plain = 0;
		if(len < 2 || off + len > tableLen)
			break;
		off += len;
		w->partCount = i + 1;
		metaFile[i] = 0xffffffffu;
		if(kind == 1 && len >= 6)
		{
			p->number = Le16(m + 4);
			plain = 1;
		}
		else if(kind == 2 && len >= 64)
		{
			p->number = Le16(m + 38);
			if(memcmp(m + 5, "*UDF Sparable Partition", 23) == 0)
				plain = 1;
			else if(memcmp(m + 5, "*UDF Metadata Partition", 23) == 0)
				metaFile[i] = Le32(m + 40);
		}
		if(plain)
			for(k = 0; k < disc->count; k++)
				if(disc->number[k] == p->number)
				{
					p->start = disc->start[k];
					p->known = 1;
				}
	}
	for(i = 0; i < w->partCount; i++)
	{ // a metadata partition: the runs of its file, which lies in the plain partition of the same number
		UdfEntry_t* n;
		uint64_t at;
		int count;
		if(metaFile[i] == 0xffffffffu)
			continue;
		for(k = 0; k < w->partCount && !(w->parts[k].known && !w->parts[k].runs && w->parts[k].number == w->parts[i].number); k++)
		{
		}
		if(k == w->partCount || !Place(w, k, metaFile[i], SECTOR, &at))
			continue;
		n = (UdfEntry_t*)malloc(sizeof *n);
		w->parts[i].runs = (UdfRun_t*)malloc(MAX_RUNS * sizeof(UdfRun_t));
		if(!n || !w->parts[i].runs)
		{
			free(n);
			return 0;
		}
		if(EntryAt(w, at, k, n, 1) && (count = Runs(w, n, w->parts[i].runs, MAX_RUNS)) > 0)
		{
			w->parts[i].runCount = (uint32_t)count;
			w->parts[i].known = 1;
		}
		free(n);
	}
	return 1;
}

int Udf_Read(const VolIn_t* f, uint64_t fileSize, VolList_t* l)
{
	uint8_t d[SECTOR];
	Udf_t w;
	UdfDisc_t* disc;
	UdfEntry_t* root;
	uint32_t i, before = VolList_Count(l);
	uint64_t at;
	int r = 0;
	if((uint64_t)(ANCHOR + 1) * SECTOR > fileSize || !VolIn_ReadAt(f, (uint64_t)ANCHOR * SECTOR, d, SECTOR) || !TagIs(d, TAG_ANCHOR) ||
		Le32(d + 12) != ANCHOR)
		return 0;
	memset(&w, 0, sizeof w);
	w.f = f;
	w.fileSize = fileSize;
	w.l = l;
	w.ok = 1;
	disc = (UdfDisc_t*)calloc(1, sizeof *disc);
	root = (UdfEntry_t*)malloc(sizeof *root);
	if(!disc || !root)
	{
		free(disc);
		free(root);
		return -1;
	}
	ReadSequence(&w, Le32(d + 20), Le32(d + 16), disc);
	if(!disc->haveLogical || !disc->count)
	{ // the main sequence is of no use: the reserve
		memset(disc, 0, sizeof *disc);
		ReadSequence(&w, Le32(d + 28), Le32(d + 24), disc);
	}
	if(disc->haveLogical && disc->count && Le32(disc->logical + 212) == SECTOR)
	{
		if(!ReadMaps(&w, disc))
			w.ok = 0;
		// the file set, and in it the root directory
		else if(Place(&w, Le16(disc->logical + 256), Le32(disc->logical + 252), SECTOR, &at) && VolIn_ReadAt(f, at, d, SECTOR) &&
			TagIs(d, TAG_FILESET) && EntryOf(&w, d + 400, root) && root->isDir)
		{
			WalkDir(&w, root, "", 0, NULL);
			r = VolList_Count(l) > before;
		}
	}
	for(i = 0; i < MAX_PARTS; i++)
		free(w.parts[i].runs);
	free(disc);
	free(root);
	return w.ok ? r : -1;
}
