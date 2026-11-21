/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * uikit.c - a dialog of the dialog toolkit (src/include/bgi/ui.h): its controls,
 *           painting it into its buffer, and what the keys, typed text and
 *           the pointer do to it
 *
 * Whoever shows the dialog - a window of the X11 host, the engine's frame
 * - hands its events over one at a time and paints the dialog again
 * (Ui_Draw) when an event set `redraw`; nothing here waits or draws on its
 * own.
 */
#include "bgi/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the controls ---------------------------------------------------------------------

// a label at (x, y); its index, -1 when the dialog is full
int Ui_LabelAdd(UiDialog_t* d, int x, int y, const char* text)
{
	UiLabel_t* l;
	if(d->nLabels >= UI_MAX_LABELS)
		return -1;
	l = &d->labels[d->nLabels++];
	l->x = x;
	l->y = y;
	snprintf(l->text, sizeof l->text, "%s", text ? text : "");
	return d->nLabels - 1;
}

// a text field of width w at (x, y) with an initial text (NULL = empty) and a byte limit (0 = unlimited); its index
int Ui_FieldAdd(UiDialog_t* d, int x, int y, int w, const char* initial, int limit)
{
	UiFieldW_t* f;
	if(d->nFields >= UI_MAX_FIELDS)
		return -1;
	f = &d->fields[d->nFields++];
	memset(f, 0, sizeof *f);
	f->x = x;
	f->y = y;
	f->w = w;
	f->h = UI_FIELD_H;
	f->f.limit = limit > 0 && limit < (int)sizeof f->f.text ? limit : 0;
	f->f.utf8 = d->utf8;
	UiField_SetText(&f->f, initial);
	return d->nFields - 1;
}

// a push button of the standard size at (x, y) that ends the dialog with `id`; its index
int Ui_ButtonAdd(UiDialog_t* d, int x, int y, const char* text, int id)
{
	UiButton_t* b;
	if(d->nButtons >= UI_MAX_BUTTONS)
		return -1;
	b = &d->buttons[d->nButtons++];
	b->x = x;
	b->y = y;
	b->w = UI_BTN_W;
	b->h = UI_BTN_H;
	b->text = text;
	b->id = id;
	return d->nButtons - 1;
}

// a list box of `rows` rows at (x, y), w wide; the index, -1 when the dialog is full
int Ui_ListAdd(UiDialog_t* d, int x, int y, int w, int rows, const char* const* items, int count, int selected)
{
	UiList_t* l;
	if(d->nLists >= UI_MAX_LISTS)
		return -1;
	l = &d->lists[d->nLists++];
	l->x = x;
	l->y = y;
	l->w = w;
	l->h = rows * UI_LIST_ROW_H + 4;
	l->items = items;
	l->count = count;
	l->selected = selected < count ? selected : -1;
	l->top = 0;
	if(l->selected >= rows)
		l->top = l->selected - rows + 1;
	return d->nLists - 1;
}

void Ui_LabelSet(UiDialog_t* d, int label, const char* text)
{
	if(label >= 0 && label < d->nLabels)
		snprintf(d->labels[label].text, sizeof d->labels[label].text, "%s", text ? text : "");
}

int Ui_TextWidth(const UiDialog_t* d, const char* text)
{
	return Ui_FontWidth(d->font, text);
}

int Ui_FocusedField(const UiDialog_t* d)
{
	return d->focus >= 0 && d->focus < d->nFields ? d->focus : -1;
}

int Ui_Alloc(UiDialog_t* d)
{
	d->pressed = -1;
	d->pix = d->w > 0 && d->h > 0 ? (uint32_t*)calloc((size_t)d->w * (size_t)d->h, 4) : NULL;
	return d->pix != NULL;
}

void Ui_Free(UiDialog_t* d)
{
	free(d->pix);
	d->pix = NULL;
}

// ---- painting -------------------------------------------------------------------------

// the rows a list shows at once
static int ListRows(const UiList_t* l)
{
	return (l->h - 4) / UI_LIST_ROW_H;
}

// keep the selection within the rows shown
static void ListScrollToSelection(UiList_t* l)
{
	int rows = ListRows(l);
	if(l->selected < 0)
		return;
	if(l->selected < l->top)
		l->top = l->selected;
	if(l->selected >= l->top + rows)
		l->top = l->selected - rows + 1;
}

static void Text(UiDialog_t* d, int x, int y, const char* text, uint32_t rgb)
{
	if(d->font && d->font->draw)
		d->font->draw(d->font->ctx, d->pix, d->w, d->w, d->h, x, y, text, rgb);
}

