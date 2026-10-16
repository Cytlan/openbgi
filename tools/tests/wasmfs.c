/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/wasmfs.c - unit test of the WebAssembly back end's file layer
 *                  (src/os/wasm/fs.c and host.c, declared in inc/bgi/os.h
 *                  and inc/bgi/os_wasm.h) run on a host; built on its own
 *                  by the Makefile as bin/test_wasmfs, since it cannot link
 *                  with the POSIX back end, which defines the same OS_File*
 *                  functions
 *
 * fs.c and host.c are built against tools/wasm/hostmock.c, which answers
 * the asset protocol from a scratch "game directory" the way
 * tools/wasm/server.js does (relative paths, case-insensitive components,
 * ranged reads, read-only), with the overlay in a second scratch
 * directory.  The mock counts its requests, so the tests can tell a cache
 * hit from a request to the server.
 *
 * The groups of tests, in the order they run:
 *
 *   paths       - OsWasm_RelPath: drive letters, separators, "." and ".."
 *   lookups     - OS_FileAttrs on files, directories and missing names
 *                 regardless of case; a repeated question costs no request
 *   reads       - the 64 KB block cache: a read fetches one block, the next
 *                 read of the same block is free, a read across blocks
 *                 fetches only the missing one, a run of missing blocks is
 *                 one request, the end of the file, a whole file read twice,
 *                 the time stamp
 *   eviction    - the cache limited to two blocks still reads a file of
 *                 five correctly, forwards and again from the start
 *   overlay     - files the engine writes: creation, listing next to the
 *                 server's entries, shadowing a server file, deletion
 *                 tombstones, copies, moves, directories, attributes, the
 *                 current directory, and the server files staying read-only
 *   persistence - after a shutdown and a new init the overlay and its
 *                 tombstones are still in force
 *
 * A failure in the cache groups means more (or wrong) requests reach the
 * server; one in the overlay groups means the engine would see another
 * set of files than it wrote.  The scratch directories are removed at the
 * end and the request counts printed.
 */
#include "bgi/os.h"
#include "bgi/os_wasm.h"
#include "tools/wasm/hostmock.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static int gFails; // the number of failed checks so far
// evaluate a condition; print it with its location and count a failure when it is false
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

// ---- the mock server (tools/wasm/hostmock.c) --------------------------------------------------

static char gRoot[0x400];                                // the "game directory"
static char gOverlay[0x400];                             // the overlay
static int gStatCalls, gReadCalls, gListCalls, gChanged; // the mock's request counters, refreshed by Counts

// refresh the request counters from the mock
static void Counts(void)
{
	HostMock_Counts(&gStatCalls, &gReadCalls, &gListCalls, &gChanged);
}

// ---- scratch directories ---------------------------------------------------------------------

// write `n` bytes to a host file (a check fails when it cannot be created)
static void WriteFile(const char* path, const void* data, size_t n)
{
	FILE* f = fopen(path, "wb");
	CHECK(f != NULL);
	if(!f)
		return;
	fwrite(data, 1, n, f);
	fclose(f);
}

// delete a host directory with everything in it
static void RemoveTree(const char* path)
{
	DIR* d = opendir(path);
	struct dirent* e;
	if(d)
	{
		while((e = readdir(d)) != NULL)
		{
			char full[0x400];
			struct stat st;
			if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
				continue;
			snprintf(full, sizeof full, "%s/%s", path, e->d_name);
			if(stat(full, &st) == 0 && S_ISDIR(st.st_mode))
				RemoveTree(full);
			else
				unlink(full);
		}
		closedir(d);
	}
	rmdir(path);
}

#define BIG 300000 // bytes of the archive file: a little over four 64 KB blocks, so it ends inside a block

static uint8_t gBig[BIG]; // the archive's contents, a deterministic pattern

/* populate the game directory: the archive, a small file and a Save directory with two
 * files */
