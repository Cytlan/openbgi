/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * source.c - where a BW stream reads its bytes from: a file, an entry of
 *            an archive, or a block of memory (inc/bgi/snd/bw.h)
 *
 * A source is a window [base, base + size) of a file with its own
 * position; the memory kind holds a copy of the data.  Whole-file streams
 * decode into a memory source at open and read the PCM back from it.
 */
#include "bw_internal.h"

// a loose file opened for shared reading; NULL when it cannot be opened
WmSource_t* WmSource_OpenFile(const char* path)
{
	WmSource_t* s;
	OsFile_t* f = OS_FileOpenRead(path);
	if(!f)
		return NULL;
	s = (WmSource_t*)BGI_Alloc(sizeof *s);
	memset(s, 0, sizeof *s);
	s->kind = WM_SRC_FILE;
	s->file = f;
	s->size = OS_FileSize(f);
	return s;
}

/* The entry `name` of the archive at `arcPath`: a "PackFile" (a 16-byte
 * header - the magic and the entry count - then 32 bytes per entry: a
 * 16-byte name, the offset and the size) or, from 1.573 on, a "BURIKO
 * ARC20" (the same header, then 0x80 bytes per entry: a 0x60-byte name,
 * the offset at +0x60 and the size at +0x64), then the data.  Names are
 * compared lower-cased, as the file layer compares them (src/core/arc.c).
 * The original keeps a list of opened archives; here the index is
 * scanned per open.  NULL when the archive cannot be opened, is neither
 * format, or lacks the entry. */
WmSource_t* WmSource_OpenArc(const char* arcPath, const char* name)
{
	WmSource_t* s;
	OsFile_t* f;
	uint8_t hdr[16];
	uint32_t count, i, recSize, nameSize;
	char want[0x60];
	int found = 0;
	uint32_t off = 0, size = 0;

	if(OS_FileAttrs(arcPath) == OS_INVALID_ATTRS)
		return NULL;
	f = OS_FileOpenRead(arcPath);
	if(!f)
		return NULL;
	if(OS_FileRead(f, hdr, 16) != 16)
	{
		OS_FileClose(f);
		return NULL;
	}
	if(memcmp(hdr, "PackFile    ", 12) == 0)
	{
		recSize = 32;
		nameSize = 16;
	}
	else if(memcmp(hdr, "BURIKO ARC20", 12) == 0)
	{
		recSize = 0x80;
		nameSize = 0x60;
	}
	else
	{
		OS_FileClose(f);
		return NULL;
	}
	memcpy(&count, hdr + 12, 4);
	// a name that fills its field loses its last character, as in the file layer
	for(i = 0; i < nameSize - 1 && name[i]; i++)
		want[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
	want[i] = 0;
	for(i = 0; i < count && !found; i++)
	{
		uint8_t e[0x80];
		char nm[0x60];
		uint32_t k;
		if(OS_FileRead(f, e, recSize) != recSize)
			break;
		memcpy(nm, e, nameSize);
		nm[nameSize - 1] = 0;
		for(k = 0; nm[k]; k++)
			if(nm[k] >= 'A' && nm[k] <= 'Z')
				nm[k] = (char)(nm[k] + 32);
		if(strcmp(nm, want) == 0)
		{
			memcpy(&off, e + nameSize, 4);
			memcpy(&size, e + nameSize + 4, 4);
			found = 1;
		}
	}
	if(!found)
	{
		OS_FileClose(f);
		return NULL;
	}
	s = (WmSource_t*)BGI_Alloc(sizeof *s);
	memset(s, 0, sizeof *s);
	s->kind = WM_SRC_ARC;
	s->file = f;
	s->base = 16 + count * recSize + off; // entry offsets count from the end of the index
	s->size = size;
	OS_FileSeek(f, s->base);
	return s;
}

// a source holding a copy of `size` bytes of `data` (NULL: zeroed, to be written by the decoder)
WmSource_t* WmSource_FromMemory(const void* data, uint32_t size)
{
	WmSource_t* s = (WmSource_t*)BGI_Alloc(sizeof *s);
	memset(s, 0, sizeof *s);
	s->kind = WM_SRC_MEMORY;
	s->mem = (uint8_t*)BGI_Alloc(size ? size : 1);
	if(data)
		memcpy(s->mem, data, size);
	s->size = size;
	return s;
}

void WmSource_Close(WmSource_t* s)
{
	if(!s)
		return;
	if(s->file)
		OS_FileClose(s->file);
	BGI_Free(s->mem);
	BGI_Free(s);
}

uint32_t WmSource_Size(const WmSource_t* s)
{
	return s->size;
}

// up to `n` bytes from the position into dst; the bytes read (fewer at the end)
uint32_t WmSource_Read(WmSource_t* s, void* dst, uint32_t n)
{
	uint32_t got;
	if(s->pos >= s->size)
		return 0;
	if(n > s->size - s->pos)
		n = s->size - s->pos;
	if(s->kind == WM_SRC_MEMORY)
	{
		memcpy(dst, s->mem + s->pos, n);
		got = n;
	}
	else
		got = OS_FileRead(s->file, dst, n);
	s->pos += got;
	return got;
}

// move the position to `pos` (relative to the source, clamped to its size)
void WmSource_Seek(WmSource_t* s, uint32_t pos)
{
	if(pos > s->size)
		pos = s->size;
	s->pos = pos;
	if(s->kind != WM_SRC_MEMORY)
		OS_FileSeek(s->file, s->base + pos);
}

uint32_t WmSource_Tell(const WmSource_t* s)
{
	return s->pos;
}
