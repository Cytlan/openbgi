/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/hostfiles.c - unit test of the host's files and registry as the
 *                     engine reaches them (src/include/bgi/host.h, src/core/host.c;
 *                     the tables of openbgi.h) and of the OS layer's
 *                     tables (src/host/os_files.c); built as test_hostfiles
 *                     by `make test`
 *
 * Most of it is an engine whose host keeps its files in memory: whatever
 * the engine reads or writes has to pass through the host's table.  The
 * groups, in the order they run:
 *
 *   none     - a host without files, registry or text encoding: every
 *              call fails as a missing file would, the strings come back
 *              empty or as they went in, nothing crashes
 *   game     - the game's files: the base directory is the host's exeDir,
 *              the engine's File_* and Fs_* calls arrive with the game's
 *              paths, the host's user and the open modes, an archive is
 *              read through them, a listing goes through the host's
 *              findFirst, the current directory is the host's
 *   drives   - the drive scan and DriveReady ask the host about the drives
 *              and the disc
 *   native   - native files: a whole file read (with a NUL after it) and
 *              written through nativeOpen, a short write fails, a zip is
 *              recognised and its game folder made in the host's user
 *              data, openbgi.json saved and loaded again
 *   settings - where openbgi.json is looked for and made: with none
 *              anywhere it is made in the user's data folder; one beside
 *              the binary goes before the user's, the game directory's
 *              before both; without a user's data folder it is made
 *              beside the binary; a file that cannot be written is
 *              passed over in each of the three places, unless it says
 *              "staticConfig": true - then it is used and nothing is
 *              ever written; a file that is named is used whatever it is
 *   sjis     - native paths in Shift-JIS (as the Windows hosts have them):
 *              a trail byte 0x5c is no separator when a zip's game folder
 *              is named and made
 *   registry - the installer's keys, the shell folders and "81 00" reach
 *              the host
 *   box text - a box's text reaches the host in the encoding the host
 *              was told ("81 00"), Shift-JIS text of a UTF-8 run converted;
 *              a new start tells the host Shift-JIS again
 *   os       - the OS layer's tables, which its host (OS_Host) has: native
 *              streams keep their size and position, a game file cannot be
 *              sought beyond 4 GB, the mode a native file is not opened
 *              in, "81 00" reaches the back end
 */
#include "bgi/core.h"
#include "bgi/host.h"
#include "bgi/fs.h"
#include "bgi/file.h"
#include "bgi/install.h"
#include "bgi/version.h"
#include "bgi/strutil.h"
#include "bgi/sjis.h"
#include "bgi/request.h"
#include "bgi/engine.h"
#include "host/os.h"
#include "host/os_common.h"

#include "testhost.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

// ---- the host's clock ------------------------------------------------------------------

static uint32_t HostTicksMs(void* user)
{
	BGI_UNUSED(user);
	return 0;
}

static int HostPerfNs(void* user, int64_t* out)
{
	BGI_UNUSED(user);
	*out = 0;
	return 1;
}

static void HostLocalTime(void* user, OpenBGI_DateTime_t* out)
{
	BGI_UNUSED(user);
	memset(out, 0, sizeof *out);
	out->year = 2026;
	out->month = 1;
	out->day = 1;
}

// ---- files in memory ----------------------------------------------------------------------

#define MEM_FILES 32

typedef struct MemFile
{
	int used;
	int native; // a native file (by a native path) rather than one of the game's
	char path[0x200];
	uint32_t attrs; // OPENBGI_ATTR_*
	uint8_t* data;
	uint32_t size;
} MemFile_t;

typedef struct MemStream
{
	MemFile_t* file;
	uint64_t pos;
} MemStream_t;

typedef struct MemFind
{
	char dir[0x200]; // the pattern's directory part, its separator included
	char pat[0x100]; // its name part
	int next;        // the file to look at next
} MemFind_t;

static struct
{
	MemFile_t files[MEM_FILES];
	int badUser; // calls that came with a user other than the host's
	int calls;   // calls of the table
	int opens;   // open calls
	char lastPath[0x200];
	int lastMode;
	char cwd[0x200]; // setCurrentDir's last directory
	int finds;       // findFirst calls
	int writeLimit;  // bytes a write may still put, -1 any
	int discIn;      // the CD drive Q: holds a disc
	int csidl;       // specialFolder's last folder
	int encoding;    // textEncoding's last value, -1 none yet
	char setGameDir[0x200];
	struct
	{
		char name[0x200]; // "key\value"
		char data[0x200];
	} reg[8];
	int regKeys;    // createKey calls
	int noData;     // the host names no user's data folder
	char fixed[32]; // nothing can be written under this folder; "": everywhere
} gMem;

static void Use(void* user)
{
	gMem.calls++;
	if(user != &gMem)
		gMem.badUser++;
}

static int Lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

// the game's paths ignore the ASCII case, as Windows does; native ones do not
static int SamePath(const char* a, const char* b, int native)
{
	if(native)
		return strcmp(a, b) == 0;
	while(*a && *b && Lower((uint8_t)*a) == Lower((uint8_t)*b))
		a++, b++;
	return *a == *b;
}

static MemFile_t* Find(const char* path, int native)
{
	int i;
	for(i = 0; i < MEM_FILES; i++)
		if(gMem.files[i].used && gMem.files[i].native == native && SamePath(gMem.files[i].path, path, native))
			return &gMem.files[i];
	return NULL;
}

static MemFile_t* Put(const char* path, int native, const void* data, uint32_t size, uint32_t attrs)
{
	MemFile_t* f = Find(path, native);
	int i;
	for(i = 0; !f && i < MEM_FILES; i++)
		if(!gMem.files[i].used)
			f = &gMem.files[i];
	if(!f)
		return NULL;
	free(f->data);
	f->used = 1;
	f->native = native;
	snprintf(f->path, sizeof f->path, "%s", path);
	f->attrs = attrs;
	f->data = size ? (uint8_t*)malloc(size) : NULL;
	if(size)
		memcpy(f->data, data, size);
	f->size = size;
	return f;
}

static void Drop(MemFile_t* f)
{
	free(f->data);
	memset(f, 0, sizeof *f);
}

static void MemReset(void)
{
	int i;
	for(i = 0; i < MEM_FILES; i++)
		if(gMem.files[i].used)
			Drop(&gMem.files[i]);
	memset(&gMem, 0, sizeof gMem);
	gMem.writeLimit = -1;
	gMem.encoding = -1;
}

static void* OpenOf(MemFile_t* f)
{
	MemStream_t* s;
	if(!f || (f->attrs & OPENBGI_ATTR_DIRECTORY))
		return NULL;
	s = (MemStream_t*)calloc(1, sizeof *s);
	s->file = f;
	return s;
}

