/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * instdlg.c - the installer's dialogs for the Win32 back end
 *             (inc/bgi/os.h, inc/bgi/install.h): the original's resources
 *             0x6C .. 0x6F (the folder choice, the two configuration
 *             choices and the copy progress) with their procedures; where
 *             one called into the installer's logic, the matching
 *             Install* function of src/sys/install is called instead
 *
 * OS_InstallerDialog dispatches on the template id (INSTALL_DLG_*) to the
 * dialog, each built with the DlgBuilder_t of ui_internal.h from the
 * layout of the original's resource.  The contexts are the Install*Dlg_t
 * structures of install.h, which also states each dialog's result; the
 * procedures keep their context in a static, as the original keeps it in
 * a global (one dialog runs at a time).  The dialogs are Unicode windows;
 * the Dlg_* helpers of ui_internal.h convert the texts and the folder.
 */
#include "ui_internal.h"

#define INSTALLER_TITLE "Buriko General Interpreter Integrated Installer" // the caption of every installer dialog

/* 0x6C: the installation folder and the two shortcut check boxes.
 * WM_INITDIALOG shows the path (0x3e8) and the check states (0x3ea,
 * 0x3eb) and renames OK (0x3ec) when a caption is given; browse (0x3e9)
 * runs InstallOptions_Browse, OK runs InstallOptions_Accept and ends with
 * 1 when it accepts the path (the check boxes are read back then);
 * cancel (0x3ed) and the close box end with 0. */
static INT_PTR CALLBACK OptionsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	static InstallOptionsDlg_t* d;
	switch(msg)
	{
		case WM_INITDIALOG:
			d = (InstallOptionsDlg_t*)lp;
			Dlg_Centre(hwnd);
			Dlg_SetText(hwnd, 0x3e8, d->path);
			SendDlgItemMessageW(hwnd, 0x3ea, BM_SETCHECK, (WPARAM)d->check1, 0);
			SendDlgItemMessageW(hwnd, 0x3eb, BM_SETCHECK, (WPARAM)d->check2, 0);
			if(d->okCaption)
				Dlg_SetText(hwnd, 0x3ec, d->okCaption);
			return 1;
		case WM_COMMAND:
			switch(LOWORD(wp))
			{
				case 0x3e9: // browse
				{
					char edit[0x200];
					if(InstallOptions_Browse(d, edit))
						Dlg_SetText(hwnd, 0x3e8, edit);
					break;
				}
				case 0x3ec: // OK
				{
					char edit[0x104];
					Dlg_GetPath(hwnd, 0x3e8, edit, (int)sizeof edit);
					if(InstallOptions_Accept(d, edit))
					{
						d->check1 = IsDlgButtonChecked(hwnd, 0x3ea) != 0;
						d->check2 = IsDlgButtonChecked(hwnd, 0x3eb) != 0;
						EndDialog(hwnd, 1);
					}
					break;
				}
				case 0x3ed: // cancel
					EndDialog(hwnd, 0);
					break;
				default:
					break;
			}
			return 1;
		case WM_CLOSE:
			EndDialog(hwnd, 0);
			return 1;
		default:
			return 0;
	}
}

// the options dialog (263 x 147 dialog units): 1 for OK, 0 for cancel
static int OptionsDialog(InstallOptionsDlg_t* d)
{
	DlgBuilder_t b;
	DbBegin(&b, BGI_DS_SYSMENU, 263, 147, INSTALLER_TITLE, BGI_DIALOG_FONT_PT);
	DbItem(&b, DLG_CLASS_BUTTON, ST_DEFBUTTON, 210, 125, 40, 14, 0x3ed, MSG_INST_DLG_ABORT);
	DbItem(&b, DLG_CLASS_BUTTON, ST_GROUP, 15, 10, 235, 40, 0xffff, MSG_INST_DLG_FOLDER);
	DbItem(&b, DLG_CLASS_EDIT, ST_EDIT, 25, 26, 165, 13, 0x3e8, NULL);
	DbItem(&b, DLG_CLASS_BUTTON, ST_BUTTON, 199, 25, 40, 14, 0x3e9, MSG_INST_DLG_BROWSE);
	DbItem(&b, DLG_CLASS_BUTTON, ST_GROUP, 15, 60, 235, 54, 0xffff, MSG_INST_DLG_SHORTCUT);
	DbItem(&b, DLG_CLASS_BUTTON, ST_CHECK, 26, 77, 159, 11, 0x3ea, MSG_INST_DLG_SHORTCUT_MENU);
	DbItem(&b, DLG_CLASS_BUTTON, ST_CHECK, 26, 93, 159, 11, 0x3eb, MSG_INST_DLG_SHORTCUT_DESKTOP);
	DbItem(&b, DLG_CLASS_BUTTON, ST_BUTTON, 135, 125, 65, 14, 0x3ec, MSG_INST_DLG_NEXT);
	return (int)DialogBoxIndirectParamW(gWin32Instance, DbEnd(&b), gWin32MainWnd, OptionsProc, (LPARAM)d);
}

