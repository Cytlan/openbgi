/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * arc.c - PackFile / BURIKO ARC20 archives, the archive manager chain and
 *         the file cache; interface in file.h
 *
 * Three parts:
 *
 *   FileSet      one archive: its index (entry names, offsets and sizes,
 *                converted from either on-disk format into ArcEntry_t) and
 *                the virtual operations contains / arcNameFor / readRange /
 *                sizeOf / matches on it.
 *   ArcMgr_*     the archive manager.  The manager is itself a FileSet:
 *                gArcMgr is the first node of a chain, one node per archive
 *                path that was ever opened.  A request names the archive by
 *                path and walks the chain until a node "matches" it (an empty
 *                node adopts the path and loads its index on the way), then
 *                forwards to that node's virtual implementation.  A request
 *                for an archive that does not exist leaves an unloaded node
 *                at the end of the chain, where the next request reuses it.
 *   FileSetList  the subclass registered by "80 38": a pseudo archive name
 *                that stands for a list of archive files in the directory of
 *                the path that matched; the members are searched in order
 *                through a private FileSet chain.
 *   FileCache    the loader's optional LRU cache of whole files, keyed by
 *                archive and entry name (enabled by "90 03").
 *
 * All names and paths are Shift-JIS; the index and every lookup are
 * lower-cased so that matching is case-insensitive.
 */
#include "bgi/file.h"
#include "bgi/strutil.h"
#include "bgi/os.h"

FileSet_t* gArcMgr; // the head of the archive manager chain

// -------------------------------------------------------------------------
// FileSet
// -------------------------------------------------------------------------

static const FileSetVtbl_t FileSet_Vtbl;

// the base-class part of the constructor, shared with FileSetList
static void FileSet_BaseCtor(FileSet_t* s)
{
	memset(s, 0, 0x11c); // 0x11c: the original's object size; the allocation is zeroed anyway
	s->vt = &FileSet_Vtbl;
	s->loaded = 0;
	s->next = NULL;
	s->entries = NULL;
}

FileSet_t* FileSet_New(void)
{
	FileSet_t* s = (FileSet_t*)BGI_Calloc(sizeof(FileSet_t));
	FileSet_BaseCtor(s);
	return s;
}

/* release the index and the rest of the chain: every following node is torn
 * down and then destroyed through its vtable, as in the original */
static void FileSet_BaseDtor(FileSet_t* s)
{
	s->loaded = 0;
	if(s->next)
	{
		FileSet_BaseDtor(s->next); // (recursion in the original)
		if(s->next)
			s->next->vt->destroy(s->next);
		s->next = NULL;
	}
	BGI_Free(s->entries);
	s->entries = NULL;
}

// the virtual destructor: releases the node, its index and the chain after it
static void FileSet_Destroy(FileSet_t* s)
{
	s->vt = &FileSet_Vtbl;
	FileSet_BaseDtor(s);
	BGI_Free(s);
}

// destroy through the vtable (a FileSetList frees its list too); NULL is allowed
void FileSet_Delete(FileSet_t* s)
{
	if(s)
		s->vt->destroy(s);
}

// copy the archive name out
static void FileSet_CopyName(FileSet_t* s, char* dst)
{
	strcpy(dst, s->name);
}

/*
 * Read the archive index of the file at `path` into the node.  Returns 0,
 * ARC_ERR_INDEX_OPEN when the file cannot be opened or ARC_ERR_INDEX_MAGIC
 * when it is neither a PackFile nor an ARC20 archive.  Entry and archive
 * names are lower-cased so that later lookups are case-insensitive.  A node
 * that already holds an index returns 0 without reading.
 */
