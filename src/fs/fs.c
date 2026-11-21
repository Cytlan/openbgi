/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fs.c - the engine's files over a folder or a zip archive (src/include/bgi/fs.h)
 *
 * Every call tries the host's files first: with no archive mounted that is
 * all there is, and with one mounted the host's game directory is the game
 * folder, whose contents win.  When the folder has nothing of the name, the
 * path's key is looked up in the archive.  A key is the path's place in the
 * game directory as the volume spells its entries (vol.h): UTF-8
 * components joined by '/', ASCII letters in lower case, "." and ".."
 * resolved; "" is the game directory itself.  A path is in the game
 * directory when it starts with the host's exeDir as it was at the mount, or is
 * relative while the current directory is in it.
 */
#include "bgi/fs.h"
#include "bgi/host.h"
#include "bgi/sjis.h"
#include "bgi/strutil.h"
#include "bgi/vol.h"
#include "bgi/core.h"

#include <time.h>

#define FS_OWN         ".openbgi"    // this layer's folder at the top of the game folder
#define FS_DELETED     "deleted.txt" // in it: the keys of what is gone from the archive, one per line
#define FS_INFLATE_MAX (32u << 20)   // a deflated entry larger than this is read from an extracted copy

struct FsFile
{
	OpenBGI_t* o;       // the engine whose host has the streams
	HostFile_t* os;     // a file of the folder (or an extracted copy); NULL for an entry read from the archive
	HostFile_t* stored; // a stored entry: the archive (a native stream), the entry's bytes at `base`
	uint64_t base;
	uint8_t* mem;    // a deflated entry, decompressed
	uint32_t size;   // an entry's size
	uint32_t pos;    // the position in it
	uint32_t timeLo; // its time stamp, a FILETIME
	uint32_t timeHi;
};

struct FsFind
{
	OpenBGI_FindData_t* items; // the merged listing: the game folder's entries, then the archive's
	uint32_t count, cap, next;
	char** folded; // while it is made: the game folder's names as UTF-8 in lower case
	uint32_t nFolded;
};

// ---- keys -------------------------------------------------------------------------------

// '\\' as '/', ASCII letters in lower case, in place
static void Fold(char* s)
{
	for(; *s; s++)
	{
		if(*s == '\\')
			*s = '/';
		else if(*s >= 'A' && *s <= 'Z')
			*s = (char)(*s + 32);
	}
}

/* the components of a '/'-separated UTF-8 path as a key: empty and "."
 * components dropped, ".." taking the one before it off; 0 when it climbs
 * above the start or does not fit */
static int Normalize(const char* s, char* key, size_t n)
{
	size_t off = 0;
	key[0] = 0;
	while(*s)
	{
		const char* start;
		size_t len;
		while(*s == '/')
			s++;
		start = s;
		while(*s && *s != '/')
			s++;
		len = (size_t)(s - start);
		if(len == 0 || (len == 1 && start[0] == '.'))
			continue;
		if(len == 2 && start[0] == '.' && start[1] == '.')
		{
			if(off == 0)
				return 0;
			while(off > 0 && key[off - 1] != '/')
				off--;
			if(off > 0)
				off--;
			key[off] = 0;
			continue;
		}
		if(off + len + 2 > n)
			return 0;
		if(off)
			key[off++] = '/';
		memcpy(key + off, start, len);
		off += len;
		key[off] = 0;
	}
	return 1;
}

// whether byte strings are equal up to `n` bytes, ignoring ASCII case and taking '/' for '\\'
static int SamePrefix(const char* a, const char* b, size_t n)
{
	size_t i;
	for(i = 0; i < n; i++)
	{
		int x = (uint8_t)a[i], y = (uint8_t)b[i];
		if(x >= 'A' && x <= 'Z')
			x += 32;
		if(y >= 'A' && y <= 'Z')
			y += 32;
		if(x == '\\')
			x = '/';
		if(y == '\\')
			y = '/';
		if(x != y)
			return 0;
	}
	return 1;
}

// the key of an engine path; 0 when the path is not in the game directory (or no archive is mounted)
static int KeyOf(OpenBGI_t* o, const char* path, char* key, size_t n)
{
	size_t len = strlen(o->files.fs.exeDir), plen = strlen(path);
	char utf[FS_PATH], joined[FS_PATH * 2];
	if(!o->files.fs.vol)
		return 0;
	if(plen >= len && SamePrefix(path, o->files.fs.exeDir, len))
		path += len; // under the game directory
	else if(len && plen == len - 1 && SamePrefix(path, o->files.fs.exeDir, plen))
		path += plen; // the game directory without its separator
	else if(path[0] == '\\' || path[0] == '/' || (path[0] && path[1] == ':'))
		return 0; // somewhere else
	else if(o->files.fs.cwdOutside)
		return 0; // relative to a current directory elsewhere
	else
	{
		Sjis_ToUtf8(path, utf, sizeof utf, o->enc.textUtf8);
		Fold(utf);
		snprintf(joined, sizeof joined, "%s/%s", o->files.fs.cwd, utf);
		return Normalize(joined, key, n);
	}
	Sjis_ToUtf8(path, utf, sizeof utf, o->enc.textUtf8);
	Fold(utf);
	return Normalize(utf, key, n);
}

// a key of this layer's own folder
static int IsOwn(const char* key)
{
	size_t n = sizeof FS_OWN - 1;
	return strncmp(key, FS_OWN, n) == 0 && (key[n] == 0 || key[n] == '/');
}

// UTF-8 as the engine's text: as it is in the UTF-8 mode, else Shift-JIS
static void ToEngine(OpenBGI_t* o, const char* utf8, char* out, size_t n)
{
	if(o->enc.textUtf8)
		snprintf(out, n, "%s", utf8);
	else
		Sjis_FromUtf8(utf8, out, n);
}