static void MakeGame(void)
{
	char p[0x400];
	uint32_t i;
	for(i = 0; i < BIG; i++)
		gBig[i] = (uint8_t)(i * 7 + (i >> 8));
	snprintf(p, sizeof p, "%s/Data01000.arc", gRoot);
	WriteFile(p, gBig, BIG);
	snprintf(p, sizeof p, "%s/games.json", gRoot);
	WriteFile(p, "[]", 2);
	snprintf(p, sizeof p, "%s/Save", gRoot);
	mkdir(p, 0755);
	snprintf(p, sizeof p, "%s/Save/sysdat.dat", gRoot);
	WriteFile(p, "hello", 5);
	snprintf(p, sizeof p, "%s/Save/old.dat", gRoot);
	WriteFile(p, "old", 3);
}

// ---- helpers -----------------------------------------------------------------------------------

/* read an engine-side file (a Windows-style path) into `buf` as a string; the byte count,
 * -1 when it does not open */
static int ReadAll(const char* win, char* buf, size_t n)
{
	OsFile_t* f = OS_FileOpenRead(win);
	uint32_t r;
	if(!f)
		return -1;
	r = OS_FileRead(f, buf, (uint32_t)n - 1);
	buf[r] = 0;
	OS_FileClose(f);
	return (int)r;
}

// create an engine-side file holding `text`; 1 when every byte was written
static int WriteAll(const char* win, const char* text)
{
	OsFile_t* f = OS_FileCreate(win);
	uint32_t r;
	if(!f)
		return 0;
	r = OS_FileWrite(f, text, (uint32_t)strlen(text));
	OS_FileClose(f);
	return r == strlen(text);
}

/* the names a pattern lists (directories with a '/' appended), joined by ','; sorted so
 * that the order does not matter */
static void ListNames(const char* pattern, char* out, size_t n)
{
	OsFindData_t fd;
	OsFind_t* h = OS_FindFirst(pattern, &fd);
	char names[32][260];
	int count = 0, i, j;
	out[0] = 0;
	if(!h)
		return;
	do
	{
		if(count < 32)
			snprintf(names[count++], 260, "%s%s", fd.name, (fd.attrs & OS_ATTR_DIRECTORY) ? "/" : "");
	} while(OS_FindNext(h, &fd));
	OS_FindClose(h);
	for(i = 0; i < count; i++)
		for(j = i + 1; j < count; j++)
			if(strcmp(names[i], names[j]) > 0)
			{
				char t[260];
				strcpy(t, names[i]);
				strcpy(names[i], names[j]);
				strcpy(names[j], t);
			}
	for(i = 0; i < count; i++)
		snprintf(out + strlen(out), n - strlen(out), "%s%s", i ? "," : "", names[i]);
}

// ---- the tests -------------------------------------------------------------------------------

/* OsWasm_RelPath: the drive is dropped, '\\' becomes '/', "." and ".." are resolved (".."
 * above the root is ignored), the root is "" */
static void TestPaths(void)
{
	char rel[0x400];
	printf("paths\n");
	OsWasm_RelPath("C:\\Data01000.arc", rel, sizeof rel);
	CHECK(strcmp(rel, "Data01000.arc") == 0);
	OsWasm_RelPath("C:\\", rel, sizeof rel);
	CHECK(strcmp(rel, "") == 0);
	OsWasm_RelPath("C:\\save\\..\\..\\games.json", rel, sizeof rel);
	CHECK(strcmp(rel, "games.json") == 0);
	OsWasm_RelPath("save\\.\\x\\", rel, sizeof rel);
	CHECK(strcmp(rel, "save/x") == 0);
	OsWasm_RelPath("D:/a//b", rel, sizeof rel);
	CHECK(strcmp(rel, "a/b") == 0);
}

/* OS_FileAttrs through the server: a file and a directory are found
 * whatever the case of the name, a missing name is OS_INVALID_ATTRS, and
 * asking again (found or not) sends no further STAT request.  A directory
 * or a missing file cannot be opened for reading. */
