/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * launchdlg.c - the launcher's "Select game" dialog for the Win32 back end
 *               (inc/bgi/os.h: OS_LauncherDialog)
 *
 * A template built with the DlgBuilder_t of ui_internal.h: the list of
 * the recorded games, the directory field with its Browse button
 * (SHBrowseForFolder through OS_BrowseFolder), the status line, and the
 * Advanced controls - a profile combo box one may also type into and an
 * options field - which start hidden; Advanced shows them and lets the
 * dialog grow.  The launcher's callbacks run on a list selection and on
 * every change of the directory field; Launch is refused with a message
 * box while the directory holds no game.
 *
 * The directory is a native path - on Windows the engine's own kind of
 * path - and the field shows what it stands for: Dlg_SetText puts the
 * real directory there even when the engine holds it under a path alias,
 * and Dlg_GetPath turns what the user typed back into the engine's
 * spelling (wide.c).
 */
#include "ui_internal.h"

#define LD_W            320 // dialog units
#define LD_H_BASIC      171
#define LD_H_ADV        209
#define ID_GAMES        0x501
#define ID_DIR          0x503
#define ID_BROWSE       0x504
#define ID_STATUS       0x505
#define ID_ADVANCED     0x506
#define ID_PROFILE_L    0x507
#define ID_PROFILE      0x508
#define ID_OPTIONS_L    0x509
#define ID_OPTIONS      0x50a

#define ST_LIST         0x50a10101u // WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT
#define ST_COMBO_EDIT   0x40210002u // WS_CHILD | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWN (hidden until Advanced)
#define ST_EDIT_HIDDEN  0x40810080u // ST_EDIT without WS_VISIBLE
#define ST_LABEL_HIDDEN 0x40020000u

static OsLauncher_t* gLauncher; // the dialog's context (one dialog runs at a time, as the original's dialogs do)
static int gAdvancedShown;
static int gInSetText; // the dialog itself is filling the directory field: its EN_CHANGE is not a change by the user

// the directory field and the status line from the launcher's state
static void ShowState(HWND hwnd)
{
	gInSetText = 1;
	Dlg_SetText(hwnd, ID_DIR, gLauncher->dir);
	gInSetText = 0;
	Dlg_SetText(hwnd, ID_STATUS, gLauncher->status);
	Dlg_SetText(hwnd, ID_PROFILE, gLauncher->profile);
	Dlg_SetText(hwnd, ID_OPTIONS, gLauncher->options);
}

// the Advanced controls appear and the dialog grows (or the reverse)
static void ShowAdvanced(HWND hwnd, int show)
{
	static const int ids[4] = {ID_PROFILE_L, ID_PROFILE, ID_OPTIONS_L, ID_OPTIONS};
	RECT r, win;
	int i;
	for(i = 0; i < 4; i++)
		ShowWindow(GetDlgItem(hwnd, ids[i]), show ? SW_SHOW : SW_HIDE);
	// the height in pixels of the two layouts, from a rectangle of dialog units
	r.left = r.top = 0;
	r.right = LD_W;
	r.bottom = show ? LD_H_ADV : LD_H_BASIC;
	MapDialogRect(hwnd, &r);
	GetWindowRect(hwnd, &win);
	{
		RECT client;
		int frame;
		GetClientRect(hwnd, &client);
		frame = (win.bottom - win.top) - (client.bottom - client.top);
		SetWindowPos(hwnd, NULL, 0, 0, win.right - win.left, r.bottom + frame, SWP_NOMOVE | SWP_NOZORDER);
	}
	gAdvancedShown = show;
	gLauncher->advanced = show;
}

