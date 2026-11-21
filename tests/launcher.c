/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/launcher_src.c - the launcher's sources (src/sys/launcher_src.c): written
 *                  into the user's data folder, kept up to date there, and
 *                  compiled
 *
 *   fresh    - an empty folder gets every file of the pack and the note
 *              of what was written; the program compiles
 *   edits    - a changed file stays, and is what is compiled; a deleted
 *              file comes back
 *   newer    - a newer pack replaces untouched files without asking; with
 *              a changed source it asks: "keep" leaves the folder alone
 *              and is not asked again for the same pack, "replace" writes
 *              the new files and keeps the old ones in a backup folder
 *   broken   - sources that do not compile are said in a box, and the
 *              pack's own are compiled instead
 *   drops    - what is dropped on the launcher's window waits as a list
 *              the program takes from: a folder without the separator
 *              after it, in the order dropped, no more than the list
 *              holds, and nothing while no launcher runs
 *   pictures - the games the program lists wait for the host's run:
 *              one at a time, for the icon of the game's own program
 *              (not the setup's or the uninstaller's beside it) and for
 *              a first picture; the settings are written before a game
 *              is handed out and not while it is out; a picture that
 *              comes in is kept and counted, a run that showed nothing
 *              is not asked for again, a run that was given up is; no
 *              pictures with --preview=none or the launcher's setting
 *   reading  - the work a host does beside the frames: a folder's listing
 *              and a look at a game as jobs that are taken, run with
 *              another engine and handed back; a listing for a search
 *              puts the looks at its games and the listings of its
 *              folders into the queue ahead, and the program's asking
 *              finds them there, done or not; what a thread waits for
 *              goes out first; a job given back waits again; what was
 *              made ahead and never asked for goes with the next search;
 *              a job dropped while it is out is freed when it comes back
 *
 * The user's data folder is one of the test's own, in the scratch
 * directory of the test run (BGI_TEST_TMP, else the current directory).
 */
#include "bgi/host.h"
#include "bgi/core.h"
#include "bgi/request.h"
#include "bgi/launcher.h"
#include "bgi/wait.h"
#include "bgi/version.h"
#include "host/os.h"

#include <stdio.h>

#include "check.h"
#include "iconexe.h"

static OpenBGI_Host_t gHost;
static OpenBGI_Files_t gFiles;
static char gData[0x400];   // the user's data folder of the test, a separator after it
static char gFolder[0x440]; // the sources' folder in it, a separator after it
static uint8_t gPack[0x1000];
static char gBackup[64]; // the backup folder the listing found

static void DataDir(void* user, char* buf, size_t n)
{
	BGI_UNUSED(user);
	snprintf(buf, n, "%s", gData);
}

#define MAIN "#engine 1.669\n#include \"a.inc\"\nstart() {\n    int x;\n    x = VALUE;\n    return x;\n}\n"

// a pack of the two sources and a text, as tools/mkpack.py makes it (stored as they are)
static uint32_t MakePack(const char* value)
{
	const char* names[3] = {"main.bpc", "a.inc", "README.txt"};
	const char* texts[3];
	char inc[64];
	uint32_t at = 12;
	int i;
	snprintf(inc, sizeof inc, "#define VALUE %s\n", value);
	texts[0] = MAIN;
	texts[1] = inc;
	texts[2] = "read me\n";
	memcpy(gPack, "OBGIPACK", 8);
	gPack[8] = 3;
	gPack[9] = gPack[10] = gPack[11] = 0;
	for(i = 0; i < 3; i++)
	{
		uint32_t n = (uint32_t)strlen(texts[i]), k;
		gPack[at++] = (uint8_t)strlen(names[i]);
		memcpy(gPack + at, names[i], strlen(names[i]));
		at += (uint32_t)strlen(names[i]);
		gPack[at++] = 0;
		for(k = 0; k < 2; k++)
		{
			gPack[at++] = (uint8_t)n;
			gPack[at++] = (uint8_t)(n >> 8);
			gPack[at++] = 0;
			gPack[at++] = 0;
		}
		memcpy(gPack + at, texts[i], n);
		at += n;
	}
	return at;
}

static void PathOf(const char* name, char* out, size_t n)
{
	snprintf(out, n, "%s%s", gFolder, name);
}

// whether the folder's file `name` is `text`
static int FileIs(OpenBGI_t* o, const char* name, const char* text)
{
	char path[0x500];
	uint32_t size = 0;
	uint8_t* d;
	int same;
	PathOf(name, path, sizeof path);
	d = Host_ReadNativeFile(o, path, &size);
	same = d && size == strlen(text) && memcmp(d, text, size) == 0;
	free(d);
	return same;
}

