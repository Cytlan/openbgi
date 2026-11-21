/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * osk.c - the on-screen keyboard: a table of kana and letters the engine
 *         draws over the picture, typed on with a pad or a pointer
 *
 * It serves the text fields the engine draws in the frame: the scripts'
 * text entry ("B0 20", edit.c) and the fields of the dialogs
 * (inframe.c).  Osk_Shown notes at the end of each frame whether one of
 * them has the keys, and where it lies, and paints the keyboard when it
 * changed; Osk_Frame, before the next pass, brings the keyboard up or
 * puts it away:
 *
 *   - it comes up when the scripts' entry gets the keys while a pad is
 *     in use, or a touch screen on a host without a keyboard
 *     (OPENBGI_CAP_KEYBOARD); when such a touch screen's tap lands in a
 *     field - unless the settings keep it down ("onScreenKeyboard" of
 *     bgi/inputcfg.h); and when it is asked for (Osk_Open: a pad's
 *     CONFIRM on a field, the action KEYBOARD);
 *   - it goes when the field loses the keys, with its key "done", with
 *     the action KEYBOARD, and when the player types on a real keyboard
 *     (Input_DeviceUsed).
 *
 * There are four tables of OSK_ROWS x OSK_COLS characters - hiragana,
 * katakana, full-width and half-width letters, digits and signs - and a
 * row of keys below them: the tables, the space, the caret left and
 * right, delete and done.  A key types its character as text of the
 * mapper's own (Steps_TextNow), as an input method's would arrive; the
 * caret keys, delete and - for the scripts' entry - done are the keys
 * Left, Right, Backspace and Enter (Steps_KeyTap).  Kana are typed as
 * they are: there is no conversion to kanji.
 *
 * With a pad the directions move over the keys (around the edges),
 * CONFIRM types, CANCEL deletes, BACKLOG and FORWARD move the caret, SKIP
 * turns to the next table and KEYBOARD puts the keyboard away.  A
 * pointer types with its left button: the press and the release on the
 * same key.  While the keyboard is up the frames ask the host for no
 * text input (Osk_Shown clears textInput), so that a host's own
 * on-screen keyboard stays down.
 *
 * The keyboard lies along the bottom of the picture, or along the top
 * when the field is at the bottom.  It is drawn in the dialogs' typeface
 * at a size to fit the keys; without one the keys are blank.
 */
#include <string.h>

#include "bgi/input.h"
#include "bgi/inframe.h"
#include "bgi/display.h"
#include "bgi/ui.h"
#include "bgi/msg.h"
#include "bgi/gfx/typeface.h"
#include "bgi/core.h"

#define EDGE      6                           // pixels around the keys
#define KEY_MIN   12                          // a table key's size ..
#define KEY_MAX   64                          // .. at least and at most
#define BOARD     0x303438u                   // 0x00RRGGBB: between the keys
#define KEY_ON    0x505860u                   // the key of the table shown
#define HELD_NONE ((OSK_ROWS + 1) * OSK_COLS) // `press`: the pointer went down on the keyboard, on no key

// the tables: Shift-JIS, two bytes a key; the last one ASCII, a byte a key
static const char* const kTable[OSK_PAGES][OSK_ROWS] = {
	{MSG_OSK_HIRA_1, MSG_OSK_HIRA_2, MSG_OSK_HIRA_3, MSG_OSK_HIRA_4, MSG_OSK_HIRA_5},
	{MSG_OSK_KATA_1, MSG_OSK_KATA_2, MSG_OSK_KATA_3, MSG_OSK_KATA_4, MSG_OSK_KATA_5},
	{MSG_OSK_WIDE_1, MSG_OSK_WIDE_2, MSG_OSK_WIDE_3, MSG_OSK_WIDE_4, MSG_OSK_WIDE_5},
	{"ABCDEFGHIJKLM0123", "NOPQRSTUVWXYZ4567", "abcdefghijklm89+-", "nopqrstuvwxyz.,!?", "@#$%&*()_/:;='\"<>"},
};