static void* MemOpen(void* user, const char* path, int mode)
{
	MemFile_t* f;
	Use(user);
	gMem.opens++;
	snprintf(gMem.lastPath, sizeof gMem.lastPath, "%s", path);
	gMem.lastMode = mode;
	f = Find(path, 0);
	if(mode == OPENBGI_OPEN_CREATE || (mode == OPENBGI_OPEN_WRITE && !f))
		f = Put(path, 0, NULL, 0, OPENBGI_ATTR_ARCHIVE);
	return OpenOf(f);
}

static void MemClose(void* user, void* f)
{
	Use(user);
	free(f);
}

static uint32_t MemRead(void* user, void* f, void* buf, uint32_t n)
{
	MemStream_t* s = (MemStream_t*)f;
	uint32_t left;
	Use(user);
	if(s->pos >= s->file->size)
		return 0;
	left = s->file->size - (uint32_t)s->pos;
	if(n > left)
		n = left;
	memcpy(buf, s->file->data + s->pos, n);
	s->pos += n;
	return n;
}

static uint32_t MemWrite(void* user, void* f, const void* buf, uint32_t n)
{
	MemStream_t* s = (MemStream_t*)f;
	MemFile_t* m = s->file;
	Use(user);
	if(gMem.writeLimit >= 0 && n > (uint32_t)gMem.writeLimit)
		n = (uint32_t)gMem.writeLimit;
	if(gMem.writeLimit >= 0)
		gMem.writeLimit -= (int)n;
	if(s->pos + n > m->size)
	{
		m->data = (uint8_t*)realloc(m->data, (size_t)(s->pos + n));
		memset(m->data + m->size, 0, (size_t)(s->pos - (s->pos < m->size ? s->pos : m->size)));
		m->size = (uint32_t)(s->pos + n);
	}
	memcpy(m->data + s->pos, buf, n);
	s->pos += n;
	return n;
}

static int MemSeek(void* user, void* f, uint64_t pos)
{
	Use(user);
	((MemStream_t*)f)->pos = pos;
	return 1;
}

static uint64_t MemSize(void* user, void* f)
{
	Use(user);
	return ((MemStream_t*)f)->file->size;
}

static int MemGetTime(void* user, void* f, uint32_t* lo, uint32_t* hi)
{
	Use(user);
	BGI_UNUSED(f);
	*lo = 0x12345678u;
	*hi = 0x01d00000u;
	return 1;
}

static uint32_t MemAttrs(void* user, const char* path)
{
	MemFile_t* f;
	Use(user);
	f = Find(path, 0);
	return f ? f->attrs : OPENBGI_INVALID_ATTRS;
}

static int MemRemove(void* user, const char* path)
{
	MemFile_t* f;
	Use(user);
	f = Find(path, 0);
	if(!f || (f->attrs & OPENBGI_ATTR_DIRECTORY))
		return 0;
	Drop(f);
	return 1;
}

static int MemMakeDir(void* user, const char* path)
{
	Use(user);
	if(Find(path, 0))
		return 0;
	return Put(path, 0, NULL, 0, OPENBGI_ATTR_DIRECTORY) != NULL;
}

// the directory part of a game's path (up to and with the last '\\') and the name after it
static void Split(const char* path, char* dir, size_t dn, const char** name)
{
	const char* p;
	const char* last = path;
	for(p = path; *p; p += Sjis_IsLead((uint8_t)*p) && p[1] ? 2 : 1)
		if(*p == '\\')
			last = p + 1;
	snprintf(dir, dn, "%.*s", (int)(last - path), path);
	*name = last;
}

static int MemFindNext(void* user, void* find, OpenBGI_FindData_t* out)
{
	MemFind_t* h = (MemFind_t*)find;
	Use(user);
	while(h->next < MEM_FILES)
	{
		MemFile_t* f = &gMem.files[h->next++];
		char dir[0x200];
		const char* name;
		if(!f->used || f->native)
			continue;
		Split(f->path, dir, sizeof dir, &name);
		if(SamePath(dir, h->dir, 0) && Sjis_WildcardMatch(h->pat, name))
		{
			out->attrs = f->attrs;
			snprintf(out->name, sizeof out->name, "%s", name);
			return 1;
		}
	}
	return 0;
}

static void* MemFindFirst(void* user, const char* pattern, OpenBGI_FindData_t* out)
{
	MemFind_t* h = (MemFind_t*)calloc(1, sizeof *h);
	const char* name;
	Use(user);
	gMem.finds++;
	Split(pattern, h->dir, sizeof h->dir, &name);
	snprintf(h->pat, sizeof h->pat, "%s", name);
	if(!MemFindNext(user, h, out))
	{
		free(h);
		return NULL;
	}
	return h;
}

static void MemFindClose(void* user, void* find)
{
	Use(user);
	free(find);
}

static int MemSetCurrentDir(void* user, const char* dir)
{
	Use(user);
	snprintf(gMem.cwd, sizeof gMem.cwd, "%s", dir);
	return 1;
}

static void MemExeDir(void* user, char* buf, size_t n)
{
	Use(user);
	snprintf(buf, n, "C:\\game\\");
}

static uint32_t MemDriveType(void* user, const char* root)
{
	Use(user);
	switch(Lower((uint8_t)root[0]))
	{
		case 'c': return OPENBGI_DRIVE_FIXED;
		case 'q': return OPENBGI_DRIVE_CDROM;
		default: return OPENBGI_DRIVE_NO_ROOT;
	}
}

static uint32_t MemLogicalDrives(void* user, char* buf, uint32_t n)
{
	static const char kList[] = "C:\\\0Q:\\\0"; // and the NUL of the literal
	Use(user);
	if(n < sizeof kList)
		return sizeof kList;
	memcpy(buf, kList, sizeof kList);
	return sizeof kList - 1;
}

static int MemVolumeReady(void* user, const char* root)
{
	Use(user);
	return Lower((uint8_t)root[0]) == 'q' && gMem.discIn;
}

static int MemMediaPresent(void* user, char drive)
{
	Use(user);
	return Lower((uint8_t)drive) == 'q' && gMem.discIn;
}

static int MemSpecialFolder(void* user, int csidl, char* buf, size_t n)
{
	Use(user);
	gMem.csidl = csidl;
	snprintf(buf, n, "C:\\folder%d", csidl);
	return 1;
}

static int MemWindowsDir(void* user, char* buf, size_t n)
{
	Use(user);
	snprintf(buf, n, "C:\\WINDOWS");
	return 1;
}

// 1 when nothing can be written at the native path
static int Fixed(const char* path)
{
	return gMem.fixed[0] && strncmp(path, gMem.fixed, strlen(gMem.fixed)) == 0;
}

static int MemNativeWritable(void* user, const char* path)
{
	Use(user);
	return Find(path, 1) != NULL && !Fixed(path);
}

static void* MemNativeOpen(void* user, const char* path, int mode)
{
	Use(user);
	if(mode == OPENBGI_OPEN_CREATE)
		return Fixed(path) ? NULL : OpenOf(Put(path, 1, NULL, 0, OPENBGI_ATTR_ARCHIVE));
	return mode == OPENBGI_OPEN_READ ? OpenOf(Find(path, 1)) : NULL;
}