static void Fill(UiDialog_t* d, int x, int y, int w, int h, uint32_t rgb)
{
	Ui_FillRect(d->pix, d->w, d->w, d->h, x, y, w, h, rgb);
}

// the sunken white box of a field or a list
static void SunkenBox(UiDialog_t* d, int x, int y, int w, int h)
{
	Fill(d, x, y, w, h, UI_FIELD_BG);
	Fill(d, x, y, w, 1, UI_SHADOW);
	Fill(d, x, y, 1, h, UI_SHADOW);
	Fill(d, x + 1, y + 1, w - 2, 1, UI_DARK);
	Fill(d, x + 1, y + 1, 1, h - 2, UI_DARK);
	Fill(d, x, y + h - 1, w, 1, UI_LIGHT);
	Fill(d, x + w - 1, y, 1, h, UI_LIGHT);
}

// the rows of a list, the selected one highlighted; a thin scroll indicator at the right when it does not all fit
static void ListDraw(UiDialog_t* d, int index)
{
	UiList_t* l = &d->lists[index];
	int rows = ListRows(l), i, focused = d->focus == d->nFields + d->nButtons + index;
	SunkenBox(d, l->x, l->y, l->w, l->h);
	for(i = 0; i < rows && l->top + i < l->count; i++)
	{
		int row = l->top + i, y = l->y + 2 + i * UI_LIST_ROW_H;
		int sel = row == l->selected;
		if(sel)
			Fill(d, l->x + 2, y, l->w - 4, UI_LIST_ROW_H, focused ? UI_SELECT_BG : UI_SHADOW);
		Text(d, l->x + 6, y + 2, l->items[row], sel ? UI_SELECT_TEXT : UI_TEXT);
	}
	if(l->count > rows)
	{ // the part of the list in view, as a bar along the right edge
		int trackH = l->h - 4, barH = trackH * rows / l->count, barY = trackH * l->top / l->count;
		if(barH < 8)
			barH = 8;
		Fill(d, l->x + l->w - 6, l->y + 2, 4, trackH, UI_FACE);
		Fill(d, l->x + l->w - 6, l->y + 2 + barY, 4, barH, UI_DARK);
	}
}

// a push button: raised (sunken while pressed), outlined when it is the default or has the focus
static void ButtonDraw(UiDialog_t* d, int i)
{
	UiButton_t* b = &d->buttons[i];
	int down = d->pressed == i, tw;
	int focused = d->focus == d->nFields + i;
	Fill(d, b->x, b->y, b->w, b->h, UI_FACE);
	Fill(d, b->x, b->y, b->w, 1, down ? UI_DARK : UI_LIGHT);
	Fill(d, b->x, b->y, 1, b->h, down ? UI_DARK : UI_LIGHT);
	Fill(d, b->x, b->y + b->h - 1, b->w, 1, down ? UI_LIGHT : UI_DARK);
	Fill(d, b->x + b->w - 1, b->y, 1, b->h, down ? UI_LIGHT : UI_DARK);
	Fill(d, b->x + 1, b->y + b->h - 2, b->w - 2, 1, down ? UI_FACE : UI_SHADOW);
	Fill(d, b->x + b->w - 2, b->y + 1, 1, b->h - 2, down ? UI_FACE : UI_SHADOW);
	if(i == d->defButton || focused)
	{
		Fill(d, b->x - 1, b->y - 1, b->w + 2, 1, UI_FRAME);
		Fill(d, b->x - 1, b->y + b->h, b->w + 2, 1, UI_FRAME);
		Fill(d, b->x - 1, b->y - 1, 1, b->h + 2, UI_FRAME);
		Fill(d, b->x + b->w, b->y - 1, 1, b->h + 2, UI_FRAME);
	}
	tw = Ui_TextWidth(d, b->text);
	Text(d, b->x + (b->w - tw) / 2 + down, b->y + (b->h - UI_FONT_H) / 2 + down, b->text, UI_TEXT); // the caption moves one pixel while pressed
}

