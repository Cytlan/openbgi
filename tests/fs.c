/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/fs.c - unit test of the volumes (src/fs/vol.c, src/include/bgi/vol.h), their zip reader (src/fs/zip.c)
 *              and of the file layer over a zip game (src/fs/fs.c,
 *              src/include/bgi/fs.h); built as test_fs by `make test`
 *
 * The archives are written by the test itself (stored and deflated
 * entries, Shift-JIS and UTF-8 names, zip64 records) into a scratch
 * directory in the temporary directory, which also holds the user data
 * ($BGI_DATA).  The groups, in the order they run:
 *
 *   stream   - Deflate_InflateStream of an input fed in small pieces,
 *              longer than the window it keeps, equals the whole buffer
 *   entries  - Vol_Open: the common top folder taken off, __MACOSX left
 *              out, the game's folder of a disc's layout made the root,
 *              implied folders, the name encodings (a Shift-JIS name
 *              whose trail byte is '\', the UTF-8 flag, the Info-ZIP
 *              Unicode path field), the last of two names differing in
 *              case kept, the keys in order
 *   read     - Vol_Load and Vol_Extract of stored and deflated entries, a
 *              damaged entry refused, Vol_OpenStored, a zip64 archive
 *   mount    - Fs_GameFolder under $BGI_DATA, Fs_MountArchive over it: what
 *              the archive holds reads through Fs_* (attributes, sizes,
 *              seeks, the time stamp), the folder's own files win
 *   write    - a new file in a folder only the archive has, a file of the
 *              archive opened for writing (copied first), deleting one
 *              (remembered across a new mount), creating it again, moving
 *              and copying, folders made and removed
 *   list     - Fs_FindFirst merges the folder and the archive without
 *              doubles, leaves out .openbgi and what is gone, has "." and
 *              ".." in a folder only the archive has
 *   paths    - relative paths under Fs_SetCurrentDir, a path above the
 *              game directory, Fs_LocalPath extracting a copy
 *   folder   - a folder read as an archive (Vol_Open of a directory): its
 *              files in place; mounted, what is written goes to the game
 *              folder and the folder stays as it was
 *
 * The scratch directory is removed at the end.
 */
#include "bgi/deflate.h"
#include "bgi/core.h"
#include "bgi/fs.h"
#include "bgi/host.h"
#include "host/os.h"
#include "bgi/vol.h"

#include "testhost.h"

#include <stdio.h>
#include <time.h>

#include "check.h"

// ---- writing archives ----------------------------------------------------------------------

#define DOS_DATE ((40u << 9) | (5u << 5) | 17u)          // 2020-05-17
#define DOS_TIME ((12u << 11) | (34u << 5) | (56u / 2u)) // 12:34:56

typedef struct TestEntry
{
	const char* name;  // the stored name bytes
	const char* data;  // NULL for a folder ("name/")
	size_t size;       // bytes of data
	int deflate;       // compressed
	int utf8;          // flag bit 11
	const char* uname; // an Info-ZIP Unicode path field with this name, or NULL
	int badCrc;        // a wrong check value
	uint32_t unixTime; // an extended timestamp field with this time, or 0
} TestEntry_t;

typedef struct Out
{
	uint8_t* p;
	size_t len, cap;
} Out_t;

static void OutBytes(Out_t* out, const void* data, size_t n)
{
	if(out->len + n > out->cap)
	{
		out->cap = (out->len + n) * 2 + 256;
		out->p = (uint8_t*)realloc(out->p, out->cap);
	}
	memcpy(out->p + out->len, data, n);
	out->len += n;
}

static void Out16(Out_t* out, uint32_t v)
{
	uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
	OutBytes(out, b, 2);
}

static void Out32(Out_t* out, uint32_t v)
{
	Out16(out, v & 0xffff);
	Out16(out, v >> 16);
}

static void Out64(Out_t* out, uint64_t v)
{
	Out32(out, (uint32_t)v);
	Out32(out, (uint32_t)(v >> 32));
}

/* an archive of `n` entries at `path`; with `zip64` every size and offset
 * goes to the zip64 extra field and the end records are the zip64 ones */
