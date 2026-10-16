/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * field.c - the single-line text field of the POSIX back end's own
 *           windows (inc/bgi/os_x11.h): editing with the caret and the
 *           select-all state, and drawing into a 32-bit buffer
 *
 * The text entry over the picture (edit.c) and the dialogs (dialogs.c)
 * share it.  The text is Shift-JIS and the caret moves by whole
 * characters.  Typed text comes through the X input method when there is
 * one, so Japanese can be entered with an IME; the conversion to
 * Shift-JIS is CP932 through iconv (sjis.c).  The selection is all or
 * nothing, as the original's EM_SETSEL 0, -1 after Tab / Enter leaves it.
 */
#include "ui_internal.h"

// byte length of the character starting at s[i]
static int CharLen(const char* s, int i)
{
	return OsCommon_SjisIsLead((uint8_t)s[i]) && s[i + 1] ? 2 : 1;
}

// the start of the character before offset i
static int PrevChar(const char* s, int i)
{
	int p = 0, q = 0;
	while(q < i)
	{
		p = q;
		q += CharLen(s, q);
	}
	return p;
}

// replace the text (NULL = empty), cut to the limit and to whole characters; caret at the end, nothing selected
void X11Field_SetText(X11Field_t* f, const char* text)
{
	snprintf(f->text, sizeof f->text, "%s", text ? text : "");
	if(f->limit > 0 && (int)strlen(f->text) > f->limit)
		f->text[f->limit] = 0;
	// never end on half a character
	{
		int i = 0, last = 0;
		while(f->text[i])
		{
			last = i;
			i += CharLen(f->text, i);
		}
		if(f->text[last] && OsCommon_SjisIsLead((uint8_t)f->text[last]) && !f->text[last + 1])
			f->text[last] = 0;
	}
	f->caret = (int)strlen(f->text);
	f->selAll = 0;
}

void X11Field_SelectAll(X11Field_t* f)
{
	f->selAll = f->text[0] != 0;
	f->caret = (int)strlen(f->text);
}

/* insert `n` bytes of typed text at the caret, replacing the selection;
 * what does not fit the limit or the buffer is dropped by whole
 * characters */
static void FieldInsert(X11Field_t* f, const char* bytes, int n)
{
	int len, room;
	if(f->selAll)
	{
		f->text[0] = 0;
		f->caret = 0;
		f->selAll = 0;
	}
	len = (int)strlen(f->text);
	room = (int)sizeof f->text - 1 - len;
	if(f->limit > 0 && f->limit - len < room)
		room = f->limit - len;
	if(n > room)
	{ // only whole characters fit
		int k = 0;
		while(k < n && k + CharLen(bytes, k) <= room)
			k += CharLen(bytes, k);
		n = k;
	}
	if(n <= 0)
		return;
	memmove(f->text + f->caret + n, f->text + f->caret, (size_t)(len - f->caret + 1));
	memcpy(f->text + f->caret, bytes, (size_t)n);
	f->caret += n;
}

/* a key press: the editing keys (BackSpace, Delete, the arrows, Home,
 * End, Ctrl+A) and the typed text; see os_x11.h for the result.  Tab,
 * Enter, Escape and any other Ctrl / Alt combination are the caller's. */
