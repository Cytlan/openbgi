/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * loadfile.c - the loader the engine reads its assets through, and the
 *              file queries of the scripts (inc/bgi/file.h)
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
#include "bgi/os.h"

/* Archive entry names are limited to 15 characters (the PackFile entry);
 * the builds with BURIKO ARC20 archives (1.573 on) allow 95.  A longer
 * name is a script error. */
static void CheckArcName(const char* name)
{
	int limit = gEngine->gen >= GEN_1_573 ? 0x5f : 15;
	char msg[0x100];
	if((int)strlen(name) > limit)
	{
		sprintf(msg, MSG_NAME_TOO_LONG, name, limit);
		ThrowScriptError(msg);
	}
}

// An archive entry into dst (NULL: its size only); the bytes, or an ARC_ERR_* code.
uint32_t Arc_Read(void* dst, const char* arcPath, const char* name)
{
	CheckArcName(name);
	return ArcMgr_Read(gArcMgr, dst, arcPath, name);
}

// A range of an archive entry (size 0: all of it); the bytes, or an ARC_ERR_* code.
static uint32_t Arc_ReadRange(void* dst, const char* arcPath, const char* name, uint32_t off, uint32_t size)
{
	CheckArcName(name);
	return ArcMgr_ReadRange(gArcMgr, dst, arcPath, name, off, size);
}

// 1 when the archive has the entry (0 also when the archive cannot be opened).
int Arc_Exists(const char* arcPath, const char* name)
{
	CheckArcName(name);
	return ArcMgr_Exists(gArcMgr, arcPath, name);
}

/* Load a whole file into buf (which must be large enough: GetFileSize_
 * says how large), prompting for the disc until it is found, and decode
 * DSC / CompressedBG data in place.  The bytes in buf.  Throws when the
 * file is missing and there is no disc to ask for, or the user aborts. */
