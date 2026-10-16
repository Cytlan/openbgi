/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dialogs.c - message boxes and the script's dialogs for the Win32 back
 *             end (inc/bgi/os.h): MessageBox, and the original's
 *             resources 0x71 (one text field, "B0 84"), 0x72 (two
 *             labelled fields, "B0 85") and 0x77 (the player's name and
 *             birthday, "B0 8F") with their dialog procedures
 *
 * Each dialog is a template built with the DlgBuilder_t of ui_internal.h
 * from the layout of the original's resource and run modally over the
 * main window with DialogBoxIndirectParamW; the procedure receives its
 * context through the lParam of WM_INITDIALOG and keeps it in a static,
 * as the original keeps it in a global (one dialog runs at a time).  The
 * result is 1 for OK, 0 for cancel or the close box; the text buffers the
 * callers pass are 0x100 bytes.
 *
 * The dialogs are Unicode windows.  Texts go in and come out through the
 * Dlg_* helpers of ui_internal.h, and a field with a length limit keeps
 * it in bytes, as the original's ANSI controls count (Win32_EditClamp on
 * every EN_CHANGE).
 */
#include "ui_internal.h"

// ---- message boxes -------------------------------------------------------------------------------

int OS_MessageBox(const char* text, const char* caption, uint32_t flags) // MessageBoxW over the main window; the button id pressed
{
	return Win32_MessageBox(gWin32MainWnd, text, caption, (UINT)flags);
}

// a field's EM_LIMITTEXT when maxLen > 0: the control's own limit is in UTF-16 units, the bytes are kept by the EN_CHANGE handlers
static void LimitField(HWND hwnd, int id, int maxLen)
{
	if(maxLen > 0)
		SendDlgItemMessageW(hwnd, id, EM_LIMITTEXT, (WPARAM)maxLen, 0);
}

// 1 when a WM_COMMAND is field `id` reporting a change of its text (EN_CHANGE): the caller then cuts it back to its limit in bytes (Win32_EditClamp)
static int FieldChanged(WPARAM wp, int id)
{
	return LOWORD(wp) == id && HIWORD(wp) == EN_CHANGE;
}

// ---- dialog 0x71: one field --------------------------------------------------------------------

// the arguments of OS_DialogInput1, handed to the procedure
typedef struct Input1Ctx
{
	const char* title;   // the caption; NULL for MSG_DLG_COMMENT_INPUT
	const char* initial; // the field's initial text; NULL for none
	int maxLen;          // EM_LIMITTEXT when > 0
	char* out;           // receives the text on OK (0x100 bytes)
} Input1Ctx_t;

/* The procedure of the one-field dialog: WM_INITDIALOG fills the field
 * (id 0x3f9) and puts the caret behind its text, OK (0x3fa) copies the
 * text out and ends with 1, cancel (0x3fb) and the close box end with 0. */