static uint32_t FileSet_LoadIndex(FileSet_t* s, const char* path)
{
	File_t f;
	uint8_t hdr[16];
	uint32_t r = 0;
	int i;

	if(s->loaded)
		return 0;
	File_Ctor(&f);
	if(!File_OpenRead(&f, path))
	{
		r = ARC_ERR_INDEX_OPEN;
	}
	else if(File_Read(&f, hdr, 16) != 16 || (memcmp(hdr, PACKFILE_MAGIC, 12) != 0 && memcmp(hdr, ARC20_MAGIC, 12) != 0))
	{
		r = ARC_ERR_INDEX_MAGIC;
		File_Close(&f);
	}
	else
	{
		// the two index formats are converted to ArcEntry_t on loading; the
		// entry count follows the 12-byte magic
		int arc20 = memcmp(hdr, ARC20_MAGIC, 12) == 0;
		uint32_t recSize = arc20 ? 0x80 : 32;
		uint8_t* raw;
		memcpy(&s->count, hdr + 12, 4);
		// entry offsets count from the end of the index
		s->dataBase = (uint32_t)s->count * recSize + 16;
		raw = (uint8_t*)BGI_Alloc((size_t)s->count * recSize + 1);
		File_Read(&f, raw, (uint32_t)s->count * recSize);
		s->entries = (ArcEntry_t*)BGI_Calloc((size_t)(s->count ? s->count : 1) * sizeof(ArcEntry_t));
		for(i = 0; i < s->count; i++)
		{
			const uint8_t* rec = raw + (size_t)i * recSize;
			if(arc20)
			{
				memcpy(s->entries[i].name, rec, 0x60);
				// a name that fills all 0x60 bytes loses its last character
				s->entries[i].name[0x5f] = 0;
				memcpy(&s->entries[i].offset, rec + 0x60, 4);
				memcpy(&s->entries[i].size, rec + 0x64, 4);
			}
			else
			{
				memcpy(s->entries[i].name, rec, 16);
				s->entries[i].name[16] = 0;
				memcpy(&s->entries[i].offset, rec + 16, 4);
				memcpy(&s->entries[i].size, rec + 20, 4);
			}
			SjisStrLwr(s->entries[i].name);
		}
		BGI_Free(raw);
		strcpy(s->name, path);
		SjisStrLwr(s->name);
		s->loaded = 1;
		File_Close(&f);
	}
	File_Dtor(&f);
	return r;
}

/* is this node the archive at `path`?  A node without an index adopts the
 * path (loads it) and matches when that succeeded; a loaded node matches
 * when the lower-cased paths are equal. */
static int FileSet_Matches(FileSet_t* s, const char* path)
{
	char buf[0x104];
	if(!s->loaded)
		return FileSet_LoadIndex(s, path) == 0;
	strcpy(buf, path);
	SjisStrLwr(buf);
	return strcmp(buf, s->name) == 0;
}

// 1 when the archive has an entry of that name (case-insensitive)
static int FileSet_Contains(FileSet_t* s, const char* name)
{
	char buf[0x104];
	int i;
	strcpy(buf, name);
	SjisStrLwr(buf);
	for(i = 0; i < s->count; i++)
		if(strcmp(s->entries[i].name, buf) == 0)
			return 1;
	return 0;
}

// copies the archive name to dst (when given) if `name` exists; 1 when it does
static int FileSet_ArcNameFor(FileSet_t* s, char* dst, const char* name)
{
	if(s->vt->contains(s, name) && dst)
		strcpy(dst, s->name);
	return s->vt->contains(s, name);
}

/*
 * Read `size` bytes from offset `off` of the entry `name` into dst; size 0
 * means "the whole entry".  Returns the number of bytes read, or
 * ARC_ERR_NOT_FOUND (no such entry), ARC_ERR_OPEN (the archive file cannot
 * be opened), ARC_ERR_OFFSET (size larger than the entry) or ARC_ERR_RANGE
 * (off + size beyond the entry).  With dst == NULL nothing is read and the
 * size that would have been read is returned.
 */