// the keys of the row below, in half keys: where each starts; the last entry is the row's end
static const uint8_t kKeyAt[OSK_KEYS + 1] = {0, 4, 8, 12, 16, 22, 24, 26, 30, 34};

// ---- the keys ---------------------------------------------------------------------------------

// the key's character in Shift-JIS into s[3]
static void TableChar(int page, int row, int col, char s[3])
{
	const char* t = kTable[page][row];
	if(page == OSK_PAGE_HALF)
	{
		s[0] = t[col];
		s[1] = 0;
	}
	else
	{
		s[0] = t[col * 2];
		s[1] = t[col * 2 + 1];
	}
	s[2] = 0;
}

// a key of the row below: what it says, Shift-JIS
static const char* KeyLabel(int key)
{
	switch(key)
	{
		case OSK_KEY_HIRA: return MSG_OSK_KEY_HIRA;
		case OSK_KEY_KATA: return MSG_OSK_KEY_KATA;
		case OSK_KEY_WIDE: return MSG_OSK_KEY_WIDE;
		case OSK_KEY_HALF: return MSG_OSK_KEY_HALF;
		case OSK_KEY_SPACE: return MSG_OSK_KEY_SPACE;
		case OSK_KEY_LEFT: return MSG_OSK_KEY_LEFT;
		case OSK_KEY_RIGHT: return MSG_OSK_KEY_RIGHT;
		case OSK_KEY_DELETE: return MSG_OSK_KEY_DELETE;
		default: return MSG_OSK_KEY_DONE;
	}
}

// where key (row, col) lies in the keyboard's picture
static void KeyRect(const Osk_t* k, int row, int col, int* x, int* y, int* w, int* h)
{
	*y = EDGE + row * k->keyH;
	*h = k->keyH;
	if(row < OSK_ROWS)
	{
		*x = EDGE + col * k->keyW;
		*w = k->keyW;
	}
	else
	{
		*x = EDGE + kKeyAt[col] * k->keyW / 2;
		*w = EDGE + kKeyAt[col + 1] * k->keyW / 2 - *x;
	}
}

// the key at (x, y) of the keyboard's picture as row * OSK_COLS + col; HELD_NONE between the keys
static int KeyAt(const Osk_t* k, int x, int y)
{
	int row, col;
	if(x < EDGE || y < EDGE || x >= EDGE + OSK_COLS * k->keyW || y >= EDGE + (OSK_ROWS + 1) * k->keyH)
		return HELD_NONE;
	row = (y - EDGE) / k->keyH;
	if(row < OSK_ROWS)
		return row * OSK_COLS + (x - EDGE) / k->keyW;
	for(col = 0; col < OSK_KEYS - 1; col++)
		if(x < EDGE + kKeyAt[col + 1] * k->keyW / 2)
			break;
	return OSK_ROWS * OSK_COLS + col;
}

// ---- its picture ------------------------------------------------------------------------------

// one key: its face, and what it says in the middle
static void PaintKey(OpenBGI_t* o, Osk_t* k, int row, int col)
{
	char sjis[16], text[48];
	uint32_t face = UI_FACE, ink = UI_TEXT;
	int x, y, w, h, tw;
	KeyRect(k, row, col, &x, &y, &w, &h);
	if(row < OSK_ROWS)
		TableChar(k->page, row, col, sjis);
	else
	{
		strncpy(sjis, KeyLabel(col), sizeof sjis - 1);
		sjis[sizeof sjis - 1] = 0;
		if(col == k->page)
		{ // (OSK_KEY_HIRA .. OSK_KEY_HALF are the tables' numbers)
			face = KEY_ON;
			ink = UI_LIGHT;
		}
	}
	if(k->press == row * OSK_COLS + col)
		face = UI_SHADOW;
	else if(k->padShown && k->row == row && k->col == col)
	{
		face = UI_SELECT_BG;
		ink = UI_SELECT_TEXT;
	}
	Ui_FillRect(k->pix, k->w, k->w, k->h, x + 1, y + 1, w - 2, h - 2, face);
	if(!k->face)
		return;
	Ui_FromSjis(sjis, text, sizeof text, k->utf8);
	tw = Typeface_SmoothWidth(k->face, text, -1);
	Typeface_SmoothText(k->face, k->pix, k->w, k->w, k->h, x + (w - tw) / 2, y + (h - k->faceH) / 2, text, ink, 0);
}