static void WriteZip(const char* path, const TestEntry_t* es, int n, int zip64)
{
	Out_t body, cd;
	uint64_t* offs = (uint64_t*)calloc((size_t)n, sizeof *offs);
	uint8_t** stored = (uint8_t**)calloc((size_t)n, sizeof *stored);
	size_t* storedLen = (size_t*)calloc((size_t)n, sizeof *storedLen);
	uint32_t* crcs = (uint32_t*)calloc((size_t)n, sizeof *crcs);
	FILE* f;
	int i;
	memset(&body, 0, sizeof body);
	memset(&cd, 0, sizeof cd);
	for(i = 0; i < n; i++)
	{
		const TestEntry_t* e = &es[i];
		size_t nameLen = strlen(e->name);
		crcs[i] = e->data ? BGI_Crc32(0, e->data, e->size) : 0;
		if(e->badCrc)
			crcs[i] ^= 1;
		if(e->data && e->deflate)
		{
			size_t z;
			uint8_t* zl = Deflate_Compress((const uint8_t*)e->data, e->size, 6, &z);
			storedLen[i] = z - 6; // the zlib header and check value off: a raw stream
			stored[i] = (uint8_t*)malloc(storedLen[i]);
			memcpy(stored[i], zl + 2, storedLen[i]);
			free(zl);
		}
		else if(e->data)
		{
			storedLen[i] = e->size;
			stored[i] = (uint8_t*)malloc(e->size + 1);
			memcpy(stored[i], e->data, e->size);
		}
		offs[i] = body.len;
		Out32(&body, 0x04034b50);
		Out16(&body, zip64 ? 45 : 20);
		Out16(&body, e->utf8 ? 0x0800 : 0);
		Out16(&body, e->deflate ? 8 : 0);
		Out16(&body, DOS_TIME);
		Out16(&body, DOS_DATE);
		Out32(&body, crcs[i]);
		Out32(&body, zip64 ? 0xffffffffu : (uint32_t)storedLen[i]);
		Out32(&body, zip64 ? 0xffffffffu : (uint32_t)e->size);
		Out16(&body, (uint32_t)nameLen);
		Out16(&body, zip64 ? 20 : 0);
		OutBytes(&body, e->name, nameLen);
		if(zip64)
		{
			Out16(&body, 1);
			Out16(&body, 16);
			Out64(&body, e->size);
			Out64(&body, storedLen[i]);
		}
		if(storedLen[i])
			OutBytes(&body, stored[i], storedLen[i]);
	}
	for(i = 0; i < n; i++)
	{
		const TestEntry_t* e = &es[i];
		size_t nameLen = strlen(e->name), extra = (zip64 ? 28 : 0) + (e->uname ? 9 + strlen(e->uname) : 0) + (e->unixTime ? 9 : 0);
		Out32(&cd, 0x02014b50);
		Out16(&cd, 0x031e);
		Out16(&cd, zip64 ? 45 : 20);
		Out16(&cd, e->utf8 ? 0x0800 : 0);
		Out16(&cd, e->deflate ? 8 : 0);
		Out16(&cd, DOS_TIME);
		Out16(&cd, DOS_DATE);
		Out32(&cd, crcs[i]);
		Out32(&cd, zip64 ? 0xffffffffu : (uint32_t)storedLen[i]);
		Out32(&cd, zip64 ? 0xffffffffu : (uint32_t)e->size);
		Out16(&cd, (uint32_t)nameLen);
		Out16(&cd, (uint32_t)extra);
		Out16(&cd, 0);
		Out16(&cd, 0);
		Out16(&cd, 0);
		Out32(&cd, e->data ? 0 : 0x10);
		Out32(&cd, zip64 ? 0xffffffffu : (uint32_t)offs[i]);
		OutBytes(&cd, e->name, nameLen);
		if(zip64)
		{
			Out16(&cd, 1);
			Out16(&cd, 24);
			Out64(&cd, e->size);
			Out64(&cd, storedLen[i]);
			Out64(&cd, offs[i]);
		}
		if(e->unixTime)
		{
			Out16(&cd, 0x5455);
			Out16(&cd, 5);
			OutBytes(&cd, "\x01", 1);
			Out32(&cd, e->unixTime);
		}
		if(e->uname)
		{
			Out16(&cd, 0x7075);
			Out16(&cd, (uint32_t)(5 + strlen(e->uname)));
			OutBytes(&cd, "\x01", 1);
			Out32(&cd, BGI_Crc32(0, e->name, nameLen));
			OutBytes(&cd, e->uname, strlen(e->uname));
		}
	}
	{
		uint64_t cdOff = body.len, cdSize = cd.len;
		OutBytes(&body, cd.p, cd.len);
		if(zip64)
		{
			uint64_t end64 = body.len;
			Out32(&body, 0x06064b50);
			Out64(&body, 44);
			Out16(&body, 45);
			Out16(&body, 45);
			Out32(&body, 0);
			Out32(&body, 0);
			Out64(&body, (uint64_t)n);
			Out64(&body, (uint64_t)n);
			Out64(&body, cdSize);
			Out64(&body, cdOff);
			Out32(&body, 0x07064b50);
			Out32(&body, 0);
			Out64(&body, end64);
			Out32(&body, 1);
		}
		Out32(&body, 0x06054b50);
		Out16(&body, 0);
		Out16(&body, 0);
		Out16(&body, zip64 ? 0xffff : (uint32_t)n);
		Out16(&body, zip64 ? 0xffff : (uint32_t)n);
		Out32(&body, zip64 ? 0xffffffffu : (uint32_t)cdSize);
		Out32(&body, zip64 ? 0xffffffffu : (uint32_t)cdOff);
		Out16(&body, 0);
	}
	f = fopen(path, "wb");
	fwrite(body.p, 1, body.len, f);
	fclose(f);
	for(i = 0; i < n; i++)
		free(stored[i]);
	free(stored);
	free(storedLen);
	free(offs);
	free(crcs);
	free(body.p);
	free(cd.p);
}

// ---- the archive of the tests ----------------------------------------------------------------

static char gBig[300000]; // compressible, longer than the inflater's window and output buffer together

static const char kIpl[] = "the boot program";
static const char kSys[] = "an archive of the system's programs, deflated: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

// "Game/" around everything, so the top folder is taken off
static const TestEntry_t kGame[] = {
	{"Game/", NULL, 0, 0, 0, NULL, 0, 0},
	{"Game/ipl._bp", kIpl, sizeof kIpl - 1, 0, 0, NULL, 0, 0},
	{"Game/Data/Sys.arc", kSys, sizeof kSys - 1, 1, 0, NULL, 0, 0},
	{"Game/Data/Sub/deep.txt", "deep", 4, 0, 0, NULL, 0, 0},
	{"Game/movie", "a file", 6, 0, 0, NULL, 0, 0}, // and a folder of the same name: the folder wins
	{"Game/movie/op.mpg", gBig, sizeof gBig, 1, 0, NULL, 0, 0},
	{"Game/\x83\x5c.txt", "so", 2, 0, 0, NULL, 0, 0},                    // "ソ.txt" in Shift-JIS: 0x5C is a trail byte
	{"Game/\xe6\x97\xa5.txt", "nichi", 5, 0, 1, NULL, 0, 0},             // "日.txt" in UTF-8 with the flag
	{"Game/old.txt", "unicode", 7, 0, 0, "Game/\xe6\x9c\xac.txt", 0, 0}, // the Unicode path field says "本.txt"
	{"Game/Case.TXT", "first", 5, 0, 0, NULL, 0, 0},
	{"Game/case.txt", "second", 6, 0, 0, NULL, 0, 0},
	{"Game/bad.txt", "damaged", 7, 1, 0, NULL, 1, 0},
	{"__MACOSX/Game/._ipl._bp", "fork", 4, 0, 0, NULL, 0, 0},
};

static char gDir[0x400];       // the scratch directory
static char gZipPath[0x400];   // the game's archive
static char gZip64Path[0x400]; // a zip64 one

// ---- the groups -----------------------------------------------------------------------------

typedef struct Feed
{
	const uint8_t* p;
	size_t left;
	Out_t out;
} Feed_t;

static size_t FeedRead(void* ctx, uint8_t* buf, size_t n)
{
	Feed_t* f = (Feed_t*)ctx;
	if(n > 777) // small and odd pieces
		n = 777;
	if(n > f->left)
		n = f->left;
	memcpy(buf, f->p, n);
	f->p += n;
	f->left -= n;
	return n;
}

static int FeedWrite(void* ctx, const uint8_t* data, size_t n)
{
	OutBytes(&((Feed_t*)ctx)->out, data, n);
	return 1;
}

