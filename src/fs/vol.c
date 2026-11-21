/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * vol.c - a volume: files of some origin as one read-only tree
 *          (src/include/bgi/vol.h)
 *
 * Vol_Open asks the reader of the origin - a folder's (voldir.c), a zip
 * archive's (zip.c), a disc image's (iso.c) - for the list of what is
 * there, with names as clean UTF-8 paths.  The rest is the same for all:
 * in a game's archive or image the common top-level folder is taken off
 * and the game's folder found when the root is not it; the folders the
 * paths imply are added; the list is sorted by key and the last of equal
 * keys kept.  The bytes of an entry are read through the origin's table
 * (VolOrigin_t), so nothing here knows how a zip keeps them.
 */
#include "vol_internal.h"

// an entry as its reader found it
typedef struct VolRaw
{
	char* path; // malloc'ed: the path, then a NUL, then the key, then a NUL
	VolEntry_t e;
	uint32_t order; // the position in the list (implied folders after all)
} VolRaw_t;

struct VolList
{
	VolRaw_t* items;
	uint32_t count, cap;
};

struct Vol
{
	VolIn_t in;                // the volume's file, for the reads of its entries (no stream for a folder)
	const VolOrigin_t* origin; // how the bytes of its entries are read
	char* path;                // its native path (Vol_Reopen opens it again)
	uint32_t count;            // entries
	VolEntry_t* entries;       // sorted by key
	char** strings;            // the allocated paths, one per entry (path, key and name share it)
	VolEntry_t* merged;        // with volumes laid over it (Vol_Overlay): the entries of all of them, sorted by key; else NULL
	uint32_t mergedCount;
	struct Vol** over; // the volumes laid over it, in the order they came
	uint32_t overCount;
};

// ---- the volume's file -----------------------------------------------------------------------------

int VolIn_ReadAt(const VolIn_t* f, uint64_t off, void* buf, size_t n)
{
	return Host_FileSeek(f->o, f->f, off) && Host_FileRead(f->o, f->f, buf, (uint32_t)n) == n;
}

uint8_t* VolIn_Load(const VolIn_t* f, uint64_t off, uint64_t n)
{
	uint8_t* d;
	if(!f->f || n > 0x7fffffffu || !(d = (uint8_t*)malloc((size_t)n + 1)))
		return NULL;
	if(!VolIn_ReadAt(f, off, d, (size_t)n))
	{
		free(d);
		return NULL;
	}
	d[n] = 0;
	return d;
}

int VolIn_Copy(const VolIn_t* f, uint64_t off, uint64_t n, VolWrite_t write, void* ctx)
{
	uint8_t* buf;
	int ok;
	if(!f->f || !Host_FileSeek(f->o, f->f, off) || !(buf = (uint8_t*)malloc(0x10000)))
		return 0;
	ok = 1;
	while(ok && n)
	{
		uint32_t want = n > 0x10000 ? 0x10000 : (uint32_t)n;
		uint32_t got = Host_FileRead(f->o, f->f, buf, want);
		ok = got == want && write(ctx, buf, got);
		n -= got;
	}
	free(buf);
	return ok;
}

const VolIn_t* Vol_In(const Vol_t* v)
{
	return &v->in;
}

HostFile_t* Vol_Reopen(const Vol_t* v)
{
	return Host_NativeOpen(v->in.o, v->path, OPENBGI_OPEN_READ);
}

// ---- names -------------------------------------------------------------------------------------------

size_t Vol_PutUtf8(char* out, size_t off, size_t cap, uint32_t cp)
{
	if(cp < 0x80)
	{
		if(off + 1 < cap)
			out[off++] = (char)cp;
	}
	else if(cp < 0x800)
	{
		if(off + 2 < cap)
		{
			out[off++] = (char)(0xc0 | (cp >> 6));
			out[off++] = (char)(0x80 | (cp & 0x3f));
		}
	}
	else if(cp < 0x10000)
	{
		if(off + 3 < cap)
		{
			out[off++] = (char)(0xe0 | (cp >> 12));
			out[off++] = (char)(0x80 | ((cp >> 6) & 0x3f));
			out[off++] = (char)(0x80 | (cp & 0x3f));
		}
	}
	else if(off + 4 < cap)
	{
		out[off++] = (char)(0xf0 | (cp >> 18));
		out[off++] = (char)(0x80 | ((cp >> 12) & 0x3f));
		out[off++] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[off++] = (char)(0x80 | (cp & 0x3f));
	}
	return off;
}

