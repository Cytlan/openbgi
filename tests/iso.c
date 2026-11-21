/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/iso.c - unit test of the disc image reader (src/fs/iso.c) behind
 *               the volumes (src/include/bgi/vol.h) and of a game played
 *               from an image (src/include/bgi/fs.h); built as test_iso by
 *               `make test`
 *
 * The images are written by the test itself into a scratch directory in
 * the temporary directory, which also holds the user data ($BGI_DATA): a
 * disc laid out like a game's (the game in a folder beside Autorun.inf),
 * with and without the Joliet names.  The groups, in the order they run:
 *
 *   joliet   - the long names of the Joliet tree (a Japanese one, one of
 *              mixed case), the game's folder found and made the root,
 *              what is outside it gone, the folders, the keys in order
 *   plain    - without Joliet: the short names without their version and
 *              their closing dot, a Shift-JIS name
 *   read     - Vol_Load, Vol_Extract and Vol_OpenStored of files of an
 *              image, an empty file, a file in two extents that follow
 *              each other, the time stamp (also of a disc recorded in
 *              another time zone)
 *   refused  - listed but not readable: extents that do not follow each
 *              other, an interleaved file, a file past the end of a cut
 *              image; a folder that holds itself ends; a file that is no
 *              image is none
 *   game     - an image in the root of which the game is: nothing moved;
 *              two game folders: the one nearer the root, then the first
 *              by name; no game at all: the image as it is
 *   mount    - Fs_IsArchive, Fs_GameFolder without ".iso", Fs_MountArchive:
 *              the image's files read through Fs_*, a write lands in the
 *              game folder
 *
 * The scratch directory is removed at the end.
 */
#include "bgi/core.h"
#include "bgi/fs.h"
#include "bgi/host.h"
#include "host/os.h"
#include "bgi/vol.h"

#include "testhost.h"

#include <stdio.h>
#include <time.h>

#include "check.h"

// ---- writing images ------------------------------------------------------------------------

#define SECTOR    2048u
#define MAX_NODES 32

// a file or folder of an image to write
typedef struct Node
{
	int parent;         // the index of its folder (-1: the root itself, node 0)
	const char* joliet; // the name in the Joliet tree (UTF-8, ASCII or below U+10000), NULL: not in it
	const char* plain;  // the name bytes in the primary tree (with ";1" for a file)
	const char* data;   // NULL for a folder
	uint32_t size;      // bytes of data
	int split;          // 1: recorded as two extents, the second right after the first; 2: with a gap between them
	int interleaved;    // a file unit size in its record
	// set by the writer
	uint32_t lba[2];  // the extents (folders: the primary tree's, the Joliet tree's)
	uint32_t ext[2];  // the bytes of each extent (files)
	uint32_t dirSize; // a folder's bytes (one sector)
} Node_t;

static int gZone;      // the time zone of the records written: quarter hours east of UTC
static uint8_t* gImg;  // the image being written
static size_t gImgCap; // its bytes

static void Both32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
	p[4] = (uint8_t)(v >> 24), p[5] = (uint8_t)(v >> 16), p[6] = (uint8_t)(v >> 8), p[7] = (uint8_t)v;
}

// a directory record at `p`; the bytes it takes
static size_t Record(uint8_t* p, uint32_t lba, uint32_t size, unsigned flags, const uint8_t* name, size_t nameLen, int unit)
{
	size_t len = 33 + nameLen + ((nameLen & 1) ? 0 : 1);
	memset(p, 0, len);
	p[0] = (uint8_t)len;
	Both32(p + 2, lba);
	Both32(p + 10, size);
	p[18] = 120, p[19] = 5, p[20] = 17, p[21] = 12, p[22] = 34, p[23] = 56, p[24] = (uint8_t)gZone; // 2020-05-17 12:34:56 there
	p[25] = (uint8_t)flags;
	p[26] = (uint8_t)unit;
	p[28] = 1, p[31] = 1;
	p[32] = (uint8_t)nameLen;
	memcpy(p + 33, name, nameLen);
	return len;
}