static int MemNativeStat(void* user, const char* path, OpenBGI_NativeStat_t* out)
{
	MemFile_t* f;
	Use(user);
	f = Find(path, 1);
	if(!f)
		return OPENBGI_NATIVE_NONE;
	if(out)
	{
		out->size = f->size;
		out->mtime = 0;
	}
	return (f->attrs & OPENBGI_ATTR_DIRECTORY) ? OPENBGI_NATIVE_DIR : OPENBGI_NATIVE_FILE;
}

static int MemNativeMkdir(void* user, const char* path)
{
	MemFile_t* f;
	Use(user);
	f = Find(path, 1);
	if(f)
		return (f->attrs & OPENBGI_ATTR_DIRECTORY) != 0;
	return Put(path, 1, NULL, 0, OPENBGI_ATTR_DIRECTORY) != NULL;
}

static void MemGameDir(void* user, char* buf, size_t n)
{
	Use(user);
	snprintf(buf, n, "/mem/game/");
}

static int MemSetGameDir(void* user, const char* dir)
{
	Use(user);
	snprintf(gMem.setGameDir, sizeof gMem.setGameDir, "%s", dir);
	return 1;
}

static void MemUserDataDir(void* user, char* buf, size_t n)
{
	Use(user);
	snprintf(buf, n, "%s", gMem.noData ? "" : "/mem/data/");
}

static void MemBinaryDir(void* user, char* buf, size_t n)
{
	Use(user);
	snprintf(buf, n, "/mem/bin/");
}

static const OpenBGI_Files_t kMemFiles = {
	sizeof(OpenBGI_Files_t),
	MemOpen,
	MemClose,
	MemRead,
	MemWrite,
	MemSeek,
	MemSize,
	MemGetTime,
	NULL, // setTime
	MemAttrs,
	NULL, // setAttrs
	MemRemove,
	NULL, // move
	NULL, // copy
	MemMakeDir,
	NULL, // removeDir
	MemFindFirst,
	MemFindNext,
	MemFindClose,
	MemSetCurrentDir,
	MemExeDir,
	MemDriveType,
	MemLogicalDrives,
	MemVolumeReady,
	MemMediaPresent,
	MemSpecialFolder,
	MemWindowsDir,
	'/', // nativeSep
	0,   // nativeSjis
	MemNativeOpen,
	MemNativeStat,
	MemNativeMkdir,
	NULL, // nativeRemove
	NULL, // nativeList
	NULL, // nativeAbsolute
	NULL, // toUtf8
	NULL, // fromUtf8
	MemGameDir,
	MemSetGameDir,
	MemUserDataDir,
	MemBinaryDir,
	MemNativeWritable,
};

// ---- the registry in memory ---------------------------------------------------------------

static int RegSlot(const char* key, const char* value, int make)
{
	char name[0x200];
	int i;
	snprintf(name, sizeof name, "%s\\%s", key, value ? value : "");
	for(i = 0; i < 8; i++)
		if(gMem.reg[i].name[0] && strcmp(gMem.reg[i].name, name) == 0)
			return i;
	for(i = 0; make && i < 8; i++)
		if(!gMem.reg[i].name[0])
		{
			snprintf(gMem.reg[i].name, sizeof gMem.reg[i].name, "%s", name);
			return i;
		}
	return -1;
}

static int MemRegRead(void* user, const char* key, const char* value, char* buf, size_t n)
{
	int i;
	Use(user);
	i = RegSlot(key, value, 0);
	if(i < 0)
		return 0;
	snprintf(buf, n, "%s", gMem.reg[i].data);
	return 1;
}

static int MemRegCreateKey(void* user, const char* key, const char* cls)
{
	Use(user);
	BGI_UNUSED(key);
	BGI_UNUSED(cls);
	gMem.regKeys++;
	return 1;
}

static int MemRegWrite(void* user, const char* key, const char* value, const char* s)
{
	int i;
	Use(user);
	i = RegSlot(key, value, 1);
	if(i < 0)
		return 0;
	snprintf(gMem.reg[i].data, sizeof gMem.reg[i].data, "%s", s);
	return 1;
}

static int MemRegDeleteKey(void* user, const char* key)
{
	size_t n = strlen(key);
	int i, any = 0;
	Use(user);
	for(i = 0; i < 8; i++)
		if(strncmp(gMem.reg[i].name, key, n) == 0 && gMem.reg[i].name[n] == '\\')
		{
			gMem.reg[i].name[0] = 0;
			any = 1;
		}
	return any;
}

static const OpenBGI_Registry_t kMemRegistry = {sizeof(OpenBGI_Registry_t), MemRegRead, MemRegCreateKey, MemRegWrite, MemRegDeleteKey};

static void MemTextEncoding(void* user, int utf8)
{
	Use(user);
	gMem.encoding = utf8;
}

static OpenBGI_Host_t gMemHost = {.size = sizeof(OpenBGI_Host_t), .api = OPENBGI_API_VERSION, .user = &gMem, .ticksMs = HostTicksMs, .perfNs = HostPerfNs, .localTime = HostLocalTime, .caps = OPENBGI_CAP_DIALOGS, .files = &kMemFiles, .registry = &kMemRegistry, .textEncoding = MemTextEncoding};
static OpenBGI_Host_t gBareHost = {.size = sizeof(OpenBGI_Host_t), .api = OPENBGI_API_VERSION, .ticksMs = HostTicksMs, .perfNs = HostPerfNs, .localTime = HostLocalTime, .caps = OPENBGI_CAP_DIALOGS};

// ---- the tests ----------------------------------------------------------------------------

