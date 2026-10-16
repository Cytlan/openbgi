/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * process.c - the Win32 back end's process services (inc/bgi/os.h):
 *             start-up and shutdown, the code page conversions, the
 *             executable's directory, the instance mutex and the string
 *             a second instance passes to the running one
 *
 * These are the calls the original makes, in their wide-character forms:
 * the engine's Shift-JIS strings are converted to UTF-16 on the way in and
 * back on the way out (wide.c), so nothing depends on the system's code
 * page.  The reimplementation's "native" paths (os.h) are the engine's own
 * paths here; a directory code page 932 cannot spell is reached through
 * its path alias.  See os.h for the contract and the POSIX back end for
 * the counterpart.
 */
#include "sys_internal.h"
#include <wchar.h>

HINSTANCE gWin32Instance;  // the module handle WinMain received
static int gCoInitialised; // CoInitialize succeeded in OS_Init: CoUninitialize at shutdown

// the module handle when WinMain did not set it, and COM for the shell and DirectShow calls; always 1
int OS_Init(void)
{
	if(!gWin32Instance)
		gWin32Instance = GetModuleHandleW(NULL);
	gCoInitialised = SUCCEEDED(CoInitialize(NULL)); // WinMain initialises COM for the whole run
	return 1;
}

void OS_Shutdown(void)
{
	if(gCoInitialised)
		CoUninitialize();
	gCoInitialised = 0;
}

/* UTF-8 to code page 932 through a UTF-16 intermediate of up to 0x400
 * characters; a character without a Shift-JIS code becomes '?' (the code
 * page's default character).  The length written; out (n bytes) gets an
 * empty string and the result is 0 when either conversion fails. */
size_t OS_Utf8ToSjis(const char* utf8, char* out, size_t n)
{
	WCHAR wide[0x400];
	int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)BGI_COUNTOF(wide));
	int len;
	if(wn <= 0)
	{
		if(n)
			out[0] = 0;
		return 0;
	}
	len = WideCharToMultiByte(WIN32_CP_SJIS, 0, wide, -1, out, (int)n, NULL, NULL);
	if(len <= 0)
	{
		if(n)
			out[0] = 0;
		return 0;
	}
	return (size_t)len - 1;
}

// the code page 932 table for one two-byte code (lead byte in the high half); U+FFFD when it is not a valid character
uint32_t OS_SjisToCodePoint(uint16_t sjis)
{
	char in[2] = {(char)(sjis >> 8), (char)sjis};
	WCHAR wide[4];
	int wn = MultiByteToWideChar(WIN32_CP_SJIS, MB_ERR_INVALID_CHARS, in, 2, wide, (int)BGI_COUNTOF(wide));
	return wn == 1 ? wide[0] : 0xfffd;
}

/* Code page 932 to UTF-8 through a UTF-16 intermediate of up to 0x400
 * characters; the length written, 0 (and an empty or truncated string in
 * out) when a conversion fails. */
size_t OS_SjisToUtf8(const char* sjis, char* out, size_t n)
{
	WCHAR wide[0x400];
	int wn = MultiByteToWideChar(WIN32_CP_SJIS, 0, sjis, -1, wide, (int)BGI_COUNTOF(wide));
	int len;
	if(wn <= 0)
	{
		if(n)
			out[0] = 0;
		return 0;
	}
	len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)n, NULL, NULL);
	if(len <= 0)
	{
		if(n)
			out[n - 1] = 0;
		return 0;
	}
	return (size_t)len - 1;
}

static char gGameDir[WIN32_WPATH]; // the game directory when the launcher chose one (an engine path with its trailing '\\'); "" for the binary's

void OS_BinaryDir(char* buf, size_t n) // GetModuleFileName up to and including the last '\\'
{
	WCHAR path[WIN32_WPATH];
	int len = (int)GetModuleFileNameW(gWin32Instance, path, (DWORD)BGI_COUNTOF(path) - 1);
	path[len] = 0;
	while(len > 0 && path[len - 1] != '\\' && path[len - 1] != '/')
		len--;
	path[len] = 0;
	if(n)
		Win32_PathFromWide(path, buf, (int)n);
}

void OS_SetArgv0(const char* argv0) // GetModuleFileName knows the binary: nothing to keep
{
	BGI_UNUSED(argv0);
}

void OsWin32_CommandLine(char* out, size_t n)
{
	if(n)
		Win32_CmdLineFromWide(GetCommandLineW(), out, (int)n);
}

// the game directory: the one chosen through OS_SetGameDir, else the binary's own, as the original has it
void OS_ExeDir(char* buf, size_t n)
{
	if(gGameDir[0])
		snprintf(buf, n, "%s", gGameDir);
	else
		OS_BinaryDir(buf, n);
}