static void TestStream(void)
{
	static uint8_t data[3000000];
	size_t i, z;
	uint8_t* zl;
	Feed_t f;
	uint32_t x = 1;
	for(i = 0; i < sizeof data; i++)
	{ // runs and repeats far apart, so the matches reach back across the flushes
		x = x * 1103515245u + 12345u;
		data[i] = (uint8_t)((i / 40000) % 2 ? data[i % 30000] : (x >> 24) & 0x0f);
	}
	zl = Deflate_Compress(data, sizeof data, 6, &z);
	memset(&f, 0, sizeof f);
	f.p = zl + 2;
	f.left = z - 6;
	CHECK(Deflate_InflateStream(FeedRead, FeedWrite, &f));
	CHECK(f.out.len == sizeof data && memcmp(f.out.p, data, sizeof data) == 0);
	free(f.out.p);
	// a truncated stream fails
	memset(&f, 0, sizeof f);
	f.p = zl + 2;
	f.left = (z - 6) / 2;
	CHECK(!Deflate_InflateStream(FeedRead, FeedWrite, &f));
	free(f.out.p);
	free(zl);
}

static void TestEntries(OpenBGI_t* o)
{
	Vol_t* z = Vol_Open(o, gZipPath);
	const VolEntry_t* e;
	uint32_t i;
	CHECK(z != NULL);
	if(!z)
		return;
	// the keys, in order: the top folder is gone, the folders are entries, __MACOSX is left out
	{
		static const char* const kKeys[] = {"bad.txt", "case.txt", "data", "data/sub", "data/sub/deep.txt", "data/sys.arc", "ipl._bp",
			"movie", "movie/op.mpg", "\xe3\x82\xbd.txt", "\xe6\x97\xa5.txt", "\xe6\x9c\xac.txt"};
		CHECK(Vol_Count(z) == sizeof kKeys / sizeof kKeys[0]);
		for(i = 0; i < Vol_Count(z) && i < sizeof kKeys / sizeof kKeys[0]; i++)
		{
			CHECK(strcmp(Vol_At(z, i)->key, kKeys[i]) == 0);
			if(strcmp(Vol_At(z, i)->key, kKeys[i]) != 0)
				printf("  key %u: %s, not %s\n", (unsigned)i, Vol_At(z, i)->key, kKeys[i]);
		}
	}
	e = Vol_Find(z, "data/sys.arc");
	CHECK(e && strcmp(e->path, "Data/Sys.arc") == 0 && strcmp(e->name, "Sys.arc") == 0 && e->method == 8 && !e->stored && !e->isDir && e->readable);
	e = Vol_Find(z, "data/sub");
	CHECK(e && e->isDir && strcmp(e->path, "Data/Sub") == 0);
	e = Vol_Find(z, "movie");
	CHECK(e && e->isDir && !e->readable);
	e = Vol_Find(z, "case.txt");
	CHECK(e && strcmp(e->path, "case.txt") == 0); // the one stored last
	CHECK(Vol_Find(z, "Data") == NULL);           // keys are in lower case
	CHECK(Vol_Find(z, "") == NULL);
	CHECK(Vol_Find(z, "game") == NULL);
	CHECK(Vol_Lower(z, "data/") == 3);
	Vol_Close(z);
	CHECK(Vol_Open(o, "no such archive.zip") == NULL);
	CHECK(Vol_Open(o, "no such folder/") == NULL); // (a folder that is there is read as an archive: the group "folder")
	{                                              // an archive of a disc's layout: the folder with the boot archive becomes the root, the rest is gone
		static const TestEntry_t kDisc[] = {
			{"Autorun.inf", "x", 1, 0, 0, NULL, 0, 0},
			{"DirectX/dxsetup.exe", "y", 1, 0, 0, NULL, 0, 0},
			{"Extras/Demo/system.arc", "deeper", 6, 0, 0, NULL, 0, 0},
			{"Game/SYSTEM.ARC", "sys", 3, 0, 0, NULL, 0, 0},
			{"Game/sub/a.txt", "a", 1, 0, 0, NULL, 0, 0},
		};
		char path[0x440];
		snprintf(path, sizeof path, "%s/disc.zip", gDir);
		WriteZip(path, kDisc, 5, 0);
		z = Vol_Open(o, path);
		CHECK(z && Vol_Count(z) == 3 && Vol_Find(z, "system.arc") && Vol_Find(z, "system.arc")->size == 3);
		CHECK(z && Vol_Find(z, "sub/a.txt") && !Vol_Find(z, "autorun.inf") && !Vol_Find(z, "game/system.arc"));
		Vol_Close(z);
	}
}

static int CountWrite(void* ctx, const uint8_t* data, size_t n)
{
	BGI_UNUSED(data);
	*(size_t*)ctx += n;
	return 1;
}

static void TestRead(OpenBGI_t* o)
{
	Vol_t* z = Vol_Open(o, gZipPath);
	const VolEntry_t* e;
	uint8_t* data;
	size_t n = 0;
	if(!z)
		return;
	e = Vol_Find(z, "ipl._bp");
	data = e ? Vol_Load(z, e, &n) : NULL;
	CHECK(data && n == sizeof kIpl - 1 && memcmp(data, kIpl, n) == 0 && data[n] == 0);
	free(data);
	e = Vol_Find(z, "data/sys.arc");
	data = e ? Vol_Load(z, e, &n) : NULL;
	CHECK(data && n == sizeof kSys - 1 && memcmp(data, kSys, n) == 0);
	free(data);
	e = Vol_Find(z, "movie/op.mpg");
	n = 0;
	CHECK(e && Vol_Extract(z, e, CountWrite, &n) && n == sizeof gBig);
	e = Vol_Find(z, "bad.txt");
	CHECK(e && Vol_Load(z, e, &n) == NULL); // the check value is wrong
	n = 0;
	CHECK(e && !Vol_Extract(z, e, CountWrite, &n));
	e = Vol_Find(z, "data");
	CHECK(e && Vol_Load(z, e, &n) == NULL); // a folder
	e = Vol_Find(z, "\xe6\x9c\xac.txt");
	data = e ? Vol_Load(z, e, &n) : NULL;
	CHECK(data && n == 7 && memcmp(data, "unicode", 7) == 0);
	free(data);
	{
		uint64_t off = 0;
		HostFile_t* f;
		char buf[32];
		e = Vol_Find(z, "ipl._bp");
		f = e ? Vol_OpenStored(z, e, &off) : NULL;
		CHECK(f && Host_FileSeek(o, f, off) && Host_FileRead(o, f, buf, sizeof kIpl - 1) == sizeof kIpl - 1 && memcmp(buf, kIpl, sizeof kIpl - 1) == 0);
		Host_FileClose(o, f);
		e = Vol_Find(z, "data/sys.arc");
		CHECK(e && Vol_OpenStored(z, e, &off) == NULL); // deflated
	}
	Vol_Close(z);

	z = Vol_Open(o, gZip64Path);
	CHECK(z != NULL);
	if(z)
	{
		CHECK(Vol_Count(z) == 3); // a.txt, d, d/b.bin
		e = Vol_Find(z, "d/b.bin");
		data = e ? Vol_Load(z, e, &n) : NULL;
		CHECK(data && n == sizeof kSys - 1 && memcmp(data, kSys, n) == 0);
		free(data);
		e = Vol_Find(z, "a.txt");
		data = e ? Vol_Load(z, e, &n) : NULL;
		CHECK(data && n == 3 && memcmp(data, "abc", 3) == 0);
		CHECK(e && e->mtime == 132341924960000000ull); // the extended timestamp, as a FILETIME
		e = Vol_Find(z, "d/b.bin");
		CHECK(e && e->mtime == 0);
		free(data);
		Vol_Close(z);
	}
}

