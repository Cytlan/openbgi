/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fs.c - the WebAssembly back end's files and directories
 *
 * The game directory lives on the asset server (tools/wasm/server.js) and is
 * read-only; the engine reads it through three operations (STAT, READ of a
 * range, LIST of a directory) carried by WasmHost_* (host.c).  Everything the
 * engine writes - save data, settings, the registry emulation - goes to an
 * *overlay*: a directory of the browser-side file system (Emscripten's
 * MEMFS, persisted to IndexedDB by the page) that shadows the server:
 *
 *   * a lookup tries the overlay first, then the list of deleted paths
 *     (a "tombstone" hides a server file the engine deleted), then the server;
 *   * OS_FileCreate always creates in the overlay (its parents are made as
 *     needed); OS_FileOpenRead never writes, so a server file is never
 *     written through;
 *   * a directory listing merges the server's entries (minus tombstones and
 *     the names the overlay shadows) with the overlay's.
 *
 * Server files are static, so what has been fetched stays: the metadata
 * (STAT, LIST) is cached without limit, and the file contents in 64 KB
 * blocks under an LRU limit (OsWasm_SetCacheLimit, 512 MB by default).  A
 * read asks the server only for the blocks it does not hold, in one request
 * per run of missing blocks (at most WASM_FETCH_MAX bytes each), so the
 * engine's own read pattern - archive header, entry table, one entry -
 * costs one round trip per step the first time and none afterwards.
 *
 * Paths are keyed in lower case, both for the caches and in the overlay
 * (the game data was written for a case-insensitive file system); the
 * server resolves case on its side.
 *
 * The interface is the file, directory and drive section of os.h plus
 * OS_ExeDir and OS_SetCurrentDir; os_wasm.h declares the back end's own
 * entries (OsWasm_FsInit / OsWasm_FsShutdown, the overlay root, the cache
 * limit and statistics, OsWasm_RelPath).  Nothing here touches the browser
 * directly: the overlay is reached through stdio and POSIX calls on
 * Emscripten's file system, the server through WasmHost_*, so the file is
 * also built on a host for tests/wasmfs.c and the simulator.
 */
#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_wasm.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>

#define WASM_BLOCK         0x10000u                   // the cache block: 64 KB
#define WASM_FETCH_MAX     (128u * WASM_BLOCK)        // the longest single READ, 8 MB (the server allows 16 MB)
#define WASM_PATH_MAX      0x400                      // bytes of a path buffer
#define WASM_HASH_SIZE     4096                       // buckets of the metadata, listing and block hash tables
#define WASM_DEFAULT_CACHE (512u * 1024u * 1024u)     // the block cache limit until OsWasm_SetCacheLimit
#define OPENBGI_DIR        ".openbgi"                 // the back end's own directory in the overlay
#define DELETED_LIST       OPENBGI_DIR "/deleted.txt" // the tombstones, one lower-case path per line

static char gOverlayRoot[WASM_PATH_MAX] = "/overlay"; // the overlay directory on the local file system
static char gCurDir[WASM_PATH_MAX];                   // relative to the game directory, "" = the root
static uint32_t gCacheLimit = WASM_DEFAULT_CACHE;     // bytes the block cache may hold
static uint32_t gRequests;                            // STAT, READ and LIST requests made (OsWasm_CacheStats)

// ---- small helpers -------------------------------------------------------------------

// ASCII letters only: that is how the server compares, and multi-byte text is left alone
static void LowerAscii(char* s)
{
	for(; *s; s++)
		if(*s >= 'A' && *s <= 'Z')
			*s = (char)(*s + ('a' - 'A'));
}

// FNV-1a of a string, for the hash tables
static uint32_t HashStr(const char* s)
{
	uint32_t h = 2166136261u;
	for(; *s; s++)
		h = (h ^ (uint8_t)*s) * 16777619u;
	return h;
}

// a copy on the heap (strdup is not C99)
static char* Strdup(const char* s)
{
	size_t n = strlen(s) + 1;
	char* d = (char*)malloc(n);
	if(d)
		memcpy(d, s, n);
	return d;
}

// the directory part (without the slash) and the name of a relative path
static void SplitPath(const char* rel, char* dir, size_t dn, const char** name)
{
	const char* slash = strrchr(rel, '/');
	if(!slash)
	{
		dir[0] = 0;
		*name = rel;
		return;
	}
	snprintf(dir, dn, "%.*s", (int)(slash - rel), rel);
	*name = slash + 1;
}

/* the OS_ATTR_* bits of an overlay entry: directory, read-only when the
 * owner has no write permission, hidden when the name starts with a dot;
 * OS_ATTR_NORMAL when none applies */
static uint32_t AttrsOfStat(const struct stat* st, const char* name)
{
	uint32_t a = 0;
	if(S_ISDIR(st->st_mode))
		a |= OS_ATTR_DIRECTORY;
	if(!(st->st_mode & S_IWUSR))
		a |= OS_ATTR_READONLY;
	if(name[0] == '.')
		a |= OS_ATTR_HIDDEN;
	return a ? a : OS_ATTR_NORMAL;
}

#define FILETIME_EPOCH_DIFF 11644473600ull // seconds from 1601-01-01 (FILETIME) to 1970-01-01 (time_t)

// ---- paths --------------------------------------------------------------------------

/* The engine's path to the server's: Shift-JIS to UTF-8, every drive letter
 * stands for the game directory, '\\' and '/' both separate, "." is dropped
 * and ".." goes back one component but never above the root.  A relative
 * path (no drive, no leading separator) starts at the current directory
 * (OS_SetCurrentDir).  The result has '/' separators, no leading or
 * trailing one, and is "" for the game directory itself. */