static INT_PTR CALLBACK Input1Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	static Input1Ctx_t* ctx;
	switch(msg)
	{
		case WM_INITDIALOG:
			ctx = (Input1Ctx_t*)lp;
			Dlg_Centre(hwnd);
			Dlg_SetTitle(hwnd, ctx->title ? ctx->title : MSG_DLG_COMMENT_INPUT);
			LimitField(hwnd, 0x3f9, ctx->maxLen);
			if(ctx->initial)
			{
				char buf[0x100];
				Dlg_InitialText(buf, ctx->initial, ctx->maxLen);
				Dlg_SetText(hwnd, 0x3f9, buf);
				// the caret goes behind the text (an offset past the end is the end)
				PostMessageW(GetDlgItem(hwnd, 0x3f9), EM_SETSEL, (WPARAM)strlen(buf), (LPARAM)strlen(buf));
			}
			SetFocus(GetDlgItem(hwnd, 0x3f9));
			return 0; // the focus was set here
		case WM_COMMAND:
			if(FieldChanged(wp, 0x3f9))
				Win32_EditClamp((HWND)lp, ctx->maxLen);
			switch(LOWORD(wp))
			{
				case 0x3fa: // OK
					Dlg_GetText(hwnd, 0x3f9, ctx->out, 0x100);
					EndDialog(hwnd, 1);
					break;
				case 0x3fb: // cancel
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

// "B0 84": the one-field dialog (resource 0x71: 266 x 35 dialog units, OK, the field, Cancel); 1 when confirmed
int OS_DialogInput1(const char* title, const char* initial, int maxLen, char* out)
{
	DlgBuilder_t b;
	Input1Ctx_t ctx;
	ctx.title = title; // NULL pointers select the defaults, as in the original
	ctx.initial = initial;
	ctx.maxLen = maxLen;
	ctx.out = out;
	DbBegin(&b, BGI_DS_PLAIN, 266, 35, NULL, BGI_DIALOG_FONT_PT);
	DbItem(&b, DLG_CLASS_BUTTON, ST_DEFBUTTON, 185, 10, 40, 15, 0x3fa, "OK");
	DbItem(&b, DLG_CLASS_EDIT, ST_EDIT, 5, 10, 175, 13, 0x3f9, NULL);
	DbItem(&b, DLG_CLASS_BUTTON, ST_BUTTON, 230, 10, 30, 15, 0x3fb, "Cancel");
	return (int)DialogBoxIndirectParamW(gWin32Instance, DbEnd(&b), gWin32MainWnd, Input1Proc, (LPARAM)&ctx) == 1;
}

// ---- dialog 0x72: two labelled fields ----------------------------------------------------------

// the arguments of OS_DialogInput2, handed to the procedure
typedef struct Input2Ctx
{
	const char* title;    // the caption; NULL for MSG_DLG_DATA_INPUT
	const char* label1;   // the first field's label (id 0x400); NULL for MSG_DLG_STRING1
	const char* initial1; // the first field's initial text; NULL for none
	int maxLen1;          // its EM_LIMITTEXT when > 0
	char* out1;           // receives the first field on OK (0x100 bytes)
	const char* label2;   // the second field's label (0x401); NULL for MSG_DLG_STRING2
	const char* initial2;
	int maxLen2;
	char* out2;
} Input2Ctx_t;

/* The procedure of the two-field dialog (fields 0x3fc and 0x3fd): the
 * focus goes to the first field without an initial text; with both
 * pre-filled the dialog manager's default (the first control) stands.  OK
 * (0x3fe) copies both fields out and ends with 1, cancel (0x3ff) and the
 * close box end with 0. */
static INT_PTR CALLBACK Input2Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	static Input2Ctx_t* ctx;
	switch(msg)
	{
		case WM_INITDIALOG:
		{
			char buf[0x100];
			int has1, has2;
			ctx = (Input2Ctx_t*)lp;
			Dlg_Centre(hwnd);
			Dlg_SetTitle(hwnd, ctx->title ? ctx->title : MSG_DLG_DATA_INPUT);
			Dlg_SetText(hwnd, 0x400, ctx->label1 ? ctx->label1 : MSG_DLG_STRING1);
			Dlg_SetText(hwnd, 0x401, ctx->label2 ? ctx->label2 : MSG_DLG_STRING2);
			LimitField(hwnd, 0x3fc, ctx->maxLen1);
			LimitField(hwnd, 0x3fd, ctx->maxLen2);
			has1 = ctx->initial1 != NULL;
			if(has1)
			{
				Dlg_InitialText(buf, ctx->initial1, ctx->maxLen1);
				Dlg_SetText(hwnd, 0x3fc, buf);
			}
			else
			{
				SetFocus(GetDlgItem(hwnd, 0x3fc));
			}
			has2 = ctx->initial2 != NULL;
			if(has2)
			{
				Dlg_InitialText(buf, ctx->initial2, ctx->maxLen2);
				Dlg_SetText(hwnd, 0x3fd, buf);
			}
			else if(has1)
			{
				SetFocus(GetDlgItem(hwnd, 0x3fd));
			}
			// both pre-filled: the default focus (the first control); otherwise the focus set above stays
			return has1 && has2;
		}
		case WM_COMMAND:
			if(FieldChanged(wp, 0x3fc))
				Win32_EditClamp((HWND)lp, ctx->maxLen1);
			else if(FieldChanged(wp, 0x3fd))
				Win32_EditClamp((HWND)lp, ctx->maxLen2);
			switch(LOWORD(wp))
			{
				case 0x3fe: // OK
					Dlg_GetText(hwnd, 0x3fc, ctx->out1, 0x100);
					Dlg_GetText(hwnd, 0x3fd, ctx->out2, 0x100);
					EndDialog(hwnd, 1);
					break;
				case 0x3ff: // cancel
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

// "B0 85": the two-field dialog (resource 0x72: 213 x 107 dialog units, a system menu); 1 when confirmed
int OS_DialogInput2(const char* title, const char* label1, const char* initial1, int maxLen1, char* out1,
	const char* label2, const char* initial2, int maxLen2, char* out2)
{
	DlgBuilder_t b;
	Input2Ctx_t ctx;
	ctx.title = title; // NULL pointers select the defaults, as in the original
	ctx.label1 = label1;
	ctx.initial1 = initial1;
	ctx.maxLen1 = maxLen1;
	ctx.out1 = out1;
	ctx.label2 = label2;
	ctx.initial2 = initial2;
	ctx.maxLen2 = maxLen2;
	ctx.out2 = out2;
	DbBegin(&b, BGI_DS_SYSMENU, 213, 107, MSG_DLG_INPUT2_TITLE, BGI_DIALOG_FONT_PT);
	DbItem(&b, DLG_CLASS_BUTTON, ST_DEFBUTTON, 112, 84, 55, 15, 0x3fe, "OK");
	DbItem(&b, DLG_CLASS_BUTTON, ST_BUTTON, 169, 84, 37, 15, 0x3ff, "Cancel");
	DbItem(&b, DLG_CLASS_EDIT, ST_EDIT, 6, 25, 200, 12, 0x3fc, NULL);
	DbItem(&b, DLG_CLASS_EDIT, ST_EDIT, 6, 60, 200, 12, 0x3fd, NULL);
	DbItem(&b, DLG_CLASS_STATIC, ST_LABEL, 6, 10, 200, 8, 0x400, "Static");
	DbItem(&b, DLG_CLASS_STATIC, ST_LABEL, 6, 45, 200, 8, 0x401, "Static");
	return (int)DialogBoxIndirectParamW(gWin32Instance, DbEnd(&b), gWin32MainWnd, Input2Proc, (LPARAM)&ctx) == 1;
}

// ---- dialog 0x77: the name / birthday profile --------------------------------------------------

// the arguments of OS_DialogProfile: in and out, handed to the procedure
typedef struct ProfileCtx
{
	char* field[4]; // surname, given name, nickname, pronoun (10 bytes each): the initial texts in, the entries out
	int32_t* month; // the month list's selection (0-based) in and out
	int32_t* day;   // the day list's selection (0-based) in and out
} ProfileCtx_t;

static const int kProfileIds[4] = {0x407, 0x410, 0x413, 0x412};                                                       // the edit fields, in field[] order
static const char* const kProfileNames[4] = {MSG_DLG_SURNAME, MSG_DLG_GIVEN_NAME, MSG_DLG_NICKNAME, MSG_DLG_PRONOUN}; // their names for the error message
static const int kDaysOfMonth[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};                                 // the day list's length per month (February always 29)

/* read a field (at most 10 bytes of Shift-JIS) and accept it only when
 * every character is a two-byte one in Shift-JIS (a full-width character:
 * the original tests every other byte for a lead byte); 1 = ok and copied
 * in the text encoding (at most 10 bytes and the NUL), 0 leaves out
 * untouched */
static int ProfileField(HWND hwnd, int id, char* out)
{
	WCHAR wide[0x20];
	int i, bytes = 0;
	wide[0] = 0;
	GetDlgItemTextW(hwnd, id, wide, (int)BGI_COUNTOF(wide));
	for(i = 0; wide[i]; i++)
	{
		char mb[4];
		BOOL lossy = FALSE;
		// one UTF-16 unit, exactly: a character code page 932 lacks is not a full-width one
		if(WideCharToMultiByte(WIN32_CP_SJIS, WC_NO_BEST_FIT_CHARS, wide + i, 1, mb, (int)sizeof mb, NULL, &lossy) != 2 || lossy)
			return 0;
		bytes += 2;
		if(bytes > 10)
		{ // what the 10-byte limit of the field lets through, should the clamp not have run
			wide[i] = 0;
			break;
		}
	}
	Win32_TextFromWide(wide, out, 0xb);
	return 1;
}

static void ComboFill(HWND hwnd, int id, int from, int to) // the numbers from..to as "%d" entries
{
	char num[16];
	for(int i = from; i <= to; i++)
	{
		sprintf(num, "%d", i);
		Dlg_AddString(hwnd, id, CB_ADDSTRING, num);
	}
}

/* The procedure of the profile dialog.  WM_INITDIALOG fills the month
 * (0x409, 1..12) and day (0x40a, 1..31) lists, limits each field to 10
 * bytes, pre-fills and selects the fields that have a text, selects the
 * lists' entries and puts the focus on OK.  OK accepts every field through
 * ProfileField, or shows MSG_DLG_HALFWIDTH with the field's name and
 * keeps the dialog open; a month change re-fills the day list to the
 * month's length.  There is no cancel: the dialog ends with 1 only. */
static INT_PTR CALLBACK ProfileProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	static ProfileCtx_t* ctx;
	switch(msg)
	{
		case WM_INITDIALOG:
		{
			char buf[0x100];
			ctx = (ProfileCtx_t*)lp;
			Dlg_Centre(hwnd);
			ComboFill(hwnd, 0x409, 1, 12);
			ComboFill(hwnd, 0x40a, 1, 31);
			for(int i = 0; i < 4; i++)
			{
				LimitField(hwnd, kProfileIds[i], 10);
				if(ctx->field[i][0])
				{
					Dlg_InitialText(buf, ctx->field[i], 10);
					Dlg_SetText(hwnd, kProfileIds[i], buf);
					SendDlgItemMessageW(hwnd, kProfileIds[i], EM_SETSEL, 0, (LPARAM)-1);
				}
			}
			SendDlgItemMessageW(hwnd, 0x409, CB_SETCURSEL, (WPARAM)*ctx->month, 0);
			SendDlgItemMessageW(hwnd, 0x40a, CB_SETCURSEL, (WPARAM)*ctx->day, 0);
			SetFocus(GetDlgItem(hwnd, IDOK));
			return 0; // the focus was set here
		}
		case WM_COMMAND:
			for(int i = 0; i < 4; i++)
				if(FieldChanged(wp, kProfileIds[i]))
					Win32_EditClamp((HWND)lp, 10);
			if(LOWORD(wp) == IDOK)
			{
				for(int i = 0; i < 4; i++)
				{
					if(!ProfileField(hwnd, kProfileIds[i], ctx->field[i]))
					{
						char text[0x100];
						sprintf(text, MSG_DLG_HALFWIDTH, kProfileNames[i]);
						Win32_MessageBox(hwnd, text, MSG_DLG_INPUT_ERROR, MB_ICONHAND);
						return 1; // the dialog stays open
					}
				}
				*ctx->month = (int32_t)SendDlgItemMessageW(hwnd, 0x409, CB_GETCURSEL, 0, 0);
				*ctx->day = (int32_t)SendDlgItemMessageW(hwnd, 0x40a, CB_GETCURSEL, 0, 0);
				EndDialog(hwnd, 1);
			}
			else if(LOWORD(wp) == 0x409 && HIWORD(wp) == CBN_SELCHANGE)
			{
				// a new month: the day list shrinks or grows, keeping the selection when it still exists
				int month = (int)SendDlgItemMessageW(hwnd, 0x409, CB_GETCURSEL, 0, 0);
				int days = (month >= 0 && month < 12) ? kDaysOfMonth[month] : 31;
				int cur = (int)SendDlgItemMessageW(hwnd, 0x40a, CB_GETCURSEL, 0, 0);
				SendDlgItemMessageW(hwnd, 0x40a, CB_RESETCONTENT, 0, 0);
				ComboFill(hwnd, 0x40a, 1, days);
				SendDlgItemMessageW(hwnd, 0x40a, CB_SETCURSEL, (WPARAM)(cur >= days ? 0 : cur), 0);
			}
			return 1;
		default:
			return 0;
	}
}

/* "B0 8F": the profile dialog (resource 0x77: 261 x 61 dialog units at
 * 10 pt, no close box).  The four texts are the fields' initial contents
 * in and the entries out (10 bytes each, plus the NUL); *month and *day
 * are the 0-based list selections in and out.  1 when confirmed, which is
 * the only way to close the dialog; 0 only when it could not be created. */
int OS_DialogProfile(char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month, int32_t* day)
{
	DlgBuilder_t b;
	ProfileCtx_t ctx;
	ctx.field[0] = surname;
	ctx.field[1] = givenName;
	ctx.field[2] = nickname;
	ctx.field[3] = pronoun;
	ctx.month = month;
	ctx.day = day;
	DbBegin(&b, BGI_DS_PLAIN, 261, 61, MSG_DLG_PROFILE_TITLE, 10);
	DbItem(&b, DLG_CLASS_BUTTON, ST_DEFBUTTON, 210, 35, 40, 14, IDOK, "OK");
	DbItem(&b, DLG_CLASS_STATIC, ST_LABEL, 10, 11, 8, 10, 0x40c, MSG_DLG_SURNAME);
	DbItem(&b, DLG_CLASS_EDIT, ST_EDIT, 22, 10, 45, 12, 0x407, NULL);
	DbItem(&b, DLG_CLASS_STATIC, ST_LABEL, 75, 11, 8, 10, 0x40e, MSG_DLG_GIVEN_NAME);
	DbItem(&b, DLG_CLASS_EDIT, ST_EDIT, 86, 10, 45, 12, 0x410, NULL);
	DbItem(&b, DLG_CLASS_STATIC, ST_LABEL, 128, 37, 25, 10, 0x411, MSG_DLG_PRONOUN);
	DbItem(&b, DLG_CLASS_EDIT, ST_EDIT, 154, 35, 45, 12, 0x412, NULL);
	DbItem(&b, DLG_CLASS_STATIC, ST_LABEL, 10, 37, 23, 10, 0x40d, MSG_DLG_BIRTHDAY);
	DbItem(&b, DLG_CLASS_COMBOBOX, ST_COMBO, 42, 35, 25, 115, 0x409, NULL);
	DbItem(&b, DLG_CLASS_STATIC, ST_LABEL, 69, 37, 10, 10, 0x408, MSG_DLG_MONTH);
	DbItem(&b, DLG_CLASS_COMBOBOX, ST_COMBO, 83, 35, 25, 100, 0x40a, NULL);
	DbItem(&b, DLG_CLASS_STATIC, ST_LABEL, 110, 37, 10, 10, 0x40b, MSG_DLG_DAY);
	DbItem(&b, DLG_CLASS_STATIC, ST_LABEL, 140, 11, 40, 8, 0x40f, MSG_DLG_NICKNAME);
	DbItem(&b, DLG_CLASS_EDIT, ST_EDIT, 182, 10, 45, 12, 0x413, NULL);
	return (int)DialogBoxIndirectParamW(gWin32Instance, DbEnd(&b), gWin32MainWnd, ProfileProc, (LPARAM)&ctx) == 1;
}
