/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pickers.c - the shell's file and folder pickers for the Win32 back end
 *             (inc/bgi/os.h): GetOpenFileName / GetSaveFileName for
 *             "80 3B" and SHBrowseForFolder for "81 3A" and the installer,
 *             in their wide-character forms: the engine's strings are
 *             converted on the way in, the chosen path on the way out
 *             (wide.c; a path the engine cannot spell comes back under
 *             its alias)
 */
#include "ui_internal.h"

/* The common file dialog over the main window.  The filter arrives as
 * "<description> <pattern>"; the original overwrites the last space with
 * a NUL to make the pair the dialog expects, and the zeroed buffer
 * supplies the second terminator.  `path` (n bytes) holds the initial
 * file name and receives the choice; `save` picks GetSaveFileName with an
 * overwrite prompt instead of GetOpenFileName requiring an existing file.
 * 1 when a file was chosen. */
int OS_FileDialog(int save, char* path, size_t n, const char* filter, const char* defExt, const char* initialDir,
	const char* title)
{
	OPENFILENAMEW ofn;
	char flt[0x104];
	WCHAR wflt[0x108], file[WIN32_WPATH];
	WCHAR* wdefExt = Win32_ToWideDup(defExt); // the optional strings stay NULL when not given
	WCHAR* wdir = Win32_ToWideDup(initialDir);
	WCHAR* wtitle = Win32_ToWideDup(title);
	char* space;
	size_t len = strlen(filter);
	int o, ok;
	if(len > sizeof flt - 3)
		len = sizeof flt - 3;
	memset(flt, 0, sizeof flt);
	memcpy(flt, filter, len);
	space = strrchr(flt, ' ');
	if(space)
		*space = 0;
	// the pair as UTF-16: the description, its NUL, the pattern, and two NULs at the end
	memset(wflt, 0, sizeof wflt);
	o = Win32_ToWide(flt, wflt, 0x104) + 1;
	if(space)
		Win32_ToWide(space + 1, wflt + o, 0x104 - o);
	Win32_ToWide(path, file, (int)BGI_COUNTOF(file));
	memset(&ofn, 0, sizeof ofn);
	ofn.lStructSize = 0x4c; // the Windows 95 size the original uses: the dialog comes up in its old style, without the places bar
	ofn.hwndOwner = gWin32MainWnd;
	ofn.lpstrFilter = wflt;
	ofn.nFilterIndex = 1;
	ofn.lpstrFile = file;
	ofn.nMaxFile = (DWORD)BGI_COUNTOF(file);
	ofn.lpstrInitialDir = wdir;
	ofn.lpstrTitle = wtitle;
	ofn.lpstrDefExt = wdefExt;
	ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
	ok = (save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn)) != 0;
	if(ok && n)
		Win32_PathFromWide(file, path, (int)n);
	free(wdefExt);
	free(wdir);
	free(wtitle);
	return ok;
}

// BFFM_INITIALIZED: centre the browser and select the initial folder (the lParam of BROWSEINFO: its path in UTF-16) when one is given
static int CALLBACK BrowseCallback(HWND hwnd, UINT msg, LPARAM lp, LPARAM data)
{
	BGI_UNUSED(lp);
	if(msg == BFFM_INITIALIZED)
	{
		Dlg_Centre(hwnd);
		if(data)
			SendMessageW(hwnd, BFFM_SETSELECTIONW, 1, data);
	}
	return 0;
}

/* SHBrowseForFolder over the main window, file-system folders only and
 * not below the domain level, with `initial` selected at the start (NULL
 * for none); 1 when a folder was chosen, its path in `path` (n bytes,
 * truncated when longer). */
int OS_BrowseFolder(char* path, size_t n, const char* title, const char* initial)
{
	BROWSEINFOW bi;
	WCHAR display[MAX_PATH];
	LPITEMIDLIST idl;
	WCHAR chosen[MAX_PATH];
	WCHAR wtitle[0x200], winitial[WIN32_WPATH];
	Win32_ToWide(title, wtitle, (int)BGI_COUNTOF(wtitle));
	memset(&bi, 0, sizeof bi);
	bi.hwndOwner = gWin32MainWnd;
	bi.pszDisplayName = display;
	bi.lpszTitle = wtitle;
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_DONTGOBELOWDOMAIN;
	bi.lpfn = BrowseCallback;
	if(initial)
	{
		Win32_ToWide(initial, winitial, (int)BGI_COUNTOF(winitial));
		bi.lParam = (LPARAM)winitial;
	}
	idl = SHBrowseForFolderW(&bi);
	if(!idl)
		return 0;
	chosen[0] = 0;
	SHGetPathFromIDListW(idl, chosen);
	CoTaskMemFree(idl);
	if(n)
		Win32_PathFromWide(chosen, path, (int)n);
	return 1;
}
