/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * machine.c - what the Win32 back end reports about the machine and does
 *             with other processes (inc/bgi/os.h): memory, the computer
 *             and user names, CPUID and the time-stamp counter, the OS
 *             version, the display adapter; starting and waiting for
 *             processes, ShellExecute, the clipboard, the wallpaper and
 *             shortcuts
 *
 * Each function is the API call os.h names in its wide-character form
 * (strings converted through wide.c), with two substitutions: the
 * CPUID and RDTSC instructions the original issues inline come from the
 * compiler's intrinsics (BGI_X86, sys_internal.h), and the display adapter
 * the original asks Direct3D for is taken from EnumDisplayDevices, which
 * has no driver version to offer.
 */
#include "sys_internal.h"

// ---- machine identity --------------------------------------------------------------------------

// GlobalMemoryStatus, every field as the 32-bit API reports it ("80 0D" reads totalPhys and availPhys)
void OS_MemoryStatus(OsMemStatus_t* m)
{
	MEMORYSTATUS ms;
	ms.dwLength = sizeof ms;
	GlobalMemoryStatus(&ms);
	m->loadPercent = ms.dwMemoryLoad;
	m->totalPhys = (uint32_t)ms.dwTotalPhys;
	m->availPhys = (uint32_t)ms.dwAvailPhys;
	m->totalPageFile = (uint32_t)ms.dwTotalPageFile;
	m->availPageFile = (uint32_t)ms.dwAvailPageFile;
	m->totalVirtual = (uint32_t)ms.dwTotalVirtual;
	m->availVirtual = (uint32_t)ms.dwAvailVirtual;
}

// the names as Shift-JIS (the installer hashes them into the machine key); 0 when the call fails or buf (n bytes) is too small, as the API has it
int OS_ComputerName(char* buf, size_t n)
{
	WCHAR wide[0x104];
	DWORD len = (DWORD)BGI_COUNTOF(wide);
	if(!n || !GetComputerNameW(wide, &len))
		return 0;
	return WideCharToMultiByte(WIN32_CP_SJIS, 0, wide, -1, buf, (int)n, NULL, NULL) > 0;
}

int OS_UserName(char* buf, size_t n)
{
	WCHAR wide[0x104];
	DWORD len = (DWORD)BGI_COUNTOF(wide);
	if(!n || !GetUserNameW(wide, &len))
		return 0;
	return WideCharToMultiByte(WIN32_CP_SJIS, 0, wide, -1, buf, (int)n, NULL, NULL) > 0;
}

/* CPUID of the leaf (sub-leaf 0) into out[] as eax, ebx, ecx, edx; 0 when
 * the processor has no CPUID or the leaf is above the highest one of its
 * range (basic or extended, by bit 31).  Always 0 when the compiler has no
 * CPUID intrinsics (no BGI_X86). */
int OS_Cpuid(uint32_t leaf, uint32_t out[4])
{
#ifdef BGI_X86
	unsigned a, b, c, d;
	if(!__get_cpuid_max(leaf & 0x80000000u, NULL) || __get_cpuid_max(leaf & 0x80000000u, NULL) < (leaf & 0x7fffffffu))
		return 0;
	__cpuid_count(leaf, 0, a, b, c, d);
	out[0] = a;
	out[1] = b;
	out[2] = c;
	out[3] = d;
	return 1;
#else
	BGI_UNUSED(leaf);
	BGI_UNUSED(out);
	return 0;
#endif
}

// RDTSC after a serialising CPUID, as the original issues it; 0 without the intrinsics
uint64_t OS_ReadTsc(void)
{
#ifdef BGI_X86
	unsigned a, b, c, d;
	__cpuid(0, a, b, c, d); // serialise, as the original does before rdtsc
	return __rdtsc();
#else
	return 0;
#endif
}

// CPUID.1:EAX (family, model, stepping), 0 without CPUID
uint32_t OS_CpuSignature(void)
{
	uint32_t r[4];
	if(!OS_Cpuid(1, r))
		return 0;
	return r[0];
}

// GetVersionEx: the platform id (VER_PLATFORM_WIN32_NT = 2) and the major / minor version; 0 when the call fails
int OS_UiLanguage(void)
{
	return (int)GetUserDefaultUILanguage();
}

