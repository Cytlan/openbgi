/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * field.c - the single-line text field of the dialog toolkit
 *           (src/include/bgi/ui.h): editing with the caret and the select-all
 *           state, typed text, drawing into a 32-bit buffer
 *
 * The dialogs and the X11 host's text entry share it.  The text is in the
 * scripts' encoding and the caret moves by whole characters; typed text
 * comes as UTF-8 and is converted (Shift-JIS through the code page 932
 * table of src/include/bgi/sjis.h).  The selection is all or nothing, as the
 * original's EM_SETSEL 0, -1 after Tab / Enter leaves it.
 */
#include "bgi/ui.h"
#include "bgi/sjis.h"

#include <stdio.h>
#include <string.h>

// ---- the text encoding ----------------------------------------------------------------

// the length of the well-formed UTF-8 sequence at s (1 .. 4), 0 when there is none
static int Utf8Len(const uint8_t* s)
{
	int n, i;
	if(s[0] < 0x80)
		return s[0] ? 1 : 0;
	if(s[0] < 0xc2 || s[0] > 0xf4)
		return 0;
	n = s[0] >= 0xf0 ? 4 : s[0] >= 0xe0 ? 3
										: 2;
	for(i = 1; i < n; i++)
		if((s[i] & 0xc0) != 0x80)
			return 0;
	return n;
}

// the first byte of a two-byte Shift-JIS character
static int IsLead(uint8_t c)
{
	return (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc);
}

int Ui_CharLen(const char* s, int utf8)
{
	const uint8_t* u = (const uint8_t*)s;
	if(!u[0])
		return 0;
	if(utf8)
	{
		int n = Utf8Len(u);
		return n ? n : 1;
	}
	return IsLead(u[0]) && u[1] ? 2 : 1;
}