/* 0x6D / 0x6E: a text (0x3ee) and two or three radio buttons (0x3ef only
 * in the three-button template, 0x3f0, 0x3f1).  WM_INITDIALOG sets the
 * captions, disables the button `disabled` names, checks radio1 when that
 * is 2 and radio2 otherwise, and shows the extra button (0x3f2) on
 * request.  Results: the extra button 3, OK (0x3f3) the index of the
 * checked radio button or 3 when none is, cancel (0x3f4) and the close
 * box -1. */
static INT_PTR CALLBACK ChoiceProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	static const int ids[3] = {0x3ef, 0x3f0, 0x3f1};
	static InstallChoiceDlg_t* d;
	switch(msg)
	{
		case WM_INITDIALOG:
			d = (InstallChoiceDlg_t*)lp;
			Dlg_Centre(hwnd);
			Dlg_SetText(hwnd, 0x3ee, d->text);
			if(d->radio0)
				Dlg_SetText(hwnd, 0x3ef, d->radio0);
			Dlg_SetText(hwnd, 0x3f0, d->radio1);
			Dlg_SetText(hwnd, 0x3f1, d->radio2);
			// the disabled button; the first one only exists in the three-button template
			if(d->disabled >= 0 && d->disabled < 3 && (d->radio0 || d->disabled > 0))
				EnableWindow(GetDlgItem(hwnd, ids[d->disabled]), FALSE);
			SendDlgItemMessageW(hwnd, d->disabled == 2 ? 0x3f0 : 0x3f1, BM_SETCHECK, 1, 0);
			ShowWindow(GetDlgItem(hwnd, 0x3f2), d->showExtra ? SW_SHOW : SW_HIDE);
			return 1;
		case WM_COMMAND:
			switch(LOWORD(wp))
			{
				case 0x3f2: // the extra button
					EndDialog(hwnd, 3);
					break;
				case 0x3f3: // OK: the checked radio button, 3 when none
				{
					int i = d->radio0 ? 0 : 1;
					while(i < 3 && !IsDlgButtonChecked(hwnd, ids[i]))
						i++;
					EndDialog(hwnd, i);
					break;
				}
				case 0x3f4: // cancel
					EndDialog(hwnd, (INT_PTR)-1);
					break;
				default:
					break;
			}
			return 1;
		case WM_CLOSE:
			EndDialog(hwnd, (INT_PTR)-1);
			return 1;
		default:
			return 0;
	}
}