// a whole file through Fs_*, compared with the expected bytes
static int FsHas(OpenBGI_t* o, const char* path, const char* want, size_t n)
{
	uint32_t size;
	uint8_t* data = Fs_LoadFile(o, path, &size);
	int ok = data && size == n && memcmp(data, want, n) == 0;
	free(data);
	return ok;
}

// a game folder's own file (a native path under it) with the given contents
static void PutNative(const char* folder, const char* rel, const char* text)
{
	char path[0x800];
	snprintf(path, sizeof path, "%s%s", folder, rel);
	OS_WriteNativeFile(path, text, (uint32_t)strlen(text));
}

static char gFolder[0x400]; // the game folder

static void TestMount(OpenBGI_t* o)
{
	char want[0x400];
	FsFile_t* f;
	char buf[64];
	uint32_t lo = 0, hi = 0;
	CHECK(Fs_IsArchive(o, gZipPath));
	CHECK(!Fs_IsArchive(o, gDir));
	CHECK(Fs_GameFolder(o, gZipPath, gFolder, sizeof gFolder));
	snprintf(want, sizeof want, "%s/data%cgames%cgame%c", gDir, OS_NATIVE_SEP, OS_NATIVE_SEP, OS_NATIVE_SEP);
	CHECK(strcmp(gFolder, want) == 0);
	CHECK(OS_NativeStat(gFolder, NULL) == OS_NATIVE_DIR);
	CHECK(!Fs_Mounted(o));
	CHECK(OS_SetGameDir(gFolder));
	CHECK(Fs_MountArchive(o, gZipPath));
	CHECK(Fs_Mounted(o) && strcmp(Fs_MountedPath(o), gZipPath) == 0);

	CHECK(Fs_FileAttrs(o, "ipl._bp") == OS_ATTR_ARCHIVE);
	CHECK(Fs_FileAttrs(o, "IPL._BP") == OS_ATTR_ARCHIVE);
	CHECK(Fs_FileAttrs(o, "data") == OS_ATTR_DIRECTORY);
	CHECK(Fs_FileAttrs(o, "Data\\SUB") == OS_ATTR_DIRECTORY);
	CHECK(Fs_FileAttrs(o, "data\\nothing") == OS_INVALID_ATTRS);
	CHECK(Fs_FileAttrs(o, "\x83\x5c.txt") == OS_ATTR_ARCHIVE); // "ソ.txt"
	CHECK(Fs_FileAttrs(o, "\x93\xfa.txt") == OS_ATTR_ARCHIVE); // "日.txt"
	CHECK(Fs_FileAttrs(o, "__MACOSX") == OS_INVALID_ATTRS);
	CHECK(FsHas(o, "ipl._bp", kIpl, sizeof kIpl - 1));
	CHECK(FsHas(o, "data\\sys.arc", kSys, sizeof kSys - 1));
	CHECK(FsHas(o, "\x96\x7b.txt", "unicode", 7)); // "本.txt"
	CHECK(Fs_LoadFile(o, "bad.txt", &lo) == NULL);

	// a stored entry read in place: seeks, the size, the time stamp; no writing
	f = Fs_FileOpenRead(o, "ipl._bp");
	CHECK(f != NULL);
	if(f)
	{
		CHECK(Fs_FileSize(f) == sizeof kIpl - 1);
		CHECK(Fs_FileSeek(f, 4) && Fs_FileRead(f, buf, 4) == 4 && memcmp(buf, "boot", 4) == 0);
		CHECK(Fs_FileRead(f, buf, sizeof buf) == sizeof kIpl - 1 - 8);
		CHECK(Fs_FileRead(f, buf, sizeof buf) == 0);
		CHECK(Fs_FileWrite(f, "x", 1) == 0);
		{ // 2020-05-17 12:34:56, local time as MS-DOS times are
			struct tm tm;
			uint64_t want;
			memset(&tm, 0, sizeof tm);
			tm.tm_year = 120;
			tm.tm_mon = 4;
			tm.tm_mday = 17;
			tm.tm_hour = 12;
			tm.tm_min = 34;
			tm.tm_sec = 56;
			tm.tm_isdst = -1;
			want = ((uint64_t)(int64_t)mktime(&tm) + 11644473600ull) * 10000000ull;
			CHECK(Fs_FileGetTime(f, &lo, &hi) && lo == (uint32_t)want && hi == (uint32_t)(want >> 32));
		}
		Fs_FileClose(f);
	}
	// a deflated one, from memory
	f = Fs_FileOpenRead(o, "DATA\\Sys.Arc");
	CHECK(f && Fs_FileSize(f) == sizeof kSys - 1 && Fs_FileSeek(f, 3) && Fs_FileRead(f, buf, 7) == 7 && memcmp(buf, "archive", 7) == 0);
	Fs_FileClose(f);
	CHECK(Fs_FileOpenRead(o, "data") == NULL);
	CHECK(Fs_FileOpenRead(o, "nothing") == NULL);

	// the folder's own file wins over the archive's
	PutNative(gFolder, "ipl._bp", "patched");
	CHECK(FsHas(o, "ipl._bp", "patched", 7));
	CHECK(Fs_FileDelete(o, "ipl._bp")); // the folder's copy, and then the archive's is gone as well
	CHECK(Fs_FileAttrs(o, "ipl._bp") == OS_INVALID_ATTRS);
	f = Fs_FileCreate(o, "ipl._bp"); // and made again: empty, in the folder
	CHECK(f != NULL);
	Fs_FileClose(f);
}