// a UTF-8 name (below U+10000) as UCS-2, big-endian; the bytes
static size_t Ucs2(const char* s, uint8_t* out)
{
	const uint8_t* u = (const uint8_t*)s;
	size_t n = 0;
	while(*u)
	{
		uint32_t c;
		if(*u < 0x80)
			c = *u++;
		else if(*u < 0xe0)
			c = ((u[0] & 0x1fu) << 6) | (u[1] & 0x3fu), u += 2;
		else
			c = ((u[0] & 0x0fu) << 12) | ((u[1] & 0x3fu) << 6) | (u[2] & 0x3fu), u += 3;
		out[n++] = (uint8_t)(c >> 8);
		out[n++] = (uint8_t)c;
	}
	return n;
}

// the records of folder `dir` in one of the trees into its sector
static void WriteDir(Node_t* ns, int n, int dir, int tree)
{
	uint8_t* p = gImg + (size_t)ns[dir].lba[tree] * SECTOR;
	size_t off = 0;
	int i, parent = ns[dir].parent < 0 ? dir : ns[dir].parent;
	static const uint8_t kSelf[1] = {0}, kParent[1] = {1};
	off += Record(p + off, ns[dir].lba[tree], SECTOR, 2, kSelf, 1, 0);
	off += Record(p + off, ns[parent].lba[tree], SECTOR, 2, kParent, 1, 0);
	for(i = 1; i < n; i++)
	{
		uint8_t name[0x100];
		size_t len;
		if(ns[i].parent != dir || (tree == 1 && !ns[i].joliet))
			continue;
		if(tree == 1)
			len = Ucs2(ns[i].joliet, name);
		else
			len = strlen(ns[i].plain), memcpy(name, ns[i].plain, len);
		if(!ns[i].data)
			off += Record(p + off, ns[i].lba[tree], SECTOR, 2, name, len, 0);
		else if(ns[i].split)
		{
			off += Record(p + off, ns[i].lba[0], ns[i].ext[0], 0x80, name, len, 0);
			off += Record(p + off, ns[i].lba[1], ns[i].ext[1], 0, name, len, 0);
		}
		else
			off += Record(p + off, ns[i].lba[0], ns[i].size, 0, name, len, ns[i].interleaved);
	}
}

/* An image of the nodes (node 0 is the root) at a native path: the
 * primary descriptor, the Joliet one when `joliet`, the terminator, the
 * folders' sectors and the files' bytes.  `cut` bytes are left off the
 * end. */