uint32_t LoadFile(void* buf, const char* arc, const char* name)
{
	uint32_t n;
	char msg[0x200];
	uint32_t* hdr = (uint32_t*)buf;

	hdr[0] = hdr[1] = hdr[2] = hdr[3] = 0; // so that the magic tests fail on an empty read
	n = ReadWholeFile(buf, gBaseDir, name);
	if(!arc)
	{
		while(!n)
		{
			n = ReadWholeFile(buf, gAltDir, name);
			if(n)
				break;
			sprintf(msg, MSG_FILE_NOT_FOUND, name);
			RetryPrompt(msg);
		}
	}
	else if(!n)
	{
		char path[0x104];
		PathJoin(path, gBaseDir, arc);
		n = Arc_Read(buf, path, name);
		while(n == ARC_ERR_OPEN || n == ARC_ERR_NOT_FOUND)
		{
			if(DriveReady(gAltDir))
			{
				PathJoin(path, gAltDir, arc);
				n = Arc_Read(buf, path, name);
			}
			if(n == ARC_ERR_OPEN || n == ARC_ERR_NOT_FOUND)
			{
				sprintf(msg, MSG_ARC_FILE_NOT_FOUND, arc, name);
				RetryPrompt(msg);
			}
		}
	}

	if(BGI_IsDsc(buf))
	{
		PixBuf_t tmp;
		uint8_t* out;
		PixBuf_Ctor(&tmp);
		out = PixBuf_Alloc(&tmp, BGI_DscOutSize(buf));
		n = BGI_DscDecode(buf, out);
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
int LoadFileRange(void* buf, const char* arc, const char* name, uint32_t off, uint32_t size)
{
	int r = ReadFileEx(buf, NULL, gBaseDir, name, off, size);
	uint32_t a;
	char path[0x104];
	if(r != 1)
		return r;
	if(!arc)
		return ReadFileEx(buf, NULL, gAltDir, name, off, size);
	PathJoin(path, gBaseDir, arc);
	a = Arc_ReadRange(buf, path, name, off, size);
	if((a == ARC_ERR_OPEN || a == ARC_ERR_NOT_FOUND) && DriveReady(gAltDir))
	{
		PathJoin(path, gAltDir, arc);
		a = Arc_ReadRange(buf, path, name, off, size);
	}
	switch(a) // the archive codes mapped to those of ReadFileEx
	{
		case ARC_ERR_OPEN:
		case ARC_ERR_NOT_FOUND: return 1;
		case ARC_ERR_RANGE: return 2;
		case ARC_ERR_OFFSET: return 3;
		default: return 0;
	}
}

/* A range of the decoded form of `src` (srcSize bytes as stored) into
 * dst; off = size = 0 asks for everything, and for CompressedBG only
 * for the size (nothing is copied then).  0 ok, 2 the range reaches past
 * the end, 3 a zero or too large size, 5 the data does not decode, 6 the
 * decoded size exceeds the limit. */
static int DecodeRange(void* dst, const uint8_t* src, uint32_t srcSize, uint32_t off, uint32_t size)
{
	uint32_t limit = gEngine->gen <= GEN_1_69_472 ? 0x2800000u : 0x4000000u; // 40 MB, 64 MB from 1.494
	uint8_t* tmp = NULL;
	const uint8_t* data = src;
	uint32_t total = srcSize;
	int r;
	if(BGI_IsDsc(src))
	{
		total = BGI_DscOutSize(src);
		if(total > limit)
			return 6;
		tmp = (uint8_t*)BGI_Alloc(total);
		if(BGI_DscDecode(src, tmp) != total)
		{
			BGI_Free(tmp);
			return 5;
		}
		data = tmp;
	}
	else if(BGI_IsCbg(src))
	{
		total = BGI_CbgOutSize(src);
		if(off != 0 || size != 0) // a size query does not decode the picture
		{
			tmp = (uint8_t*)BGI_Alloc(total);
			if(BGI_CbgDecode(src, tmp) != 0)
			{
				BGI_Free(tmp);
				return 5;
			}
			data = tmp;
		}
	}
	if(off == 0 && size == 0)
		size = total;
	if(size == 0 || size > total)
		r = 3;
	else if(off + size > total)
		r = 2;
	else
	{
		if(data != src || !BGI_IsCbg(src)) // an undecoded CompressedBG: the size only
			memcpy(dst, data + off, size);
		r = 0;
	}
	if(tmp)
		BGI_Free(tmp);
	return r;
}

/* one loose file, read whole and decoded for the range; 1 when it cannot
 * be opened, 5 when it cannot be read whole, 6 when it is larger than
 * `limit` as stored, else the codes of DecodeRange */
static int LoadLooseDecoded(void* buf, uint32_t limit, const char* path, uint32_t off, uint32_t size)
{
	File_t f;
	uint32_t total;
	uint8_t* raw;
	int r;
	File_Ctor(&f);
	if(!DriveReady(path) || !File_OpenRead(&f, path))
	{
		File_Dtor(&f);
		return 1;
	}
	total = File_Size(&f);
	if(total > limit)
		r = 6;
	else
	{
		raw = (uint8_t*)BGI_Alloc(total ? total : 1);
		if(File_Read(&f, raw, total) != total)
			r = 5;
		else
			r = DecodeRange(buf, raw, total, off, size);
		BGI_Free(raw);
	}
	File_Close(&f);
	File_Dtor(&f);
	return r;
}

// the loose file of a directory, resolved as ReadFileEx does (absolute name, dir + name, the search list)
static int LoadDirDecoded(void* buf, uint32_t limit, const char* dir, const char* name, uint32_t off, uint32_t size)
{
	char path[0x104];
	int r;
	if(name[0] == '\\' || name[1] == ':')
		return LoadLooseDecoded(buf, limit, name, off, size);
	sprintf(path, "%s%s", dir, name);
	r = LoadLooseDecoded(buf, limit, path, off, size);
	if(gFileSearchOn)
	{
		SearchNode_t* n;
		for(n = gSearchList; n && r == 1; n = n->next)
		{
			sprintf(path, "%s%s\\%s", dir, n->name, name);
			r = LoadLooseDecoded(buf, limit, path, off, size);
		}
	}
	return r;
}

/* "80 31" of 1.69 build 472 on: a range of the decoded form of a file
 * (DSC / CompressedBG), looked for like LoadFile does but without the
 * disc prompt; the codes of DecodeRange, 1 when the file is not found, 5
 * also when the archive entry cannot be read whole, 6 when it is larger
 * than the limit as stored. */
int LoadFileRangeDecoded(void* buf, const char* arc, const char* name, uint32_t off, uint32_t size)
{
	uint32_t limit = gEngine->gen <= GEN_1_69_472 ? 0x2800000u : 0x4000000u; // 40 MB, 64 MB from 1.494
	uint32_t n, a;
	uint8_t* raw;
	int r;
	char path[0x104];

	r = LoadDirDecoded(buf, limit, gBaseDir, name, off, size);
	if(r != 1)
		return r;
	if(!arc)
		return LoadDirDecoded(buf, limit, gAltDir, name, off, size);
	// the archive member: its size first, then the whole member
	PathJoin(path, gBaseDir, arc);
	CheckArcName(name);
	n = ArcMgr_SizeOf(gArcMgr, path, name);
	if((n == ARC_ERR_OPEN || n == ARC_ERR_NOT_FOUND) && DriveReady(gAltDir))
	{
		PathJoin(path, gAltDir, arc);
		n = ArcMgr_SizeOf(gArcMgr, path, name);
	}
	if(n == ARC_ERR_OPEN || n == ARC_ERR_NOT_FOUND)
		return 1;
	if(n >= 0x80000000u) // any other archive error
		return 5;
	if(n > limit)
		return 6;
	raw = (uint8_t*)BGI_Alloc(n ? n : 1);
	a = ArcMgr_Read(gArcMgr, raw, path, name);
	r = a == n ? DecodeRange(buf, raw, n, off, size) : 5;
	BGI_Free(raw);
	return r;
}

/* "81 35" of 1.588 on: the size of a loose file (arc NULL) or of an
 * archive entry as stored, without reading the data (a loose copy in the
 * base directory wins over the archive entry; the alternative directory
 * is tried only when the base directory has no such archive); 0 when it
 * is not found. */
uint32_t GetFileSizeFast(const char* arc, const char* name)
{
	uint32_t n = 0;
	char path[0x104];
	if(!arc)
	{
		if(ReadFileEx(NULL, &n, gBaseDir, name, 0, 0) != 0 && DriveReady(gAltDir))
		{
			n = 0;
			ReadFileEx(NULL, &n, gAltDir, name, 0, 0);
		}
		return n;
	}
	if(ReadFileEx(NULL, &n, gBaseDir, name, 0, 0) == 0 && n)
		return n;
	PathJoin(path, gBaseDir, arc);
	n = Arc_Read(NULL, path, name);
	if(n == ARC_ERR_OPEN && DriveReady(gAltDir))
	{
		PathJoin(path, gAltDir, arc);
		n = Arc_Read(NULL, path, name);
	}
	return n >= 0x80000000u ? 0 : n;
}

/* "80 35": the size a file takes once loaded (the decoded size of a DSC
 * stream; a CompressedBG reports its stored size), found by reading it
 * into a 32 MB scratch buffer; 0 when it is not found.  The alternative
 * directory is tried only when the base directory has no such archive. */
uint32_t GetFileSize_(const char* arc, const char* name)
{
	uint8_t* buf = (uint8_t*)BGI_Alloc(0x2000000);
	uint32_t n;
	char path[0x104];
	memset(buf, 0, 16); // so that the DSC magic test fails on an empty read
	n = ReadWholeFile(buf, gBaseDir, name);
	if(!arc)
	{
		if(!n)
			n = ReadWholeFile(buf, gAltDir, name);
	}
	else if(!n)
	{
		PathJoin(path, gBaseDir, arc);
		n = Arc_Read(buf, path, name);
		if(n == ARC_ERR_OPEN && DriveReady(gAltDir))
		{
			PathJoin(path, gAltDir, arc);
			n = Arc_Read(buf, path, name);
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
int Dir_Exists(const char* dir, const char* name)
{
	char path[0x104];
	uint32_t a;
	int r;
	SearchNode_t* n;
	if(!DriveReady(dir))
		return 0;
	sprintf(path, "%s%s", dir, name);
	a = OS_FileAttrs(path);
	r = ((~a) >> 4) & 1; // the attributes are valid and the directory bit is clear
	if(!gFileSearchOn)
		return r;
	for(n = gSearchList; n && !r; n = n->next)
	{
		if(!DriveReady(dir))
			continue;
		sprintf(path, "%s%s\\%s", dir, n->name, name);
		a = OS_FileAttrs(path);
		r = ((~a) >> 4) & 1;
	}
	return r;
}

// "80 34": whether a loose file or archive entry exists along the loader's search order.
int FileExists(const char* arc, const char* name)
{
	char path[0x104];
	int r = Dir_Exists(gBaseDir, name);
	if(!arc)
		return r ? r : Dir_Exists(gAltDir, name);
	if(r)
		return r;
	PathJoin(path, gBaseDir, arc);
	r = Arc_Exists(path, name);
	if(r)
		return r;
	if(!DriveReady(gAltDir))
		return 0;
	PathJoin(path, gAltDir, arc);
	return Arc_Exists(path, name);
}

// "90 F0": the full path of a loose file, in the base directory, else the alternative one; 1 when found.
int FindMoviePath(char* dst, const char* name)
{
	char path[0x104];
	PathJoin(path, gBaseDir, name);
	if(OS_FileAttrs(path) != OS_INVALID_ATTRS)
	{
		strcpy(dst, path);
		return 1;
	}
	if(!DriveReady(gAltDir))
		return 0;
	PathJoin(path, gAltDir, name);
	if(OS_FileAttrs(path) != OS_INVALID_ATTRS)
	{
		strcpy(dst, path);
		return 1;
	}
	return 0;
}

/* "80 3C": prompt (OK retries, Cancel asks whether to abort) until `name`
 * exists as a file in the base directory; 1 when it does, 0 when the user
 * gave up. */
int PromptForDisk(const char* name, const char* title, const char* msg)
{
	int found = 0;
	while(!found)
	{
		char path[0x104];
		uint32_t a;
		if(DriveReady(gBaseDir))
		{
			PathJoin(path, gBaseDir, name);
			a = OS_FileAttrs(path);
			if(a != OS_INVALID_ATTRS)
			{
				found = ((~a) >> 4) & 1; // a file, not a directory
				if(found)
					continue;
			}
		}
		if(MsgBox(msg, title, OS_MB_OKCANCEL | OS_MB_ICONINFO) == OS_IDCANCEL &&
			MsgBox(MSG_ASK_ABORT, title, OS_MB_YESNO | OS_MB_ICONQUESTION | OS_MB_DEFBUTTON2) == OS_IDYES)
			break;
	}
	return found;
}

/* "80 3B": the file dialog - open (mode 0) or save (mode 1) - with the
 * filter "<desc>(*.<ext>) *.<ext>", the default extension, a title and an
 * initial directory.  0 when a file was chosen (its path in `path`, 0x104
 * bytes), -1 when the dialog was cancelled, 4 for an unknown mode. */
int FileDialog(char* path, const char* desc, const char* ext, const char* title, const char* initialDir, int mode)
{
	char filter[0x100];
	int ok;
	memset(path, 0, 0x104);
	sprintf(filter, "%s(*.%s) *.%s", desc, ext, ext);
	if(mode != 0 && mode != 1)
		return 4;
	ok = OS_FileDialog(mode, path, 0x104, filter, ext, initialDir, title);
	return ok ? 0 : -1;
}

char gEmptyPath[8];

/* "80 24" / "80 25": the files matching `pattern`, their names (with
 * `prefix` in front when given) stored into out[] when out is not NULL,
 * at most `max`; sub-directories are entered when `recurse` is set, with
 * their name added to the prefix.  The count. */
int FindFiles(char** out, const char* pattern, int recurse, const char* prefix, int max)
{
	OsFindData_t fd;
	OsFind_t* h = OS_FindFirst(pattern, &fd);
	int count = 0;
	if(!h)
		return 0;
	do
	{
		if(fd.attrs & OS_ATTR_DIRECTORY)
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
			n = FindFiles(out, sub, 1, subPrefix, max - count);
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
	} while(OS_FindNext(h, &fd) && count < max);
	OS_FindClose(h);
	return count;
}
