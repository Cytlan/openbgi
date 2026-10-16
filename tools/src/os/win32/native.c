/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * native.c - the Win32 back end's native file system services for the
 *            command-line tools (inc/bgi/os.h: OS_NativeStat,
 *            OS_NativeMkdir, OS_NativeRemove, OS_NativeList,
 *            OS_NativeSeek, OS_NativeAbsolute, OS_NativeArgs) and
 *            its workers (OS_WorkerLimit, OS_RunWorkers), which run one
 *            after the other here: Windows has no fork
 *
 * A native path is one of the engine's Shift-JIS paths here (see
 * process.c); each call converts it to UTF-16 with wide.c and uses the
 * wide-character API, so a directory the system's code page cannot spell
 * is reached all the same.
 */
#include "sys_internal.h"
#include <wchar.h>
#include <shellapi.h>

int OS_NativeStat(const char* path, OsNativeStat_t* out)
{
	WCHAR wide[WIN32_WPATH];
	WIN32_FILE_ATTRIBUTE_DATA data;
	Win32_ToWide(path, wide, (int)BGI_COUNTOF(wide));
	if(!GetFileAttributesExW(wide, GetFileExInfoStandard, &data))
		return OS_NATIVE_NONE;
	if(out)
	{
		// a FILETIME counts 100 ns units from 1601; 11644473600 seconds lie between that and 1970
		uint64_t t = ((uint64_t)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
		out->size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
		out->mtime = (int64_t)(t / 10000) - (int64_t)11644473600000;
	}
	return (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? OS_NATIVE_DIR : OS_NATIVE_FILE;
}

int OS_NativeMkdir(const char* path)
{
	WCHAR wide[WIN32_WPATH];
	Win32_ToWide(path, wide, (int)BGI_COUNTOF(wide));
	if(CreateDirectoryW(wide, NULL))
		return 1;
	return OS_NativeStat(path, NULL) == OS_NATIVE_DIR;
}

int OS_NativeRemove(const char* path)
{
	WCHAR wide[WIN32_WPATH];
	Win32_ToWide(path, wide, (int)BGI_COUNTOF(wide));
	return DeleteFileW(wide) || OS_NativeStat(path, NULL) == OS_NATIVE_NONE;
}

int OS_NativeList(const char* dir, OsNativeListFn fn, void* ctx)
{
	WCHAR wide[WIN32_WPATH];
	WIN32_FIND_DATAW data;
	HANDLE h;
	int len, count = 0;
	len = Win32_ToWide(dir[0] ? dir : ".", wide, (int)BGI_COUNTOF(wide) - 3);
	if(len && wide[len - 1] != '\\' && wide[len - 1] != '/')
		wide[len++] = '\\';
	wide[len++] = '*';
	wide[len] = 0;
	h = FindFirstFileW(wide, &data);
	if(h == INVALID_HANDLE_VALUE)
		return -1;
	do
	{
		char name[0x300];
		if(wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
			continue;
		Win32_PathFromWide(data.cFileName, name, (int)sizeof name);
		fn(ctx, name, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
		count++;
	} while(FindNextFileW(h, &data));
	FindClose(h);
	return count;
}

int OS_NativeSeek(FILE* f, uint64_t pos)
{
	return _fseeki64(f, (long long)pos, SEEK_SET) == 0;
}

void OS_NativeAbsolute(const char* path, char* out, size_t n)
{
	WCHAR wide[WIN32_WPATH], full[WIN32_WPATH];
	DWORD len;
	Win32_ToWide(path, wide, (int)BGI_COUNTOF(wide));
	len = GetFullPathNameW(wide, (DWORD)BGI_COUNTOF(full), full, NULL);
	if(len == 0 || len >= BGI_COUNTOF(full))
		snprintf(out, n, "%s", path);
	else
		Win32_PathFromWide(full, out, (int)n);
}

int OS_WorkerLimit(void)
{
	return 1;
}

int OS_RunWorkers(int n, int (*fn)(int index, void* ctx), void* ctx)
{
	int i, failed = 0;
	for(i = 0; i < n; i++)
		failed += fn(i, ctx) != 0;
	return failed;
}

/* main's arguments were converted with the system's code page, which may
 * not be able to spell a path; the wide command line is split again and
 * every argument converted as a path (wide.c: an alias stands for a
 * directory code page 932 lacks).  The strings are kept for the life of
 * the process. */
void OS_NativeArgs(int* argc, char*** argv)
{
	int n = 0, i;
	LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &n);
	char** out;
	if(!wide || n <= 0)
		return;
	out = (char**)calloc((size_t)n + 1, sizeof *out);
	for(i = 0; out && i < n; i++)
	{
		char buf[WIN32_WPATH * 2];
		Win32_PathFromWide(wide[i], buf, (int)sizeof buf);
		out[i] = (char*)malloc(strlen(buf) + 1);
		if(out[i])
			strcpy(out[i], buf);
	}
	LocalFree(wide);
	if(out)
	{
		*argc = n;
		*argv = out;
	}
}
