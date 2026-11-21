/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * loadfile.c - the loader the engine reads its assets through, and the
 *              file queries of the scripts (src/include/bgi/file.h)
 *
 * Everything the engine loads comes through LoadFile or one of its range
 * variants.  The search order: a loose file in the base directory; then
 * either a loose file in the alternative directory (no archive given) or
 * the archive in the base directory and then in the alternative
 * directory; when nothing is found the user is prompted for the disc and
 * the search starts over.  DSC-compressed and CompressedBG data is
 * decoded on the way, so the callers see the engine's own formats only.
 *
 * An archive argument of NULL (a null script pointer) means "a loose
 * file": the loose file is then also looked for in the alternative
 * directory.
 */
#include "bgi/file.h"
#include "bgi/codec.h"
#include "bgi/error.h"
#include "bgi/vm.h"
#include "bgi/sys.h"
#include "bgi/strutil.h"
#include "bgi/msg.h"
#include "bgi/engine.h"
#include "bgi/version.h"
#include "bgi/core.h"

/* Archive entry names are limited to 15 characters (the PackFile entry);
 * the builds with BURIKO ARC20 archives (1.573 on) allow 95.  A longer
 * name is a script error. */
static void CheckArcName(OpenBGI_t* o, const char* name)
{
	int limit = o->engine.profile->gen >= GEN_1_573 ? 0x5f : 15;
	char msg[0x100];
	if((int)strlen(name) > limit)
	{
		sprintf(msg, MSG_NAME_TOO_LONG, name, limit);
		ThrowScriptError(o, msg);
	}
}

// An archive entry into dst (NULL: its size only); the bytes, or an ARC_ERR_* code.
uint32_t Arc_Read(OpenBGI_t* o, void* dst, const char* arcPath, const char* name)
{
	CheckArcName(o, name);
	return ArcMgr_Read(o, o->files.arcMgr, dst, arcPath, name);
}

// A range of an archive entry (size 0: all of it); the bytes, or an ARC_ERR_* code.
static uint32_t Arc_ReadRange(OpenBGI_t* o, void* dst, const char* arcPath, const char* name, uint32_t off, uint32_t size)
{
	CheckArcName(o, name);
	return ArcMgr_ReadRange(o, o->files.arcMgr, dst, arcPath, name, off, size);
}

// 1 when the archive has the entry (0 also when the archive cannot be opened).
int Arc_Exists(OpenBGI_t* o, const char* arcPath, const char* name)
{
	CheckArcName(o, name);
	return ArcMgr_Exists(o, o->files.arcMgr, arcPath, name);
}

static uint32_t LoadFileDecoded(OpenBGI_t* o, void* buf, const char* arc, const char* name);
static uint32_t GetFileSizeDecoded(OpenBGI_t* o, const char* arc, const char* name);

/* Load a whole file into buf (which must be large enough: GetFileSize_
 * says how large), prompting for the disc until it is found, and decode
 * DSC / CompressedBG data in place - from 1.69 build 472 on through a
 * buffer of its own (LoadFileDecoded).  The bytes in buf.  Throws when the
 * file is missing and there is no disc to ask for, or the user aborts. */