// the start of the last component of an engine path (multi-byte characters in the text encoding step as a whole)
static const char* FilePart(OpenBGI_t* o, const char* path)
{
	const char* p = path;
	const char* last = path;
	while(*p)
	{
		int len = TextCharLen(o, p);
		if(*p == '\\' || *p == '/')
			last = p + 1;
		if(len < 1)
			len = 1;
		while(len-- > 0 && *p) // a lead byte at the end is a character of its own
			p++;
	}
	return last;
}

// ---- what is gone from the archive -----------------------------------------------------

// the index of the first deleted key not below `key`
static uint32_t DeletedLower(OpenBGI_t* o, const char* key)
{
	uint32_t lo = 0, hi = o->files.fs.nDeleted;
	while(lo < hi)
	{
		uint32_t mid = lo + (hi - lo) / 2;
		if(strcmp(o->files.fs.deleted[mid], key) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static int IsDeleted(OpenBGI_t* o, const char* key)
{
	uint32_t i = DeletedLower(o, key);
	return i < o->files.fs.nDeleted && strcmp(o->files.fs.deleted[i], key) == 0;
}

// the native path of the list (FS_OWN made when `make` is set)
static void DeletedPath(OpenBGI_t* o, char* out, size_t n, int make)
{
	snprintf(out, n, "%s" FS_OWN, o->files.fs.folder);
	if(make)
		Host_NativeMkdir(o, out);
	snprintf(out + strlen(out), n - strlen(out), "%c" FS_DELETED, Host_NativeSep(o));
}

static void SaveDeleted(OpenBGI_t* o)
{
	char path[FS_PATH];
	size_t total = 1, off = 0;
	uint32_t i;
	char* text;
	for(i = 0; i < o->files.fs.nDeleted; i++)
		total += strlen(o->files.fs.deleted[i]) + 1;
	text = (char*)malloc(total);
	if(!text)
		return;
	for(i = 0; i < o->files.fs.nDeleted; i++)
		off += (size_t)sprintf(text + off, "%s\n", o->files.fs.deleted[i]);
	DeletedPath(o, path, sizeof path, 1);
	if(!Host_WriteNativeFile(o, path, text, (uint32_t)off))
		fprintf(stderr, "openbgi: cannot write %s\n", path);
	free(text);
}

// add a key (0 when it was there already or memory ran out)
static int MarkDeleted(OpenBGI_t* o, const char* key)
{
	uint32_t i = DeletedLower(o, key);
	char* copy;
	if(i < o->files.fs.nDeleted && strcmp(o->files.fs.deleted[i], key) == 0)
		return 0;
	if(o->files.fs.nDeleted == o->files.fs.capDeleted)
	{
		uint32_t cap = o->files.fs.capDeleted ? o->files.fs.capDeleted * 2 : 64;
		char** list = (char**)realloc(o->files.fs.deleted, cap * sizeof *list);
		if(!list)
			return 0;
		o->files.fs.deleted = list;
		o->files.fs.capDeleted = cap;
	}
	copy = (char*)malloc(strlen(key) + 1);
	if(!copy)
		return 0;
	strcpy(copy, key);
	memmove(o->files.fs.deleted + i + 1, o->files.fs.deleted + i, (o->files.fs.nDeleted - i) * sizeof *o->files.fs.deleted);
	o->files.fs.deleted[i] = copy;
	o->files.fs.nDeleted++;
	return 1;
}

// take a key off the list, saving it when it changed
static void Undelete(OpenBGI_t* o, const char* key)
{
	uint32_t i = DeletedLower(o, key);
	if(i == o->files.fs.nDeleted || strcmp(o->files.fs.deleted[i], key) != 0)
		return;
	free(o->files.fs.deleted[i]);
	memmove(o->files.fs.deleted + i, o->files.fs.deleted + i + 1, (o->files.fs.nDeleted - i - 1) * sizeof *o->files.fs.deleted);
	o->files.fs.nDeleted--;
	SaveDeleted(o);
}

static void LoadDeleted(OpenBGI_t* o)
{
	char path[FS_PATH];
	uint32_t size;
	char* text;
	char* line;
	DeletedPath(o, path, sizeof path, 0);
	text = (char*)Host_ReadNativeFile(o, path, &size);
	if(!text)
		return;
	for(line = text; *line;)
	{
		char* end = line;
		while(*end && *end != '\n' && *end != '\r')
			end++;
		if(*end)
			*end++ = 0;
		if(*line)
			MarkDeleted(o, line);
		line = end;
	}
	free(text);
}

// ---- the archive's entries -------------------------------------------------------------

// the archive's entry of a key, unless it is gone (none for the game directory and this layer's folder)
static const VolEntry_t* EntryOf(OpenBGI_t* o, const char* key)
{
	if(!o->files.fs.vol || !key[0] || IsOwn(key) || IsDeleted(o, key))
		return NULL;
	return Vol_Find(o->files.fs.vol, key);
}

// the engine path of an entry, as the archive spells it
static void EnginePathOf(OpenBGI_t* o, const VolEntry_t* e, char* out, size_t n)
{
	char rel[FS_PATH];
	size_t i;
	ToEngine(o, e->path, rel, sizeof rel);
	for(i = 0; rel[i]; i++) // no trail byte of Shift-JIS is '/'
		if(rel[i] == '/')
			rel[i] = '\\';
	snprintf(out, n, "%s%s", o->files.fs.exeDir, rel);
}

// a folder of the archive made in the game folder (1 when it is there)
static int MakeDir(OpenBGI_t* o, const VolEntry_t* e)
{
	char path[FS_PATH];
	EnginePathOf(o, e, path, sizeof path);
	return Host_FileAttrs(o, path) != OPENBGI_INVALID_ATTRS || Host_DirCreate(o, path);
}

// the folders of the archive on the way to `key` (not `key` itself) made in the game folder where they are missing
static void MakeParents(OpenBGI_t* o, const char* key)
{
	const char* p;
	for(p = strchr(key, '/'); p; p = strchr(p + 1, '/'))
	{
		char sub[FS_PATH];
		const VolEntry_t* e;
		size_t n = (size_t)(p - key);
		memcpy(sub, key, n);
		sub[n] = 0;
		e = EntryOf(o, sub);
		if(e && e->isDir)
			MakeDir(o, e);
	}
}

/* an entry's time stamp as a FILETIME: as the archive has it, else its
 * MS-DOS date and time, which are local time */
static void EntryTime(const VolEntry_t* e, uint32_t* lo, uint32_t* hi)
{
	uint64_t t = e->mtime;
	if(!t)
	{
		struct tm tm;
		time_t secs;
		memset(&tm, 0, sizeof tm);
		tm.tm_year = (int)((e->dosTime >> 25) & 0x7f) + 80;
		tm.tm_mon = (int)((e->dosTime >> 21) & 0x0f) - 1;
		tm.tm_mday = (int)((e->dosTime >> 16) & 0x1f);
		tm.tm_hour = (int)((e->dosTime >> 11) & 0x1f);
		tm.tm_min = (int)((e->dosTime >> 5) & 0x3f);
		tm.tm_sec = (int)(e->dosTime & 0x1f) * 2;
		tm.tm_isdst = -1;
		if(tm.tm_mon < 0)
			tm.tm_mon = 0;
		if(tm.tm_mday < 1)
			tm.tm_mday = 1;
		secs = mktime(&tm);
		t = secs == (time_t)-1 ? 0 : ((uint64_t)(int64_t)secs + 11644473600ull) * 10000000ull;
	}
	*lo = (uint32_t)t;
	*hi = (uint32_t)(t >> 32);
}

// where Vol_Extract writes an entry: a stream of the host's
typedef struct WriteTo
{
	OpenBGI_t* o;
	HostFile_t* f;
} WriteTo_t;

static int WriteOs(void* ctx, const uint8_t* data, size_t n)
{
	WriteTo_t* w = (WriteTo_t*)ctx;
	return Host_FileWrite(w->o, w->f, data, (uint32_t)n) == n;
}

/* an entry of the archive written to the engine path `to` (a folder is
 * made); its time stamp kept.  1 when written. */
static int CopyUp(OpenBGI_t* o, const VolEntry_t* e, const char* to)
{
	HostFile_t* f;
	WriteTo_t w;
	uint32_t lo, hi;
	int ok;
	if(e->isDir)
		return Host_FileAttrs(o, to) != OPENBGI_INVALID_ATTRS || Host_DirCreate(o, to);
	if(!e->readable || e->size > 0xffffffffu || !(f = Host_FileOpen(o, to, OPENBGI_OPEN_CREATE)))
		return 0;
	w.o = o;
	w.f = f;
	ok = Vol_Extract(o->files.fs.vol, e, WriteOs, &w);
	if(ok)
	{
		EntryTime(e, &lo, &hi);
		Host_FileSetTime(o, f, lo, hi);
	}
	Host_FileClose(o, f);
	if(!ok)
	{
		Host_FileDelete(o, to);
		fprintf(stderr, "openbgi: %s: %s cannot be read (damaged?)\n", Vol_Path(o->files.fs.vol), e->path);
	}
	return ok;
}

/* the engine path of an entry's extracted copy in FS_OWN\cache, made when
 * it is not there yet: named after its check value and size, with its
 * extension (ASCII letters and digits only) for the programs that go by
 * it.  `byName` says why it is made: for a program that opens the file
 * by its name, else because the entry is deflated and too large to hold
 * in memory.  1 when the copy is there. */
static int Cached(OpenBGI_t* o, const VolEntry_t* e, char* out, size_t n, int byName)
{
	char dir[FS_PATH], part[FS_PATH], ext[12] = "";
	const char* dot = strrchr(e->name, '.');
	HostFile_t* f;
	if(dot && strlen(dot) < sizeof ext)
	{
		size_t i;
		for(i = 1; dot[i]; i++)
			if(!((dot[i] >= 'a' && dot[i] <= 'z') || (dot[i] >= 'A' && dot[i] <= 'Z') || (dot[i] >= '0' && dot[i] <= '9')))
				break;
		if(!dot[i])
			snprintf(ext, sizeof ext, "%s", dot);
	}
	snprintf(dir, sizeof dir, "%s" FS_OWN, o->files.fs.exeDir);
	Host_DirCreate(o, dir);
	snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "\\cache");
	Host_DirCreate(o, dir);
	snprintf(out, n, "%s\\%08X-%08X%s", dir, e->check, (uint32_t)e->size, ext);
	f = Host_FileOpen(o, out, OPENBGI_OPEN_READ);
	if(f)
	{
		uint64_t size = Host_FileSize(o, f);
		Host_FileClose(o, f);
		if(size == e->size)
			return 1;
	}
	if(e->size >= (8u << 20) && byName)
		fprintf(stderr, "openbgi: extracting %s (%u MB) from %s for a program that opens it by its name\n", e->path, (unsigned)(e->size >> 20),
			Vol_Path(o->files.fs.vol));
	else if(e->size >= (8u << 20))
		fprintf(stderr, "openbgi: extracting %s (%u MB) from %s: it is compressed there; stored without compression it would be read in place\n",
			e->path, (unsigned)(e->size >> 20), Vol_Path(o->files.fs.vol));
	snprintf(part, sizeof part, "%s.part", out);
	if(!CopyUp(o, e, part))
		return 0;
	Host_FileDelete(o, out);
	return Host_FileMove(o, part, out);
}

// a handle on an entry of the archive (NULL for a folder, an entry that cannot be read, or one past 4 GB)
static FsFile_t* OpenEntry(OpenBGI_t* o, const VolEntry_t* e)
{
	FsFile_t* f;
	if(e->isDir || !e->readable || e->size > 0xffffffffu || !(f = (FsFile_t*)calloc(1, sizeof *f)))
		return NULL;
	f->o = o;
	f->size = (uint32_t)e->size;
	EntryTime(e, &f->timeLo, &f->timeHi);
	if(e->stored)
	{
		f->stored = Vol_OpenStored(o->files.fs.vol, e, &f->base);
		if(f->stored)
			return f;
	}
	else if(e->size <= FS_INFLATE_MAX)
	{
		size_t n;
		f->mem = Vol_Load(o->files.fs.vol, e, &n);
		if(f->mem)
			return f;
		fprintf(stderr, "openbgi: %s: %s cannot be read (damaged?)\n", Vol_Path(o->files.fs.vol), e->path);
	}
	else
	{
		char path[FS_PATH];
		if(Cached(o, e, path, sizeof path, 0) && (f->os = Host_FileOpen(o, path, OPENBGI_OPEN_READ)) != NULL)
			return f;
	}
	free(f);
	return NULL;
}

// whether a folder of the archive holds anything that is not gone
static int HasEntries(OpenBGI_t* o, const char* key)
{
	char prefix[FS_PATH];
	uint32_t i;
	size_t n;
	snprintf(prefix, sizeof prefix, "%s/", key);
	n = strlen(prefix);
	for(i = Vol_Lower(o->files.fs.vol, prefix); i < Vol_Count(o->files.fs.vol); i++)
	{
		const VolEntry_t* e = Vol_At(o->files.fs.vol, i);
		if(strncmp(e->key, prefix, n) != 0)
			break;
		if(!IsDeleted(o, e->key))
			return 1;
	}
	return 0;
}

/* the archive's part of a folder copied into the game folder: every entry
 * under `key` that is neither gone nor there already, at `path` (the
 * folder's engine path) */
static int CopyUpTree(OpenBGI_t* o, const char* key, const char* path)
{
	char prefix[FS_PATH];
	uint32_t i;
	size_t n;
	const VolEntry_t* top = EntryOf(o, key);
	if(top && !CopyUp(o, top, path))
		return 0;
	snprintf(prefix, sizeof prefix, "%s/", key);
	n = strlen(prefix);
	for(i = Vol_Lower(o->files.fs.vol, prefix); i < Vol_Count(o->files.fs.vol); i++)
	{
		const VolEntry_t* e = Vol_At(o->files.fs.vol, i);
		char rel[FS_PATH], to[FS_PATH];
		size_t k;
		if(strncmp(e->key, prefix, n) != 0)
			break;
		if(IsDeleted(o, e->key))
			continue;
		ToEngine(o, e->path + n, rel, sizeof rel); // the key and the path have the same length
		for(k = 0; rel[k]; k++)
			if(rel[k] == '/')
				rel[k] = '\\';
		snprintf(to, sizeof to, "%s\\%s", path, rel);
		if(Host_FileAttrs(o, to) == OPENBGI_INVALID_ATTRS && !CopyUp(o, e, to))
			return 0;
	}
	return 1;
}

// mark an entry and everything under it as gone (saving the list)
static void DeleteTree(OpenBGI_t* o, const char* key)
{
	char prefix[FS_PATH];
	uint32_t i;
	size_t n;
	int changed = 0;
	if(Vol_Find(o->files.fs.vol, key))
		changed |= MarkDeleted(o, key);
	snprintf(prefix, sizeof prefix, "%s/", key);
	n = strlen(prefix);
	for(i = Vol_Lower(o->files.fs.vol, prefix); i < Vol_Count(o->files.fs.vol); i++)
	{
		const VolEntry_t* e = Vol_At(o->files.fs.vol, i);
		if(strncmp(e->key, prefix, n) != 0)
			break;
		changed |= MarkDeleted(o, e->key);
	}
	if(changed)
		SaveDeleted(o);
}

// ---- files --------------------------------------------------------------------------------

static FsFile_t* Wrap(OpenBGI_t* o, HostFile_t* os)
{
	FsFile_t* f;
	if(!os)
		return NULL;
	f = (FsFile_t*)calloc(1, sizeof *f);
	if(!f)
	{
		Host_FileClose(o, os);
		return NULL;
	}
	f->o = o;
	f->os = os;
	return f;
}

/* Whether a path names the file the engine carries itself
 * (o->files.builtinName): that name in the game directory, by its full
 * path or as a name alone, the case of ASCII letters aside.  A file of
 * that name in the folder goes first, as with an archive's. */
static int IsBuiltin(OpenBGI_t* o, const char* path)
{
	char dir[FS_PATH];
	size_t n;
	if(!o->files.builtinName || !o->files.builtinData)
		return 0;
	Host_ExeDir(o, dir, sizeof dir);
	n = strlen(dir);
	if(n && strlen(path) > n && SamePrefix(path, dir, n))
		path += n;
	return strlen(path) == strlen(o->files.builtinName) && SamePrefix(path, o->files.builtinName, strlen(path));
}

// a handle on the built-in file: a copy of its bytes, read as a decompressed entry is
static FsFile_t* OpenBuiltin(OpenBGI_t* o)
{
	FsFile_t* f = (FsFile_t*)calloc(1, sizeof *f);
	if(!f || !(f->mem = (uint8_t*)malloc(o->files.builtinSize ? o->files.builtinSize : 1)))
	{
		free(f);
		return NULL;
	}
	memcpy(f->mem, o->files.builtinData, o->files.builtinSize);
	f->o = o;
	f->size = o->files.builtinSize;
	return f;
}

// a handle on the engine's own program file (file.h): a copy of its bytes, read as a decompressed entry is
static FsFile_t* OpenProgram(OpenBGI_t* o)
{
	FsFile_t* f = (FsFile_t*)calloc(1, sizeof *f);
	if(!f || !(f->mem = (uint8_t*)malloc(PROGRAM_IMAGE_SIZE)))
	{
		free(f);
		return NULL;
	}
	memcpy(f->mem, Program_Image(o), PROGRAM_IMAGE_SIZE);
	f->o = o;
	f->size = PROGRAM_IMAGE_SIZE;
	return f;
}

FsFile_t* Fs_FileOpenRead(OpenBGI_t* o, const char* path)
{
	HostFile_t* os;
	char key[FS_PATH];
	const VolEntry_t* e;
	if(Program_IsPath(o, path)) // before a file of that name in the folder: every game gets the same
		return OpenProgram(o);
	os = Host_FileOpen(o, path, OPENBGI_OPEN_READ);
	if(os)
		return Wrap(o, os);
	if(IsBuiltin(o, path) && Host_FileAttrs(o, path) == OPENBGI_INVALID_ATTRS)
		return OpenBuiltin(o);
	if(!KeyOf(o, path, key, sizeof key) || Host_FileAttrs(o, path) != OPENBGI_INVALID_ATTRS)
		return NULL;
	e = EntryOf(o, key);
	return e ? OpenEntry(o, e) : NULL;
}

FsFile_t* Fs_FileCreate(OpenBGI_t* o, const char* path)
{
	char key[FS_PATH];
	if(KeyOf(o, path, key, sizeof key) && Host_FileAttrs(o, path) == OPENBGI_INVALID_ATTRS)
	{
		const VolEntry_t* e = EntryOf(o, key);
		if(e && e->isDir)
			return NULL; // a folder of that name
		MakeParents(o, key);
		Undelete(o, key);
	}
	return Wrap(o, Host_FileOpen(o, path, OPENBGI_OPEN_CREATE));
}

FsFile_t* Fs_FileOpenWrite(OpenBGI_t* o, const char* path)
{
	char key[FS_PATH];
	if(KeyOf(o, path, key, sizeof key) && Host_FileAttrs(o, path) == OPENBGI_INVALID_ATTRS)
	{
		const VolEntry_t* e = EntryOf(o, key);
		if(e && e->isDir)
			return NULL;
		MakeParents(o, key);
		if(e && !CopyUp(o, e, path))
			return NULL;
		Undelete(o, key);
	}
	return Wrap(o, Host_FileOpen(o, path, OPENBGI_OPEN_WRITE));
}

void Fs_FileClose(FsFile_t* f)
{
	if(!f)
		return;
	Host_FileClose(f->o, f->os);
	if(f->stored)
		Host_FileClose(f->o, f->stored);
	free(f->mem);
	free(f);
}

uint32_t Fs_FileRead(FsFile_t* f, void* buf, uint32_t n)
{
	if(f->os)
		return Host_FileRead(f->o, f->os, buf, n);
	if(f->pos >= f->size)
		return 0;
	if(n > f->size - f->pos)
		n = f->size - f->pos;
	if(f->mem)
		memcpy(buf, f->mem + f->pos, n);
	else if(!Host_FileSeek(f->o, f->stored, f->base + f->pos) || (n = Host_FileRead(f->o, f->stored, buf, n)) == 0)
		return 0;
	f->pos += n;
	return n;
}

uint32_t Fs_FileWrite(FsFile_t* f, const void* buf, uint32_t n)
{
	return f->os ? Host_FileWrite(f->o, f->os, buf, n) : 0;
}

int Fs_FileSeek(FsFile_t* f, uint32_t pos)
{
	if(f->os)
		return Host_FileSeek(f->o, f->os, pos);
	f->pos = pos; // past the end as well, as SetFilePointer allows; reads there give nothing
	return 1;
}

uint32_t Fs_FileSize(FsFile_t* f)
{
	return f->os ? (uint32_t)Host_FileSize(f->o, f->os) : f->size;
}

int Fs_FileGetTime(FsFile_t* f, uint32_t* lo, uint32_t* hi)
{
	if(f->os)
		return Host_FileGetTime(f->o, f->os, lo, hi);
	*lo = f->timeLo;
	*hi = f->timeHi;
	return 1;
}

int Fs_FileSetTime(FsFile_t* f, uint32_t lo, uint32_t hi)
{
	return f->os ? Host_FileSetTime(f->o, f->os, lo, hi) : 0;
}

uint8_t* Fs_LoadFile(OpenBGI_t* o, const char* path, uint32_t* size)
{
	FsFile_t* f = Fs_FileOpenRead(o, path);
	uint8_t* buf;
	uint32_t n;
	*size = 0;
	if(!f)
		return NULL;
	n = Fs_FileSize(f);
	buf = (uint8_t*)malloc((size_t)n + 1);
	if(!buf || Fs_FileRead(f, buf, n) != n)
	{
		free(buf);
		Fs_FileClose(f);
		return NULL;
	}
	Fs_FileClose(f);
	buf[n] = 0;
	*size = n;
	return buf;
}

int Fs_LocalPath(OpenBGI_t* o, const char* path, char* out, size_t n)
{
	char key[FS_PATH];
	const VolEntry_t* e;
	snprintf(out, n, "%s", path);
	if(Host_FileAttrs(o, path) != OPENBGI_INVALID_ATTRS)
		return 1;
	if(!KeyOf(o, path, key, sizeof key) || !(e = EntryOf(o, key)))
		return 0;
	if(e->isDir)
	{
		MakeParents(o, key);
		return MakeDir(o, e);
	}
	if(e->file)
	{ // a folder's file is a file of its own already: by its native path, when the host takes that as a path of the game's
		char own[FS_PATH];
		if(Host_NativeSjis(o) || o->enc.textUtf8)
			snprintf(own, sizeof own, "%s", e->file);
		else
			Sjis_FromUtf8(e->file, own, sizeof own);
		if(Host_FileAttrs(o, own) != OPENBGI_INVALID_ATTRS)
		{
			snprintf(out, n, "%s", own);
			return 1;
		}
	}
	return Cached(o, e, out, n, 1);
}

// ---- files of the engine's own beside the game ------------------------------------------

// the native path of FS_OWN/<name> in the game folder (FS_OWN made when `make` is set); 0 without a game folder
static int OwnPath(OpenBGI_t* o, const char* name, char* out, size_t n, int make)
{
	size_t len;
	Host_GameDir(o, out, n);
	len = strlen(out);
	if(len == 0 || len + sizeof FS_OWN + strlen(name) + 2 > n)
		return 0;
	strcpy(out + len, FS_OWN);
	if(make)
		Host_NativeMkdir(o, out);
	sprintf(out + strlen(out), "%c%s", Host_NativeSep(o), name);
	return 1;
}

char* Fs_OwnRead(OpenBGI_t* o, const char* name)
{
	char path[FS_PATH];
	uint32_t size = 0;
	return OwnPath(o, name, path, sizeof path, 0) ? (char*)Host_ReadNativeFile(o, path, &size) : NULL;
}

int Fs_OwnWrite(OpenBGI_t* o, const char* name, const char* text)
{
	char path[FS_PATH];
	return OwnPath(o, name, path, sizeof path, 1) && Host_WriteNativeFile(o, path, text, (uint32_t)strlen(text));
}

// ---- names --------------------------------------------------------------------------------

uint32_t Fs_FileAttrs(OpenBGI_t* o, const char* path)
{
	uint32_t a = Host_FileAttrs(o, path);
	char key[FS_PATH];
	const VolEntry_t* e;
	if(Program_IsPath(o, path) || (a == OPENBGI_INVALID_ATTRS && IsBuiltin(o, path)))
		return OPENBGI_ATTR_ARCHIVE;
	if(a != OPENBGI_INVALID_ATTRS || !KeyOf(o, path, key, sizeof key) || !(e = EntryOf(o, key)))
		return a;
	return e->isDir ? OPENBGI_ATTR_DIRECTORY : OPENBGI_ATTR_ARCHIVE;
}

int Fs_FileSetAttrs(OpenBGI_t* o, const char* path, uint32_t attrs)
{
	char key[FS_PATH];
	const VolEntry_t* e;
	if(Host_FileAttrs(o, path) == OPENBGI_INVALID_ATTRS && KeyOf(o, path, key, sizeof key) && (e = EntryOf(o, key)) != NULL)
	{ // the archive's file or folder comes to the game folder first
		MakeParents(o, key);
		if(!CopyUp(o, e, path))
			return 0;
	}
	return Host_FileSetAttrs(o, path, attrs);
}

int Fs_FileDelete(OpenBGI_t* o, const char* path)
{
	char key[FS_PATH];
	const VolEntry_t* e;
	if(!KeyOf(o, path, key, sizeof key))
		return Host_FileDelete(o, path);
	e = EntryOf(o, key);
	if(e && e->isDir)
		return 0; // DeleteFile takes no folder
	if(Host_FileAttrs(o, path) != OPENBGI_INVALID_ATTRS)
	{
		if(!Host_FileDelete(o, path))
			return 0;
	}
	else if(!e)
		return 0;
	if(e && MarkDeleted(o, key))
		SaveDeleted(o);
	return 1;
}

int Fs_FileMove(OpenBGI_t* o, const char* from, const char* to)
{
	char kf[FS_PATH], kt[FS_PATH];
	int fromGame, toGame, ok;
	uint32_t a;
	if(!o->files.fs.vol)
		return Host_FileMove(o, from, to);
	fromGame = KeyOf(o, from, kf, sizeof kf);
	toGame = KeyOf(o, to, kt, sizeof kt);
	a = Fs_FileAttrs(o, from);
	if(a == OPENBGI_INVALID_ATTRS || Fs_FileAttrs(o, to) != OPENBGI_INVALID_ATTRS)
		return 0; // MoveFile wants the source and no destination
	if(toGame)
		MakeParents(o, kt);
	if(fromGame && (Host_FileAttrs(o, from) == OPENBGI_INVALID_ATTRS || ((a & OPENBGI_ATTR_DIRECTORY) && HasEntries(o, kf))))
	{
		const VolEntry_t* e = EntryOf(o, kf);
		if(e && !e->isDir && Host_FileAttrs(o, from) == OPENBGI_INVALID_ATTRS)
		{ // a file of the archive: written at the destination
			if(!CopyUp(o, e, to))
				return 0;
			DeleteTree(o, kf);
			if(toGame)
				Undelete(o, kt);
			return 1;
		}
		// a folder: what the archive has of it joins the game folder's, which then moves
		MakeParents(o, kf);
		if(!CopyUpTree(o, kf, from))
			return 0;
	}
	ok = Host_FileMove(o, from, to);
	if(ok && fromGame)
		DeleteTree(o, kf);
	if(ok && toGame)
		Undelete(o, kt);
	return ok;
}

int Fs_FileCopy(OpenBGI_t* o, const char* from, const char* to, int failIfExists)
{
	char kf[FS_PATH], kt[FS_PATH];
	const VolEntry_t* e;
	int ok, toGame;
	if(!o->files.fs.vol)
		return Host_FileCopy(o, from, to, failIfExists);
	if(failIfExists && Fs_FileAttrs(o, to) != OPENBGI_INVALID_ATTRS)
		return 0;
	toGame = KeyOf(o, to, kt, sizeof kt);
	if(toGame)
		MakeParents(o, kt);
	if(Host_FileAttrs(o, from) != OPENBGI_INVALID_ATTRS)
		ok = Host_FileCopy(o, from, to, failIfExists);
	else if(KeyOf(o, from, kf, sizeof kf) && (e = EntryOf(o, kf)) != NULL && !e->isDir)
		ok = CopyUp(o, e, to);
	else
		ok = 0;
	if(ok && toGame)
		Undelete(o, kt);
	return ok;
}

int Fs_DirCreate(OpenBGI_t* o, const char* path)
{
	char key[FS_PATH];
	int ok;
	if(!KeyOf(o, path, key, sizeof key))
		return Host_DirCreate(o, path);
	if(Fs_FileAttrs(o, path) != OPENBGI_INVALID_ATTRS)
		return 0; // there already
	MakeParents(o, key);
	ok = Host_DirCreate(o, path);
	if(ok)
		Undelete(o, key);
	return ok;
}

int Fs_DirRemove(OpenBGI_t* o, const char* path)
{
	char key[FS_PATH];
	const VolEntry_t* e;
	if(!KeyOf(o, path, key, sizeof key))
		return Host_DirRemove(o, path);
	e = EntryOf(o, key);
	if(e && (!e->isDir || HasEntries(o, key)))
		return 0; // not a folder, or not empty
	if(Host_FileAttrs(o, path) != OPENBGI_INVALID_ATTRS)
	{
		if(!Host_DirRemove(o, path))
			return 0;
	}
	else if(!e)
		return 0;
	if(e && MarkDeleted(o, key))
		SaveDeleted(o);
	return 1;
}

int Fs_SetCurrentDir(OpenBGI_t* o, const char* dir)
{
	char key[FS_PATH];
	int inGame = KeyOf(o, dir, key, sizeof key);
	if(inGame && key[0])
	{ // a folder only the archive has is made, to be the host's current directory
		const VolEntry_t* e = EntryOf(o, key);
		if(e && e->isDir)
		{
			MakeParents(o, key);
			MakeDir(o, e);
		}
	}
	if(!Host_SetCurrentDir(o, dir))
		return 0;
	if(o->files.fs.vol)
	{
		o->files.fs.cwdOutside = !inGame;
		if(inGame)
			snprintf(o->files.fs.cwd, sizeof o->files.fs.cwd, "%s", key);
	}
	return 1;
}

// ---- listings -------------------------------------------------------------------------------

static int FindAdd(FsFind_t* h, uint32_t attrs, const char* name)
{
	if(h->count == h->cap)
	{
		uint32_t cap = h->cap ? h->cap * 2 : 32;
		OpenBGI_FindData_t* items = (OpenBGI_FindData_t*)realloc(h->items, cap * sizeof *items);
		if(!items)
			return 0;
		h->items = items;
		h->cap = cap;
	}
	h->items[h->count].attrs = attrs;
	snprintf(h->items[h->count].name, sizeof h->items[h->count].name, "%s", name);
	h->count++;
	return 1;
}

// whether the game folder listed a name (UTF-8 in lower case)
static int FindHas(const FsFind_t* h, const char* folded)
{
	uint32_t i;
	for(i = 0; i < h->nFolded; i++)
		if(strcmp(h->folded[i], folded) == 0)
			return 1;
	return 0;
}

// the archive's entries of the folder `key` that match `pattern` (UTF-8) and are not listed yet
static void FindZip(OpenBGI_t* o, FsFind_t* h, const char* key, const char* pattern)
{
	char prefix[FS_PATH];
	uint32_t i;
	size_t n;
	if(key[0])
		snprintf(prefix, sizeof prefix, "%s/", key);
	else
		prefix[0] = 0;
	n = strlen(prefix);
	for(i = Vol_Lower(o->files.fs.vol, prefix); i < Vol_Count(o->files.fs.vol); i++)
	{
		const VolEntry_t* e = Vol_At(o->files.fs.vol, i);
		char name[sizeof h->items[0].name];
		if(strncmp(e->key, prefix, n) != 0)
			break;
		if(strchr(e->key + n, '/') || IsDeleted(o, e->key) || IsOwn(e->key))
			continue; // deeper down, gone, or this layer's
		if(!Sjis_WildcardMatch(pattern, e->name) || FindHas(h, e->key + n))
			continue;
		ToEngine(o, e->name, name, sizeof name);
		FindAdd(h, e->isDir ? OPENBGI_ATTR_DIRECTORY : OPENBGI_ATTR_ARCHIVE, name);
	}
	// a folder the game folder lacks still lists "." and "..", as FindFirstFile has them
	if(key[0] && !FindHas(h, "."))
	{
		if(Sjis_WildcardMatch(pattern, "."))
			FindAdd(h, OPENBGI_ATTR_DIRECTORY, ".");
		if(Sjis_WildcardMatch(pattern, ".."))
			FindAdd(h, OPENBGI_ATTR_DIRECTORY, "..");
	}
}

FsFind_t* Fs_FindFirst(OpenBGI_t* o, const char* pattern, OpenBGI_FindData_t* out)
{
	FsFind_t* h = (FsFind_t*)calloc(1, sizeof *h);
	OpenBGI_FindData_t d;
	HostFind_t* os;
	char dir[FS_PATH], key[FS_PATH], utfPattern[FS_PATH];
	const char* file = FilePart(o, pattern);
	size_t dirLen = (size_t)(file - pattern);
	int inGame;
	if(!h)
		return NULL;
	if(dirLen >= sizeof dir)
		dirLen = sizeof dir - 1;
	memcpy(dir, pattern, dirLen);
	dir[dirLen] = 0;
	inGame = KeyOf(o, dirLen ? dir : ".", key, sizeof key);
	os = Host_FindFirst(o, pattern, &d);
	if(os)
	{
		do
		{
			char utf[FS_PATH];
			char** folded;
			if(!inGame)
			{
				FindAdd(h, d.attrs, d.name);
				continue;
			}
			Sjis_ToUtf8(d.name, utf, sizeof utf, o->enc.textUtf8);
			Fold(utf);
			if(!key[0] && strcmp(utf, FS_OWN) == 0)
				continue; // this layer's folder is not the game's
			folded = (char**)realloc(h->folded, (h->nFolded + 1) * sizeof *folded);
			if(!folded || !FindAdd(h, d.attrs, d.name))
			{
				h->folded = folded ? folded : h->folded;
				continue;
			}
			h->folded = folded;
			h->folded[h->nFolded] = (char*)malloc(strlen(utf) + 1);
			if(h->folded[h->nFolded])
				strcpy(h->folded[h->nFolded++], utf);
		} while(Host_FindNext(o, os, &d));
		Host_FindClose(o, os);
	}
	if(inGame)
	{
		const VolEntry_t* e = EntryOf(o, key);
		if(!key[0] || (e && e->isDir))
		{
			Sjis_ToUtf8(file, utfPattern, sizeof utfPattern, o->enc.textUtf8);
			FindZip(o, h, key, utfPattern);
		}
	}
	while(h->nFolded)
		free(h->folded[--h->nFolded]);
	free(h->folded);
	h->folded = NULL;
	if(!h->count)
	{
		free(h->items);
		free(h);
		return NULL;
	}
	*out = h->items[0];
	h->next = 1;
	return h;
}

int Fs_FindNext(FsFind_t* h, OpenBGI_FindData_t* out)
{
	if(h->next >= h->count)
		return 0;
	*out = h->items[h->next++];
	return 1;
}

void Fs_FindClose(FsFind_t* h)
{
	if(!h)
		return;
	free(h->items);
	free(h);
}

// ---- mounting --------------------------------------------------------------------------------

int Fs_IsArchive(OpenBGI_t* o, const char* nativePath)
{
	HostFile_t* f = Host_NativeOpen(o, nativePath, OPENBGI_OPEN_READ);
	uint8_t sig[6];
	int is;
	if(!f)
		return 0;
	is = Host_FileRead(o, f, sig, 4) == 4 && sig[0] == 'P' && sig[1] == 'K' && ((sig[2] == 3 && sig[3] == 4) || (sig[2] == 5 && sig[3] == 6));
	if(!is) // a disc image: the primary volume descriptor at sector 16
		is = Host_FileSeek(o, f, 16 * 2048) && Host_FileRead(o, f, sig, 6) == 6 && sig[0] == 1 && memcmp(sig + 1, "CD001", 5) == 0;
	Host_FileClose(o, f);
	return is;
}

// a native directory and its parents made where missing; 1 when it is there
static int MakeNativeDirs(OpenBGI_t* o, const char* path)
{
	char buf[FS_PATH], sep = Host_NativeSep(o);
	int sjis = Host_NativeSjis(o);
	size_t i;
	snprintf(buf, sizeof buf, "%s", path);
	for(i = 0; buf[i]; i++)
	{
		if(sjis && Sjis_IsLead((uint8_t)buf[i]) && buf[i + 1])
			i++; // the trail byte is no separator
		else if(i > 0 && (buf[i] == '/' || buf[i] == sep))
		{
			char c = buf[i];
			buf[i] = 0;
			if(!(i == 2 && buf[1] == ':')) // not a drive
				Host_NativeMkdir(o, buf);
			buf[i] = c;
		}
	}
	return Host_NativeMkdir(o, buf);
}

int Fs_GameFolder(OpenBGI_t* o, const char* nativeZip, char* out, size_t n)
{
	char base[FS_PATH], name[FS_PATH], sep = Host_NativeSep(o);
	const char* env = getenv("BGI_DATA");
	size_t len;
	if(env && *env)
		snprintf(base, sizeof base, "%s", env);
	else
		Host_UserDataDir(o, base, sizeof base);
	len = strlen(base);
	if(!len)
		return 0;
	if(*Host_NativeFilePart(o, base) && len + 1 < sizeof base)
	{
		base[len++] = sep;
		base[len] = 0;
	}
	{ // the last part of the path: an archive's file name, or a folder's name (which may end in a separator)
		char path[FS_PATH];
		len = (size_t)snprintf(path, sizeof path, "%s", nativeZip);
		if(len >= sizeof path)
			return 0;
		while(len > 1 && !*Host_NativeFilePart(o, path))
			path[--len] = 0;
		snprintf(name, sizeof name, "%s", Host_NativeFilePart(o, path));
	}
	len = strlen(name);
	if(len > 4 && name[len - 4] == '.' &&
		(((name[len - 3] | 32) == 'z' && (name[len - 2] | 32) == 'i' && (name[len - 1] | 32) == 'p') ||
			((name[len - 3] | 32) == 'i' && (name[len - 2] | 32) == 's' && (name[len - 1] | 32) == 'o')))
		name[len - 4] = 0;
	if(!name[0])
		return 0;
	snprintf(out, n, "%sgames%c%s", base, sep, name);
	if(!MakeNativeDirs(o, out))
		return 0;
	len = strlen(out);
	if(len + 1 >= n)
		return 0;
	out[len] = sep;
	out[len + 1] = 0;
	return 1;
}

int Fs_MountArchive(OpenBGI_t* o, const char* nativeZip)
{
	Vol_t* z = Vol_Open(o, nativeZip);
	if(!z)
		return 0;
	Fs_Unmount(o);
	o->files.fs.vol = z;
	Host_ExeDir(o, o->files.fs.exeDir, sizeof o->files.fs.exeDir);
	Host_GameDir(o, o->files.fs.folder, sizeof o->files.fs.folder);
	o->files.fs.cwd[0] = 0;
	o->files.fs.cwdOutside = 0;
	LoadDeleted(o);
	return 1;
}

int Fs_MountPatch(OpenBGI_t* o, const char* nativePath)
{
	Vol_t* v = o->files.fs.vol ? Vol_OpenPlain(o, nativePath) : NULL;
	uint32_t i, n;
	int files = 0;
	if(!v)
		return -1;
	n = Vol_Count(v);
	for(i = 0; i < n; i++)
		files += !Vol_At(v, i)->isDir;
	if(!Vol_Overlay(o->files.fs.vol, v))
	{
		Vol_Close(v);
		return -1;
	}
	return files;
}

void Fs_Unmount(OpenBGI_t* o)
{
	uint32_t i;
	Vol_Close(o->files.fs.vol);
	for(i = 0; i < o->files.fs.nDeleted; i++)
		free(o->files.fs.deleted[i]);
	free(o->files.fs.deleted);
	memset(&o->files.fs, 0, sizeof o->files.fs);
}

int Fs_Mounted(OpenBGI_t* o)
{
	return o->files.fs.vol != NULL;
}

const char* Fs_MountedPath(OpenBGI_t* o)
{
	return o->files.fs.vol ? Vol_Path(o->files.fs.vol) : "";
}