static void Put(OpenBGI_t* o, const char* name, const char* text)
{
	char path[0x500];
	PathOf(name, path, sizeof path);
	CHECK(Host_WriteNativeFile(o, path, text, (uint32_t)strlen(text)));
}

static void Listed(void* ctx, const char* name, int isDir)
{
	BGI_UNUSED(ctx);
	if(isDir && strncmp(name, "backup-", 7) == 0)
		snprintf(gBackup, sizeof gBackup, "%s", name);
}

// what an earlier run left: the files, and those of its backup folders
static void Sweep(OpenBGI_t* o)
{
	const char* names[4] = {"main.bpc", "a.inc", "README.txt", LAUNCHER_WRITTEN};
	char path[0x500];
	int i, k;
	for(k = 0; k < 8; k++)
	{
		gBackup[0] = 0;
		Host_NativeList(o, gFolder, Listed, NULL);
		for(i = 0; i < 4; i++)
		{
			if(gBackup[0])
				snprintf(path, sizeof path, "%s%s%c%s", gFolder, gBackup, Host_NativeSep(o), names[i]);
			else
				PathOf(names[i], path, sizeof path);
			Host_NativeRemove(o, path);
		}
		if(!gBackup[0])
			break;
		snprintf(path, sizeof path, "%s%s", gFolder, gBackup);
		if(!OS_DirRemove(path))
			break;
	}
	// and the two folders themselves, by their names without the separator
	snprintf(path, sizeof path, "%s", gFolder);
	path[strlen(path) - 1] = 0;
	OS_DirRemove(path);
	snprintf(path, sizeof path, "%s", gData);
	path[strlen(path) - 1] = 0;
	OS_DirRemove(path);
}

// Launcher_Prepare with the pack of `value`; what it returned
static int Prepare(OpenBGI_t* o, const char* value)
{
	OpenBGI_SetLauncher(o, gPack, MakePack(value));
	return Launcher_Prepare(o);
}

// the boot's compile, without the run: 1 when a program came of it
static int Compiles(OpenBGI_t* o)
{
	int ok = Launcher_Boot(o) && o->launcher.compiled != NULL;
	Launcher_SourcesFree(o);
	o->launcher.running = 0;
	return ok;
}

static void TestFresh(OpenBGI_t* o)
{
	char path[0x500];
	CHECK(Launcher_Wanted(o) == 0);
	CHECK(Prepare(o, "1") == 0 && Launcher_Wanted(o));
	CHECK(FileIs(o, "main.bpc", MAIN) && FileIs(o, "a.inc", "#define VALUE 1\n") && FileIs(o, "README.txt", "read me\n"));
	PathOf(LAUNCHER_WRITTEN, path, sizeof path);
	CHECK(Host_NativeStat(o, path, NULL) == OPENBGI_NATIVE_FILE);
	CHECK(Compiles(o) && !Request_Pending(o));
}

static void TestEdits(OpenBGI_t* o)
{
	char path[0x500];
	Put(o, "a.inc", "#define VALUE 20\n");
	CHECK(Prepare(o, "1") == 0 && FileIs(o, "a.inc", "#define VALUE 20\n")); // the same pack: the change stays
	CHECK(Compiles(o) && !Request_Pending(o));
	PathOf("main.bpc", path, sizeof path);
	CHECK(Host_NativeRemove(o, path));
	CHECK(Prepare(o, "1") == 0 && FileIs(o, "main.bpc", MAIN) && FileIs(o, "a.inc", "#define VALUE 20\n")); // a deleted file comes back
	Launcher_SourcesFree(o);
}

static void TestNewer(OpenBGI_t* o)
{
	char path[0x500];
	// a newer pack and a changed source: asked; "keep"
	CHECK(Prepare(o, "2") == 1 && o->launcher.askReq != 0 && Request_Pending(o) == 1);
	Request_Forget(o, o->launcher.askReq);
	Launcher_Answer(o, 0);
	CHECK(FileIs(o, "a.inc", "#define VALUE 20\n"));
	Request_Clear(o);
	CHECK(Prepare(o, "2") == 0 && !Request_Pending(o) && FileIs(o, "a.inc", "#define VALUE 20\n")); // not asked again for this pack
	// the next pack: asked again; "replace"
	CHECK(Prepare(o, "3") == 1 && Request_Pending(o) == 1);
	Request_Forget(o, o->launcher.askReq);
	Launcher_Answer(o, 1);
	Request_Clear(o);
	CHECK(FileIs(o, "a.inc", "#define VALUE 3\n") && FileIs(o, "main.bpc", MAIN));
	gBackup[0] = 0;
	Host_NativeList(o, gFolder, Listed, NULL);
	CHECK(gBackup[0] != 0);
	snprintf(path, sizeof path, "%s%c%s", gBackup, Host_NativeSep(o), "a.inc");
	CHECK(FileIs(o, path, "#define VALUE 20\n")); // the old file is kept
	CHECK(Compiles(o));
	// a newer pack still, and nothing changed: replaced without a question
	CHECK(Prepare(o, "4") == 0 && !Request_Pending(o) && FileIs(o, "a.inc", "#define VALUE 4\n"));
	Launcher_SourcesFree(o);
}