uint32_t LoadFile(OpenBGI_t* o, void* buf, const char* arc, const char* name)
{
	uint32_t n;
	char msg[0x200];
	uint32_t* hdr = (uint32_t*)buf;

	if(o->engine.profile->gen >= GEN_1_69_472)
		return LoadFileDecoded(o, buf, arc, name);
	hdr[0] = hdr[1] = hdr[2] = hdr[3] = 0; // so that the magic tests fail on an empty read
	n = ReadWholeFile(o, buf, o->files.baseDir, name);
	if(!arc)
	{
		while(!n)
		{
			n = ReadWholeFile(o, buf, o->files.altDir, name);
			if(n)
				break;
			sprintf(msg, MSG_FILE_NOT_FOUND, name);
			RetryPrompt(o, msg);
		}
	}
	else if(!n)
	{
		char path[0x104];
		PathJoin(path, o->files.baseDir, arc);
		n = Arc_Read(o, buf, path, name);
		while(n == ARC_ERR_OPEN || n == ARC_ERR_NOT_FOUND)
		{
			if(DriveReady(o, o->files.altDir))
			{
				PathJoin(path, o->files.altDir, arc);
				n = Arc_Read(o, buf, path, name);
			}
			if(n == ARC_ERR_OPEN || n == ARC_ERR_NOT_FOUND)
			{
				sprintf(msg, MSG_ARC_FILE_NOT_FOUND, arc, name);
				RetryPrompt(o, msg);
			}
		}
	}

	if(BGI_IsDsc(buf))
	{
		PixBuf_t tmp;
		uint8_t* out;
		PixBuf_Ctor(&tmp);
		out = PixBuf_Alloc(&tmp, BGI_DscOutSize(buf));
		n = BGI_DscDecode(o, buf, out);
		memcpy(buf, out, n);
		PixBuf_Dtor(&tmp);
	}
	else if(BGI_IsCbg(buf))
	{
		uint32_t size = BGI_CbgOutSize(buf);
		uint8_t* out = (uint8_t*)BGI_Alloc(size);
		if(BGI_CbgDecode(buf, out) == 0)
		{
			memcpy(buf, out, size);
			n = size;
		}
		BGI_Free(out); // a CompressedBG that does not decode is left as stored
	}
	return n;
}

/* "80 31" up to 1.69 build 444, "81 30" of 1.529 on: [off, off + size) of
 * a file as stored (no decoding, no prompt).  0 ok, 1 not found, 2 the
 * range reaches past the end, 3 the size exceeds the file, -1 short read. */
int LoadFileRange(OpenBGI_t* o, void* buf, const char* arc, const char* name, uint32_t off, uint32_t size)
{
	int r = ReadLooseFile(o, buf, NULL, o->files.baseDir, name, off, size);
	uint32_t a;
	char path[0x104];
	if(r != 1)
		return r;
	if(!arc)
		return ReadLooseFile(o, buf, NULL, o->files.altDir, name, off, size);
	PathJoin(path, o->files.baseDir, arc);
	a = Arc_ReadRange(o, buf, path, name, off, size);
	if((a == ARC_ERR_OPEN || a == ARC_ERR_NOT_FOUND) && DriveReady(o, o->files.altDir))
	{
		PathJoin(path, o->files.altDir, arc);
		a = Arc_ReadRange(o, buf, path, name, off, size);
	}
	switch(a) // the archive codes mapped to those of ReadLooseFile
	{
		case ARC_ERR_OPEN:
		case ARC_ERR_NOT_FOUND: return 1;
		case ARC_ERR_RANGE: return 2;
		case ARC_ERR_OFFSET: return 3;
		default: return 0;
	}
}

/* ---- the decoded readers of 1.69 build 472 on ----
 *
 * From 1.69 build 472 the loader reads a file as stored into a buffer of
 * its own and hands the caller the decoded data only: LoadFile, the file
 * size query and "80 31" go through these. */

#define LOAD_ERR_DECODE    0x80000050u // the archive entry cannot be read whole, or its data does not decode
#define LOAD_ERR_TOO_LARGE 0x80000060u // the entry, as stored or as a decoded DSC stream, exceeds DecodedLimit()

// the largest file the decoded readers take, as stored and as a decoded DSC stream
static uint32_t DecodedLimit(OpenBGI_t* o)
{
	if(o->engine.profile->gen <= GEN_1_69_472)
		return 0x2800000u; // 40 MB
	if(o->engine.profile->gen <= GEN_1_494)
		return 0x3000000u; // 48 MB
	return 0x4000000u;     // 64 MB from 1.529
}

// n bytes for a file as stored, with a zeroed tail so that the header tests never read past the data
static uint8_t* RawAlloc(uint32_t n)
{
	uint8_t* raw = (uint8_t*)BGI_Alloc((size_t)n + 0x30);
	memset(raw + n, 0, 0x30);
	return raw;
}

/* A range of the decoded form of `src` (srcSize bytes as stored) into
 * dst, its size into *out (when not NULL); off = size = 0 asks for
 * everything, dst NULL for the size only.  A whole CompressedBG is decoded
 * straight into dst.  The decoded size of a CompressedBG counts 4 bytes a
 * pixel for a 24-bit picture: the original decoders widen those pixels to
 * 32 bits, this one keeps 3 bytes, so the end of that size is not written.
 * 0 ok, 2 the range reaches past the end, 3 a zero or too large size, 5
 * the data does not decode, 6 a DSC stream that decodes to more than the
 * limit. */
