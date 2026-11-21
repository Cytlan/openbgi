/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_preview.c - what is done for the launcher beside its frames: the
 *                first pictures - a game run out of sight for a few
 *                seconds, with an engine of its own, for the picture of
 *                its title (OpenBGI_PreviewNext, openbgi.h) - and the
 *                reading of its places (OpenBGI_JobNext)
 *
 * The run has a host of its own, which is everything that sets it apart
 * from a game that is played:
 *
 *   * its clock is counted, not measured: the run takes as long as the
 *     machine needs for the ten seconds or so of the game's time;
 *   * it has no window, no sound output, no movies, no registry and no
 *     shell, and it paints nothing until its last frame
 *     (OpenBGI_SetDrawing);
 *   * what it writes stays in memory and is gone with it: the game's own
 *     paths are a drive "C:" that exists in memory alone, and a native
 *     file that is made is kept there too.  Native files are read as they
 *     are (the game's archives, the disc image), nothing is made, changed
 *     or removed on a disk;
 *   * its keys are held for it: Ctrl and a click every so often, which
 *     take a game past its brand calls, let go in time for the title to
 *     settle; a message box gets its default button;
 *   * its picture is taken when the title has settled (OsPreview_Run).
 *
 * OsPreview_Run is the run itself, on the caller's thread.  OsPreview_Poll
 * and OsPreview_Stop are the desktop hosts' use of it: one game at a
 * time on a thread beside the launcher's engine, the next when the one
 * before is done.  A second thread does the launcher's reading - a
 * folder's listing, a look at a game - a few pieces a turn, with an
 * engine that is made once and never started; no run is begun while
 * there is something to read.  The threads use the native files of the
 * OS layer and, the run's, the font files (never the window, the dialogs
 * or the game directory), so they are stopped before a game is started
 * for play: the back ends keep one text encoding and one game directory
 * for the program.
 */
#include "host/os.h"
#include "host/os_common.h"
#include "bgi/sjis.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(BGI_WASM)

int OsPreview_Run(const char* cmdLine, int want, const int* cancel, OsPreviewResult_t* out)
{
	BGI_UNUSED(cmdLine);
	BGI_UNUSED(want);
	BGI_UNUSED(cancel);
	memset(out, 0, sizeof *out);
	return 0;
}

void OsPreview_Poll(OpenBGI_t* o)
{
	BGI_UNUSED(o);
}

void OsPreview_Stop(void)
{
}

OpenBGI_Host_t* OsPreview_HostNew(void)
{
	return NULL;
}

void OsPreview_HostFree(OpenBGI_Host_t* host)
{
	BGI_UNUSED(host);
}

#else

#define PREVIEW_MENU    3           // so many things to click in the picture: the title's menu is there
#define PREVIEW_CHECK   100         // .. looked for this often,
#define PREVIEW_SEEK    15000       // .. for this long of the game's time at most; the keys are held until then
#define PREVIEW_CLICK   250         // a click this often
#define PREVIEW_EVERY   1000        // then a look at the picture this often
#define PREVIEW_SAME    3           // .. until so many in a row showed the same: the title has settled
#define PREVIEW_SOON    5000        // .. this long after the keys were let go a single one is enough
#define PREVIEW_MOVES   8000        // .. and this long after, the picture is taken as it is: a menu that moves
#define PREVIEW_LAST    13000       // .. or, where no menu was seen, this long after; then the run is given up
#define PREVIEW_BOXES   16          // message boxes before the run is given up
#define PREVIEW_FILE    (64u << 20) // the largest file kept in memory, bytes
#define PREVIEW_REFRESH 60          // the display's rate the frames are paced by

// ---- the files in memory ------------------------------------------------------------------

/* A file or a folder that exists in memory: one of the game's (key 'G',
 * then its path on the drive, folded) or a native one (key 'N', then the
 * path as it was given).  One that was removed stays as a mark, so that
 * the native file of the same name is not seen through it. */
typedef struct MemFile
{
	char* key;
	char* name; // the last part of the path, as it was spelt
	uint8_t* data;
	size_t size, cap;
	int isDir, gone, readOnly;
	uint32_t timeLo, timeHi;
} MemFile_t;

// an open file: one in memory, or a native one that is read
typedef struct Stream
{
	MemFile_t* m;
	size_t pos;
	FILE* fp;
	uint64_t size;
	int64_t mtime;
} Stream_t;

typedef struct Find
{
	OpenBGI_FindData_t* list;
	int count, next;
} Find_t;

typedef struct Preview
{
	OpenBGI_Host_t host; // (the first member: OsPreview_HostNew hands it out)
	OpenBGI_Files_t files;
	OpenBGI_FontFiles_t fonts;
	OpenBGI_FontRenderer_t renderer;
	const OpenBGI_FontRenderer_t* drawn; // the back end's renderer, when it draws the text itself
	int utf8;                            // the run's text is UTF-8 ("81 00"), else Shift-JIS
	uint32_t now;                        // the run's clock, ms
	OpenBGI_DateTime_t date;
	uint64_t fileTime; // the time of what is written, a FILETIME
	char gameDir[1024], userDir[1024], binDir[1024];
	MemFile_t** mem;
	int memCount, memCap;
	const int* cancel; // becomes 1 when what this host's engine does is wanted no more; NULL: never
} Preview_t;

/* The run, or the reading, is being ended: nothing more is read from the
 * disk for it - a drive that takes its time would keep the one who waits
 * for the end waiting for files nobody wants any more.  The engine finds
 * no file then, and what comes of that is thrown away. */
static int Cancelled(const Preview_t* p)
{
	return p->cancel && __atomic_load_n(p->cancel, __ATOMIC_ACQUIRE);
}

static MemFile_t* MemFind(Preview_t* p, const char* key)
{
	int i;
	for(i = 0; i < p->memCount; i++)
		if(strcmp(p->mem[i]->key, key) == 0)
			return p->mem[i];
	return NULL;
}

// the entry of `key`, made (or made again) as a file or a folder; NULL without memory
static MemFile_t* MemMake(Preview_t* p, const char* key, int isDir)
{
	MemFile_t* m = MemFind(p, key);
	if(!m)
	{
		const char* last = key + 1;
		const char* s;
		if(p->memCount == p->memCap)
		{
			int cap = p->memCap ? p->memCap * 2 : 64;
			MemFile_t** grown = (MemFile_t**)realloc(p->mem, (size_t)cap * sizeof *grown);
			if(!grown)
				return NULL;
			p->mem = grown;
			p->memCap = cap;
		}
		m = (MemFile_t*)calloc(1, sizeof *m);
		if(!m)
			return NULL;
		m->key = (char*)malloc(strlen(key) + 1);
		for(s = key + 1; *s; s++)
			if((*s == '/' || *s == '\\') && s[1])
				last = s + 1;
		m->name = (char*)malloc(strlen(last) + 1);
		if(!m->key || !m->name)
		{
			free(m->key);
			free(m->name);
			free(m);
			return NULL;
		}
		strcpy(m->key, key);
		strcpy(m->name, last);
		p->mem[p->memCount++] = m;
	}
	m->isDir = isDir;
	m->gone = 0;
	m->readOnly = 0;
	m->size = 0;
	m->timeLo = (uint32_t)(p->fileTime + (uint64_t)p->now * 10000u);
	m->timeHi = (uint32_t)((p->fileTime + (uint64_t)p->now * 10000u) >> 32);
	return m;
}

static void MemFree(Preview_t* p)
{
	int i;
	for(i = 0; i < p->memCount; i++)
	{
		free(p->mem[i]->key);
		free(p->mem[i]->name);
		free(p->mem[i]->data);
		free(p->mem[i]);
	}
	free(p->mem);
	p->mem = NULL;
	p->memCount = p->memCap = 0;
}