static uint32_t FileSet_ReadRange(FileSet_t* s, void* dst, const char* name, uint32_t off, uint32_t size)
{
	char buf[0x104];
	int i;
	File_t f;
	uint32_t r;
	ArcEntry_t* e;

	strcpy(buf, name);
	SjisStrLwr(buf);
	for(i = 0; i < s->count; i++)
		if(strcmp(s->entries[i].name, buf) == 0)
			break;
	if(i >= s->count)
		return ARC_ERR_NOT_FOUND;
	e = &s->entries[i];

	File_Ctor(&f);
	if(!File_OpenRead(&f, s->name))
	{
		r = ARC_ERR_OPEN;
	}
	else
	{
		if(size == 0)
			size = e->size;
		if(size > e->size)
		{
			r = ARC_ERR_OFFSET;
		}
		else if(off + size > e->size)
		{
			r = ARC_ERR_RANGE;
		}
		else if(!dst)
		{
			r = size; // a size query (the 1.588+ loaders pass no buffer for one)
		}
		else
		{
			File_Seek(&f, s->dataBase + e->offset + off);
			r = File_Read(&f, dst, size);
		}
		File_Close(&f);
	}
	File_Dtor(&f);
	return r;
}

// the stored size of the entry `name`, or ARC_ERR_NOT_FOUND
static uint32_t FileSet_SizeOf(FileSet_t* s, const char* name)
{
	char buf[0x104];
	int i;
	strcpy(buf, name);
	SjisStrLwr(buf);
	for(i = 0; i < s->count; i++)
		if(strcmp(s->entries[i].name, buf) == 0)
			return s->entries[i].size;
	return ARC_ERR_NOT_FOUND;
}

static const FileSetVtbl_t FileSet_Vtbl = {
	FileSet_Destroy, FileSet_Contains, FileSet_ArcNameFor,
	FileSet_ReadRange, FileSet_SizeOf, FileSet_Matches};

// -------------------------------------------------------------------------
// Archive manager chain
// -------------------------------------------------------------------------

/* the node after `s`, created on demand once `s` holds an index; NULL when
 * `s` is still empty (it will adopt the next path itself) */
static FileSet_t* ArcMgr_NextNode(FileSet_t* s)
{
	if(s->next)
		return s->next;
	if(!s->loaded)
		return NULL;
	s->next = FileSet_New();
	return s->next;
}

// 1 when the archive `arc` has an entry `name`; 0 when it has not or cannot be opened
int ArcMgr_Exists(FileSet_t* mgr, const char* arc, const char* name)
{
	FileSet_t* n;
	if(mgr->vt->matches(mgr, arc))
		return mgr->vt->contains(mgr, name);
	n = ArcMgr_NextNode(mgr);
	return n ? ArcMgr_Exists(n, arc, name) : 0;
}

/* copy the lower-cased path of the archive file that holds `name` to dst
 * (for a list archive that is the member, not the list name); 1 when found.
 * With dst == NULL only the existence check is made. */
int ArcMgr_ArcNameFor(FileSet_t* mgr, char* dst, const char* arc, const char* name)
{
	FileSet_t* n;
	if(!dst)
		return ArcMgr_Exists(mgr, arc, name);
	if(mgr->vt->matches(mgr, arc))
		return mgr->vt->arcNameFor(mgr, dst, name);
	n = ArcMgr_NextNode(mgr);
	return n ? ArcMgr_ArcNameFor(n, dst, arc, name) : 0;
}

// read the whole entry; see ArcMgr_ReadRange for the result
uint32_t ArcMgr_Read(FileSet_t* mgr, void* dst, const char* arc, const char* name)
{
	return ArcMgr_ReadRange(mgr, dst, arc, name, 0, 0);
}

/* read a range of the entry `name` of the archive `arc` (size 0: the whole
 * entry; dst NULL: only report the size).  Returns the bytes read or one
 * of the ARC_ERR_* codes of FileSet_ReadRange; ARC_ERR_OPEN also when the
 * archive itself cannot be opened or is not an archive. */