static int DecodeRange(OpenBGI_t* o, void* dst, const uint8_t* src, uint32_t srcSize, uint32_t off, uint32_t size, uint32_t* out)
{
	uint8_t* tmp = NULL;
	const uint8_t* data = src;
	uint32_t total = srcSize;
	int direct = 0, r;
	if(BGI_IsDsc(src))
	{
		total = BGI_DscOutSize(src);
		if(total > DecodedLimit(o))
			return 6;
		tmp = (uint8_t*)BGI_Alloc(total);
		if(BGI_DscDecode(o, src, tmp) != total)
		{
			BGI_Free(tmp);
			return 5;
		}
		data = tmp;
	}
	else if(BGI_IsCbg(src))
	{
		const CbgHeader_t* h = (const CbgHeader_t*)src;
		uint32_t bpp = h->bpp & 0xffffu;
		uint8_t* to;
		total = 0x10u + (uint32_t)h->width * h->height * (bpp == 24 ? 4 : bpp >> 3);
		direct = dst && off == 0 && size == 0;
		to = direct ? (uint8_t*)dst : (tmp = (uint8_t*)BGI_Calloc(total));
		if(BGI_CbgDecode(src, to) != 0)
		{
			BGI_Free(tmp);
			return 5;
		}
		data = to;
	}
	if(off == 0 && size == 0)
		size = total;
	if(size == 0 || size > total)
		r = 3;
	else if(off + size > total)
		r = 2;
	else
	{
		if(dst && !direct)
			memcpy(dst, data + off, size);
		if(out)
			*out = size;
		r = 0;
	}
	BGI_Free(tmp);
	return r;
}

/* one loose file, read whole and decoded for the range; 1 when it cannot
 * be opened, 5 when it cannot be read whole, 6 when it is larger than
 * the limit as stored, else the codes of DecodeRange */
static int LoadLooseDecoded(OpenBGI_t* o, void* buf, const char* path, uint32_t off, uint32_t size, uint32_t* out)
{
	File_t f;
	uint32_t total;
	uint8_t* raw;
	int r;
	File_Ctor(&f);
	if(!DriveReady(o, path) || !File_OpenRead(o, &f, path))
	{
		File_Dtor(&f);
		return 1;
	}
	total = File_Size(&f);
	if(total > DecodedLimit(o))
		r = 6;
	else
	{
		raw = RawAlloc(total);
		if(File_Read(&f, raw, total) != total)
			r = 5;
		else
			r = DecodeRange(o, buf, raw, total, off, size, out);
		BGI_Free(raw);
	}
	File_Close(&f);
	File_Dtor(&f);
	return r;
}

// the loose file of a directory, resolved as ReadLooseFile does (absolute name, dir + name, the search list)
static int LoadDirDecoded(OpenBGI_t* o, void* buf, const char* dir, const char* name, uint32_t off, uint32_t size, uint32_t* out)
{
	char path[0x104];
	int r;
	if(name[0] == '\\' || name[1] == ':')
		return LoadLooseDecoded(o, buf, name, off, size, out);
	sprintf(path, "%s%s", dir, name);
	r = LoadLooseDecoded(o, buf, path, off, size, out);
	if(o->files.fileSearchOn)
	{
		SearchNode_t* n;
		for(n = o->files.searchList; n && r == 1; n = n->next)
		{
			sprintf(path, "%s%s\\%s", dir, n->name, name);
			r = LoadLooseDecoded(o, buf, path, off, size, out);
		}
	}
	return r;
}

// the decoded size of the loose file of a directory read into buf (NULL: the size only); 0 when it is missing or does not load
static uint32_t LoadDirWhole(OpenBGI_t* o, void* buf, const char* dir, const char* name)
{
	uint32_t n = 0;
	return LoadDirDecoded(o, buf, dir, name, 0, 0, &n) == 0 ? n : 0;
}