static void TestWrite(OpenBGI_t* o)
{
	FsFile_t* f;
	uint32_t size;
	char native[0x800];
	uint8_t* data;
	f = Fs_FileOpenRead(o, "ipl._bp");
	CHECK(f && Fs_FileSize(f) == 0);
	Fs_FileClose(f);

	// a new file in a folder only the archive has: the folder is made in the game folder
	f = Fs_FileCreate(o, "data\\sub\\save.dat");
	CHECK(f != NULL);
	if(f)
	{
		CHECK(Fs_FileWrite(f, "save", 4) == 4);
		Fs_FileClose(f);
	}
	snprintf(native, sizeof native, "%sData%cSub%csave.dat", gFolder, OS_NATIVE_SEP, OS_NATIVE_SEP);
	CHECK(OS_NativeStat(native, NULL) == OS_NATIVE_FILE); // as the archive spells the folders
	CHECK(FsHas(o, "data\\sub\\save.dat", "save", 4));
	CHECK(FsHas(o, "data\\sub\\deep.txt", "deep", 4)); // the archive's file next to it still reads

	// a file of the archive opened for writing keeps its contents
	f = Fs_FileOpenWrite(o, "data\\sub\\deep.txt");
	CHECK(f != NULL);
	if(f)
	{
		CHECK(Fs_FileSize(f) == 4);
		CHECK(Fs_FileSeek(f, 0) && Fs_FileWrite(f, "D", 1) == 1);
		Fs_FileClose(f);
	}
	CHECK(FsHas(o, "data\\sub\\deep.txt", "Deep", 4));
	CHECK(Fs_FileOpenWrite(o, "data") == NULL); // a folder

	// deleting a file of the archive: remembered, also across a new mount
	CHECK(Fs_FileDelete(o, "case.txt"));
	CHECK(Fs_FileAttrs(o, "case.txt") == OS_INVALID_ATTRS);
	CHECK(!Fs_FileDelete(o, "case.txt"));
	CHECK(!Fs_FileDelete(o, "data")); // DeleteFile takes no folder
	Fs_Unmount(o);
	CHECK(Fs_MountArchive(o, gZipPath));
	CHECK(Fs_FileAttrs(o, "case.txt") == OS_INVALID_ATTRS);
	snprintf(native, sizeof native, "%s.openbgi%cdeleted.txt", gFolder, OS_NATIVE_SEP);
	data = OS_ReadNativeFile(native, &size);
	CHECK(data && strstr((const char*)data, "case.txt\n") && strstr((const char*)data, "ipl._bp\n") == NULL);
	free(data);
	// and created again it is the new one
	f = Fs_FileCreate(o, "Case.txt");
	CHECK(f && Fs_FileWrite(f, "new", 3) == 3);
	Fs_FileClose(f);
	CHECK(FsHas(o, "case.txt", "new", 3));

	// moving and copying
	CHECK(Fs_FileMove(o, "\x83\x5c.txt", "data\\moved.txt"));
	CHECK(Fs_FileAttrs(o, "\x83\x5c.txt") == OS_INVALID_ATTRS);
	CHECK(FsHas(o, "data\\moved.txt", "so", 2));
	CHECK(!Fs_FileMove(o, "data\\moved.txt", "data\\sys.arc")); // the destination exists (in the archive)
	CHECK(Fs_FileCopy(o, "data\\sys.arc", "copy.arc", 1));
	CHECK(FsHas(o, "copy.arc", kSys, sizeof kSys - 1));
	CHECK(!Fs_FileCopy(o, "data\\sys.arc", "copy.arc", 1));
	CHECK(Fs_FileAttrs(o, "data\\sys.arc") == OS_ATTR_ARCHIVE);
	// a folder of the archive moves with what is in it, the game folder's files included
	CHECK(Fs_FileMove(o, "data\\sub", "sub2"));
	CHECK(Fs_FileAttrs(o, "data\\sub") == OS_INVALID_ATTRS);
	CHECK(FsHas(o, "sub2\\deep.txt", "Deep", 4));
	CHECK(FsHas(o, "sub2\\save.dat", "save", 4));
	CHECK(Fs_FileAttrs(o, "data\\sub\\deep.txt") == OS_INVALID_ATTRS);

	// folders
	CHECK(!Fs_DirCreate(o, "movie")); // there already
	CHECK(Fs_DirCreate(o, "movie\\more"));
	CHECK(Fs_FileAttrs(o, "movie\\more") == OS_ATTR_DIRECTORY);
	CHECK(!Fs_DirRemove(o, "movie")); // not empty
	CHECK(Fs_DirRemove(o, "movie\\more"));
	CHECK(Fs_FileDelete(o, "movie\\op.mpg"));
	CHECK(Fs_DirRemove(o, "movie"));
	CHECK(Fs_FileAttrs(o, "movie") == OS_INVALID_ATTRS);
	CHECK(Fs_DirCreate(o, "movie"));
	CHECK(Fs_FileAttrs(o, "movie\\op.mpg") == OS_INVALID_ATTRS); // what was in it stays gone
	CHECK(Fs_FileSetAttrs(o, "\x93\xfa.txt", OS_ATTR_ARCHIVE));  // copied to the folder first
	snprintf(native, sizeof native, "%s\xe6\x97\xa5.txt", gFolder);
	CHECK(OS_NativeStat(native, NULL) == OS_NATIVE_FILE || OS_NATIVE_SEP == '\\');
}

// the names a listing gives, as one string "a|b|c|" in the order given
static void List(OpenBGI_t* o, const char* pattern, char* out, size_t n, int* count)
{
	OsFindData_t d;
	FsFind_t* h = Fs_FindFirst(o, pattern, &d);
	out[0] = 0;
	*count = 0;
	if(!h)
		return;
	do
	{
		snprintf(out + strlen(out), n - strlen(out), "%s|", d.name);
		(*count)++;
	} while(Fs_FindNext(h, &d));
	Fs_FindClose(h);
}