int Vol_PutUtf16Be(const uint8_t* raw, size_t n, char* out, size_t cap)
{
	size_t off = 0, i;
	for(i = 0; i + 1 < n; i += 2)
	{
		uint32_t c = ((uint32_t)raw[i] << 8) | raw[i + 1];
		if(c >= 0xd800 && c < 0xdc00 && i + 3 < n)
		{
			uint32_t d = ((uint32_t)raw[i + 2] << 8) | raw[i + 3];
			if(d >= 0xdc00 && d < 0xe000)
			{
				c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
				i += 2;
			}
		}
		if(c < 0x20 || c == '/' || c == '\\')
			return 0;
		off = Vol_PutUtf8(out, off, cap, c);
	}
	out[off] = 0;
	return 1;
}

void Vol_SetTime(VolEntry_t* e, int year, int month, int day, int hour, int min, int sec, int zoneMin)
{
	int64_t days, secs;
	int y, m;
	if(month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59)
		return;
	if(year >= 1980 && year <= 2107)
		e->dosTime = ((uint32_t)(year - 1980) << 25) | ((uint32_t)month << 21) | ((uint32_t)day << 16) | ((uint32_t)hour << 11) |
			((uint32_t)min << 5) | ((uint32_t)sec >> 1);
	if(year < 1601 || year > 9999)
		return;
	// the days since 1 January 1601 (the civil calendar counted from March)
	y = month <= 2 ? year - 1 : year;
	m = month <= 2 ? month + 9 : month - 3;
	days = (int64_t)365 * y + y / 4 - y / 100 + y / 400 + (153 * m + 2) / 5 + day - 1;
	days -= (int64_t)365 * 1600 + 1600 / 4 - 1600 / 100 + 1600 / 400 + (153 * 10 + 2) / 5; // 1 January 1601 is March-year 1600, month 10, day 1
	secs = days * 86400 + hour * 3600 + min * 60 + sec - (int64_t)zoneMin * 60;
	if(secs > 0)
		e->mtime = (uint64_t)secs * 10000000u;
}

// ---- the list a reader fills ---------------------------------------------------------------------