/* An archive entry, read whole and decoded for the range (dst NULL: the
 * size only): the bytes of the range, or ARC_ERR_NOT_FOUND - also when the
 * archive cannot be opened, the original does not tell the two apart
 * here -, ARC_ERR_RANGE, ARC_ERR_OFFSET, LOAD_ERR_DECODE or
 * LOAD_ERR_TOO_LARGE. */
static uint32_t ArcReadDecoded(OpenBGI_t* o, void* dst, const char* path, const char* name, uint32_t off, uint32_t size)
{
	static const uint32_t kCodes[7] = {0, 0, ARC_ERR_RANGE, ARC_ERR_OFFSET, 0, LOAD_ERR_DECODE, LOAD_ERR_TOO_LARGE};
	uint32_t n, out = 0;
	uint8_t* raw;
	int r;
	CheckArcName(o, name);
	n = ArcMgr_SizeOf(o, o->files.arcMgr, path, name);
	if(n >= 0x80000000u)
		return ARC_ERR_NOT_FOUND;
	if(n > DecodedLimit(o))
		return LOAD_ERR_TOO_LARGE;
	raw = RawAlloc(n);
	r = ArcMgr_Read(o, o->files.arcMgr, raw, path, name) == n ? DecodeRange(o, dst, raw, n, off, size, &out) : 5;
	BGI_Free(raw);
	return r == 0 ? out : kCodes[r];
}

/* LoadFile of 1.69 build 472 on: searched for in the same order, but
 * read through the decoded readers, so that buf receives the decoded data
 * only (GetFileSize_ gives its size).  A file that does not decode, or
 * that exceeds the limit, loads as 0 bytes; as a loose file it counts as
 * missing.  (The original also clears the first 16 bytes of buf first,
 * which nothing reads any more; buf may be smaller.) */
static uint32_t LoadFileDecoded(OpenBGI_t* o, void* buf, const char* arc, const char* name)
{
	uint32_t n;
	char msg[0x200], path[0x104];

	n = LoadDirWhole(o, buf, o->files.baseDir, name);
	if(!arc)
	{
		while(!n)
		{
			n = LoadDirWhole(o, buf, o->files.altDir, name);
			if(n)
				break;
			sprintf(msg, MSG_FILE_NOT_FOUND, name);
			RetryPrompt(o, msg);
		}
		return n;
	}
	if(n)
		return n;
	PathJoin(path, o->files.baseDir, arc);
	n = ArcReadDecoded(o, buf, path, name, 0, 0);
	while(n == ARC_ERR_OPEN || n == ARC_ERR_NOT_FOUND)
	{
		if(DriveReady(o, o->files.altDir))
		{
			PathJoin(path, o->files.altDir, arc);
			n = ArcReadDecoded(o, buf, path, name, 0, 0);
		}
		if(n == ARC_ERR_OPEN || n == ARC_ERR_NOT_FOUND)
		{
			sprintf(msg, MSG_ARC_FILE_NOT_FOUND, arc, name);
			RetryPrompt(o, msg);
		}
	}
	return n >= 0x80000000u ? 0 : n;
}

/* GetFileSize_ of 1.69 build 472 on: the size LoadFile gives, found
 * through the same readers (without the disc prompt); 0 when the file is
 * not found or does not load.  For an archive entry the alternative
 * directory is never tried: the original tries it only on an archive
 * that cannot be opened, which its decoded reader does not report. */
static uint32_t GetFileSizeDecoded(OpenBGI_t* o, const char* arc, const char* name)
{
	uint32_t n;
	char path[0x104];
	n = LoadDirWhole(o, NULL, o->files.baseDir, name);
	if(!arc)
	{
		if(!n)
			n = LoadDirWhole(o, NULL, o->files.altDir, name);
	}
	else if(!n)
	{
		PathJoin(path, o->files.baseDir, arc);
		n = ArcReadDecoded(o, NULL, path, name, 0, 0);
		if(n >= 0x80000000u)
			n = 0;
	}
	return n;
}

/* "80 31" of 1.69 build 472 on: a range of the decoded form of a file
 * (DSC / CompressedBG), looked for like LoadFile does but without the
 * disc prompt; the codes of DecodeRange, 1 when the file is not found, 5
 * also when the archive entry cannot be read whole, 6 when it is larger
 * than the limit as stored. */