static void TestBroken(OpenBGI_t* o)
{
	Put(o, "a.inc", "#define VALUE (\n");
	CHECK(Prepare(o, "4") == 0 && FileIs(o, "a.inc", "#define VALUE (\n"));
	CHECK(Compiles(o));             // the pack's own
	CHECK(Request_Pending(o) == 1); // and a box that says so
	Request_Clear(o);
	Put(o, "a.inc", "#define VALUE 5\n"); // mended
	CHECK(Prepare(o, "4") == 0 && Compiles(o) && !Request_Pending(o));
}

static void TestDrops(OpenBGI_t* o)
{
	char buf[0x400], path[0x500];
	char sep = Host_NativeSep(o);
	int i;
	Launcher_Drop(o, gData); // no launcher runs: not kept
	CHECK(Launcher_Dropped(o, 0, buf, sizeof buf) == -1);
	o->launcher.running = 1;
	Launcher_Drop(o, gData); // (ends in a separator)
	snprintf(path, sizeof path, "%sgame.zip", gData);
	Launcher_Drop(o, path);
	Launcher_Drop(o, "");
	CHECK(Launcher_Dropped(o, 0, buf, sizeof buf) == (int)strlen(gData) - 1 && strncmp(buf, gData, strlen(gData) - 1) == 0 && buf[strlen(buf) - 1] != sep);
	CHECK(Launcher_Dropped(o, 1, buf, sizeof buf) == (int)strlen(path) && strcmp(buf, path) == 0);
	CHECK(Launcher_Dropped(o, 2, buf, sizeof buf) == -1 && Launcher_Dropped(o, -1, buf, sizeof buf) == -1);
	CHECK(Launcher_Dropped(o, 1, buf, 4) == 3 && strlen(buf) == 3); // (cut to the room)
	CHECK(Launcher_DropTake(o, 0) == 1);                            // the first is taken: the second is the first now
	CHECK(Launcher_Dropped(o, 0, buf, sizeof buf) == (int)strlen(path) && strcmp(buf, path) == 0);
	CHECK(Launcher_DropTake(o, 1) == 0 && Launcher_DropTake(o, 0) == 1 && Launcher_DropTake(o, 0) == 0);
	for(i = 0; i < LAUNCHER_DROPS + 5; i++)
		Launcher_Drop(o, path); // more than the list holds: the rest is not kept
	CHECK(Launcher_Dropped(o, LAUNCHER_DROPS - 1, buf, sizeof buf) > 0 && Launcher_Dropped(o, LAUNCHER_DROPS, buf, sizeof buf) == -1);
	for(i = 0; i < LAUNCHER_DROPS; i++)
		CHECK(Launcher_DropTake(o, 0) == 1);
	CHECK(Launcher_DropTake(o, 0) == 0);
	o->launcher.running = 0;
}

// a file of the test's data folder: made from `data`, or (NULL) removed
static void DataFile(OpenBGI_t* o, const char* dir, const char* name, const uint8_t* data, size_t n)
{
	char path[0x600];
	snprintf(path, sizeof path, "%s%s%c%s", gData, dir, Host_NativeSep(o), name);
	if(data)
		CHECK(Host_WriteNativeFile(o, path, data, (uint32_t)n));
	else
		Host_NativeRemove(o, path);
}

static int DataThere(OpenBGI_t* o, const char* dir, const char* name)
{
	char path[0x600];
	snprintf(path, sizeof path, "%s%s%c%s", gData, dir, Host_NativeSep(o), name);
	return Host_NativeStat(o, path, NULL) == OPENBGI_NATIVE_FILE;
}

