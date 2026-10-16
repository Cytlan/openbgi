/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ui_internal.h - what the dialog files of the Win32 back end share
 *                 (dlgtemplate.c, dialogs.c, instdlg.c, pickers.c): the
 *                 in-memory DLGTEMPLATE builder, the control styles of the
 *                 original's resource templates and two helpers every
 *                 dialog procedure uses
 *
 * The original keeps its dialogs as resources; this build has none, so
 * dlgtemplate.c assembles the same templates in memory and dialogs.c /
 * instdlg.c run them with DialogBoxIndirectParamW.  A dialog is written as
 * a DbBegin, one DbItem per control in the order of the resource, and
 * DbEnd; the styles below are the ones the resources use, so a control
 * line reads like the resource script it reproduces.  The dialogs are
 * Unicode windows: the Dlg_* helpers below put the engine's strings into
 * their controls and read them back (wide.c does the converting).
 */
#ifndef BGI_OS_WIN32_UI_INTERNAL_H
#define BGI_OS_WIN32_UI_INTERNAL_H

#include "bgi/os_win32.h"
#include <commdlg.h>
#include <shlobj.h>
#include <commctrl.h>

#include "bgi/os.h"
#include "bgi/msg.h"
#include "bgi/install.h"

// ---- dlgtemplate.c: DLGTEMPLATEs built in memory ------------------------------------------------

// a template under construction; one per dialog, on the caller's stack
typedef struct DlgBuilder
{
	uint8_t buf[0x1000]; // the DLGTEMPLATE followed by its DLGITEMTEMPLATEs
	size_t len;          // bytes written so far (keeps counting past the end of buf; the writes then stop)
	size_t cditAt;       // offset of the item count, patched by DbEnd
	int items;           // controls added so far
} DlgBuilder_t;

// the predefined control class atoms of a dialog template
#define DLG_CLASS_BUTTON   0x80
#define DLG_CLASS_EDIT     0x81
#define DLG_CLASS_STATIC   0x82
#define DLG_CLASS_COMBOBOX 0x85

// the two dialog styles of the resources (DS_SETFONT is added by DbBegin)
#define BGI_DS_PLAIN       0x80c00080u // WS_POPUP | WS_CAPTION | DS_MODALFRAME
#define BGI_DS_SYSMENU     0x80c80080u // WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME

/* the control styles of the resource templates, as stored there (decoded
 * in the comments) */
#define ST_DEFBUTTON       0x50010001u // WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON
#define ST_BUTTON          0x50010000u // WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON
#define ST_CHECK           0x50010003u // WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX
#define ST_RADIO           0x50001009u // WS_CHILD | WS_VISIBLE | BS_PUSHLIKE | BS_AUTORADIOBUTTON
#define ST_GROUP           0x50000007u // WS_CHILD | WS_VISIBLE | BS_GROUPBOX
#define ST_EDIT            0x50810080u // WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL
#define ST_LABEL           0x50020000u // WS_CHILD | WS_VISIBLE | WS_GROUP | SS_LEFT
#define ST_TEXT            0x50001201u // WS_CHILD | WS_VISIBLE | SS_SUNKEN | SS_CENTERIMAGE | SS_CENTER
#define ST_COMBO           0x50210003u // WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST
#define ST_PROGRESS        0x50800001u // WS_CHILD | WS_VISIBLE | WS_BORDER | PBS_SMOOTH

/* Start a template: `style` as in the resource (DS_SETFONT is added, with
 * `fontPt` as the point size and MSG_DLG_FONT_FACE as the face), no menu,
 * the default dialog class, cx x cy dialog units, `title` (NULL for none;
 * Shift-JIS, stored as UTF-16). */
void DbBegin(DlgBuilder_t* b, uint32_t style, int cx, int cy, const char* title, int fontPt);
// A control of one of the predefined classes (DLG_CLASS_*): position and size in dialog units, `text` NULL for none.
void DbItem(DlgBuilder_t* b, int cls, uint32_t style, int x, int y, int cx, int cy, int id, const char* text);
// A control of a named window class (the progress bars), with an extended style.
void DbItemClass(DlgBuilder_t* b, const char* cls, uint32_t style, uint32_t exStyle, int x, int y, int cx,
	int cy, int id, const char* text);
// Finish the template; the pointer DialogBoxIndirectParamW takes (valid until the next DbBegin on the same builder).
const DLGTEMPLATE* DbEnd(DlgBuilder_t* b);

// Centre a dialog on the screen and make it top-most (called from WM_INITDIALOG).
void Dlg_Centre(HWND hwnd);
// Copy at most `limit` bytes (when limit > 0, at most 0xff otherwise) of an initial text into a zeroed 0x100-byte buffer.
void Dlg_InitialText(char* buf, const char* text, int limit);

// the progress bar's window class (PROGRESS_CLASS), as the narrow string DbItemClass takes
#define BGI_PROGRESS_CLASS "msctls_progress32"

// SetWindowText of a dialog (its caption) or of any window, the text converted
void Dlg_SetTitle(HWND hwnd, const char* text);
// SetDlgItemText of control `id`, the text converted (a path alias shows as the real directory)
void Dlg_SetText(HWND hwnd, int id, const char* text);
// GetDlgItemText of control `id` in the text encoding into out (n bytes, cut at a character boundary); the bytes copied
int Dlg_GetText(HWND hwnd, int id, char* out, int n);
// the same for a field that holds a path: converted as one (Win32_PathFromWide)
int Dlg_GetPath(HWND hwnd, int id, char* out, int n);
// CB_ADDSTRING / LB_ADDSTRING (`msg`) of a converted string to control `id`
void Dlg_AddString(HWND hwnd, int id, UINT msg, const char* text);

#endif
