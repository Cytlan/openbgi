/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/preview.c - unit test of the host a game is run with out of sight
 *                   for the launcher's first picture (src/host/os_preview.c;
 *                   src/host/os_common.h); built as test_preview by
 *                   `make test`
 *
 * The run itself needs a game (the test host makes it on one:
 * `openbgi-headless --preview=FILE`); what is tested here is that its
 * host keeps to itself what a game writes.
 *
 * The groups of tests, in the order they run:
 *
 *   game    - the game's own paths are a drive in memory: files are made,
 *             read, written past their end, found by any spelling of
 *             their path, listed by pattern, copied, moved and removed as
 *             Windows would have it (a folder must be there for a file,
 *             and empty to go), and none of it is on the disk
 *   native  - native paths: a file of the disk is read as it is; one that
 *             is made, a folder that is made and a file that is removed
 *             are so for this host alone, and a listing shows both kinds
 *   host    - the clock stands, the game directory is a name, the user's
 *             data has a folder of its own
 */
#include "host/os.h"
#include "host/os_common.h"

#include "testhost.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

static OpenBGI_Host_t* gHost;
static const OpenBGI_Files_t* gF;
static char gDir[0x400];  // the scratch directory, on the disk
static char gWork[0x440]; // a folder in it for the test's files (the user's data folder comes to lie beside it)

// a game's file made with the text `s`; 1 when it was
static int Put(const char* path, const char* s)
{
	void* f = gF->open(gHost->user, path, OPENBGI_OPEN_CREATE);
	uint32_t n = (uint32_t)strlen(s);
	int ok;
	if(!f)
		return 0;
	ok = gF->write(gHost->user, f, s, n) == n;
	gF->close(gHost->user, f);
	return ok;
}

// the text of a game's file is `s`
static int Is(const char* path, const char* s)
{
	void* f = gF->open(gHost->user, path, OPENBGI_OPEN_READ);
	char buf[64];
	uint32_t n;
	int ok;
	if(!f)
		return 0;
	n = gF->read(gHost->user, f, buf, sizeof buf - 1);
	buf[n] = 0;
	ok = gF->length(gHost->user, f) == strlen(s) && strcmp(buf, s) == 0;
	gF->close(gHost->user, f);
	return ok;
}

// the names a pattern finds, each followed by a blank (a folder's with a slash before it), in the order found
static void Found(const char* pattern, char* out, size_t n)
{
	OpenBGI_FindData_t d;
	void* h = gF->findFirst(gHost->user, pattern, &d);
	out[0] = 0;
	if(!h)
		return;
	do
	{
		size_t len = strlen(out);
		snprintf(out + len, n - len, "%s%s ", d.attrs & OPENBGI_ATTR_DIRECTORY ? "/" : "", d.name);
	} while(gF->findNext(gHost->user, h, &d));
	gF->findClose(gHost->user, h);
}

