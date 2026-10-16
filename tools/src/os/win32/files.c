/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * files.c - files, directories and drives of the Win32 back end
 *           (inc/bgi/os.h): the OsFile_t operations over HANDLEs, file
 *           attributes and times, directory enumeration, drive types and
 *           media detection
 *
 * Every function is the Win32 call os.h names it after in its
 * wide-character form, with the engine's Shift-JIS path converted to
 * UTF-16 first (WPATH below; wide.c) and the names Windows reports
 * converted back; the result conventions (0 for failure,
 * OS_INVALID_ATTRS, OS_INVALID_FILE) are the API's own.
 */
#include "sys_internal.h"

// declare `name` as the UTF-16 form of the engine path `path`
#define WPATH(name, path)    \
	WCHAR name[WIN32_WPATH]; \
	Win32_ToWide((path), name, (int)BGI_COUNTOF(name))

// ---- files -------------------------------------------------------------------------------------

struct OsFile
{
	HANDLE h; // the CreateFile handle
};

// GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING; NULL when the file cannot be opened
OsFile_t* OS_FileOpenRead(const char* path)
{
	WPATH(wide, path);
	HANDLE h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	OsFile_t* f;
	if(h == INVALID_HANDLE_VALUE)
		return NULL;
	f = (OsFile_t*)calloc(1, sizeof *f);
	f->h = h;
	return f;
}

// GENERIC_WRITE, no sharing, CREATE_ALWAYS (an existing file is truncated); NULL on failure
OsFile_t* OS_FileCreate(const char* path)
{
	WPATH(wide, path);
	HANDLE h = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	OsFile_t* f;
	if(h == INVALID_HANDLE_VALUE)
		return NULL;
	f = (OsFile_t*)calloc(1, sizeof *f);
	f->h = h;
	return f;
}

void OS_FileClose(OsFile_t* f)
{
	if(!f)
		return;
	CloseHandle(f->h);
	free(f);
}

// the bytes ReadFile delivered: fewer than n at the end of the file, 0 at the end or on an error
uint32_t OS_FileRead(OsFile_t* f, void* buf, uint32_t n)
{
	DWORD got = 0;
	if(!ReadFile(f->h, buf, n, &got, NULL))
		return 0;
	return got;
}

// the bytes WriteFile accepted, 0 on an error
uint32_t OS_FileWrite(OsFile_t* f, const void* buf, uint32_t n)
{
	DWORD put = 0;
	if(!WriteFile(f->h, buf, n, &put, NULL))
		return 0;
	return put;
}