static void WriteIso(const char* path, Node_t* ns, int n, int joliet, size_t cut)
{
	uint32_t next = 16 + 3;
	int i, tree;
	size_t total;
	for(i = 0; i < n; i++)
		if(!ns[i].data)
		{
			ns[i].lba[0] = next++;
			ns[i].lba[1] = next++;
		}
	for(i = 0; i < n; i++)
		if(ns[i].data)
		{
			if(ns[i].split)
			{
				ns[i].ext[0] = SECTOR; // the first extent is one whole sector
				ns[i].ext[1] = ns[i].size - SECTOR;
				ns[i].lba[0] = next++;
				if(ns[i].split == 2)
					next++; // a sector of something else between them
				ns[i].lba[1] = next;
				next += (ns[i].ext[1] + SECTOR - 1) / SECTOR;
			}
			else
			{
				ns[i].lba[0] = ns[i].size ? next : 0;
				next += (ns[i].size + SECTOR - 1) / SECTOR;
			}
		}
	total = (size_t)next * SECTOR;
	if(total > gImgCap)
	{
		gImg = (uint8_t*)realloc(gImg, total);
		gImgCap = total;
	}
	memset(gImg, 0, total);
	for(tree = 0; tree < 2; tree++)
	{
		uint8_t* d = gImg + (size_t)(16 + tree) * SECTOR;
		static const uint8_t kSelf[1] = {0};
		if(tree == 1 && !joliet)
		{ // the terminator comes right after the primary descriptor
			d[0] = 255;
			memcpy(d + 1, "CD001\1", 6);
			break;
		}
		d[0] = (uint8_t)(tree + 1);
		memcpy(d + 1, "CD001\1", 6);
		Both32(d + 80, next);
		if(tree == 1)
			d[88] = 0x25, d[89] = 0x2f, d[90] = 0x45; // UCS-2 level 3
		Record(d + 156, ns[0].lba[tree], SECTOR, 2, kSelf, 1, 0);
		for(i = 0; i < n; i++)
			if(!ns[i].data)
				WriteDir(ns, n, i, tree);
	}
	if(joliet)
	{
		uint8_t* d = gImg + (size_t)18 * SECTOR;
		d[0] = 255;
		memcpy(d + 1, "CD001\1", 6);
	}
	for(i = 0; i < n; i++)
		if(ns[i].data && ns[i].split)
		{
			memcpy(gImg + (size_t)ns[i].lba[0] * SECTOR, ns[i].data, SECTOR);
			memcpy(gImg + (size_t)ns[i].lba[1] * SECTOR, ns[i].data + SECTOR, ns[i].size - SECTOR);
		}
		else if(ns[i].data && ns[i].size)
			memcpy(gImg + (size_t)ns[i].lba[0] * SECTOR, ns[i].data, ns[i].size);
	OS_WriteNativeFile(path, gImg, (uint32_t)(total - cut));
}

// ---- the images ------------------------------------------------------------------------------

static const char kIpl[] = "the boot program";
static const char kSys[] = "an archive of the system's programs";
static const char kText[] = "a text beside the game";
static char gBig[5000]; // longer than two sectors

#define JP_NAME "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0.txt" // ゲーム.txt
#define JP_SJIS "\x83\x51\x81\x5b\x83\x80.TXT;1"           // the same in Shift-JIS, upper case, with its version