int X11Field_Key(X11Field_t* f, XKeyEvent* ev, XIC ic)
{
	int sys, vk = X11_EventVk(ev, &sys);
	char typed[64];
	int len;
	switch(vk)
	{
		case 0x08: // BackSpace
			if(f->selAll)
			{
				f->text[0] = 0;
				f->caret = 0;
				f->selAll = 0;
				return 1;
			}
			if(f->caret > 0)
			{
				int p = PrevChar(f->text, f->caret);
				memmove(f->text + p, f->text + f->caret, strlen(f->text + f->caret) + 1);
				f->caret = p;
				return 1;
			}
			return 0;
		case 0x2e: // Delete
			if(f->selAll)
			{
				f->text[0] = 0;
				f->caret = 0;
				f->selAll = 0;
				return 1;
			}
			if(f->text[f->caret])
			{
				int n = CharLen(f->text, f->caret);
				memmove(f->text + f->caret, f->text + f->caret + n, strlen(f->text + f->caret + n) + 1);
				return 1;
			}
			return 0;
		case 0x25: // Left
			if(f->selAll)
				f->selAll = 0;
			else if(f->caret > 0)
				f->caret = PrevChar(f->text, f->caret);
			return 0;
		case 0x27: // Right
			if(f->selAll)
				f->selAll = 0;
			else if(f->text[f->caret])
				f->caret += CharLen(f->text, f->caret);
			return 0;
		case 0x24: // Home
			f->selAll = 0;
			f->caret = 0;
			return 0;
		case 0x23: // End
			f->selAll = 0;
			f->caret = (int)strlen(f->text);
			return 0;
		case 0x09: // Tab, Enter, Escape: the owner decides (focus, confirm, cancel)
		case 0x0d:
		case 0x1b:
			return -1;
		default:
			break;
	}
	if(ev->state & (ControlMask | Mod1Mask))
	{
		if(vk == 'A' && (ev->state & ControlMask))
		{
			X11Field_SelectAll(f);
			return 0;
		}
		return -1;
	}
	len = X11_EventText(ev, ic, typed, (int)sizeof typed);
	if(len <= 0)
		return vk ? 0 : -1; // a key that types nothing: consumed when it is a known key, else not ours
	if(f->noAscii && len == 1 && (uint8_t)typed[0] >= 0x20 && (uint8_t)typed[0] < 0x80)
		return 0; // WM_CHAR 0x20 .. 0x7f swallowed by the subclass of 1.69/472 on ("B0 29")
	FieldInsert(f, typed, len);
	return 1;
}

// fill a rectangle of a 32-bit buffer (pitch in pixels), clipped to bufW x bufH
void Ui_FillRect(uint32_t* pix, int pitch, int bufW, int bufH, int x, int y, int w, int h, uint32_t rgb)
{
	int xx, yy;
	for(yy = y; yy < y + h; yy++)
	{
		if(yy < 0 || yy >= bufH)
			continue;
		for(xx = x; xx < x + w; xx++)
			if(xx >= 0 && xx < bufW)
				pix[(size_t)yy * pitch + xx] = rgb;
	}
}

/* draw the field (see os_x11.h): the optional sunken box, the text
 * centred or left-aligned behind 2 pixels of padding, the selection
 * highlight when the whole text is selected, and a one-pixel caret in
 * the text colour */
void X11Field_Draw(const X11Field_t* f, uint32_t* pix, int pitch, int bufW, int bufH, int x, int y, int w, int h,
	struct OsFont* font, uint32_t rgb, int centred, int focused, int frame)
{
	int fontH = OsPosix_FontHeight(font);
	int textW = OsPosix_FontTextWidth(font, f->text, -1);
	int tx, ty = y + (h - fontH) / 2;
	int inner = frame ? 3 : 0; // the box's border width
	if(frame)
	{ // a sunken white box
		Ui_FillRect(pix, pitch, bufW, bufH, x, y, w, h, UI_FIELD_BG);
		Ui_FillRect(pix, pitch, bufW, bufH, x, y, w, 1, UI_SHADOW);
		Ui_FillRect(pix, pitch, bufW, bufH, x, y, 1, h, UI_SHADOW);
		Ui_FillRect(pix, pitch, bufW, bufH, x + 1, y + 1, w - 2, 1, UI_DARK);
		Ui_FillRect(pix, pitch, bufW, bufH, x + 1, y + 1, 1, h - 2, UI_DARK);
		Ui_FillRect(pix, pitch, bufW, bufH, x, y + h - 1, w, 1, UI_LIGHT);
		Ui_FillRect(pix, pitch, bufW, bufH, x + w - 1, y, 1, h, UI_LIGHT);
	}
	tx = centred ? x + (w - textW) / 2 : x + inner + 2;
	// keep the caret in view when the text is wider than the field
	if(!centred && textW > w - 2 * inner - 4)
	{
		int caretX = OsPosix_FontTextWidth(font, f->text, f->caret);
		if(caretX > w - 2 * inner - 6)
			tx -= caretX - (w - 2 * inner - 6);
	}
	if(focused && f->selAll && f->text[0])
	{
		Ui_FillRect(pix, pitch, bufW, bufH, tx, ty, textW, fontH, UI_SELECT_BG);
		OsPosix_FontDraw(font, pix, pitch, bufW, bufH, tx, ty, f->text, UI_SELECT_TEXT);
	}
	else
		OsPosix_FontDraw(font, pix, pitch, bufW, bufH, tx, ty, f->text, rgb);
	if(focused && !f->selAll)
	{
		int cx = tx + OsPosix_FontTextWidth(font, f->text, f->caret);
		Ui_FillRect(pix, pitch, bufW, bufH, cx, ty, 1, fontH, rgb);
	}
}