static void TestLookups(void)
{
	printf("lookups\n");
	CHECK(OS_FileAttrs("C:\\data01000.ARC") == OS_ATTR_NORMAL);
	CHECK(OS_FileAttrs("C:\\") == OS_ATTR_DIRECTORY);
	CHECK(OS_FileAttrs("C:\\SAVE") == OS_ATTR_DIRECTORY);
	CHECK((OS_FileAttrs("C:\\save\\SYSDAT.DAT") & OS_ATTR_NORMAL) != 0);
	CHECK(OS_FileAttrs("C:\\nothing.arc") == OS_INVALID_ATTRS);
	CHECK(OS_FileAttrs("C:\\save\\nothing") == OS_INVALID_ATTRS);
	{ // the same question again costs nothing
		int before;
		Counts();
		before = gStatCalls;
		CHECK(OS_FileAttrs("C:\\Data01000.arc") == OS_ATTR_NORMAL);
		CHECK(OS_FileAttrs("C:\\nothing.arc") == OS_INVALID_ATTRS);
		Counts();
		CHECK(gStatCalls == before);
	}
	CHECK(OS_FileOpenRead("C:\\save") == NULL);
	CHECK(OS_FileOpenRead("C:\\nothing.arc") == NULL);
}

/* The block cache on the archive: a 16-byte read costs one READ request
 * (the whole 64 KB block), the next 16 bytes of the same block none, a
 * read straddling a block boundary fetches only the missing block, a read
 * spanning several missing blocks is a single request, reads at and past
 * the end return the remaining bytes and then 0, a whole-file read comes
 * back exactly and a second one entirely from the cache, and the time
 * stamp is a FILETIME later than the year 2000. */
static void TestReads(void)
{
	OsFile_t* f;
	uint8_t buf[0x20000];
	int before;
	printf("reads\n");
	f = OS_FileOpenRead("C:\\DATA01000.ARC");
	CHECK(f != NULL);
	if(!f)
		return;
	CHECK(OS_FileSize(f) == BIG);
	Counts();
	before = gReadCalls;
	CHECK(OS_FileSeek(f, 1000));
	CHECK(OS_FileRead(f, buf, 16) == 16);
	CHECK(memcmp(buf, gBig + 1000, 16) == 0);
	Counts();
	CHECK(gReadCalls == before + 1);
	// the next 16 bytes come from the block already held
	CHECK(OS_FileRead(f, buf, 16) == 16);
	CHECK(memcmp(buf, gBig + 1016, 16) == 0);
	Counts();
	CHECK(gReadCalls == before + 1);
	// across a block boundary: one request for the missing block only
	CHECK(OS_FileSeek(f, 0x10000 - 10));
	CHECK(OS_FileRead(f, buf, 100) == 100);
	CHECK(memcmp(buf, gBig + 0x10000 - 10, 100) == 0);
	Counts();
	CHECK(gReadCalls == before + 2);
	// a run of missing blocks is one request
	before = gReadCalls;
	CHECK(OS_FileSeek(f, 0x20000 + 5));
	CHECK(OS_FileRead(f, buf, 0x20000) == 0x20000);
	CHECK(memcmp(buf, gBig + 0x20000 + 5, 0x20000) == 0);
	Counts();
	CHECK(gReadCalls == before + 1);
	// the end of the file
	CHECK(OS_FileSeek(f, BIG - 10));
	CHECK(OS_FileRead(f, buf, 100) == 10);
	CHECK(memcmp(buf, gBig + BIG - 10, 10) == 0);
	CHECK(OS_FileRead(f, buf, 100) == 0);
	CHECK(OS_FileSeek(f, BIG + 100)); // a seek past the end is accepted; the read then gives nothing
	CHECK(OS_FileRead(f, buf, 100) == 0);
	OS_FileClose(f);
	// the whole file, then again from the cache
	{
		uint8_t* all = (uint8_t*)malloc(BIG);
		f = OS_FileOpenRead("C:\\data01000.arc");
		CHECK(f != NULL);
		CHECK(OS_FileRead(f, all, BIG) == BIG);
		CHECK(memcmp(all, gBig, BIG) == 0);
		OS_FileClose(f);
		Counts();
		before = gReadCalls;
		f = OS_FileOpenRead("C:\\data01000.arc");
		CHECK(OS_FileRead(f, all, BIG) == BIG);
		CHECK(memcmp(all, gBig, BIG) == 0);
		Counts();
		CHECK(gReadCalls == before);
		OS_FileClose(f);
		free(all);
	}
	{ // a time stamp: the high FILETIME word of any date after the year 2000 is above 0x01c00000
		uint32_t lo, hi;
		f = OS_FileOpenRead("C:\\data01000.arc");
		CHECK(OS_FileGetTime(f, &lo, &hi) && hi > 0x01c00000u);
		OS_FileClose(f);
	}
}