static void TestGame(void)
{
	void* u = gHost->user;
	char buf[256];
	uint32_t lo = 0, hi = 0;
	void* f;
	printf("game\n");
	// the drive and what a Windows has on it
	gF->exeDir(u, buf, sizeof buf);
	CHECK(strcmp(buf, "C:\\") == 0 && gF->attrs(u, "C:\\") == OPENBGI_ATTR_DIRECTORY && gF->attrs(u, "") == OPENBGI_ATTR_DIRECTORY);
	CHECK(gF->driveType(u, "C:\\") == OPENBGI_DRIVE_FIXED && gF->driveType(u, "D:\\") == OPENBGI_DRIVE_NO_ROOT && gF->volumeReady(u, "c:\\"));
	CHECK(gF->specialFolder(u, 5, buf, sizeof buf) && gF->attrs(u, buf) == OPENBGI_ATTR_DIRECTORY);
	CHECK(gF->windowsDir(u, buf, sizeof buf) && gF->attrs(u, buf) == OPENBGI_ATTR_DIRECTORY);

	// a file: made, read, found by any spelling of its path
	CHECK(gF->attrs(u, "BGI.gdb") == OPENBGI_INVALID_ATTRS && gF->open(u, "BGI.gdb", OPENBGI_OPEN_READ) == NULL);
	CHECK(Put("BGI.gdb", "one") && Is("BGI.gdb", "one"));
	CHECK(Is("C:\\bgi.GDB", "one") && Is("c:/BGI.gdb", "one") && Is(".\\BGI.gdb", "one") && Is("x\\..\\BGI.gdb", "one") && Is("\\BGI.gdb", "one"));
	CHECK(gF->attrs(u, "C:\\BGI.GDB") == OPENBGI_ATTR_ARCHIVE);
	CHECK(Put("c:\\bgi.gdb", "three") && Is("BGI.gdb", "three")); // made again: emptied first
	// kept and written from its start; written past its end
	f = gF->open(u, "BGI.gdb", OPENBGI_OPEN_WRITE);
	CHECK(f != NULL && gF->length(u, f) == 5 && gF->write(u, f, "TH", 2) == 2);
	CHECK(gF->seek(u, f, 7) && gF->write(u, f, "!", 1) == 1 && gF->length(u, f) == 8);
	CHECK(gF->seek(u, f, 0) && gF->read(u, f, buf, 64) == 8 && memcmp(buf, "THree\0\0!", 8) == 0 && gF->read(u, f, buf, 64) == 0);
	CHECK(gF->setTime(u, f, 0x1234, 0x01d0) && gF->getTime(u, f, &lo, &hi) && lo == 0x1234 && hi == 0x01d0);
	gF->close(u, f);
	f = gF->open(u, "new.dat", OPENBGI_OPEN_WRITE); // not there: made
	CHECK(f != NULL && gF->length(u, f) == 0 && gF->getTime(u, f, &lo, &hi) && hi > 0x01d00000u);
	gF->close(u, f);

	// folders: one must be there for a file, and is made once
	CHECK(!Put("UserData\\save.dat", "x") && gF->makeDir(u, "UserData") && !gF->makeDir(u, "userdata") && !gF->makeDir(u, "a\\b"));
	CHECK(gF->attrs(u, "C:\\USERDATA\\") == OPENBGI_ATTR_DIRECTORY && gF->open(u, "UserData", OPENBGI_OPEN_READ) == NULL && !Put("UserData", "x"));
	CHECK(Put("UserData\\save.dat", "save") && Put("UserData\\Save2.DAT", "second") && Put("UserData\\note.txt", "note"));
	CHECK(gF->makeDir(u, "UserData\\sub") && Put("userdata/sub/deep.dat", "deep"));

	// listings: the names as they were spelt, by pattern
	Found("UserData\\*.dat", buf, sizeof buf);
	CHECK(strcmp(buf, "save.dat Save2.DAT ") == 0);
	Found("C:\\userdata\\*.*", buf, sizeof buf);
	CHECK(strcmp(buf, "save.dat Save2.DAT note.txt /sub ") == 0);
	Found("UserData\\s???.dat", buf, sizeof buf);
	CHECK(strcmp(buf, "save.dat ") == 0);
	Found("UserData\\*", buf, sizeof buf);
	CHECK(strcmp(buf, "save.dat Save2.DAT note.txt /sub ") == 0);
	Found("UserData\\nothing*", buf, sizeof buf);
	CHECK(buf[0] == 0);
	Found("*.gdb", buf, sizeof buf);
	CHECK(strcmp(buf, "BGI.gdb ") == 0);

	// copied, moved, removed
	CHECK(gF->copy(u, "UserData\\save.dat", "UserData\\copy.dat", 1) && Is("UserData\\copy.dat", "save"));
	CHECK(!gF->copy(u, "UserData\\note.txt", "UserData\\copy.dat", 1) && gF->copy(u, "UserData\\note.txt", "UserData\\copy.dat", 0));
	CHECK(Is("UserData\\copy.dat", "note") && !gF->copy(u, "UserData\\none", "UserData\\x", 0) && !gF->copy(u, "UserData\\sub", "UserData\\x", 0));
	CHECK(gF->move(u, "UserData\\copy.dat", "Moved.dat") && Is("moved.dat", "note") && gF->attrs(u, "UserData\\copy.dat") == OPENBGI_INVALID_ATTRS);
	CHECK(!gF->move(u, "Moved.dat", "BGI.gdb") && !gF->move(u, "Moved.dat", "nowhere\\x") && !gF->move(u, "none", "x"));
	CHECK(gF->move(u, "UserData", "Kept") && Is("Kept\\sub\\deep.dat", "deep") && Is("kept\\save.dat", "save"));
	CHECK(gF->attrs(u, "UserData") == OPENBGI_INVALID_ATTRS && gF->attrs(u, "UserData\\save.dat") == OPENBGI_INVALID_ATTRS);
	Found("*", buf, sizeof buf);
	CHECK(strstr(buf, "/Kept ") && strstr(buf, "Moved.dat ") && !strstr(buf, "UserData"));
	CHECK(!gF->move(u, "Kept", "Kept\\sub\\inside"));
	CHECK(gF->setAttrs(u, "Moved.dat", OPENBGI_ATTR_READONLY) && (gF->attrs(u, "Moved.dat") & OPENBGI_ATTR_READONLY));
	CHECK(!gF->remove(u, "Moved.dat") && !Put("Moved.dat", "x") && gF->setAttrs(u, "Moved.dat", OPENBGI_ATTR_NORMAL) && gF->remove(u, "Moved.dat"));
	CHECK(gF->attrs(u, "Moved.dat") == OPENBGI_INVALID_ATTRS && !gF->remove(u, "Moved.dat") && !gF->remove(u, "Kept"));
	CHECK(!gF->removeDir(u, "Kept\\sub") && gF->remove(u, "Kept\\sub\\deep.dat") && gF->removeDir(u, "Kept\\sub") && !gF->removeDir(u, "Kept\\sub"));
	CHECK(Put("Moved.dat", "again") && Is("Moved.dat", "again")); // a name that was given up is free

	// none of it is on the disk
	snprintf(buf, sizeof buf, "%s/BGI.gdb", gWork);
	CHECK(OS_NativeStat(buf, NULL) == OS_NATIVE_NONE);
	snprintf(buf, sizeof buf, "%s/Kept", gWork);
	CHECK(OS_NativeStat(buf, NULL) == OS_NATIVE_NONE);
}