// a code point below 0x10000 as UTF-8 into out; the bytes written
static int PutUtf8(uint32_t cp, char* out)
{
	if(cp < 0x80)
	{
		out[0] = (char)cp;
		return 1;
	}
	if(cp < 0x800)
	{
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	out[0] = (char)(0xe0 | (cp >> 12));
	out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[2] = (char)(0x80 | (cp & 0x3f));
	return 3;
}

int Ui_FromUtf8(const char* utf8, char* out, int n, int utf8Out)
{
	const uint8_t* p = (const uint8_t*)utf8;
	int o = 0;
	if(n <= 0)
		return 0;
	while(*p)
	{
		int len = Utf8Len(p), k = 0, i;
		uint32_t cp;
		char c[6];
		if(!len)
		{ // a malformed byte: skipped
			p++;
			continue;
		}
		cp = len == 1 ? p[0] : (uint32_t)(p[0] & (0x7f >> len));
		for(i = 1; i < len; i++)
			cp = (cp << 6) | (p[i] & 0x3f);
		p += len;
		if(cp < 0x20 || cp == 0x7f)
			continue; // control characters are keys, not text
		if(utf8Out)
		{
			if(cp >= 0x10000)
			{ // as a surrogate pair of three-byte sequences
				k = PutUtf8(0xd800 + ((cp - 0x10000) >> 10), c);
				k += PutUtf8(0xdc00 + ((cp - 0x10000) & 0x3ff), c + k);
			}
			else
				k = PutUtf8(cp, c);
		}
		else if(cp < 0x10000)
			k = Sjis_FromCodePoint(cp, c);
		if(k <= 0)
			continue; // no code for it
		if(o + k >= n)
			break;
		memcpy(out + o, c, (size_t)k);
		o += k;
	}
	out[o] = 0;
	return o;
}

int Ui_FromSjis(const char* sjis, char* out, int n, int utf8Out)
{
	const uint8_t* s = (const uint8_t*)sjis;
	int o = 0;
	if(n <= 0)
		return 0;
	if(!utf8Out)
	{
		while(s[o] && o + 1 < n)
		{
			out[o] = (char)s[o];
			o++;
		}
		out[o] = 0;
		return o;
	}
	while(*s)
	{
		uint32_t cp;
		char c[3];
		int k;
		if(s[1] && IsLead(s[0]))
		{
			cp = Sjis_CodePoint((uint16_t)(s[0] << 8 | s[1]));
			s += 2;
		}
		else
		{
			cp = s[0] >= 0xa1 && s[0] <= 0xdf ? 0xff61u + (s[0] - 0xa1u) : s[0]; // the half-width katakana
			s++;
		}
		k = PutUtf8(cp, c);
		if(o + k >= n)
			break;
		memcpy(out + o, c, (size_t)k);
		o += k;
	}
	out[o] = 0;
	return o;
}

// ---- editing --------------------------------------------------------------------------

// the start of the character before offset i
static int PrevChar(const UiField_t* f, int i)
{
	int p = 0, q = 0;
	while(q < i && f->text[q])
	{
		p = q;
		q += Ui_CharLen(f->text + q, f->utf8);
	}
	return p;
}

// replace the text (NULL = empty), cut to the limit and to whole characters; caret at the end, nothing selected
void UiField_SetText(UiField_t* f, const char* text)
{
	int i = 0, max = (int)sizeof f->text - 1;
	snprintf(f->text, sizeof f->text, "%s", text ? text : "");
	if(f->limit > 0 && f->limit < max)
		max = f->limit;
	// never end on part of a character
	while(f->text[i])
	{
		int n = Ui_CharLen(f->text + i, f->utf8);
		if(i + n > max || (n == 1 && (f->utf8 ? (uint8_t)f->text[i] >= 0xc0 : IsLead((uint8_t)f->text[i]) && !f->text[i + 1])))
			break; // (a lead byte without the rest of its character)
		i += n;
	}
	f->text[i] = 0;
	f->caret = i;
	f->selAll = 0;
}

void UiField_SelectAll(UiField_t* f)
{
	f->selAll = f->text[0] != 0;
	f->caret = (int)strlen(f->text);
}

// the whole text goes (BackSpace or Delete over the selection)
static void ClearAll(UiField_t* f)
{
	f->text[0] = 0;
	f->caret = 0;
	f->selAll = 0;
}

int UiField_Key(UiField_t* f, int vk, int ctrl, int alt)
{
	switch(vk)
	{
		case 0x08: // BackSpace
			if(f->selAll)
			{
				ClearAll(f);
				return 1;
			}
			if(f->caret > 0)
			{
				int p = PrevChar(f, f->caret);
				memmove(f->text + p, f->text + f->caret, strlen(f->text + f->caret) + 1);
				f->caret = p;
				return 1;
			}
			return 0;
		case 0x2e: // Delete
			if(f->selAll)
			{
				ClearAll(f);
				return 1;
			}
			if(f->text[f->caret])
			{
				int n = Ui_CharLen(f->text + f->caret, f->utf8);
				memmove(f->text + f->caret, f->text + f->caret + n, strlen(f->text + f->caret + n) + 1);
				return 1;
			}
			return 0;
		case 0x25: // Left
			if(f->selAll)
				f->selAll = 0;
			else if(f->caret > 0)
				f->caret = PrevChar(f, f->caret);
			return 0;
		case 0x27: // Right
			if(f->selAll)
				f->selAll = 0;
			else if(f->text[f->caret])
				f->caret += Ui_CharLen(f->text + f->caret, f->utf8);
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
	if(ctrl || alt)
	{
		if(vk == 'A' && ctrl && !alt)
		{
			UiField_SelectAll(f);
			return 0;
		}
		return -1;
	}
	return UI_FIELD_TYPES;
}

int UiField_Type(UiField_t* f, const char* utf8)
{
	char typed[0x100];
	int n = Ui_FromUtf8(utf8, typed, (int)sizeof typed, f->utf8), len, room, k;
	if(n <= 0)
		return 0;
	if(f->noAscii && n == 1 && (uint8_t)typed[0] >= 0x20 && (uint8_t)typed[0] < 0x80)
		return 0; // WM_CHAR 0x20 .. 0x7f swallowed by the subclass of 1.69/472 on ("B0 29")
	if(f->selAll)
		ClearAll(f);
	len = (int)strlen(f->text);
	room = (int)sizeof f->text - 1 - len;
	if(f->limit > 0 && f->limit - len < room)
		room = f->limit - len;
	for(k = 0; k < n && k + Ui_CharLen(typed + k, f->utf8) <= room;) // only whole characters fit
		k += Ui_CharLen(typed + k, f->utf8);
	if(k <= 0)
		return 0;
	memmove(f->text + f->caret + k, f->text + f->caret, (size_t)(len - f->caret + 1));
	memcpy(f->text + f->caret, typed, (size_t)k);
	f->caret += k;
	return 1;
}

// ---- drawing --------------------------------------------------------------------------

void Ui_FillRect(uint32_t* pix, int pitch, int bufW, int bufH, int x, int y, int w, int h, uint32_t rgb)
{
	int xx, yy, x0 = x < 0 ? 0 : x, x1 = x + w > bufW ? bufW : x + w;
	for(yy = y < 0 ? 0 : y; yy < y + h && yy < bufH; yy++)
		for(xx = x0; xx < x1; xx++)
			pix[(size_t)yy * pitch + xx] = rgb;
}

// the width of the first `bytes` bytes (all when < 0)
static int Width(const UiFont_t* font, const char* text, int bytes)
{
	return font && font->width ? font->width(font->ctx, text, bytes) : 0;
}

int Ui_FontWidth(const UiFont_t* font, const char* text)
{
	return Width(font, text, -1);
}

static void Draw(const UiFont_t* font, uint32_t* pix, int pitch, int w, int h, int x, int y, const char* text, uint32_t rgb)
{
	if(font && font->draw)
		font->draw(font->ctx, pix, pitch, w, h, x, y, text, rgb);
}

/* the optional sunken box, the text centred or left-aligned behind 2
 * pixels of padding, the selection highlight when the whole text is
 * selected, and a one-pixel caret in the text colour */
void UiField_Draw(const UiField_t* f, uint32_t* pix, int pitch, int bufW, int bufH, int x, int y, int w, int h,
	const UiFont_t* font, uint32_t rgb, int centred, int focused, int frame)
{
	int fontH = font ? font->height : 0;
	int textW = Width(font, f->text, -1);
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
		int caretX = Width(font, f->text, f->caret);
		if(caretX > w - 2 * inner - 6)
			tx -= caretX - (w - 2 * inner - 6);
	}
	if(focused && f->selAll && f->text[0])
	{
		Ui_FillRect(pix, pitch, bufW, bufH, tx, ty, textW, fontH, UI_SELECT_BG);
		Draw(font, pix, pitch, bufW, bufH, tx, ty, f->text, UI_SELECT_TEXT);
	}
	else
		Draw(font, pix, pitch, bufW, bufH, tx, ty, f->text, rgb);
	if(focused && !f->selAll)
	{
		int cx = tx + Width(font, f->text, f->caret);
		Ui_FillRect(pix, pitch, bufW, bufH, cx, ty, 1, fontH, rgb);
	}
}