// SetFilePointer from the start; 1 = ok (a position of 0xFFFFFFFF cannot be told from a failure, as in the original)
int OS_FileSeek(OsFile_t* f, uint32_t pos)
{
	return SetFilePointer(f->h, (LONG)pos, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
}

uint32_t OS_FileSize(OsFile_t* f)
{
	return GetFileSize(f->h, NULL);
}

// the last-write FILETIME split into its two dwords; 0 when GetFileTime fails
int OS_FileGetTime(OsFile_t* f, uint32_t* lo, uint32_t* hi)
{
	FILETIME ft;
	if(!GetFileTime(f->h, NULL, NULL, &ft))
		return 0;
	*lo = ft.dwLowDateTime;
	*hi = ft.dwHighDateTime;
	return 1;
}

// set the last-write time alone (creation and access times untouched); 1 = ok
int OS_FileSetTime(OsFile_t* f, uint32_t lo, uint32_t hi)
{
	FILETIME ft;
	ft.dwLowDateTime = lo;
	ft.dwHighDateTime = hi;
	return SetFileTime(f->h, NULL, NULL, &ft) != 0;
}

uint32_t OS_FileAttrs(const char* path)
{
	WPATH(wide, path);
	return GetFileAttributesW(wide);
}

int OS_FileSetAttrs(const char* path, uint32_t attrs)
{
	WPATH(wide, path);
	return SetFileAttributesW(wide, attrs) != 0;
}

int OS_FileDelete(const char* path)
{
	WPATH(wide, path);
	return DeleteFileW(wide) != 0;
}

int OS_FileMove(const char* from, const char* to)
{
	WPATH(wfrom, from);
	WPATH(wto, to);
	return MoveFileW(wfrom, wto) != 0;
}

int OS_FileCopy(const char* from, const char* to, int failIfExists)
{
	WPATH(wfrom, from);
	WPATH(wto, to);
	return CopyFileW(wfrom, wto, failIfExists) != 0;
}

int OS_DirCreate(const char* path)
{
	WPATH(wide, path);
	return CreateDirectoryW(wide, NULL) != 0;
}

int OS_DirRemove(const char* path)
{
	WPATH(wide, path);
	return RemoveDirectoryW(wide) != 0;
}

// ---- directory enumeration ---------------------------------------------------------------------

struct OsFind
{
	HANDLE h; // the FindFirstFile handle
};

// the attributes and the file name of one match into the caller's record: the name as Shift-JIS, '_' for a character code page 932 lacks
static void FindCopy(const WIN32_FIND_DATAW* fd, OsFindData_t* out)
{
	out->attrs = fd->dwFileAttributes;
	Win32_PathFromWide(fd->cFileName, out->name, (int)sizeof out->name);
}

// FindFirstFile of the pattern; the first match in *out and a handle for OS_FindNext, NULL when nothing matches
OsFind_t* OS_FindFirst(const char* pattern, OsFindData_t* out)
{
	WPATH(wide, pattern);
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(wide, &fd);
	OsFind_t* f;
	if(h == INVALID_HANDLE_VALUE)
		return NULL;
	f = (OsFind_t*)calloc(1, sizeof *f);
	f->h = h;
	FindCopy(&fd, out);
	return f;
}

// the next match in *out; 0 when there is none
int OS_FindNext(OsFind_t* h, OsFindData_t* out)
{
	WIN32_FIND_DATAW fd;
	if(!FindNextFileW(h->h, &fd))
		return 0;
	FindCopy(&fd, out);
	return 1;
}

void OS_FindClose(OsFind_t* h)
{
	if(!h)
		return;
	FindClose(h->h);
	free(h);
}

// ---- drives ------------------------------------------------------------------------------------

uint32_t OS_DriveType(const char* root)
{
	WPATH(wide, root);
	return GetDriveTypeW(wide);
}

/* GetLogicalDriveStrings: the roots ("A:\", ASCII) as bytes with their
 * NULs and the second NUL at the end; the length without that last NUL,
 * or - as the API does - the size needed when buf (n bytes) is too
 * small, 0 on failure. */
uint32_t OS_LogicalDrives(char* buf, uint32_t n)
{
	WCHAR wide[0x200];
	DWORD len = GetLogicalDriveStringsW((DWORD)BGI_COUNTOF(wide), wide), i;
	if(len == 0 || len >= BGI_COUNTOF(wide))
		return 0;
	if(len + 1 > n)
		return len + 1;
	for(i = 0; i <= len; i++)
		buf[i] = (char)wide[i];
	return len;
}

/* 1 when GetVolumeInformation succeeds for the root ("X:\"): a volume is
 * mounted and readable.  Critical errors are suppressed around the call
 * (SEM_FAILCRITICALERRORS) so an empty drive fails quietly instead of
 * raising the system's "insert a disk" box.  The drive-ready check of
 * src/core/paths.c falls back to this when OS_MediaPresent says 0. */
int OS_VolumeReady(const char* root)
{
	WPATH(wide, root);
	WCHAR name[0x104];
	UINT old = SetErrorMode(SEM_FAILCRITICALERRORS);
	BOOL ok = GetVolumeInformationW(wide, name, (DWORD)BGI_COUNTOF(name), NULL, NULL, NULL, NULL, 0);
	SetErrorMode(old);
	return ok != 0;
}

/* The NT branch of the drive-ready check (src/core/paths.c):
 * IOCTL_STORAGE_CHECK_VERIFY on the device "\\.\X:" of the drive letter;
 * 1 when the drive reports a medium.  0 on Windows 9x, when the device
 * cannot be opened or when the IOCTL fails; the caller then tries the
 * volume with OS_VolumeReady. */
int OS_MediaPresent(char driveLetter)
{
	OSVERSIONINFOW vi;
	WCHAR dev[8] = {'\\', '\\', '.', '\\', 0, ':', 0, 0}; // "\\.\X:"
	HANDLE h;
	DWORD n = 0;
	BOOL ok;
	vi.dwOSVersionInfoSize = sizeof vi;
	GetVersionExW(&vi);
	if(vi.dwPlatformId != VER_PLATFORM_WIN32_NT)
		return 0;
	dev[4] = (WCHAR)(uint8_t)driveLetter;
	h = CreateFileW(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
		NULL);
	if(h == INVALID_HANDLE_VALUE)
		return 0;
	ok = DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, NULL, 0, &n, NULL);
	CloseHandle(h);
	return ok != 0;
}