int LoadFileRangeDecoded(OpenBGI_t* o, void* buf, const char* arc, const char* name, uint32_t off, uint32_t size)
{
	uint32_t a;
	int r;
	char path[0x104];

	r = LoadDirDecoded(o, buf, o->files.baseDir, name, off, size, NULL);
	if(r != 1)
		return r;
	if(!arc)
		return LoadDirDecoded(o, buf, o->files.altDir, name, off, size, NULL);
	PathJoin(path, o->files.baseDir, arc);
	a = ArcReadDecoded(o, buf, path, name, off, size);
	if((a == ARC_ERR_OPEN || a == ARC_ERR_NOT_FOUND) && DriveReady(o, o->files.altDir))
	{
		PathJoin(path, o->files.altDir, arc);
		a = ArcReadDecoded(o, buf, path, name, off, size);
	}
	switch(a) // the reader's codes mapped to those of DecodeRange
	{
		case ARC_ERR_OPEN:
		case ARC_ERR_NOT_FOUND: return 1;
		case ARC_ERR_RANGE: return 2;
		case ARC_ERR_OFFSET: return 3;
		case LOAD_ERR_DECODE: return 5;
		case LOAD_ERR_TOO_LARGE: return 6;
		default: return 0;
	}
}

/* "81 3C" of 1.573 on: the archive file `name` of the base directory -
 * or, when that is not one, of the alternative directory - has its index
 * read again on its next use (ArcMgr_Reset); the configuration windows
 * call it after replacing or deleting a theme archive.  0 when the
 * archive was found, 1 when not. */
int ReloadArchive(OpenBGI_t* o, const char* name)
{
	char path[0x104];
	uint32_t r;
	PathJoin(path, o->files.baseDir, name);
	r = ArcMgr_Reset(o, o->files.arcMgr, path);
	if(r == ARC_ERR_OPEN && DriveReady(o, o->files.altDir))
	{
		PathJoin(path, o->files.altDir, name);
		r = ArcMgr_Reset(o, o->files.arcMgr, path);
	}
	return r != 0;
}

/* "81 35" of 1.588 on: the size of a loose file (arc NULL) or of an
 * archive entry as stored, without reading the data (a loose copy in the
 * base directory wins over the archive entry; the alternative directory
 * is tried only when the base directory has no such archive); 0 when it
 * is not found. */
uint32_t GetFileSizeFast(OpenBGI_t* o, const char* arc, const char* name)
{
	uint32_t n = 0;
	char path[0x104];
	if(!arc)
	{
		if(ReadLooseFile(o, NULL, &n, o->files.baseDir, name, 0, 0) != 0 && DriveReady(o, o->files.altDir))
		{
			n = 0;
			ReadLooseFile(o, NULL, &n, o->files.altDir, name, 0, 0);
		}
		return n;
	}
	if(ReadLooseFile(o, NULL, &n, o->files.baseDir, name, 0, 0) == 0 && n)
		return n;
	PathJoin(path, o->files.baseDir, arc);
	n = Arc_Read(o, NULL, path, name);
	if(n == ARC_ERR_OPEN && DriveReady(o, o->files.altDir))
	{
		PathJoin(path, o->files.altDir, arc);
		n = Arc_Read(o, NULL, path, name);
	}
	return n >= 0x80000000u ? 0 : n;
}

/* "80 35": the size a file takes once loaded (the decoded size of a DSC
 * stream; a CompressedBG reports its stored size), found by reading it
 * into a 32 MB scratch buffer; 0 when it is not found.  The alternative
 * directory is tried only when the base directory has no such archive.
 * From 1.69 build 472 on: GetFileSizeDecoded. */
