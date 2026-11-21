/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * launcher_src.c - the launcher's program (src/include/bgi/launcher.h): its
 *            sources, kept in the user's data folder and compiled at
 *            every start; not part of the original engine
 *
 * The host hands the engine the sources of the launcher, packed
 * (OpenBGI_SetLauncher; tools/mkpack.py describes the pack).  At a start
 * without a game they are written into the folder LAUNCHER_SOURCES of
 * the user's data folder - a file that is not there yet - and the first
 * of them is compiled, there, by the compiler the engine has in it
 * (bgi/bpc.h).  What the folder holds is what runs: a change of a file
 * shows at the next start.
 *
 * The folder is kept up to date with the engine.  LAUNCHER_WRITTEN in it
 * notes a checksum of every file the engine wrote and of the pack they
 * came from, which tells a file somebody changed from one nobody
 * touched, and a newer pack from the one the folder was written from:
 *
 *   the pack is the same      missing files are written, nothing else
 *   a newer pack, no changes  every file is replaced
 *   a newer pack, changes     the player is asked: keep them, or replace
 *                             them (the old files go into a folder
 *                             "backup-<date>-<time>" there first); a
 *                             "keep" is noted and not asked again until
 *                             the pack changes once more
 *
 * A "change" is one of a source (.bpc, .inc); the other files of the
 * pack follow along.  When the folder's sources do not compile the
 * messages are shown and the pack's own are compiled instead, from
 * memory; the same when there is no folder to write to.
 */
#include "bgi/bpc.h"
#include "bgi/bytes.h"
#include "bgi/deflate.h"
#include "bgi/engine.h"
#include "bgi/error.h"
#include "bgi/file.h"
#include "bgi/host.h"
#include "bgi/msg.h"
#include "bgi/core.h"
#include "bgi/request.h"
#include "bgi/launcher.h"
#include "bgi/sjis.h"
#include "bgi/version.h"

#include <ctype.h>

#define SRC_NAME      48         // bytes of a file's name in the pack, the NUL included
#define MESSAGE_LINES 5          // lines of the compiler's messages shown in a box,
#define MESSAGE_WIDTH 96         // .. and characters of each
#define FILE_MAX      0x1000000u // a source file larger than this is none
static const char kPackMagic[8] = {'O', 'B', 'G', 'I', 'P', 'A', 'C', 'K'};

typedef struct SrcFile
{
	char name[SRC_NAME];
	uint8_t* data; // the file as the pack has it, a NUL after it
	uint32_t size;
	uint64_t hash; // of those bytes
	// what the folder has
	int there;      // a file of that name
	uint64_t disk;  // .. and the checksum of its bytes
	int noted;      // LAUNCHER_WRITTEN names it
	uint64_t wrote; // .. with this checksum
} SrcFile_t;

struct LauncherSources
{
	SrcFile_t f[LAUNCHER_FILES];
	int count;
	uint64_t hash;           // of the whole pack
	char dir[LAUNCHER_PATH]; // the folder, a separator after it; "" when there is none to use
	int hasFrom, hasAsked;
	uint64_t from;   // LAUNCHER_WRITTEN: the pack the folder was written from
	uint64_t asked;  // .. and the pack the player was asked about and kept the folder's files over
	char backup[40]; // the folder a "replace" keeps the old files in
};

void OpenBGI_SetLauncher(OpenBGI_t* o, const void* sources, uint32_t size)
{
	o->launcher.pack = sources && size ? (const uint8_t*)sources : NULL;
	o->launcher.packSize = sources ? size : 0;
}

// ---- the pack --------------------------------------------------------------------------------

static uint64_t Hash(uint64_t h, const void* data, size_t n)
{ // FNV-1a
	const uint8_t* p = (const uint8_t*)data;
	size_t i;
	for(i = 0; i < n; i++)
		h = (h ^ p[i]) * 0x100000001b3ull;
	return h;
}

#define HASH_START 0xcbf29ce484222325ull

static void SourcesFree(LauncherSources_t* s)
{
	int i;
	if(!s)
		return;
	for(i = 0; i < s->count; i++)
		free(s->f[i].data);
	free(s);
}