/* The cache limit: with room for two blocks only, the statistics never
 * show more, and the five-block archive still reads back exactly, both
 * straight through and a second time from the start (every block evicted
 * in between).  The limit is restored to the default afterwards. */
static void TestEviction(void)
{
	uint8_t* all = (uint8_t*)malloc(BIG);
	uint32_t bytes, blocks, req;
	OsFile_t* f;
	printf("eviction\n");
	OsWasm_SetCacheLimit(0x20000); // two blocks
	OsWasm_CacheStats(&bytes, &blocks, &req);
	CHECK(bytes <= 0x20000 && blocks <= 2);
	f = OS_FileOpenRead("C:\\data01000.arc");
	CHECK(OS_FileRead(f, all, BIG) == BIG);
	CHECK(memcmp(all, gBig, BIG) == 0);
	OsWasm_CacheStats(&bytes, &blocks, &req);
	CHECK(bytes <= 0x20000 && blocks <= 2);
	CHECK(OS_FileSeek(f, 0));
	CHECK(OS_FileRead(f, all, BIG) == BIG);
	CHECK(memcmp(all, gBig, BIG) == 0);
	OS_FileClose(f);
	OsWasm_SetCacheLimit(512u * 1024u * 1024u); // back to the default
	free(all);
}

/* The overlay, where everything the engine writes goes: a new file is
 * reported to the host as a change, is found and read back regardless of
 * case and listed next to the server's entries; a file written over a
 * server file shadows it; deleting a server file leaves a tombstone that
 * hides it (a second delete fails, creating the file again removes the
 * tombstone), and deleting the overlay copy of a shadowed file hides the
 * server's too; the root listing marks directories, hides the back end's
 * own directory and shows the overlay's lower-case "save" for the
 * server's "Save"; OS_FileCopy refuses to overwrite, OS_FileMove of a
 * server file copies it and hides the original; directories are created
 * (with their parents), refuse removal while not empty and can be removed
 * when the engine sees them empty even if the server still has files in
 * them; read-only attributes are set and cleared; relative paths follow
 * OS_SetCurrentDir; and a server file opened for reading cannot be
 * written. */