static void Paint(OpenBGI_t* o, Osk_t* k)
{
	int row, col;
	Ui_FillRect(k->pix, k->w, k->w, k->h, 0, 0, k->w, k->h, BOARD);
	for(row = 0; row < OSK_ROWS; row++)
		for(col = 0; col < OSK_COLS; col++)
			PaintKey(o, k, row, col);
	for(col = 0; col < OSK_KEYS; col++)
		PaintKey(o, k, OSK_ROWS, col);
	k->dirty = 0;
	Screen_Overlay(o, SCREEN_OVERLAY_OSK, k->pix, k->w, k->x, k->y, k->w, k->h, 0);
}

static void Drop(OpenBGI_t* o, Osk_t* k)
{
	if(o->screen.overlay[SCREEN_OVERLAY_OSK].pix)
		Screen_Overlay(o, SCREEN_OVERLAY_OSK, NULL, 0, 0, 0, 0, 0, 0);
	BGI_Free(k->pix);
	k->pix = NULL;
	Typeface_Close(k->face);
	k->face = NULL;
}

/* lay it out for the picture as it is: the keys as large as 17 of them
 * fit across and six rows in half the height, along the bottom unless the
 * field lies there; 0 when it cannot be had */
static int Place(OpenBGI_t* o, Osk_t* k)
{
	int w = o->screen.pic.w, h = o->screen.pic.h;
	Drop(o, k);
	if(!o->screen.pic.pixels || w <= 0 || h <= 0)
		return 0;
	k->keyW = (w - 2 * EDGE) / OSK_COLS;
	if(k->keyW < KEY_MIN)
		k->keyW = KEY_MIN;
	if(k->keyW > KEY_MAX)
		k->keyW = KEY_MAX;
	k->keyH = (h / 2 - 2 * EDGE) / (OSK_ROWS + 1);
	if(k->keyH > k->keyW)
		k->keyH = k->keyW; // (no taller than wide)
	if(k->keyH < KEY_MIN)
		k->keyH = KEY_MIN;
	k->w = OSK_COLS * k->keyW + 2 * EDGE;
	k->h = (OSK_ROWS + 1) * k->keyH + 2 * EDGE;
	k->x = (w - k->w) / 2;
	k->y = h - k->h;
	if(k->rect.y + k->rect.h > k->y && k->rect.y >= k->h)
		k->y = 0; // the field is at the bottom, and clear of the top
	k->picW = w;
	k->picH = h;
	k->fieldY = k->rect.y;
	k->fieldH = k->rect.h;
	k->utf8 = o->enc.textUtf8;
	k->faceH = k->keyH * 6 / 10 < 8 ? 8 : k->keyH * 6 / 10;
	k->pix = (uint32_t*)BGI_Calloc((size_t)k->w * (size_t)k->h * sizeof *k->pix);
	if(!k->pix)
		return 0;
	k->face = Inframe_OpenFontSized(o, k->faceH);
	k->press = -1;
	k->held = 0;
	k->dirty = 1;
	return 1;
}

// ---- up and away ------------------------------------------------------------------------------

int Osk_Up(OpenBGI_t* o)
{
	return o->input.osk.up;
}

void Osk_Open(OpenBGI_t* o)
{
	Osk_t* k = &o->input.osk;
	k->want = 0;
	if(k->up || !k->field)
		return;
	if(!Place(o, k))
		return;
	k->up = 1;
	k->padShown = o->input.device == INPUT_DEV_PAD;
	if(k->row < 0 || k->row > OSK_ROWS || k->col < 0 || k->col >= (k->row < OSK_ROWS ? OSK_COLS : OSK_KEYS))
		k->row = k->col = 0;
}

