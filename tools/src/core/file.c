/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * file.c - the File_t wrapper over the OS layer's files and the reading
 *          and writing of loose files (inc/bgi/file.h)
 *
 * A loose file is one on disk rather than in an archive.  ReadFileEx
 * resolves a name the way the engine does everywhere: an absolute name as
 * it is, else directory + name, then - with the search list of "80 36"
 * on - directory + each registered sub-directory + name.
 */
#include "bgi/file.h"
#include "bgi/strutil.h"
#include "bgi/os.h"

// ---- File_t -------------------------------------------------------------------------------

void File_Ctor(File_t* f)
{
	f->h = OS_INVALID_FILE;
}

void File_Dtor(File_t* f)
{
	File_Close(f);
}

// Open for reading; 0 when the file is already open or cannot be opened.
int File_OpenRead(File_t* f, const char* path)
{
	if(f->h != OS_INVALID_FILE)
		return 0;
	f->h = OS_FileOpenRead(path);
	return f->h != OS_INVALID_FILE;
}

// Create or truncate for writing; 0 when the file is already open or cannot be created.
int File_Create(File_t* f, const char* path)
{
	if(f->h != OS_INVALID_FILE)
		return 0;
	f->h = OS_FileCreate(path);
	return f->h != OS_INVALID_FILE;
}

void File_Close(File_t* f)
{
	if(f->h != OS_INVALID_FILE)
	{
		OS_FileClose(f->h);
		f->h = OS_INVALID_FILE;
	}
}

uint32_t File_Read(File_t* f, void* buf, uint32_t n)
{
	return OS_FileRead(f->h, buf, n);
}

uint32_t File_Write(File_t* f, const void* buf, uint32_t n)
{
	return OS_FileWrite(f->h, buf, n);
}

int File_Seek(File_t* f, uint32_t pos)
{
	return OS_FileSeek(f->h, pos);
}

uint32_t File_Size(File_t* f)
{
	return OS_FileSize(f->h);
}

// ---- loose files ----------------------------------------------------------------------------

/* Read [off, off + size) of a file (off = size = 0: all of it) into buf,
 * or - with buf NULL - only report the size.  0 ok, 1 cannot open (or
 * the drive is not ready), 2 the range reaches past the end, 3 the size
 * exceeds the file, -1 short read; *outSize receives the byte count. */
static int ReadFileRange(void* buf, uint32_t* outSize, const char* path, uint32_t off, uint32_t size)
{
	File_t f;
	int r;
	uint32_t total;

	File_Ctor(&f);
	if(!DriveReady(path) || !File_OpenRead(&f, path))
	{
		r = 1;
	}
	else
	{
		total = File_Size(&f);
		if(off == 0 && size == 0)
			size = total;
		else if(size > total)
		{
			File_Close(&f);
			File_Dtor(&f);
			return 3;
		}
		if(off + size > total)
		{
			r = 2;
		}
		else if(!buf)
		{
			if(outSize)
				*outSize = size;
			r = 0;
		}
		else
		{
			File_Seek(&f, off);
			if(File_Read(&f, buf, size) != size)
			{
				r = -1;
			}
			else
			{
				if(outSize)
					*outSize = size;
				r = 0;
			}
		}
		File_Close(&f);
	}
	File_Dtor(&f);
	return r;
}

/* Read a range of a loose file found by the engine's rule: an absolute
 * name ("\..." or "x:...") as it is, else dir + name, then - when the
 * search list is on - dir + sub-directory + name for every registered
 * sub-directory, newest first, until one can be opened.  The codes of
 * ReadFileRange. */
int ReadFileEx(void* buf, uint32_t* outSize, const char* dir, const char* name, uint32_t off, uint32_t size)
{
	char path[0x104];
	int r;
	if(name[0] == '\\' || name[1] == ':')
		return ReadFileRange(buf, outSize, name, off, size);
	sprintf(path, "%s%s", dir, name);
	r = ReadFileRange(buf, outSize, path, off, size);
	if(gFileSearchOn)
	{
		SearchNode_t* n;
		for(n = gSearchList; n && r == 1; n = n->next)
		{
			sprintf(path, "%s%s\\%s", dir, n->name, name);
			r = ReadFileRange(buf, outSize, path, off, size);
		}
	}
	return r;
}

// The whole of a loose file into buf; the bytes read, 0 when it could not be read.
uint32_t ReadWholeFile(void* buf, const char* dir, const char* name)
{
	uint32_t size = 0;
	if(ReadFileEx(buf, &size, dir, name, 0, 0) != 0)
		return 0;
	return size;
}

// ReadWholeFile from the base directory, then from the alternative one.
uint32_t ReadFileAt(void* buf, const char* name)
{
	uint32_t n = ReadWholeFile(buf, gBaseDir, name);
	if(!n)
		n = ReadWholeFile(buf, gAltDir, name);
	return n;
}

/* Create (or overwrite) a file - an absolute name as it is, else in the
 * base directory - with `len` bytes; the bytes written, 0 when it could
 * not be created. */
int WriteTextFile(const char* name, const void* buf, uint32_t len)
{
	File_t f;
	char path[0x104];
	uint32_t n = 0;
	File_Ctor(&f);
	if(name[0] == '\\' || name[1] == ':')
		strcpy(path, name);
	else
		PathJoin(path, gBaseDir, name);
	if(File_Create(&f, path))
	{
		n = File_Write(&f, buf, len);
		File_Close(&f);
	}
	File_Dtor(&f);
	return (int)n;
}
