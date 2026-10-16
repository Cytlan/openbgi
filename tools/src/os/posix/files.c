/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * files.c - files, directories and drives of the POSIX back end
 *           (inc/bgi/os.h): the OsFile_t operations over stdio, file
 *           attributes and times, directory enumeration, and the one
 *           fixed pseudo drive "C:" that stands for the game directory
 *
 * Every path goes through OsPosix_Path first (paths.c), so the engine's
 * Shift-JIS Windows paths land under the game directory with their case
 * corrected.  The Windows attributes are approximated: a directory, the
 * owner's write bit as read-only, a leading '.' as hidden.
 */
#include "posix_internal.h"

// ---- files -------------------------------------------------------------------------------------

struct OsFile
{
	FILE* f;             // the stream
	char path[PATH_MAX]; // the POSIX path it was opened with
};

// fopen "rb" of the converted path; NULL when it cannot be opened
OsFile_t* OS_FileOpenRead(const char* path)
{
	OsFile_t* f = (OsFile_t*)calloc(1, sizeof *f);
	OsPosix_Path(path, f->path, sizeof f->path);
	f->f = fopen(f->path, "rb");
	if(!f->f)
	{
		free(f);
		return NULL;
	}
	return f;
}

// fopen "w+b" (truncating) of the converted path; NULL when it cannot be created
OsFile_t* OS_FileCreate(const char* path)
{
	OsFile_t* f = (OsFile_t*)calloc(1, sizeof *f);
	OsPosix_Path(path, f->path, sizeof f->path);
	f->f = fopen(f->path, "w+b");
	if(!f->f)
	{
		free(f);
		return NULL;
	}
	return f;
}

void OS_FileClose(OsFile_t* f)
{
	if(!f)
		return;
	if(f->f)
		fclose(f->f);
	free(f);
}

uint32_t OS_FileRead(OsFile_t* f, void* buf, uint32_t n)
{
	return (uint32_t)fread(buf, 1, n, f->f);
}

uint32_t OS_FileWrite(OsFile_t* f, const void* buf, uint32_t n)
{
	return (uint32_t)fwrite(buf, 1, n, f->f);
}

int OS_FileSeek(OsFile_t* f, uint32_t pos)
{
	return fseek(f->f, (long)pos, SEEK_SET) == 0;
}

// the size from fstat, cut to 32 bits; 0 on failure
uint32_t OS_FileSize(OsFile_t* f)
{
	struct stat st;
	if(fstat(fileno(f->f), &st) != 0)
		return 0;
	return (uint32_t)st.st_size;
}

// FILETIME (100 ns units since 1601) <-> time_t: the seconds between the two epochs
#define FILETIME_EPOCH_DIFF 11644473600ull

// the modification time as a FILETIME (whole seconds: st_mtime has no finer resolution here)
int OS_FileGetTime(OsFile_t* f, uint32_t* lo, uint32_t* hi)
{
	struct stat st;
	uint64_t ft;
	if(fstat(fileno(f->f), &st) != 0)
		return 0;
	ft = ((uint64_t)st.st_mtime + FILETIME_EPOCH_DIFF) * 10000000ull;
	*lo = (uint32_t)ft;
	*hi = (uint32_t)(ft >> 32);
	return 1;
}

// set the access and modification times from a FILETIME, after flushing so that the write does not update them again
int OS_FileSetTime(OsFile_t* f, uint32_t lo, uint32_t hi)
{
	uint64_t ft = ((uint64_t)hi << 32) | lo;
	struct timespec ts[2];
	fflush(f->f);
	ts[0].tv_sec = ts[1].tv_sec = (time_t)(ft / 10000000ull - FILETIME_EPOCH_DIFF);
	ts[0].tv_nsec = ts[1].tv_nsec = (long)(ft % 10000000ull) * 100;
	return futimens(fileno(f->f), ts) == 0;
}

// GetFileAttributes: directory, read-only (no owner write bit), hidden (a '.' name); OS_ATTR_NORMAL when none applies
uint32_t OS_FileAttrs(const char* path)
{
	char p[PATH_MAX];
	struct stat st;
	uint32_t a = 0;
	OsPosix_Path(path, p, sizeof p);
	if(stat(p, &st) != 0)
		return OS_INVALID_ATTRS;
	if(S_ISDIR(st.st_mode))
		a |= OS_ATTR_DIRECTORY;
	if(!(st.st_mode & S_IWUSR))
		a |= OS_ATTR_READONLY;
	if(OsCommon_FilePart(p)[0] == '.')
		a |= OS_ATTR_HIDDEN;
	return a ? a : OS_ATTR_NORMAL;
}

// SetFileAttributes: only the read-only bit has a counterpart (all write bits off, or the owner's back on)
int OS_FileSetAttrs(const char* path, uint32_t attrs)
{
	char p[PATH_MAX];
	struct stat st;
	OsPosix_Path(path, p, sizeof p);
	if(stat(p, &st) != 0)
		return 0;
	if(attrs & OS_ATTR_READONLY)
		st.st_mode &= (mode_t) ~(S_IWUSR | S_IWGRP | S_IWOTH);
	else
		st.st_mode |= S_IWUSR;
	return chmod(p, st.st_mode) == 0;
}