// the files of a pack in memory; NULL when it is not one, or holds nothing
static LauncherSources_t* Unpack(const uint8_t* pack, uint32_t size)
{
	LauncherSources_t* s;
	uint32_t at = 12, count, i;
	if(!pack || size < 12 || memcmp(pack, kPackMagic, 8) != 0)
		return NULL;
	count = Le32(pack + 8);
	if(!count || count > LAUNCHER_FILES || !(s = (LauncherSources_t*)calloc(1, sizeof *s)))
		return NULL;
	s->hash = HASH_START;
	for(i = 0; i < count; i++)
	{
		SrcFile_t* f = &s->f[s->count];
		uint32_t nameLen, method, raw, packed;
		size_t got = 0;
		if(at + 1 > size || (nameLen = pack[at]) == 0 || nameLen >= SRC_NAME || at + 1 + nameLen + 9 > size)
			break;
		memcpy(f->name, pack + at + 1, nameLen);
		at += 1 + nameLen;
		method = pack[at];
		raw = Le32(pack + at + 1);
		packed = Le32(pack + at + 5);
		at += 9;
		if(packed > size - at || raw > FILE_MAX)
			break;
		if(method == 8)
		{
			uint8_t* d = Deflate_InflateRaw(pack + at, packed, raw, &got);
			if(d && got == raw && (f->data = (uint8_t*)malloc((size_t)raw + 1)) != NULL)
				memcpy(f->data, d, raw);
			free(d);
		}
		else if(method == 0 && packed == raw && (f->data = (uint8_t*)malloc((size_t)raw + 1)) != NULL)
			memcpy(f->data, pack + at, raw);
		if(!f->data)
			break;
		f->data[raw] = 0;
		f->size = raw;
		f->hash = Hash(HASH_START, f->data, raw);
		s->hash = Hash(Hash(s->hash, f->name, nameLen + 1), f->data, raw);
		at += packed;
		s->count++;
	}
	if((uint32_t)s->count != count)
	{
		SourcesFree(s);
		return NULL;
	}
	return s;
}

// ---- the folder ------------------------------------------------------------------------------

static int IsSource(const char* name)
{
	size_t n = strlen(name);
	char ext[4];
	int i;
	if(n < 5 || name[n - 4] != '.')
		return 0;
	for(i = 0; i < 3; i++)
		ext[i] = (char)tolower((unsigned char)name[n - 3 + i]);
	ext[3] = 0;
	return strcmp(ext, "bpc") == 0 || strcmp(ext, "inc") == 0;
}

// sixteen hex digits at `p` into *h; 1 when they are there
static int ReadHash(const char* p, uint64_t* h)
{
	int i;
	*h = 0;
	for(i = 0; i < 16; i++)
	{
		int c = tolower((unsigned char)p[i]);
		if(!isxdigit(c))
			return 0;
		*h = *h << 4 | (uint64_t)(c <= '9' ? c - '0' : c - 'a' + 10);
	}
	return 1;
}

static void PathOf(const LauncherSources_t* s, const char* name, char* out, size_t n)
{
	snprintf(out, n, "%s%s", s->dir, name);
}

static SrcFile_t* FileNamed(LauncherSources_t* s, const char* name)
{
	int i;
	for(i = 0; i < s->count; i++)
		if(strcmp(s->f[i].name, name) == 0)
			return &s->f[i];
	return NULL;
}

// where the sources live: the folder made when it is missing, "" when the user's data has no place
static void FindFolder(OpenBGI_t* o, LauncherSources_t* s)
{
	char data[LAUNCHER_PATH - 32];
	size_t n;
	s->dir[0] = 0;
	Host_UserDataDir(o, data, sizeof data);
	if(!data[0])
		return;
	snprintf(s->dir, sizeof s->dir, "%s", data);
	n = strlen(s->dir);
	if(n > 1)
	{ // the data folder itself, by its name without the separator
		s->dir[n - 1] = 0;
		Host_NativeMkdir(o, s->dir);
	}
	snprintf(s->dir, sizeof s->dir, "%s" LAUNCHER_SOURCES, data);
	if(!Host_NativeMkdir(o, s->dir))
	{
		s->dir[0] = 0;
		return;
	}
	snprintf(s->dir, sizeof s->dir, "%s" LAUNCHER_SOURCES "%c", data, Host_NativeSep(o));
}

