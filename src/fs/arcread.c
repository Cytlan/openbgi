/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * arcread.c - the stand-alone archive reader of the tools; interface in
 *             arcread.h
 *
 * Reads a PackFile or BURIKO ARC20 index with stdio (the tools), through
 * the host's native files (ArcRead_OpenNative) or through the source the
 * caller gives (ArcRead_OpenReader), and hands out entries, removing the
 * DSC layer on request.  Independent of the engine's file layer (file.h)
 * so that the tools and the extraction options can use it without a game;
 * only the DSC decoder of codec.h is shared.
 */
#include "bgi/arcread.h"
#include "bgi/host.h"
#include "bgi/codec.h"

#include <limits.h>

// the two index magics (12 bytes each), repeated here so that file.h is not needed
#define PACKFILE_MAGIC "PackFile    "
#define ARC20_MAGIC    "BURIKO ARC20"

// open the archive at `path` with stdio and read its index (ArcRead_OpenFile); NULL with a message on stderr when the file cannot be opened
ArcRead_t* ArcRead_Open(const char* path)
{
	FILE* f = fopen(path, "rb");
	if(!f)
	{
		fprintf(stderr, "%s: cannot open\n", path);
		return NULL;
	}
	return ArcRead_OpenFile(f, path);
}

/* a stdio stream at `off` from its start, also where `long` has 32 bits
 * and the offset does not fit in one */
static int SeekFile(FILE* f, uint64_t off)
{
	if(fseek(f, 0, SEEK_SET) != 0)
		return 0;
	while(off > (uint64_t)LONG_MAX)
	{
		if(fseek(f, LONG_MAX, SEEK_CUR) != 0)
			return 0;
		off -= (uint64_t)LONG_MAX;
	}
	return fseek(f, (long)off, SEEK_CUR) == 0;
}

int ArcRead_ReadAt(const ArcRead_t* a, uint32_t off, void* buf, uint32_t n)
{
	if(a->f)
		return SeekFile(a->f, off) && fread(buf, 1, n, a->f) == n;
	return a->src.read(a->src.ctx, off, buf, n);
}

/* read the index of the archive `a` reads from (called `path` in the
 * messages): the 16-byte header (magic, entry count), then one record per
 * entry, converted to ArcReadEntry_t with an absolute offset.  NULL with a
 * message on stderr - and the archive closed - when it is not an archive or
 * its index is truncated. */