uint32_t ArcMgr_ReadRange(FileSet_t* mgr, void* dst, const char* arc, const char* name, uint32_t off, uint32_t size)
{
	FileSet_t* n;
	if(mgr->vt->matches(mgr, arc))
		return mgr->vt->readRange(mgr, dst, name, off, size);
	n = ArcMgr_NextNode(mgr);
	return n ? ArcMgr_ReadRange(n, dst, arc, name, off, size) : ARC_ERR_OPEN;
}

/* the stored size of the entry; ARC_ERR_NOT_FOUND, or ARC_ERR_OPEN when the
 * archive cannot be opened */
uint32_t ArcMgr_SizeOf(FileSet_t* mgr, const char* arc, const char* name)
{
	FileSet_t* n;
	if(mgr->vt->matches(mgr, arc))
		return mgr->vt->sizeOf(mgr, name);
	n = ArcMgr_NextNode(mgr);
	return n ? ArcMgr_SizeOf(n, arc, name) : ARC_ERR_OPEN;
}

/* append `other` to the chain unless a node of that name exists; returns 1
 * when appended (the chain owns `other` from then on), 0 when a node of the
 * same name is already there (`other` is not taken over) */
int ArcMgr_Register(FileSet_t* mgr, FileSet_t* other)
{
	char buf[0x104];
	FileSet_CopyName(other, buf);
	if(strcmp(buf, mgr->name) == 0)
		return 0;
	if(mgr->next)
		return ArcMgr_Register(mgr->next, other);
	mgr->next = other;
	return 1;
}

// -------------------------------------------------------------------------
// FileSetList: a name that stands for a list of archive files
// -------------------------------------------------------------------------

static const FileSetVtbl_t FileSetList_Vtbl;

// an empty list archive; FileSetList_SetName and FileSetList_SetList fill it
FileSet_t* FileSetList_New(void)
{
	FileSet_t* s = (FileSet_t*)BGI_Calloc(sizeof(FileSet_t));
	FileSet_BaseCtor(s);
	s->vt = &FileSetList_Vtbl;
	s->listCount = 0;
	s->list = NULL;
	s->inner = FileSet_New();
	return s;
}

// the virtual destructor: frees the member names, the private chain and the base part
static void FileSetList_Destroy(FileSet_t* s)
{
	int i;
	s->vt = &FileSetList_Vtbl;
	for(i = 0; i < s->listCount; i++)
		BGI_Free(s->list[i]);
	BGI_Free(s->list);
	if(s->inner)
		s->inner->vt->destroy(s->inner);
	s->vt = &FileSet_Vtbl;
	FileSet_BaseDtor(s);
	BGI_Free(s);
}

// the pseudo archive name the list answers to (lower-cased); 0 for NULL
int FileSetList_SetName(FileSet_t* s, const char* name)
{
	if(!name)
		return 0;
	strcpy(s->name, name);
	SjisStrLwr(s->name);
	s->loaded = 1;
	return 1;
}

/* the member archive file names, a NULL-terminated array that is copied
 * (lower-cased); 0 for NULL or an empty array */
int FileSetList_SetList(FileSet_t* s, char** names)
{
	int i;
	if(!names)
		return 0;
	s->listCount = 0;
	while(names[s->listCount])
		s->listCount++;
	if(s->listCount <= 0)
		return 0;
	BGI_Free(s->list);
	s->list = (char**)BGI_Alloc(sizeof(char*) * s->listCount);
	for(i = 0; names[i]; i++)
	{
		s->list[i] = BGI_Strdup(names[i]);
		SjisStrLwr(s->list[i]);
	}
	return 1;
}

// directory part of a path (without the separator); 0 when there is none
static int Path_DirName(char* dst, const char* path)
{
	char* p;
	strcpy(dst, path);
	p = strrchr(dst, '\\');
	if(!p)
		return 0;
	*p = 0;
	return 1;
}