static void TestPictures(OpenBGI_t* o)
{
	static uint8_t exe[0x2000];
	static uint32_t pix[64 * 36];
	static const char* const kSide[4] = {"Setup.exe", "UnInstaller.exe", "BGIForInstalling.exe", "BHVC.exe"};
	const int one[1] = {16};
	char game[0x500], other[0x500], third[0x500], cmd[0x1000], icon[64], picture[64], value[64];
	OpenBGI_Frame_t f = {.size = sizeof f};
	size_t n, size = 0;
	uint8_t* ico;
	int i;
	snprintf(game, sizeof game, "%sgame1", gData);
	snprintf(other, sizeof other, "%sgame2", gData);
	snprintf(third, sizeof third, "%sgame3", gData);
	Host_NativeMkdir(o, game);
	Host_NativeMkdir(o, other);
	// the first game: its program (which names the boot program), a tool, and the programs that come with a setup
	n = IconExe_Make(exe, 1, one, 0x50, 0, -1, NULL);
	DataFile(o, "game1", "Album.exe", exe, n);
	n = IconExe_Make(exe, 1, one, 0x20, 0, -1, "ipl._bp");
	DataFile(o, "game1", "Heroine.exe", exe, n);
	n = IconExe_Make(exe, 1, one, 0x90, 0, -1, "ipl._bp");
	for(i = 0; i < 4; i++)
		DataFile(o, "game1", kSide[i], exe, n);
	DataFile(o, "game1", "note.exe", (const uint8_t*)"MZ, but no program", 18);
	DataFile(o, "game2", "Setup.exe", exe, n); // the second has no program of its own
	ico = Launcher_GameIcon(o, game, &size);
	CHECK(ico != NULL && size == 22 + 40 + 16 * 16 * 4 + 64 && ico[22 + 40] == 0x20); // (the first pixel's blue)
	BGI_Free(ico);
	ico = Launcher_GameIcon(o, other, &size);
	CHECK(ico == NULL && size == 0);
	BGI_Free(ico);
	CHECK(Launcher_GameIcon(o, third, &size) == NULL); // nothing there at all

	// listed: nothing while no launcher runs; then once.  The game waits for its icon and its picture
	CHECK(Launcher_Listed(o, game) == 0);
	o->launcher.running = 1;
	snprintf(o->engine.gamesPath, sizeof o->engine.gamesPath, "%ssettings.json", gData); // (where this test's settings are written)
	CHECK(Launcher_Listed(o, game) == 1 && Launcher_Listed(o, game) == 0);
	CHECK(Launcher_Listed(o, other) == 1);
	CHECK(Launcher_Listed(o, "") == 0 && Launcher_Listed(o, "a\"b") == 0); // (what cannot stand on a command line)
	CHECK(Launcher_PreviewCount(o, value, sizeof value) == 1 && strcmp(value, "0") == 0);
	CHECK(Engine_CfgGet(o, game, "icon", 0, icon, sizeof icon) <= 0); // (nothing is read on the launcher's own thread)

	// the host takes one game at a time, and the settings are written before it gets one
	Launcher_SettingsChanged(o);
	CHECK(!DataThere(o, ".", "settings.json"));
	CHECK(OpenBGI_PreviewNext(o, cmd, sizeof cmd) == (OPENBGI_PREVIEW_PICTURE | OPENBGI_PREVIEW_ICON));
	CHECK(strstr(cmd, "--preview ") && strstr(cmd, game) && cmd[strlen(cmd) - 1] == '"');
	CHECK(DataThere(o, ".", "settings.json") && !o->launcher.cfgChanged);
	CHECK(OpenBGI_PreviewNext(o, cmd, sizeof cmd) == 0);
	// .. and not again while the game is out, however long ago they changed
	DataFile(o, ".", "settings.json", NULL, 0);
	Launcher_SettingsChanged(o);
	o->launcher.cfgSaveMs = o->host.ticksMs(o->host.user) - 1;
	Launcher_Frame(o, &f);
	CHECK(!DataThere(o, ".", "settings.json") && o->launcher.cfgChanged);

	// the icon the host's engine found, and the picture of its run
	ico = Launcher_GameIcon(o, game, &size);
	OpenBGI_PreviewIcon(o, ico, size);
	BGI_Free(ico);
	CHECK(Engine_CfgGet(o, game, "icon", 0, icon, sizeof icon) > 4 && strcmp(icon + strlen(icon) - 4, ".ico") == 0);
	CHECK(DataThere(o, LAUNCHER_ICONS, icon));
	for(i = 0; i < 64 * 36; i++)
		pix[i] = (uint32_t)i * 0x010203u;
	f.pixels = (const uint8_t*)pix;
	f.width = 64;
	f.height = 36;
	f.pitch = 64 * 4;
	f.bpp = 32;
	OpenBGI_PreviewDone(o, &f);
	CHECK(Engine_CfgGet(o, game, "picture", 0, picture, sizeof picture) > 4 && DataThere(o, LAUNCHER_PICTURES, picture));
	CHECK(Engine_CfgGet(o, game, "preview", 0, value, sizeof value) <= 0);
	CHECK(Launcher_PreviewCount(o, value, sizeof value) == 1 && strcmp(value, "1") == 0);
	o->launcher.cfgSaveMs = o->host.ticksMs(o->host.user) - 1;
	f.pixels = NULL;
	Launcher_Frame(o, &f); // the game is back: the settings are written when their time has come
	CHECK(DataThere(o, ".", "settings.json") && !o->launcher.cfgChanged);

	// the second has no icon, and its run showed nothing: both said in its settings, nothing counted
	CHECK(OpenBGI_PreviewNext(o, cmd, sizeof cmd) == (OPENBGI_PREVIEW_PICTURE | OPENBGI_PREVIEW_ICON) && strstr(cmd, other));
	OpenBGI_PreviewIcon(o, NULL, 0);
	OpenBGI_PreviewDone(o, &f);
	CHECK(Engine_CfgGet(o, other, "icon", 0, value, sizeof value) > 0 && strcmp(value, "none") == 0);
	CHECK(Engine_CfgGet(o, other, "preview", 0, value, sizeof value) > 0 && strcmp(value, "none") == 0);
	CHECK(Engine_CfgGet(o, other, "picture", 0, value, sizeof value) <= 0);
	CHECK(Launcher_PreviewCount(o, value, sizeof value) == 1 && strcmp(value, "1") == 0);
	OpenBGI_PreviewIcon(o, "x", 1); // (no game is out: nothing happens)
	OpenBGI_PreviewDone(o, &f);
	CHECK(Engine_CfgGet(o, other, "icon", 0, value, sizeof value) > 0 && strcmp(value, "none") == 0);

	// the third's run is given up: nothing is said of it
	CHECK(Launcher_Listed(o, third) == 1 && OpenBGI_PreviewNext(o, cmd, sizeof cmd) != 0 && strstr(cmd, third));
	OpenBGI_PreviewDone(o, NULL);
	CHECK(Engine_CfgGet(o, third, "preview", 0, value, sizeof value) <= 0 && Engine_CfgGet(o, third, "picture", 0, value, sizeof value) <= 0);
	CHECK(Engine_CfgGet(o, third, "icon", 0, value, sizeof value) <= 0 && OpenBGI_PreviewNext(o, cmd, sizeof cmd) == 0);

	// the next time the launcher runs: only the third waits again, and the first's icon is not looked for anew
	free(o->launcher.listed);
	o->launcher.listed = NULL;
	o->launcher.listedCount = o->launcher.listedCap = 0;
	DataFile(o, LAUNCHER_ICONS, icon, NULL, 0);
	CHECK(Launcher_Listed(o, game) == 1 && Launcher_Listed(o, other) == 1 && Launcher_Listed(o, third) == 1);
	CHECK(OpenBGI_PreviewNext(o, cmd, sizeof cmd) == (OPENBGI_PREVIEW_PICTURE | OPENBGI_PREVIEW_ICON) && strstr(cmd, third));
	CHECK(!DataThere(o, LAUNCHER_ICONS, icon));
	OpenBGI_PreviewDone(o, NULL);
	CHECK(OpenBGI_PreviewNext(o, cmd, sizeof cmd) == 0);
	// a game that is run under another profile, with a patch and options, is run so for its picture
	Engine_CfgSet(o, third, "profile", 0, "1.64");
	Engine_CfgSet(o, third, "patches", 0, "fix.zip");
	Engine_CfgSet(o, third, "options", 0, "--movies=decoder");
	free(o->launcher.listed);
	o->launcher.listed = NULL;
	o->launcher.listedCount = o->launcher.listedCap = 0;
	CHECK(Launcher_Listed(o, third) == 1 && OpenBGI_PreviewNext(o, cmd, sizeof cmd) != 0);
	CHECK(strstr(cmd, "--engine=1.64 ") && strstr(cmd, "--patch=\"fix.zip\" ") && strstr(cmd, "--movies=decoder ") && strstr(cmd, "--preview "));
	OpenBGI_PreviewDone(o, NULL);

	// no first pictures: by the command line, or by the launcher's settings.  The icon is still wanted
	o->engine.optPreviewNone = 1;
	free(o->launcher.listed);
	o->launcher.listed = NULL;
	o->launcher.listedCount = o->launcher.listedCap = 0;
	CHECK(Launcher_PreviewsOff(o) && Launcher_Listed(o, third) == 1 && OpenBGI_PreviewNext(o, cmd, sizeof cmd) == OPENBGI_PREVIEW_ICON);
	OpenBGI_PreviewIcon(o, NULL, 0);
	OpenBGI_PreviewDone(o, &f); // (no picture was wanted: none is missed)
	CHECK(Engine_CfgGet(o, third, "icon", 0, value, sizeof value) > 0 && Engine_CfgGet(o, third, "preview", 0, value, sizeof value) <= 0);
	free(o->launcher.listed);
	o->launcher.listed = NULL;
	o->launcher.listedCount = o->launcher.listedCap = 0;
	CHECK(Launcher_Listed(o, third) == 1 && OpenBGI_PreviewNext(o, cmd, sizeof cmd) == 0); // nothing is wanted of it now
	o->engine.optPreviewNone = 0;
	CHECK(!Launcher_PreviewsOff(o));
	Engine_CfgSet(o, "", "preview", 0, "none");
	CHECK(Launcher_PreviewsOff(o));
	Engine_CfgSet(o, "", "preview", 0, NULL);
	CHECK(!Launcher_PreviewsOff(o));

	// outside the launcher nothing is handed out or kept
	o->launcher.running = 0;
	CHECK(OpenBGI_PreviewNext(o, cmd, sizeof cmd) == 0);
	free(o->launcher.listed);
	o->launcher.listed = NULL;
	o->launcher.listedCount = o->launcher.listedCap = 0;
	o->launcher.cfgChanged = 0; // (the settings of this test are written nowhere)

	// the window's icon: the launched game's as it was kept, none for a game without one
	DataFile(o, LAUNCHER_ICONS, "kept.ico", (const uint8_t*)"icon bytes", 10);
	o->launcher.launched = 1;
	snprintf(o->launcher.gameIcon, sizeof o->launcher.gameIcon, "kept.ico");
	ico = Launcher_RunIcon(o, &size);
	CHECK(ico != NULL && size == 10 && memcmp(ico, "icon bytes", 10) == 0);
	BGI_Free(ico);
	snprintf(o->launcher.gameIcon, sizeof o->launcher.gameIcon, "none");
	CHECK(Launcher_RunIcon(o, &size) == NULL && size == 0);
	o->launcher.running = 1; // the launcher itself: OpenBGI's own
	CHECK(Launcher_RunIcon(o, &size) == NULL);
	o->launcher.running = 0;
	o->launcher.launched = 0;
	o->launcher.gameIcon[0] = 0;

	DataFile(o, LAUNCHER_ICONS, "kept.ico", NULL, 0);
	DataFile(o, ".", "settings.json", NULL, 0);
	o->engine.gamesPath[0] = 0;
	DataFile(o, LAUNCHER_PICTURES, picture, NULL, 0);
	DataFile(o, "game1", "Album.exe", NULL, 0);
	DataFile(o, "game1", "Heroine.exe", NULL, 0);
	DataFile(o, "game1", "note.exe", NULL, 0);
	for(i = 0; i < 4; i++)
		DataFile(o, "game1", kSide[i], NULL, 0);
	DataFile(o, "game2", "Setup.exe", NULL, 0);
	OS_DirRemove(game);
	OS_DirRemove(other);
	snprintf(cmd, sizeof cmd, "%s" LAUNCHER_ICONS, gData);
	OS_DirRemove(cmd);
	snprintf(cmd, sizeof cmd, "%s" LAUNCHER_PICTURES, gData);
	OS_DirRemove(cmd);
}