static void TestOverlay(void)
{
	char buf[0x100], names[0x400];
	uint32_t a;
	printf("overlay\n");
	// a new file lands in the overlay and reads back
	CHECK(WriteAll("C:\\Save\\New.dat", "fresh"));
	Counts();
	CHECK(gChanged > 0); // the host was told to persist the overlay
	CHECK(OS_FileAttrs("C:\\save\\new.dat") == OS_ATTR_NORMAL);
	CHECK(ReadAll("C:\\SAVE\\NEW.DAT", buf, sizeof buf) == 5 && strcmp(buf, "fresh") == 0);
	// it is listed next to the server's entries
	ListNames("C:\\save\\*", names, sizeof names);
	CHECK(strcmp(names, "new.dat,old.dat,sysdat.dat") == 0);
	ListNames("C:\\save\\*.dat", names, sizeof names);
	CHECK(strcmp(names, "new.dat,old.dat,sysdat.dat") == 0);
	ListNames("C:\\save\\n*", names, sizeof names);
	CHECK(strcmp(names, "new.dat") == 0);
	// a file written over a server file shadows it
	CHECK(ReadAll("C:\\save\\sysdat.dat", buf, sizeof buf) == 5 && strcmp(buf, "hello") == 0);
	CHECK(WriteAll("C:\\save\\SysDat.dat", "mine"));
	CHECK(ReadAll("C:\\save\\sysdat.dat", buf, sizeof buf) == 4 && strcmp(buf, "mine") == 0);
	ListNames("C:\\save\\*", names, sizeof names);
	CHECK(strcmp(names, "new.dat,old.dat,sysdat.dat") == 0); // listed once, not twice
	// deleting a server file leaves a tombstone
	CHECK(OS_FileDelete("C:\\save\\old.dat"));
	CHECK(OS_FileAttrs("C:\\save\\old.dat") == OS_INVALID_ATTRS);
	CHECK(OS_FileOpenRead("C:\\save\\old.dat") == NULL);
	ListNames("C:\\save\\*", names, sizeof names);
	CHECK(strcmp(names, "new.dat,sysdat.dat") == 0);
	CHECK(!OS_FileDelete("C:\\save\\old.dat"));
	// and creating it again takes the tombstone away
	CHECK(WriteAll("C:\\save\\old.dat", "again"));
	CHECK(ReadAll("C:\\save\\old.dat", buf, sizeof buf) == 5 && strcmp(buf, "again") == 0);
	// deleting the overlay copy of a shadowed server file hides the server's too
	CHECK(OS_FileDelete("C:\\save\\sysdat.dat"));
	CHECK(OS_FileAttrs("C:\\save\\sysdat.dat") == OS_INVALID_ATTRS);
	ListNames("C:\\save\\*", names, sizeof names);
	CHECK(strcmp(names, "new.dat,old.dat") == 0);
	// the root listing: directories are marked, the back end's own directory is not
	// shown; the
	// overlay's "save" (made for new.dat, kept in lower case) stands for the server's
	// "Save"
	ListNames("C:\\*", names, sizeof names);
	CHECK(strcmp(names, "Data01000.arc,games.json,save/") == 0);
	// copies and moves
	CHECK(OS_FileCopy("C:\\games.json", "C:\\save\\copy.json", 1));
	CHECK(!OS_FileCopy("C:\\games.json", "C:\\save\\copy.json", 1)); // the target exists: refused
	CHECK(ReadAll("C:\\save\\copy.json", buf, sizeof buf) == 2 && strcmp(buf, "[]") == 0);
	CHECK(OS_FileMove("C:\\save\\copy.json", "C:\\save\\moved.json"));
	CHECK(OS_FileAttrs("C:\\save\\copy.json") == OS_INVALID_ATTRS);
	CHECK(ReadAll("C:\\save\\moved.json", buf, sizeof buf) == 2);
	CHECK(OS_FileMove("C:\\games.json", "C:\\save\\games2.json")); // a server file: copied, then hidden
	CHECK(OS_FileAttrs("C:\\games.json") == OS_INVALID_ATTRS);
	CHECK(ReadAll("C:\\save\\games2.json", buf, sizeof buf) == 2);
	ListNames("C:\\*", names, sizeof names);
	CHECK(strcmp(names, "Data01000.arc,save/") == 0);
	// directories
	CHECK(OS_DirCreate("C:\\Save\\Thumbs"));
	CHECK(!OS_DirCreate("C:\\save\\thumbs")); // exists already (case-insensitively)
	CHECK(OS_FileAttrs("C:\\save\\thumbs") == OS_ATTR_DIRECTORY);
	CHECK(WriteAll("C:\\save\\thumbs\\1.bmp", "x"));
	CHECK(!OS_DirRemove("C:\\save\\thumbs")); // not empty
	CHECK(OS_FileDelete("C:\\save\\thumbs\\1.bmp"));
	CHECK(OS_DirRemove("C:\\save\\thumbs"));
	CHECK(OS_FileAttrs("C:\\save\\thumbs") == OS_INVALID_ATTRS);
	CHECK(WriteAll("C:\\deep\\er\\file.txt", "deep")); // parents are made
	CHECK(ReadAll("C:\\deep\\er\\file.txt", buf, sizeof buf) == 4);
	CHECK(!OS_DirRemove("C:\\Save")); // not empty as the engine sees it
	CHECK(OS_FileDelete("C:\\save\\new.dat") && OS_FileDelete("C:\\save\\old.dat"));
	CHECK(OS_FileDelete("C:\\save\\moved.json") && OS_FileDelete("C:\\save\\games2.json"));
	ListNames("C:\\save\\*", names, sizeof names);
	CHECK(strcmp(names, "") == 0);
	CHECK(OS_DirRemove("C:\\Save")); // empty now: the overlay's copy goes, the server's is hidden
	CHECK(OS_FileAttrs("C:\\save") == OS_INVALID_ATTRS);
	CHECK(OS_FileAttrs("C:\\save\\sysdat.dat") == OS_INVALID_ATTRS);
	CHECK(OS_FileOpenRead("C:\\save\\sysdat.dat") == NULL);
	// a directory of the server alone cannot go while it has files
	CHECK(!OS_DirRemove("C:\\"));
	// attributes
	CHECK(OS_FileSetAttrs("C:\\deep\\er\\file.txt", OS_ATTR_READONLY));
	a = OS_FileAttrs("C:\\deep\\er\\file.txt");
	CHECK((a & OS_ATTR_READONLY) != 0);
	CHECK(OS_FileSetAttrs("C:\\deep\\er\\file.txt", OS_ATTR_NORMAL));
	CHECK(OS_FileAttrs("C:\\deep\\er\\file.txt") == OS_ATTR_NORMAL);
	// the current directory
	CHECK(OS_SetCurrentDir("C:\\deep"));
	CHECK(ReadAll("er\\file.txt", buf, sizeof buf) == 4);
	CHECK(OS_FileAttrs("..\\Data01000.arc") == OS_ATTR_NORMAL);
	CHECK(!OS_SetCurrentDir("C:\\nowhere"));
	CHECK(OS_SetCurrentDir("C:\\"));
	// a server file is never written
	{
		OsFile_t* f = OS_FileOpenRead("C:\\data01000.arc");
		CHECK(f && OS_FileWrite(f, "x", 1) == 0);
		OS_FileClose(f);
	}
}

