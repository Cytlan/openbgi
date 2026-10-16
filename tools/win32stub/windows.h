/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * windows.h - a STUB of the Win32 SDK headers for syntax-checking
 * the files of src/os/win32/ on a machine without MinGW ("make check-win").
 *
 * It declares the types and structures the back end touches with the real
 * member names, and the functions that take or return strings with their
 * real prototypes, so that a narrow string handed to a wide-character
 * entry point (or the reverse) is an error here as it is with the SDK;
 * the check is compiled with -fshort-wchar to give L"" literals the
 * 16-bit units of Windows.  Every other API function is left undeclared
 * (GCC warns about the implicit declaration and goes on).  The ANSI (A)
 * structures and entry points are absent on purpose: the back end does not
 * use them.  Constants the files use that are not listed here are
 * generated into autogen.h with distinct dummy values by
 * tools/win32stub/gen.py.  Nothing here is meant to compile into a
 * working program.
 */
#ifndef WIN32STUB_WINDOWS_H_
#define WIN32STUB_WINDOWS_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define WINAPI
#define CALLBACK
#define TRUE  1
#define FALSE 0
#define CONST const

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef long LONG;
typedef long long LONGLONG;
typedef intptr_t LONG_PTR;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t DWORD_PTR;
typedef intptr_t INT_PTR;
typedef uintptr_t UINT_PTR;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
typedef long HRESULT;
typedef DWORD COLORREF;
typedef wchar_t WCHAR; // 16 bits: the check is compiled with -fshort-wchar
typedef char* LPSTR;
typedef const char* LPCSTR;
typedef WCHAR* LPWSTR;
typedef const WCHAR* LPCWSTR;
typedef void* LPVOID;
typedef void* HANDLE;
typedef void* PVOID;
typedef LONG_PTR OAHWND;
typedef UINT MCIDEVICEID;
typedef HANDLE HWND, HDC, HFONT, HCURSOR, HICON, HBRUSH, HRGN, HBITMAP, HGDIOBJ, HDROP, HWAVEOUT, HINSTANCE, HMODULE, HKEY, HMENU, HPALETTE, HGLOBAL, HIMC;
typedef LRESULT(CALLBACK* WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef INT_PTR(CALLBACK* DLGPROC)(HWND, UINT, WPARAM, LPARAM);
typedef DWORD(WINAPI* LPTHREAD_START_ROUTINE)(LPVOID);

typedef struct
{
	DWORD Data1;
	WORD Data2;
	WORD Data3;
	BYTE Data4[8];
} GUID, IID, CLSID;
typedef const GUID* REFIID;
typedef struct
{
	LONG left, top, right, bottom;
} RECT;
typedef struct
{
	LONG x, y;
} POINT;
typedef struct
{
	LONG cx, cy;
} SIZE;
typedef struct
{
	HWND hwnd;
	UINT message;
	WPARAM wParam;
	LPARAM lParam;
	DWORD time;
	POINT pt;
} MSG;
typedef struct
{
	HDC hdc;
	BOOL fErase;
	RECT rcPaint;
	BOOL fRestore, fIncUpdate;
	BYTE rgbReserved[32];
} PAINTSTRUCT;
typedef struct
{
	UINT cbSize, style;
	WNDPROC lpfnWndProc;
	int cbClsExtra, cbWndExtra;
	HINSTANCE hInstance;
	HICON hIcon;
	HCURSOR hCursor;
	HBRUSH hbrBackground;
	LPCWSTR lpszMenuName, lpszClassName;
	HICON hIconSm;
} WNDCLASSEXW;
typedef struct
{
	WCHAR dmDeviceName[32];
	WORD dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
	DWORD dmFields;
	DWORD dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
} DEVMODEW;
typedef struct
{
	DWORD biSize;
	LONG biWidth, biHeight;
	WORD biPlanes, biBitCount;
	DWORD biCompression, biSizeImage;
	LONG biXPelsPerMeter, biYPelsPerMeter;
	DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;
typedef struct
{
	BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved;
} RGBQUAD;
typedef struct
{
	BITMAPINFOHEADER bmiHeader;
	RGBQUAD bmiColors[1];
} BITMAPINFO;
typedef struct
{
	WORD wFormatTag, nChannels;
	DWORD nSamplesPerSec, nAvgBytesPerSec;
	WORD nBlockAlign, wBitsPerSample, cbSize;
} WAVEFORMATEX;
typedef struct WAVEHDR
{
	LPSTR lpData;
	DWORD dwBufferLength, dwBytesRecorded;
	DWORD_PTR dwUser;
	DWORD dwFlags, dwLoops;
	struct WAVEHDR* lpNext;
	DWORD_PTR reserved;
} WAVEHDR;
typedef struct
{
	DWORD_PTR dwCallback;
	MCIDEVICEID wDeviceID;
	LPCWSTR lpstrDeviceType, lpstrElementName, lpstrAlias;
} MCI_OPEN_PARMSW;
typedef struct
{
	DWORD_PTR dwCallback;
	DWORD dwTimeFormat, dwAudio;
} MCI_SET_PARMS;
typedef struct
{
	DWORD_PTR dwCallback;
} MCI_GENERIC_PARMS;
typedef struct
{
	DWORD_PTR dwCallback, dwReturn;
	DWORD dwItem, dwTrack;
} MCI_STATUS_PARMS;
typedef struct
{
	DWORD_PTR dwCallback;
	DWORD dwFrom, dwTo;
} MCI_PLAY_PARMS;
typedef struct
{
	DWORD lStructSize;
	HWND hwndOwner;
	HINSTANCE hInstance;
	LPCWSTR lpstrFilter;
	LPWSTR lpstrCustomFilter;
	DWORD nMaxCustFilter, nFilterIndex;
	LPWSTR lpstrFile;
	DWORD nMaxFile;
	LPWSTR lpstrFileTitle;
	DWORD nMaxFileTitle;
	LPCWSTR lpstrInitialDir, lpstrTitle;
	DWORD Flags;
	WORD nFileOffset, nFileExtension;
	LPCWSTR lpstrDefExt;
	LPARAM lCustData;
	void* lpfnHook;
	LPCWSTR lpTemplateName;
} OPENFILENAMEW;
typedef struct _ITEMIDLIST* LPITEMIDLIST;
typedef const struct _ITEMIDLIST* LPCITEMIDLIST;
typedef int(CALLBACK* BFFCALLBACK)(HWND, UINT, LPARAM, LPARAM);
typedef struct
{
	HWND hwndOwner;
	LPITEMIDLIST pidlRoot;
	LPWSTR pszDisplayName;
	LPCWSTR lpszTitle;
	UINT ulFlags;
	BFFCALLBACK lpfn;
	LPARAM lParam;
	int iImage;
} BROWSEINFOW;
typedef struct
{
	DWORD dwSize, dwICC;
} INITCOMMONCONTROLSEX;
typedef struct
{
	DWORD style, dwExtendedStyle;
	WORD cdit;
	short x, y, cx, cy;
} DLGTEMPLATE;
typedef struct
{
	DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
	WCHAR szCSDVersion[128];
} OSVERSIONINFOW;
typedef union
{
	struct
	{
		DWORD LowPart;
		LONG HighPart;
	} u;
	LONGLONG QuadPart;
} LARGE_INTEGER;
typedef struct
{
	UINT wPeriodMin, wPeriodMax;
} TIMECAPS;
typedef struct
{
	DWORD dwLength, dwMemoryLoad;
	size_t dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual, dwAvailVirtual;
} MEMORYSTATUS;
typedef struct
{
	DWORD dwLength, dwMemoryLoad;
	unsigned long long ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
} MEMORYSTATUSEX;
typedef struct
{
	DWORD cb;
	WCHAR DeviceName[32];
	WCHAR DeviceString[128];
	DWORD StateFlags;
	WCHAR DeviceID[128];
	WCHAR DeviceKey[128];
} DISPLAY_DEVICEW;
typedef struct
{
	LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
	BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
	WCHAR lfFaceName[32];
} LOGFONTW;
typedef struct
{
	LONG tmHeight, tmAscent, tmDescent;
} TEXTMETRICW;
typedef int(CALLBACK* FONTENUMPROCW)(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM);
typedef struct
{
	DWORD dwLowDateTime, dwHighDateTime;
} FILETIME;
typedef struct
{
	WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;
typedef struct
{
	DWORD dwFileAttributes;
	FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
	DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
	WCHAR cFileName[260];
	WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW;
typedef struct
{
	DWORD dwFileAttributes;
	FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
	DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
typedef struct
{
	DWORD cb;
	LPWSTR lpReserved, lpDesktop, lpTitle;
	DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
	WORD wShowWindow, cbReserved2;
	BYTE* lpReserved2;
	HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW;
typedef struct
{
	HANDLE hProcess, hThread;
	DWORD dwProcessId, dwThreadId;
} PROCESS_INFORMATION;
typedef struct
{
	void* DebugInfo;
	LONG LockCount, RecursionCount;
	HANDLE OwningThread, LockSemaphore;
	ULONG_PTR SpinCount;
} CRITICAL_SECTION;
typedef struct
{
	DWORD dwOemId;
	DWORD dwPageSize;
	void* lpMinimumApplicationAddress;
	void* lpMaximumApplicationAddress;
	DWORD_PTR dwActiveProcessorMask;
	DWORD dwNumberOfProcessors;
} SYSTEM_INFO;
typedef struct
{
	DWORD nLength;
	void* lpSecurityDescriptor;
	BOOL bInheritHandle;
} SECURITY_ATTRIBUTES;
typedef struct
{
	DWORD DeviceType, DeviceNumber, PartitionNumber;
} STORAGE_DEVICE_NUMBER;

// COM interfaces are opaque; their methods are the implicit COBJMACROS calls
typedef struct IUnknown IUnknown;
typedef struct IGraphBuilder IGraphBuilder;
typedef struct IMediaControl IMediaControl;
typedef struct IMediaEventEx IMediaEventEx;
typedef struct IMediaSeeking IMediaSeeking;
typedef struct IBasicAudio IBasicAudio;
typedef struct IVideoWindow IVideoWindow;
typedef struct IShellLinkW IShellLinkW;
typedef struct IPersistFile IPersistFile;
extern const GUID CLSID_FilterGraph, IID_IGraphBuilder, IID_IMediaControl, IID_IMediaEventEx, IID_IVideoWindow,
	IID_IMediaSeeking, IID_IBasicAudio, TIME_FORMAT_MEDIA_TIME, CLSID_ShellLink, IID_IShellLinkW, IID_IPersistFile;

#define RGB(r, g, b)         ((COLORREF)(((BYTE)(r)) | (((WORD)(g)) << 8) | (((DWORD)(b)) << 16)))
#define LOWORD(l)            ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l)            ((WORD)(((DWORD_PTR)(l) >> 16) & 0xffff))
#define MAKELPARAM(l, h)     ((LPARAM)(DWORD)(((WORD)(l)) | ((DWORD)((WORD)(h))) << 16))
#define MAKELRESULT(l, h)    ((LRESULT)(DWORD)(((WORD)(l)) | ((DWORD)((WORD)(h))) << 16))
#define MAKEINTRESOURCEA(i)  ((LPSTR)(ULONG_PTR)((WORD)(i)))
#define MAKEINTRESOURCEW(i)  ((LPWSTR)(ULONG_PTR)((WORD)(i)))
#define IDC_ARROW            MAKEINTRESOURCEA(32512) // without UNICODE the SDK's IDC_* are narrow: a W call needs a cast
#define FAILED(hr)           ((HRESULT)(hr) < 0)
#define SUCCEEDED(hr)        ((HRESULT)(hr) >= 0)
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR) - 1)
#define HWND_TOPMOST         ((HWND) - 1)
#define HKEY_LOCAL_MACHINE   ((HKEY)(ULONG_PTR)0x80000002)
#define HKEY_CURRENT_USER    ((HKEY)(ULONG_PTR)0x80000001)
#define HKEY_CLASSES_ROOT    ((HKEY)(ULONG_PTR)0x80000000)
#define ZeroMemory(p, n)     memset((p), 0, (n))
#define GetWindowLongPtrW    GetWindowLongW
#define SetWindowLongPtrW    SetWindowLongW

/* The functions with strings among their arguments or results, as the SDK
 * declares them (handles are all void* here, so only the string and
 * structure types are checked). */
int MultiByteToWideChar(UINT, DWORD, LPCSTR, int, LPWSTR, int);
int WideCharToMultiByte(UINT, DWORD, LPCWSTR, int, LPSTR, int, LPCSTR, BOOL*);
int lstrlenW(LPCWSTR);
LPWSTR GetCommandLineW(void);
HMODULE GetModuleHandleW(LPCWSTR);
DWORD GetModuleFileNameW(HMODULE, LPWSTR, DWORD);
DWORD GetFileAttributesW(LPCWSTR);
BOOL GetFileAttributesExW(LPCWSTR, int, void*);
DWORD GetFullPathNameW(LPCWSTR, DWORD, LPWSTR, LPWSTR*);
BOOL SetFileAttributesW(LPCWSTR, DWORD);
BOOL SetCurrentDirectoryW(LPCWSTR);
HANDLE CreateFileW(LPCWSTR, DWORD, DWORD, SECURITY_ATTRIBUTES*, DWORD, DWORD, HANDLE);
BOOL DeleteFileW(LPCWSTR);
BOOL MoveFileW(LPCWSTR, LPCWSTR);
BOOL CopyFileW(LPCWSTR, LPCWSTR, BOOL);
BOOL CreateDirectoryW(LPCWSTR, SECURITY_ATTRIBUTES*);
BOOL RemoveDirectoryW(LPCWSTR);
HANDLE FindFirstFileW(LPCWSTR, WIN32_FIND_DATAW*);
BOOL FindNextFileW(HANDLE, WIN32_FIND_DATAW*);
UINT GetDriveTypeW(LPCWSTR);
DWORD GetLogicalDriveStringsW(DWORD, LPWSTR);
BOOL GetVolumeInformationW(LPCWSTR, LPWSTR, DWORD, DWORD*, DWORD*, DWORD*, LPWSTR, DWORD);
BOOL GetVersionExW(OSVERSIONINFOW*);
HANDLE CreateMutexW(SECURITY_ATTRIBUTES*, BOOL, LPCWSTR);
HANDLE OpenMutexW(DWORD, BOOL, LPCWSTR);
HANDLE CreateFileMappingW(HANDLE, SECURITY_ATTRIBUTES*, DWORD, DWORD, DWORD, LPCWSTR);
HANDLE CreateEventW(SECURITY_ATTRIBUTES*, BOOL, BOOL, LPCWSTR);
HANDLE CreateWaitableTimerW(SECURITY_ATTRIBUTES*, BOOL, LPCWSTR);
BOOL CreateProcessW(LPCWSTR, LPWSTR, SECURITY_ATTRIBUTES*, SECURITY_ATTRIBUTES*, BOOL, DWORD, LPVOID, LPCWSTR, STARTUPINFOW*, PROCESS_INFORMATION*);
BOOL GetComputerNameW(LPWSTR, DWORD*);
BOOL GetUserNameW(LPWSTR, DWORD*);
UINT GetWindowsDirectoryW(LPWSTR, UINT);
LONG RegOpenKeyExW(HKEY, LPCWSTR, DWORD, DWORD, HKEY*);
LONG RegQueryValueExW(HKEY, LPCWSTR, DWORD*, DWORD*, BYTE*, DWORD*);
LONG RegCreateKeyExW(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, DWORD, SECURITY_ATTRIBUTES*, HKEY*, DWORD*);
LONG RegSetValueExW(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
LONG RegDeleteKeyW(HKEY, LPCWSTR);
WORD RegisterClassExW(const WNDCLASSEXW*);
WORD GetUserDefaultUILanguage(void);
HWND CreateWindowExW(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
BOOL SetWindowTextW(HWND, LPCWSTR);
int GetWindowTextW(HWND, LPWSTR, int);
LRESULT SendMessageW(HWND, UINT, WPARAM, LPARAM);
BOOL PostMessageW(HWND, UINT, WPARAM, LPARAM);
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM);
LRESULT CallWindowProcW(WNDPROC, HWND, UINT, WPARAM, LPARAM);
BOOL PeekMessageW(MSG*, HWND, UINT, UINT, UINT);
LRESULT DispatchMessageW(const MSG*);
BOOL IsDialogMessageW(HWND, MSG*);
HICON LoadIconW(HINSTANCE, LPCWSTR);
HCURSOR LoadCursorW(HINSTANCE, LPCWSTR);
LONG SetWindowLongW(HWND, int, LONG);
LONG ChangeDisplaySettingsW(DEVMODEW*, DWORD);
int MessageBoxW(HWND, LPCWSTR, LPCWSTR, UINT);
LRESULT SendDlgItemMessageW(HWND, int, UINT, WPARAM, LPARAM);
BOOL SetDlgItemTextW(HWND, int, LPCWSTR);
UINT GetDlgItemTextW(HWND, int, LPWSTR, int);
INT_PTR DialogBoxIndirectParamW(HINSTANCE, const DLGTEMPLATE*, HWND, DLGPROC, LPARAM);
HWND CreateDialogIndirectParamW(HINSTANCE, const DLGTEMPLATE*, HWND, DLGPROC, LPARAM);
HWND FindWindowW(LPCWSTR, LPCWSTR);
HWND WindowFromDC(HDC);
LRESULT SendMessageTimeoutW(HWND, UINT, WPARAM, LPARAM, UINT, UINT, DWORD_PTR*);
BOOL SystemParametersInfoW(UINT, UINT, PVOID, UINT);
UINT MapVirtualKeyW(UINT, UINT);
BOOL EnumDisplayDevicesW(LPCWSTR, DWORD, DISPLAY_DEVICEW*, DWORD);
HFONT CreateFontW(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCWSTR);
BOOL TextOutW(HDC, int, int, LPCWSTR, int);
BOOL GetTextExtentPoint32W(HDC, LPCWSTR, int, SIZE*);
int AddFontResourceW(LPCWSTR);
int EnumFontFamiliesExW(HDC, LOGFONTW*, FONTENUMPROCW, LPARAM, DWORD);
UINT DragQueryFileW(HDROP, UINT, LPWSTR, UINT);
HINSTANCE ShellExecuteW(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, int);
LPITEMIDLIST SHBrowseForFolderW(BROWSEINFOW*);
BOOL SHGetPathFromIDListW(LPCITEMIDLIST, LPWSTR);
BOOL GetOpenFileNameW(OPENFILENAMEW*);
BOOL GetSaveFileNameW(OPENFILENAMEW*);
BOOL PlaySoundW(LPCWSTR, HMODULE, DWORD);
DWORD mciSendCommandW(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
HRESULT IShellLinkW_SetPath(IShellLinkW*, LPCWSTR);
HRESULT IShellLinkW_SetArguments(IShellLinkW*, LPCWSTR);
HRESULT IPersistFile_Save(IPersistFile*, LPCWSTR, BOOL);
HRESULT IGraphBuilder_RenderFile(IGraphBuilder*, LPCWSTR, LPCWSTR);
FILE* _wfopen(const wchar_t*, const wchar_t*);

#include "autogen.h"
#endif