static int Listed(const char* list, const char* name)
{
	char want[0x200];
	const char* p = list;
	size_t n;
	snprintf(want, sizeof want, "%s|", name);
	n = strlen(want);
	while(*p)
	{
		if(strncmp(p, want, n) == 0)
			return 1;
		p = strchr(p, '|') + 1;
	}
	return 0;
}

static void TestList(OpenBGI_t* o)
{
	char list[0x1000];
	int count;
	List(o, "*", list, sizeof list, &count);
	CHECK(Listed(list, "ipl._bp") && Listed(list, "Case.txt") && Listed(list, "Data") && Listed(list, "copy.arc")); // the folder's spelling
	CHECK(Listed(list, "\x93\xfa.txt") && Listed(list, "\x96\x7b.txt") && Listed(list, "sub2") && Listed(list, "bad.txt"));
	CHECK(!Listed(list, ".openbgi") && !Listed(list, "\x83\x5c.txt") && !Listed(list, "Case.TXT") && !Listed(list, "__MACOSX"));
	{ // no name twice, whatever its case
		char copy[0x1000];
		char* p;
		int doubles = 0;
		snprintf(copy, sizeof copy, "%s", list);
		for(p = copy; *p; p++)
			if(*p >= 'A' && *p <= 'Z')
				*p = (char)(*p + 32);
		for(p = copy; *p; p = strchr(p, '|') + 1)
		{
			char name[0x200];
			size_t len = (size_t)(strchr(p, '|') - p);
			const char* q;
			int seen = 0;
			memcpy(name, p, len);
			name[len] = 0;
			for(q = copy; *q; q = strchr(q, '|') + 1)
				if(strncmp(q, name, len) == 0 && q[len] == '|')
					seen++;
			doubles += seen > 1;
		}
		CHECK(doubles == 0);
	}
	List(o, "*.txt", list, sizeof list, &count);
	CHECK(Listed(list, "Case.txt") && Listed(list, "bad.txt") && !Listed(list, "Data") && !Listed(list, "ipl._bp"));
	List(o, "data\\*", list, sizeof list, &count);
	CHECK(Listed(list, "Sys.arc") && Listed(list, "moved.txt") && !Listed(list, "Sub") && !Listed(list, "sub"));
	List(o, "data\\s*.arc", list, sizeof list, &count);
	CHECK(count == 1 && Listed(list, "Sys.arc"));
	List(o, "nothing\\*", list, sizeof list, &count);
	CHECK(count == 0);
	List(o, "movie\\*", list, sizeof list, &count);
	CHECK(!Listed(list, "op.mpg"));
}

static void TestPaths(OpenBGI_t* o)
{
	char local[0x400], native[0x400];
	FILE* f;
	CHECK(Fs_SetCurrentDir(o, "data"));
	CHECK(FsHas(o, "sys.arc", kSys, sizeof kSys - 1));
	CHECK(FsHas(o, "..\\copy.arc", kSys, sizeof kSys - 1));
	CHECK(Fs_FileAttrs(o, "..\\..\\ipl._bp") == OS_INVALID_ATTRS); // above the game directory
	CHECK(Fs_SetCurrentDir(o, ".."));                              // back to the game directory
	CHECK(FsHas(o, "data\\sys.arc", kSys, sizeof kSys - 1));
	// an absolute path under the game directory
	{
		char exe[0x400], path[0x800];
		OS_ExeDir(exe, sizeof exe);
		snprintf(path, sizeof path, "%sdata\\sys.arc", exe);
		CHECK(FsHas(o, path, kSys, sizeof kSys - 1));
		snprintf(path, sizeof path, "%sDATA", exe);
		CHECK(Fs_FileAttrs(o, path) == OS_ATTR_DIRECTORY);
	}
	// a copy for an OS service that opens the file by its name
	CHECK(Fs_LocalPath(o, "data\\sys.arc", local, sizeof local));
	CHECK(strstr(local, ".openbgi\\cache\\") != NULL && strstr(local, ".arc") != NULL);
	OS_PathNative(local, native, sizeof native);
	f = OS_NativeOpen(native, "rb");
	CHECK(f != NULL);
	if(f)
	{
		char buf[sizeof kSys];
		CHECK(fread(buf, 1, sizeof buf, f) == sizeof kSys - 1 && memcmp(buf, kSys, sizeof kSys - 1) == 0);
		fclose(f);
	}
	CHECK(Fs_LocalPath(o, "copy.arc", local, sizeof local) && strcmp(local, "copy.arc") == 0); // the folder has it
	CHECK(!Fs_LocalPath(o, "nothing", local, sizeof local));
}