void OS_GameDir(char* buf, size_t n) // native paths are the engine's paths here
{
	OS_ExeDir(buf, n);
}

int OS_SetGameDir(const char* dir)
{
	WCHAR wide[WIN32_WPATH];
	DWORD attrs;
	size_t len;
	if(!dir || !*dir)
		return 0;
	Win32_ToWide(dir, wide, (int)BGI_COUNTOF(wide));
	attrs = GetFileAttributesW(wide);
	if(attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
		return 0;
	snprintf(gGameDir, sizeof gGameDir, "%s", dir);
	len = strlen(gGameDir);
	if(len && OsCommon_FilePart(gGameDir)[0] && len + 1 < sizeof gGameDir)
		strcat(gGameDir, "\\"); // no separator at the end yet
	Win32_ToWide(gGameDir, wide, (int)BGI_COUNTOF(wide));
	return SetCurrentDirectoryW(wide) != 0;
}

/* A native path to UTF-8 and back (games.json keeps its directories in
 * UTF-8): through UTF-16, so a path alias becomes the directory's real
 * name in the file and a real name read from it gets its alias again. */
void OS_NativeToUtf8(const char* native, char* out, size_t n)
{
	WCHAR wide[0x800];
	if(!n)
		return;
	Win32_ToWide(native, wide, (int)BGI_COUNTOF(wide));
	Win32_WideToUtf8(wide, out, (int)n);
}

void OS_Utf8ToNative(const char* utf8, char* out, size_t n)
{
	WCHAR wide[0x800];
	if(!n)
		return;
	Win32_Utf8ToWide(utf8, wide, (int)BGI_COUNTOF(wide));
	Win32_PathFromWide(wide, out, (int)n);
}

// fopen of a native path: _wfopen of the converted path (the mode is ASCII)
FILE* OS_NativeOpen(const char* path, const char* mode)
{
	WCHAR wide[WIN32_WPATH], wmode[8];
	Win32_ToWide(path, wide, (int)BGI_COUNTOF(wide));
	Win32_ToWide(mode, wmode, (int)BGI_COUNTOF(wmode));
	return _wfopen(wide, wmode);
}

uint8_t* OS_ReadNativeFile(const char* path, uint32_t* size)
{
	FILE* f = OS_NativeOpen(path, "rb");
	long len;
	uint8_t* buf;
	*size = 0;
	if(!f)
		return NULL;
	if(fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
	{
		fclose(f);
		return NULL;
	}
	buf = (uint8_t*)malloc((size_t)len + 1);
	if(!buf || fread(buf, 1, (size_t)len, f) != (size_t)len)
	{
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[len] = 0;
	*size = (uint32_t)len;
	return buf;
}

int OS_WriteNativeFile(const char* path, const void* data, uint32_t size)
{
	FILE* f = OS_NativeOpen(path, "wb");
	int ok;
	if(!f)
		return 0;
	ok = fwrite(data, 1, size, f) == size;
	ok = fclose(f) == 0 && ok;
	return ok;
}

int OS_SetCurrentDir(const char* dir)
{
	WCHAR wide[WIN32_WPATH];
	Win32_ToWide(dir, wide, (int)BGI_COUNTOF(wide));
	return SetCurrentDirectoryW(wide) != 0;
}

// ---- the instance mutex ------------------------------------------------------------------------

struct OsMutex
{
	HANDLE h; // the named mutex; NULL when CreateMutex failed
};

/* CreateMutex of the name without taking ownership; *alreadyExists is 1
 * when another process created it first (ERROR_ALREADY_EXISTS).  The
 * record is returned even then so the caller releases it the same way. */
OsMutex_t* OS_InstanceMutexCreate(const char* name, int* alreadyExists)
{
	OsMutex_t* m = (OsMutex_t*)calloc(1, sizeof *m);
	WCHAR wide[0x200];
	Win32_ToWide(name, wide, (int)BGI_COUNTOF(wide));
	m->h = CreateMutexW(NULL, FALSE, wide);
	*alreadyExists = GetLastError() == ERROR_ALREADY_EXISTS;
	return m;
}

void OS_InstanceMutexRelease(OsMutex_t* m)
{
	if(!m)
		return;
	if(m->h)
	{
		ReleaseMutex(m->h);
		CloseHandle(m->h);
	}
	free(m);
}

// 1 when a mutex of the name exists (OpenMutex succeeds): another program holds it
int OS_NamedMutexExists(const char* name)
{
	WCHAR wide[0x200];
	HANDLE h;
	Win32_ToWide(name, wide, (int)BGI_COUNTOF(wide));
	h = OpenMutexW(SYNCHRONIZE, FALSE, wide);
	if(!h)
		return 0;
	CloseHandle(h);
	return 1;
}

// ---- inter-process string ----------------------------------------------------------------------

/* The running instance is found by its window class; the string travels in
 * a named file mapping "FMO%.8xForBGI" whose random key is the wParam of
 * message 0x9000 (BGI_WM_IPC, os_win32.h) and whose length (with the NUL)
 * is the lParam.  The message is sent, not posted, so the mapping may be
 * closed as soon as it returns.
 *
 * The string is the engine's own bytes, as the original sends them.  It
 * is a path, and a path alias means nothing to another process, so the
 * mapping carries the path once more behind the string's NUL: the mark
 * IPC_WIDE_MARK and the real path in UTF-16, which the receiver converts
 * for itself.  The original's receiver never looks there.  Everything
 * stays within the mapping's first page (IPC_PAGE). */
#define IPC_WIDE_MARK "W16" // four bytes with its NUL
#define IPC_PAGE      0x1000

// 1 when such a window exists and the mapping could be created; 0 otherwise
int OS_IpcSendToRunning(const char* windowClass, const char* data)
{
	WCHAR wide[0x100], real[WIN32_WPATH];
	HWND hwnd;
	uint32_t key, len, realBytes, total;
	char name[64];
	HANDLE map;
	uint8_t* view;
	Win32_ToWide(windowClass, wide, (int)BGI_COUNTOF(wide));
	hwnd = FindWindowW(wide, NULL);
	if(!hwnd || !IsWindow(hwnd))
		return 0;
	len = (uint32_t)strlen(data) + 1;
	realBytes = (uint32_t)(Win32_ToWide(data, real, (int)BGI_COUNTOF(real)) + 1) * (uint32_t)sizeof(WCHAR);
	total = len + 4 + realBytes;
	if(total > IPC_PAGE)
		total = len; // too long to carry twice: the string alone
	key = ((uint32_t)rand() << 20) ^ ((uint32_t)rand() << 10) ^ (uint32_t)rand();
	sprintf(name, "FMO%.8xForBGI", key);
	Win32_ToWide(name, wide, (int)BGI_COUNTOF(wide));
	map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, total, wide);
	if(!map)
		return 0;
	view = (uint8_t*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if(!view)
	{
		CloseHandle(map);
		return 0;
	}
	memcpy(view, data, len);
	if(total > len)
	{
		memcpy(view + len, IPC_WIDE_MARK, 4);
		memcpy(view + len + 4, real, realBytes);
	}
	UnmapViewOfFile(view);
	SendMessageW(hwnd, BGI_WM_IPC, key, len);
	CloseHandle(map);
	return 1;
}

/* The receiving side, called by the main window procedure on BGI_WM_IPC:
 * open the mapping of the same name (CreateFileMapping on an existing name
 * returns the sender's) and copy `len` bytes into out, NUL-terminated at
 * len - 1 whatever the sender wrote; when the path in UTF-16 follows
 * (IPC_WIDE_MARK), out receives that path converted here instead, so that
 * a directory this process cannot spell gets its alias in this process.
 * 0 when len is 0 or above n, or when the mapping cannot be opened. */
int OsWin32_IpcReceive(uint32_t key, uint32_t len, char* out, size_t n)
{
	char name[64];
	WCHAR wide[64];
	HANDLE map;
	const uint8_t* view;
	if(len == 0 || len > n)
		return 0;
	sprintf(name, "FMO%.8xForBGI", key);
	Win32_ToWide(name, wide, (int)BGI_COUNTOF(wide));
	map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, len, wide);
	if(!map)
		return 0;
	view = (const uint8_t*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if(!view)
	{
		CloseHandle(map);
		return 0;
	}
	memcpy(out, view, len);
	out[len - 1] = 0;
	// a view is whole pages, zero-filled behind the data: the mark can be looked for without knowing the mapping's size
	if(len + 4 + sizeof(WCHAR) <= IPC_PAGE && memcmp(view + len, IPC_WIDE_MARK, 4) == 0)
	{
		WCHAR real[WIN32_WPATH];
		size_t units = (IPC_PAGE - len - 4) / sizeof(WCHAR);
		if(units > BGI_COUNTOF(real) - 1)
			units = BGI_COUNTOF(real) - 1;
		memcpy(real, view + len + 4, units * sizeof(WCHAR));
		real[units] = 0;
		Win32_PathFromWide(real, out, (int)n);
	}
	UnmapViewOfFile(view);
	CloseHandle(map);
	return 1;
}
