/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ui_internal.h - shared between the dialog files of the POSIX back end
 *                 (field.c, uikit.c, msgbox.c, dialogs.c): the colours and
 *                 metrics of the classic look and the dialog toolkit of
 *                 uikit.c, on which the message box and the script's
 *                 dialogs are built
 */
#ifndef BGI_OS_POSIX_UI_INTERNAL_H
#define BGI_OS_POSIX_UI_INTERNAL_H

#include "bgi/os_x11.h"
#include "bgi/os_common.h"
#include "bgi/msg.h"

#include <stdio.h>

// ---- colours and metrics of the classic look --------------------------------------------------
#define UI_FACE        0xf0f0f0 // 0x00RRGGBB: the dialog background and the buttons
#define UI_TEXT        0x000000
#define UI_FIELD_BG    0xffffff // the text fields
#define UI_SELECT_BG   0x3399ff // the selection highlight and its text
#define UI_SELECT_TEXT 0xffffff
#define UI_SHADOW      0xa0a0a0 // the 3D edges: shadow, dark, light
#define UI_DARK        0x696969
#define UI_LIGHT       0xffffff
#define UI_FRAME       0x7a7a7a // the outline of the default button

#define UI_FONT_H      16 // pixels: the UI font's cell height
#define UI_MARGIN      12 // pixels between the border and the controls
#define UI_BTN_W       80 // push button size in pixels
#define UI_BTN_H       24
#define UI_FIELD_H     22 // text field height in pixels

// Fill a rectangle of a 32-bit 0x00RRGGBB buffer, clipped to it (field.c).
void Ui_FillRect(uint32_t* pix, int pitch, int bufW, int bufH, int x, int y, int w, int h, uint32_t rgb);

// ---- uikit.c: a dialog --------------------------------------------------------------------------

#define UI_MAX_LABELS  12
#define UI_MAX_FIELDS  6
#define UI_MAX_BUTTONS 6
#define UI_MAX_LISTS   1
#define UI_LIST_ROW_H  (UI_FONT_H + 4) // pixels per row of a list box

typedef struct UiLabel
{
	int x, y;         // top-left in dialog pixels
	char text[0x200]; // Shift-JIS
} UiLabel_t;

typedef struct UiFieldW
{
	int x, y, w, h;             // in dialog pixels
	X11Field_t f;               // the text and its editing state
	int numeric, min, max;      // a number picker standing in for a combo box: digits only, kept within min .. max
	const char* const* choices; // a choice picker: up / down step through these texts (typing is still free)
	int nChoices;
} UiFieldW_t;

/* a list box: rows of texts, one selected, scrolled by the keyboard, the
 * wheel or by dragging past the edges; a double click activates the
 * default button */
typedef struct UiList
{
	int x, y, w, h;           // in dialog pixels
	const char* const* items; // Shift-JIS (not copied)
	int count;
	int selected; // -1 for none
	int top;      // the first row shown
} UiList_t;

typedef struct UiButton
{
	int x, y, w, h;   // in dialog pixels
	const char* text; // the caption (not copied)
	int id;           // what Ui_Run returns when it ends the dialog
} UiButton_t;

/* A dialog is filled in by its caller (zeroed, then Ui_*Add and the
 * fields below), run with Ui_Run and read back from `fields` afterwards. */
typedef struct UiDialog UiDialog_t;
struct UiDialog
{
	Window win;          // the dialog's top-level window while it runs
	int w, h;            // client size in pixels (set by the caller)
	uint32_t* pix;       // the w * h back buffer the dialog is painted into
	struct OsFont* font; // the UI font (OsPosix_FontUi)
	XIC ic;              // the input context, NULL without an input method or fields
	UiLabel_t labels[UI_MAX_LABELS];
	int nLabels;
	UiFieldW_t fields[UI_MAX_FIELDS];
	int nFields;
	UiButton_t buttons[UI_MAX_BUTTONS];
	int nButtons;
	UiList_t lists[UI_MAX_LISTS];
	int nLists;
	int focus;               // 0 .. nFields-1 a field, then the buttons, then the lists
	int defButton;           // index, -1 none: Enter
	int escButton;           // index, -1 none: Escape and the close box
	int icon;                // 0 none, else the character drawn in the icon disc
	uint32_t iconRgb;        // the disc's colour
	int pressed;             // button index under a held mouse button, -1 none
	unsigned long lastClick; // the time of the last click on a list row, for the double click
	int result, done;        // the id of the button that ended the dialog; set once it did
	// a button handler may refuse to close (returns 0); id is the button's id
	int (*onButton)(UiDialog_t* d, int id, void* ctx);
	void* ctx; // handed to onButton and onChange
	// a field changed (the month picker rebuilds the day range)
	void (*onChange)(UiDialog_t* d, int field, void* ctx);
	// a list's selection changed (a click or the keyboard); `list` is its index
	void (*onListSelect)(UiDialog_t* d, int list, void* ctx);
	int redraw; // set by a handler that changed labels or fields: the dialog is painted again
};

// Add a label / a text field (`limit` bytes, 0 unlimited) / a button returning `id` / a list box; the index, -1 when full.
int Ui_LabelAdd(UiDialog_t* d, int x, int y, const char* sjis);
int Ui_FieldAdd(UiDialog_t* d, int x, int y, int w, const char* initial, int limit);
int Ui_ButtonAdd(UiDialog_t* d, int x, int y, const char* text, int id);
int Ui_ListAdd(UiDialog_t* d, int x, int y, int w, int rows, const char* const* items, int count, int selected);
// Replace a label's text while the dialog runs (the painter is run by the caller through `redraw`).
void Ui_LabelSet(UiDialog_t* d, int label, const char* sjis);
// Create the window centred on the screen and run the modal loop; the id of the button that ended it.
int Ui_Run(UiDialog_t* d, const char* titleSjis);
// The width of a string in the UI font, in pixels.
int Ui_TextWidth(const char* sjis);

#endif