// every job that waits, taken, run with the engine `w` and handed back; how many
static int RunJobs(OpenBGI_t* o, OpenBGI_t* w)
{
	OpenBGI_Job_t* job;
	int n = 0;
	while((job = OpenBGI_JobNext(o)) != NULL)
	{
		OpenBGI_JobRun(job, w);
		OpenBGI_JobDone(o, job);
		n++;
	}
	return n;
}

static void TestReading(OpenBGI_t* o)
{
	static const char* const kFiles[5][2] = {{"place", "a.zip"}, {"place", "b.iso"}, {"sub1", "system.arc"}, {"sub2", "readme.txt"}, {"sub3", "ipl._bp"}};
	static const uint8_t kBoot[20] = {16, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x17, 0, 0, 0}; // a boot program: its header, the end of the thread
	OpenBGI_t* w = OpenBGI_New(&gHost);                                                                // the engine a host reads with
	char sep = Host_NativeSep(o);
	char place[0x500], sub[3][0x540], zip[0x540], iso[0x540], path[0x600];
	struct OpenBGI_Job *top, *job, *a, *b;
	LauncherGame_t g;
	Wait_t waiter;
	int i, h;
	CHECK(w != NULL);
	if(!w)
		return;
	memset(&waiter, 0, sizeof waiter);
	snprintf(place, sizeof place, "%splace", gData);
	Host_NativeMkdir(o, place);
	for(i = 0; i < 3; i++)
	{
		snprintf(sub[i], sizeof sub[i], "%s%csub%d", place, sep, i + 1);
		Host_NativeMkdir(o, sub[i]);
	}
	snprintf(zip, sizeof zip, "%s%ca.zip", place, sep);
	snprintf(iso, sizeof iso, "%s%cb.iso", place, sep);
	for(i = 0; i < 5; i++)
	{
		snprintf(path, sizeof path, "%s%s%s%c%s", place, i < 2 ? "" : (sep == '\\' ? "\\" : "/"), i < 2 ? "" : kFiles[i][0], sep, kFiles[i][1]);
		CHECK(i == 4 ? Host_WriteNativeFile(o, path, kBoot, sizeof kBoot) : Host_WriteNativeFile(o, path, "no archive, no program", 22));
	}

	// no such host, or no launcher: no jobs
	CHECK(!Launcher_JobsOn(o));
	o->launcher.running = 1;
	CHECK(Launcher_JobsOn(o) == ((o->host.caps & OPENBGI_CAP_JOBS) != 0));
	if(!Launcher_JobsOn(o))
	{ // (a build whose host does not take them)
		o->launcher.running = 0;
		OpenBGI_Delete(w);
		return;
	}
	snprintf(o->engine.gamesPath, sizeof o->engine.gamesPath, "%ssettings.json", gData);

	// a listing for a search: taken, run, handed back; the settings are written before it goes out
	CHECK(OpenBGI_JobNext(o) == NULL);
	top = Launcher_JobFor(o, LAUNCHER_JOB_LIST, place, LAUNCHER_DIR_GAMES | LAUNCHER_DIR_DEEPER);
	CHECK(top != NULL && !Launcher_JobReady(top));
	Launcher_SettingsChanged(o);
	job = OpenBGI_JobNext(o);
	CHECK(job == top && OpenBGI_JobNext(o) == NULL && DataThere(o, ".", "settings.json") && !o->launcher.cfgChanged);
	// .. and not while it is out
	Launcher_SettingsChanged(o);
	o->launcher.cfgSaveMs = o->host.ticksMs(o->host.user) - 1;
	DataFile(o, ".", "settings.json", NULL, 0);
	{
		OpenBGI_Frame_t f = {.size = sizeof f};
		Launcher_Frame(o, &f);
		CHECK(!DataThere(o, ".", "settings.json") && o->launcher.cfgChanged);
	}
	// given back without being run: it waits again
	OpenBGI_JobBack(o, job);
	CHECK(!Launcher_JobReady(top) && OpenBGI_JobNext(o) == top);
	OpenBGI_JobRun(top, w);
	OpenBGI_JobDone(o, top);
	CHECK(Launcher_JobReady(top));
	h = Launcher_JobTake(o, top, NULL);
	CHECK(h > 0 && Launcher_DirCount(o, h) == 5);
	CHECK(Launcher_DirEntry(o, h, 0, path, sizeof path) == LAUNCHER_ENTRY_FOLDER && strcmp(path, "sub1") == 0);
	CHECK(Launcher_DirEntry(o, h, 3, path, sizeof path) == LAUNCHER_ENTRY_FILE && strcmp(path, "a.zip") == 0);
	Launcher_DirClose(o, h);

	// what it puts into the queue ahead: its folders' listings and the looks at its archives.
	// The program asks for the third folder first, and a thread waits for it: that one goes out first
	job = Launcher_JobFor(o, LAUNCHER_JOB_LIST, sub[2], LAUNCHER_DIR_GAMES);
	CHECK(job != NULL && !Launcher_JobReady(job));
	Launcher_JobWait(job, &waiter);
	CHECK(OpenBGI_JobNext(o) == job);
	OpenBGI_JobRun(job, w);
	OpenBGI_JobDone(o, job);
	h = Launcher_JobTake(o, job, NULL);
	CHECK(h > 0 && Launcher_DirCount(o, h) == 1);
	Launcher_DirClose(o, h);
	// the rest, in one go: two listings, two looks at archives, and the looks at the two folders that are games by their files
	CHECK(RunJobs(o, w) == 6 && OpenBGI_JobNext(o) == NULL);
	// the program's asking finds them done
	a = Launcher_JobFor(o, LAUNCHER_JOB_PROBE, zip, 0);
	CHECK(a != NULL && Launcher_JobReady(a) && Launcher_JobTake(o, a, &g) == 0); // (no archive at all)
	job = Launcher_JobFor(o, LAUNCHER_JOB_PROBE, sub[2], 0);
	memset(&g, 0, sizeof g);
	CHECK(job != NULL && Launcher_JobReady(job) && Launcher_JobTake(o, job, &g) == 1 && g.kind == LAUNCHER_GAME_FOLDER && g.profile[0]);
	job = Launcher_JobFor(o, LAUNCHER_JOB_PROBE, sub[0], 0);
	CHECK(job != NULL && Launcher_JobReady(job) && Launcher_JobTake(o, job, &g) == 0); // (system.arc without a boot program)
	job = Launcher_JobFor(o, LAUNCHER_JOB_LIST, sub[0], LAUNCHER_DIR_GAMES);
	CHECK(job != NULL && Launcher_JobReady(job));
	h = Launcher_JobTake(o, job, NULL);
	CHECK(h > 0);
	Launcher_DirClose(o, h);
	// taken once: asked for again, it is read again
	a = Launcher_JobFor(o, LAUNCHER_JOB_PROBE, zip, 0);
	CHECK(a != NULL && !Launcher_JobReady(a));
	Launcher_JobDrop(o, a);
	CHECK(OpenBGI_JobNext(o) == NULL);

	// the next search: what the one before left unasked (the second folder's listing, the look at the image) goes
	top = Launcher_JobFor(o, LAUNCHER_JOB_LIST, place, LAUNCHER_DIR_GAMES | LAUNCHER_DIR_DEEPER);
	b = Launcher_JobFor(o, LAUNCHER_JOB_PROBE, iso, 0);
	CHECK(top != NULL && b != NULL && !Launcher_JobReady(b));
	// a job that is dropped while the host has it is freed when it comes back; one that waits, at once
	CHECK(OpenBGI_JobNext(o) == top);
	Launcher_JobDrop(o, top);
	Launcher_JobDrop(o, b);
	CHECK(OpenBGI_JobNext(o) == NULL);
	OpenBGI_JobRun(top, w);
	OpenBGI_JobDone(o, top);
	CHECK(OpenBGI_JobNext(o) == NULL && o->launcher.jobsOut == 0);
	// a folder that is not there; and a run that ends with jobs in the queue and one out
	job = Launcher_JobFor(o, LAUNCHER_JOB_LIST, zip, 0);
	CHECK(RunJobs(o, w) == 1 && Launcher_JobReady(job) && Launcher_JobTake(o, job, NULL) == 0);
	top = Launcher_JobFor(o, LAUNCHER_JOB_LIST, place, LAUNCHER_DIR_GAMES);
	Launcher_JobFor(o, LAUNCHER_JOB_PROBE, iso, 0);
	CHECK(OpenBGI_JobNext(o) == top);
	o->launcher.cfgChanged = 0;
	Launcher_RunEnded(o);
	CHECK(!o->launcher.running && o->launcher.jobs == NULL);
	OpenBGI_JobRun(top, w);
	OpenBGI_JobDone(o, top); // (after the run: freed)
	CHECK(OpenBGI_JobNext(o) == NULL);

	o->engine.gamesPath[0] = 0;
	OpenBGI_Delete(w);
	DataFile(o, ".", "settings.json", NULL, 0);
	for(i = 0; i < 5; i++)
	{
		snprintf(path, sizeof path, "%s%s%s%c%s", place, i < 2 ? "" : (sep == '\\' ? "\\" : "/"), i < 2 ? "" : kFiles[i][0], sep, kFiles[i][1]);
		Host_NativeRemove(o, path);
	}
	for(i = 0; i < 3; i++)
		OS_DirRemove(sub[i]);
	OS_DirRemove(place);
}