void Osk_Close(OpenBGI_t* o)
{
	Osk_t* k = &o->input.osk;
	k->want = 0;
	if(!k->up)
		return;
	k->up = 0;
	Drop(o, k);
}

void Osk_Free(OpenBGI_t* o)
{
	Osk_t* k = &o->input.osk;
	Osk_Close(o);
	k->field = k->fieldWas = k->fieldEntry = 0;
}

int Osk_Over(OpenBGI_t* o, int x, int y)
{
	const Osk_t* k = &o->input.osk;
	return k->up && x >= k->x && y >= k->y && x < k->x + k->w && y < k->y + k->h;
}

void Osk_Shown(OpenBGI_t* o, OpenBGI_Frame_t* f)
{
	Osk_t* k = &o->input.osk;
	k->field = f->textInput != 0;
	k->fieldEntry = k->field && !Inframe_Up(o);
	if(k->field)
		k->rect = f->textRect;
	if(!k->up)
		return;
	f->textInput = 0; // the keys are typed here: the host's input method is not asked for
	if(!k->field)
		return; // (it goes before the next pass)
	if(k->picW != o->screen.pic.w || k->picH != o->screen.pic.h || k->utf8 != o->enc.textUtf8 || k->fieldY != k->rect.y ||
		k->fieldH != k->rect.h)
	{ // the picture or the field changed under it
		if(!Place(o, k))
		{
			Osk_Close(o);
			return;
		}
	}
	if(k->padShown != (o->input.device == INPUT_DEV_PAD))
	{ // the key the directions are on shows while a pad is in use
		k->padShown = !k->padShown;
		k->dirty = 1;
	}
	if(k->dirty)
		Paint(o, k);
}

void Osk_Frame(OpenBGI_t* o)
{
	Osk_t* k = &o->input.osk;
	const TouchState_t* t = &o->input.touch;
	int touchOnly = o->input.device == INPUT_DEV_TOUCH && !(o->host.caps & OPENBGI_CAP_KEYBOARD) && !o->input.oskAsked;
	int byPad = o->input.device == INPUT_DEV_PAD && !o->input.oskAsked;
	if(k->up && (!k->field || !o->screen.pic.pixels))
		Osk_Close(o);
	if(!k->up && k->field)
	{
		if(k->want)
			Osk_Open(o); // it was asked for before the field had the keys
		else if(!k->fieldWas && k->fieldEntry && (byPad || touchOnly))
			Osk_Open(o); // the scripts' entry got the keys
	}
	if(k->tapSerial != t->tapSerial && !Steps_Pending(o))
	{ // a tap, and its click is through: one in a field asks for the keyboard
		k->tapSerial = t->tapSerial;
		if(!k->up && k->field && touchOnly && t->tapToX >= k->rect.x && t->tapToX < k->rect.x + k->rect.w && t->tapToY >= k->rect.y &&
			t->tapToY < k->rect.y + k->rect.h)
			Osk_Open(o);
	}
	if(k->want && !k->field && !Steps_Pending(o))
		k->want = 0; // the click that was to give the field the keys did not
	k->fieldWas = k->field;
}

// ---- typing -----------------------------------------------------------------------------------

// key (row, col) is typed
static void Type(OpenBGI_t* o, Osk_t* k, int row, int col)
{
	char sjis[3], utf8[8];
	if(row < OSK_ROWS)
	{
		TableChar(k->page, row, col, sjis);
		if(Ui_FromSjis(sjis, utf8, sizeof utf8, 1) > 0)
			Steps_TextNow(o, utf8);
		return;
	}
	switch(col)
	{
		case OSK_KEY_HIRA:
		case OSK_KEY_KATA:
		case OSK_KEY_WIDE:
		case OSK_KEY_HALF:
			k->page = col;
			k->dirty = 1;
			break;
		case OSK_KEY_SPACE: Steps_TextNow(o, k->page == OSK_PAGE_HALF ? " " : "\xe3\x80\x80"); break;
		case OSK_KEY_LEFT: Steps_KeyTap(o, 0x25); break;
		case OSK_KEY_RIGHT: Steps_KeyTap(o, 0x27); break;
		case OSK_KEY_DELETE: Steps_KeyTap(o, 0x08); break;
		case OSK_KEY_DONE:
		{
			int entry = k->fieldEntry;
			Osk_Close(o);
			if(entry)
				Steps_KeyTap(o, 0x0d); // the scripts' entry is given up with Enter
			break;
		}
		default: break;
	}
}