static int gSeen;
static char gNames[256];

static void Each(void* ctx, const char* name, int isDir)
{
	size_t len = strlen(gNames);
	BGI_UNUSED(ctx);
	gSeen++;
	snprintf(gNames + len, sizeof gNames - len, "%s%s ", isDir ? "/" : "", name);
}

static void TestNative(void)
{
	void* u = gHost->user;
	char real[0x500], made[0x500], dir[0x500], sub[0x500], buf[64];
	OpenBGI_NativeStat_t st;
	FILE* fp;
	void* f;
	printf("native\n");
	snprintf(real, sizeof real, "%s/real.txt", gWork);
	snprintf(made, sizeof made, "%s/made.txt", gWork);
	snprintf(dir, sizeof dir, "%s/folder", gWork);
	snprintf(sub, sizeof sub, "%s/folder/in.txt", gWork);
	fp = fopen(real, "wb");
	CHECK(fp != NULL);
	if(!fp)
		return;
	fputs("on the disk", fp);
	fclose(fp);

	// a file of the disk: read as it is, and not to be written
	CHECK(gF->nativeStat(u, real, &st) == OPENBGI_NATIVE_FILE && st.size == 11);
	f = gF->nativeOpen(u, real, OPENBGI_OPEN_READ);
	CHECK(f != NULL && gF->length(u, f) == 11 && gF->seek(u, f, 3) && gF->read(u, f, buf, 64) == 8 && memcmp(buf, "the disk", 8) == 0);
	CHECK(f != NULL && gF->write(u, f, "x", 1) == 0);
	gF->close(u, f);

	// made: there for this host, not on the disk
	CHECK(gF->nativeStat(u, made, NULL) == OPENBGI_NATIVE_NONE && gF->nativeOpen(u, made, OPENBGI_OPEN_READ) == NULL);
	f = gF->nativeOpen(u, made, OPENBGI_OPEN_CREATE);
	CHECK(f != NULL && gF->write(u, f, "kept in memory", 14) == 14);
	gF->close(u, f);
	CHECK(gF->nativeStat(u, made, &st) == OPENBGI_NATIVE_FILE && st.size == 14 && OS_NativeStat(made, NULL) == OS_NATIVE_NONE);
	f = gF->nativeOpen(u, made, OPENBGI_OPEN_READ);
	CHECK(f != NULL && gF->read(u, f, buf, 64) == 14 && memcmp(buf, "kept in memory", 14) == 0);
	gF->close(u, f);
	CHECK(gF->nativeMkdir(u, dir) && gF->nativeMkdir(u, dir) && gF->nativeStat(u, dir, NULL) == OPENBGI_NATIVE_DIR);
	CHECK(OS_NativeStat(dir, NULL) == OS_NATIVE_NONE && gF->nativeMkdir(u, gWork) && !gF->nativeMkdir(u, real));
	f = gF->nativeOpen(u, sub, OPENBGI_OPEN_CREATE);
	CHECK(f != NULL);
	gF->close(u, f);
	CHECK(gF->nativeOpen(u, dir, OPENBGI_OPEN_CREATE) == NULL && gF->nativeOpen(u, dir, OPENBGI_OPEN_READ) == NULL);

	// a listing: the disk's and memory's
	gSeen = 0;
	gNames[0] = 0;
	CHECK(gF->nativeList(u, gWork, Each, NULL) == 3 && gSeen == 3);
	CHECK(strstr(gNames, "real.txt ") && strstr(gNames, "made.txt ") && strstr(gNames, "/folder "));
	gSeen = 0;
	gNames[0] = 0;
	CHECK(gF->nativeList(u, dir, Each, NULL) == 1 && strcmp(gNames, "in.txt ") == 0);
	CHECK(gF->nativeList(u, made, Each, NULL) == -1 && gF->nativeList(u, sub, Each, NULL) == -1);

	// a file of the disk made anew is the one in memory; removed, it is gone for this host and stays on the disk
	f = gF->nativeOpen(u, real, OPENBGI_OPEN_CREATE);
	CHECK(f != NULL && gF->write(u, f, "new", 3) == 3);
	gF->close(u, f);
	CHECK(gF->nativeStat(u, real, &st) == OPENBGI_NATIVE_FILE && st.size == 3 && OS_NativeStat(real, &st) == OS_NATIVE_FILE && st.size == 11);
	gSeen = 0;
	gNames[0] = 0;
	CHECK(gF->nativeList(u, gWork, Each, NULL) == 3); // (listed once)
	CHECK(gF->nativeRemove(u, real) && gF->nativeStat(u, real, NULL) == OPENBGI_NATIVE_NONE && gF->nativeOpen(u, real, OPENBGI_OPEN_READ) == NULL);
	CHECK(OS_NativeStat(real, NULL) == OS_NATIVE_FILE);
	gSeen = 0;
	gNames[0] = 0;
	CHECK(gF->nativeList(u, gWork, Each, NULL) == 2 && !strstr(gNames, "real.txt"));
	CHECK(gF->nativeRemove(u, made) && gF->nativeStat(u, made, NULL) == OPENBGI_NATIVE_NONE && gF->nativeRemove(u, made));
	CHECK(!gF->nativeRemove(u, dir));
	// another host sees the disk as it is
	{
		OpenBGI_Host_t* other = OsPreview_HostNew();
		CHECK(other != NULL);
		if(other)
		{
			CHECK(other->files->nativeStat(other->user, real, &st) == OPENBGI_NATIVE_FILE && st.size == 11);
			CHECK(other->files->nativeStat(other->user, dir, NULL) == OPENBGI_NATIVE_NONE);
			CHECK(other->files->attrs(other->user, "BGI.gdb") == OPENBGI_INVALID_ATTRS);
		}
		OsPreview_HostFree(other);
	}
	remove(real);
}