// what LAUNCHER_WRITTEN says: "from HASH", "asked HASH", "file HASH NAME"
static void ReadWritten(OpenBGI_t* o, LauncherSources_t* s)
{
	char path[LAUNCHER_PATH + 64];
	uint32_t size = 0;
	char* text;
	char* line;
	PathOf(s, LAUNCHER_WRITTEN, path, sizeof path);
	text = (char*)Host_ReadNativeFile(o, path, &size);
	for(line = text; line && *line;)
	{
		char* end = strchr(line, '\n');
		uint64_t h;
		if(end)
			*end = 0;
		if(strncmp(line, "from ", 5) == 0 && ReadHash(line + 5, &h))
		{
			s->hasFrom = 1;
			s->from = h;
		}
		else if(strncmp(line, "asked ", 6) == 0 && ReadHash(line + 6, &h))
		{
			s->hasAsked = 1;
			s->asked = h;
		}
		else if(strncmp(line, "file ", 5) == 0)
		{ // "file HASH NAME"
			SrcFile_t* f = ReadHash(line + 5, &h) && line[21] == ' ' ? FileNamed(s, line + 22) : NULL;
			size_t n = strlen(line);
			if(!f && n && line[n - 1] == '\r')
			{ // (a file that an editor gave CR LF)
				line[n - 1] = 0;
				f = ReadHash(line + 5, &h) && line[21] == ' ' ? FileNamed(s, line + 22) : NULL;
			}
			if(f)
			{
				f->noted = 1;
				f->wrote = h;
			}
		}
		line = end ? end + 1 : NULL;
	}
	free(text);
}

static size_t AddHash(char* text, size_t at, size_t cap, const char* word, uint64_t h, const char* name)
{
	int n = snprintf(text + at, cap - at, "%s %08x%08x%s%s\n", word, (unsigned)(h >> 32), (unsigned)(h & 0xffffffffu), name ? " " : "", name ? name : "");
	return at + (size_t)(n > 0 ? n : 0);
}

static void WriteWritten(OpenBGI_t* o, LauncherSources_t* s)
{
	char path[LAUNCHER_PATH + 64];
	size_t cap = 0x200 + (size_t)s->count * (SRC_NAME + 32), n;
	char* text = (char*)malloc(cap);
	int i;
	if(!text)
		return;
	n = (size_t)snprintf(text, cap,
		"# What OpenBGI wrote into this folder: a checksum of each file and\n"
		"# of the set they came from.  It tells a file that was changed since from one\n"
		"# that was not, when a newer OpenBGI brings newer files.\n");
	if(s->hasFrom)
		n = AddHash(text, n, cap, "from", s->from, NULL);
	if(s->hasAsked)
		n = AddHash(text, n, cap, "asked", s->asked, NULL);
	for(i = 0; i < s->count; i++)
		if(s->f[i].noted)
			n = AddHash(text, n, cap, "file", s->f[i].wrote, s->f[i].name);
	PathOf(s, LAUNCHER_WRITTEN, path, sizeof path);
	Host_WriteNativeFile(o, path, text, (uint32_t)n);
	free(text);
}

// what the folder has of the pack's files
static void Look(OpenBGI_t* o, LauncherSources_t* s)
{
	char path[LAUNCHER_PATH + 64];
	int i;
	for(i = 0; i < s->count; i++)
	{
		SrcFile_t* f = &s->f[i];
		uint32_t size = 0;
		uint8_t* d;
		PathOf(s, f->name, path, sizeof path);
		f->there = Host_NativeStat(o, path, NULL) == OPENBGI_NATIVE_FILE;
		d = f->there ? Host_ReadNativeFile(o, path, &size) : NULL;
		f->disk = d ? Hash(HASH_START, d, size) : 0;
		free(d);
	}
}

// the pack's file into the folder; 1 when it is there now
static int Put(OpenBGI_t* o, LauncherSources_t* s, SrcFile_t* f)
{
	char path[LAUNCHER_PATH + 64];
	PathOf(s, f->name, path, sizeof path);
	if(!Host_WriteNativeFile(o, path, f->data, f->size))
		return 0;
	f->there = f->noted = 1;
	f->disk = f->wrote = f->hash;
	return 1;
}

// every file of the pack that is missing (all = 0) or not the pack's (all = 1); 1 when anything was written
static int PutFiles(OpenBGI_t* o, LauncherSources_t* s, int all)
{
	int i, any = 0;
	for(i = 0; i < s->count; i++)
		if(!s->f[i].there || (all && s->f[i].disk != s->f[i].hash))
			any |= Put(o, s, &s->f[i]);
	return any;
}

// a source of the folder is not what the engine wrote (or, where nothing says what it wrote, not the pack's)
static int Changed(const LauncherSources_t* s)
{
	int i;
	for(i = 0; i < s->count; i++)
	{
		const SrcFile_t* f = &s->f[i];
		if(f->there && IsSource(f->name) && f->disk != (f->noted ? f->wrote : f->hash))
			return 1;
	}
	return 0;
}