static ArcRead_t* ReadIndex(ArcRead_t* a, const char* path)
{
	uint8_t hdr[16];
	uint32_t i, recSize, nameLen, base;
	uint8_t* raw;
	if(!ArcRead_ReadAt(a, 0, hdr, 16) || (memcmp(hdr, PACKFILE_MAGIC, 12) != 0 && memcmp(hdr, ARC20_MAGIC, 12) != 0))
	{
		fprintf(stderr, "%s: not a PackFile / ARC20 archive\n", path);
		ArcRead_Close(a);
		return NULL;
	}
	a->arc20 = memcmp(hdr, ARC20_MAGIC, 12) == 0;
	a->count = hdr[12] | (hdr[13] << 8) | (hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
	recSize = a->arc20 ? 0x80 : 32; // record: name, offset, size (and padding)
	nameLen = a->arc20 ? 0x60 : 16;
	base = 16 + a->count * recSize; // the stored offsets count from the end of the index
	if((uint64_t)a->count * recSize > 0x7fffffffu)
		a->count = 0x7fffffffu / recSize + 1; // more than any file holds: the read below fails
	raw = (uint8_t*)malloc((size_t)a->count * recSize + 1);
	a->entries = (ArcReadEntry_t*)calloc(a->count + 1, sizeof *a->entries);
	if(!raw || !a->entries || !ArcRead_ReadAt(a, 16, raw, a->count * recSize))
	{
		fprintf(stderr, "%s: truncated index\n", path);
		free(raw);
		ArcRead_Close(a);
		return NULL;
	}
	for(i = 0; i < a->count; i++)
	{
		const uint8_t* rec = raw + (size_t)i * recSize;
		uint32_t off, size;
		memcpy(a->entries[i].name, rec, nameLen);
		a->entries[i].name[nameLen] = 0;
		memcpy(&off, rec + nameLen, 4);
		memcpy(&size, rec + nameLen + 4, 4);
		a->entries[i].offset = base + off;
		a->entries[i].size = size;
	}
	free(raw);
	return a;
}

ArcRead_t* ArcRead_OpenFile(FILE* f, const char* path)
{
	ArcRead_t* a = (ArcRead_t*)calloc(1, sizeof *a);
	if(!a)
	{
		fclose(f);
		return NULL;
	}
	a->f = f;
	return ReadIndex(a, path);
}

// an archive of the host's native files
typedef struct NativeArc
{
	OpenBGI_t* o;
	HostFile_t* f;
} NativeArc_t;

static int NativeRead(void* ctx, uint32_t off, void* buf, uint32_t n)
{
	NativeArc_t* a = (NativeArc_t*)ctx;
	return Host_FileSeek(a->o, a->f, off) && Host_FileRead(a->o, a->f, buf, n) == n;
}

static void NativeClose(void* ctx)
{
	NativeArc_t* a = (NativeArc_t*)ctx;
	Host_FileClose(a->o, a->f);
	free(a);
}

ArcRead_t* ArcRead_OpenNative(OpenBGI_t* o, const char* path)
{
	ArcSource_t src;
	NativeArc_t* a = (NativeArc_t*)calloc(1, sizeof *a);
	if(!a)
		return NULL;
	a->o = o;
	a->f = Host_NativeOpen(o, path, OPENBGI_OPEN_READ);
	if(!a->f)
	{
		fprintf(stderr, "%s: cannot open\n", path);
		free(a);
		return NULL;
	}
	src.read = NativeRead;
	src.close = NativeClose;
	src.ctx = a;
	return ArcRead_OpenReader(&src, path);
}

ArcRead_t* ArcRead_OpenReader(const ArcSource_t* src, const char* path)
{
	ArcRead_t* a = (ArcRead_t*)calloc(1, sizeof *a);
	if(!a)
	{
		src->close(src->ctx);
		return NULL;
	}
	a->src = *src;
	return ReadIndex(a, path);
}

// close the file and free the index; NULL is allowed
void ArcRead_Close(ArcRead_t* a)
{
	if(!a)
		return;
	if(a->f)
		fclose(a->f);
	else if(a->src.close)
		a->src.close(a->src.ctx);
	free(a->entries);
	free(a);
}

// string equality ignoring the case of ASCII letters (Shift-JIS bytes compare as they are)
static int EqualNoCase(const char* a, const char* b)
{
	for(; *a && *b; a++, b++)
	{
		int x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
		int y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
		if(x != y)
			return 0;
	}
	return *a == *b;
}

// the first entry whose name equals `name` ignoring ASCII case, or NULL
const ArcReadEntry_t* ArcRead_Find(const ArcRead_t* a, const char* name)
{
	uint32_t i;
	for(i = 0; i < a->count; i++)
		if(EqualNoCase(a->entries[i].name, name))
			return &a->entries[i];
	return NULL;
}

/* read the entry into a new buffer (BGI_Free it) and decode it when it is a
 * DSC stream, in which case *wasDsc is set and *size is the decoded size;
 * otherwise *size is the stored size.  The buffer has one spare byte after
 * the data.  NULL when the file cannot be read. */
uint8_t* ArcRead_Load(OpenBGI_t* o, const ArcRead_t* a, const ArcReadEntry_t* e, uint32_t* size, int* wasDsc)
{
	uint8_t* raw = (uint8_t*)BGI_Alloc(e->size + 1);
	uint8_t* out;
	*wasDsc = 0;
	if(!ArcRead_ReadAt(a, e->offset, raw, e->size))
	{
		BGI_Free(raw);
		return NULL;
	}
	if(e->size >= sizeof(DscHeader_t) && BGI_IsDsc(raw))
	{
		uint32_t n = BGI_DscOutSize(raw);
		out = (uint8_t*)BGI_Alloc(n + 1);
		*size = BGI_DscDecode(o, raw, out);
		BGI_Free(raw);
		*wasDsc = 1;
		return out;
	}
	*size = e->size;
	return raw;
}