static INT_PTR CALLBACK LaunchProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch(msg)
	{
		case WM_INITDIALOG:
		{
			int i;
			gLauncher = (OsLauncher_t*)lp;
			Dlg_Centre(hwnd);
			for(i = 0; i < gLauncher->count; i++)
				Dlg_AddString(hwnd, ID_GAMES, LB_ADDSTRING, gLauncher->names[i]);
			if(gLauncher->selected >= 0)
				SendDlgItemMessageW(hwnd, ID_GAMES, LB_SETCURSEL, (WPARAM)gLauncher->selected, 0);
			for(i = 0; i < gLauncher->profileCount; i++)
				Dlg_AddString(hwnd, ID_PROFILE, CB_ADDSTRING, gLauncher->profiles[i]);
			// in UTF-16 units; what comes out is cut to the launcher's buffers when it is read
			SendDlgItemMessageW(hwnd, ID_DIR, EM_LIMITTEXT, OS_LAUNCHER_DIR_MAX - 1, 0);
			SendDlgItemMessageW(hwnd, ID_OPTIONS, EM_LIMITTEXT, OS_LAUNCHER_OPTIONS_MAX - 1, 0);
			SendDlgItemMessageW(hwnd, ID_PROFILE, CB_LIMITTEXT, (WPARAM)(sizeof gLauncher->profile - 1), 0);
			ShowState(hwnd);
			if(gLauncher->advanced)
				ShowAdvanced(hwnd, 1);
			SetFocus(GetDlgItem(hwnd, ID_GAMES));
			return 0;
		}
		case WM_COMMAND:
		{
			int id = LOWORD(wp), code = HIWORD(wp);
			if(id == ID_GAMES && (code == LBN_SELCHANGE || code == LBN_DBLCLK))
			{
				int sel = (int)SendDlgItemMessageW(hwnd, ID_GAMES, LB_GETCURSEL, 0, 0);
				if(sel >= 0 && sel < gLauncher->count)
				{
					gLauncher->selected = sel;
					if(gLauncher->onSelect)
						gLauncher->onSelect(gLauncher, sel);
					ShowState(hwnd);
				}
				if(code == LBN_DBLCLK)
					PostMessageW(hwnd, WM_COMMAND, IDOK, 0);
			}
			else if(id == ID_DIR && code == EN_CHANGE && !gInSetText)
			{
				Dlg_GetPath(hwnd, ID_DIR, gLauncher->dir, (int)sizeof gLauncher->dir);
				gLauncher->selected = -1;
				SendDlgItemMessageW(hwnd, ID_GAMES, LB_SETCURSEL, (WPARAM)-1, 0);
				if(gLauncher->onDirectory)
					gLauncher->onDirectory(gLauncher);
				Dlg_SetText(hwnd, ID_STATUS, gLauncher->status);
				Dlg_SetText(hwnd, ID_PROFILE, gLauncher->profile);
			}
			else if(id == ID_BROWSE)
			{
				char chosen[OS_LAUNCHER_DIR_MAX];
				if(OS_BrowseFolder(chosen, sizeof chosen, MSG_BROWSE_TITLE, gLauncher->dir[0] ? gLauncher->dir : NULL))
				{
					snprintf(gLauncher->dir, sizeof gLauncher->dir, "%s", chosen);
					gLauncher->selected = -1;
					SendDlgItemMessageW(hwnd, ID_GAMES, LB_SETCURSEL, (WPARAM)-1, 0);
					if(gLauncher->onDirectory)
						gLauncher->onDirectory(gLauncher);
					ShowState(hwnd);
				}
			}
			else if(id == ID_ADVANCED)
				ShowAdvanced(hwnd, !gAdvancedShown);
			else if(id == ID_PROFILE && (code == CBN_SELCHANGE || code == CBN_EDITCHANGE))
			{
				if(code == CBN_SELCHANGE)
				{
					int sel = (int)SendDlgItemMessageW(hwnd, ID_PROFILE, CB_GETCURSEL, 0, 0);
					if(sel >= 0 && sel < gLauncher->profileCount)
						snprintf(gLauncher->profile, sizeof gLauncher->profile, "%s", gLauncher->profiles[sel]);
				}
				else
					Dlg_GetText(hwnd, ID_PROFILE, gLauncher->profile, (int)sizeof gLauncher->profile);
			}
			else if(id == ID_OPTIONS && code == EN_CHANGE)
				Dlg_GetPath(hwnd, ID_OPTIONS, gLauncher->options, (int)sizeof gLauncher->options);
			else if(id == IDOK)
			{
				if(!gLauncher->launchable)
				{
					Win32_MessageBox(hwnd, gLauncher->refused ? gLauncher->refused : MSG_LAUNCH_REFUSED, MSG_LAUNCH_TITLE,
						MB_ICONEXCLAMATION);
					return 1;
				}
				Dlg_GetText(hwnd, ID_PROFILE, gLauncher->profile, (int)sizeof gLauncher->profile);
				Dlg_GetPath(hwnd, ID_OPTIONS, gLauncher->options, (int)sizeof gLauncher->options);
				EndDialog(hwnd, 1);
			}
			else if(id == IDCANCEL)
				EndDialog(hwnd, 0);
			return 1;
		}
		default:
			return 0;
	}
}

int OS_LauncherDialog(OsLauncher_t* l)
{
	DlgBuilder_t b;
	gAdvancedShown = 0;
	DbBegin(&b, BGI_DS_SYSMENU, LD_W, LD_H_BASIC, MSG_LAUNCH_TITLE, BGI_DIALOG_FONT_PT);
	DbItem(&b, DLG_CLASS_STATIC, ST_LABEL, 7, 7, 100, 8, 0x500, MSG_LAUNCH_GAMES);
	DbItem(&b, 0x83, ST_LIST, 7, 18, 306, 80, ID_GAMES, NULL); // LISTBOX
	DbItem(&b, DLG_CLASS_STATIC, ST_LABEL, 7, 102, 100, 8, 0x502, MSG_LAUNCH_DIRECTORY);
	DbItem(&b, DLG_CLASS_EDIT, ST_EDIT, 7, 113, 250, 12, ID_DIR, NULL);
	DbItem(&b, DLG_CLASS_BUTTON, ST_BUTTON, 262, 112, 51, 14, ID_BROWSE, MSG_LAUNCH_BROWSE);
	DbItem(&b, DLG_CLASS_STATIC, ST_LABEL, 7, 131, 306, 10, ID_STATUS, "");
	DbItem(&b, DLG_CLASS_BUTTON, ST_BUTTON, 7, 150, 51, 14, ID_ADVANCED, MSG_LAUNCH_ADVANCED);
	DbItem(&b, DLG_CLASS_BUTTON, ST_DEFBUTTON, 208, 150, 51, 14, IDOK, MSG_LAUNCH_LAUNCH);
	DbItem(&b, DLG_CLASS_BUTTON, ST_BUTTON, 262, 150, 51, 14, IDCANCEL, MSG_LAUNCH_QUIT);
	DbItem(&b, DLG_CLASS_STATIC, ST_LABEL_HIDDEN, 7, 174, 40, 8, ID_PROFILE_L, MSG_LAUNCH_PROFILE);
	DbItem(&b, DLG_CLASS_COMBOBOX, ST_COMBO_EDIT, 50, 171, 90, 120, ID_PROFILE, NULL);
	DbItem(&b, DLG_CLASS_STATIC, ST_LABEL_HIDDEN, 7, 192, 40, 8, ID_OPTIONS_L, MSG_LAUNCH_OPTIONS);
	DbItem(&b, DLG_CLASS_EDIT, ST_EDIT_HIDDEN, 50, 190, 263, 12, ID_OPTIONS, NULL);
	return (int)DialogBoxIndirectParamW(gWin32Instance, DbEnd(&b), gWin32MainWnd, LaunchProc, (LPARAM)l) == 1;
}