/* A new visit: after OsWasm_FsShutdown and OsWasm_FsInit the overlay
 * directory and the list of deleted paths are read back, so the hidden
 * server file and directory stay hidden, the written file is still there
 * and the root lists what the previous group left. */
static void TestPersistence(void)
{
	char buf[0x100], names[0x400];
	printf("persistence\n");
	OsWasm_FsShutdown();
	OsWasm_FsInit(); // a new visit: the overlay directory and the tombstones come back
	CHECK(OS_FileAttrs("C:\\games.json") == OS_INVALID_ATTRS);
	CHECK(OS_FileAttrs("C:\\save") == OS_INVALID_ATTRS);
	CHECK(ReadAll("C:\\deep\\er\\file.txt", buf, sizeof buf) == 4 && strcmp(buf, "deep") == 0);
	ListNames("C:\\*", names, sizeof names);
	CHECK(strcmp(names, "Data01000.arc,deep/") == 0);
}

/* make the scratch directories, point the mock and the overlay at them, run every group;
 * exit status 1 when any check failed */
int main(void)
{
	char tmpl[0x400];
	snprintf(tmpl, sizeof tmpl, "/tmp/bgi_wasmfs_%ld", (long)getpid());
	mkdir(tmpl, 0755);
	snprintf(gRoot, sizeof gRoot, "%s/game", tmpl);
	snprintf(gOverlay, sizeof gOverlay, "%s/Overlay", tmpl);
	mkdir(gRoot, 0755);
	MakeGame();
	HostMock_SetRoot(gRoot);
	OsWasm_SetOverlayRoot(gOverlay);
	OsWasm_FsInit();

	TestPaths();
	TestLookups();
	TestReads();
	TestEviction();
	TestOverlay();
	TestPersistence();

	OsWasm_FsShutdown();
	RemoveTree(tmpl);
	Counts();
	printf("%s (%d stat, %d read, %d list requests)\n", gFails ? "FAILED" : "all passed", gStatCalls, gReadCalls, gListCalls);
	return gFails ? 1 : 0;
}
