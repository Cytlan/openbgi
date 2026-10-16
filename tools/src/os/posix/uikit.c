/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * uikit.c - the dialog toolkit of the POSIX back end (ui_internal.h)
 *
 * Without a toolkit the back end draws its dialogs itself: a dialog is a
 * top-level window painted from a 32-bit buffer with labels, text fields
 * and push buttons in the classic grey Windows look, run as a modal loop
 * (X11_RunModal) that keeps repainting the engine's windows but discards
 * their input.  The keyboard works as in a Windows dialog: Tab cycles the
 * fields and buttons, Enter activates the focused or the default button,
 * Escape the cancel button, Space a focused button, the arrows move
 * between buttons or step a number picker.
 */
#include "ui_internal.h"

// a label at (x, y); its index, -1 when the dialog is full
int Ui_LabelAdd(UiDialog_t* d, int x, int y, const char* sjis)
{
	UiLabel_t* l;
	if(d->nLabels >= UI_MAX_LABELS)
		return -1;
	l = &d->labels[d->nLabels++];
	l->x = x;
	l->y = y;
	snprintf(l->text, sizeof l->text, "%s", sjis);
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
	X11Field_SetText(&f->f, initial ? initial : "");
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

void Ui_LabelSet(UiDialog_t* d, int label, const char* sjis)
{
	if(label >= 0 && label < d->nLabels)
		snprintf(d->labels[label].text, sizeof d->labels[label].text, "%s", sjis);
}

// the rows a list shows at once
static int UiListRows(const UiList_t* l)
{
	return (l->h - 4) / UI_LIST_ROW_H;
}

// keep the selection within the rows shown
static void UiListScrollToSelection(UiList_t* l)
{
	int rows = UiListRows(l);
	if(l->selected < 0)
		return;
	if(l->selected < l->top)
		l->top = l->selected;
	if(l->selected >= l->top + rows)
		l->top = l->selected - rows + 1;
}

// the sunken white box of a field or a list
static void UiSunkenBox(UiDialog_t* d, int x, int y, int w, int h)
{
	uint32_t* p = d->pix;
	Ui_FillRect(p, d->w, d->w, d->h, x, y, w, h, UI_FIELD_BG);
	Ui_FillRect(p, d->w, d->w, d->h, x, y, w, 1, UI_SHADOW);
	Ui_FillRect(p, d->w, d->w, d->h, x, y, 1, h, UI_SHADOW);
	Ui_FillRect(p, d->w, d->w, d->h, x + 1, y + 1, w - 2, 1, UI_DARK);
	Ui_FillRect(p, d->w, d->w, d->h, x + 1, y + 1, 1, h - 2, UI_DARK);
	Ui_FillRect(p, d->w, d->w, d->h, x, y + h - 1, w, 1, UI_LIGHT);
	Ui_FillRect(p, d->w, d->w, d->h, x + w - 1, y, 1, h, UI_LIGHT);
}

// the rows of a list, the selected one highlighted; a thin scroll indicator at the right when it does not all fit
static void UiListDraw(UiDialog_t* d, int index)
{
	UiList_t* l = &d->lists[index];
	int rows = UiListRows(l), i, focused = d->focus == d->nFields + d->nButtons + index;
	uint32_t* p = d->pix;
	UiSunkenBox(d, l->x, l->y, l->w, l->h);
	for(i = 0; i < rows && l->top + i < l->count; i++)
	{
		int row = l->top + i, y = l->y + 2 + i * UI_LIST_ROW_H;
		int sel = row == l->selected;
		if(sel)
			Ui_FillRect(p, d->w, d->w, d->h, l->x + 2, y, l->w - 4, UI_LIST_ROW_H, focused ? UI_SELECT_BG : UI_SHADOW);
		OsPosix_FontDraw(d->font, p, d->w, d->w, d->h, l->x + 6, y + 2, l->items[row], sel ? UI_SELECT_TEXT : UI_TEXT);
	}
	if(l->count > rows)
	{ // the part of the list in view, as a bar along the right edge
		int trackH = l->h - 4, barH = trackH * rows / l->count, barY = trackH * l->top / l->count;
		if(barH < 8)
			barH = 8;
		Ui_FillRect(p, d->w, d->w, d->h, l->x + l->w - 6, l->y + 2, 4, trackH, UI_FACE);
		Ui_FillRect(p, d->w, d->w, d->h, l->x + l->w - 6, l->y + 2 + barY, 4, barH, UI_DARK);
	}
}

// paint the whole dialog into its buffer and put it on the window
static void UiDraw(UiDialog_t* d)
{
	int i;
	uint32_t* p = d->pix;
	Ui_FillRect(p, d->w, d->w, d->h, 0, 0, d->w, d->h, UI_FACE);
	if(d->icon)
	{ // a disc with a letter: the message box icons (32 x 32 at the top-left margin)
		int cx = UI_MARGIN + 16, cy = UI_MARGIN + 16, x, y;
		char letter[2] = {(char)d->icon, 0};
		int lw;
		for(y = -16; y < 16; y++)
			for(x = -16; x < 16; x++)
				if(x * x + y * y <= 15 * 15)
					p[(size_t)(cy + y) * d->w + cx + x] = d->iconRgb;
		lw = OsPosix_FontTextWidth(d->font, letter, -1);
		OsPosix_FontDraw(d->font, p, d->w, d->w, d->h, cx - lw / 2, cy - UI_FONT_H / 2, letter, 0xffffff);
	}
	for(i = 0; i < d->nLabels; i++)
		OsPosix_FontDraw(d->font, p, d->w, d->w, d->h, d->labels[i].x, d->labels[i].y, d->labels[i].text, UI_TEXT);
	for(i = 0; i < d->nFields; i++)
	{
		UiFieldW_t* f = &d->fields[i];
		X11Field_Draw(&f->f, p, d->w, d->w, d->h, f->x, f->y, f->w, f->h, d->font, UI_TEXT, 0, d->focus == i, 1);
	}
	for(i = 0; i < d->nLists; i++)
		UiListDraw(d, i);
	for(i = 0; i < d->nButtons; i++)
	{
		UiButton_t* b = &d->buttons[i];
		int down = d->pressed == i, tw;
		int focused = d->focus == d->nFields + i;
		Ui_FillRect(p, d->w, d->w, d->h, b->x, b->y, b->w, b->h, UI_FACE);
		// raised (or sunken while pressed) 3D edge
		Ui_FillRect(p, d->w, d->w, d->h, b->x, b->y, b->w, 1, down ? UI_DARK : UI_LIGHT);
		Ui_FillRect(p, d->w, d->w, d->h, b->x, b->y, 1, b->h, down ? UI_DARK : UI_LIGHT);
		Ui_FillRect(p, d->w, d->w, d->h, b->x, b->y + b->h - 1, b->w, 1, down ? UI_LIGHT : UI_DARK);
		Ui_FillRect(p, d->w, d->w, d->h, b->x + b->w - 1, b->y, 1, b->h, down ? UI_LIGHT : UI_DARK);
		Ui_FillRect(p, d->w, d->w, d->h, b->x + 1, b->y + b->h - 2, b->w - 2, 1, down ? UI_FACE : UI_SHADOW);
		Ui_FillRect(p, d->w, d->w, d->h, b->x + b->w - 2, b->y + 1, 1, b->h - 2, down ? UI_FACE : UI_SHADOW);
		if(i == d->defButton || focused)
		{ // the default / focused button carries a dark outline
			Ui_FillRect(p, d->w, d->w, d->h, b->x - 1, b->y - 1, b->w + 2, 1, UI_FRAME);
			Ui_FillRect(p, d->w, d->w, d->h, b->x - 1, b->y + b->h, b->w + 2, 1, UI_FRAME);
			Ui_FillRect(p, d->w, d->w, d->h, b->x - 1, b->y - 1, 1, b->h + 2, UI_FRAME);
			Ui_FillRect(p, d->w, d->w, d->h, b->x + b->w, b->y - 1, 1, b->h + 2, UI_FRAME);
		}
		tw = OsPosix_FontTextWidth(d->font, b->text, -1);
		OsPosix_FontDraw(d->font, p, d->w, d->w, d->h, b->x + (b->w - tw) / 2 + down, b->y + (b->h - UI_FONT_H) / 2 + down,
			b->text, UI_TEXT); // the caption moves one pixel while pressed
	}
	X11_PutArgb(d->win, 0, 0, d->w, d->h, d->pix, d->w);
}

// the button under (x, y), -1 for none
static int UiHitButton(UiDialog_t* d, int x, int y)
{
	int i;
	for(i = 0; i < d->nButtons; i++)
	{
		UiButton_t* b = &d->buttons[i];
		if(x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h)
			return i;
	}
	return -1;
}

// the field under (x, y), -1 for none
static int UiHitField(UiDialog_t* d, int x, int y)
{
	int i;
	for(i = 0; i < d->nFields; i++)
	{
		UiFieldW_t* f = &d->fields[i];
		if(x >= f->x && x < f->x + f->w && y >= f->y && y < f->y + f->h)
			return i;
	}
	return -1;
}

// the list under (x, y), -1 for none
static int UiHitList(UiDialog_t* d, int x, int y)
{
	int i;
	for(i = 0; i < d->nLists; i++)
	{
		UiList_t* l = &d->lists[i];
		if(x >= l->x && x < l->x + l->w && y >= l->y && y < l->y + l->h)
			return i;
	}
	return -1;
}

/* a button was pressed: end the dialog with its id unless the onButton
 * handler refuses (a handler that changed the dialog's texts sets `redraw`) */
static void UiActivate(UiDialog_t* d, int button)
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
static void UiListSelect(UiDialog_t* d, int index, int row)
{
	UiList_t* l = &d->lists[index];
	if(l->count == 0)
		row = -1;
	else if(row >= l->count)
		row = l->count - 1;
	if(row == l->selected)
		return;
	l->selected = row;
	UiListScrollToSelection(l);
	if(d->onListSelect)
		d->onListSelect(d, index, d->ctx);
	d->redraw = 1;
}

// a numeric picker only holds digits inside its range: anything else typed is dropped and the value clamped
static void UiClampNumeric(UiFieldW_t* f)
{
	int v, i, o = 0;
	char digits[16];
	if(!f->numeric)
		return;
	for(i = 0; f->f.text[i] && o < 15; i++)
		if(f->f.text[i] >= '0' && f->f.text[i] <= '9')
			digits[o++] = f->f.text[i];
	digits[o] = 0;
	v = atoi(digits);
	if(o == 0)
		return; // empty while typing
	if(v < f->min)
		v = f->min;
	if(v > f->max)
		v = f->max;
	snprintf(f->f.text, sizeof f->f.text, "%d", v);
	f->f.caret = (int)strlen(f->f.text);
}

// the dialog window's events for X11_RunModal; 0 once a button ended the dialog
static int UiHandler(XEvent* ev, void* ctx)
{
	UiDialog_t* d = (UiDialog_t*)ctx;
	switch(ev->type)
	{
		case Expose:
			if(ev->xexpose.count == 0)
				UiDraw(d);
			break;
		case KeyPress:
		{
			int sys, vk;
			if(d->ic && XFilterEvent(ev, None))
				break; // the input method took it (composition in progress)
			vk = X11_EventVk(&ev->xkey, &sys);
			if(vk == 0x1b)
			{ // Escape: the cancel button
				if(d->escButton >= 0)
					UiActivate(d, d->escButton);
				break;
			}
			if(vk == 0x0d)
			{ // Enter: the focused button, else the default one
				if(d->focus >= d->nFields)
					UiActivate(d, d->focus - d->nFields);
				else if(d->defButton >= 0)
					UiActivate(d, d->defButton);
				break;
			}
			if(vk == 0x09)
			{ // Tab / Shift+Tab: the next / previous control; a field gets its text selected, as Windows does
				int n = d->nFields + d->nButtons + d->nLists;
				if(n > 0)
				{
					if(ev->xkey.state & ShiftMask)
						d->focus = (d->focus + n - 1) % n;
					else
						d->focus = (d->focus + 1) % n;
					if(d->focus < d->nFields)
						X11Field_SelectAll(&d->fields[d->focus].f);
				}
				UiDraw(d);
				break;
			}
			if(d->focus >= d->nFields + d->nButtons)
			{ // a list has the focus: the arrows, page keys, Home and End move the selection
				int index = d->focus - d->nFields - d->nButtons;
				UiList_t* l = &d->lists[index];
				int rows = UiListRows(l), row = l->selected;
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
					break;
				UiListSelect(d, index, row);
				UiDraw(d);
				break;
			}
			if(d->focus >= d->nFields)
			{ // a button has the focus: Space presses it
				if(vk == 0x20)
					UiActivate(d, d->focus - d->nFields);
				else if(vk == 0x25 || vk == 0x27)
				{ // arrows move between the buttons
					int n = d->nButtons, b = d->focus - d->nFields;
					b = vk == 0x25 ? (b + n - 1) % n : (b + 1) % n;
					d->focus = d->nFields + b;
					UiDraw(d);
				}
				break;
			}
			{ // a field has the focus
				UiFieldW_t* f = &d->fields[d->focus];
				int r;
				if(f->numeric && (vk == 0x26 || vk == 0x28))
				{ // up / down steps a number picker, wrapping at the ends of its range
					int v = atoi(f->f.text) + (vk == 0x26 ? 1 : -1);
					if(v < f->min)
						v = f->max;
					if(v > f->max)
						v = f->min;
					snprintf(f->f.text, sizeof f->f.text, "%d", v);
					f->f.caret = (int)strlen(f->f.text);
					f->f.selAll = 0;
					if(d->onChange)
						d->onChange(d, d->focus, d->ctx);
					UiDraw(d);
					break;
				}
				if(f->nChoices > 0 && (vk == 0x26 || vk == 0x28))
				{ // up / down steps a choice picker through its texts, from the one the field holds (else the first)
					int cur = -1, k;
					for(k = 0; k < f->nChoices; k++)
						if(strcmp(f->choices[k], f->f.text) == 0)
							cur = k;
					if(cur < 0)
						cur = vk == 0x26 ? f->nChoices : -1;
					cur = vk == 0x26 ? (cur + f->nChoices - 1) % f->nChoices : (cur + 1) % f->nChoices;
					X11Field_SetText(&f->f, f->choices[cur]);
					if(d->onChange)
						d->onChange(d, d->focus, d->ctx);
					UiDraw(d);
					break;
				}
				r = X11Field_Key(&f->f, &ev->xkey, d->ic);
				if(r > 0)
				{
					UiClampNumeric(f);
					if(d->onChange)
						d->onChange(d, d->focus, d->ctx);
				}
				if(r >= 0)
					UiDraw(d);
			}
			break;
		}
		case ButtonPress: // a left click: press a button (sunken until released), focus a field (caret at the end) or pick a row
			if(ev->xbutton.button == Button1)
			{
				int b = UiHitButton(d, ev->xbutton.x, ev->xbutton.y);
				int f = UiHitField(d, ev->xbutton.x, ev->xbutton.y);
				int li = UiHitList(d, ev->xbutton.x, ev->xbutton.y);
				if(b >= 0)
				{
					d->pressed = b;
					d->focus = d->nFields + b;
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
					int row = l->top + (ev->xbutton.y - l->y - 2) / UI_LIST_ROW_H;
					int same = row == l->selected && row < l->count;
					d->focus = d->nFields + d->nButtons + li;
					if(row < l->count)
						UiListSelect(d, li, row);
					// a second click on the row within the double-click time activates the default button
					if(same && ev->xbutton.time - d->lastClick < 400 && d->defButton >= 0)
					{
						d->lastClick = 0;
						UiDraw(d);
						UiActivate(d, d->defButton);
						break;
					}
					d->lastClick = ev->xbutton.time;
				}
				UiDraw(d);
			}
			else if(ev->xbutton.button == Button4 || ev->xbutton.button == Button5)
			{ // the wheel scrolls the list under the pointer by three rows
				int li = UiHitList(d, ev->xbutton.x, ev->xbutton.y);
				if(li >= 0)
				{
					UiList_t* l = &d->lists[li];
					int rows = UiListRows(l), maxTop = l->count > rows ? l->count - rows : 0;
					l->top += ev->xbutton.button == Button4 ? -3 : 3;
					if(l->top > maxTop)
						l->top = maxTop;
					if(l->top < 0)
						l->top = 0;
					UiDraw(d);
				}
			}
			break;
		case ButtonRelease: // the button fires when released over the one that was pressed
			if(ev->xbutton.button == Button1 && d->pressed >= 0)
			{
				int b = UiHitButton(d, ev->xbutton.x, ev->xbutton.y);
				int was = d->pressed;
				d->pressed = -1;
				UiDraw(d);
				if(b == was)
					UiActivate(d, b);
				if(d->redraw && !d->done)
				{
					d->redraw = 0;
					UiDraw(d);
				}
			}
			break;
		case ClientMessage: // the close box acts as the cancel button; nothing without one
			if(ev->xclient.message_type == gX11WmProtocols && (Atom)ev->xclient.data.l[0] == gX11WmDelete)
			{
				if(d->escButton >= 0)
					UiActivate(d, d->escButton);
			}
			break;
		case FocusIn:
			if(d->ic)
				XSetICFocus(d->ic);
			break;
		case FocusOut:
			if(d->ic)
				XUnsetICFocus(d->ic);
			break;
		default:
			break;
	}
	if(d->redraw && !d->done)
	{
		d->redraw = 0;
		UiDraw(d);
	}
	return !d->done;
}

/* create the window (a fixed-size dialog for the window manager) centred
 * on the screen and run the modal loop; the id of the button that ended
 * it.  Without memory or a font the dialog counts as cancelled: the
 * cancel button's id, or 0 when it has none. */
int Ui_Run(UiDialog_t* d, const char* titleSjis)
{
	XSetWindowAttributes attr;
	XSizeHints* hints;
	int sw = DisplayWidth(gX11Dpy, gX11Screen), sh = DisplayHeight(gX11Dpy, gX11Screen);
	Atom typeDialog = XInternAtom(gX11Dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	Atom typeAtom = XInternAtom(gX11Dpy, "_NET_WM_WINDOW_TYPE", False);

	d->pix = (uint32_t*)calloc((size_t)d->w * d->h, 4);
	d->font = OsPosix_FontUi(UI_FONT_H);
	if(!d->pix || !d->font)
	{
		free(d->pix);
		if(d->font)
			OS_FontClose(d->font);
		return d->escButton >= 0 ? d->buttons[d->escButton].id : 0;
	}
	memset(&attr, 0, sizeof attr);
	attr.background_pixel = 0xf0f0f0;
	attr.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | ExposureMask |
		StructureNotifyMask | FocusChangeMask;
	d->win = XCreateWindow(gX11Dpy, gX11Root, (sw - d->w) / 2, (sh - d->h) / 2, (unsigned)d->w, (unsigned)d->h, 0,
		gX11Depth, InputOutput, gX11Visual, CWBackPixel | CWEventMask, &attr);
	hints = XAllocSizeHints();
	if(hints)
	{
		hints->flags = PMinSize | PMaxSize | PPosition;
		hints->min_width = hints->max_width = d->w;
		hints->min_height = hints->max_height = d->h;
		hints->x = (sw - d->w) / 2;
		hints->y = (sh - d->h) / 2;
		XSetWMNormalHints(gX11Dpy, d->win, hints);
		XFree(hints);
	}
	XChangeProperty(gX11Dpy, d->win, typeAtom, XA_ATOM, 32, PropModeReplace, (unsigned char*)&typeDialog, 1);
	XSetWMProtocols(gX11Dpy, d->win, &gX11WmDelete, 1);
	X11_SetTitle(d->win, titleSjis);
	if(gX11Im && d->nFields) // an input context only when there is something to type into
		d->ic = XCreateIC(gX11Im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, d->win,
			XNFocusWindow, d->win, NULL);
	d->pressed = -1;
	XMapRaised(gX11Dpy, d->win);
	XFlush(gX11Dpy);
	UiDraw(d);
	X11_RunModal(d->win, UiHandler, d);
	if(d->ic)
		XDestroyIC(d->ic);
	XDestroyWindow(gX11Dpy, d->win);
	XFlush(gX11Dpy);
	OS_FontClose(d->font);
	free(d->pix);
	return d->result;
}

// the width of a string in the UI font (opened for the measurement), for laying a dialog out
int Ui_TextWidth(const char* sjis)
{
	struct OsFont* f = OsPosix_FontUi(UI_FONT_H);
	int w = OsPosix_FontTextWidth(f, sjis, -1);
	OS_FontClose(f);
	return w;
}