void OsWasm_RelPath(const char* win, char* out, size_t n)
{
	char utf[WASM_PATH_MAX * 2];
	char comps[WASM_PATH_MAX];
	const char* p;
	size_t o = 0;
	int absolute = 0;

	OsWasm_SjisToUtf8(win, utf, sizeof utf);
	p = utf;
	if(((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
	{
		p += 2; // every drive is the game directory
		absolute = 1;
	}
	if(*p == '\\' || *p == '/')
		absolute = 1;
	comps[0] = 0;
	if(!absolute && gCurDir[0])
	{
		snprintf(comps, sizeof comps, "%s", gCurDir);
		o = strlen(comps);
	}
	while(*p)
	{
		char comp[WASM_PATH_MAX];
		size_t c = 0;
		while(*p == '\\' || *p == '/')
			p++;
		while(*p && *p != '\\' && *p != '/' && c + 1 < sizeof comp)
			comp[c++] = *p++;
		comp[c] = 0;
		if(c == 0 || strcmp(comp, ".") == 0)
			continue;
		if(strcmp(comp, "..") == 0)
		{ // back to the previous component, never above the root
			while(o > 0 && comps[o - 1] != '/')
				o--;
			if(o > 0)
				o--;
			comps[o] = 0;
			continue;
		}
		if(o + c + 2 >= sizeof comps)
			break; // too long: the rest of the path is dropped
		if(o)
			comps[o++] = '/';
		memcpy(comps + o, comp, c + 1);
		o += c;
	}
	snprintf(out, n, "%s", comps);
}

// the native path of a relative path inside the overlay (the relative part in lower case)
static void OverlayPath(const char* rel, char* out, size_t n)
{
	snprintf(out, n, "%s%s%s", gOverlayRoot, rel[0] ? "/" : "", rel);
	LowerAscii(out + strlen(gOverlayRoot));
}

// stat of a relative path's overlay entry; 1 when it exists
static int OverlayStat(const char* rel, struct stat* st)
{
	char p[WASM_PATH_MAX];
	OverlayPath(rel, p, sizeof p);
	return stat(p, st) == 0;
}

// make the overlay directories a path's parents need (the path's own last component is not made)
static void OverlayMakeParents(const char* rel)
{
	char p[WASM_PATH_MAX];
	char* s;
	if(!rel[0])
		return;
	OverlayPath(rel, p, sizeof p);
	s = p + strlen(gOverlayRoot) + 1;
	for(; *s; s++)
	{
		if(*s == '/')
		{
			*s = 0;
			mkdir(p, 0755);
			*s = '/';
		}
	}
}

// ---- tombstones: server paths the engine deleted ---------------------------------------

/* A tombstone hides a server path (and everything below it) from every
 * lookup and listing until a file or directory is created under that exact
 * path again.  The list lives in DELETED_LIST of the overlay and is saved
 * whenever it changes. */
typedef struct Tomb
{
	char* key; // lower case relative path
	struct Tomb* next;
} Tomb_t;
static Tomb_t* gTombs; // unordered

// write the list to DELETED_LIST and schedule a sync of the overlay
static void TombSave(void)
{
	char p[WASM_PATH_MAX];
	FILE* f;
	Tomb_t* t;
	snprintf(p, sizeof p, "%s/%s", gOverlayRoot, DELETED_LIST);
	f = fopen(p, "wb");
	if(!f)
		return;
	for(t = gTombs; t; t = t->next)
		fprintf(f, "%s\n", t->key);
	fclose(f);
	WasmHost_OverlayChanged();
}

// read the list back (OsWasm_FsInit); a missing file is an empty list
static void TombLoad(void)
{
	char p[WASM_PATH_MAX], line[WASM_PATH_MAX];
	FILE* f;
	snprintf(p, sizeof p, "%s/%s", gOverlayRoot, DELETED_LIST);
	f = fopen(p, "rb");
	if(!f)
		return;
	while(fgets(line, sizeof line, f))
	{
		Tomb_t* t;
		size_t n = strlen(line);
		while(n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = 0;
		if(!n)
			continue;
		t = (Tomb_t*)malloc(sizeof *t);
		t->key = Strdup(line);
		t->next = gTombs;
		gTombs = t;
	}
	fclose(f);
}

// 1 when the path or one of its parents is a tombstone (`lower`: lower case relative path)
static int TombHas(const char* lower)
{
	Tomb_t* t;
	size_t n;
	for(t = gTombs; t; t = t->next)
	{
		n = strlen(t->key);
		if(strncmp(t->key, lower, n) == 0 && (lower[n] == 0 || lower[n] == '/'))
			return 1;
	}
	return 0;
}

// hide a server path; nothing to do when it is hidden already
static void TombAdd(const char* lower)
{
	Tomb_t* t;
	if(TombHas(lower))
		return;
	t = (Tomb_t*)malloc(sizeof *t);
	t->key = Strdup(lower);
	t->next = gTombs;
	gTombs = t;
	TombSave();
}

// a file created again under a deleted path takes the tombstone away (the exact path only)
static void TombRemove(const char* lower)
{
	Tomb_t** pp;
	for(pp = &gTombs; *pp; pp = &(*pp)->next)
	{
		if(strcmp((*pp)->key, lower) == 0)
		{
			Tomb_t* t = *pp;
			*pp = t->next;
			free(t->key);
			free(t);
			TombSave();
			return;
		}
	}
}

// ---- the server's metadata --------------------------------------------------------------

/* What the server has said about its files is kept for the rest of the
 * run, negative answers included: the game directory does not change while
 * the page is open.  Two tables, both keyed by the lower-case relative
 * path: the entries (STAT answers, or seeded from a listing) and the
 * directory listings (LIST answers). */
typedef struct SrvEntry
{
	char* key;      // lower case relative path
	int exists;     // 0: the server has no such path (a cached negative answer)
	int mtimeKnown; // 0 for an entry seeded from a listing (LIST carries no times)
	uint32_t attrs; // OS_ATTR_* bits as the server reports them
	uint64_t size;  // bytes
	uint64_t mtime; // FILETIME
	struct SrvEntry* next;
} SrvEntry_t;

typedef struct SrvDirEnt
{
	char* name;     // as the server spells it
	uint32_t attrs; // OS_ATTR_* bits
	uint64_t size;  // bytes
} SrvDirEnt_t;

typedef struct SrvDir
{
	char* key;         // lower case relative path, "" for the game directory
	int exists;        // 1 when the server lists it as a directory
	int count;         // entries in `ents`
	SrvDirEnt_t* ents; // in the server's order
	struct SrvDir* next;
} SrvDir_t;

static SrvEntry_t* gSrvHash[WASM_HASH_SIZE]; // the entries, chained by `next`
static SrvDir_t* gDirHash[WASM_HASH_SIZE];   // the listings, chained by `next`

// the cached entry of a path, NULL when the server was never asked about it
static SrvEntry_t* SrvFind(const char* lower)
{
	SrvEntry_t* e;
	for(e = gSrvHash[HashStr(lower) % WASM_HASH_SIZE]; e; e = e->next)
		if(strcmp(e->key, lower) == 0)
			return e;
	return NULL;
}

// add an entry (the caller knows there is none for the path yet)
static SrvEntry_t* SrvInsert(const char* lower, int exists, uint32_t attrs, uint64_t size, uint64_t mtime, int mtimeKnown)
{
	uint32_t h = HashStr(lower) % WASM_HASH_SIZE;
	SrvEntry_t* e = (SrvEntry_t*)calloc(1, sizeof *e);
	e->key = Strdup(lower);
	e->exists = exists;
	e->mtimeKnown = mtimeKnown;
	e->attrs = attrs;
	e->size = size;
	e->mtime = mtime;
	e->next = gSrvHash[h];
	gSrvHash[h] = e;
	return e;
}

/* the server's answer about a path (`lower`: lower case relative path),
 * cached for good; the entry is never NULL, its `exists` says whether the
 * path is there */
static SrvEntry_t* SrvStat(const char* lower)
{
	SrvEntry_t* e = SrvFind(lower);
	uint32_t out[5]; // attrs, size low, size high, mtime low, mtime high
	if(e)
		return e;
	gRequests++;
	if(WasmHost_Stat(lower, out) == 0)
		return SrvInsert(lower, 1, out[0], ((uint64_t)out[2] << 32) | out[1], ((uint64_t)out[4] << 32) | out[3], 1);
	return SrvInsert(lower, 0, 0, 0, 0, 1);
}

// the modification time (FILETIME) of a server file, asked for when the listing seeded the entry
static uint64_t SrvMtime(SrvEntry_t* e)
{
	uint32_t out[5];
	if(!e->mtimeKnown)
	{
		e->mtimeKnown = 1;
		gRequests++;
		if(WasmHost_Stat(e->key, out) == 0)
			e->mtime = ((uint64_t)out[4] << 32) | out[3];
	}
	return e->mtime;
}

// the cached listing of a directory, NULL when the server was never asked for it
static SrvDir_t* SrvDirFind(const char* lower)
{
	SrvDir_t* d;
	for(d = gDirHash[HashStr(lower) % WASM_HASH_SIZE]; d; d = d->next)
		if(strcmp(d->key, lower) == 0)
			return d;
	return NULL;
}

/* the server's listing of a directory, cached for good (`exists` 0 when the
 * path is not a directory there); the entries also seed the metadata cache
 * so that the files found do not cost a STAT each */
static SrvDir_t* SrvList(const char* lower)
{
	SrvDir_t* d = SrvDirFind(lower);
	uint32_t h;
	int size;
	uint8_t* raw;
	if(d)
		return d;
	d = (SrvDir_t*)calloc(1, sizeof *d);
	d->key = Strdup(lower);
	h = HashStr(lower) % WASM_HASH_SIZE;
	d->next = gDirHash[h];
	gDirHash[h] = d;
	gRequests++;
	size = WasmHost_ListSize(lower);
	if(size < 0)
		return d; // not a directory
	d->exists = 1;
	raw = (uint8_t*)malloc((size_t)size + 1);
	if(!raw)
		return d;
	WasmHost_ListTake(raw);
	{
		// the records (server.js): u32 attrs, u32 size low, u32 size high, u16 name length, the name (UTF-8, no NUL)
		size_t p = 0, cap = 0;
		while(p + 14 <= (size_t)size)
		{
			uint32_t attrs, lo, hi;
			uint16_t nlen;
			memcpy(&attrs, raw + p, 4);
			memcpy(&lo, raw + p + 4, 4);
			memcpy(&hi, raw + p + 8, 4);
			memcpy(&nlen, raw + p + 12, 2);
			p += 14;
			if(p + nlen > (size_t)size)
				break; // a truncated record ends the listing
			if((size_t)d->count == cap)
			{
				cap = cap ? cap * 2 : 64;
				d->ents = (SrvDirEnt_t*)realloc(d->ents, cap * sizeof *d->ents);
			}
			d->ents[d->count].name = (char*)malloc((size_t)nlen + 1);
			memcpy(d->ents[d->count].name, raw + p, nlen);
			d->ents[d->count].name[nlen] = 0;
			d->ents[d->count].attrs = attrs;
			d->ents[d->count].size = ((uint64_t)hi << 32) | lo;
			{ // seed the entry (without a time: SrvMtime fetches it when asked)
				char key[WASM_PATH_MAX];
				snprintf(key, sizeof key, "%s%s%s", lower, lower[0] ? "/" : "", d->ents[d->count].name);
				LowerAscii(key);
				if(!SrvFind(key))
					SrvInsert(key, 1, attrs, d->ents[d->count].size, 0, 0);
			}
			d->count++;
			p += nlen;
		}
	}
	free(raw);
	return d;
}

// ---- the block cache ---------------------------------------------------------------------

/* The contents of server files in WASM_BLOCK pieces, found through a hash
 * table keyed by (file, block index) and ordered by use in a doubly linked
 * list whose tail is evicted when the cache is over its limit. */
typedef struct Block
{
	SrvEntry_t* file;                // the server file the block belongs to
	uint32_t index;                  // offset / WASM_BLOCK
	uint32_t len;                    // bytes held (the last block of a file is short)
	uint8_t* data;                   // `len` bytes
	struct Block* hashNext;          // the bucket chain
	struct Block *lruPrev, *lruNext; // most recently used at gLruHead
} Block_t;

static Block_t* gBlockHash[WASM_HASH_SIZE];
static Block_t *gLruHead, *gLruTail;       // the LRU list: head most recent, tail the next victim
static uint32_t gCacheBytes, gCacheBlocks; // what the cache holds (OsWasm_CacheStats)

// the bucket of a (file, block index) pair
static uint32_t BlockHash(const SrvEntry_t* f, uint32_t index)
{
	return (uint32_t)((uintptr_t)f * 2654435761u + index * 40503u) % WASM_HASH_SIZE;
}

// take a block out of the LRU list
static void LruUnlink(Block_t* b)
{
	if(b->lruPrev)
		b->lruPrev->lruNext = b->lruNext;
	else
		gLruHead = b->lruNext;
	if(b->lruNext)
		b->lruNext->lruPrev = b->lruPrev;
	else
		gLruTail = b->lruPrev;
	b->lruPrev = b->lruNext = NULL;
}

// put a block at the head of the LRU list (the most recently used)
static void LruPush(Block_t* b)
{
	b->lruPrev = NULL;
	b->lruNext = gLruHead;
	if(gLruHead)
		gLruHead->lruPrev = b;
	gLruHead = b;
	if(!gLruTail)
		gLruTail = b;
}

// the cached block, made the most recently used; NULL when it is not held
static Block_t* BlockFind(SrvEntry_t* f, uint32_t index)
{
	Block_t* b;
	for(b = gBlockHash[BlockHash(f, index)]; b; b = b->hashNext)
	{
		if(b->file == f && b->index == index)
		{
			LruUnlink(b);
			LruPush(b);
			return b;
		}
	}
	return NULL;
}

// drop a block from the table, the LRU list and the statistics
static void BlockEvict(Block_t* b)
{
	Block_t** pp;
	for(pp = &gBlockHash[BlockHash(b->file, b->index)]; *pp; pp = &(*pp)->hashNext)
	{
		if(*pp == b)
		{
			*pp = b->hashNext;
			break;
		}
	}
	LruUnlink(b);
	gCacheBytes -= b->len;
	gCacheBlocks--;
	free(b->data);
	free(b);
}

/* keep a copy of a fetched block, evicting the least recently used ones to
 * stay under the limit; an empty block, or one larger than the whole limit,
 * is not kept (the caller has its data anyway) */
static void BlockStore(SrvEntry_t* f, uint32_t index, const uint8_t* data, uint32_t len)
{
	Block_t* b;
	uint32_t h;
	if(len == 0 || len > gCacheLimit)
		return;
	while(gCacheBytes + len > gCacheLimit && gLruTail)
		BlockEvict(gLruTail);
	b = (Block_t*)calloc(1, sizeof *b);
	if(!b)
		return;
	b->data = (uint8_t*)malloc(len);
	if(!b->data)
	{
		free(b);
		return;
	}
	memcpy(b->data, data, len);
	b->file = f;
	b->index = index;
	b->len = len;
	h = BlockHash(f, index);
	b->hashNext = gBlockHash[h];
	gBlockHash[h] = b;
	LruPush(b);
	gCacheBytes += len;
	gCacheBlocks++;
}

/* `len` bytes of a server file from `off` into `dst`: the blocks held are
 * copied, the others fetched in runs of whole blocks (so a fetch covers the
 * block's full 64 KB even for a 16-byte read, and the rest is kept for the
 * next one); the bytes delivered, fewer than `len` at the end of the file
 * or after a failed fetch */
static uint32_t SrvRead(SrvEntry_t* f, uint64_t off, uint8_t* dst, uint32_t len)
{
	uint32_t done = 0;
	if(off >= f->size)
		return 0;
	if(off + len > f->size)
		len = (uint32_t)(f->size - off);
	while(done < len)
	{
		uint64_t pos = off + done;
		uint32_t index = (uint32_t)(pos / WASM_BLOCK);
		uint32_t inBlock = (uint32_t)(pos % WASM_BLOCK);
		Block_t* b = BlockFind(f, index);
		if(b)
		{
			uint32_t n = b->len > inBlock ? b->len - inBlock : 0;
			if(n > len - done)
				n = len - done;
			if(n == 0)
				break; // a short block at the end: the file ends here
			memcpy(dst + done, b->data + inBlock, n);
			done += n;
			continue;
		}
		else
		{ // the run of missing blocks from here, within the request and the fetch limit
			uint32_t first = index, last = index, count;
			uint64_t runStart, runEnd, fileEnd = f->size;
			uint8_t* buf;
			int got;
			while((uint64_t)(last + 1) * WASM_BLOCK < off + len && last + 1 - first < WASM_FETCH_MAX / WASM_BLOCK &&
				!BlockFind(f, last + 1))
				last++;
			runStart = (uint64_t)first * WASM_BLOCK;
			runEnd = (uint64_t)(last + 1) * WASM_BLOCK;
			if(runEnd > fileEnd)
				runEnd = fileEnd;
			buf = (uint8_t*)malloc((size_t)(runEnd - runStart));
			if(!buf)
				break;
			gRequests++;
			got = WasmHost_Read(f->key, (uint32_t)runStart, (uint32_t)(runStart >> 32), (uint32_t)(runEnd - runStart), buf);
			if(got <= 0)
			{
				free(buf);
				break;
			}
			// the part the caller wants, then the blocks for later
			{
				uint64_t wantEnd = off + len;
				uint64_t copyEnd = runStart + (uint64_t)got;
				if(copyEnd > wantEnd)
					copyEnd = wantEnd;
				if(copyEnd > pos)
				{
					memcpy(dst + done, buf + (pos - runStart), (size_t)(copyEnd - pos));
					done += (uint32_t)(copyEnd - pos);
				}
			}
			for(count = 0; (uint64_t)count * WASM_BLOCK < (uint64_t)got; count++)
			{
				uint32_t blen = (uint32_t)got - count * WASM_BLOCK;
				if(blen > WASM_BLOCK)
					blen = WASM_BLOCK;
				BlockStore(f, first + count, buf + (size_t)count * WASM_BLOCK, blen);
			}
			free(buf);
			if((uint64_t)got < runEnd - runStart)
				break; // the server had less than its STAT said
		}
	}
	return done;
}

// ---- lookups through the overlay ------------------------------------------------------

// where a path was found and what is known about it there
typedef struct Lookup
{
	int where;       // 0 nowhere, 1 the overlay, 2 the server
	uint32_t attrs;  // OS_ATTR_* bits
	uint64_t size;   // bytes
	uint64_t mtime;  // FILETIME; for a server entry seeded from a listing 0 until SrvMtime is asked
	SrvEntry_t* srv; // the server's entry (where 2)
	struct stat st;  // the overlay's (where 1)
} Lookup_t;

/* find a relative path: the overlay first, then the tombstones (a hidden
 * path is "nowhere" without asking the server), then the server; 1 when
 * found, with *l filled in */
static int LookupRel(const char* rel, Lookup_t* l)
{
	char lower[WASM_PATH_MAX];
	const char* name;
	char dir[WASM_PATH_MAX];
	memset(l, 0, sizeof *l);
	snprintf(lower, sizeof lower, "%s", rel);
	LowerAscii(lower);
	SplitPath(rel, dir, sizeof dir, &name);
	if(OverlayStat(rel, &l->st))
	{
		l->where = 1;
		l->attrs = AttrsOfStat(&l->st, name);
		l->size = (uint64_t)l->st.st_size;
		l->mtime = ((uint64_t)l->st.st_mtime + FILETIME_EPOCH_DIFF) * 10000000ull;
		return 1;
	}
	if(TombHas(lower))
		return 0;
	l->srv = SrvStat(lower);
	if(!l->srv->exists)
		return 0;
	l->where = 2;
	l->attrs = l->srv->attrs;
	l->size = l->srv->size;
	l->mtime = l->srv->mtime; // may still be 0: a caller that needs the time asks SrvMtime
	return 1;
}

// the same from the engine's Windows path; `rel` (n bytes) receives the relative path
static int Lookup(const char* win, Lookup_t* l, char* rel, size_t n)
{
	OsWasm_RelPath(win, rel, n);
	return LookupRel(rel, l);
}

// ---- files -----------------------------------------------------------------------------

/* An open file is either a stdio stream on the overlay or a read position in
 * a server file: the server side has no handle, every read is positional
 * through the block cache. */
struct OsFile
{
	int overlay;             // 1: `f` of the overlay; 0: `srv` with `pos`
	FILE* f;                 // the overlay file
	SrvEntry_t* srv;         // the server file
	uint64_t pos;            // the read position in the server file, bytes
	int written;             // 1 when the overlay changed through this handle: the close syncs it
	char rel[WASM_PATH_MAX]; // the relative path (OS_FileSetTime needs it)
};

// a read-only handle on either side; NULL for a missing path or a directory
OsFile_t* OS_FileOpenRead(const char* path)
{
	OsFile_t* f = (OsFile_t*)calloc(1, sizeof *f);
	Lookup_t l;
	if(!f)
		return NULL;
	if(!Lookup(path, &l, f->rel, sizeof f->rel) || (l.attrs & OS_ATTR_DIRECTORY))
	{
		free(f);
		return NULL;
	}
	if(l.where == 1)
	{
		char p[WASM_PATH_MAX];
		OverlayPath(f->rel, p, sizeof p);
		f->overlay = 1;
		f->f = fopen(p, "rb");
		if(!f->f)
		{
			free(f);
			return NULL;
		}
		return f;
	}
	f->srv = l.srv;
	return f;
}

/* always in the overlay, truncating what was there: the parent directories
 * are made and a tombstone of the path is taken away, so the file shadows a
 * server file of the same name from now on; NULL for the root or when the
 * overlay refuses */
OsFile_t* OS_FileCreate(const char* path)
{
	OsFile_t* f = (OsFile_t*)calloc(1, sizeof *f);
	char p[WASM_PATH_MAX], lower[WASM_PATH_MAX];
	if(!f)
		return NULL;
	OsWasm_RelPath(path, f->rel, sizeof f->rel);
	if(!f->rel[0])
	{
		free(f);
		return NULL;
	}
	OverlayMakeParents(f->rel);
	OverlayPath(f->rel, p, sizeof p);
	f->overlay = 1;
	f->f = fopen(p, "w+b");
	if(!f->f)
	{
		free(f);
		return NULL;
	}
	f->written = 1;
	snprintf(lower, sizeof lower, "%s", f->rel);
	LowerAscii(lower);
	TombRemove(lower);
	return f;
}

// close either kind; a handle that wrote schedules the overlay's sync
void OS_FileClose(OsFile_t* f)
{
	if(!f)
		return;
	if(f->f)
		fclose(f->f);
	if(f->written)
		WasmHost_OverlayChanged();
	free(f);
}

// bytes read: a server read may suspend into the browser for the fetch
uint32_t OS_FileRead(OsFile_t* f, void* buf, uint32_t n)
{
	uint32_t r;
	if(f->overlay)
		return (uint32_t)fread(buf, 1, n, f->f);
	r = SrvRead(f->srv, f->pos, (uint8_t*)buf, n);
	f->pos += r;
	return r;
}

uint32_t OS_FileWrite(OsFile_t* f, const void* buf, uint32_t n)
{
	if(!f->overlay)
		return 0; // a server file is read-only
	f->written = 1;
	return (uint32_t)fwrite(buf, 1, n, f->f);
}

// from the start; a server position past the end is accepted and reads nothing
int OS_FileSeek(OsFile_t* f, uint32_t pos)
{
	if(f->overlay)
		return fseek(f->f, (long)pos, SEEK_SET) == 0;
	f->pos = pos;
	return 1;
}

// bytes; an overlay file is flushed first so that what was written counts
uint32_t OS_FileSize(OsFile_t* f)
{
	if(f->overlay)
	{
		struct stat st;
		fflush(f->f);
		if(fstat(fileno(f->f), &st) != 0)
			return 0;
		return (uint32_t)st.st_size;
	}
	return (uint32_t)f->srv->size;
}

// the last-write time as a FILETIME in two dwords; a server file's may cost a STAT
int OS_FileGetTime(OsFile_t* f, uint32_t* lo, uint32_t* hi)
{
	uint64_t ft;
	if(f->overlay)
	{
		struct stat st;
		if(fstat(fileno(f->f), &st) != 0)
			return 0;
		ft = ((uint64_t)st.st_mtime + FILETIME_EPOCH_DIFF) * 10000000ull;
	}
	else
		ft = SrvMtime(f->srv);
	*lo = (uint32_t)ft;
	*hi = (uint32_t)(ft >> 32);
	return 1;
}

// overlay files only (0 for a server file); the FILETIME is cut to whole seconds
int OS_FileSetTime(OsFile_t* f, uint32_t lo, uint32_t hi)
{
	uint64_t ft = ((uint64_t)hi << 32) | lo;
	struct utimbuf ut;
	char p[WASM_PATH_MAX];
	if(!f->overlay)
		return 0;
	fflush(f->f);
	OverlayPath(f->rel, p, sizeof p);
	ut.actime = ut.modtime = (time_t)(ft / 10000000ull - FILETIME_EPOCH_DIFF);
	return utime(p, &ut) == 0;
}

// the OS_ATTR_* bits of whichever side has the path, OS_INVALID_ATTRS when neither has
uint32_t OS_FileAttrs(const char* path)
{
	Lookup_t l;
	char rel[WASM_PATH_MAX];
	OsWasm_RelPath(path, rel, sizeof rel);
	if(!rel[0])
		return OS_ATTR_DIRECTORY; // the game directory itself
	if(!LookupRel(rel, &l))
		return OS_INVALID_ATTRS;
	return l.attrs;
}

// the read-only bit of an overlay entry (the owner's write permission); the other bits are ignored
int OS_FileSetAttrs(const char* path, uint32_t attrs)
{
	Lookup_t l;
	char rel[WASM_PATH_MAX], p[WASM_PATH_MAX];
	if(!Lookup(path, &l, rel, sizeof rel))
		return 0;
	if(l.where != 1)
		return 1; // the server's files have no attributes to change; the engine only ever clears read-only before deleting
	OverlayPath(rel, p, sizeof p);
	if(attrs & OS_ATTR_READONLY)
		l.st.st_mode &= (mode_t) ~(S_IWUSR | S_IWGRP | S_IWOTH);
	else
		l.st.st_mode |= S_IWUSR;
	if(chmod(p, l.st.st_mode) != 0)
		return 0;
	WasmHost_OverlayChanged();
	return 1;
}

/* an overlay file is unlinked, a server file gets a tombstone; 0 for a
 * missing path or a directory */
int OS_FileDelete(const char* path)
{
	Lookup_t l;
	char rel[WASM_PATH_MAX], p[WASM_PATH_MAX], lower[WASM_PATH_MAX];
	if(!Lookup(path, &l, rel, sizeof rel) || (l.attrs & OS_ATTR_DIRECTORY))
		return 0;
	snprintf(lower, sizeof lower, "%s", rel);
	LowerAscii(lower);
	if(l.where == 1)
	{
		OverlayPath(rel, p, sizeof p);
		if(unlink(p) != 0)
			return 0;
		WasmHost_OverlayChanged();
		// a server file of the same name must not reappear
		if(!TombHas(lower) && SrvStat(lower)->exists)
			TombAdd(lower);
		return 1;
	}
	TombAdd(lower);
	return 1;
}

/* copy through the engine's own calls: the source may be on either side,
 * the copy is created in the overlay; 1 when both could be opened (a short
 * read is not detected) */
static int CopyRel(const char* fromWin, const char* toWin)
{
	OsFile_t* in = OS_FileOpenRead(fromWin);
	OsFile_t* out;
	uint8_t buf[0x10000];
	uint32_t n;
	if(!in)
		return 0;
	out = OS_FileCreate(toWin);
	if(!out)
	{
		OS_FileClose(in);
		return 0;
	}
	while((n = OS_FileRead(in, buf, sizeof buf)) > 0)
		OS_FileWrite(out, buf, n);
	OS_FileClose(in);
	OS_FileClose(out);
	return 1;
}

/* an overlay entry (file or directory) is renamed, and the old name gets a
 * tombstone when the server has it too; a server file is copied into the
 * overlay and tombstoned; a server directory cannot be moved (0) */
int OS_FileMove(const char* from, const char* to)
{
	Lookup_t l;
	char rel[WASM_PATH_MAX], relTo[WASM_PATH_MAX];
	if(!Lookup(from, &l, rel, sizeof rel))
		return 0;
	OsWasm_RelPath(to, relTo, sizeof relTo);
	if(!relTo[0])
		return 0;
	if(l.where == 1)
	{
		char a[WASM_PATH_MAX], b[WASM_PATH_MAX], lower[WASM_PATH_MAX];
		OverlayMakeParents(relTo);
		OverlayPath(rel, a, sizeof a);
		OverlayPath(relTo, b, sizeof b);
		if(rename(a, b) != 0)
			return 0;
		snprintf(lower, sizeof lower, "%s", rel);
		LowerAscii(lower);
		if(SrvStat(lower)->exists)
			TombAdd(lower);
		snprintf(lower, sizeof lower, "%s", relTo);
		LowerAscii(lower);
		TombRemove(lower);
		WasmHost_OverlayChanged();
		return 1;
	}
	if(l.attrs & OS_ATTR_DIRECTORY)
		return 0; // moving one of the server's directories is not supported
	if(!CopyRel(from, to))
		return 0;
	return OS_FileDelete(from);
}

// CopyFile: the copy lands in the overlay; 0 when `to` exists (either side) and failIfExists is set
int OS_FileCopy(const char* from, const char* to, int failIfExists)
{
	if(failIfExists && OS_FileAttrs(to) != OS_INVALID_ATTRS)
		return 0;
	return CopyRel(from, to);
}

// CreateDirectory in the overlay: 0 when the path exists on either side (or is the root)
int OS_DirCreate(const char* path)
{
	Lookup_t l;
	char rel[WASM_PATH_MAX], p[WASM_PATH_MAX], lower[WASM_PATH_MAX];
	if(Lookup(path, &l, rel, sizeof rel) || !rel[0])
		return 0; // exists already
	OverlayMakeParents(rel);
	OverlayPath(rel, p, sizeof p);
	if(mkdir(p, 0755) != 0)
		return 0;
	snprintf(lower, sizeof lower, "%s", rel);
	LowerAscii(lower);
	TombRemove(lower);
	WasmHost_OverlayChanged();
	return 1;
}

static int DirIsEmpty(const char* dirRel);

/* RemoveDirectory: the directory must be empty as the engine sees it
 * (overlay and server together); an overlay directory is removed, a server
 * directory tombstoned; the root cannot be removed */
int OS_DirRemove(const char* path)
{
	Lookup_t l;
	char rel[WASM_PATH_MAX], p[WASM_PATH_MAX], lower[WASM_PATH_MAX];
	if(!Lookup(path, &l, rel, sizeof rel) || !(l.attrs & OS_ATTR_DIRECTORY) || !rel[0])
		return 0;
	if(!DirIsEmpty(rel))
		return 0;
	snprintf(lower, sizeof lower, "%s", rel);
	LowerAscii(lower);
	if(l.where == 1)
	{
		OverlayPath(rel, p, sizeof p);
		if(rmdir(p) != 0)
			return 0;
		WasmHost_OverlayChanged();
		if(SrvStat(lower)->exists)
			TombAdd(lower);
		return 1;
	}
	TombAdd(lower);
	return 1;
}

// ---- directory enumeration ---------------------------------------------------------------

/* OS_FindFirst collects the whole merged listing of the directory at once;
 * OS_FindNext then walks it, testing each name against the pattern. */
typedef struct FindEnt
{
	char name[260]; // UTF-8, as the side it came from spells it
	uint32_t attrs; // OS_ATTR_* bits
} FindEnt_t;

struct OsFind
{
	FindEnt_t* ents;   // the merged listing
	int count, next;   // entries held, the next one to test
	char pattern[260]; // UTF-8 file part with wildcards
};

// append an entry, growing the array (`cap` is its capacity)
static void FindAdd(OsFind_t* h, int* cap, const char* name, uint32_t attrs)
{
	if(h->count == *cap)
	{
		*cap = *cap ? *cap * 2 : 64;
		h->ents = (FindEnt_t*)realloc(h->ents, (size_t)*cap * sizeof *h->ents);
	}
	snprintf(h->ents[h->count].name, sizeof h->ents[h->count].name, "%s", name);
	h->ents[h->count].attrs = attrs;
	h->count++;
}

/* the merged listing of a directory (relative path): the overlay's entries
 * (without the back end's own OPENBGI_DIR), then the server's that neither
 * an overlay entry of the same name nor a tombstone hides; nothing of the
 * server when the directory itself is tombstoned */
static void FindCollect(OsFind_t* h, const char* dirRel)
{
	char lower[WASM_PATH_MAX], p[WASM_PATH_MAX];
	DIR* d;
	struct dirent* e;
	SrvDir_t* sd;
	int cap = 0, i, j, overlayCount;

	snprintf(lower, sizeof lower, "%s", dirRel);
	LowerAscii(lower);
	OverlayPath(dirRel, p, sizeof p);
	d = opendir(p);
	if(d)
	{
		while((e = readdir(d)) != NULL)
		{
			char full[WASM_PATH_MAX];
			struct stat st;
			if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
				continue;
			if(!dirRel[0] && strcmp(e->d_name, OPENBGI_DIR) == 0)
				continue; // the back end's own directory
			snprintf(full, sizeof full, "%s/%s", p, e->d_name);
			if(stat(full, &st) != 0)
				continue;
			FindAdd(h, &cap, e->d_name, AttrsOfStat(&st, e->d_name));
		}
		closedir(d);
	}
	overlayCount = h->count;
	if(TombHas(lower))
		return;
	sd = SrvList(lower);
	if(!sd->exists)
		return;
	for(i = 0; i < sd->count; i++)
	{
		char key[WASM_PATH_MAX], lname[260];
		int shadowed = 0;
		snprintf(lname, sizeof lname, "%s", sd->ents[i].name);
		LowerAscii(lname);
		for(j = 0; j < overlayCount; j++) // the overlay's names are lower case already
			if(strcmp(h->ents[j].name, lname) == 0)
				shadowed = 1;
		if(shadowed)
			continue;
		snprintf(key, sizeof key, "%s%s%s", lower, lower[0] ? "/" : "", lname);
		if(TombHas(key))
			continue;
		FindAdd(h, &cap, sd->ents[i].name, sd->ents[i].attrs);
	}
}

// 1 when the merged listing of a directory (relative path) has no entries
static int DirIsEmpty(const char* dirRel)
{
	OsFind_t h;
	int empty;
	memset(&h, 0, sizeof h);
	FindCollect(&h, dirRel);
	empty = h.count == 0;
	free(h.ents);
	return empty;
}

// the next entry that matches the pattern into *out; 0 when there is none left
static int FindFill(OsFind_t* h, OsFindData_t* out)
{
	while(h->next < h->count)
	{
		const FindEnt_t* e = &h->ents[h->next++];
		if(!OsCommon_WildcardMatch(h->pattern, e->name))
			continue;
		out->attrs = e->attrs;
		snprintf(out->name, sizeof out->name, "%s", e->name);
		return 1;
	}
	return 0;
}

/* the directory part of `pattern` is resolved and listed once (one LIST at
 * most), the file part becomes the wildcard pattern in UTF-8; NULL when
 * nothing matches */
OsFind_t* OS_FindFirst(const char* pattern, OsFindData_t* out)
{
	OsFind_t* h = (OsFind_t*)calloc(1, sizeof *h);
	char win[WASM_PATH_MAX], dirRel[WASM_PATH_MAX], utfPat[260];
	const char* file;
	size_t dirLen;
	if(!h)
		return NULL;
	file = OsCommon_FilePart(pattern);
	dirLen = (size_t)(file - pattern);
	if(dirLen >= sizeof win)
		dirLen = sizeof win - 1;
	memcpy(win, pattern, dirLen);
	win[dirLen] = 0;
	OsWasm_RelPath(win, dirRel, sizeof dirRel);
	OsWasm_SjisToUtf8(file, utfPat, sizeof utfPat);
	snprintf(h->pattern, sizeof h->pattern, "%s", utfPat);
	FindCollect(h, dirRel);
	if(!FindFill(h, out))
	{
		OS_FindClose(h);
		return NULL;
	}
	return h;
}

int OS_FindNext(OsFind_t* h, OsFindData_t* out)
{
	return FindFill(h, out);
}

void OS_FindClose(OsFind_t* h)
{
	if(!h)
		return;
	free(h->ents);
	free(h);
}

// ---- drives: the game directory is the one fixed drive ----------------------------------

// C: is fixed and exists; any other letter has no root (so the CD search finds nothing)
uint32_t OS_DriveType(const char* root)
{
	return (root[0] == 'C' || root[0] == 'c') ? OS_DRIVE_FIXED : OS_DRIVE_NO_ROOT;
}

// the list "C:\" NUL NUL; its length without the final NUL (4), 0 when `buf` is too small
uint32_t OS_LogicalDrives(char* buf, uint32_t n)
{
	static const char list[] = "C:\\\0"; // "C:\" and the first NUL; the array's own NUL is the second
	if(n < sizeof list + 1)
		return 0;
	memcpy(buf, list, sizeof list);
	buf[sizeof list] = 0;
	return sizeof list - 1;
}

int OS_VolumeReady(const char* root)
{
	return OS_DriveType(root) == OS_DRIVE_FIXED;
}

int OS_MediaPresent(char driveLetter)
{
	return driveLetter == 'C' || driveLetter == 'c';
}

void OS_ExeDir(char* buf, size_t n)
{
	snprintf(buf, n, "C:\\"); // the game directory is the root of the one drive
}

/* the binary has no directory of its own on a page: games.json is looked
 * for in the game directory, where OS_BinaryDir points too; a game
 * directory cannot be switched, native paths are the overlay's own and
 * are read through the file services (nothing is written) */
void OS_BinaryDir(char* buf, size_t n)
{
	OS_ExeDir(buf, n);
}

void OS_SetArgv0(const char* argv0)
{
	BGI_UNUSED(argv0);
}

void OS_GameDir(char* buf, size_t n)
{
	OS_ExeDir(buf, n);
}

int OS_SetGameDir(const char* dir)
{
	BGI_UNUSED(dir);
	return 0;
}

void OS_NativeToUtf8(const char* native, char* out, size_t n)
{
	snprintf(out, n, "%s", native);
}

void OS_Utf8ToNative(const char* utf8, char* out, size_t n)
{
	snprintf(out, n, "%s", utf8);
}

FILE* OS_NativeOpen(const char* path, const char* mode) // the page's own (in-memory) file system, not the game's files
{
	return fopen(path, mode);
}

uint8_t* OS_ReadNativeFile(const char* path, uint32_t* size)
{
	OsFile_t* f = OS_FileOpenRead(path);
	uint32_t len;
	uint8_t* buf;
	*size = 0;
	if(!f)
		return NULL;
	len = OS_FileSize(f);
	buf = (uint8_t*)malloc((size_t)len + 1);
	if(!buf || OS_FileRead(f, buf, len) != len)
	{
		free(buf);
		OS_FileClose(f);
		return NULL;
	}
	OS_FileClose(f);
	buf[len] = 0;
	*size = len;
	return buf;
}

int OS_WriteNativeFile(const char* path, const void* data, uint32_t size)
{
	BGI_UNUSED(path);
	BGI_UNUSED(data);
	BGI_UNUSED(size);
	return 0;
}

// the command-line tools are not built for the page: their native file system services answer "nothing there"
int OS_NativeStat(const char* path, OsNativeStat_t* out)
{
	BGI_UNUSED(path);
	BGI_UNUSED(out);
	return OS_NATIVE_NONE;
}

int OS_NativeMkdir(const char* path)
{
	BGI_UNUSED(path);
	return 0;
}

int OS_NativeRemove(const char* path)
{
	BGI_UNUSED(path);
	return 0;
}

int OS_NativeList(const char* dir, OsNativeListFn fn, void* ctx)
{
	BGI_UNUSED(dir);
	BGI_UNUSED(fn);
	BGI_UNUSED(ctx);
	return -1;
}

int OS_NativeSeek(FILE* f, uint64_t pos)
{
	return fseek(f, (long)pos, SEEK_SET) == 0;
}

void OS_NativeAbsolute(const char* path, char* out, size_t n)
{
	snprintf(out, n, "%s", path);
}

int OS_WorkerLimit(void)
{
	return 1;
}

int OS_RunWorkers(int n, int (*fn)(int index, void* ctx), void* ctx)
{
	int i, failed = 0;
	for(i = 0; i < n; i++)
		failed += fn(i, ctx) != 0;
	return failed;
}

void OS_NativeArgs(int* argc, char*** argv)
{
	BGI_UNUSED(argc);
	BGI_UNUSED(argv);
}

// the directory must exist on either side (the root always does); relative paths start there from now on
int OS_SetCurrentDir(const char* dir)
{
	Lookup_t l;
	char rel[WASM_PATH_MAX];
	OsWasm_RelPath(dir, rel, sizeof rel);
	if(rel[0] && (!LookupRel(rel, &l) || !(l.attrs & OS_ATTR_DIRECTORY)))
		return 0;
	snprintf(gCurDir, sizeof gCurDir, "%s", rel);
	return 1;
}

// ---- set-up ----------------------------------------------------------------------------

void OsWasm_SetOverlayRoot(const char* dir)
{
	snprintf(gOverlayRoot, sizeof gOverlayRoot, "%s", dir);
}

const char* OsWasm_OverlayRoot(void)
{
	return gOverlayRoot;
}

// a limit below what is held evicts at once
void OsWasm_SetCacheLimit(uint32_t bytes)
{
	gCacheLimit = bytes;
	while(gCacheBytes > gCacheLimit && gLruTail)
		BlockEvict(gLruTail);
}

void OsWasm_CacheStats(uint32_t* bytes, uint32_t* blocks, uint32_t* requests)
{
	*bytes = gCacheBytes;
	*blocks = gCacheBlocks;
	*requests = gRequests;
}

/* OS_Init, after the overlay is mounted: make the overlay root and the back
 * end's directory in it (both may exist), start at the game directory and
 * load the tombstones */
void OsWasm_FsInit(void)
{
	char p[WASM_PATH_MAX];
	mkdir(gOverlayRoot, 0755);
	snprintf(p, sizeof p, "%s/%s", gOverlayRoot, OPENBGI_DIR);
	mkdir(p, 0755);
	gCurDir[0] = 0;
	TombLoad();
}

// free the block cache and the tombstones (the metadata tables are left to the process's end)
void OsWasm_FsShutdown(void)
{
	while(gLruTail)
		BlockEvict(gLruTail);
	while(gTombs)
	{
		Tomb_t* t = gTombs;
		gTombs = t->next;
		free(t->key);
		free(t);
	}
}