static void TestNone(void)
{
	OpenBGI_t* o = OpenBGI_New(&gBareHost);
	OpenBGI_FindData_t fd;
	OpenBGI_NativeStat_t st;
	File_t f;
	char buf[64];
	uint32_t size = 7, lo = 1, hi = 1;
	printf("none\n");
	CHECK(o != NULL);
	if(!o)
		return;
	CHECK(Host_FileOpen(o, "C:\\a.txt", OPENBGI_OPEN_READ) == NULL);
	CHECK(Host_FileOpen(o, "C:\\a.txt", OPENBGI_OPEN_CREATE) == NULL);
	Host_FileClose(o, NULL);
	CHECK(Host_FileAttrs(o, "C:\\a.txt") == OPENBGI_INVALID_ATTRS);
	CHECK(!Host_FileSetAttrs(o, "C:\\a.txt", 0) && !Host_FileDelete(o, "C:\\a.txt") && !Host_FileMove(o, "a", "b"));
	CHECK(!Host_FileCopy(o, "a", "b", 0) && !Host_DirCreate(o, "C:\\d") && !Host_DirRemove(o, "C:\\d"));
	CHECK(Host_FindFirst(o, "C:\\*", &fd) == NULL);
	Host_FindClose(o, NULL);
	CHECK(!Host_SetCurrentDir(o, "C:\\"));
	strcpy(buf, "x");
	Host_ExeDir(o, buf, sizeof buf);
	CHECK(buf[0] == 0);
	CHECK(Host_DriveType(o, "C:\\") == OPENBGI_DRIVE_UNKNOWN);
	strcpy(buf, "x");
	CHECK(Host_LogicalDrives(o, buf, sizeof buf) == 0);
	CHECK(!Host_VolumeReady(o, "C:\\") && !Host_MediaPresent(o, 'C'));
	strcpy(buf, "x");
	CHECK(!Host_SpecialFolder(o, 5, buf, sizeof buf) && buf[0] == 0);
	strcpy(buf, "x");
	CHECK(!Host_WindowsDir(o, buf, sizeof buf) && buf[0] == 0);
	CHECK(Host_NativeSep(o) == '/' && !Host_NativeSjis(o));
	CHECK(Host_NativeOpen(o, "/x", OPENBGI_OPEN_READ) == NULL);
	CHECK(Host_NativeStat(o, "/x", &st) == OPENBGI_NATIVE_NONE);
	CHECK(!Host_NativeMkdir(o, "/x") && !Host_NativeRemove(o, "/x") && Host_NativeList(o, "/", NULL, NULL) == -1);
	Host_NativeAbsolute(o, "rel/a", buf, sizeof buf);
	CHECK(strcmp(buf, "rel/a") == 0);
	Host_NativeToUtf8(o, "\xe6\x97\xa5", buf, sizeof buf);
	CHECK(strcmp(buf, "\xe6\x97\xa5") == 0);
	Host_Utf8ToNative(o, "abc", buf, sizeof buf);
	CHECK(strcmp(buf, "abc") == 0);
	CHECK(Host_ReadNativeFile(o, "/x", &size) == NULL && size == 0);
	CHECK(!Host_WriteNativeFile(o, "/x", "a", 1));
	strcpy(buf, "x");
	Host_GameDir(o, buf, sizeof buf);
	CHECK(buf[0] == 0);
	CHECK(!Host_SetGameDir(o, "/x"));
	strcpy(buf, "x");
	Host_UserDataDir(o, buf, sizeof buf);
	CHECK(buf[0] == 0);
	strcpy(buf, "x");
	Host_BinaryDir(o, buf, sizeof buf);
	CHECK(buf[0] == 0);
	strcpy(buf, "x");
	CHECK(!Host_RegRead(o, "HKCU\\k", "v", buf, sizeof buf) && strcmp(buf, "x") == 0); // as a missing value leaves it
	CHECK(!Host_RegCreateKey(o, "HKCU\\k", NULL) && !Host_RegWrite(o, "HKCU\\k", "v", "s") && !Host_RegDeleteKey(o, "HKCU\\k"));
	Host_TextEncoding(o, 1);
	Text_SetEncoding(o, TEXT_UTF8);
	Text_SetEncoding(o, TEXT_SJIS);
	CHECK(!Host_FileGetTime(o, NULL, &lo, &hi));
	// the engine's own layers fail the same way
	SetBaseDir(o, NULL);
	CHECK(o->files.baseDir[0] == 0);
	File_Ctor(&f);
	CHECK(!File_OpenRead(o, &f, "C:\\ipl._bp") && !File_Create(o, &f, "C:\\save.dat"));
	File_Dtor(&f);
	CHECK(Fs_LoadFile(o, "C:\\ipl._bp", &size) == NULL);
	CHECK(Fs_FileAttrs(o, "C:\\ipl._bp") == OPENBGI_INVALID_ATTRS);
	CHECK(!Fs_IsArchive(o, "/game.zip"));
	CHECK(Engine_LoadGamesJson(o, "/openbgi.json") == 0);
	OpenBGI_Delete(o);
}

// a PackFile archive of one entry
static uint32_t MakeArchive(uint8_t* out, const char* name, const char* data)
{
	uint32_t n = (uint32_t)strlen(data), count = 1, off = 0;
	memset(out, 0, 16 + 32);
	memcpy(out, PACKFILE_MAGIC, 12);
	memcpy(out + 12, &count, 4);
	snprintf((char*)out + 16, 16, "%s", name);
	memcpy(out + 16 + 16, &off, 4);
	memcpy(out + 16 + 20, &n, 4);
	memcpy(out + 16 + 32, data, n);
	return 16 + 32 + n;
}

static void TestGame(OpenBGI_t* o)
{
	static const char kIpl[] = "the boot program";
	uint8_t arc[0x100], buf[0x100];
	OpenBGI_FindData_t fd;
	FsFind_t* h;
	MemFile_t* m;
	File_t f;
	uint32_t n, lo = 0, hi = 0;
	int found;
	printf("game\n");
	Put("C:\\game\\ipl._bp", 0, kIpl, sizeof kIpl - 1, OPENBGI_ATTR_ARCHIVE);
	Put("C:\\game\\data.arc", 0, arc, MakeArchive(arc, "hello", "archived text"), OPENBGI_ATTR_ARCHIVE);
	Put("C:\\game\\sub", 0, NULL, 0, OPENBGI_ATTR_DIRECTORY);
	Put("C:\\game\\second.arc", 0, "-", 1, OPENBGI_ATTR_ARCHIVE);
	// the base directory and the current directory are the host's
	SetBaseDir(o, NULL);
	CHECK(strcmp(o->files.baseDir, "C:\\game\\") == 0);
	CHECK(strcmp(gMem.cwd, "C:\\game\\") == 0);
	// a read, with the path and the mode as the engine gave them
	File_Ctor(&f);
	CHECK(File_OpenRead(o, &f, "C:\\game\\IPL._BP"));
	CHECK(strcmp(gMem.lastPath, "C:\\game\\IPL._BP") == 0 && gMem.lastMode == OPENBGI_OPEN_READ);
	CHECK(File_Size(&f) == sizeof kIpl - 1);
	n = File_Read(&f, buf, sizeof buf);
	CHECK(n == sizeof kIpl - 1 && memcmp(buf, kIpl, n) == 0);
	CHECK(File_Seek(&f, 4) && File_Read(&f, buf, 4) == 4 && memcmp(buf, "boot", 4) == 0);
	CHECK(Fs_FileGetTime(f.h, &lo, &hi) && lo == 0x12345678u && hi == 0x01d00000u);
	File_Close(&f);
	CHECK(!File_OpenRead(o, &f, "C:\\game\\missing.txt"));
	CHECK(!File_OpenRead(o, &f, "C:\\game\\sub")); // a directory is no file
	// a write lands in the host's file
	CHECK(File_Create(o, &f, "C:\\game\\save.dat"));
	CHECK(gMem.lastMode == OPENBGI_OPEN_CREATE);
	CHECK(File_Write(&f, "saved", 5) == 5);
	File_Close(&f);
	m = Find("C:\\game\\save.dat", 0);
	CHECK(m && m->size == 5 && memcmp(m->data, "saved", 5) == 0);
	CHECK(Fs_FileAttrs(o, "C:\\game\\save.dat") == OPENBGI_ATTR_ARCHIVE);
	CHECK(Fs_FileAttrs(o, "C:\\game\\sub") == OPENBGI_ATTR_DIRECTORY);
	CHECK(Fs_FileAttrs(o, "C:\\game\\none") == OPENBGI_INVALID_ATTRS);
	CHECK(Fs_FileDelete(o, "C:\\game\\save.dat") && !Find("C:\\game\\save.dat", 0));
	// a whole file, an archive entry read through the archive manager
	n = 0;
	{
		uint8_t* d = Fs_LoadFile(o, "C:\\game\\ipl._bp", &n);
		CHECK(d && n == sizeof kIpl - 1 && memcmp(d, kIpl, n) == 0);
		free(d);
	}
	n = ArcMgr_Read(o, o->files.arcMgr, buf, "C:\\game\\data.arc", "hello");
	CHECK(n == 13 && memcmp(buf, "archived text", 13) == 0);
	CHECK(ArcMgr_Exists(o, o->files.arcMgr, "C:\\game\\data.arc", "hello"));
	CHECK(!ArcMgr_Exists(o, o->files.arcMgr, "C:\\game\\data.arc", "other"));
	// a listing is the host's
	found = 0;
	h = Fs_FindFirst(o, "C:\\game\\*.arc", &fd);
	CHECK(h != NULL && gMem.finds == 1);
	while(h)
	{
		found += strcmp(fd.name, "data.arc") == 0 || strcmp(fd.name, "second.arc") == 0;
		if(!Fs_FindNext(h, &fd))
			break;
	}
	Fs_FindClose(h);
	CHECK(found == 2);
	CHECK(Fs_FindFirst(o, "C:\\game\\*.none", &fd) == NULL);
	// a directory made through the host
	CHECK(Fs_DirCreate(o, "C:\\game\\new"));
	CHECK(Fs_FileAttrs(o, "C:\\game\\new") == OPENBGI_ATTR_DIRECTORY);
	CHECK(!Fs_DirCreate(o, "C:\\game\\new"));
}