int OS_Version(int* platform, int* major, int* minor)
{
	OSVERSIONINFOW vi;
	vi.dwOSVersionInfoSize = sizeof vi;
	if(!GetVersionExW(&vi))
		return 0;
	*platform = (int)vi.dwPlatformId;
	*major = (int)vi.dwMajorVersion;
	*minor = (int)vi.dwMinorVersion;
	return 1;
}

// "81 0C": major, minor, build and platform id into out[], the service pack string into csd (n bytes)
void OS_VersionEx(uint32_t out[4], char* csd, size_t n)
{
	OSVERSIONINFOW vi;
	memset(&vi, 0, sizeof vi);
	vi.dwOSVersionInfoSize = sizeof vi;
	GetVersionExW(&vi);
	out[0] = vi.dwMajorVersion;
	out[1] = vi.dwMinorVersion;
	out[2] = vi.dwBuildNumber;
	out[3] = vi.dwPlatformId;
	if(n)
		Win32_TextFromWide(vi.szCSDVersion, csd, (int)n);
}

// "81 0D": GlobalMemoryStatusEx, the physical memory in bytes
void OS_MemoryStatusEx(uint64_t* totalPhys, uint64_t* availPhys)
{
	MEMORYSTATUSEX ms;
	memset(&ms, 0, sizeof ms);
	ms.dwLength = sizeof ms;
	GlobalMemoryStatusEx(&ms);
	*totalPhys = ms.ullTotalPhys;
	*availPhys = ms.ullAvailPhys;
}

/* "81 0B": the description of the primary display device from
 * EnumDisplayDevices into desc (n bytes); 1 when a device was found.  The
 * driver version the original takes from the Direct3D adapter identifier
 * is not available this way: version[] is always zero. */
int OS_DisplayAdapter(char* desc, size_t n, uint32_t version[4])
{
	// the primary display device (the original asks Direct3D)
	DISPLAY_DEVICEW dd;
	memset(&dd, 0, sizeof dd);
	dd.cb = sizeof dd;
	version[0] = version[1] = version[2] = version[3] = 0;
	if(!EnumDisplayDevicesW(NULL, 0, &dd, 0))
		return 0;
	if(n)
		Win32_TextFromWide(dd.DeviceString, desc, (int)n);
	return 1;
}

// ---- processes ---------------------------------------------------------------------------------

struct OsProcess
{
	PROCESS_INFORMATION pi; // the process and primary thread handles CreateProcess returned
};

// CreateProcess with the command line alone (no application name, the current directory and environment inherited); NULL when it fails
OsProcess_t* OS_ProcessStart(const char* cmdLine)
{
	STARTUPINFOW si;
	OsProcess_t* p = (OsProcess_t*)calloc(1, sizeof *p);
	WCHAR cmd[0x400];
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	Win32_ToWide(cmdLine, cmd, (int)BGI_COUNTOF(cmd)); // a buffer of its own: CreateProcess may write into the command line
	if(!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &p->pi))
	{
		free(p);
		return NULL;
	}
	return p;
}

void OS_ProcessWaitIdle(OsProcess_t* p)
{
	WaitForInputIdle(p->pi.hProcess, INFINITE);
}

// 1 once the process has ended, 0 when it is still running after `ms` milliseconds
int OS_ProcessWait(OsProcess_t* p, uint32_t ms)
{
	return WaitForSingleObject(p->pi.hProcess, ms) == WAIT_OBJECT_0;
}

// close both handles and free the record; the process itself goes on
void OS_ProcessClose(OsProcess_t* p)
{
	if(!p)
		return;
	CloseHandle(p->pi.hThread);
	CloseHandle(p->pi.hProcess);
	free(p);
}

// ShellExecute "open" with SW_SHOWNORMAL; a result above 32 means it started (the values up to 32 are error codes)
int OS_ShellExecute(const char* file, const char* params, const char* dir)
{
	WCHAR* wfile = Win32_ToWideDup(file);
	WCHAR* wparams = Win32_ToWideDup(params); // NULL stays NULL
	WCHAR* wdir = Win32_ToWideDup(dir);
	int ok = (INT_PTR)ShellExecuteW(NULL, L"open", wfile, wparams, wdir, SW_SHOWNORMAL) > 32;
	free(wfile);
	free(wparams);
	free(wdir);
	return ok;
}

