/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * edit.h - the text entry control of "B0 20" .. "B0 27" (src/sys/edit.c)
 *
 * One single-line edit control sits over the main window.  It is created
 * hidden with the window; a script positions, sizes and shows it with
 * Edit_Setup, reads it back with Edit_GetText.  The control is transparent:
 * the main window's back buffer shows through, re-presented under it from
 * the control's paint event (Present_EditPaint).
 */
#ifndef BGI_EDIT_H_
#define BGI_EDIT_H_

#include "bgi/common.h"
#include "bgi/gfx/bitmap.h"

#define EDIT_TEXT_MAX 0x100 // the text buffer and GetWindowText limit, bytes

void Edit_Show(int show); // "B0 24": show or hide the control
/* "B0 20": 0 ok, 1 size outside 8 .. picture size, 2 unknown font number,
 * 3 font size outside 8 .. 0x40, 4 maxLen outside 1 .. 0x100.  Shows the
 * control with the current text selected; `focus` gives it the keyboard. */
int Edit_Setup(int x, int y, int w, int h, int fontNo, int size, int maxLen, int focus);
void Edit_SetText(const char* s);  // "B0 26": the text applied by the next Edit_Setup
int Edit_GetText(char* buf);       // "B0 27": the control's current text (0x100 bytes); its length
void Edit_GetRect(Rect_t* out);    // the client rectangle of the last setup
void Edit_SetColour(uint32_t rgb); // "B0 25": text colour 0x00RRGGBB
uint32_t Edit_GetColour(void);     // as stored (BGR for the window system)

#endif // BGI_EDIT_H_