static void TestDrives(OpenBGI_t* o)
{
	printf("drives\n");
	Drives_Scan(o);
	CHECK(Drive_TypeOf(o, 'c') == OPENBGI_DRIVE_FIXED && Drive_TypeOf(o, 'C') == OPENBGI_DRIVE_FIXED);
	CHECK(Drive_TypeOf(o, 'q') == OPENBGI_DRIVE_CDROM);
	CHECK(Drive_TypeOf(o, 'd') == OPENBGI_DRIVE_NO_ROOT);
	CHECK(DriveReady(o, "C:\\game\\ipl._bp")); // a fixed drive is ready
	gMem.discIn = 0;
	CHECK(!DriveReady(o, "Q:\\ipl._bp"));
	gMem.discIn = 1;
	CHECK(DriveReady(o, "Q:\\ipl._bp"));
	CHECK(!DriveReady(o, "ipl._bp"));
}

static void TestNative(OpenBGI_t* o)
{
	static const uint8_t kZip[] = {'P', 'K', 3, 4, 0, 0};
	uint8_t* d;
	uint32_t size = 0;
	int games;
	printf("native\n");
	Put("/mem/game/text.txt", 1, "native", 6, OPENBGI_ATTR_ARCHIVE);
	Put("/mem/game.zip", 1, kZip, sizeof kZip, OPENBGI_ATTR_ARCHIVE);
	Put("C:\\game\\notnative.txt", 0, "game", 4, OPENBGI_ATTR_ARCHIVE);
	d = Host_ReadNativeFile(o, "/mem/game/text.txt", &size);
	CHECK(d && size == 6 && memcmp(d, "native", 6) == 0 && d[6] == 0);
	free(d);
	CHECK(Host_ReadNativeFile(o, "C:\\game\\notnative.txt", &size) == NULL && size == 0);
	{ // the NUL comes after the data though the buffer held something else before (freed just now, it is likely reused)
		static const char kLong[] = "a native file of thirty bytes!";
		volatile char* junk; // (stores the compiler keeps)
		size_t i;
		Put("/mem/game/long.txt", 1, kLong, sizeof kLong - 1, OPENBGI_ATTR_ARCHIVE);
		junk = (volatile char*)malloc(sizeof kLong);
		for(i = 0; junk && i < sizeof kLong; i++)
			junk[i] = 'x';
		free((void*)junk);
		d = Host_ReadNativeFile(o, "/mem/game/long.txt", &size);
		CHECK(d && size == sizeof kLong - 1 && d[size] == 0);
		free(d);
	}
	CHECK(Host_WriteNativeFile(o, "/mem/bin/out.txt", "written", 7));
	CHECK(Find("/mem/bin/out.txt", 1) && Find("/mem/bin/out.txt", 1)->size == 7);
	gMem.writeLimit = 3;
	CHECK(!Host_WriteNativeFile(o, "/mem/bin/short.txt", "written", 7));
	gMem.writeLimit = -1;
	CHECK(Fs_IsArchive(o, "/mem/game.zip"));
	CHECK(!Fs_IsArchive(o, "/mem/game/text.txt"));
	CHECK(!Fs_IsArchive(o, "/mem/nothing.zip"));
	// a zip's game folder, made in the user's data
	{
		char folder[0x100];
		TestHost_SetEnv("BGI_DATA", "");
		CHECK(Fs_GameFolder(o, "/mem/My Game.ZIP", folder, sizeof folder));
		CHECK(strcmp(folder, "/mem/data/games/My Game/") == 0);
		CHECK(Host_NativeStat(o, "/mem/data/games/My Game", NULL) == OPENBGI_NATIVE_DIR);
		CHECK(Host_NativeStat(o, "/mem/data/games", NULL) == OPENBGI_NATIVE_DIR);
	}
	// openbgi.json: written to the host's native file and read back
	Engine_ResetGames(o);
	games = Engine_GameCount(o);
	CHECK(games > 0);
	CHECK(Engine_SaveGamesJson(o, "/mem/bin/openbgi.json"));
	CHECK(Find("/mem/bin/openbgi.json", 1) != NULL);
	CHECK(Engine_LoadGamesJson(o, "/mem/bin/openbgi.json") == games);
	CHECK(Engine_LoadGamesJson(o, "/mem/bin/missing.json") == 0);
	{
		char buf[0x40];
		Host_NativeAbsolute(o, "rel", buf, sizeof buf); // no nativeAbsolute: the path as it is
		CHECK(strcmp(buf, "rel") == 0);
		Host_BinaryDir(o, buf, sizeof buf);
		CHECK(strcmp(buf, "/mem/bin/") == 0);
		CHECK(Host_SetGameDir(o, "/mem/other/") && strcmp(gMem.setGameDir, "/mem/other/") == 0);
	}
}