// "80 05" of 1.69 build 472 on: counter / frequency * 1e9 in double precision, truncated to an integer; 0 without a performance counter
int OS_PerfCounterNs(int64_t* out)
{
	LARGE_INTEGER f, c;
	if(!QueryPerformanceFrequency(&f))
		return 0;
	QueryPerformanceCounter(&c);
	*out = (int64_t)((double)c.QuadPart / (double)f.QuadPart * 1000000000.0);
	return 1;
}

/* "7E" of 1.69 build 472 on: a GlobalAlloc'd copy of the text (with its
 * NUL) handed to the clipboard, replacing its contents - as
 * CF_UNICODETEXT, from which the system makes CF_TEXT for a program that
 * asks for it; 1 when it was set, 0 when the allocation fails or the
 * clipboard cannot be opened. */
int OS_ClipboardSetText(const char* text)
{
	int cap = (int)strlen(text) + 1; // UTF-16 units: never more than the bytes
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, (size_t)cap * sizeof(WCHAR));
	WCHAR* p;
	if(!h)
		return 0;
	p = (WCHAR*)GlobalLock(h);
	Win32_ToWide(text, p, cap);
	GlobalUnlock(h);
	if(!OpenClipboard(gWin32MainWnd))
	{
		GlobalFree(h);
		return 0;
	}
	EmptyClipboard();
	SetClipboardData(CF_UNICODETEXT, h);
	CloseClipboard();
	return 1;
}

/* "B0 F0": WallpaperStyle ("2" stretched, "0" centred) and TileWallpaper
 * ("1" / "0") under HKCU\control panel\desktop, then
 * SystemParametersInfo(SPI_SETDESKWALLPAPER) with the file, updating the
 * user profile and broadcasting the change; its result.  A registry key
 * that cannot be opened is skipped silently, as in the original. */
int OS_SetWallpaper(const char* path, int stretch, int tile)
{
	HKEY key;
	WCHAR wide[WIN32_WPATH];
	if(RegOpenKeyExW(HKEY_CURRENT_USER, L"control panel\\desktop", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
	{
		const WCHAR* style = stretch ? L"2" : L"0";
		const WCHAR* tiled = tile ? L"1" : L"0";
		// one character and its NUL, in bytes
		RegSetValueExW(key, L"WallpaperStyle", 0, REG_SZ, (const BYTE*)style, 2 * sizeof(WCHAR));
		RegSetValueExW(key, L"TileWallpaper", 0, REG_SZ, (const BYTE*)tiled, 2 * sizeof(WCHAR));
		RegCloseKey(key);
	}
	Win32_ToWide(path, wide, (int)BGI_COUNTOF(wide));
	return SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, wide, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) != 0;
}

/* A shortcut file through IShellLink (1.494 on with arguments): the target
 * path, the arguments when `args` is not NULL, nothing else, saved with
 * IPersistFile at linkPath; 1 when the save succeeded.  Needs COM
 * (OS_Init). */
int OS_CreateShortcutArgs(const char* linkPath, const char* target, const char* args)
{
	IShellLinkW* link = NULL;
	IPersistFile* file = NULL;
	WCHAR wide[WIN32_WPATH];
	HRESULT hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void**)&link);
	int ok = 0;
	if(FAILED(hr))
		return 0;
	Win32_ToWide(target, wide, (int)BGI_COUNTOF(wide));
	IShellLinkW_SetPath(link, wide);
	if(args)
	{
		Win32_ToWide(args, wide, (int)BGI_COUNTOF(wide));
		IShellLinkW_SetArguments(link, wide);
	}
	if(SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void**)&file)))
	{
		Win32_ToWide(linkPath, wide, (int)BGI_COUNTOF(wide));
		ok = SUCCEEDED(IPersistFile_Save(file, wide, TRUE));
		IPersistFile_Release(file);
	}
	IShellLinkW_Release(link);
	return ok;
}

int OS_CreateShortcut(const char* linkPath, const char* target) // IShellLink: the target only
{
	return OS_CreateShortcutArgs(linkPath, target, NULL);
}