uint32_t GetFileSize_(OpenBGI_t* o, const char* arc, const char* name)
{
	uint8_t* buf;
	uint32_t n;
	char path[0x104];
	if(o->engine.profile->gen >= GEN_1_69_472)
		return GetFileSizeDecoded(o, arc, name);
	buf = (uint8_t*)BGI_Alloc(0x2000000);
	memset(buf, 0, 16); // so that the DSC magic test fails on an empty read
	n = ReadWholeFile(o, buf, o->files.baseDir, name);
	if(!arc)
	{
		if(!n)
			n = ReadWholeFile(o, buf, o->files.altDir, name);
	}
	else if(!n)
	{
		PathJoin(path, o->files.baseDir, arc);
		n = Arc_Read(o, buf, path, name);
		if(n == ARC_ERR_OPEN && DriveReady(o, o->files.altDir))
		{
			PathJoin(path, o->files.altDir, arc);
			n = Arc_Read(o, buf, path, name);
		}
		if(n == ARC_ERR_OPEN || n == ARC_ERR_NOT_FOUND)
			n = 0;
	}
	if(n && BGI_IsDsc(buf))
		n = BGI_DscOutSize(buf);
	BGI_Free(buf);
	return n;
}

// Whether dir + name (also through the search list) exists and is not a directory.
int Dir_Exists(OpenBGI_t* o, const char* dir, const char* name)
{
	char path[0x104];
	uint32_t a;
	int r;
	SearchNode_t* n;
	if(!DriveReady(o, dir))
		return 0;
	sprintf(path, "%s%s", dir, name);
	a = Fs_FileAttrs(o, path);
	r = ((~a) >> 4) & 1; // the attributes are valid and the directory bit is clear
	if(!o->files.fileSearchOn)
		return r;
	for(n = o->files.searchList; n && !r; n = n->next)
	{
		if(!DriveReady(o, dir))
			continue;
		sprintf(path, "%s%s\\%s", dir, n->name, name);
		a = Fs_FileAttrs(o, path);
		r = ((~a) >> 4) & 1;
	}
	return r;
}

// "80 34": whether a loose file or archive entry exists along the loader's search order.
int FileExists(OpenBGI_t* o, const char* arc, const char* name)
{
	char path[0x104];
	int r = Dir_Exists(o, o->files.baseDir, name);
	if(!arc)
		return r ? r : Dir_Exists(o, o->files.altDir, name);
	if(r)
		return r;
	PathJoin(path, o->files.baseDir, arc);
	r = Arc_Exists(o, path, name);
	if(r)
		return r;
	if(!DriveReady(o, o->files.altDir))
		return 0;
	PathJoin(path, o->files.altDir, arc);
	return Arc_Exists(o, path, name);
}

// "90 F0": the full path of a loose file, in the base directory, else the alternative one; 1 when found.
int FindMoviePath(OpenBGI_t* o, char* dst, const char* name)
{
	char path[0x104];
	PathJoin(path, o->files.baseDir, name);
	if(Fs_FileAttrs(o, path) != OPENBGI_INVALID_ATTRS)
	{
		strcpy(dst, path);
		return 1;
	}
	if(!DriveReady(o, o->files.altDir))
		return 0;
	PathJoin(path, o->files.altDir, name);
	if(Fs_FileAttrs(o, path) != OPENBGI_INVALID_ATTRS)
	{
		strcpy(dst, path);
		return 1;
	}
	return 0;
}

/* "80 3C": the prompt (OK looks again, Cancel asks whether to give up)
 * until `name` exists as a file in the base directory (DiskPrompt_t,
 * bgi/file.h), a step per frame: 0 while a box is up, 1 once the file is
 * there, -1 when the user gave up. */
int DiskPrompt_Step(OpenBGI_t* o, DiskPrompt_t* d)
{
	for(;;)
	{
		if(d->ask.state == 0 && DriveReady(o, o->files.baseDir))
		{ // no box is up: look
			char path[0x104];
			uint32_t a;
			PathJoin(path, o->files.baseDir, d->name);
			a = Fs_FileAttrs(o, path);
			if(a != OPENBGI_INVALID_ATTRS && ((~a) >> 4) & 1) // a file, not a directory
				return 1;
		}
		switch(AskRetry_Step(o, &d->ask, d->msg, d->title, d->title, 1))
		{
			case ASK_WAIT: return 0;
			case ASK_ABORT: return -1;
			default: break; // OK: look again
		}
	}
}

/* "80 3B": the file dialog - open (mode 0) or save (mode 1) - with the
 * filter "<desc>(*.<ext>) *.<ext>" (built into `filter`, 0x100 bytes, which
 * must last as long as the request), the default extension, a title and
 * an initial directory; `path` (0x104 bytes) is zeroed and receives the
 * file chosen.  4 for an unknown mode; else 0 and the request in *req,
 * whose answer is 1 when a file was chosen. */