int main(void)
{
	const char* tmp = getenv("BGI_TEST_TMP");
	OpenBGI_t* o;
	char sep, base[0x300];
	gHost = *OS_Host();
	gFiles = *gHost.files;
	gFiles.userDataDir = DataDir;
	gHost.files = &gFiles;
	o = OpenBGI_New(&gHost); // the engine whose code the test runs
	if(!o)
		return 1;
	if(!OS_Init())
		return 1;
	sep = Host_NativeSep(o);
	// (a full path: the boot makes the data folder the current one)
	Host_NativeAbsolute(o, tmp ? tmp : ".", base, sizeof base);
	snprintf(gData, sizeof gData, "%s%copenbgi_test_launcher%c", base, sep, sep);
	snprintf(gFolder, sizeof gFolder, "%s" LAUNCHER_SOURCES "%c", gData, sep);
	Sweep(o);
	printf("fresh\n");
	TestFresh(o);
	printf("edits\n");
	TestEdits(o);
	printf("newer\n");
	TestNewer(o);
	printf("broken\n");
	TestBroken(o);
	printf("drops\n");
	TestDrops(o);
	printf("pictures\n");
	TestPictures(o);
	printf("reading\n");
	TestReading(o);
	Host_SetGameDir(o, base); // (a folder that is the current one cannot be removed on Windows)
	Sweep(o);
	OS_Shutdown();
	if(gFails)
	{
		printf("%d check(s) failed\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
