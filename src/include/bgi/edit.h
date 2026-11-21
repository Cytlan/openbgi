/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * edit.h - the text entry control of "B0 20" .. "B0 2A" (src/sys/edit.c)
 *
 * One edit control sits over the main window: made hidden with the window
 * up to 1.69/451, made by each setup and destroyed by "B0 21" from
 * 1.69/472 on; a script positions, sizes and shows it with Edit_Setup and
 * reads it back with Edit_GetText.  The control is transparent: the main
 * window's picture shows through.  A host with an entry of its own
 * (OpenBGI_Host_t.textEntry) shows it, and puts the picture back under it
 * from the last frame when it paints; otherwise the engine
 * draws it over the frame's picture and takes its input (Edit_Frame and
 * Edit_Key .. Edit_Over, from the frame and the input calls of openbgi.h).
 */
#ifndef BGI_EDIT_H_
#define BGI_EDIT_H_

#include "bgi/common.h"
#include "openbgi.h"
#include "bgi/gfx/bitmap.h"

#define EDIT_TEXT_MAX 0x100 // the text buffer and GetWindowText limit, bytes

void Edit_Show(OpenBGI_t* o, int show); // "B0 24": show or hide the control
/* "B0 20": 0 ok, 1 size outside 8 .. picture size, 2 unknown font number,
 * 3 font size outside 8 .. 0x40, 4 maxLen outside 1 .. 0x100.  Shows the
 * control with the current text selected; `focus` gives it the keyboard. */
int Edit_Setup(OpenBGI_t* o, int x, int y, int w, int h, int fontNo, int size, int maxLen, int focus);
void Edit_SetText(OpenBGI_t* o, const char* s);  // "B0 26": the text applied by the next Edit_Setup
int Edit_SetFontWidthPct(OpenBGI_t* o, int pct); // "B0 22" (1.529 on): the next Edit_Setup's glyph width in percent, 25 .. 200; 1 taken, 0 out of range
int Edit_Remove(OpenBGI_t* o);                   // "B0 21" (1.69/472 on): the control is destroyed; 0
int Edit_GetText(OpenBGI_t* o, char* buf);       // "B0 27": the control's current text (0x100 bytes); its length, -1 without a control
void Edit_SetColour(OpenBGI_t* o, uint32_t rgb); // "B0 25": text colour 0x00RRGGBB
void Edit_RejectAscii(OpenBGI_t* o, int reject); // "B0 29" (1.69/472 on): single-byte printable characters are swallowed
void Edit_SetCloseOnEnter(OpenBGI_t* o, int on); // "B0 28" (1.69/472 on): Enter hides the control as well
int Edit_SetAlign(OpenBGI_t* o, uint32_t align); // "B0 2A" (1.636 on): the next setup's alignment, 0 left, 1 centred, 2 right; 1 taken, 0 not
int Edit_Shown(OpenBGI_t* o);                    // "B0 23" (1.69/472 on): the control is shown (its input method status window open)
void Edit_Destroy(OpenBGI_t* o);                 // the control goes with the window (the close, the engine's shutdown)
/* A pad took the turn: a text entry of the host's that is shown becomes
 * the engine's, where the pad and the on-screen keyboard reach it - at
 * its place, with its text, the caret at the end.  The next setup made
 * while a pad has no turn is the host's again. */
void Edit_ToFrame(OpenBGI_t* o);

/* The engine's entry (without a host's): at the end of a frame it is
 * drawn again when it changed, and says when it takes text (the frame's
 * textInput).  The input calls ask it first: Edit_Key and Edit_Text take
 * a key or text while it has the focus, Edit_Button a press on it (and
 * that button's release) - a press elsewhere only takes its focus away;
 * Edit_Over says that the pointer is over it, where the main window gets
 * no moves.  1 when taken.  Edit_Rect is for the input mapper, which
 * lists the entry among what a click reaches. */
void Edit_Frame(OpenBGI_t* o, OpenBGI_Frame_t* f);
int Edit_Key(OpenBGI_t* o, int vk, int down, int flags);
int Edit_Text(OpenBGI_t* o, const char* utf8);
int Edit_Button(OpenBGI_t* o, int button, int down, int x, int y);
int Edit_Over(OpenBGI_t* o, int x, int y);
int Edit_Rect(OpenBGI_t* o, OpenBGI_Rect_t* out); // where the engine's entry lies in the picture: 0 not up, 1 up, 2 up with the keys
void Edit_Unfocus(OpenBGI_t* o);                  // another window drawn in the frame was clicked: the entry has the focus no more

#endif // BGI_EDIT_H_