/* The key of one of the game's paths: 'G', the drive ("C:" where the path
 * names none: the game's directory is its root) and the parts of the path
 * with a backslash before each, the ASCII letters as capitals; "." and
 * ".." are taken out.  `name`, when asked for, gets the last part as it
 * was spelt.  0 when it does not fit. */
static int GameKey(const char* path, char* out, size_t n, char* name, size_t nameSize)
{
	size_t len = 3, i;
	const char* s = path;
	if(n < 8)
		return 0;
	out[0] = 'G';
	out[1] = 'C';
	out[2] = ':';
	if(s[0] && s[1] == ':')
	{
		out[1] = (char)(s[0] >= 'a' && s[0] <= 'z' ? s[0] - 32 : s[0]);
		s += 2;
	}
	if(name && nameSize)
		name[0] = 0;
	while(*s)
	{
		const char* part;
		size_t k;
		while(*s == '\\' || *s == '/')
			s++;
		part = s;
		while(*s && *s != '\\' && *s != '/')
			s++;
		k = (size_t)(s - part);
		if(!k || (k == 1 && part[0] == '.'))
			continue;
		if(k == 2 && part[0] == '.' && part[1] == '.')
		{ // up: the last part goes
			while(len > 3 && out[len - 1] != '\\')
				len--;
			if(len > 3)
				len--;
			continue;
		}
		if(len + 1 + k + 1 > n)
			return 0;
		out[len++] = '\\';
		for(i = 0; i < k; i++)
			out[len++] = (char)(part[i] >= 'a' && part[i] <= 'z' ? part[i] - 32 : part[i]);
		if(name && nameSize)
			snprintf(name, nameSize, "%.*s", (int)k, part);
	}
	out[len] = 0;
	return 1;
}