void Ui_Draw(UiDialog_t* d)
{
	int i;
	if(!d->pix)
		return;
	Fill(d, 0, 0, d->w, d->h, UI_FACE);
	if(d->icon)
	{ // a disc with a letter: the message box icons (32 x 32 at the top-left margin)
		int cx = UI_MARGIN + 16, cy = UI_MARGIN + 16, x, y;
		char letter[2];
		letter[0] = (char)d->icon;
		letter[1] = 0;
		for(y = -16; y < 16; y++)
			for(x = -16; x < 16; x++)
				if(x * x + y * y <= 15 * 15 && cy + y < d->h && cx + x < d->w)
					d->pix[(size_t)(cy + y) * d->w + cx + x] = d->iconRgb;
		Text(d, cx - Ui_TextWidth(d, letter) / 2, cy - UI_FONT_H / 2, letter, 0xffffff);
	}
	for(i = 0; i < d->nLabels; i++)
		Text(d, d->labels[i].x, d->labels[i].y, d->labels[i].text, UI_TEXT);
	for(i = 0; i < d->nFields; i++)
	{
		UiFieldW_t* f = &d->fields[i];
		UiField_Draw(&f->f, d->pix, d->w, d->w, d->h, f->x, f->y, f->w, f->h, d->font, UI_TEXT, 0, d->focus == i, 1);
	}
	for(i = 0; i < d->nLists; i++)
		ListDraw(d, i);
	for(i = 0; i < d->nButtons; i++)
		ButtonDraw(d, i);
}

// ---- what the events do ---------------------------------------------------------------