int OS_FileDelete(const char* path)
{
	char p[PATH_MAX];
	OsPosix_Path(path, p, sizeof p);
	return unlink(p) == 0;
}

int OS_FileMove(const char* from, const char* to)
{
	char a[PATH_MAX], b[PATH_MAX];
	OsPosix_Path(from, a, sizeof a);
	OsPosix_Path(to, b, sizeof b);
	return rename(a, b) == 0;
}

// CopyFile: a plain copy through a 64 KB buffer; 0 when `to` exists and failIfExists is set, or either file fails to open
int OS_FileCopy(const char* from, const char* to, int failIfExists)
{
	char a[PATH_MAX], b[PATH_MAX], buf[0x10000];
	FILE *in, *out;
	size_t n;
	OsPosix_Path(from, a, sizeof a);
	OsPosix_Path(to, b, sizeof b);
	if(failIfExists && access(b, F_OK) == 0)
		return 0;
	in = fopen(a, "rb");
	if(!in)
		return 0;
	out = fopen(b, "wb");
	if(!out)
	{
		fclose(in);
		return 0;
	}
	while((n = fread(buf, 1, sizeof buf, in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in);
	fclose(out);
	return 1;
}

int OS_DirCreate(const char* path)
{
	char p[PATH_MAX];
	OsPosix_Path(path, p, sizeof p);
	return mkdir(p, 0755) == 0;
}

int OS_DirRemove(const char* path)
{
	char p[PATH_MAX];
	OsPosix_Path(path, p, sizeof p);
	return rmdir(p) == 0;
}

// ---- directory enumeration ---------------------------------------------------------------------

struct OsFind
{
	DIR* dir;               // the directory being read
	char dirPath[PATH_MAX]; // its POSIX path, for the stat of each entry
	char pattern[260];      // UTF-8 file part with wildcards
};

// the next entry that matches the pattern into `out` (attributes from stat, OS_ATTR_NORMAL when stat fails); 0 at the end
static int FindFill(OsFind_t* h, OsFindData_t* out)
{
	struct dirent* e;
	while((e = readdir(h->dir)) != NULL)
	{
		char full[PATH_MAX];
		struct stat st;
		if(!OsCommon_WildcardMatch(h->pattern, e->d_name))
			continue;
		snprintf(full, sizeof full, "%s/%s", h->dirPath, e->d_name);
		out->attrs = OS_ATTR_NORMAL;
		if(stat(full, &st) == 0)
		{
			out->attrs = 0;
			if(S_ISDIR(st.st_mode))
				out->attrs |= OS_ATTR_DIRECTORY;
			if(!(st.st_mode & S_IWUSR))
				out->attrs |= OS_ATTR_READONLY;
			if(!out->attrs)
				out->attrs = OS_ATTR_NORMAL;
		}
		// the name goes back to the engine as it is stored (UTF-8 names that are plain ASCII survive unchanged; others are not converted)
		snprintf(out->name, sizeof out->name, "%s", e->d_name);
		return 1;
	}
	return 0;
}

/* open the pattern's directory (the current one without a directory
 * part) and deliver the first match; NULL when the directory cannot be
 * read or nothing matches */
OsFind_t* OS_FindFirst(const char* pattern, OsFindData_t* out)
{
	OsFind_t* h = (OsFind_t*)calloc(1, sizeof *h);
	char win[PATH_MAX], utfPat[260];
	const char* file;
	size_t dirLen;

	// split the Windows pattern into directory and file part before converting
	file = OsCommon_FilePart(pattern);
	dirLen = (size_t)(file - pattern);
	if(dirLen >= sizeof win)
		dirLen = sizeof win - 1;
	memcpy(win, pattern, dirLen);
	win[dirLen] = 0;
	if(dirLen == 0)
		strcpy(h->dirPath, ".");
	else
		OsPosix_Path(win, h->dirPath, sizeof h->dirPath);
	OsPosix_SjisToUtf8(file, utfPat, sizeof utfPat);
	snprintf(h->pattern, sizeof h->pattern, "%s", utfPat);
	h->dir = opendir(h->dirPath);
	if(!h->dir || !FindFill(h, out))
	{
		if(h->dir)
			closedir(h->dir);
		free(h);
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
	closedir(h->dir);
	free(h);
}

// ---- drives: one fixed pseudo drive ------------------------------------------------------------

// "C:" is the game directory and fixed; any other letter has no root (so the CD search finds nothing)
uint32_t OS_DriveType(const char* root)
{
	return (root[0] == 'C' || root[0] == 'c') ? OS_DRIVE_FIXED : OS_DRIVE_NO_ROOT;
}

// the list holds the one drive: "C:\" NUL NUL
uint32_t OS_LogicalDrives(char* buf, uint32_t n)
{
	static const char list[] = "C:\\\0";
	if(n < sizeof list + 1)
		return 0;
	memcpy(buf, list, sizeof list);
	buf[sizeof list] = 0;
	return sizeof list - 1; // "C:\" NUL
}

// the pseudo drive is always ready and always holds its medium
int OS_VolumeReady(const char* root)
{
	return OS_DriveType(root) == OS_DRIVE_FIXED;
}

int OS_MediaPresent(char driveLetter)
{
	return driveLetter == 'C' || driveLetter == 'c';
}