// the key of a native path: 'N' and the path without the separators at its end
static int NativeKey(const char* path, char* out, size_t n)
{
	size_t len = strlen(path);
	while(len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\'))
		len--;
	if(len + 2 > n)
		return 0;
	out[0] = 'N';
	memcpy(out + 1, path, len);
	out[len + 1] = 0;
	return 1;
}

// 1 when the folder that holds the game's `key` is there (the drive's root always is)
static int ParentThere(Preview_t* p, const char* key)
{
	char parent[1100];
	char* cut;
	MemFile_t* m;
	snprintf(parent, sizeof parent, "%s", key);
	cut = strrchr(parent, '\\');
	if(!cut)
		return 0; // (the root itself)
	*cut = 0;
	if(!strchr(parent, '\\'))
		return 1;
	m = MemFind(p, parent);
	return m && !m->gone && m->isDir;
}

// `key` is right below the folder `dir`: where its name starts, else NULL
static const char* ChildOf(const char* key, const char* dir, char sep1, char sep2)
{
	size_t n = strlen(dir);
	const char* rest;
	if(strncmp(key, dir, n) != 0)
		return NULL;
	rest = key + n;
	if(n > 1 && (dir[n - 1] == sep1 || dir[n - 1] == sep2))
		; // (a root that ends in its separator)
	else if(*rest == sep1 || *rest == sep2)
		rest++;
	else
		return NULL;
	if(!*rest || strchr(rest, sep1) || strchr(rest, sep2))
		return NULL;
	return rest;
}

// ---- the streams ----------------------------------------------------------------------------

static Stream_t* StreamOf(MemFile_t* m)
{
	Stream_t* s = (Stream_t*)calloc(1, sizeof *s);
	if(s)
		s->m = m;
	return s;
}

static void* FilesOpen(void* user, const char* path, int mode)
{
	Preview_t* p = (Preview_t*)user;
	char key[1100];
	MemFile_t* m;
	if(!GameKey(path, key, sizeof key, NULL, 0))
		return NULL;
	m = MemFind(p, key);
	if(m && !m->gone && m->isDir)
		return NULL;
	if(mode == OPENBGI_OPEN_READ)
		return m && !m->gone ? StreamOf(m) : NULL;
	if(m && !m->gone)
	{ // an existing one: emptied, or kept
		if(m->readOnly)
			return NULL;
		if(mode == OPENBGI_OPEN_CREATE)
			m->size = 0;
		return StreamOf(m);
	}
	if(!ParentThere(p, key))
		return NULL;
	{ // a new one, under the name as it was spelt
		char name[300];
		GameKey(path, key, sizeof key, name, sizeof name);
		m = MemMake(p, key, 0);
		if(m && name[0] && strlen(name) <= strlen(m->name))
			strcpy(m->name, name);
	}
	return m ? StreamOf(m) : NULL;
}

static void FilesClose(void* user, void* f)
{
	Stream_t* s = (Stream_t*)f;
	BGI_UNUSED(user);
	if(!s)
		return;
	if(s->fp)
		fclose(s->fp);
	free(s);
}

static uint32_t FilesRead(void* user, void* f, void* buf, uint32_t n)
{
	Stream_t* s = (Stream_t*)f;
	if(s->fp)
		return Cancelled((const Preview_t*)user) ? 0 : (uint32_t)fread(buf, 1, n, s->fp);
	if(s->pos >= s->m->size)
		return 0;
	if(n > s->m->size - s->pos)
		n = (uint32_t)(s->m->size - s->pos);
	memcpy(buf, s->m->data + s->pos, n);
	s->pos += n;
	return n;
}

static uint32_t FilesWrite(void* user, void* f, const void* buf, uint32_t n)
{
	Preview_t* p = (Preview_t*)user;
	Stream_t* s = (Stream_t*)f;
	MemFile_t* m = s->m;
	uint64_t t;
	if(s->fp || !n)
		return 0; // (a native file is only read)
	if(s->pos > PREVIEW_FILE || n > PREVIEW_FILE - s->pos)
		return 0; // (as a full disk)
	if(s->pos + n > m->cap)
	{
		size_t cap = m->cap ? m->cap : 256;
		uint8_t* grown;
		while(cap < s->pos + n)
			cap *= 2;
		grown = (uint8_t*)realloc(m->data, cap);
		if(!grown)
			return 0;
		m->data = grown;
		m->cap = cap;
	}
	if(s->pos > m->size)
		memset(m->data + m->size, 0, s->pos - m->size); // (written past the end: nothing in between)
	memcpy(m->data + s->pos, buf, n);
	s->pos += n;
	if(s->pos > m->size)
		m->size = s->pos;
	t = p->fileTime + (uint64_t)p->now * 10000u;
	m->timeLo = (uint32_t)t;
	m->timeHi = (uint32_t)(t >> 32);
	return n;
}

static int FilesSeek(void* user, void* f, uint64_t pos)
{
	Stream_t* s = (Stream_t*)f;
	BGI_UNUSED(user);
	if(s->fp)
		return OS_NativeSeek(s->fp, pos);
	if(pos > PREVIEW_FILE)
		return 0;
	s->pos = (size_t)pos;
	return 1;
}

static uint64_t FilesLength(void* user, void* f)
{
	Stream_t* s = (Stream_t*)f;
	BGI_UNUSED(user);
	return s->fp ? s->size : s->m->size;
}

static int FilesGetTime(void* user, void* f, uint32_t* lo, uint32_t* hi)
{
	Stream_t* s = (Stream_t*)f;
	BGI_UNUSED(user);
	if(s->fp)
	{ // milliseconds since 1970 as a FILETIME
		uint64_t t = (uint64_t)(s->mtime + 11644473600000LL) * 10000u;
		*lo = (uint32_t)t;
		*hi = (uint32_t)(t >> 32);
	}
	else
	{
		*lo = s->m->timeLo;
		*hi = s->m->timeHi;
	}
	return 1;
}

static int FilesSetTime(void* user, void* f, uint32_t lo, uint32_t hi)
{
	Stream_t* s = (Stream_t*)f;
	BGI_UNUSED(user);
	if(s->fp)
		return 0;
	s->m->timeLo = lo;
	s->m->timeHi = hi;
	return 1;
}

// ---- the game's paths -------------------------------------------------------------------------

// the game's file or folder of that path; NULL when there is none
static MemFile_t* GameEntry(Preview_t* p, const char* path)
{
	char key[1100];
	MemFile_t* m;
	if(!GameKey(path, key, sizeof key, NULL, 0))
		return NULL;
	m = MemFind(p, key);
	return m && !m->gone ? m : NULL;
}

// 1 for a path that is a drive's root
static int IsRoot(const char* path)
{
	char key[1100];
	return GameKey(path, key, sizeof key, NULL, 0) && !strchr(key, '\\');
}

static uint32_t FilesAttrs(void* user, const char* path)
{
	Preview_t* p = (Preview_t*)user;
	MemFile_t* m = GameEntry(p, path);
	if(IsRoot(path))
		return OPENBGI_ATTR_DIRECTORY;
	if(!m)
		return OPENBGI_INVALID_ATTRS;
	if(m->isDir)
		return OPENBGI_ATTR_DIRECTORY;
	return OPENBGI_ATTR_ARCHIVE | (m->readOnly ? OPENBGI_ATTR_READONLY : 0);
}

static int FilesSetAttrs(void* user, const char* path, uint32_t attrs)
{
	MemFile_t* m = GameEntry((Preview_t*)user, path);
	if(!m)
		return 0;
	m->readOnly = !m->isDir && (attrs & OPENBGI_ATTR_READONLY) != 0;
	return 1;
}

static int FilesRemove(void* user, const char* path)
{
	MemFile_t* m = GameEntry((Preview_t*)user, path);
	if(!m || m->isDir || m->readOnly)
		return 0;
	m->gone = 1;
	return 1;
}

static int FilesCopy(void* user, const char* from, const char* to, int failIfExists)
{
	Preview_t* p = (Preview_t*)user;
	MemFile_t* src = GameEntry(p, from);
	MemFile_t* dst = GameEntry(p, to);
	Stream_t* out;
	uint32_t n;
	if(!src || src->isDir || src == dst || (dst && (failIfExists || dst->isDir)))
		return 0;
	out = (Stream_t*)FilesOpen(user, to, OPENBGI_OPEN_CREATE);
	if(!out)
		return 0;
	n = src->size ? FilesWrite(user, out, src->data, (uint32_t)src->size) : 0;
	out->m->timeLo = src->timeLo;
	out->m->timeHi = src->timeHi;
	FilesClose(user, out);
	return n == src->size;
}

/* MoveFile: a file or a folder under another name, which must be free; a
 * folder takes what is in it along. */
static int FilesMove(void* user, const char* from, const char* to)
{
	Preview_t* p = (Preview_t*)user;
	char a[1100], b[1100], name[300];
	MemFile_t* src = GameEntry(p, from);
	size_t alen;
	int i;
	if(!src || !GameKey(from, a, sizeof a, NULL, 0) || !GameKey(to, b, sizeof b, name, sizeof name) || !strchr(b, '\\'))
		return 0;
	if(strcmp(a, b) == 0)
		return 1;
	if(GameEntry(p, to) || !ParentThere(p, b))
		return 0;
	alen = strlen(a);
	if(src->isDir && strncmp(b, a, alen) == 0 && b[alen] == '\\')
		return 0; // (into itself)
	for(i = 0; i < p->memCount; i++)
	{
		MemFile_t* m = p->mem[i];
		char* key;
		if(strncmp(m->key, a, alen) != 0 || (m->key[alen] && m->key[alen] != '\\') || (m != src && (!src->isDir || m->gone)))
			continue;
		key = (char*)malloc(strlen(b) + strlen(m->key + alen) + 1);
		if(!key)
			return 0;
		strcpy(key, b);
		strcat(key, m->key + alen);
		{ // (a mark that was left under the new name gives way)
			MemFile_t* old = MemFind(p, key);
			if(old && old != m)
				old->key[0] = 'X';
		}
		free(m->key);
		m->key = key;
		if(m == src && name[0])
		{
			char* renamed = (char*)malloc(strlen(name) + 1);
			if(renamed)
			{
				strcpy(renamed, name);
				free(m->name);
				m->name = renamed;
			}
		}
	}
	return 1;
}

static int FilesMakeDir(void* user, const char* path)
{
	Preview_t* p = (Preview_t*)user;
	char key[1100], name[300];
	MemFile_t* m;
	if(!GameKey(path, key, sizeof key, name, sizeof name) || !strchr(key, '\\') || GameEntry(p, path) || !ParentThere(p, key))
		return 0;
	m = MemMake(p, key, 1);
	if(m && name[0] && strlen(name) <= strlen(m->name))
		strcpy(m->name, name);
	return m != NULL;
}

static int FilesRemoveDir(void* user, const char* path)
{
	Preview_t* p = (Preview_t*)user;
	MemFile_t* m = GameEntry(p, path);
	int i;
	if(!m || !m->isDir)
		return 0;
	for(i = 0; i < p->memCount; i++)
		if(!p->mem[i]->gone && ChildOf(p->mem[i]->key, m->key, '\\', '\\'))
			return 0; // not empty
	m->gone = 1;
	return 1;
}

// a name against a pattern with '*' and '?', the ASCII letters alike in both cases
static int Wild(const char* pat, const char* s)
{
	for(; *pat; pat++, s++)
	{
		if(*pat == '*')
		{
			while(pat[1] == '*')
				pat++;
			do
			{
				if(Wild(pat + 1, s))
					return 1;
			} while(*s++);
			return 0;
		}
		if(!*s)
			return 0;
		if(*pat != '?')
		{
			char a = *pat, b = *s;
			if(a >= 'a' && a <= 'z')
				a = (char)(a - 32);
			if(b >= 'a' && b <= 'z')
				b = (char)(b - 32);
			if(a != b)
				return 0;
		}
	}
	return !*s;
}

static int FilesFindNext(void* user, void* find, OpenBGI_FindData_t* out)
{
	Find_t* f = (Find_t*)find;
	BGI_UNUSED(user);
	if(!f || f->next >= f->count)
		return 0;
	*out = f->list[f->next++];
	return 1;
}

static void FilesFindClose(void* user, void* find)
{
	Find_t* f = (Find_t*)find;
	BGI_UNUSED(user);
	if(!f)
		return;
	free(f->list);
	free(f);
}

// the entries of the pattern's folder whose names match its last part ("*.*" is every name)
static void* FilesFindFirst(void* user, const char* pattern, OpenBGI_FindData_t* out)
{
	Preview_t* p = (Preview_t*)user;
	char key[1100], pat[300];
	char* cut;
	Find_t* f;
	int i;
	if(!GameKey(pattern, key, sizeof key, pat, sizeof pat) || (cut = strrchr(key, '\\')) == NULL)
		return NULL;
	*cut = 0; // the folder's key
	if(strcmp(pat, "*.*") == 0)
		strcpy(pat, "*");
	f = (Find_t*)calloc(1, sizeof *f);
	if(!f)
		return NULL;
	f->list = (OpenBGI_FindData_t*)calloc((size_t)p->memCount + 1, sizeof *f->list);
	for(i = 0; f->list && i < p->memCount; i++)
	{
		MemFile_t* m = p->mem[i];
		if(m->gone || m->key[0] != 'G' || !ChildOf(m->key, key, '\\', '\\') || !Wild(pat, m->name))
			continue;
		f->list[f->count].attrs = m->isDir ? OPENBGI_ATTR_DIRECTORY : OPENBGI_ATTR_ARCHIVE | (m->readOnly ? OPENBGI_ATTR_READONLY : 0);
		snprintf(f->list[f->count].name, sizeof f->list[f->count].name, "%s", m->name);
		f->count++;
	}
	if(!FilesFindNext(user, f, out))
	{
		FilesFindClose(user, f);
		return NULL;
	}
	return f;
}

static int FilesSetCurrentDir(void* user, const char* dir)
{
	BGI_UNUSED(user);
	BGI_UNUSED(dir);
	return 1; // (paths without a drive are the game directory's, wherever the game says it is)
}

static void FilesExeDir(void* user, char* buf, size_t n)
{
	BGI_UNUSED(user);
	snprintf(buf, n, "C:\\");
}

static uint32_t FilesDriveType(void* user, const char* root)
{
	BGI_UNUSED(user);
	return root && (root[0] | 32) == 'c' ? OPENBGI_DRIVE_FIXED : OPENBGI_DRIVE_NO_ROOT;
}

static uint32_t FilesLogicalDrives(void* user, char* buf, uint32_t n)
{
	static const char list[] = "C:\\\0";
	BGI_UNUSED(user);
	if(n < sizeof list + 1)
		return 0;
	memcpy(buf, list, sizeof list);
	buf[sizeof list] = 0;
	return sizeof list - 1;
}

static int FilesVolumeReady(void* user, const char* root)
{
	BGI_UNUSED(user);
	return root && (root[0] | 32) == 'c';
}

static int FilesMediaPresent(void* user, char drive)
{
	BGI_UNUSED(user);
	return (drive | 32) == 'c';
}

static int FilesSpecialFolder(void* user, int csidl, char* buf, size_t n)
{
	BGI_UNUSED(user);
	BGI_UNUSED(csidl);
	snprintf(buf, n, "C:\\Users\\user");
	return 1;
}

static int FilesWindowsDir(void* user, char* buf, size_t n)
{
	BGI_UNUSED(user);
	snprintf(buf, n, "C:\\Windows");
	return 1;
}

// ---- native paths -----------------------------------------------------------------------------

// the native file or folder that is in memory under that path (a mark of a removed one too); NULL: none
static MemFile_t* NativeEntry(Preview_t* p, const char* path)
{
	char key[1100];
	return NativeKey(path, key, sizeof key) ? MemFind(p, key) : NULL;
}

static void* FilesNativeOpen(void* user, const char* path, int mode)
{
	Preview_t* p = (Preview_t*)user;
	MemFile_t* m = NativeEntry(p, path);
	char key[1100];
	if(mode == OPENBGI_OPEN_CREATE)
	{ // made in memory, whatever is on the disk
		if((m && !m->gone && m->isDir) || !NativeKey(path, key, sizeof key))
			return NULL;
		m = MemMake(p, key, 0);
		return m ? StreamOf(m) : NULL;
	}
	if(mode != OPENBGI_OPEN_READ)
		return NULL;
	if(m)
		return !m->gone && !m->isDir ? StreamOf(m) : NULL;
	if(Cancelled(p))
		return NULL;
	{
		OsNativeStat_t st;
		Stream_t* s;
		if(OS_NativeStat(path, &st) != OS_NATIVE_FILE || (s = (Stream_t*)calloc(1, sizeof *s)) == NULL)
			return NULL;
		s->fp = OS_NativeOpen(path, "rb");
		s->size = st.size;
		s->mtime = st.mtime;
		if(!s->fp)
		{
			free(s);
			return NULL;
		}
		return s;
	}
}

static int FilesNativeStat(void* user, const char* path, OpenBGI_NativeStat_t* out)
{
	Preview_t* p = (Preview_t*)user;
	MemFile_t* m = NativeEntry(p, path);
	if(!m)
		return Cancelled(p) ? OPENBGI_NATIVE_NONE : OS_NativeStat(path, out);
	if(m->gone)
		return OPENBGI_NATIVE_NONE;
	if(out)
	{ // (the FILETIME as milliseconds since 1970)
		out->size = m->size;
		out->mtime = (int64_t)((((uint64_t)m->timeHi << 32) | m->timeLo) / 10000u) - 11644473600000LL;
	}
	return m->isDir ? OPENBGI_NATIVE_DIR : OPENBGI_NATIVE_FILE;
}

// (a file of the run's own can be written to; one on the disk as the disk has it, though the run writes none)
static int FilesNativeWritable(void* user, const char* path)
{
	Preview_t* p = (Preview_t*)user;
	MemFile_t* m = NativeEntry(p, path);
	if(m)
		return !m->gone && !m->isDir && !m->readOnly;
	return !Cancelled(p) && OS_NativeWritable(path);
}

static int FilesNativeMkdir(void* user, const char* path)
{
	Preview_t* p = (Preview_t*)user;
	MemFile_t* m = NativeEntry(p, path);
	char key[1100];
	if(m && !m->gone)
		return m->isDir;
	if(!m)
	{
		int kind = OS_NativeStat(path, NULL);
		if(kind != OS_NATIVE_NONE)
			return kind == OS_NATIVE_DIR;
	}
	return NativeKey(path, key, sizeof key) && MemMake(p, key, 1) != NULL;
}

static int FilesNativeRemove(void* user, const char* path)
{
	Preview_t* p = (Preview_t*)user;
	MemFile_t* m = NativeEntry(p, path);
	char key[1100];
	if(m)
	{
		if(!m->gone && m->isDir)
			return 0;
		m->gone = 1;
		return 1;
	}
	if(OS_NativeStat(path, NULL) == OS_NATIVE_FILE)
	{ // one on the disk: it stays there, and this run sees it no more
		if(!NativeKey(path, key, sizeof key) || (m = MemMake(p, key, 0)) == NULL)
			return 0;
		m->gone = 1;
	}
	return 1;
}

typedef struct ListCtx
{
	Preview_t* p;
	const char* dir;
	void (*fn)(void* ctx, const char* name, int isDir);
	void* ctx;
	int count;
} ListCtx_t;

// an entry of the disk's folder, unless memory has one of that name (a file made over it, or a mark)
static void ListReal(void* ctx, const char* name, int isDir)
{
	ListCtx_t* l = (ListCtx_t*)ctx;
	char path[1100];
	size_t n = strlen(l->dir);
	snprintf(path, sizeof path, "%s%s%s", l->dir, n && (l->dir[n - 1] == '/' || l->dir[n - 1] == '\\') ? "" : "/", name);
	if(NativeEntry(l->p, path))
		return;
	if(l->p->files.nativeSep == '\\')
	{ // (the same, joined as the engine joins)
		snprintf(path, sizeof path, "%s%s%s", l->dir, n && (l->dir[n - 1] == '/' || l->dir[n - 1] == '\\') ? "" : "\\", name);
		if(NativeEntry(l->p, path))
			return;
	}
	l->fn(l->ctx, name, isDir);
	l->count++;
}

static int FilesNativeList(void* user, const char* dir, void (*fn)(void* ctx, const char* name, int isDir), void* ctx)
{
	Preview_t* p = (Preview_t*)user;
	ListCtx_t l = {p, dir, fn, ctx, 0};
	MemFile_t* m = NativeEntry(p, dir);
	char key[1100];
	int i, real = -1;
	if(m && (m->gone || !m->isDir))
		return -1;
	if(!NativeKey(dir, key, sizeof key))
		return -1;
	if(!Cancelled(p) && OS_NativeStat(dir, NULL) == OS_NATIVE_DIR)
		real = OS_NativeList(dir, ListReal, &l);
	if(real < 0 && !m)
		return -1;
	for(i = 0; i < p->memCount; i++)
	{
		MemFile_t* e = p->mem[i];
		if(e->gone || e->key[0] != 'N' || !ChildOf(e->key, key, '/', '\\'))
			continue;
		fn(ctx, e->name, e->isDir);
		l.count++;
	}
	return l.count;
}

static void FilesNativeAbsolute(void* user, const char* path, char* out, size_t n)
{
	BGI_UNUSED(user);
	OS_NativeAbsolute(path, out, n);
}

// native text is UTF-8 while the launcher runs, which is when this host exists
static void FilesSameText(void* user, const char* in, char* out, size_t n)
{
	BGI_UNUSED(user);
	snprintf(out, n, "%s", in);
}

static void FilesGameDir(void* user, char* buf, size_t n)
{
	snprintf(buf, n, "%s", ((Preview_t*)user)->gameDir);
}

// the game directory is a name: the game's paths are in memory wherever it says they are
static int FilesSetGameDir(void* user, const char* dir)
{
	Preview_t* p = (Preview_t*)user;
	size_t len = strlen(dir);
	if(!len || len + 2 > sizeof p->gameDir || FilesNativeStat(user, dir, NULL) != OPENBGI_NATIVE_DIR)
		return 0;
	memcpy(p->gameDir, dir, len + 1);
	if(dir[len - 1] != '/' && dir[len - 1] != '\\')
	{
		p->gameDir[len] = (char)p->files.nativeSep;
		p->gameDir[len + 1] = 0;
	}
	return 1;
}

static void FilesUserDataDir(void* user, char* buf, size_t n)
{
	snprintf(buf, n, "%s", ((Preview_t*)user)->userDir);
}

static void FilesBinaryDir(void* user, char* buf, size_t n)
{
	snprintf(buf, n, "%s", ((Preview_t*)user)->binDir);
}

// ---- the run's host ------------------------------------------------------------------------

static uint32_t HostTicksMs(void* user)
{
	return ((Preview_t*)user)->now;
}

static int HostPerfNs(void* user, int64_t* out)
{
	*out = (int64_t)((Preview_t*)user)->now * 1000000;
	return 1;
}

static void HostLocalTime(void* user, OpenBGI_DateTime_t* out)
{
	*out = ((Preview_t*)user)->date;
}

// ---- the text of a back end that draws it itself -------------------------------------

/* A back end with a renderer of its own (Windows: GDI) takes the faces'
 * names and the characters in the one text encoding it was told, which
 * is the launcher's UTF-8 while a run is made.  The run's game has its
 * own - Shift-JIS until its scripts say otherwise - so its text goes on
 * as UTF-8.  The fonts built into the program are the back end's already
 * (the launcher's engine handed them over), and a font file of the game
 * stays where it is, in an archive perhaps: the face that stands in for
 * it is drawn. */
static void HostTextEncoding(void* user, int utf8)
{
	((Preview_t*)user)->utf8 = utf8;
}

// the run's string of `len` bytes (-1: to its NUL) as the back end takes it; the bytes of that
static const char* DrawnText(Preview_t* p, const char* s, int* len, char* buf, size_t n)
{
	char own[1024];
	size_t k = *len < 0 ? strlen(s) : (size_t)*len;
	if(p->utf8 || k >= sizeof own)
	{
		*len = (int)k;
		return s;
	}
	memcpy(own, s, k);
	own[k] = 0;
	*len = (int)Sjis_ToUtf8(own, buf, n, 0);
	return buf;
}

static void* DrawnOpen(void* user, const char* face, int height, int width, int bold, int italic)
{
	Preview_t* p = (Preview_t*)user;
	char buf[1024];
	int len = -1;
	return p->drawn->open(NULL, DrawnText(p, face, &len, buf, sizeof buf), height, width, bold, italic);
}

static void* DrawnOpenMono(void* user, const char* face, int size, int bold)
{
	Preview_t* p = (Preview_t*)user;
	char buf[1024];
	int len = -1;
	return p->drawn->openMono(NULL, DrawnText(p, face, &len, buf, sizeof buf), size, bold);
}

static void DrawnClose(void* user, void* font)
{
	((Preview_t*)user)->drawn->close(NULL, font);
}

static void DrawnRender(void* user, void* font, const char* ch, int len, uint8_t* out, int pitch, int w, int h)
{
	Preview_t* p = (Preview_t*)user;
	char buf[16];
	const char* text = DrawnText(p, ch, &len, buf, sizeof buf);
	p->drawn->render(NULL, font, text, len, out, pitch, w, h);
}

static void DrawnRenderMono(void* user, void* font, const char* ch, int len, uint8_t* bits, int pitch, int w, int h)
{
	Preview_t* p = (Preview_t*)user;
	char buf[16];
	const char* text = DrawnText(p, ch, &len, buf, sizeof buf);
	p->drawn->renderMono(NULL, font, text, len, bits, pitch, w, h);
}

static int DrawnAdvance(void* user, void* font, const char* ch, int len)
{
	Preview_t* p = (Preview_t*)user;
	char buf[16];
	const char* text = DrawnText(p, ch, &len, buf, sizeof buf);
	return p->drawn->advance(NULL, font, text, len);
}

static int DrawnAddFile(void* user, const char* path)
{
	BGI_UNUSED(user);
	BGI_UNUSED(path);
	return 0;
}

static int DrawnAddMemory(void* user, const void* data, uint32_t size)
{
	return ((Preview_t*)user)->drawn->addMemory(NULL, data, size);
}

static int DrawnAddSubstitute(void* user, const void* data, uint32_t size)
{
	BGI_UNUSED(user);
	BGI_UNUSED(data);
	BGI_UNUSED(size);
	return 1;
}

static int DrawnFaceExists(void* user, const char* face)
{
	Preview_t* p = (Preview_t*)user;
	char buf[1024];
	int len = -1;
	return p->drawn->faceExists(NULL, DrawnText(p, face, &len, buf, sizeof buf));
}

static int DrawnFaceInstalled(void* user, const char* face)
{
	Preview_t* p = (Preview_t*)user;
	char buf[1024];
	int len = -1;
	return p->drawn->faceInstalled(NULL, DrawnText(p, face, &len, buf, sizeof buf));
}

// the faces' names come back in the back end's encoding: into the run's
static int DrawnEnumFaces(void* user, char* buf, int charset, int* bytes)
{
	Preview_t* p = (Preview_t*)user;
	int all = 0, count = p->drawn->enumFaces(NULL, NULL, charset, &all), used = 0, i;
	char* names;
	const char* s;
	if(p->utf8 || count <= 0 || all <= 0 || (names = (char*)malloc((size_t)all + 1)) == NULL)
		return p->drawn->enumFaces(NULL, buf, charset, bytes);
	count = p->drawn->enumFaces(NULL, names, charset, &all);
	for(i = 0, s = names; i < count; i++, s += strlen(s) + 1)
	{
		char one[256];
		size_t k = Sjis_FromUtf8(s, one, sizeof one) + 1;
		if(buf)
			memcpy(buf + used, one, k);
		used += (int)k;
	}
	free(names);
	if(bytes)
		*bytes = used;
	return count;
}

/* The host of a run: the clock at 0, the drive with the folders a
 * Windows has on it, the user's data in a folder of its own that is never
 * made (what goes there stays in memory), and of the back end the native
 * files to read and the font files to draw with. */
static void PreviewHost(Preview_t* p)
{
	const OpenBGI_Files_t* real = OsCommon_Files();
	const OpenBGI_FontFiles_t* fonts = OS_FontFiles();
	OsLocalTime_t t;
	OpenBGI_Files_t* f = &p->files;
	size_t len;
	memset(p, 0, sizeof *p);
	OS_LocalTime(&t);
	p->date.year = t.year;
	p->date.month = t.month;
	p->date.dayOfWeek = t.dayOfWeek;
	p->date.day = t.day;
	p->date.hour = t.hour;
	p->date.minute = t.minute;
	p->date.second = t.second;
	p->date.millis = t.millis;
	p->fileTime = ((uint64_t)time(NULL) + 11644473600u) * 10000000u;

	f->size = sizeof *f;
	f->open = FilesOpen;
	f->close = FilesClose;
	f->read = FilesRead;
	f->write = FilesWrite;
	f->seek = FilesSeek;
	f->length = FilesLength;
	f->getTime = FilesGetTime;
	f->setTime = FilesSetTime;
	f->attrs = FilesAttrs;
	f->setAttrs = FilesSetAttrs;
	f->remove = FilesRemove;
	f->move = FilesMove;
	f->copy = FilesCopy;
	f->makeDir = FilesMakeDir;
	f->removeDir = FilesRemoveDir;
	f->findFirst = FilesFindFirst;
	f->findNext = FilesFindNext;
	f->findClose = FilesFindClose;
	f->setCurrentDir = FilesSetCurrentDir;
	f->exeDir = FilesExeDir;
	f->driveType = FilesDriveType;
	f->logicalDrives = FilesLogicalDrives;
	f->volumeReady = FilesVolumeReady;
	f->mediaPresent = FilesMediaPresent;
	f->specialFolder = FilesSpecialFolder;
	f->windowsDir = FilesWindowsDir;
	f->nativeSep = real->nativeSep;
	f->nativeSjis = 0;
	f->nativeOpen = FilesNativeOpen;
	f->nativeStat = FilesNativeStat;
	f->nativeMkdir = FilesNativeMkdir;
	f->nativeRemove = FilesNativeRemove;
	f->nativeList = FilesNativeList;
	f->nativeAbsolute = FilesNativeAbsolute;
	f->toUtf8 = FilesSameText;
	f->fromUtf8 = FilesSameText;
	f->gameDir = FilesGameDir;
	f->setGameDir = FilesSetGameDir;
	f->userDataDir = FilesUserDataDir;
	f->binaryDir = FilesBinaryDir;
	f->nativeWritable = FilesNativeWritable;

	OS_UserDataDir(p->userDir, sizeof p->userDir - 16);
	len = strlen(p->userDir);
	if(len)
		snprintf(p->userDir + len, sizeof p->userDir - len, "preview%c", f->nativeSep);
	OS_BinaryDir(p->binDir, sizeof p->binDir);
	OS_GameDir(p->gameDir, sizeof p->gameDir);

	MemMake(p, "GC:\\USERS", 1);
	MemMake(p, "GC:\\USERS\\USER", 1);
	MemMake(p, "GC:\\WINDOWS", 1);

	if(fonts)
	{ // (a font the game brings is not the windows' to know of)
		p->fonts = *fonts;
		p->fonts.added = NULL;
	}
	p->drawn = OS_FontRenderer();
	if(p->drawn)
	{
		OpenBGI_FontRenderer_t* r = &p->renderer;
		r->size = sizeof *r;
		r->open = DrawnOpen;
		r->openMono = DrawnOpenMono;
		r->close = DrawnClose;
		r->render = DrawnRender;
		r->renderMono = DrawnRenderMono;
		r->advance = DrawnAdvance;
		r->addFile = DrawnAddFile;
		r->addMemory = DrawnAddMemory;
		r->faceExists = DrawnFaceExists;
		r->faceInstalled = DrawnFaceInstalled;
		r->enumFaces = DrawnEnumFaces;
		r->addSubstitute = p->drawn->addSubstitute ? DrawnAddSubstitute : NULL;
		p->host.fontRenderer = r;
		p->host.textEncoding = HostTextEncoding;
	}
	p->host.size = sizeof p->host;
	p->host.api = OPENBGI_API_VERSION;
	p->host.user = p;
	p->host.ticksMs = HostTicksMs;
	p->host.perfNs = HostPerfNs;
	p->host.localTime = HostLocalTime;
	p->host.fontFiles = fonts ? &p->fonts : NULL;
	p->host.caps = OPENBGI_CAP_DIALOGS | OPENBGI_CAP_POINTER | OPENBGI_CAP_KEYBOARD; // (nothing of the engine's own in the picture)
	p->host.files = f;
}

OpenBGI_Host_t* OsPreview_HostNew(void)
{
	Preview_t* p = (Preview_t*)malloc(sizeof *p);
	if(!p)
		return NULL;
	PreviewHost(p);
	return &p->host;
}

void OsPreview_HostFree(OpenBGI_Host_t* host)
{
	Preview_t* p = (Preview_t*)host;
	if(!p)
		return;
	MemFree(p);
	free(p);
}

// ---- the run ----------------------------------------------------------------------------------

// a message box gets its default button, one that would ask again its other; anything else is declined
static int Answer(OpenBGI_t* g)
{
	OpenBGI_Request_t q = {.size = sizeof q};
	int boxes = 0;
	while(OpenBGI_NextRequest(g, &q))
	{
		int r = 0;
		if(q.kind == OPENBGI_ASK_MESSAGE)
		{
			int second = (q.u.message.flags & OPENBGI_MB_DEFBUTTON2) != 0;
			switch(q.u.message.flags & 0xf)
			{
				case OPENBGI_MB_OKCANCEL: r = second ? OPENBGI_IDCANCEL : OPENBGI_IDOK; break;
				case OPENBGI_MB_YESNO: r = second ? OPENBGI_IDNO : OPENBGI_IDYES; break;
				case OPENBGI_MB_RETRYCANCEL: r = OPENBGI_IDCANCEL; break;
				default: r = OPENBGI_IDOK; break;
			}
			boxes++;
		}
		OpenBGI_Answer(g, q.id, r);
	}
	return boxes;
}

// 1 when the frame's picture is of one colour (nothing on it yet, or a fade at its darkest)
static int OneColour(const OpenBGI_Frame_t* f)
{
	int x, y;
	if(f->bpp == 32)
	{
		uint32_t first = *(const uint32_t*)(const void*)f->pixels & 0xffffffu;
		for(y = 0; y < f->height; y++)
		{
			const uint32_t* row = (const uint32_t*)(const void*)((const uint8_t*)f->pixels + (size_t)y * (size_t)f->pitch);
			for(x = 0; x < f->width; x++)
				if((row[x] & 0xffffffu) != first)
					return 0;
		}
	}
	else
	{
		uint16_t first = *(const uint16_t*)(const void*)f->pixels & 0x7fffu;
		for(y = 0; y < f->height; y++)
		{
			const uint16_t* row = (const uint16_t*)(const void*)((const uint8_t*)f->pixels + (size_t)y * (size_t)f->pitch);
			for(x = 0; x < f->width; x++)
				if((row[x] & 0x7fffu) != first)
					return 0;
		}
	}
	return 1;
}

// the frame's picture is the one kept in `last`
static int SamePicture(const OsPreviewResult_t* last, const OpenBGI_Frame_t* f)
{
	return last->pixels && last->width == f->width && last->height == f->height && last->pitch == f->pitch && last->bpp == f->bpp &&
		memcmp(last->pixels, f->pixels, (size_t)f->pitch * (size_t)f->height) == 0;
}

// the frame's picture kept in `last`; 0 without memory
static int KeepPicture(OsPreviewResult_t* last, const OpenBGI_Frame_t* f)
{
	size_t bytes = (size_t)f->pitch * (size_t)f->height;
	uint8_t* pixels = (uint8_t*)realloc(last->pixels, bytes ? bytes : 1);
	if(!pixels)
		return 0;
	memcpy(pixels, f->pixels, bytes);
	last->pixels = pixels;
	last->width = f->width;
	last->height = f->height;
	last->pitch = f->pitch;
	last->bpp = f->bpp;
	return 1;
}

/* The things to click that the picture has now: a panel's buttons, a
 * menu's entries, icons, sprites and objects the script tests the pointer
 * against - not what takes a click anywhere (a text, a wait, a layer),
 * and not what takes none at the moment. */
static int Buttons(OpenBGI_t* g)
{
	OpenBGI_Target_t t[64];
	int n = OpenBGI_Targets(g, t, (int)BGI_COUNTOF(t), sizeof t[0]), i, count = 0;
	for(i = 0; i < n && i < (int)BGI_COUNTOF(t); i++)
	{
		if(t[i].flags & (OPENBGI_TARGET_ANYWHERE | OPENBGI_TARGET_DISABLED | OPENBGI_TARGET_COVERED))
			continue;
		switch(t[i].kind)
		{
			case OPENBGI_TARGET_BUTTON:
			case OPENBGI_TARGET_ITEM:
			case OPENBGI_TARGET_ICON:
			case OPENBGI_TARGET_SPRITE:
			case OPENBGI_TARGET_PROBE:
			case OPENBGI_TARGET_HOTSPOT: count++; break;
			default: break;
		}
	}
	return count;
}

/* The run.  First the title is looked for: Ctrl is held and the picture's
 * corner clicked, which takes a game past its brand calls and notices,
 * until the picture has PREVIEW_MENU things to click - a title's menu
 * has "start", "settings" and "end" at least - or PREVIEW_SEEK is over (a
 * title that asks for a click anywhere is passed, and the menu behind it
 * is the one found; a game without a menu is found in its first scene).
 * Then the keys are let go and the picture is looked at every
 * PREVIEW_EVERY, each time with one painted frame, and taken when it
 * stopped changing - a title is often dark at first, or comes in piece by
 * piece - or, where it never stands still, as it is after a while; never
 * while it is of one colour.
 *
 * `want` is what the launcher asked for (OPENBGI_PREVIEW_*): the icon is
 * taken from the engine as soon as the game is started, and without a
 * picture wanted the game is not run at all.  1 when the game was
 * started and the run not cut short: out->pixels is the picture then,
 * NULL when there was none to take. */
int OsPreview_Run(const char* cmdLine, int want, const int* cancel, OsPreviewResult_t* out)
{
	Preview_t* p = (Preview_t*)OsPreview_HostNew();
	OpenBGI_Frame_t f = {.size = sizeof f};
	OpenBGI_t* g;
	uint32_t from = 0, lookAt = 0, nextClick = PREVIEW_CLICK, nextCheck = 0;
	int started = 0, got = 0, seeking = 1, menu = 0, drawing = 0, ctrl = 0, button = 0, same = 0, plain = 1, boxes = 0;
	memset(out, 0, sizeof *out);
	if(!p)
		return 0;
	p->cancel = cancel;
	g = OpenBGI_New(&p->host);
	if(g && OpenBGI_Start(g, cmdLine) == OPENBGI_RUNNING)
	{
		started = 1;
		if(want & OPENBGI_PREVIEW_ICON)
		{
			size_t n = 0;
			const void* ico = OpenBGI_GameIcon(g, &n);
			if(ico && n && (out->icon = (uint8_t*)malloc(n)) != NULL)
			{
				memcpy(out->icon, ico, n);
				out->iconSize = n;
			}
		}
		OpenBGI_SetDrawing(g, 0);
		while((want & OPENBGI_PREVIEW_PICTURE) && boxes <= PREVIEW_BOXES && (seeking || p->now - from < PREVIEW_LAST))
		{
			uint32_t d;
			if(__atomic_load_n(cancel, __ATOMIC_ACQUIRE))
			{
				started = 0;
				break;
			}
			boxes += Answer(g);
			OpenBGI_PointerPlace(g, 1, 1);
			if(OpenBGI_RunFrame(g, &f) != OPENBGI_RUNNING)
				break;
			if(seeking)
			{
				if(p->now >= nextCheck)
				{
					nextCheck = p->now + PREVIEW_CHECK;
					menu = Buttons(g) >= PREVIEW_MENU;
				}
				if(menu || p->now >= PREVIEW_SEEK)
				{ // the keys let go: the title is to be left alone
					if(button)
						OpenBGI_Button(g, OPENBGI_BUTTON_LEFT, 0, 1, 1, 0);
					if(ctrl)
						OpenBGI_Key(g, 0x11, 0, 0);
					ctrl = button = seeking = 0;
					from = p->now;
					lookAt = from + PREVIEW_EVERY;
				}
				else
				{
					if(!ctrl)
					{
						OpenBGI_Key(g, 0x11, 1, 0); // VK_CONTROL
						ctrl = 1;
					}
					if(button)
					{
						OpenBGI_Button(g, OPENBGI_BUTTON_LEFT, 0, 1, 1, 0);
						button = 0;
					}
					else if(p->now >= nextClick)
					{
						OpenBGI_Button(g, OPENBGI_BUTTON_LEFT, 1, 1, 1, 0);
						button = 1;
						nextClick = p->now + PREVIEW_CLICK;
					}
				}
			}
			else if(drawing && f.presented && f.pixels)
			{ // a look
				uint32_t since = p->now - from;
				if(SamePicture(out, &f))
					same++;
				else
				{
					same = 0;
					plain = OneColour(&f);
					if(!KeepPicture(out, &f))
						break;
				}
				if(!plain && (same >= PREVIEW_SAME || (since >= PREVIEW_SOON && same) || since >= (menu ? PREVIEW_MOVES : PREVIEW_LAST - PREVIEW_EVERY)))
				{
					got = 1;
					break;
				}
				drawing = 0;
				OpenBGI_SetDrawing(g, 0);
				lookAt = p->now + PREVIEW_EVERY;
			}
			OpenBGI_PumpEvents(g);
			// the clock: the wait the frame asked for, and after a frame that was shown the display's next one
			d = f.idleMs > 1 ? (uint32_t)f.idleMs : 1;
			if(f.presented)
			{
				uint64_t k = (uint64_t)p->now * PREVIEW_REFRESH / 1000u + 1;
				uint32_t blank = (uint32_t)((k * 1000u + PREVIEW_REFRESH - 1) / PREVIEW_REFRESH);
				if(blank - p->now > d)
					d = blank - p->now;
			}
			p->now += d;
			if(!seeking && !drawing && p->now >= lookAt)
			{
				drawing = 1;
				OpenBGI_SetDrawing(g, 1);
			}
		}
	}
	if(g)
	{
		OpenBGI_Stop(g);
		Answer(g);
		OpenBGI_Delete(g);
	}
	OsPreview_HostFree(&p->host);
	if(!got)
	{
		free(out->pixels);
		out->pixels = NULL;
	}
	return started;
}

// ---- beside the launcher: its reading, and one game at a time -----------------------------

/* Two threads at most do what the launcher's engine hands out.  One
 * reads for its program (OpenBGI_JobNext: folders' listings, looks at
 * games), several pieces a turn, with an engine that is kept for it.
 * The other runs a game for its first picture, one game at a time, and
 * no run is begun while there is something to read: the player waits for
 * the reading, and a drive that takes its time is not asked for two
 * things at once.  A run that is on its way does not hold the reading
 * up. */
enum
{
	JOB_NONE,
	JOB_RUNNING,
	JOB_DONE
};

#define JOB_BATCH       16 // pieces of reading taken at once,
#define JOB_TURN        30 // .. of which no more are begun after this many ms: the rest goes back, and what the program waits for comes first

typedef struct Worker
{
	OsThread_t* thread;
	int state;  // JOB_*: set to JOB_DONE by the thread, the rest by the engine's
	int cancel; // 1: the thread is to end what it does
} Worker_t;

// a run for a first picture
static struct
{
	Worker_t w;
	OpenBGI_t* owner; // the launcher's engine, which waits for the end
	char cmd[0x2000];
	int want; // OPENBGI_PREVIEW_*: what the launcher asked for
	OsPreviewResult_t result;
} gRun;

// the reading
static struct
{
	Worker_t w;
	OpenBGI_t* owner;
	OpenBGI_Job_t* work[JOB_BATCH];
	int workCount;
	int workRan;                // .. of which the thread has done so many (it counts up as it goes),
	int workGiven;              // .. and so many are back with the launcher's engine
	OpenBGI_Host_t* readerHost; // the engine that reads, and its host (as a run's: nothing is written)
	OpenBGI_t* reader;
} gRead;

static void RunThread(void* arg)
{
	BGI_UNUSED(arg);
	OsPreview_Run(gRun.cmd, gRun.want, &gRun.w.cancel, &gRun.result);
	__atomic_store_n(&gRun.w.state, JOB_DONE, __ATOMIC_RELEASE);
}

static void ReadThread(void* arg)
{
	uint32_t began = OS_TicksMs();
	int i;
	BGI_UNUSED(arg);
	for(i = 0; i < gRead.workCount && !__atomic_load_n(&gRead.w.cancel, __ATOMIC_ACQUIRE); i++)
	{
		if(i && OS_TicksMs() - began >= JOB_TURN)
			break; // a drive that takes its time: one piece at a time
		OpenBGI_JobRun(gRead.work[i], gRead.reader);
		if(__atomic_load_n(&gRead.w.cancel, __ATOMIC_ACQUIRE))
			break; // cut short: it may have found less than there is, so it does not count as done
		__atomic_store_n(&gRead.workRan, i + 1, __ATOMIC_RELEASE);
	}
	__atomic_store_n(&gRead.w.state, JOB_DONE, __ATOMIC_RELEASE);
}

static int WorkerStart(Worker_t* w, void (*fn)(void*), int priority)
{
	w->cancel = 0;
	w->state = JOB_RUNNING;
	w->thread = OS_ThreadStart(fn, NULL, priority);
	if(!w->thread)
		w->state = JOB_NONE;
	return w->thread != NULL;
}

// the thread joined; 1 when there was one
static int WorkerEnd(Worker_t* w)
{
	if(__atomic_load_n(&w->state, __ATOMIC_ACQUIRE) == JOB_NONE)
		return 0;
	OS_ThreadJoin(w->thread);
	w->thread = NULL;
	w->state = JOB_NONE;
	w->cancel = 0;
	return 1;
}

static void RunFree(void)
{
	free(gRun.result.pixels);
	free(gRun.result.icon);
	memset(&gRun.result, 0, sizeof gRun.result);
}

// the reading the thread has done so far, to the launcher's engine: the program goes on with it
static void WorkGive(void)
{
	int ran = __atomic_load_n(&gRead.workRan, __ATOMIC_ACQUIRE);
	while(gRead.workGiven < ran)
		OpenBGI_JobDone(gRead.owner, gRead.work[gRead.workGiven++]);
}

// the thread is through: the rest of what was taken goes back as it is, to wait again
static void WorkBack(void)
{
	WorkGive();
	while(gRead.workGiven < gRead.workCount)
		OpenBGI_JobBack(gRead.owner, gRead.work[gRead.workGiven++]);
	gRead.workCount = gRead.workRan = gRead.workGiven = 0;
}

// the program's reading: what is done goes back to it, the next pieces go out
static void ReadPoll(OpenBGI_t* o)
{
	int state = __atomic_load_n(&gRead.w.state, __ATOMIC_ACQUIRE);
	if(state == JOB_RUNNING)
	{
		WorkGive();
		return;
	}
	if(state == JOB_DONE)
	{
		WorkerEnd(&gRead.w);
		WorkBack();
	}
	while(gRead.workCount < JOB_BATCH && (gRead.work[gRead.workCount] = OpenBGI_JobNext(o)) != NULL)
		gRead.workCount++;
	if(!gRead.workCount)
		return;
	if(!gRead.reader)
	{
		gRead.readerHost = OsPreview_HostNew();
		gRead.reader = gRead.readerHost ? OpenBGI_New(gRead.readerHost) : NULL;
		if(gRead.readerHost)
			((Preview_t*)gRead.readerHost)->cancel = &gRead.w.cancel;
	}
	gRead.owner = o;
	if(!gRead.reader || !WorkerStart(&gRead.w, ReadThread, OS_PRIO_NORMAL))
	{ // no thread for it: read here, as a host that does not take such work; no engine either: nothing is found
		int i;
		for(i = 0; i < gRead.workCount; i++)
		{
			if(gRead.reader)
				OpenBGI_JobRun(gRead.work[i], gRead.reader);
			OpenBGI_JobDone(o, gRead.work[i]);
		}
		gRead.workCount = gRead.workRan = gRead.workGiven = 0;
	}
}

// the runs for the first pictures: what the one that ended brought goes to the launcher, the next begins
static void RunPoll(OpenBGI_t* o)
{
	int state = __atomic_load_n(&gRun.w.state, __ATOMIC_ACQUIRE);
	if(state == JOB_RUNNING)
		return;
	if(state == JOB_DONE)
	{ // (a frame without a picture: the run showed nothing)
		OpenBGI_Frame_t f = {.size = sizeof f};
		WorkerEnd(&gRun.w);
		if(gRun.want & OPENBGI_PREVIEW_ICON)
			OpenBGI_PreviewIcon(o, gRun.result.icon, gRun.result.iconSize);
		f.pixels = gRun.result.pixels;
		f.width = gRun.result.width;
		f.height = gRun.result.height;
		f.pitch = gRun.result.pitch;
		f.bpp = gRun.result.bpp;
		OpenBGI_PreviewDone(o, (gRun.want & OPENBGI_PREVIEW_PICTURE) ? &f : NULL);
		RunFree();
	}
	if(gRead.workCount)
		return; // the program's reading first: the player waits for it
	gRun.owner = o;
	gRun.want = OpenBGI_PreviewNext(o, gRun.cmd, sizeof gRun.cmd);
	if(gRun.want && !WorkerStart(&gRun.w, RunThread, OS_PRIO_LOW)) // (the launcher's own frames go first)
		OpenBGI_PreviewDone(o, NULL);                              // no thread: the game waits for another time
}

void OsPreview_Poll(OpenBGI_t* o)
{
	ReadPoll(o);
	RunPoll(o);
}

void OsPreview_Stop(void)
{
	__atomic_store_n(&gRead.w.cancel, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&gRun.w.cancel, 1, __ATOMIC_RELEASE);
	if(WorkerEnd(&gRead.w))
		WorkBack();
	if(WorkerEnd(&gRun.w))
	{ // given up: the launcher may ask for the game another time
		RunFree();
		OpenBGI_PreviewDone(gRun.owner, NULL);
	}
	gRead.w.cancel = gRun.w.cancel = 0;
	if(gRead.reader)
		OpenBGI_Delete(gRead.reader);
	OsPreview_HostFree(gRead.readerHost);
	gRead.reader = NULL;
	gRead.readerHost = NULL;
}

#endif // !BGI_WASM
