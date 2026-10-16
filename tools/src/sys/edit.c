/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * edit.c - the text entry control of "B0 20" .. "B0 27"; interface in
 *          bgi/edit.h
 *
 * One single-line edit control sits over the main window; the control
 * itself belongs to the window system (OS_Edit*).  This file holds the
 * script-visible state: the text applied by the next setup, the rectangle
 * of the last setup and the colour.  The original's subclass procedure -
 * Tab / Enter end the entry and hand the focus back to the main window,
 * WM_PAINT re-presents the picture under the transparent control - is
 * split between the OS layer and Present_EditPaint (present.c).
 */
#include "bgi/edit.h"
#include "bgi/display.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/os.h"

static char gEditText[EDIT_TEXT_MAX]; // empty until "B0 26" (the control itself is created with MSG_EDIT_DEFAULT_TEXT)
static Rect_t gEditRect;              // the client rectangle of the last Edit_Setup, inclusive
static uint32_t gEditColour;          // COLORREF (0x00BBGGRR)

// "B0 24": show or hide the control
void Edit_Show(int show)
{
	OS_EditShow(show != 0);
}

/* "B0 20": place the control at (x, y) with a w x h client area, give it
 * the font `fontNo` at `size` pixels and a text limit of `maxLen` bytes,
 * put the pending text (Edit_SetText) in it selected, and show it;
 * `focus` non-zero also gives it the keyboard.  0 ok, 1 w or h outside
 * 8 .. the picture size, 2 unknown font number, 3 size outside 8 .. 0x40,
 * 4 maxLen outside 1 .. 0x100. */
int Edit_Setup(int x, int y, int w, int h, int fontNo, int size, int maxLen, int focus)
{
	const char* face;
	if(w < 8 || w > gScreenWidths[gSizeIndex] || h < 8 || h > gScreenHeights[gSizeIndex])
		return 1;
	face = FontNameByNo(fontNo);
	if(!face)
		return 2;
	if(size < 8 || size > 0x40)
		return 3;
	if(maxLen < 1 || maxLen > 0x100)
		return 4;

	Edit_Show(1);
	OS_EditMove(x, y, w, h);
	OS_EditSetFont(face, size);
	OS_EditLimitText(maxLen);
	OS_EditSetText(gEditText);
	OS_EditSelectAll();
	OS_EditFocus(focus != 0);
	gEditRect.l = x;
	gEditRect.t = y;
	gEditRect.r = x + w - 1;
	gEditRect.b = y + h - 1;
	return 0;
}

// "B0 26": the text the next Edit_Setup puts in the control (truncated to 0xff bytes)
void Edit_SetText(const char* s)
{
	// the original copies without a bound into the 0x100 byte buffer
	strncpy(gEditText, s, sizeof gEditText - 1);
	gEditText[sizeof gEditText - 1] = 0;
}

// "B0 27": the control's current text into buf (0x100 bytes); returns its length
int Edit_GetText(char* buf)
{
	return OS_EditGetText(buf, EDIT_TEXT_MAX);
}

void Edit_GetRect(Rect_t* out)
{
	*out = gEditRect;
}

// "B0 25": the text colour as 0x00RRGGBB; stored as a COLORREF (0x00BBGGRR)
void Edit_SetColour(uint32_t rgb)
{
	gEditColour = ((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff);
	OS_EditSetColour(rgb);
}

uint32_t Edit_GetColour(void)
{
	return gEditColour;
}