// the pack is not the one the folder was written from
static int Newer(const LauncherSources_t* s)
{
	int i;
	if(s->hasFrom)
		return s->from != s->hash;
	for(i = 0; i < s->count; i++)
		if(s->f[i].there && s->f[i].disk != s->f[i].hash)
			return 1;
	return 0;
}

// the folder's files are the pack's from here on
static void NowFrom(OpenBGI_t* o, LauncherSources_t* s)
{
	int i;
	for(i = 0; i < s->count; i++)
		if(s->f[i].there && s->f[i].disk == s->f[i].hash)
		{ // (also a file that was the pack's all along)
			s->f[i].noted = 1;
			s->f[i].wrote = s->f[i].hash;
		}
	s->hasFrom = 1;
	s->from = s->hash;
	s->hasAsked = 0;
	WriteWritten(o, s);
}

// a name for the folder of the old files that nothing has yet
static void BackupName(OpenBGI_t* o, LauncherSources_t* s)
{
	OpenBGI_DateTime_t dt;
	char path[LAUNCHER_PATH + 64];
	int k;
	memset(&dt, 0, sizeof dt);
	o->host.localTime(o->host.user, &dt);
	for(k = 1; k < 100; k++)
	{
		int n = snprintf(s->backup, sizeof s->backup, "backup-%04u%02u%02u-%02u%02u%02u", (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
			(unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second);
		if(k > 1)
			snprintf(s->backup + n, sizeof s->backup - (size_t)n, "-%d", k);
		PathOf(s, s->backup, path, sizeof path);
		if(Host_NativeStat(o, path, NULL) == OPENBGI_NATIVE_NONE)
			return;
	}
}

// a native path as the engine's text, for a message
static void PathText(OpenBGI_t* o, const char* native, char* out, size_t n)
{
	char utf8[LAUNCHER_PATH + 64];
	Host_NativeToUtf8(o, native, utf8, sizeof utf8);
	Sjis_FromUtf8(utf8, out, n);
}

int Launcher_Wanted(OpenBGI_t* o)
{
	return !o->engine.optLauncherDialog && o->launcher.pack && o->launcher.packSize;
}

int Launcher_Prepare(OpenBGI_t* o)
{
	LauncherSources_t* s;
	SourcesFree(o->launcher.src);
	o->launcher.askReq = 0;
	s = o->launcher.src = Unpack(o->launcher.pack, o->launcher.packSize);
	if(!s)
		return 0;
	FindFolder(o, s);
	if(!s->dir[0])
		return 0; // nowhere to keep them: the pack's own are compiled
	ReadWritten(o, s);
	Look(o, s);
	if(!Newer(s))
	{ // the folder is this pack's: only what is missing is written
		if(PutFiles(o, s, 0) || !s->hasFrom)
			NowFrom(o, s);
		return 0;
	}
	if(!Changed(s))
	{ // a newer pack, and nobody changed the old files
		PutFiles(o, s, 1);
		NowFrom(o, s);
		return 0;
	}
	if(s->hasAsked && s->asked == s->hash)
	{ // asked before, and the answer was to keep them
		if(PutFiles(o, s, 0))
			WriteWritten(o, s);
		return 0;
	}
	{
		char text[LAUNCHER_PATH + 0x400], where[LAUNCHER_PATH + 64];
		BackupName(o, s);
		PathText(o, s->dir, where, sizeof where);
		snprintf(text, sizeof text, MSG_LAUNCHER_NEWER, where, s->backup);
		fprintf(stderr, "openbgi: this OpenBGI has newer sources of the launcher than the changed ones in %s\n", s->dir);
		o->launcher.askReq = MsgBox_Ask(o, text, "OpenBGI", OPENBGI_MB_YESNO | OPENBGI_MB_ICONQUESTION | OPENBGI_MB_DEFBUTTON2);
		if(!o->launcher.askReq)
		{ // nobody to ask: they are kept, and it is asked again next time
			if(PutFiles(o, s, 0))
				WriteWritten(o, s);
			return 0;
		}
	}
	return 1;
}

void Launcher_Answer(OpenBGI_t* o, int replace)
{
	LauncherSources_t* s = o->launcher.src;
	char dir[LAUNCHER_PATH + 64], from[LAUNCHER_PATH + 64], to[LAUNCHER_PATH + 128];
	int i, ok;
	o->launcher.askReq = 0;
	if(!s || !s->dir[0])
		return;
	if(!replace)
	{
		s->hasAsked = 1;
		s->asked = s->hash;
		PutFiles(o, s, 0);
		WriteWritten(o, s);
		return;
	}
	// the old files into their folder first; without it nothing is replaced
	PathOf(s, s->backup, dir, sizeof dir);
	ok = s->backup[0] && Host_NativeMkdir(o, dir);
	for(i = 0; ok && i < s->count; i++)
	{
		uint32_t size = 0;
		uint8_t* d;
		if(!s->f[i].there || s->f[i].disk == s->f[i].hash)
			continue;
		PathOf(s, s->f[i].name, from, sizeof from);
		snprintf(to, sizeof to, "%s%c%s", dir, Host_NativeSep(o), s->f[i].name);
		d = Host_ReadNativeFile(o, from, &size);
		ok = d && Host_WriteNativeFile(o, to, d, size);
		free(d);
	}
	if(!ok)
	{
		fprintf(stderr, "openbgi: the launcher's old sources could not be kept in %s: nothing is replaced\n", dir);
		return;
	}
	fprintf(stderr, "openbgi: the launcher's old sources are kept in %s\n", dir);
	PutFiles(o, s, 1);
	NowFrom(o, s);
}

// ---- the compile -----------------------------------------------------------------------------

typedef struct ReadCtx
{
	OpenBGI_t* o;
	LauncherSources_t* s;
} ReadCtx_t;

static uint8_t* ReadFolder(void* ctx, const char* path, uint32_t* size)
{
	ReadCtx_t* c = (ReadCtx_t*)ctx;
	uint8_t* d;
	*size = 0;
	if(Host_NativeStat(c->o, path, NULL) != OPENBGI_NATIVE_FILE)
		return NULL;
	d = Host_ReadNativeFile(c->o, path, size);
	if(d && *size > FILE_MAX)
	{
		free(d);
		*size = 0;
		return NULL;
	}
	return d;
}

static uint8_t* ReadPack(void* ctx, const char* path, uint32_t* size)
{
	ReadCtx_t* c = (ReadCtx_t*)ctx;
	SrcFile_t* f = FileNamed(c->s, path);
	uint8_t* d;
	*size = 0;
	if(!f || !(d = (uint8_t*)malloc((size_t)f->size + 1)))
		return NULL;
	memcpy(d, f->data, (size_t)f->size + 1);
	*size = f->size;
	return d;
}

static void NameToNative(void* ctx, const char* utf8, char* out, size_t n)
{
	Host_Utf8ToNative(((ReadCtx_t*)ctx)->o, utf8, out, n);
}

static EngineGen_t LauncherGen(void)
{
	int g;
	for(g = 0; g < GEN_COUNT; g++)
		if(strcmp(kGenNames[g], LAUNCHER_PROFILE) == 0)
			return (EngineGen_t)g;
	return GEN_COUNT;
}

/* Compile the first file: the folder's (folder = 1) or the pack's own.
 * 1 and *out when it compiled; else 0, with what the compiler said in
 * *messages (free() it; NULL when it said nothing, or the file is not
 * there at all). */
static int CompileSources(OpenBGI_t* o, LauncherSources_t* s, int folder, BpcResult_t* out, char** messages)
{
	ReadCtx_t ctx;
	PrepIncludes_t inc;
	char path[LAUNCHER_PATH + 64];
	uint8_t* text;
	uint32_t size = 0;
	*messages = NULL;
	memset(out, 0, sizeof *out);
	memset(&inc, 0, sizeof inc);
	ctx.o = o;
	ctx.s = s;
	inc.ctx = &ctx;
	if(folder)
	{
		PathOf(s, s->f[0].name, path, sizeof path);
		inc.read = ReadFolder;
		inc.fromUtf8 = NameToNative;
		inc.sep = Host_NativeSep(o);
		inc.sjis = Host_NativeSjis(o);
		inc.foldCase = inc.sjis; // (the hosts whose paths are Shift-JIS are those that fold the case)
	}
	else
	{
		snprintf(path, sizeof path, "%s", s->f[0].name);
		inc.read = ReadPack;
	}
	text = inc.read(&ctx, path, &size);
	if(!text)
		return 0;
	Bpc_CompileText(o, (const char*)text, size, path, LauncherGen(), &inc, messages, out);
	free(text);
	BGI_Free(out->labels);
	out->labels = NULL;
	if(!out->errors && out->file)
		return 1;
	BGI_Free(out->file);
	out->file = NULL;
	return 0;
}

/* The compiler's messages as a box shows them: the folder's own name
 * taken out, the first MESSAGE_LINES of them, none longer than
 * MESSAGE_WIDTH (the engine's own box has room for some twenty lines
 * of a hundred characters, and the text around the messages takes eight). */
static void ShortMessages(const LauncherSources_t* s, const char* messages, char* out, size_t n)
{
	size_t dirLen = strlen(s->dir), at = 0;
	int lines = 0, col = 0;
	const char* p = messages ? messages : "";
	out[0] = 0;
	while(*p && at + 8 < n)
	{
		if(dirLen && strncmp(p, s->dir, dirLen) == 0)
		{
			p += dirLen;
			continue;
		}
		if(*p == '\n')
		{
			col = 0;
			if(++lines == MESSAGE_LINES)
			{
				if(p[1])
					at += (size_t)snprintf(out + at, n - at, "\n...");
				break;
			}
		}
		else if(++col > MESSAGE_WIDTH)
		{
			p++;
			continue;
		}
		out[at++] = *p++;
		out[at] = 0;
	}
	if(at && at + 1 < n && out[at - 1] != '\n')
	{
		out[at++] = '\n';
		out[at] = 0;
	}
}

int Launcher_Boot(OpenBGI_t* o)
{
	LauncherSources_t* s = o->launcher.src;
	char dir[LAUNCHER_PATH + 16];
	BpcResult_t res;
	char* said = NULL;
	int ok = 0;
	if(!s)
		s = o->launcher.src = Unpack(o->launcher.pack, o->launcher.packSize);
	if(!s)
		return 0;
	memset(&res, 0, sizeof res);
	Host_UserDataDir(o, dir, sizeof dir);
	if(!dir[0])
		Host_BinaryDir(o, dir, sizeof dir);
	if(dir[0] && Host_SetGameDir(o, dir))
	{
		if(s->dir[0])
		{
			ok = CompileSources(o, s, 1, &res, &said);
			if(!ok && said)
			{ // the folder's sources are broken: say so, and go on with the pack's own
				char where[LAUNCHER_PATH + 64], brief[0x600];
				char* text = (char*)malloc(sizeof where + sizeof brief + 0x200);
				fprintf(stderr, "openbgi: the launcher's sources in %s do not compile:\n%sopenbgi: the launcher built into OpenBGI runs instead\n", s->dir,
					said);
				PathText(o, s->dir, where, sizeof where);
				ShortMessages(s, said, brief, sizeof brief);
				if(text)
				{
					snprintf(text, sizeof where + sizeof brief + 0x200, MSG_LAUNCHER_BROKEN, where, brief);
					MsgBox(o, text, "OpenBGI", OPENBGI_MB_ICONWARNING);
					free(text);
				}
			}
			free(said);
			said = NULL;
		}
		if(!ok)
		{
			ok = CompileSources(o, s, 0, &res, &said);
			if(!ok)
				fprintf(stderr, "openbgi: the launcher built into OpenBGI does not compile:\n%s", said ? said : "");
			free(said);
		}
	}
	SourcesFree(s);
	o->launcher.src = NULL;
	if(!ok)
		return 0;
	BGI_Free(o->launcher.compiled);
	o->launcher.compiled = res.file;
	// the compiled program is the file of that name in the game directory (fs.h)
	o->files.builtinName = LAUNCHER_PROGRAM;
	o->files.builtinData = res.file;
	o->files.builtinSize = res.size;
	o->launcher.running = 1;
	SetBootProgram(o, ".", LAUNCHER_PROGRAM);
	snprintf(o->engine.optEngine, sizeof o->engine.optEngine, "%s", LAUNCHER_PROFILE);
	o->engine.launcherMode = 1; // no sound device is needed
	o->engine.noMutex = 1;      // and no game's single-instance lock is taken
	return 1;
}

void Launcher_SourcesFree(OpenBGI_t* o)
{
	SourcesFree(o->launcher.src);
	o->launcher.src = NULL;
	o->launcher.askReq = 0;
	if(o->files.builtinData && o->files.builtinData == o->launcher.compiled)
	{
		o->files.builtinName = NULL;
		o->files.builtinData = NULL;
		o->files.builtinSize = 0;
	}
	BGI_Free(o->launcher.compiled);
	o->launcher.compiled = NULL;
}