static void TestHost(void)
{
	void* u = gHost->user;
	char a[0x500], b[0x500], dir[0x500];
	OpenBGI_DateTime_t t;
	int64_t ns = -1;
	printf("host\n");
	CHECK(gHost->size == sizeof *gHost && gHost->window == NULL && gHost->shell == NULL && gHost->registry == NULL);
	CHECK(gHost->ticksMs(u) == 0 && gHost->ticksMs(u) == 0 && gHost->perfNs(u, &ns) && ns == 0);
	memset(&t, 0, sizeof t);
	gHost->localTime(u, &t);
	CHECK(t.year >= 2024 && t.month >= 1 && t.month <= 12);
	// the user's data: a folder of the run's own, which is not on the disk
	gF->userDataDir(u, a, sizeof a);
	OS_UserDataDir(b, sizeof b);
	CHECK(strlen(a) > strlen(b) && strncmp(a, b, strlen(b)) == 0 && strstr(a + strlen(b), "preview"));
	// the game directory: a folder that is there, in memory or on the disk
	snprintf(dir, sizeof dir, "%sgames%cone", a, gF->nativeSep);
	CHECK(!gF->setGameDir(u, dir));
	snprintf(b, sizeof b, "%sgames", a);
	CHECK(gF->nativeMkdir(u, a) && gF->nativeMkdir(u, b) && gF->nativeMkdir(u, dir) && gF->setGameDir(u, dir));
	gF->gameDir(u, b, sizeof b);
	CHECK(strncmp(b, dir, strlen(dir)) == 0 && b[strlen(dir)] == gF->nativeSep && b[strlen(dir) + 1] == 0);
	CHECK(gF->setGameDir(u, gWork) && !gF->setGameDir(u, ""));
	gF->toUtf8(u, "caf\xc3\xa9", a, sizeof a);
	gF->fromUtf8(u, a, b, sizeof b);
	CHECK(strcmp(a, "caf\xc3\xa9") == 0 && strcmp(b, a) == 0 && !gF->nativeSjis);
}

int main(void)
{
	TestHost_ScratchName(gDir, sizeof gDir, "openbgi_preview");
	if(TestHost_Mkdir(gDir) != 0)
	{
		printf("preview: no scratch directory\n");
		return 1;
	}
	snprintf(gWork, sizeof gWork, "%s/work", gDir);
	if(TestHost_Mkdir(gWork) != 0)
		return 1;
	TestHost_SetEnv("HOME", gDir); // (the user's data folder, which the host asks for, is the test's own where the system goes by these)
	TestHost_SetEnv("XDG_DATA_HOME", gDir);
	OS_Init();
	gHost = OsPreview_HostNew();
	if(!gHost)
	{
		printf("preview: no host\n");
		return 1;
	}
	gF = gHost->files;
	TestGame();
	TestNative();
	TestHost();
	OsPreview_HostFree(gHost);
	OS_Shutdown();
	TestHost_RemoveTree(gDir);
	if(gFails)
	{
		printf("preview: %d checks failed\n", gFails);
		return 1;
	}
	printf("preview: all tests passed\n");
	return 0;
}