int VolList_Add(VolList_t* l, const char* path, const VolEntry_t* e)
{
	size_t len = strlen(path), i;
	VolRaw_t* r;
	if(l->count == l->cap)
	{
		uint32_t cap = l->cap ? l->cap * 2 : 256;
		VolRaw_t* items = (VolRaw_t*)realloc(l->items, cap * sizeof *items);
		if(!items)
			return 0;
		l->items = items;
		l->cap = cap;
	}
	r = &l->items[l->count];
	r->path = (char*)malloc(len * 2 + 2);
	if(!r->path)
		return 0;
	memcpy(r->path, path, len + 1);
	for(i = 0; i <= len; i++)
	{
		char c = path[i];
		r->path[len + 1 + i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
	}
	r->e = *e;
	r->order = l->count;
	l->count++;
	return 1;
}

uint32_t VolList_Count(const VolList_t* l)
{
	return l->count;
}

static void RawFree(VolList_t* l)
{
	uint32_t i;
	for(i = 0; i < l->count; i++)
	{
		free(l->items[i].path);
		free(l->items[i].e.file);
	}
	free(l->items);
}

static const char* RawKey(const VolRaw_t* r)
{
	return r->path + strlen(r->path) + 1;
}

// by key, then by position: the last of equal keys is the one kept
static int CompareRaw(const void* a, const void* b)
{
	const VolRaw_t* x = (const VolRaw_t*)a;
	const VolRaw_t* y = (const VolRaw_t*)b;
	int c = strcmp(RawKey(x), RawKey(y));
	if(c)
		return c;
	return x->order < y->order ? -1 : x->order > y->order;
}

// ---- where the game is ---------------------------------------------------------------------------

/* while no file is at the top and all entries are in one top-level
 * folder, make that folder the root */
static void StripTop(VolList_t* l)
{
	int round;
	for(round = 0; round < 8; round++)
	{
		const char* first = NULL;
		size_t firstLen = 0;
		uint32_t i, kept = 0;
		int one = l->count > 0;
		for(i = 0; i < l->count && one; i++)
		{
			const char* p = l->items[i].path;
			const char* slash = strchr(p, '/');
			size_t len = slash ? (size_t)(slash - p) : strlen(p);
			if(!slash && !l->items[i].e.isDir)
				one = 0; // a file at the top
			else if(!first)
				first = p, firstLen = len;
			else if(len != firstLen || memcmp(p, first, len) != 0)
				one = 0;
		}
		if(!one || !first)
			return;
		for(i = 0; i < l->count; i++)
		{
			VolRaw_t* r = &l->items[i];
			const char* slash = strchr(r->path, '/');
			if(!slash)
			{ // the folder itself
				free(r->path);
				continue;
			}
			{
				size_t len = strlen(slash + 1), k;
				memmove(r->path, slash + 1, len + 1);
				for(k = 0; k <= len; k++)
				{
					char c = r->path[k];
					r->path[len + 1 + k] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
				}
			}
			l->items[kept++] = *r;
		}
		l->count = kept;
	}
}

// whether `path` is the boot archive or a loose boot program directly in the folder `dir` (`dirLen` bytes, "" the root)
static int IsBootFile(const char* key, const char* dir, size_t dirLen)
{
	const char* name = key;
	if(dirLen)
	{
		if(strncmp(key, dir, dirLen) != 0 || key[dirLen] != '/')
			return 0;
		name = key + dirLen + 1;
	}
	return strcmp(name, "system.arc") == 0 || strcmp(name, "ipl._bp") == 0;
}

/* When the root holds neither the boot archive (system.arc) nor a loose
 * boot program (ipl._bp) but a folder below it does, that folder is the
 * game's and becomes the root: a disc keeps its game beside an
 * Autorun.inf and the DirectX setup, and so does an archive made from
 * one.  Of several such folders the one nearest the root is taken, the
 * first by name among equals; what is outside it is dropped. */
static void FindGame(VolList_t* l)
{
	const char* best = NULL;
	size_t bestLen = 0, bestDepth = 0;
	uint32_t i, kept = 0;
	for(i = 0; i < l->count; i++)
	{
		const VolRaw_t* r = &l->items[i];
		const char* key = RawKey(r);
		const char* slash = strrchr(key, '/');
		size_t len = slash ? (size_t)(slash - key) : 0, depth = 0, k;
		if(r->e.isDir || !IsBootFile(key, key, len))
			continue;
		if(!len)
			return; // the root is a game
		for(k = 0; k < len; k++)
			depth += key[k] == '/';
		if(best && depth == bestDepth)
		{ // of two at one depth the first by name
			int c = strncmp(key, best, len < bestLen ? len : bestLen);
			if(c > 0 || (c == 0 && len >= bestLen))
				continue;
		}
		else if(best && depth > bestDepth)
			continue;
		best = key, bestLen = len, bestDepth = depth;
	}
	if(!best)
		return;
	{
		char dir[VOL_NAME_MAX];
		if(bestLen >= sizeof dir)
			return;
		memcpy(dir, best, bestLen);
		dir[bestLen] = 0;
		for(i = 0; i < l->count; i++)
		{
			VolRaw_t* r = &l->items[i];
			const char* key = RawKey(r);
			size_t len, k;
			if(strncmp(key, dir, bestLen) != 0 || key[bestLen] != '/')
			{
				free(r->path);
				continue;
			}
			len = strlen(r->path + bestLen + 1);
			memmove(r->path, r->path + bestLen + 1, len + 1);
			for(k = 0; k <= len; k++)
			{
				char c = r->path[k];
				r->path[len + 1 + k] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
			}
			l->items[kept++] = *r;
		}
		l->count = kept;
	}
}

// add a folder entry for every parent of every path (duplicates are dropped by the sort afterwards)
static int AddImplied(VolList_t* l)
{
	uint32_t i, n = l->count;
	for(i = 0; i < n; i++)
	{
		char buf[VOL_NAME_MAX];
		char* slash;
		snprintf(buf, sizeof buf, "%s", l->items[i].path);
		while((slash = strrchr(buf, '/')) != NULL)
		{
			VolEntry_t e;
			*slash = 0;
			memset(&e, 0, sizeof e);
			e.isDir = 1;
			if(!VolList_Add(l, buf, &e))
				return 0;
		}
	}
	return 1;
}

// ---- opening -------------------------------------------------------------------------------------

static Vol_t* Open(OpenBGI_t* o, const char* nativePath, int plain)
{
	OpenBGI_NativeStat_t st;
	VolIn_t in;
	const VolOrigin_t* origin;
	VolList_t l;
	Vol_t* v;
	uint32_t i, kept = 0, real;
	int ok, kind;
	in.o = o;
	in.f = NULL;
	kind = Host_NativeStat(o, nativePath, &st);
	if(kind != OPENBGI_NATIVE_DIR && (kind != OPENBGI_NATIVE_FILE || !(in.f = Host_NativeOpen(o, nativePath, OPENBGI_OPEN_READ))))
		return NULL;
	memset(&l, 0, sizeof l);
	if(kind == OPENBGI_NATIVE_DIR)
	{ // a folder: its files as they are, each read from itself
		char dir[VOL_NAME_MAX];
		size_t len = (size_t)snprintf(dir, sizeof dir, "%s", nativePath);
		ok = len < sizeof dir;
		while(ok && len > 1 && (dir[len - 1] == '/' || dir[len - 1] == Host_NativeSep(o)) && !(len == 3 && dir[1] == ':'))
			dir[--len] = 0;
		ok = ok && VolDir_Read(o, dir, &l);
		origin = &kVolDirOrigin;
	}
	else if((ok = Zip_Read(&in, st.size, &l)) != 0)
	{ // a zip archive (one that cannot be read is nothing else either)
		ok = ok > 0;
		origin = &kZipOrigin;
	}
	else
	{ // a disc image: its files are runs of the image's bytes
		ok = Iso_Read(&in, st.size, &l);
		origin = &kVolRangeOrigin;
	}
	if(!ok)
	{
		RawFree(&l);
		Host_FileClose(o, in.f);
		return NULL;
	}
	if(kind != OPENBGI_NATIVE_DIR && !plain)
	{
		StripTop(&l);
		FindGame(&l);
	}
	for(i = 0; i < l.count; i++) // the positions again, without the top folder's entries
		l.items[i].order = i;
	real = l.count; // the entries from here on are implied folders
	if(!AddImplied(&l))
	{
		RawFree(&l);
		Host_FileClose(o, in.f);
		return NULL;
	}
	qsort(l.items, l.count, sizeof *l.items, CompareRaw);
	v = (Vol_t*)calloc(1, sizeof *v);
	if(v)
	{
		v->entries = (VolEntry_t*)calloc(l.count + 1, sizeof *v->entries);
		v->strings = (char**)calloc(l.count + 1, sizeof *v->strings);
		v->path = (char*)malloc(strlen(nativePath) + 1);
	}
	if(!v || !v->entries || !v->strings || !v->path)
	{
		if(v)
		{
			free(v->entries);
			free(v->strings);
			free(v->path);
			free(v);
		}
		RawFree(&l);
		Host_FileClose(o, in.f);
		return NULL;
	}
	strcpy(v->path, nativePath);
	v->in = in;
	v->origin = origin;
	for(i = 0; i < l.count;)
	{
		/* a run of equal keys: when an implied folder is among them the path
		 * has entries below it and is a folder (the last folder stored, else
		 * the implied one); otherwise the last entry stored wins */
		uint32_t end = i + 1, k, pick;
		VolRaw_t* r;
		const char* slash;
		while(end < l.count && strcmp(RawKey(&l.items[i]), RawKey(&l.items[end])) == 0)
			end++;
		pick = end - 1; // the run is in stored order, the implied folders last
		if(l.items[pick].order >= real)
			for(k = i; k < end; k++)
				if(l.items[k].order < real && l.items[k].e.isDir)
					pick = k;
		for(k = i; k < end; k++)
			if(k != pick)
			{
				free(l.items[k].path);
				free(l.items[k].e.file);
			}
		r = &l.items[pick];
		v->entries[kept] = r->e;
		v->entries[kept].path = r->path;
		v->entries[kept].key = RawKey(r);
		slash = strrchr(r->path, '/');
		v->entries[kept].name = slash ? slash + 1 : r->path;
		v->strings[kept] = r->path;
		kept++;
		i = end;
	}
	free(l.items);
	v->count = kept;
	return v;
}

Vol_t* Vol_Open(OpenBGI_t* o, const char* nativePath)
{
	return Open(o, nativePath, 0);
}

Vol_t* Vol_OpenPlain(OpenBGI_t* o, const char* nativePath)
{
	return Open(o, nativePath, 1);
}

// ---- volumes over each other, and the lookups ------------------------------------------------------

// the entries the archive lists: its own, or with archives laid over it those of all
static const VolEntry_t* Listed(const Vol_t* v, uint32_t* count)
{
	*count = v->merged ? v->mergedCount : v->count;
	return v->merged ? v->merged : v->entries;
}

// the index of the first of `n` sorted entries whose key is not below `key`
static uint32_t Lower(const VolEntry_t* e, uint32_t n, const char* key)
{
	uint32_t lo = 0, hi = n;
	while(lo < hi)
	{
		uint32_t mid = lo + (hi - lo) / 2;
		if(strcmp(e[mid].key, key) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

// whether the `n` entries have a file of the key
static int HasFile(const VolEntry_t* e, uint32_t n, const char* key)
{
	uint32_t i = Lower(e, n, key);
	return i < n && !e[i].isDir && strcmp(e[i].key, key) == 0;
}

/* what to leave out at the start of the paths of the `nb` entries laid
 * over the `na`: the folder at the top that holds them all, with its
 * '/', when their files meet files of the others only without it; else 0 */
static size_t LeftOut(const VolEntry_t* a, uint32_t na, const VolEntry_t* b, uint32_t nb)
{
	size_t len;
	uint32_t j, asIs = 0, inside = 0;
	if(nb < 2 || !b[0].isDir || strchr(b[0].key, '/'))
		return 0;
	len = strlen(b[0].key);
	for(j = 1; j < nb; j++)
	{
		if(strncmp(b[j].key, b[0].key, len) != 0 || b[j].key[len] != '/')
			return 0;
		if(!b[j].isDir)
		{
			asIs += (uint32_t)HasFile(a, na, b[j].key);
			inside += (uint32_t)HasFile(a, na, b[j].key + len + 1);
		}
	}
	return !asIs && inside ? len + 1 : 0;
}

int Vol_Overlay(Vol_t* v, Vol_t* over)
{
	uint32_t na, nb, i = 0, j = 0, n = 0;
	const VolEntry_t *a = Listed(v, &na), *b = Listed(over, &nb);
	size_t skip = LeftOut(a, na, b, nb);
	VolEntry_t* m = (VolEntry_t*)calloc((size_t)na + nb + 1, sizeof *m);
	Vol_t** list = (Vol_t**)realloc(v->over, ((size_t)v->overCount + 1) * sizeof *list);
	if(list)
		v->over = list;
	if(!m || !list)
	{
		free(m);
		return 0;
	}
	if(skip)
		j = 1; // (the folder itself)
	while(i < na || j < nb)
	{
		VolEntry_t e;
		int c;
		if(j < nb)
		{
			e = b[j];
			if(!e.from)
				e.from = over;
			e.path += skip;
			e.key += skip;
		}
		c = i >= na ? 1 : j >= nb ? -1
								  : strcmp(a[i].key, e.key);
		if(c < 0)
			m[n++] = a[i++];
		else
		{
			m[n++] = c == 0 && a[i].isDir ? a[i] : e;
			i += c == 0;
			j++;
		}
	}
	free(v->merged);
	v->merged = m;
	v->mergedCount = n;
	v->over[v->overCount++] = over;
	return 1;
}

void Vol_Close(Vol_t* v)
{
	uint32_t i;
	if(!v)
		return;
	for(i = 0; i < v->overCount; i++)
		Vol_Close(v->over[i]);
	free(v->over);
	free(v->merged);
	for(i = 0; i < v->count; i++)
	{
		free(v->strings[i]);
		free(v->entries[i].file);
	}
	free(v->strings);
	free(v->entries);
	free(v->path);
	Host_FileClose(v->in.o, v->in.f);
	free(v);
}

const char* Vol_Path(const Vol_t* v)
{
	return v->path;
}

uint32_t Vol_Count(const Vol_t* v)
{
	uint32_t n;
	Listed(v, &n);
	return n;
}

const VolEntry_t* Vol_At(const Vol_t* v, uint32_t i)
{
	uint32_t n;
	const VolEntry_t* e = Listed(v, &n);
	return i < n ? &e[i] : NULL;
}

uint32_t Vol_Lower(const Vol_t* v, const char* key)
{
	uint32_t n;
	const VolEntry_t* e = Listed(v, &n);
	return Lower(e, n, key);
}

const VolEntry_t* Vol_Find(const Vol_t* v, const char* key)
{
	uint32_t n, i;
	const VolEntry_t* e = Listed(v, &n);
	if(!key[0])
		return NULL;
	i = Lower(e, n, key);
	return i < n && strcmp(e[i].key, key) == 0 ? &e[i] : NULL;
}

// ---- the bytes of an entry -----------------------------------------------------------------------

static uint8_t* RangeLoad(Vol_t* v, const VolEntry_t* e, size_t* size)
{
	uint8_t* d = VolIn_Load(&v->in, e->data, e->size);
	if(d)
		*size = (size_t)e->size;
	return d;
}

static int RangeExtract(Vol_t* v, const VolEntry_t* e, VolWrite_t write, void* ctx)
{
	return VolIn_Copy(&v->in, e->data, e->size, write, ctx);
}

static HostFile_t* RangeOpenStored(Vol_t* v, const VolEntry_t* e, uint64_t* offset)
{
	HostFile_t* f = Vol_Reopen(v);
	if(f)
		*offset = e->data;
	return f;
}

const VolOrigin_t kVolRangeOrigin = {RangeLoad, RangeExtract, RangeOpenStored};

uint8_t* Vol_Load(Vol_t* v, const VolEntry_t* e, size_t* size)
{
	if(e->from)
		v = e->from; // an entry of a volume laid over this one
	*size = 0;
	if(!e->readable || e->size > 0x7fffffffu || e->csize > 0x7fffffffu)
		return NULL;
	return v->origin->load(v, e, size);
}

int Vol_Extract(Vol_t* v, const VolEntry_t* e, int (*write)(void* ctx, const uint8_t* data, size_t n), void* ctx)
{
	if(e->from)
		v = e->from; // an entry of a volume laid over this one
	return e->readable && v->origin->extract(v, e, write, ctx);
}

HostFile_t* Vol_OpenStored(Vol_t* v, const VolEntry_t* e, uint64_t* offset)
{
	if(e->from)
		v = e->from; // an entry of a volume laid over this one
	return e->readable && e->stored ? v->origin->openStored(v, e, offset) : NULL;
}
