/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dlgtemplate.c - dialog templates built in memory for the Win32 back end
 *                 (ui_internal.h)
 *
 * The original keeps its dialogs as resources and runs them with
 * DialogBoxParamA.  This build has no resource section, so the templates
 * are assembled here from the layouts of the original's resources
 * (positions and sizes in dialog units, control ids, styles) with the
 * strings coming from msg.h, and run with DialogBoxIndirectParamW.  The
 * templates of the original are all DIALOG (not DIALOGEX) resources except
 * the progress dialog, whose only DIALOGEX-specific fields are the default
 * font weight and charset; a plain template reproduces it.  Every string
 * is Shift-JIS from msg.h and goes in as UTF-16.
 *
 * A DlgBuilder_t writes little-endian words into its buffer: DbBegin the
 * DLGTEMPLATE header, DbItem / DbItemClass one DWORD-aligned
 * DLGITEMTEMPLATE each, DbEnd patches the item count in.  A template that
 * outgrows the 4 KB buffer is cut short (the words are dropped, the
 * length keeps counting); none of the engine's dialogs comes close.
 */
#include "ui_internal.h"

// one 16-bit word, little-endian; dropped when the buffer is full
static void DbWord(DlgBuilder_t* b, uint16_t v)
{
	if(b->len + 2 <= sizeof b->buf)
	{
		b->buf[b->len] = (uint8_t)v;
		b->buf[b->len + 1] = (uint8_t)(v >> 8);
	}
	b->len += 2;
}

static void DbDword(DlgBuilder_t* b, uint32_t v)
{
	DbWord(b, (uint16_t)v);
	DbWord(b, (uint16_t)(v >> 16));
}

static void DbAlign(DlgBuilder_t* b) // items start on a DWORD boundary
{
	while(b->len & 3)
		DbWord(b, 0);
}

// a Shift-JIS string as NUL-terminated UTF-16 (Win32_ToWide); an empty string for NULL or ""
static void DbString(DlgBuilder_t* b, const char* sjis)
{
	WCHAR w[0x200];
	int n = Win32_ToWide(sjis, w, (int)BGI_COUNTOF(w));
	for(int i = 0; i <= n; i++) // includes the terminator
		DbWord(b, (uint16_t)w[i]);
}

/* the DLGTEMPLATE header: `style` as in the resource (DS_SETFONT is added
 * with the point size and face), no menu, the default class, the dialog
 * at (0, 0) - Dlg_Centre moves it on WM_INITDIALOG */
void DbBegin(DlgBuilder_t* b, uint32_t style, int cx, int cy, const char* title, int fontPt)
{
	b->len = 0;
	b->items = 0;
	DbDword(b, style | DS_SETFONT);
	DbDword(b, 0); // dwExtendedStyle
	b->cditAt = b->len;
	DbWord(b, 0); // cdit, patched by DbEnd
	DbWord(b, 0); // x
	DbWord(b, 0); // y
	DbWord(b, (uint16_t)cx);
	DbWord(b, (uint16_t)cy);
	DbWord(b, 0); // no menu
	DbWord(b, 0); // the default dialog class
	DbString(b, title);
	DbWord(b, (uint16_t)fontPt);
	DbString(b, MSG_DLG_FONT_FACE);
}

// the fixed part of a DLGITEMTEMPLATE (aligned); the class, text and creation data follow
static void DbItemRaw(DlgBuilder_t* b, uint32_t style, uint32_t exStyle, int x, int y, int cx, int cy, int id)
{
	DbAlign(b);
	DbDword(b, style);
	DbDword(b, exStyle);
	DbWord(b, (uint16_t)x);
	DbWord(b, (uint16_t)y);
	DbWord(b, (uint16_t)cx);
	DbWord(b, (uint16_t)cy);
	DbWord(b, (uint16_t)id);
	b->items++;
}

// a control of one of the predefined classes: the class as the 0xFFFF + atom pair
void DbItem(DlgBuilder_t* b, int cls, uint32_t style, int x, int y, int cx, int cy, int id, const char* text)
{
	DbItemRaw(b, style, 0, x, y, cx, cy, id);
	DbWord(b, 0xffff);
	DbWord(b, (uint16_t)cls);
	DbString(b, text);
	DbWord(b, 0); // no creation data
}

// a control of a named class (the progress bars): the class as a string
void DbItemClass(DlgBuilder_t* b, const char* cls, uint32_t style, uint32_t exStyle, int x, int y, int cx,
	int cy, int id, const char* text)
{
	DbItemRaw(b, style, exStyle, x, y, cx, cy, id);
	DbString(b, cls);
	DbString(b, text);
	DbWord(b, 0);
}

// patch the item count into the header; the template is the buffer itself
const DLGTEMPLATE* DbEnd(DlgBuilder_t* b)
{
	b->buf[b->cditAt] = (uint8_t)b->items;
	b->buf[b->cditAt + 1] = (uint8_t)(b->items >> 8);
	return (const DLGTEMPLATE*)b->buf;
}

// centre a dialog on the screen and make it top-most, as the original's dialog procedures do on WM_INITDIALOG
void Dlg_Centre(HWND hwnd)
{
	RECT r;
	int w, h;
	GetWindowRect(hwnd, &r);
	w = GetSystemMetrics(SM_CXSCREEN) + (r.left - r.right) - 1; // the screen width minus the dialog's, less one
	h = GetSystemMetrics(SM_CYSCREEN) + (r.top - r.bottom) - 1;
	SetWindowPos(hwnd, HWND_TOPMOST, w / 2, h / 2, 0, 0, SWP_NOSIZE | SWP_FRAMECHANGED);
}

// copy at most `limit` bytes (when limit > 0; at most 0xff in any case) of an initial text into a zeroed 0x100-byte buffer
void Dlg_InitialText(char* buf, const char* text, int limit)
{
	size_t len = strlen(text);
	memset(buf, 0, 0x100);
	if(limit > 0 && (size_t)limit < len)
		len = (size_t)limit;
	if(len > 0xff)
		len = 0xff;
	memcpy(buf, text, len);
}

// ---- the engine's strings in a dialog's controls -----------------------------------------------

void Dlg_SetTitle(HWND hwnd, const char* text)
{
	WCHAR wide[WIN32_WPATH];
	Win32_ToWide(text, wide, (int)BGI_COUNTOF(wide));
	SetWindowTextW(hwnd, wide);
}

void Dlg_SetText(HWND hwnd, int id, const char* text)
{
	WCHAR wide[WIN32_WPATH];
	Win32_ToWide(text, wide, (int)BGI_COUNTOF(wide));
	SetDlgItemTextW(hwnd, id, wide);
}

int Dlg_GetText(HWND hwnd, int id, char* out, int n)
{
	WCHAR wide[WIN32_WPATH];
	wide[0] = 0;
	GetDlgItemTextW(hwnd, id, wide, (int)BGI_COUNTOF(wide));
	return Win32_TextFromWide(wide, out, n);
}

int Dlg_GetPath(HWND hwnd, int id, char* out, int n)
{
	WCHAR wide[WIN32_WPATH];
	wide[0] = 0;
	GetDlgItemTextW(hwnd, id, wide, (int)BGI_COUNTOF(wide));
	return Win32_PathFromWide(wide, out, n);
}

void Dlg_AddString(HWND hwnd, int id, UINT msg, const char* text)
{
	WCHAR wide[WIN32_WPATH];
	Win32_ToWide(text, wide, (int)BGI_COUNTOF(wide));
	SendDlgItemMessageW(hwnd, id, msg, 0, (LPARAM)wide);
}