// the choice dialog in its two- (0x6D, 295 x 127) or three-button (0x6E, 295 x 147) form; the result of ChoiceProc
static int ChoiceDialog(InstallChoiceDlg_t* d, int three)
{
	DlgBuilder_t b;
	int h = three ? 147 : 127, by = three ? 125 : 105; // the three-button template is 20 units taller
	DbBegin(&b, BGI_DS_SYSMENU, 295, h, INSTALLER_TITLE, BGI_DIALOG_FONT_PT);
	DbItem(&b, DLG_CLASS_BUTTON, ST_DEFBUTTON, 170, by, 50, 14, 0x3f3, MSG_INST_DLG_INSTALL);
	DbItem(&b, DLG_CLASS_BUTTON, ST_BUTTON, 230, by, 50, 14, 0x3f4, MSG_INST_DLG_ABORT);
	DbItem(&b, DLG_CLASS_BUTTON, ST_GROUP, 15, 35, 265, three ? 80 : 60, 0xffff, MSG_INST_DLG_CONFIG);
	if(three)
	{
		DbItem(&b, DLG_CLASS_BUTTON, ST_RADIO, 25, 50, 245, 15, 0x3ef, MSG_INST_DLG_MIN);
		DbItem(&b, DLG_CLASS_BUTTON, ST_RADIO, 25, 70, 245, 15, 0x3f0, MSG_INST_DLG_STD);
		DbItem(&b, DLG_CLASS_BUTTON, ST_RADIO, 25, 90, 245, 15, 0x3f1, MSG_INST_DLG_MAX);
	}
	else
	{
		DbItem(&b, DLG_CLASS_BUTTON, ST_RADIO, 25, 50, 245, 15, 0x3f0, MSG_INST_DLG_STD);
		DbItem(&b, DLG_CLASS_BUTTON, ST_RADIO, 25, 70, 245, 15, 0x3f1, MSG_INST_DLG_MAX);
	}
	DbItem(&b, DLG_CLASS_STATIC, ST_TEXT, 15, 10, 265, 15, 0x3ee, MSG_INST_DLG_DESC);
	DbItem(&b, DLG_CLASS_BUTTON, ST_BUTTON, 110, by, 50, 14, 0x3f2, MSG_INST_DLG_BACK);
	return (int)DialogBoxIndirectParamW(gWin32Instance, DbEnd(&b), gWin32MainWnd, ChoiceProc, (LPARAM)d);
}

/* 0x6F: the copy progress.  The original runs the copy on a second thread
 * that posts 0x8000 .. 0x8004 to the dialog; here the copy runs inside
 * Install_CopyAll() on this thread and reports through the callbacks of
 * InstallProgressDlg_t, which forward to the same controls.  The window
 * is modeless and pumped from the `alive` callback. */
typedef struct ProgressUi
{
	HWND hwnd;         // the modeless dialog
	int aborted;       // the user confirmed the abort: `alive` reports 0 from then on
	int cancelAllowed; // the cancel button (0x3f8) is shown
} ProgressUi_t;

// WM_INITDIALOG shows or hides the cancel button; the button asks MSG_ASK_ABORT and sets `aborted` on Yes
static INT_PTR CALLBACK ProgressProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	static ProgressUi_t* ui;
	switch(msg)
	{
		case WM_INITDIALOG:
			ui = (ProgressUi_t*)lp;
			ui->hwnd = hwnd;
			Dlg_Centre(hwnd);
			ShowWindow(GetDlgItem(hwnd, 0x3f8), ui->cancelAllowed ? SW_SHOW : SW_HIDE);
			return 1;
		case WM_COMMAND:
			if(LOWORD(wp) == 0x3f8 && !ui->aborted)
			{
				// "Abort?" with No as the default
				if(Win32_MessageBox(hwnd, MSG_ASK_ABORT, MSG_CONFIRM, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
					ui->aborted = 1;
			}
			return 1;
		default:
			return 0;
	}
}

// a new file: its caption into the text (0x3f5), the per-file bar (0x3f6) reset to 0 of 10 (the original's message 0x8000)
static void ProgressSetFile(void* u, int index, const char* caption)
{
	ProgressUi_t* ui = (ProgressUi_t*)u;
	BGI_UNUSED(index);
	Dlg_SetText(ui->hwnd, 0x3f5, caption);
	SendDlgItemMessageW(ui->hwnd, 0x3f6, PBM_SETRANGE32, 0, 10);
	SendDlgItemMessageW(ui->hwnd, 0x3f6, PBM_SETPOS, 0, 0);
}

static void ProgressSetRange(void* u, int blocks) // the per-file bar's range: the file's blocks (0x8003)
{
	SendDlgItemMessageW(((ProgressUi_t*)u)->hwnd, 0x3f6, PBM_SETRANGE32, 0, (LPARAM)blocks);
}

static void ProgressSetPos(void* u, int block) // the per-file bar's position (0x8004)
{
	SendDlgItemMessageW(((ProgressUi_t*)u)->hwnd, 0x3f6, PBM_SETPOS, (WPARAM)block, 0);
}

static void ProgressSetTotal(void* u, int done) // the total bar (0x3f7) after each file (0x8001)
{
	SendDlgItemMessageW(((ProgressUi_t*)u)->hwnd, 0x3f7, PBM_SETPOS, (WPARAM)done, 0);
}

// pump every pending message through the dialog, then report whether the copy may go on (0 once the abort was confirmed)
static int ProgressAlive(void* u)
{
	ProgressUi_t* ui = (ProgressUi_t*)u;
	while(OsWin32_PumpOnce(ui->hwnd) && !ui->aborted)
	{
		MSG m;
		if(!PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE))
			break; // the queue is empty: back to copying
	}
	return !ui->aborted;
}