static void TestFolder(OpenBGI_t* o)
{
	char src[0x440], sub[0x480], withSep[0x480], folder[0x400], want[0x800], native[0x800];
	Vol_t* z;
	const VolEntry_t* e;
	uint8_t* data;
	size_t n = 0;
	uint32_t size = 0;
	uint64_t off = 1;
	HostFile_t* h;
	FsFile_t* f;
	char buf[16];
	snprintf(src, sizeof src, "%s/Folder Game", gDir);
	snprintf(sub, sizeof sub, "%s/Sub", src);
	snprintf(withSep, sizeof withSep, "%s/", src);
	TestHost_Mkdir(src);
	TestHost_Mkdir(sub);
	PutNative(withSep, "ipl._bp", kIpl);
	PutNative(withSep, "Data.TXT", "from the folder");
	PutNative(withSep, "Sub/deep.bin", "deep");

	z = Vol_Open(o, src); // a folder read as an archive: its files in place
	CHECK(z && Vol_Count(z) == 4);
	e = z ? Vol_Find(z, "data.txt") : NULL;
	CHECK(e && strcmp(e->path, "Data.TXT") == 0 && e->size == 15 && e->stored && !e->check && e->readable && e->file && e->mtime);
	data = e ? Vol_Load(z, e, &n) : NULL;
	CHECK(data && n == 15 && memcmp(data, "from the folder", 15) == 0 && data[15] == 0);
	free(data);
	n = 0;
	CHECK(e && Vol_Extract(z, e, CountWrite, &n) && n == 15);
	h = e ? Vol_OpenStored(z, e, &off) : NULL;
	CHECK(h && off == 0 && Host_FileRead(o, h, buf, 4) == 4 && memcmp(buf, "from", 4) == 0);
	Host_FileClose(o, h);
	e = z ? Vol_Find(z, "sub") : NULL;
	CHECK(e && e->isDir && !e->file);
	e = z ? Vol_Find(z, "sub/deep.bin") : NULL;
	CHECK(e && e->size == 4 && strcmp(e->name, "deep.bin") == 0);
	Vol_Close(z);
	z = Vol_Open(o, withSep); // named with its separator
	CHECK(z && Vol_Count(z) == 4);
	Vol_Close(z);

	// mounted: read through the file layer, written into the game folder, the folder itself as it was
	Fs_Unmount(o);
	CHECK(Fs_GameFolder(o, withSep, folder, sizeof folder));
	snprintf(want, sizeof want, "%s/data%cgames%cFolder Game%c", gDir, OS_NATIVE_SEP, OS_NATIVE_SEP, OS_NATIVE_SEP);
	CHECK(strcmp(folder, want) == 0);
	CHECK(OS_SetGameDir(folder));
	CHECK(Fs_MountArchive(o, src));
	CHECK(FsHas(o, "data.txt", "from the folder", 15) && FsHas(o, "SUB\\DEEP.BIN", "deep", 4));
	f = Fs_FileCreate(o, "Data.TXT");
	CHECK(f && Fs_FileWrite(f, "changed", 7) == 7);
	Fs_FileClose(f);
	CHECK(FsHas(o, "data.txt", "changed", 7));
	snprintf(native, sizeof native, "%s/Data.TXT", src);
	data = OS_ReadNativeFile(native, &size);
	CHECK(data && size == 15 && memcmp(data, "from the folder", 15) == 0);
	free(data);
	Fs_Unmount(o);
}

/* Patches: archives laid over the game's (Vol_Overlay, Fs_MountPatch).
 * Their files take the place of the game's of the same path, whatever
 * the case, and their other files are there too; the later over the
 * earlier; a patch zipped as a folder is read from inside it. */