// file-name part of a path; 0 when the path has no directory part
static int Path_BaseName(char* dst, const char* path)
{
	const char* p = strrchr(path, '\\');
	if(!p)
		return 0;
	strcpy(dst, p + 1);
	return 1;
}

/* the list matches a path whose file name equals the list name (a bare name
 * without a directory never matches); the directory of that path becomes
 * the place where the members live */
static int FileSetList_Matches(FileSet_t* s, const char* path)
{
	char base[0x104];
	int m;
	if(!Path_BaseName(base, path))
		return 0;
	SjisStrLwr(base);
	m = strcmp(base, s->name) == 0;
	if(m)
	{
		Path_DirName(s->dir, path);
		SjisStrLwr(s->dir);
	}
	return m;
}

/* search the members in order; the path of the first one that has `name`
 * goes to dst (when given).  1 when found. */
static int FileSetList_ArcNameFor(FileSet_t* s, char* dst, const char* name)
{
	char path[0x104];
	int i;
	for(i = 0; i < s->listCount; i++)
	{
		sprintf(path, "%s\\%s", s->dir, s->list[i]);
		if(ArcMgr_Exists(s->inner, path, name))
		{
			if(dst)
				strcpy(dst, path);
			return 1;
		}
	}
	return 0;
}

// 1 when any member has the entry
static int FileSetList_Contains(FileSet_t* s, const char* name)
{
	return s->vt->arcNameFor(s, NULL, name);
}

/* read from the first member that has the file; a hard error of a member
 * (the range errors) is returned at once, a missing entry or archive moves
 * on to the next member.  Returns as ArcMgr_ReadRange does. */
static uint32_t FileSetList_ReadRange(FileSet_t* s, void* dst, const char* name, uint32_t off, uint32_t size)
{
	char path[0x104];
	uint32_t r = ARC_ERR_OPEN; // the result for an empty list (uninitialised in the original)
	int i;
	for(i = 0; i < s->listCount; i++)
	{
		sprintf(path, "%s\\%s", s->dir, s->list[i]);
		r = ArcMgr_ReadRange(s->inner, dst, path, name, off, size);
		if(r < 0x80000000u || r == ARC_ERR_RANGE || r == ARC_ERR_OFFSET)
			return r;
	}
	return r;
}

/* the size from the first member that has the file; otherwise the last
 * member's error code (0x80000000 for an empty list) */
static uint32_t FileSetList_SizeOf(FileSet_t* s, const char* name)
{
	char path[0x104];
	uint32_t r = 0x80000000u;
	int i;
	for(i = 0; i < s->listCount; i++)
	{
		sprintf(path, "%s\\%s", s->dir, s->list[i]);
		r = ArcMgr_SizeOf(s->inner, path, name);
		if(r < 0x80000000u)
			break;
	}
	return r;
}

static const FileSetVtbl_t FileSetList_Vtbl = {
	FileSetList_Destroy, FileSetList_Contains, FileSetList_ArcNameFor,
	FileSetList_ReadRange, FileSetList_SizeOf, FileSetList_Matches};

/* "80 38": register the pseudo archive `name` that stands for the
 * NULL-terminated `list` of archive files.  1 when registered, 0 when the
 * name or the list is empty or a node of that name already exists (the new
 * object is destroyed again then). */
int Sys_RegisterFilelist(const char* name, char** list)
{
	FileSet_t* s = FileSetList_New();
	int ok = 0;
	if(FileSetList_SetName(s, name) && FileSetList_SetList(s, list))
		ok = ArcMgr_Register(gArcMgr, s);
	if(!ok)
		s->vt->destroy(s);
	return ok;
}

// -------------------------------------------------------------------------
// FileCache
// -------------------------------------------------------------------------