// a direction: to the next key that way, around the edges
static void MoveKey(Osk_t* k, int action)
{
	int n = k->row < OSK_ROWS ? OSK_COLS : OSK_KEYS, was = k->row;
	if(action == OPENBGI_ACTION_LEFT)
		k->col = (k->col + n - 1) % n;
	else if(action == OPENBGI_ACTION_RIGHT)
		k->col = (k->col + 1) % n;
	else
	{
		k->row = (k->row + (action == OPENBGI_ACTION_UP ? OSK_ROWS : 1)) % (OSK_ROWS + 1);
		if(was == OSK_ROWS && k->row != OSK_ROWS)
			k->col = (kKeyAt[k->col] + kKeyAt[k->col + 1]) / 4; // the table key over the middle of the key left
		else if(was != OSK_ROWS && k->row == OSK_ROWS)
		{ // the key under the table key left
			int half = k->col * 2 + 1, i;
			for(i = 0; i < OSK_KEYS - 1 && half >= kKeyAt[i + 1]; i++)
			{
			}
			k->col = i;
		}
	}
	if(k->row < OSK_ROWS && k->col >= OSK_COLS)
		k->col = OSK_COLS - 1;
	k->dirty = 1;
}

void Osk_Action(OpenBGI_t* o, int action, int down)
{
	Osk_t* k = &o->input.osk;
	if(!k->up || !down)
		return;
	switch(action)
	{
		case OPENBGI_ACTION_UP:
		case OPENBGI_ACTION_DOWN:
		case OPENBGI_ACTION_LEFT:
		case OPENBGI_ACTION_RIGHT: MoveKey(k, action); break;
		case OPENBGI_ACTION_CONFIRM: Type(o, k, k->row, k->col); break;
		case OPENBGI_ACTION_CANCEL: Steps_KeyTap(o, 0x08); break;
		case OPENBGI_ACTION_BACKLOG: Steps_KeyTap(o, 0x25); break;
		case OPENBGI_ACTION_FORWARD: Steps_KeyTap(o, 0x27); break;
		case OPENBGI_ACTION_SKIP:
			k->page = (k->page + 1) % OSK_PAGES;
			k->dirty = 1;
			break;
		case OPENBGI_ACTION_KEYBOARD: Osk_Close(o); break;
		default: break;
	}
}

int Osk_Button(OpenBGI_t* o, int button, int down, int x, int y)
{
	Osk_t* k = &o->input.osk;
	int key;
	if(button < 0 || button > 4)
		return 0;
	if(!down)
	{ // the release of a press it took is its too, wherever it is
		if(!(k->held & (1 << button)))
			return 0;
		k->held &= ~(1 << button);
		if(button != OPENBGI_BUTTON_LEFT || !k->up)
			return 1;
		key = Osk_Over(o, x, y) ? KeyAt(k, x - k->x, y - k->y) : HELD_NONE;
		if(key == k->press && key != HELD_NONE)
			Type(o, k, key / OSK_COLS, key % OSK_COLS);
		k->press = -1;
		k->dirty = 1;
		return 1;
	}
	if(!Osk_Over(o, x, y))
		return 0;
	k->held |= 1 << button;
	if(button == OPENBGI_BUTTON_LEFT)
	{
		k->press = KeyAt(k, x - k->x, y - k->y);
		k->dirty = 1;
	}
	return 1;
}