// the native directories made so far whose path ends in the given byte
static int NativeDirsEndingIn(uint8_t c)
{
	int i, n = 0;
	for(i = 0; i < MEM_FILES; i++)
	{
		const MemFile_t* f = &gMem.files[i];
		size_t len = strlen(f->path);
		if(f->used && f->native && (f->attrs & OPENBGI_ATTR_DIRECTORY) && len && (uint8_t)f->path[len - 1] == c)
			n++;
	}
	return n;
}

// the settings file as the engine writes it, with one more game in it and, with `fixed`, "staticConfig": true: at `path`
static void PutSettings(OpenBGI_t* o, const char* path, const char* product, int fixed)
{
	size_t len = 0;
	char* text;
	char* brace;
	char profile[64];
	EngineGame_t g;
	Engine_ResetGames(o);
	memset(&g, 0, sizeof g);
	CHECK(Engine_GameAt(o, 0, &g) && g.profile);
	snprintf(profile, sizeof profile, "%s", g.profile ? g.profile : "");
	CHECK(Engine_RecordGame(o, -1, product, NULL, product, NULL, profile, NULL) >= 0);
	text = Engine_FormatGamesJson(o, &len);
	CHECK(text != NULL);
	brace = text ? strstr(text, "\n{") : NULL; // (after the header's comment lines)
	CHECK(brace != NULL);
	if(brace && fixed)
	{
		static const char kFlag[] = "\n\t\"staticConfig\": true,";
		size_t at = (size_t)(brace - text) + 2;
		char* more = (char*)malloc(len + sizeof kFlag);
		memcpy(more, text, at);
		memcpy(more + at, kFlag, sizeof kFlag - 1);
		memcpy(more + at + sizeof kFlag - 1, text + at, len - at);
		free(text);
		text = more;
		len += sizeof kFlag - 1;
	}
	if(text)
		Put(path, 1, text, (uint32_t)len, OPENBGI_ATTR_ARCHIVE);
	free(text);
	Engine_ResetGames(o);
}

// 1 when the games table has a game of that product name
static int HasGame(OpenBGI_t* o, const char* product)
{
	EngineGame_t g;
	int i;
	for(i = 0; Engine_GameAt(o, i, &g); i++)
		if(g.product && strcmp(g.product, product) == 0)
			return 1;
	return 0;
}

static void DropAt(const char* path)
{
	MemFile_t* f = Find(path, 1);
	if(f)
		Drop(f);
}

static void TestSettings(OpenBGI_t* o)
{
	static const char* const kPlaces[3] = {"/mem/game/openbgi.json", "/mem/bin/openbgi.json", "/mem/data/openbgi.json"};
	static const char* const kFolders[3] = {"/mem/game/", "/mem/bin/", "/mem/data/"};
	static const char* const kNames[3] = {"GamesOwn", "BesideBinary", "UsersOwn"};
	MemFile_t* f;
	uint32_t size;
	int i, games;
	printf("settings\n");
	if(getenv("BGI_GAMES"))
		return; // (the file is named: nothing is looked for)
	for(i = 0; i < 3; i++)
		DropAt(kPlaces[i]);
	o->engine.optGames[0] = 0;
	Engine_ResetGames(o);
	games = Engine_GameCount(o);

	// none anywhere: the defaults, written to the user's data folder
	Engine_LoadGames(o);
	CHECK(strcmp(o->engine.gamesPath, kPlaces[2]) == 0 && Find(kPlaces[2], 1) != NULL && !Find(kPlaces[1], 1) && !Find(kPlaces[0], 1));
	CHECK(Engine_GameCount(o) == games && !o->engine.gamesStatic);
	// .. which is found the next time
	PutSettings(o, kPlaces[2], kNames[2], 0);
	Engine_LoadGames(o);
	CHECK(strcmp(o->engine.gamesPath, kPlaces[2]) == 0 && HasGame(o, kNames[2]));
	// one beside the binary goes before it
	PutSettings(o, kPlaces[1], kNames[1], 0);
	Engine_LoadGames(o);
	CHECK(strcmp(o->engine.gamesPath, kPlaces[1]) == 0 && HasGame(o, kNames[1]) && !HasGame(o, kNames[2]));
	// .. and the game directory's before both
	PutSettings(o, kPlaces[0], kNames[0], 0);
	Engine_LoadGames(o);
	CHECK(strcmp(o->engine.gamesPath, kPlaces[0]) == 0 && HasGame(o, kNames[0]) && !HasGame(o, kNames[1]));

	// a file that cannot be written is passed over, in each of the three places: the next one is the one
	for(i = 0; i < 3; i++)
	{
		snprintf(gMem.fixed, sizeof gMem.fixed, "%s", kFolders[i]);
		Engine_LoadGames(o);
		CHECK(!HasGame(o, kNames[i]) && !o->engine.gamesStatic);
		if(i < 2)
			CHECK(strcmp(o->engine.gamesPath, kPlaces[i ? 2 : 1]) == 0 && HasGame(o, kNames[i ? 2 : 1]));
		else // (the user's own: the defaults, which cannot be written there and go beside the binary)
			CHECK(strcmp(o->engine.gamesPath, kPlaces[1]) == 0 && Engine_GameCount(o) == games);
		f = Find(kPlaces[i], 1);
		CHECK(f != NULL && !Find("/mem/game/openbgi.json.bad", 1) && !Find("/mem/bin/openbgi.json.bad", 1) && !Find("/mem/data/openbgi.json.bad", 1));
		DropAt(kPlaces[i]); // (the places before it are empty from here on)
		if(i == 1)
			PutSettings(o, kPlaces[2], kNames[2], 0); // (.. and what the last turn wrote over the user's file is put back)
	}
	gMem.fixed[0] = 0;
	DropAt(kPlaces[1]);

	// .. unless it says "staticConfig": true - it is used then, and nothing is written, there or anywhere else
	for(i = 0; i < 3; i++)
	{
		PutSettings(o, kPlaces[i], kNames[i], 1);
		f = Find(kPlaces[i], 1);
		size = f ? f->size : 0;
		snprintf(gMem.fixed, sizeof gMem.fixed, "%s", kFolders[i]);
		Engine_LoadGames(o);
		CHECK(strcmp(o->engine.gamesPath, kPlaces[i]) == 0 && HasGame(o, kNames[i]) && o->engine.gamesStatic);
		CHECK(Engine_RecordGame(o, -1, "AddedInTheRun", NULL, "AddedInTheRun", NULL, "1.64", NULL) >= 0);
		CHECK(!Engine_SaveGames(o) && strcmp(o->engine.gamesPath, kPlaces[i]) == 0 && HasGame(o, "AddedInTheRun"));
		// (nor when the place could be written to after all)
		gMem.fixed[0] = 0;
		CHECK(!Engine_SaveGames(o));
		f = Find(kPlaces[i], 1);
		CHECK(f != NULL && f->size == size);
		CHECK(!Find(kPlaces[(i + 1) % 3], 1) && !Find(kPlaces[(i + 2) % 3], 1));
		DropAt(kPlaces[i]);
	}

	// a file that is named is used as it is: one that cannot be written, with the flag and without
	PutSettings(o, "/mem/named/own.json", "Named", 0);
	snprintf(gMem.fixed, sizeof gMem.fixed, "/mem/named/");
	snprintf(o->engine.optGames, sizeof o->engine.optGames, "/mem/named/own.json");
	Engine_LoadGames(o);
	CHECK(strcmp(o->engine.gamesPath, "/mem/named/own.json") == 0 && HasGame(o, "Named") && !o->engine.gamesStatic);
	CHECK(!Engine_SaveGames(o) && strcmp(o->engine.gamesPath, "/mem/named/own.json") == 0); // (nothing takes its place)
	gMem.fixed[0] = 0;
	CHECK(Engine_SaveGames(o));
	PutSettings(o, "/mem/named/own.json", "Named", 1);
	Engine_LoadGames(o);
	CHECK(HasGame(o, "Named") && o->engine.gamesStatic && !Engine_SaveGames(o));
	o->engine.optGames[0] = 0;
	DropAt("/mem/named/own.json");
	for(i = 0; i < 3; i++)
		DropAt(kPlaces[i]);

	// nothing can be written beside the binary and there is no file: still the user's data folder
	snprintf(gMem.fixed, sizeof gMem.fixed, "/mem/bin/");
	Engine_LoadGames(o);
	CHECK(strcmp(o->engine.gamesPath, kPlaces[2]) == 0 && Find(kPlaces[2], 1) != NULL && !Find(kPlaces[1], 1));
	gMem.fixed[0] = 0;

	// no user's data folder: made beside the binary; and when that cannot be written either, kept in memory
	DropAt(kPlaces[2]);
	gMem.noData = 1;
	Engine_LoadGames(o);
	CHECK(strcmp(o->engine.gamesPath, kPlaces[1]) == 0 && Find(kPlaces[1], 1) != NULL && Engine_GameCount(o) == games);
	DropAt(kPlaces[1]);
	snprintf(gMem.fixed, sizeof gMem.fixed, "/mem/bin/");
	Engine_LoadGames(o);
	CHECK(!Find(kPlaces[1], 1) && Engine_GameCount(o) == games && !Engine_SaveGames(o));
	gMem.fixed[0] = 0;
	gMem.noData = 0;

	// a file that cannot be used is kept beside the one that takes its place
	Put(kPlaces[2], 1, "{ no json", 9, OPENBGI_ATTR_ARCHIVE);
	Engine_LoadGames(o);
	CHECK(strcmp(o->engine.gamesPath, kPlaces[2]) == 0 && Engine_GameCount(o) == games);
	f = Find("/mem/data/openbgi.json.bad", 1);
	CHECK(f != NULL && f->size == 9);
	f = Find(kPlaces[2], 1);
	CHECK(f != NULL && f->size > 9);
	DropAt("/mem/data/openbgi.json.bad");
	DropAt(kPlaces[2]);
	o->engine.gamesPath[0] = 0;
	Engine_ResetGames(o);
}