// a disc as a game's is laid out: the game in a folder beside Autorun.inf and the DirectX setup
static Node_t gDisc[] = {
	{-1, NULL, NULL, NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
	{0, "Autorun.inf", "AUTORUN.INF;1", kText, sizeof kText - 1, 0, 0, {0, 0}, {0, 0}, 0},
	{0, "DirectX", "DIRECTX", NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
	{2, "DXSETUP.exe", "DXSETUP.EXE;1", kText, 4, 0, 0, {0, 0}, {0, 0}, 0},
	{0, "TheGame", "THEGAME", NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
	{4, "system.arc", "SYSTEM.ARC;1", kSys, sizeof kSys - 1, 0, 0, {0, 0}, {0, 0}, 0},
	{4, "ipl._bp", "IPL._BP;1", kIpl, sizeof kIpl - 1, 0, 0, {0, 0}, {0, 0}, 0},
	{4, "A Long Name With Spaces.dat", "ALONGNAM.DAT;1", gBig, sizeof gBig, 0, 0, {0, 0}, {0, 0}, 0},
	{4, JP_NAME, JP_SJIS, kText, sizeof kText - 1, 0, 0, {0, 0}, {0, 0}, 0},
	{4, "TheGame", "THEGAME.;1", kText, 16, 0, 0, {0, 0}, {0, 0}, 0}, // the disc marker: no extension
	{4, "Sub", "SUB", NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
	{10, "empty.bin", "EMPTY.BIN;1", "", 0, 0, 0, {0, 0}, {0, 0}, 0},
	{10, "two.bin", "TWO.BIN;1", gBig, sizeof gBig, 1, 0, {0, 0}, {0, 0}, 0},
};
#define DISC_N ((int)(sizeof gDisc / sizeof gDisc[0]))

static char gDir[0x400];       // the scratch directory
static char gIsoPath[0x400];   // the disc with Joliet names
static char gPlainPath[0x400]; // the same without

static const VolEntry_t* Find(const Vol_t* z, const char* key)
{
	return z ? Vol_Find(z, key) : NULL;
}

// ---- the groups -----------------------------------------------------------------------------

static void TestJoliet(OpenBGI_t* o)
{
	static const char* const kKeys[] = {
		"a long name with spaces.dat",
		"ipl._bp",
		"sub",
		"sub/empty.bin",
		"sub/two.bin",
		"system.arc",
		"thegame",
		"\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0.txt",
	};
	Vol_t* z = Vol_Open(o, gIsoPath);
	const VolEntry_t* e;
	uint32_t i;
	CHECK(z != NULL);
	if(!z)
		return;
	CHECK(Vol_Count(z) == sizeof kKeys / sizeof kKeys[0]); // the game's folder is the root: Autorun.inf and DirectX are gone
	for(i = 0; i < Vol_Count(z) && i < sizeof kKeys / sizeof kKeys[0]; i++)
		if(strcmp(Vol_At(z, i)->key, kKeys[i]) != 0)
		{
			printf("  key %u: %s, not %s\n", (unsigned)i, Vol_At(z, i)->key, kKeys[i]);
			gFails++;
		}
	e = Find(z, "a long name with spaces.dat");
	CHECK(e && strcmp(e->path, "A Long Name With Spaces.dat") == 0 && e->size == sizeof gBig && !e->isDir && e->readable && e->stored && !e->check);
	e = Find(z, "sub");
	CHECK(e && e->isDir && strcmp(e->name, "Sub") == 0);
	e = Find(z, "sub/two.bin");
	CHECK(e && strcmp(e->name, "two.bin") == 0 && strcmp(e->path, "Sub/two.bin") == 0);
	CHECK(Find(z, "autorun.inf") == NULL && Find(z, "directx") == NULL && Find(z, "thegame/system.arc") == NULL);
	e = Find(z, "thegame"); // the marker file, which has the folder's name
	CHECK(e && !e->isDir && e->size == 16);
	Vol_Close(z);
}

static void TestPlain(OpenBGI_t* o)
{
	Vol_t* z = Vol_Open(o, gPlainPath);
	const VolEntry_t* e;
	CHECK(z != NULL);
	if(!z)
		return;
	CHECK(Vol_Count(z) == 8);
	e = Find(z, "system.arc");
	CHECK(e && strcmp(e->path, "SYSTEM.ARC") == 0 && e->size == sizeof kSys - 1); // without ";1"
	e = Find(z, "thegame");
	CHECK(e && strcmp(e->path, "THEGAME") == 0 && !e->isDir); // without the closing dot
	e = Find(z, "alongnam.dat");
	CHECK(e && e->size == sizeof gBig);
	e = Find(z, "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0.txt"); // the Shift-JIS name, its extension folded
	CHECK(e && strcmp(e->path, "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0.TXT") == 0);
	CHECK(Find(z, "sub/two.bin") != NULL && Find(z, "sub/empty.bin") != NULL);
	Vol_Close(z);
}

static int CountWrite(void* ctx, const uint8_t* data, size_t n)
{
	size_t* at = (size_t*)ctx;
	if(*at + n > sizeof gBig || memcmp(gBig + *at, data, n) != 0)
		return 0;
	*at += n;
	return 1;
}

static void TestRead(OpenBGI_t* o)
{
	Vol_t* z = Vol_Open(o, gIsoPath);
	const VolEntry_t* e;
	uint8_t* data;
	size_t n = 0, at = 0;
	uint64_t off = 0;
	HostFile_t* f;
	char buf[64];
	CHECK(z != NULL);
	if(!z)
		return;
	e = Find(z, "ipl._bp");
	data = e ? Vol_Load(z, e, &n) : NULL;
	CHECK(data && n == sizeof kIpl - 1 && memcmp(data, kIpl, n) == 0 && data[n] == 0);
	free(data);
	CHECK(e && e->mtime == UINT64_C(132341924960000000)); // 2020-05-17 12:34:56 UTC as a FILETIME
	CHECK(e && e->dosTime == (((40u << 9) | (5u << 5) | 17u) << 16 | ((12u << 11) | (34u << 5) | 28u)));
	e = Find(z, "a long name with spaces.dat");
	data = e ? Vol_Load(z, e, &n) : NULL;
	CHECK(data && n == sizeof gBig && memcmp(data, gBig, n) == 0);
	free(data);
	CHECK(e && Vol_Extract(z, e, CountWrite, &at) && at == sizeof gBig);
	f = e ? Vol_OpenStored(z, e, &off) : NULL; // read in place
	CHECK(f != NULL && off % SECTOR == 0);
	if(f)
	{
		CHECK(Host_FileSeek(o, f, off + 100) && Host_FileRead(o, f, buf, 32) == 32 && memcmp(buf, gBig + 100, 32) == 0);
		Host_FileClose(o, f);
	}
	e = Find(z, "sub/empty.bin");
	data = e ? Vol_Load(z, e, &n) : NULL;
	CHECK(e && e->size == 0 && e->readable && data && n == 0);
	free(data);
	e = Find(z, "sub/two.bin"); // two extents, one after the other: one file
	data = e ? Vol_Load(z, e, &n) : NULL;
	CHECK(e && e->size == sizeof gBig && e->readable && data && n == sizeof gBig && memcmp(data, gBig, n) == 0);
	free(data);
	e = Find(z, "sub");
	CHECK(e && Vol_Load(z, e, &n) == NULL); // a folder
	Vol_Close(z);
	{ // recorded nine hours east of UTC (Japan): the same wall clock is nine hours earlier
		static Node_t kZone[] = {
			{-1, NULL, NULL, NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
			{0, "system.arc", "SYSTEM.ARC;1", kSys, sizeof kSys - 1, 0, 0, {0, 0}, {0, 0}, 0},
		};
		char path[0x440];
		snprintf(path, sizeof path, "%s/zone.iso", gDir);
		gZone = 36;
		WriteIso(path, kZone, 2, 1, 0);
		gZone = 0;
		z = Vol_Open(o, path);
		e = Find(z, "system.arc");
		CHECK(e && e->mtime == UINT64_C(132341924960000000) - UINT64_C(9) * 3600 * 10000000);
		Vol_Close(z);
	}
}

static void TestRefused(OpenBGI_t* o)
{
	static Node_t kBad[] = {
		{-1, NULL, NULL, NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{0, "system.arc", "SYSTEM.ARC;1", kSys, sizeof kSys - 1, 0, 0, {0, 0}, {0, 0}, 0},
		{0, "gap.bin", "GAP.BIN;1", gBig, sizeof gBig, 2, 0, {0, 0}, {0, 0}, 0},
		{0, "inter.bin", "INTER.BIN;1", kText, sizeof kText - 1, 0, 3, {0, 0}, {0, 0}, 0},
		{0, "last.bin", "LAST.BIN;1", gBig, sizeof gBig, 0, 0, {0, 0}, {0, 0}, 0},
	};
	char path[0x440];
	Vol_t* z;
	const VolEntry_t* e;
	size_t n;
	snprintf(path, sizeof path, "%s/bad.iso", gDir);
	WriteIso(path, kBad, 5, 1, 0);
	z = Vol_Open(o, path);
	CHECK(z != NULL);
	e = Find(z, "gap.bin");
	CHECK(e && e->size == sizeof gBig && !e->readable && Vol_Load(z, e, &n) == NULL);
	e = Find(z, "inter.bin");
	CHECK(e && !e->readable);
	e = Find(z, "last.bin");
	CHECK(e && e->readable);
	e = Find(z, "system.arc");
	CHECK(e && e->readable);
	Vol_Close(z);

	WriteIso(path, kBad, 5, 1, 3000); // the image ends inside the last file
	z = Vol_Open(o, path);
	CHECK(z != NULL);
	e = Find(z, "last.bin");
	CHECK(e && !e->readable);
	e = Find(z, "system.arc");
	CHECK(e && e->readable);
	Vol_Close(z);

	{ // a folder whose record names its own sector: the walk ends
		static Node_t kLoop[] = {
			{-1, NULL, NULL, NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
			{0, "system.arc", "SYSTEM.ARC;1", kSys, sizeof kSys - 1, 0, 0, {0, 0}, {0, 0}, 0},
			{0, "loop", "LOOP", NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		};
		uint8_t* rec;
		WriteIso(path, kLoop, 3, 0, 0);
		// the root's third record (after ".", ".." and SYSTEM.ARC;1 the folder LOOP): point it at the root's own sector
		rec = gImg + (size_t)kLoop[0].lba[0] * SECTOR;
		rec += rec[0];
		rec += rec[0];
		rec += rec[0];
		CHECK(rec[32] == 4 && memcmp(rec + 33, "LOOP", 4) == 0);
		Both32(rec + 2, kLoop[0].lba[0]);
		OS_WriteNativeFile(path, gImg, (uint32_t)gImgCap);
		z = Vol_Open(o, path);
		CHECK(z != NULL);
		CHECK(Find(z, "system.arc") != NULL && Find(z, "loop") != NULL);
		Vol_Close(z);
	}

	snprintf(path, sizeof path, "%s/none.iso", gDir);
	OS_WriteNativeFile(path, gBig, sizeof gBig); // too short to be one
	CHECK(Vol_Open(o, path) == NULL && !Fs_IsArchive(o, path));
	memset(gImg, 'x', 20 * SECTOR);
	OS_WriteNativeFile(path, gImg, 20 * SECTOR); // long enough, without a descriptor
	CHECK(Vol_Open(o, path) == NULL && !Fs_IsArchive(o, path));
}

static void TestGame(OpenBGI_t* o)
{
	static Node_t kRoot[] = {
		// the game in the root: nothing moves
		{-1, NULL, NULL, NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{0, "ipl._bp", "IPL._BP;1", kIpl, sizeof kIpl - 1, 0, 0, {0, 0}, {0, 0}, 0},
		{0, "Other", "OTHER", NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{2, "system.arc", "SYSTEM.ARC;1", kSys, sizeof kSys - 1, 0, 0, {0, 0}, {0, 0}, 0},
	};
	static Node_t kTwo[] = {
		// two games: the one nearer the root; of two as near, the first by name
		{-1, NULL, NULL, NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{0, "Deep", "DEEP", NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{1, "Aaa", "AAA", NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{2, "system.arc", "SYSTEM.ARC;1", kText, 4, 0, 0, {0, 0}, {0, 0}, 0},
		{0, "Zed", "ZED", NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{4, "system.arc", "SYSTEM.ARC;1", kText, 5, 0, 0, {0, 0}, {0, 0}, 0},
		{0, "Mid", "MID", NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{6, "ipl._bp", "IPL._BP;1", kText, 6, 0, 0, {0, 0}, {0, 0}, 0},
		{6, "mid.txt", "MID.TXT;1", kText, 7, 0, 0, {0, 0}, {0, 0}, 0},
		{0, "none.txt", "NONE.TXT;1", kText, 8, 0, 0, {0, 0}, {0, 0}, 0},
	};
	static Node_t kTwoB[] = {
		// the same the other way round: the nearer and the first by name are found first
		{-1, NULL, NULL, NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{0, "Mid", "MID", NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{1, "ipl._bp", "IPL._BP;1", kText, 6, 0, 0, {0, 0}, {0, 0}, 0},
		{1, "mid.txt", "MID.TXT;1", kText, 7, 0, 0, {0, 0}, {0, 0}, 0},
		{0, "Zed", "ZED", NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{4, "system.arc", "SYSTEM.ARC;1", kText, 5, 0, 0, {0, 0}, {0, 0}, 0},
		{0, "Deep", "DEEP", NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{6, "Aaa", "AAA", NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{7, "system.arc", "SYSTEM.ARC;1", kText, 4, 0, 0, {0, 0}, {0, 0}, 0},
	};
	static Node_t kNone[] = {
		// no game at all: the image as it is
		{-1, NULL, NULL, NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{0, "Stuff", "STUFF", NULL, 0, 0, 0, {0, 0}, {0, 0}, 0},
		{1, "a.txt", "A.TXT;1", kText, 4, 0, 0, {0, 0}, {0, 0}, 0},
		{0, "b.txt", "B.TXT;1", kText, 4, 0, 0, {0, 0}, {0, 0}, 0},
	};
	char path[0x440];
	Vol_t* z;
	const VolEntry_t* e;
	snprintf(path, sizeof path, "%s/game.iso", gDir);
	WriteIso(path, kRoot, 4, 1, 0);
	z = Vol_Open(o, path);
	CHECK(z && Vol_Count(z) == 3 && Find(z, "ipl._bp") && Find(z, "other/system.arc"));
	Vol_Close(z);
	WriteIso(path, kTwo, 10, 1, 0);
	z = Vol_Open(o, path);
	e = Find(z, "ipl._bp");
	CHECK(z && Vol_Count(z) == 2 && e && e->size == 6 && Find(z, "mid.txt")); // Mid before Zed, both before Deep/Aaa
	Vol_Close(z);
	WriteIso(path, kTwoB, 9, 1, 0);
	z = Vol_Open(o, path);
	e = Find(z, "ipl._bp");
	CHECK(z && Vol_Count(z) == 2 && e && e->size == 6 && Find(z, "mid.txt"));
	Vol_Close(z);
	WriteIso(path, kNone, 4, 1, 0);
	z = Vol_Open(o, path);
	CHECK(z && Vol_Count(z) == 3 && Find(z, "stuff/a.txt") && Find(z, "b.txt"));
	Vol_Close(z);
}

static int FsHas(OpenBGI_t* o, const char* path, const char* want, size_t n)
{
	uint32_t size;
	uint8_t* data = Fs_LoadFile(o, path, &size);
	int ok = data && size == n && memcmp(data, want, n) == 0;
	free(data);
	return ok;
}

static void TestMount(OpenBGI_t* o)
{
	char folder[0x400], want[0x800], native[0x800], local[0x400];
	uint8_t* data;
	uint8_t buf[128];
	uint32_t size = 0;
	FsFile_t* f;
	CHECK(Fs_IsArchive(o, gIsoPath) && Fs_IsArchive(o, gPlainPath));
	CHECK(!Fs_IsArchive(o, gDir));
	CHECK(Fs_GameFolder(o, gIsoPath, folder, sizeof folder));
	snprintf(want, sizeof want, "%s/data%cgames%cDisc One%c", gDir, OS_NATIVE_SEP, OS_NATIVE_SEP, OS_NATIVE_SEP);
	CHECK(strcmp(folder, want) == 0); // without ".ISO"
	CHECK(OS_SetGameDir(folder));
	CHECK(Fs_MountArchive(o, gIsoPath));
	CHECK(Fs_Mounted(o) && strcmp(Fs_MountedPath(o), gIsoPath) == 0);
	CHECK(FsHas(o, "ipl._bp", kIpl, sizeof kIpl - 1));
	CHECK(FsHas(o, "SYSTEM.ARC", kSys, sizeof kSys - 1));
	CHECK(FsHas(o, "Sub\\two.bin", gBig, sizeof gBig));
	CHECK(FsHas(o, "a long name with spaces.DAT", gBig, sizeof gBig));
	CHECK(Fs_FileAttrs(o, "Sub\\empty.bin") == OPENBGI_ATTR_ARCHIVE && Fs_FileAttrs(o, "Sub") == OPENBGI_ATTR_DIRECTORY);
	f = Fs_FileOpenRead(o, "Autorun.inf"); // the image's, outside the game's folder: not there
	CHECK(f == NULL);
	if(f)
		Fs_FileClose(f);
	/* a file read through a handle, from any place (as the movie decoder
	 * reads a movie): in place, nothing copied out of the image.  Only what
	 * something outside the engine opens by its name is extracted. */
	f = Fs_FileOpenRead(o, "Sub\\two.bin");
	CHECK(f && Fs_FileSize(f) == sizeof gBig && Fs_FileSeek(f, 4000) && Fs_FileRead(f, buf, 100) == 100 && memcmp(buf, gBig + 4000, 100) == 0);
	CHECK(f && Fs_FileSeek(f, 10) && Fs_FileRead(f, buf, 50) == 50 && memcmp(buf, gBig + 10, 50) == 0);
	if(f)
		Fs_FileClose(f);
	snprintf(native, sizeof native, "%s.openbgi", folder);
	CHECK(OS_NativeStat(native, NULL) == OS_NATIVE_NONE);
	CHECK(Fs_LocalPath(o, "Sub\\two.bin", local, sizeof local) && strstr(local, ".openbgi\\cache\\") != NULL);
	CHECK(OS_NativeStat(native, NULL) == OS_NATIVE_DIR);
	// what the game writes goes to its folder, and is read from there afterwards
	f = Fs_FileCreate(o, "ipl._bp");
	CHECK(f && Fs_FileWrite(f, "changed", 7) == 7);
	if(f)
		Fs_FileClose(f);
	CHECK(FsHas(o, "ipl._bp", "changed", 7));
	snprintf(native, sizeof native, "%sipl._bp", folder);
	data = OS_ReadNativeFile(native, &size);
	CHECK(data && size == 7 && memcmp(data, "changed", 7) == 0);
	free(data);
	Fs_Unmount(o);
}

int main(void)
{
	char data[0x420], cwd[0x400];
	size_t i;
	OpenBGI_t* o = OpenBGI_New(OS_Host()); // the engine whose code the test runs
	if(!o)
		return 1;
	OS_Init();
	for(i = 0; i < sizeof gBig; i++)
		gBig[i] = (char)('a' + (i * 7 + i / 1000) % 26);
	TestHost_ScratchName(gDir, sizeof gDir, "openbgi_iso");
	TestHost_Mkdir(gDir);
	snprintf(gIsoPath, sizeof gIsoPath, "%s/Disc One.ISO", gDir);
	snprintf(gPlainPath, sizeof gPlainPath, "%s/plain.iso", gDir);
	snprintf(data, sizeof data, "%s/data", gDir);
	TestHost_SetEnv("BGI_DATA", data);
	WriteIso(gIsoPath, gDisc, DISC_N, 1, 0);
	WriteIso(gPlainPath, gDisc, DISC_N, 0, 0);
	OS_NativeAbsolute(".", cwd, sizeof cwd);

	printf("joliet\n");
	TestJoliet(o);
	printf("plain\n");
	TestPlain(o);
	printf("read\n");
	TestRead(o);
	printf("refused\n");
	TestRefused(o);
	printf("game\n");
	TestGame(o);
	printf("mount\n");
	TestMount(o);

	Fs_Unmount(o);
	TestHost_Chdir(cwd);
	TestHost_RemoveTree(gDir);
	free(gImg);
	if(gFails)
	{
		printf("iso: %d checks failed\n", gFails);
		return 1;
	}
	printf("iso: all tests passed\n");
	return 0;
}
