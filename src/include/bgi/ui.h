/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ui.h - the dialog toolkit (src/ui/): dialogs in the classic grey look,
 *        drawn into a 32-bit buffer and driven by keys, typed text and the
 *        pointer; not part of the original engine
 *
 * The engine draws the message boxes and the scripts' dialogs with it in
 * the frame when its host has no dialogs of its own (src/sys/inframe.c),
 * and the X11 host draws its dialog windows with it (src/host/posix/).  A
 * dialog is filled in by its builder (zeroed, a font set, then the Ui_*Add
 * calls and the fields below; src/ui/dialogs.c has the original's), given
 * a buffer (Ui_Alloc), and then fed the events of whoever shows it: Ui_Key
 * for a key press, Ui_Type for the text a key typed, Ui_Press / Ui_Release
 * / Ui_Wheel for the pointer, Ui_Close for a close box.  Each sets `redraw`
 * when the dialog has to be painted again (Ui_Draw) and `done` with
 * `result` once a button ended it.  Shown in the frame, a dialog gets a
 * caption bar and a border of its own (frame.c).
 *
 * The keyboard works as in a Windows dialog: Tab cycles the fields,
 * buttons and lists, Enter activates the focused or the default button,
 * Escape the cancel button, Space a focused button, the arrows move
 * between buttons, through a list or step a number picker.  Texts are in
 * the scripts' encoding: Shift-JIS, or UTF-8 when the dialog's `utf8` is
 * set (the builders set it from their argument); typed text comes as UTF-8
 * and is converted.
 */
#ifndef BGI_UI_H_
#define BGI_UI_H_

#include "bgi/common.h"

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

/* What a dialog draws its text with: `draw` puts `text` (in the dialog's
 * encoding) with its cell's top left at (x, y) into a 32-bit buffer (pitch
 * in pixels, w x h), blended with `rgb`, and returns the pen position after
 * it; `width` measures the first `bytes` bytes (all when < 0); `height` is
 * the cell height.  A NULL font draws nothing and measures 0. */
typedef struct UiFont
{
	void* ctx;
	int height;
	int (*draw)(void* ctx, uint32_t* pix, int pitch, int w, int h, int x, int y, const char* text, uint32_t rgb);
	int (*width)(void* ctx, const char* text, int bytes);
} UiFont_t;

// Fill a rectangle of a 32-bit 0x00RRGGBB buffer (pitch in pixels), clipped to bufW x bufH.
void Ui_FillRect(uint32_t* pix, int pitch, int bufW, int bufH, int x, int y, int w, int h, uint32_t rgb);
// the width of `text` in `font` (all of it), 0 without a font
int Ui_FontWidth(const UiFont_t* font, const char* text);
// the bytes of the character at `s` in the encoding (UTF-8 when `utf8`, else Shift-JIS); 0 at the end
int Ui_CharLen(const char* s, int utf8);
/* UTF-8 text into the encoding (whole characters into `out`, n bytes,
 * NUL-terminated): control characters are left out, and towards Shift-JIS
 * a character without a code too (the Basic Multilingual Plane only); a
 * character beyond it is two three-byte sequences in UTF-8, as the
 * scripts write it.  The length written. */
int Ui_FromUtf8(const char* utf8, char* out, int n, int utf8Out);
// Shift-JIS text into the encoding (copied, or as UTF-8; whole characters into `out`, n bytes, NUL-terminated); the length written
int Ui_FromSjis(const char* sjis, char* out, int n, int utf8Out);

// ---- a single-line text field (field.c) -------------------------------------------------------

typedef struct UiField
{
	char text[0x100]; // NUL-terminated
	int limit;        // bytes, 0 = unlimited (the buffer size still applies)
	int caret;        // byte offset of the caret in `text`
	int selAll;       // the whole text is selected: typing replaces it
	int noAscii;      // reject single-byte printable characters (the edit control's "B0 29")
	int utf8;         // the text is UTF-8 rather than Shift-JIS (set before the text)
} UiField_t;

#define UI_FIELD_TYPES 2 // UiField_Key: a key that types a character: its text decides (UiField_Type)

void UiField_SetText(UiField_t* f, const char* text); // replace the text (cut to the limit), caret at the end, nothing selected
void UiField_SelectAll(UiField_t* f);                 // select everything (nothing when empty)
/* A key press with `vk` (a Windows virtual key) and Ctrl / Alt as held:
 * the editing keys (BackSpace, Delete, the arrows, Home, End, Ctrl+A).  1
 * when the text changed, 0 when the key was handled without a change, -1
 * when it is not the field's (Tab, Enter, Escape, other Ctrl / Alt
 * combinations), UI_FIELD_TYPES for any other key. */
int UiField_Key(UiField_t* f, int vk, int ctrl, int alt);
/* Text typed at the caret (UTF-8, converted to the field's encoding),
 * replacing the selection; what does not fit the limit or the buffer is
 * dropped by whole characters.  1 when the text changed, 0 when nothing
 * was taken (a "B0 29" field rejects a typed single-byte printable
 * character). */
int UiField_Type(UiField_t* f, const char* utf8);
/* draw the field into a 32-bit buffer (pitch in pixels, bufW x bufH) at
 * (x, y, w, h): the text (centred when `centred`, else left-aligned and
 * scrolled to keep the caret in view) with the selection and caret when
 * `focused`; `frame` adds the sunken white box of a dialog field */
void UiField_Draw(const UiField_t* f, uint32_t* pix, int pitch, int bufW, int bufH, int x, int y, int w, int h,
	const UiFont_t* font, uint32_t rgb, int centred, int focused, int frame);

// ---- a dialog (uikit.c) -------------------------------------------------------------------------

#define UI_MAX_LABELS  20
#define UI_MAX_FIELDS  6
#define UI_MAX_BUTTONS 6
#define UI_MAX_LISTS   1
#define UI_LIST_ROW_H  (UI_FONT_H + 4) // pixels per row of a list box

typedef struct UiLabel
{
	int x, y;         // top-left in dialog pixels
	char text[0x200]; // in the dialog's encoding
} UiLabel_t;

typedef struct UiFieldW
{
	int x, y, w, h;             // in dialog pixels
	UiField_t f;                // the text and its editing state
	int numeric, min, max;      // a number picker standing in for a combo box: digits only, kept within min .. max
	int digits;                 // a digits-only field: typing adds only digits, and a '-' to an empty field
	const char* const* choices; // a choice picker: up / down step through these texts (typing is still free)
	int nChoices;
	int pick; // .. and nothing else: a click or the wheel steps too, nothing is typed into it
} UiFieldW_t;

/* a list box: rows of texts, one selected, scrolled by the keyboard or the
 * wheel; a second click on the selected row activates the default button */
typedef struct UiList
{
	int x, y, w, h;           // in dialog pixels
	const char* const* items; // (not copied)
	int count;
	int selected; // -1 for none
	int top;      // the first row shown
} UiList_t;

typedef struct UiButton
{
	int x, y, w, h;   // in dialog pixels
	const char* text; // the caption (not copied)
	int id;           // what ends the dialog as its result
} UiButton_t;

typedef struct UiDialog UiDialog_t;
struct UiDialog
{
	int w, h;             // the client size in pixels (set by the builder)
	uint32_t* pix;        // the w * h buffer the dialog is painted into (Ui_Alloc)
	const UiFont_t* font; // what the texts are drawn and measured with (set before building)
	char title[0x200];    // the caption (the builders set it)
	int utf8;             // the texts are UTF-8 (the fields take it from here)
	UiLabel_t labels[UI_MAX_LABELS];
	int nLabels;
	UiFieldW_t fields[UI_MAX_FIELDS];
	int nFields;
	UiButton_t buttons[UI_MAX_BUTTONS];
	int nButtons;
	UiList_t lists[UI_MAX_LISTS];
	int nLists;
	int focus;          // 0 .. nFields-1 a field, then the buttons, then the lists
	int defButton;      // index, -1 none: Enter
	int escButton;      // index, -1 none: Escape and the close box
	int icon;           // 0 none, else the character drawn in the icon disc
	uint32_t iconRgb;   // the disc's colour
	int pressed;        // button index under a held mouse button, -1 none
	uint32_t lastClick; // the time (ms) of the last click on a list row, for the double click
	int result, done;   // the id of the button that ended the dialog; set once it did
	// a button handler may refuse to close (returns 0); id is the button's id
	int (*onButton)(UiDialog_t* d, int id, void* ctx);
	void* ctx; // handed to onButton, onChange and onListSelect
	// a field changed (the month picker rebuilds the day range)
	void (*onChange)(UiDialog_t* d, int field, void* ctx);
	// a list's selection changed (a click or the keyboard); `list` is its index
	void (*onListSelect)(UiDialog_t* d, int list, void* ctx);
	int redraw;   // the dialog has to be painted again (the events set it, as may a handler that changed a text)
	int inactive; // Ui_FrameDraw: the caption bar as an inactive window's (grey)
	/* a refusing onButton may leave a message here for whoever shows the
	 * dialog to put up in a box of its own (with `complaintCaption` and the
	 * OPENBGI_MB_* `complaintFlags`) before the dialog goes on */
	char complaint[0x200];
	char complaintCaption[0x100];
	uint32_t complaintFlags;
	int aux[4]; // what a builder keeps for its handlers
};

// Add a label / a text field (`limit` bytes, 0 unlimited) / a button returning `id` / a list box; the index, -1 when full.
int Ui_LabelAdd(UiDialog_t* d, int x, int y, const char* text);
int Ui_FieldAdd(UiDialog_t* d, int x, int y, int w, const char* initial, int limit);
int Ui_ButtonAdd(UiDialog_t* d, int x, int y, const char* text, int id);
int Ui_ListAdd(UiDialog_t* d, int x, int y, int w, int rows, const char* const* items, int count, int selected);
// Replace a label's text (the caller sets `redraw`).
void Ui_LabelSet(UiDialog_t* d, int label, const char* text);
// The width of a text in the dialog's font, in pixels.
int Ui_TextWidth(const UiDialog_t* d, const char* text);
// The buffer for w x h pixels, and no button pressed; 0 when out of memory.  Ui_Free releases it.
int Ui_Alloc(UiDialog_t* d);
void Ui_Free(UiDialog_t* d);
// Paint the whole dialog into its buffer.
void Ui_Draw(UiDialog_t* d);

#define UI_KEY_NONE 0 // the key did nothing
#define UI_KEY_DONE 1 // the key was taken
#define UI_KEY_TYPE 2 // a field has the focus and the key may type: its text goes to Ui_Type
/* A key press: `vk` a Windows virtual key, Shift / Ctrl / Alt as held. */
int Ui_Key(UiDialog_t* d, int vk, int shift, int ctrl, int alt);
/* Text typed into the focused field (UTF-8); `vk` is the key that typed
 * it, 0 for text that comes on its own: a key that typed nothing still
 * counts as taken.  UI_KEY_* as Ui_Key: UI_KEY_NONE when no field has the
 * focus. */
int Ui_Type(UiDialog_t* d, int vk, const char* utf8);
// The left button pressed / released at (x, y) of the client area; `ms` a clock for the double click.
void Ui_Press(UiDialog_t* d, int x, int y, uint32_t ms);
void Ui_Release(UiDialog_t* d, int x, int y);
// The wheel turned `notches` (positive away from the player) over (x, y): a list scrolls by three rows a notch.
void Ui_Wheel(UiDialog_t* d, int x, int y, int notches);
// The close box: the cancel button, nothing without one.
void Ui_Close(UiDialog_t* d);
// The focused field, -1 when a button or a list has the focus.
int Ui_FocusedField(const UiDialog_t* d);

// ---- the caption bar and the border of a dialog in the frame (frame.c) ---------------------------

#define UI_BORDER      3  // pixels of border around the caption bar and the client area
#define UI_CAPTION_H   20 // the caption bar's height
#define UI_HIT_NONE    0
#define UI_HIT_CLIENT  1 // the client area: *cx, *cy are the client's coordinates
#define UI_HIT_CAPTION 2 // the caption bar, where a drag moves the dialog
#define UI_HIT_CLOSE   3 // the close box (only with a cancel button)

// the size of the dialog with its border and caption bar
void Ui_FrameSize(const UiDialog_t* d, int* w, int* h);
/* the border, the caption bar with the title and the close box (pressed
 * when `closeDown`) and, when the dialog has its buffer, the client area
 * painted last (Ui_Draw) into a buffer of Ui_FrameSize (pitch in pixels) */
void Ui_FrameDraw(const UiDialog_t* d, uint32_t* pix, int pitch, int closeDown);
// what (x, y) of the framed dialog is (UI_HIT_*)
int Ui_FrameHit(const UiDialog_t* d, int x, int y, int* cx, int* cy);

// ---- the original's dialogs (dialogs.c) ---------------------------------------------------------

/* The builders lay a dialog out (its font set, the rest zeroed) and set its
 * title; `utf8` the texts are UTF-8.  The message box: the buttons of the
 * OPENBGI_MB_* flags (OK, OK / Cancel, Yes / No, Retry / Cancel), the icon as
 * a coloured disc with a letter, the text split at its line breaks (at
 * most 16 lines) and, with `maxW` > 0, wrapped to a dialog no wider than
 * that; its result is the OPENBGI_ID* of the button. */
void Ui_BuildMessageBox(UiDialog_t* d, const char* text, const char* caption, uint32_t flags, int utf8, int maxW);
/* dialog 0x71: a field with the initial text, OK / Cancel (result 1 / 0);
 * the entry is fields[0] */
void Ui_BuildInput1(UiDialog_t* d, const char* title, const char* initial, int maxLen, int digits, int utf8);
/* dialog 0x72: two labelled fields, OK / Cancel; `compact` (0x7E, "B0 87")
 * puts them side by side; the entries are fields[0] and fields[1] */
void Ui_BuildInput2(UiDialog_t* d, const char* title, const char* label1, const char* initial1, int maxLen1, int digits1,
	const char* label2, const char* initial2, int maxLen2, int digits2, int compact, int utf8);
/* dialog 0x77: four name fields of 10 bytes and the birthday as two number
 * pickers, OK only (a name with a single-byte character is refused with a
 * complaint); month and day are 0-based */
void Ui_BuildProfile(UiDialog_t* d, const char* surname, const char* givenName, const char* nickname, const char* pronoun,
	int month, int day, int utf8);
// the profile's entries once it ended with OK: the names into the 0x100-byte buffers, month and day 0-based
void Ui_ProfileResult(const UiDialog_t* d, char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month,
	int32_t* day);

// ---- OpenBGI's own (dialogs.c) ------------------------------------------------------------------

#define UI_SETTINGS_SIZE       0 // the fields of the display settings, in their order
#define UI_SETTINGS_FILTER     1
#define UI_SETTINGS_QUALITY(d) ((d)->aux[0] ? 2 : -1) // (only with filters that have a quality)
#define UI_SETTINGS_WHOLE(d)   (2 + (d)->aux[0])
#define UI_SETTINGS_FULL(d)    (3 + (d)->aux[0])
#define UI_SETTINGS_RESET      2 // the result of "Reset window size" (a handler refuses it: the box stays)

/* The display settings: choice pickers that only step - the window's size
 * (VIEW_SIZE_* of bgi/display.h), the filter (one of the nFilters names,
 * kept by the caller: the four of VIEW_FILTER_* and then the host's own;
 * NULL for the four alone), the filter's quality (0 fast, 1 high; below 0:
 * no such picker), whole steps and full screen (0 / 1) - with "Reset
 * window size" and "Close" (result 0, also Escape and the close box).
 * Ui_PickValue tells which of its texts a picker shows, Ui_PickSet makes
 * it show one (the caller sets `redraw`). */
void Ui_BuildDisplaySettings(UiDialog_t* d, int size, const char* const* filters, int nFilters, int filter, int quality, int whole,
	int fullscreen, int utf8);
int Ui_PickValue(const UiDialog_t* d, int field);
void Ui_PickSet(UiDialog_t* d, int field, int value);

#endif // BGI_UI_H_
