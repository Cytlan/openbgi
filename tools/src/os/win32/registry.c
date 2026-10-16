/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * registry.c - the registry and the shell's folders for the Win32 back
 *              end (inc/bgi/os.h): "HKLM\\..." paths to the root handles,
 *              string values, keys, and the special folders the
 *              installer writes into
 *
 * Key names arrive as os.h describes them: "HKLM\", "HKCU\" or "HKCR\"
 * and the path below the hive; a name without a prefix is taken under
 * HKEY_CURRENT_USER.  Only REG_SZ values are read and written; names
 * and values go through the wide-character calls, converted from and to
 * the engine's Shift-JIS (wide.c).  A value is converted as a path, since
 * the values the installer keeps are paths: one the engine could not
 * spell comes back under its alias.
 */
#include "sys_internal.h"

// split "HKLM\...", "HKCU\...", "HKCR\..." into the root handle and the sub key; no prefix: HKEY_CURRENT_USER and the whole name
static HKEY RegRoot(const char* key, const char** sub)
{
	HKEY root = HKEY_CURRENT_USER;
	if(strncmp(key, "HKLM\\", 5) == 0)
		root = HKEY_LOCAL_MACHINE;
	else if(strncmp(key, "HKCR\\", 5) == 0)
		root = HKEY_CLASSES_ROOT;
	else if(strncmp(key, "HKCU\\", 5) != 0)
	{
		*sub = key;
		return root;
	}
	*sub = key + 5;
	return root;
}

/* RegQueryValueEx of `value` under the key into buf (n bytes); 1 when the
 * key opens and the value is a REG_SZ.  The result is NUL-terminated even
 * when the stored string was not or did not fit. */
int OS_RegReadString(const char* key, const char* value, char* buf, size_t n)
{
	const char* sub;
	HKEY root = RegRoot(key, &sub), h;
	WCHAR wsub[WIN32_WPATH], wvalue[0x100], data[0x800];
	DWORD type = 0, len = (DWORD)(sizeof data - sizeof(WCHAR)); // bytes, with room for a NUL the stored string may lack
	int ok = 0;
	if(!n)
		return 0;
	Win32_ToWide(sub, wsub, (int)BGI_COUNTOF(wsub));
	if(RegOpenKeyExW(root, wsub, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
		return 0;
	if(value)
		Win32_ToWide(value, wvalue, (int)BGI_COUNTOF(wvalue));
	if(RegQueryValueExW(h, value ? wvalue : NULL, NULL, &type, (BYTE*)data, &len) == ERROR_SUCCESS && type == REG_SZ)
	{
		char text[0x800];
		data[len / sizeof(WCHAR)] = 0;
		// a string longer than the caller's buffer is a failure, as RegQueryValueEx has it (ERROR_MORE_DATA)
		if((size_t)Win32_PathFromWide(data, text, (int)sizeof text) < n)
		{
			strcpy(buf, text);
			ok = 1;
		}
	}
	RegCloseKey(h);
	return ok;
}

// RegCreateKeyEx with the class string (opens an existing key as well); 1 when the key exists afterwards
int OS_RegCreateKey(const char* key, const char* cls)
{
	const char* sub;
	HKEY root = RegRoot(key, &sub), h;
	WCHAR wsub[WIN32_WPATH], wcls[0x100];
	DWORD disp;
	Win32_ToWide(sub, wsub, (int)BGI_COUNTOF(wsub));
	if(cls)
		Win32_ToWide(cls, wcls, (int)BGI_COUNTOF(wcls));
	if(RegCreateKeyExW(root, wsub, 0, cls ? wcls : NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &h, &disp) != ERROR_SUCCESS)
		return 0;
	RegCloseKey(h);
	return 1;
}

// create or open the key, then RegSetValueEx of a REG_SZ (`value` NULL sets the key's default value); 1 when written
int OS_RegWriteString(const char* key, const char* value, const char* s)
{
	const char* sub;
	HKEY root = RegRoot(key, &sub), h;
	WCHAR wsub[WIN32_WPATH], wvalue[0x100];
	WCHAR* data;
	DWORD disp;
	int ok = 0;
	Win32_ToWide(sub, wsub, (int)BGI_COUNTOF(wsub));
	if(RegCreateKeyExW(root, wsub, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &h, &disp) != ERROR_SUCCESS)
		return 0;
	if(value)
		Win32_ToWide(value, wvalue, (int)BGI_COUNTOF(wvalue));
	data = Win32_ToWideDup(s);
	if(data) // the size in bytes, with the NUL
		ok = RegSetValueExW(h, value ? wvalue : NULL, 0, REG_SZ, (const BYTE*)data,
				 (DWORD)((lstrlenW(data) + 1) * sizeof(WCHAR))) == ERROR_SUCCESS;
	free(data);
	RegCloseKey(h);
	return ok;
}

// RegDeleteKey: the key with its values (not its sub keys); 1 when deleted
int OS_RegDeleteKey(const char* key)
{
	const char* sub;
	HKEY root = RegRoot(key, &sub);
	WCHAR wsub[WIN32_WPATH];
	Win32_ToWide(sub, wsub, (int)BGI_COUNTOF(wsub));
	return RegDeleteKeyW(root, wsub) == ERROR_SUCCESS;
}

// WM_SETTINGCHANGE to every top-level window (1 s timeout, hung windows skipped) and SHChangeNotify(SHCNE_ASSOCCHANGED)
void OS_ShellNotifyAssocChanged(void)
{
	SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, 0, SMTO_ABORTIFHUNG, 1000, NULL);
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

// SHGetSpecialFolderLocation(csidl) resolved to a path with SHGetPathFromIDList into buf (n bytes); 1 = ok
int OS_SpecialFolder(int csidl, char* buf, size_t n)
{
	LPITEMIDLIST idl = NULL;
	WCHAR path[MAX_PATH];
	if(FAILED(SHGetSpecialFolderLocation(NULL, csidl, &idl)) || !idl)
		return 0;
	if(!SHGetPathFromIDListW(idl, path))
	{
		CoTaskMemFree(idl);
		return 0;
	}
	CoTaskMemFree(idl);
	if(n)
		Win32_PathFromWide(path, buf, (int)n);
	return 1;
}

int OS_WindowsDir(char* buf, size_t n)
{
	WCHAR path[MAX_PATH];
	UINT len = GetWindowsDirectoryW(path, MAX_PATH);
	if(len == 0 || len >= MAX_PATH || !n)
		return 0;
	Win32_PathFromWide(path, buf, (int)n);
	return 1;
}