typedef struct CacheEntry // one cached file
{
	char* arc;               // archive path, NULL for a loose file
	char* name;              // entry / file name
	uint8_t* data;           // a copy of the file
	uint32_t size;           // bytes
	uint32_t time;           // last use (ms, OS_TicksMs)
	struct CacheEntry* next; // next entry; the list is kept most recently used first
} CacheEntry_t;

struct FileCache
{
	uint32_t capacity;  // bytes of file data the cache may hold
	uint32_t used;      // bytes held
	uint32_t half;      // capacity / 2: the largest file accepted
	CacheEntry_t* head; // most recently used entry first
};

FileCache_t* gFileCache; // the loader's cache, NULL until "90 03" creates it

// a cache that holds up to `capacity` bytes of file data
FileCache_t* FileCache_New(uint32_t capacity)
{
	FileCache_t* c = (FileCache_t*)BGI_Calloc(sizeof(FileCache_t));
	c->capacity = capacity;
	c->used = 0;
	c->half = capacity >> 1;
	c->head = NULL;
	return c;
}

// unlink and free one entry; 1 if found
static int FileCache_Remove(FileCache_t* c, CacheEntry_t* e)
{
	CacheEntry_t **link = &c->head, *n = c->head;
	while(n && n != e)
	{
		link = &n->next;
		n = n->next;
	}
	if(!n)
		return 0;
	*link = n->next;
	c->used -= n->size;
	BGI_Free(n->arc);
	BGI_Free(n->name);
	BGI_Free(n->data);
	BGI_Free(n);
	return 1;
}

// the least recently used entry (by time stamp), NULL when empty
static CacheEntry_t* FileCache_Oldest(FileCache_t* c)
{
	CacheEntry_t *best = NULL, *n;
	uint32_t t = 0xffffffffu;
	for(n = c->head; n; n = n->next)
		if(n->time < t)
		{
			t = n->time;
			best = n;
		}
	return best;
}

// free every entry and the cache itself
void FileCache_Delete(FileCache_t* c)
{
	while(FileCache_Remove(c, c->head))
		;
	BGI_Free(c);
}

/* store a copy of `data` under (arc, name) at the front; the least recently
 * used entries are evicted until it fits.  0 when the file is empty or
 * larger than half the capacity, 1 when stored. */
int FileCache_Add(FileCache_t* c, const char* arc, const char* name, const void* data, uint32_t size)
{
	CacheEntry_t* e;
	if(size == 0 || size > c->half)
		return 0;
	while(c->used + size > c->capacity)
		FileCache_Remove(c, FileCache_Oldest(c));
	e = (CacheEntry_t*)BGI_Alloc(sizeof(CacheEntry_t));
	e->arc = arc ? BGI_Strdup(arc) : NULL;
	e->name = name ? BGI_Strdup(name) : NULL;
	e->data = (uint8_t*)BGI_Alloc(size);
	memcpy(e->data, data, size);
	e->size = size;
	e->time = OS_TicksMs();
	e->next = c->head;
	c->head = e;
	c->used += size;
	return 1;
}

// string equality where NULL only equals NULL
static int CacheStrEq(const char* a, const char* b)
{
	if(!a)
		return b == NULL;
	if(!b)
		return 0;
	return strcmp(a, b) == 0;
}

/* look up (arc, name), compared exactly (not lower-cased); on a hit copy the
 * data to dst, report its size in *size, move the entry to the front and
 * refresh its time stamp.  1 on a hit, 0 otherwise. */
int FileCache_Lookup(FileCache_t* c, void* dst, uint32_t* size, const char* arc, const char* name)
{
	CacheEntry_t **link = &c->head, *e = c->head;
	while(e)
	{
		if(CacheStrEq(arc, e->arc) && CacheStrEq(name, e->name))
			break;
		link = &e->next;
		e = e->next;
	}
	if(!e)
		return 0;
	memcpy(dst, e->data, e->size);
	*size = e->size;
	*link = e->next;
	e->next = c->head;
	c->head = e;
	e->time = OS_TicksMs();
	return 1;
}