static void TestPatch(OpenBGI_t* o)
{
	static const TestEntry_t kA[] = {
		{"ipl._bp", "patched boot", 12, 0, 0, NULL, 0, 0},
		{"data/sys.ARC", "patched system", 14, 1, 0, NULL, 0, 0},
		{"Patch/Scenario/a.txt", "new", 3, 0, 0, NULL, 0, 0},
		{"movie", "no folder", 9, 0, 0, NULL, 0, 0}, // the game has a folder of the name: it stays one
	};
	static const TestEntry_t kB[] = {
		// zipped as a folder
		{"MyPatch/", NULL, 0, 0, 0, NULL, 0, 0},
		{"MyPatch/IPL._bp", "patched twice", 13, 1, 0, NULL, 0, 0},
		{"MyPatch/Extra/b.txt", "extra", 5, 0, 0, NULL, 0, 0},
	};
	static const TestEntry_t kD[] = {
		// one folder at the top that is the game's own: nothing of it meets the game's files either way
		{"Patch/Images/x.bmp", "picture", 7, 0, 0, NULL, 0, 0},
	};
	char game[0x440], a[0x440], b[0x440], d[0x440], c[0x440], cSub[0x480], cSep[0x480], folder[0x400], list[0x400];
	Vol_t *z, *pa, *pb, *pd;
	const VolEntry_t* e;
	uint8_t* data;
	size_t n = 0;
	uint64_t off = 0;
	uint32_t i;
	HostFile_t* h;
	char buf[16];
	int count = 0, sorted = 1;
	snprintf(game, sizeof game, "%s/patched.zip", gDir);
	snprintf(a, sizeof a, "%s/a.zip", gDir);
	snprintf(b, sizeof b, "%s/b.zip", gDir);
	snprintf(d, sizeof d, "%s/d.zip", gDir);
	snprintf(c, sizeof c, "%s/Loose Patch", gDir);
	snprintf(cSub, sizeof cSub, "%s/DATA", c);
	snprintf(cSep, sizeof cSep, "%s/", c);
	WriteZip(game, kGame, (int)(sizeof kGame / sizeof kGame[0]), 0);
	WriteZip(a, kA, 4, 0);
	WriteZip(b, kB, 3, 0);
	WriteZip(d, kD, 1, 0);
	TestHost_Mkdir(c);
	TestHost_Mkdir(cSub);
	snprintf(cSub, sizeof cSub, "%s/DATA/Sub", c);
	TestHost_Mkdir(cSub);
	PutNative(cSep, "DATA/Sub/Deep.TXT", "from the loose patch");

	// a patch is opened as it is: the folder at its top stays
	pb = Vol_OpenPlain(o, b);
	CHECK(pb && Vol_Find(pb, "mypatch/ipl._bp") && !Vol_Find(pb, "ipl._bp") && Vol_Count(pb) == 4);
	z = Vol_Open(o, game);
	pa = Vol_OpenPlain(o, a);
	pd = Vol_OpenPlain(o, d);
	CHECK(z && pa && pd);
	if(!z || !pa || !pb || !pd)
		return;
	n = Vol_Count(z);
	// one over the game: its files in place of the game's, its new ones beside them
	CHECK(Vol_Overlay(z, pa) && Vol_Count(z) == n + 3); // patch, patch/scenario and the file in it
	e = Vol_Find(z, "ipl._bp");
	CHECK(e && e->from == pa && e->size == 12);
	data = e ? Vol_Load(z, e, &n) : NULL;
	CHECK(data && n == 12 && memcmp(data, "patched boot", 12) == 0);
	free(data);
	h = e ? Vol_OpenStored(z, e, &off) : NULL;
	CHECK(h && Host_FileSeek(o, h, (uint32_t)off) && Host_FileRead(o, h, buf, 12) == 12 && memcmp(buf, "patched boot", 12) == 0);
	Host_FileClose(o, h);
	n = 0;
	CHECK(e && Vol_Extract(z, e, CountWrite, &n) && n == 12);
	e = Vol_Find(z, "data/sys.arc");
	CHECK(e && e->from == pa && strcmp(e->path, "data/sys.ARC") == 0 && strcmp(e->name, "sys.ARC") == 0);
	data = e ? Vol_Load(z, e, &n) : NULL;
	CHECK(data && n == 14 && memcmp(data, "patched system", 14) == 0);
	free(data);
	n = 0;
	CHECK(e && Vol_Extract(z, e, CountWrite, &n) && n == 14);
	e = Vol_Find(z, "data/sub/deep.txt"); // the game's own still
	CHECK(e && !e->from);
	data = e ? Vol_Load(z, e, &n) : NULL;
	CHECK(data && n == 4 && memcmp(data, "deep", 4) == 0);
	free(data);
	e = Vol_Find(z, "patch/scenario/a.txt");
	CHECK(e && e->from == pa && Vol_Find(z, "patch") && Vol_Find(z, "patch")->isDir && Vol_Find(z, "patch/scenario")->isDir);
	e = Vol_Find(z, "movie");
	CHECK(e && e->isDir && Vol_Find(z, "movie/op.mpg") && !Vol_Find(z, "movie/op.mpg")->from);
	// a second one over both, zipped as a folder: read from inside it
	CHECK(Vol_Overlay(z, pb));
	e = Vol_Find(z, "ipl._bp");
	CHECK(e && e->from == pb && strcmp(e->path, "IPL._bp") == 0 && !Vol_Find(z, "mypatch") && !Vol_Find(z, "mypatch/ipl._bp"));
	data = e ? Vol_Load(z, e, &n) : NULL;
	CHECK(data && n == 13 && memcmp(data, "patched twice", 13) == 0);
	free(data);
	e = Vol_Find(z, "extra/b.txt");
	CHECK(e && e->from == pb && strcmp(e->path, "Extra/b.txt") == 0 && Vol_Find(z, "extra") && Vol_Find(z, "extra")->isDir);
	CHECK(Vol_Find(z, "data/sys.arc") && Vol_Find(z, "data/sys.arc")->from == pa);
	// a folder at the top that meets nothing either way stays
	CHECK(Vol_Overlay(z, pd) && Vol_Find(z, "patch/images/x.bmp") && !Vol_Find(z, "images/x.bmp") && Vol_Find(z, "patch/scenario/a.txt"));
	for(i = 1; i < Vol_Count(z); i++)
		if(strcmp(Vol_At(z, i - 1)->key, Vol_At(z, i)->key) >= 0)
			sorted = 0;
	CHECK(sorted && Vol_Lower(z, "patch/") < Vol_Count(z) && strncmp(Vol_At(z, Vol_Lower(z, "patch/"))->key, "patch/", 6) == 0);
	Vol_Close(z); // (and the three over it)

	// through the file layer: nothing to lay a patch on without a mount; then the same, and the game folder's files still win
	Fs_Unmount(o);
	CHECK(Fs_MountPatch(o, a) == -1);
	CHECK(Fs_GameFolder(o, game, folder, sizeof folder) && OS_SetGameDir(folder) && Fs_MountArchive(o, game));
	CHECK(FsHas(o, "ipl._bp", kIpl, sizeof kIpl - 1));
	CHECK(Fs_MountPatch(o, a) == 4 && Fs_MountPatch(o, cSep) == 1);
	snprintf(list, sizeof list, "%s/none.zip", gDir);
	CHECK(Fs_MountPatch(o, list) == -1 && Fs_MountPatch(o, gDir) >= 0); // (a folder of anything is a patch of anything)
	CHECK(FsHas(o, "ipl._bp", "patched boot", 12) && FsHas(o, "Data\\Sys.arc", "patched system", 14));
	CHECK(FsHas(o, "data\\sub\\deep.txt", "from the loose patch", 20) && FsHas(o, "Patch\\Scenario\\a.txt", "new", 3));
	CHECK(FsHas(o, "movie\\op.mpg", gBig, sizeof gBig) && (Fs_FileAttrs(o, "movie") & OPENBGI_ATTR_DIRECTORY));
	List(o, "Patch\\Scenario\\*", list, sizeof list, &count);
	CHECK(count == 3 && Listed(list, "a.txt") && Listed(list, ".") && Listed(list, ".."));
	List(o, "*", list, sizeof list, &count);
	CHECK(Listed(list, "Patch") && Listed(list, "ipl._bp") && Listed(list, "Data") && !Listed(list, "data"));
	PutNative(folder, "ipl._bp", "the player's");
	CHECK(FsHas(o, "ipl._bp", "the player's", 12));
	Fs_Unmount(o);
}

int main(void)
{
	static const TestEntry_t kZip64[] = {
		{"a.txt", "abc", 3, 0, 0, NULL, 0, 1589718896u}, // 2020-05-17 12:34:56 UTC
		{"d/b.bin", kSys, sizeof kSys - 1, 1, 0, NULL, 0, 0},
	};
	char data[0x420], cwd[0x400];
	size_t i;
	OpenBGI_t* o = OpenBGI_New(OS_Host()); // the engine whose code the test runs
	if(!o)
		return 1;
	OS_Init();
	for(i = 0; i < sizeof gBig; i++)
		gBig[i] = (char)('a' + (i * 7 + i / 1000) % 26);
	TestHost_ScratchName(gDir, sizeof gDir, "openbgi_fs");
	TestHost_Mkdir(gDir);
	snprintf(gZipPath, sizeof gZipPath, "%s/game.ZIP", gDir);
	snprintf(gZip64Path, sizeof gZip64Path, "%s/z64.zip", gDir);
	snprintf(data, sizeof data, "%s/data", gDir);
	TestHost_SetEnv("BGI_DATA", data);
	WriteZip(gZipPath, kGame, (int)(sizeof kGame / sizeof kGame[0]), 0);
	WriteZip(gZip64Path, kZip64, 2, 1);
	OS_NativeAbsolute(".", cwd, sizeof cwd);

	printf("stream\n");
	TestStream();
	printf("entries\n");
	TestEntries(o);
	printf("read\n");
	TestRead(o);
	printf("mount\n");
	TestMount(o);
	printf("write\n");
	TestWrite(o);
	printf("list\n");
	TestList(o);
	printf("paths\n");
	TestPaths(o);
	printf("folder\n");
	TestFolder(o);
	printf("patch\n");
	TestPatch(o);

	Fs_Unmount(o);
	TestHost_Chdir(cwd);
	TestHost_RemoveTree(gDir);
	if(gFails)
	{
		printf("fs: %d checks failed\n", gFails);
		return 1;
	}
	printf("fs: all tests passed\n");
	return 0;
}