static int Inside(int x, int y, int rx, int ry, int rw, int rh)
{
	return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// the button / field / list under (x, y), -1 for none
static int HitButton(const UiDialog_t* d, int x, int y)
{
	int i;
	for(i = 0; i < d->nButtons; i++)
		if(Inside(x, y, d->buttons[i].x, d->buttons[i].y, d->buttons[i].w, d->buttons[i].h))
			return i;
	return -1;
}

static int HitField(const UiDialog_t* d, int x, int y)
{
	int i;
	for(i = 0; i < d->nFields; i++)
		if(Inside(x, y, d->fields[i].x, d->fields[i].y, d->fields[i].w, d->fields[i].h))
			return i;
	return -1;
}

static int HitList(const UiDialog_t* d, int x, int y)
{
	int i;
	for(i = 0; i < d->nLists; i++)
		if(Inside(x, y, d->lists[i].x, d->lists[i].y, d->lists[i].w, d->lists[i].h))
			return i;
	return -1;
}

/* a button was pressed: end the dialog with its id unless the onButton
 * handler refuses (a handler that changed the dialog's texts sets `redraw`) */
static void Activate(UiDialog_t* d, int button)
{
	if(button < 0 || button >= d->nButtons)
		return;
	if(d->onButton && !d->onButton(d, d->buttons[button].id, d->ctx))
	{
		d->redraw = 1;
		return;
	}
	d->result = d->buttons[button].id;
	d->done = 1;
}

// select a row of a list (clamped; -1 clears) and tell the owner
static void ListSelect(UiDialog_t* d, int index, int row)
{
	UiList_t* l = &d->lists[index];
	if(l->count == 0)
		row = -1;
	else if(row >= l->count)
		row = l->count - 1;
	if(row == l->selected)
		return;
	l->selected = row;
	ListScrollToSelection(l);
	if(d->onListSelect)
		d->onListSelect(d, index, d->ctx);
	d->redraw = 1;
}

// a numeric picker only holds digits inside its range: anything else typed is dropped and the value clamped
static void ClampNumeric(UiFieldW_t* f)
{
	int v, i, o = 0;
	char digits[16];
	if(!f->numeric)
		return;
	for(i = 0; f->f.text[i] && o < 15; i++)
		if(f->f.text[i] >= '0' && f->f.text[i] <= '9')
			digits[o++] = f->f.text[i];
	digits[o] = 0;
	if(o == 0)
		return; // empty while typing
	v = atoi(digits);
	if(v < f->min)
		v = f->min;
	if(v > f->max)
		v = f->max;
	snprintf(f->f.text, sizeof f->f.text, "%d", v);
	f->f.caret = (int)strlen(f->f.text);
}

/* whether an edit of a digits-only field stays within what typing may add:
 * no new character but digits, and a '-' only into a field that was empty */
static int DigitsKept(const char* before, const char* after)
{
	int i, otherB = 0, otherA = 0, minusB = 0, minusA = 0;
	for(i = 0; before[i]; i++)
	{
		if(before[i] == '-')
			minusB++;
		else if(before[i] < '0' || before[i] > '9')
			otherB++;
	}
	for(i = 0; after[i]; i++)
	{
		if(after[i] == '-')
			minusA++;
		else if(after[i] < '0' || after[i] > '9')
			otherA++;
	}
	return otherA <= otherB && (minusA <= minusB || before[0] == 0);
}

/* after an edit of field `i` (r: 1 changed, 0 not, -1 not the field's)
 * that started from `before`: a digits-only field drops what it may not
 * hold, a number picker keeps its range, the owner hears of a change */
static int Edited(UiDialog_t* d, int i, const UiField_t* before, int r)
{
	UiFieldW_t* f = &d->fields[i];
	if(r > 0 && f->digits && !DigitsKept(before->text, f->f.text))
	{
		f->f = *before; // the character is swallowed
		r = 0;
	}
	if(r > 0)
	{
		ClampNumeric(f);
		if(d->onChange)
			d->onChange(d, i, d->ctx);
	}
	if(r >= 0)
		d->redraw = 1;
	return r;
}

// up / down over a number or choice picker: 1 when it stepped
static int StepPicker(UiDialog_t* d, UiFieldW_t* f, int vk)
{
	if(f->numeric)
	{ // wrapping at the ends of its range
		int v = atoi(f->f.text) + (vk == 0x26 ? 1 : -1);
		if(v < f->min)
			v = f->max;
		if(v > f->max)
			v = f->min;
		snprintf(f->f.text, sizeof f->f.text, "%d", v);
		f->f.caret = (int)strlen(f->f.text);
		f->f.selAll = 0;
	}
	else if(f->nChoices > 0)
	{ // through its texts, from the one the field holds (else the first)
		int cur = -1, k;
		for(k = 0; k < f->nChoices; k++)
			if(strcmp(f->choices[k], f->f.text) == 0)
				cur = k;
		if(cur < 0)
			cur = vk == 0x26 ? f->nChoices : -1;
		cur = vk == 0x26 ? (cur + f->nChoices - 1) % f->nChoices : (cur + 1) % f->nChoices;
		UiField_SetText(&f->f, f->choices[cur]);
		if(f->pick)
			UiField_SelectAll(&f->f); // (shown as chosen, without a caret)
	}
	else
		return 0;
	if(d->onChange)
		d->onChange(d, d->focus, d->ctx);
	d->redraw = 1;
	return 1;
}

int Ui_Key(UiDialog_t* d, int vk, int shift, int ctrl, int alt)
{
	int n = d->nFields + d->nButtons + d->nLists;
	if(d->done)
		return UI_KEY_NONE;
	if(vk == 0x1b)
	{ // Escape: the cancel button
		if(d->escButton >= 0)
			Activate(d, d->escButton);
		return UI_KEY_DONE;
	}
	if(vk == 0x0d)
	{ // Enter: the focused button, else the default one
		if(d->focus >= d->nFields && d->focus < d->nFields + d->nButtons)
			Activate(d, d->focus - d->nFields);
		else if(d->defButton >= 0)
			Activate(d, d->defButton);
		return UI_KEY_DONE;
	}
	if(vk == 0x09)
	{ // Tab / Shift+Tab: the next / previous control; a field gets its text selected, as Windows does
		if(n > 0)
		{
			d->focus = shift ? (d->focus + n - 1) % n : (d->focus + 1) % n;
			if(d->focus < d->nFields)
				UiField_SelectAll(&d->fields[d->focus].f);
		}
		d->redraw = 1;
		return UI_KEY_DONE;
	}
	if(d->focus >= d->nFields + d->nButtons)
	{ // a list has the focus: the arrows, page keys, Home and End move the selection
		int index = d->focus - d->nFields - d->nButtons;
		UiList_t* l;
		int rows, row;
		if(index >= d->nLists)
			return UI_KEY_NONE;
		l = &d->lists[index];
		rows = ListRows(l);
		row = l->selected;
		if(vk == 0x26)
			row = row > 0 ? row - 1 : 0;
		else if(vk == 0x28)
			row = row + 1;
		else if(vk == 0x21)
			row = row > rows ? row - rows : 0;
		else if(vk == 0x22)
			row = row + rows;
		else if(vk == 0x24)
			row = 0;
		else if(vk == 0x23)
			row = l->count - 1;
		else
			return UI_KEY_NONE;
		ListSelect(d, index, row);
		d->redraw = 1;
		return UI_KEY_DONE;
	}
	if(d->focus >= d->nFields)
	{ // a button has the focus: Space presses it, the arrows move between the buttons
		int b = d->focus - d->nFields;
		if(vk == 0x20)
			Activate(d, b);
		else if(vk == 0x25 || vk == 0x27)
		{
			b = vk == 0x25 ? (b + d->nButtons - 1) % d->nButtons : (b + 1) % d->nButtons;
			d->focus = d->nFields + b;
			d->redraw = 1;
		}
		else
			return UI_KEY_NONE;
		return UI_KEY_DONE;
	}
	{ // a field has the focus
		UiFieldW_t* f = &d->fields[d->focus];
		UiField_t before;
		int r;
		if((vk == 0x26 || vk == 0x28) && StepPicker(d, f, vk))
			return UI_KEY_DONE;
		if(f->pick)
		{ // left and right step as up and down do, Space goes on to the next; nothing is typed
			if(vk == 0x25 || vk == 0x27 || vk == 0x20)
				StepPicker(d, f, vk == 0x25 ? 0x26 : 0x28);
			return UI_KEY_DONE;
		}
		before = f->f;
		r = UiField_Key(&f->f, vk, ctrl, alt);
		if(r == UI_FIELD_TYPES)
			return UI_KEY_TYPE;
		return Edited(d, d->focus, &before, r) >= 0 ? UI_KEY_DONE : UI_KEY_NONE;
	}
}

int Ui_Type(UiDialog_t* d, int vk, const char* utf8)
{
	int i = Ui_FocusedField(d), r;
	UiField_t before;
	if(i < 0 || d->done || (!vk && !utf8[0]))
		return UI_KEY_NONE;
	if(d->fields[i].pick)
		return UI_KEY_DONE;
	before = d->fields[i].f;
	r = UiField_Type(&d->fields[i].f, utf8);
	return Edited(d, i, &before, r) >= 0 ? UI_KEY_DONE : UI_KEY_NONE;
}

/* a left click: press a button (sunken until released), focus a field
 * (caret at the end) or pick a row; a second click on the selected row
 * within 400 ms activates the default button */
void Ui_Press(UiDialog_t* d, int x, int y, uint32_t ms)
{
	int b = HitButton(d, x, y), f = HitField(d, x, y), li = HitList(d, x, y);
	if(d->done)
		return;
	if(b >= 0)
	{
		d->pressed = b;
		d->focus = d->nFields + b;
	}
	else if(f >= 0 && d->fields[f].pick)
	{ // a picker that only steps: on to its next text
		d->focus = f;
		StepPicker(d, &d->fields[f], 0x28);
	}
	else if(f >= 0)
	{
		d->focus = f;
		d->fields[f].f.selAll = 0;
		d->fields[f].f.caret = (int)strlen(d->fields[f].f.text);
	}
	else if(li >= 0)
	{
		UiList_t* l = &d->lists[li];
		int row = l->top + (y - l->y - 2) / UI_LIST_ROW_H;
		int same = row == l->selected && row < l->count;
		d->focus = d->nFields + d->nButtons + li;
		if(row < l->count)
			ListSelect(d, li, row);
		if(same && d->lastClick && ms - d->lastClick < 400 && d->defButton >= 0)
		{
			d->lastClick = 0;
			d->redraw = 1;
			Activate(d, d->defButton);
			return;
		}
		d->lastClick = ms ? ms : 1;
	}
	d->redraw = 1;
}

// the button fires when released over the one that was pressed
void Ui_Release(UiDialog_t* d, int x, int y)
{
	int was = d->pressed;
	if(was < 0 || d->done)
		return;
	d->pressed = -1;
	d->redraw = 1;
	if(HitButton(d, x, y) == was)
		Activate(d, was);
}

void Ui_Wheel(UiDialog_t* d, int x, int y, int notches)
{
	int li = HitList(d, x, y), f = HitField(d, x, y);
	if(f >= 0 && !d->done && d->fields[f].pick)
	{ // over a picker that only steps: a text a notch
		d->focus = f;
		for(; notches > 0; notches--)
			StepPicker(d, &d->fields[f], 0x26);
		for(; notches < 0; notches++)
			StepPicker(d, &d->fields[f], 0x28);
		return;
	}
	if(li >= 0 && !d->done)
	{
		UiList_t* l = &d->lists[li];
		int rows = ListRows(l), maxTop = l->count > rows ? l->count - rows : 0;
		l->top -= 3 * notches;
		if(l->top > maxTop)
			l->top = maxTop;
		if(l->top < 0)
			l->top = 0;
		d->redraw = 1;
	}
}

void Ui_Close(UiDialog_t* d)
{
	if(d->escButton >= 0 && !d->done)
		Activate(d, d->escButton);
}
