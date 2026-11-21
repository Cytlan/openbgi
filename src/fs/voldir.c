/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * voldir.c - a folder of the host as a volume (src/include/bgi/vol.h)
 *
 * The files and sub-folders of a native folder as they are when it is
 * opened, their names turned into UTF-8.  Every file is an entry kept in
 * a file of its own: it is read from there, whole or in place, and has no
 * check value.
 */
#include "vol_internal.h"

#define FOLDER_DEPTH 32 // folders inside folders

typedef struct FolderWalk
{
	OpenBGI_t* o;
	VolList_t* l;
	const char* native; // the native path of the folder being listed, no trailing separator
	const char* prefix; // its path in the archive ("" for the root)
	int depth;
	int ok; // 0 once memory ran out
} FolderWalk_t;

static void FolderEntry(void* ctx, const char* name, int isDir);

// the entries of one folder
static void FolderList(OpenBGI_t* o, VolList_t* l, const char* native, const char* prefix, int depth, int* ok)
{
	FolderWalk_t w;
	if(!*ok || depth > FOLDER_DEPTH || VolList_Count(l) >= VOL_MAX_ENTRIES)
		return;
	w.o = o;
	w.l = l;
	w.native = native;
	w.prefix = prefix;
	w.depth = depth;
	w.ok = 1;
	Host_NativeList(o, native, FolderEntry, &w);
	if(!w.ok)
		*ok = 0;
}

static void FolderEntry(void* ctx, const char* name, int isDir)
{
	FolderWalk_t* w = (FolderWalk_t*)ctx;
	char utf[VOL_NAME_MAX], path[VOL_NAME_MAX];
	OpenBGI_NativeStat_t st;
	VolEntry_t e;
	size_t len = strlen(w->native) + strlen(name) + 2;
	char* native;
	if(!w->ok)
		return;
	Host_NativeToUtf8(w->o, name, utf, sizeof utf);
	if(!utf[0] || strchr(utf, '/') || (size_t)snprintf(path, sizeof path, "%s%s%s", w->prefix, w->prefix[0] ? "/" : "", utf) >= sizeof path)
		return;
	native = (char*)malloc(len);
	if(!native)
	{
		w->ok = 0;
		return;
	}
	snprintf(native, len, "%s%c%s", w->native, Host_NativeSep(w->o), name);
	memset(&e, 0, sizeof e);
	memset(&st, 0, sizeof st);
	if(isDir)
	{
		e.isDir = 1;
		if(!VolList_Add(w->l, path, &e))
			w->ok = 0;
		else
			FolderList(w->o, w->l, native, path, w->depth + 1, &w->ok);
		free(native);
		return;
	}
	if(Host_NativeStat(w->o, native, &st) != OPENBGI_NATIVE_FILE)
	{
		free(native);
		return;
	}
	e.size = e.csize = st.size;
	e.readable = 1;
	e.stored = 1;
	if(st.mtime > -INT64_C(11644473600000))
		e.mtime = (uint64_t)(st.mtime + INT64_C(11644473600000)) * 10000u; // milliseconds since 1970 as a FILETIME
	e.file = native;
	if(!VolList_Add(w->l, path, &e))
	{
		free(native);
		w->ok = 0;
	}
}
int VolDir_Read(OpenBGI_t* o, const char* native, VolList_t* l)
{
	int ok = 1;
	FolderList(o, l, native, "", 0, &ok);
	return ok;
}

// ---- the bytes of an entry -------------------------------------------------------------------------

// the entry's own file as a volume's
static int OpenOwn(Vol_t* v, const VolEntry_t* e, VolIn_t* own)
{
	own->o = Vol_In(v)->o;
	own->f = Host_NativeOpen(own->o, e->file, OPENBGI_OPEN_READ);
	return own->f != NULL;
}

static uint8_t* DirLoad(Vol_t* v, const VolEntry_t* e, size_t* size)
{
	VolIn_t own;
	uint8_t* d = OpenOwn(v, e, &own) ? VolIn_Load(&own, 0, e->size) : NULL;
	Host_FileClose(own.o, own.f);
	if(d)
		*size = (size_t)e->size;
	return d;
}

static int DirExtract(Vol_t* v, const VolEntry_t* e, VolWrite_t write, void* ctx)
{
	VolIn_t own;
	int ok = OpenOwn(v, e, &own) && VolIn_Copy(&own, 0, e->size, write, ctx);
	Host_FileClose(own.o, own.f);
	return ok;
}

static HostFile_t* DirOpenStored(Vol_t* v, const VolEntry_t* e, uint64_t* offset)
{
	VolIn_t own;
	if(OpenOwn(v, e, &own))
		*offset = 0;
	return own.f;
}

const VolOrigin_t kVolDirOrigin = {DirLoad, DirExtract, DirOpenStored};