static void TestSjis(void)
{
	// "\x83\x5c\x83\x74\x83\x67\x95\x5c" is "ソフト表": two of its trail bytes are 0x5c
	static const char kZip[] = "D:\\games\\\x83\x5c\x83\x74\x83\x67\x95\x5c.zip";
	OpenBGI_Files_t files = kMemFiles;
	OpenBGI_Host_t host = gMemHost;
	OpenBGI_t* o;
	char folder[0x100];
	printf("sjis\n");
	files.nativeSep = '\\';
	files.nativeSjis = 1;
	host.files = &files;
	o = OpenBGI_New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	CHECK(strcmp(Host_NativeFilePart(o, kZip), "\x83\x5c\x83\x74\x83\x67\x95\x5c.zip") == 0);
	CHECK(strcmp(Host_NativeFilePart(o, "D:\\\x95\x5c"), "\x95\x5c") == 0); // no separator at the end
	CHECK(strcmp(Host_NativeFilePart(o, "D:\\a/b"), "b") == 0);             // '/' is one too
	CHECK(*Host_NativeFilePart(o, "D:\\a\\") == 0);
	CHECK(strcmp(Host_NativeFilePart(o, "name"), "name") == 0);
	TestHost_SetEnv("BGI_DATA", "");
	CHECK(Fs_GameFolder(o, kZip, folder, sizeof folder));
	CHECK(strcmp(folder, "/mem/data/games\\\x83\x5c\x83\x74\x83\x67\x95\x5c\\") == 0);
	CHECK(Host_NativeStat(o, "/mem/data/games\\\x83\x5c\x83\x74\x83\x67\x95\x5c", NULL) == OPENBGI_NATIVE_DIR);
	CHECK(NativeDirsEndingIn(0x83) == 0 && NativeDirsEndingIn(0x95) == 0); // no folder cut at a trail byte
	OpenBGI_Delete(o);
	// where native paths are not Shift-JIS, a backslash is part of a name
	o = OpenBGI_New(&gMemHost);
	if(o)
	{
		CHECK(strcmp(Host_NativeFilePart(o, "/a/b\\c"), "b\\c") == 0);
		OpenBGI_Delete(o);
	}
}

static void TestRegistry(OpenBGI_t* o)
{
	char buf[0x104];
	printf("registry\n");
	CHECK(!RegGetInstalledFolder(o, buf, "Company", "Product"));
	CHECK(RegSetInstalledFolder(o, "Company", "Product", "D:\\Games\\Product"));
	CHECK(RegSlot("HKLM\\Software\\Company\\Product", "InstalledFolder", 0) >= 0);
	CHECK(RegGetInstalledFolder(o, buf, "Company", "Product") && strcmp(buf, "D:\\Games\\Product") == 0);
	CHECK(RegDeleteSoftwareKey(o, "Company", "Product"));
	CHECK(!RegGetInstalledFolder(o, buf, "Company", "Product"));
	CHECK(RegisterFileAssoc(o, "sav", "Game.Save", "a save", "C:\\game\\icon.ico", "C:\\game\\game.exe \"%1\""));
	CHECK(gMem.regKeys == 4);
	CHECK(RegSlot("HKCR\\.sav", NULL, 0) >= 0 && strcmp(gMem.reg[RegSlot("HKCR\\.sav", NULL, 0)].data, "Game.Save") == 0);
	CHECK(GetSpecialFolder(o, buf, 1) && gMem.csidl == 0x10 && strcmp(buf, "C:\\folder16") == 0);
	CHECK(GetSpecialFolder(o, buf, 3) && gMem.csidl == 5);
	CHECK(GetSpecialFolder(o, buf, 0) && strcmp(buf, "C:\\WINDOWS") == 0);
	// "81 00" tells the host
	Text_SetEncoding(o, TEXT_UTF8);
	CHECK(gMem.encoding == 1);
	Text_SetEncoding(o, TEXT_SJIS);
	CHECK(gMem.encoding == 0);
}