/* The progress dialog (179 x 107 dialog units, two progress bars): created
 * modeless and shown, the callbacks of `d` pointed at the controls, then
 * Install_CopyAll runs the copy on this thread; its result (1 every file
 * copied, 0 failed or cancelled) is the dialog's.  0 as well when the
 * dialog cannot be created. */
static int ProgressDialog(InstallProgressDlg_t* d)
{
	DlgBuilder_t b;
	ProgressUi_t ui;
	HWND hwnd;
	int result;
	INITCOMMONCONTROLSEX icc;

	icc.dwSize = sizeof icc;
	icc.dwICC = ICC_PROGRESS_CLASS; // the progress bar class must be registered before the template is used
	InitCommonControlsEx(&icc);

	memset(&ui, 0, sizeof ui);
	ui.cancelAllowed = d->cancelAllowed;
	DbBegin(&b, BGI_DS_PLAIN, 179, 107, INSTALLER_TITLE, BGI_DIALOG_FONT_PT);
	DbItem(&b, DLG_CLASS_BUTTON, ST_BUTTON, 65, 85, 50, 14, 0x3f8, MSG_INST_DLG_ABORT);
	DbItemClass(&b, BGI_PROGRESS_CLASS, ST_PROGRESS, 0, 10, 60, 160, 15, 0x3f7, "Progress2");
	DbItem(&b, DLG_CLASS_STATIC, ST_TEXT, 10, 10, 160, 15, 0x3f5, MSG_INST_DLG_DESC);
	DbItemClass(&b, BGI_PROGRESS_CLASS, ST_PROGRESS, WS_EX_CLIENTEDGE, 10, 35, 160, 15, 0x3f6, "Progress1");
	hwnd = CreateDialogIndirectParamW(gWin32Instance, DbEnd(&b), gWin32MainWnd, ProgressProc, (LPARAM)&ui);
	if(!hwnd)
		return 0;
	ShowWindow(hwnd, SW_SHOW);
	SendDlgItemMessageW(hwnd, 0x3f7, PBM_SETRANGE32, 0, (LPARAM)d->fileCount);

	d->ui = &ui;
	d->setFile = ProgressSetFile;
	d->setRange = ProgressSetRange;
	d->setPos = ProgressSetPos;
	d->setTotal = ProgressSetTotal;
	d->alive = ProgressAlive;
	result = Install_CopyAll(d);
	DestroyWindow(hwnd);
	return result;
}

// the installer's dialog `id` (INSTALL_DLG_*) over its context structure; the dialog's result as install.h states it, 0 for an unknown id
int OS_InstallerDialog(int id, void* ctx)
{
	switch(id)
	{
		case INSTALL_DLG_OPTIONS: return OptionsDialog((InstallOptionsDlg_t*)ctx);
		case INSTALL_DLG_CHOICE: return ChoiceDialog((InstallChoiceDlg_t*)ctx, 0);
		case INSTALL_DLG_CHOICE3: return ChoiceDialog((InstallChoiceDlg_t*)ctx, 1);
		case INSTALL_DLG_PROGRESS: return ProgressDialog((InstallProgressDlg_t*)ctx);
		default: return 0;
	}
}