int FileDialog_Ask(OpenBGI_t* o, uint32_t* req, char* filter, char* path, const char* desc, const char* ext, const char* title,
	const char* initialDir, int mode)
{
	OpenBGI_Request_t q;
	memset(path, 0, 0x104);
	sprintf(filter, "%s(*.%s) *.%s", desc, ext, ext);
	if(mode != 0 && mode != 1)
		return 4;
	memset(&q, 0, sizeof q);
	q.kind = OPENBGI_ASK_FILE;
	q.u.file.save = mode;
	q.u.file.path = path;
	q.u.file.pathSize = 0x104;
	q.u.file.filter = filter;
	q.u.file.defExt = ext;
	q.u.file.initialDir = initialDir;
	q.u.file.title = title;
	*req = Request_Post(o, &q, REQUEST_TREAT_NONE);
	return 0;
}

/* "81 38" (1.553 on): the file dialog - open (kind 0) or save (kind 1) -
 * with `count` description / pattern pairs (the arrays must last as long
 * as the request), a title and an initial directory (either may be NULL),
 * the default extension that of the chosen filter.  The clock stands
 * still while it is up.  7 when count is below 1 (path untouched); else
 * `path` (0x30c bytes) is zeroed, and 4 for an unknown kind, or 0 and the
 * request in *req, whose answer is 1 when a file was chosen. */
int FileDialogN_Ask(OpenBGI_t* o, uint32_t* req, char* path, int count, const char* const* descs, const char* const* pats,
	const char* title, const char* initialDir, int kind)
{
	OpenBGI_Request_t q;
	if(count < 1)
		return 7;
	memset(path, 0, 0x30c);
	if(kind != 0 && kind != 1)
		return 4;
	memset(&q, 0, sizeof q);
	q.kind = OPENBGI_ASK_FILE;
	q.u.file.save = kind;
	q.u.file.path = path;
	q.u.file.pathSize = 0x30c;
	q.u.file.desc = descs;
	q.u.file.pattern = pats;
	q.u.file.count = count;
	q.u.file.defExt = "";
	q.u.file.initialDir = initialDir;
	q.u.file.title = title;
	*req = Request_Post(o, &q, REQUEST_TREAT_CLOCK);
	return 0;
}

/* "80 24" / "80 25": the files matching `pattern`, their names (with
 * `prefix` in front when given) stored into out[] when out is not NULL,
 * at most `max`; sub-directories are entered when `recurse` is set, with
 * their name added to the prefix.  The count. */
int FindFiles(OpenBGI_t* o, char** out, const char* pattern, int recurse, const char* prefix, int max)
{
	OpenBGI_FindData_t fd;
	FsFind_t* h = Fs_FindFirst(o, pattern, &fd);
	int count = 0;
	if(!h)
		return 0;
	do
	{
		if(fd.attrs & OPENBGI_ATTR_DIRECTORY)
		{
			char sub[0x208], subPrefix[0x104], filePart[0x104];
			char* slash;
			int n;
			if(!recurse || strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0)
				continue;
			// the pattern with the directory inserted before its file part
			strcpy(sub, pattern);
			slash = strrchr(sub, '\\');
			if(slash)
			{
				strcpy(filePart, slash + 1);
				sprintf(slash + 1, "%s\\%s", fd.name, filePart);
			}
			else
			{
				sprintf(sub, "%s\\%s", fd.name, pattern);
			}
			if(prefix)
				sprintf(subPrefix, "%s%s\\", prefix, fd.name);
			else
				sprintf(subPrefix, "%s\\", fd.name);
			n = FindFiles(o, out, sub, 1, subPrefix, max - count);
			count += n;
			if(out)
				out += n;
		}
		else
		{
			if(out)
			{
				if(prefix)
					sprintf(*out, "%s%s", prefix, fd.name);
				else
					strcpy(*out, fd.name);
				out++;
			}
			count++;
		}
	} while(Fs_FindNext(h, &fd) && count < max);
	Fs_FindClose(h);
	return count;
}