// the text of the box the engine asks for next, answered at once
static const char* BoxAsked(OpenBGI_t* o, const char* text)
{
	static char got[0x100];
	OpenBGI_Request_t q = {.size = sizeof q};
	uint32_t id = Request_Box(o, text, "caption", 0, REQUEST_TREAT_NONE);
	got[0] = 0;
	CHECK(id != 0 && OpenBGI_NextRequest(o, &q) && q.kind == OPENBGI_ASK_MESSAGE);
	if(q.u.message.text)
		snprintf(got, sizeof got, "%s", q.u.message.text);
	OpenBGI_Answer(o, q.id, 1);
	Request_Forget(o, id);
	return got;
}

/* A box's text reaches the host in the encoding the host was told: the
 * games that switch to UTF-8 keep their system scripts' notices in
 * Shift-JIS.  And every start tells the host Shift-JIS again, whatever
 * the run before it chose (the launcher runs in UTF-8). */
static void TestBoxText(void)
{
	static const char kSjis[] = "\x83\x65\x83\x58\x83\x67 [ a ]";             // "テスト [ a ]"
	static const char kUtf8[] = "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88 [ a ]"; // the same
	OpenBGI_t* o = OpenBGI_New(&gMemHost);
	printf("box text\n");
	CHECK(o != NULL);
	if(!o)
		return;
	CHECK(strcmp(BoxAsked(o, kSjis), kSjis) == 0); // Shift-JIS mode: as it is
	Text_SetEncoding(o, TEXT_UTF8);
	CHECK(gMem.encoding == 1);
	CHECK(strcmp(BoxAsked(o, kSjis), kUtf8) == 0); // converted
	CHECK(strcmp(BoxAsked(o, kUtf8), kUtf8) == 0); // UTF-8 already
	CHECK(strcmp(BoxAsked(o, "plain"), "plain") == 0);
	Engine_Renew(o);
	CHECK(gMem.encoding == 0 && !o->enc.textUtf8);
	OpenBGI_Delete(o);
}

static void TestOs(void)
{
	OpenBGI_Host_t host = gBareHost;
	OpenBGI_t* o;
	char dir[0x400], cwd[0x400], path[0x500], game[0x500];
	HostFile_t* f;
	uint8_t buf[16];
	uint8_t* d;
	uint32_t size, lo, hi;
	printf("os\n");
	// the OS layer's host has them
	CHECK(OS_Host()->files == OsCommon_Files() && OS_Host()->registry == OsCommon_Registry());
	CHECK(OS_Host()->textEncoding != NULL);
	if(OS_Host()->textEncoding)
	{
		OS_Host()->textEncoding(OS_Host()->user, 1);
		CHECK(gOsTextUtf8 == 1);
		OS_Host()->textEncoding(OS_Host()->user, 0);
		CHECK(gOsTextUtf8 == 0);
	}
	host.files = OsCommon_Files();
	host.registry = OsCommon_Registry();
	o = OpenBGI_New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	OS_NativeAbsolute(".", cwd, sizeof cwd);
	TestHost_ScratchName(dir, sizeof dir, "openbgi_hostfiles");
	TestHost_Mkdir(dir);
	CHECK(Host_NativeSep(o) == OS_NATIVE_SEP);
	snprintf(path, sizeof path, "%s%cnative.bin", dir, Host_NativeSep(o));
	// a native stream knows its size and position
	f = Host_NativeOpen(o, path, OPENBGI_OPEN_CREATE);
	CHECK(f != NULL);
	if(f)
	{
		CHECK(Host_FileWrite(o, f, "0123456789", 10) == 10 && Host_FileSize(o, f) == 10);
		CHECK(Host_FileSeek(o, f, 3) && Host_FileWrite(o, f, "ab", 2) == 2 && Host_FileSize(o, f) == 10);
		CHECK(!Host_FileGetTime(o, f, &lo, &hi)); // a native stream has no FILETIME here
		Host_FileClose(o, f);
	}
	f = Host_NativeOpen(o, path, OPENBGI_OPEN_READ);
	CHECK(f && Host_FileSize(o, f) == 10);
	if(f)
	{
		CHECK(Host_FileSeek(o, f, 8) && Host_FileRead(o, f, buf, sizeof buf) == 2 && memcmp(buf, "89", 2) == 0);
		CHECK(Host_FileSeek(o, f, 0) && Host_FileRead(o, f, buf, 5) == 5 && memcmp(buf, "012ab", 5) == 0);
		Host_FileClose(o, f);
	}
	CHECK(Host_NativeOpen(o, path, OPENBGI_OPEN_WRITE) == NULL); // native files are read or created
	d = Host_ReadNativeFile(o, path, &size);
	CHECK(d && size == 10 && d[10] == 0);
	free(d);
	snprintf(path, sizeof path, "%s%cnone.bin", dir, Host_NativeSep(o));
	CHECK(Host_NativeOpen(o, path, OPENBGI_OPEN_READ) == NULL);
	// a game file: 32-bit positions
	CHECK(Host_SetGameDir(o, dir));
	Host_ExeDir(o, game, sizeof game);
	strncat(game, "game.bin", sizeof game - strlen(game) - 1);
	f = Host_FileOpen(o, game, OPENBGI_OPEN_CREATE);
	CHECK(f != NULL);
	if(f)
	{
		CHECK(Host_FileWrite(o, f, "game", 4) == 4);
		CHECK(!Host_FileSeek(o, f, (uint64_t)1 << 32));
		CHECK(Host_FileSeek(o, f, 2) && Host_FileSize(o, f) == 4);
		CHECK(Host_FileGetTime(o, f, &lo, &hi));
		Host_FileClose(o, f);
	}
	CHECK(Host_FileAttrs(o, game) != OPENBGI_INVALID_ATTRS && Host_FileDelete(o, game));
	CHECK(Host_FileAttrs(o, game) == OPENBGI_INVALID_ATTRS);
	OpenBGI_Delete(o);
	TestHost_Chdir(cwd);
	TestHost_RemoveTree(dir);
}

int main(void)
{
	OpenBGI_t* o;
	MemReset();
	TestNone();
	o = OpenBGI_New(&gMemHost);
	CHECK(o != NULL);
	if(o)
	{
		o->files.arcMgr = FileSet_New();
		TestGame(o);
		TestDrives(o);
		TestNative(o);
		TestSettings(o);
		TestSjis();
		TestRegistry(o);
		TestBoxText();
		CHECK(gMem.calls > 0 && gMem.badUser == 0);
		FileSet_Delete(o, o->files.arcMgr);
		o->files.arcMgr = NULL;
		OpenBGI_Delete(o);
	}
	OS_Init();
	TestOs();
	OS_Shutdown();
	MemReset();
	if(gFails)
	{
		printf("hostfiles: %d checks failed\n", gFails);
		return 1;
	}
	printf("hostfiles: all tests passed\n");
	return 0;
}
