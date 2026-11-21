/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dialogs.c - the original's dialogs on the dialog toolkit
 *             (src/include/bgi/ui.h): MessageBoxA and the dialog resources 0x71
 *             (one text field), 0x72 (two labelled fields) and 0x77 (the
 *             player's name and birthday)
 *
 * Each is laid out to resemble the original's; whoever shows it reads the
 * entries back from the fields once it ended with OK (result 1).  The
 * engine's own texts (msg.h) are Shift-JIS and go into a UTF-8 dialog
 * converted.
 */
#include "bgi/ui.h"
#include "openbgi.h" // OPENBGI_MB_*, OPENBGI_ID*
#include "bgi/msg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DLG_FIELD_W 320 // pixels: the text fields of the input dialogs

// a text of the engine's (Shift-JIS) in the dialog's encoding, into out (n bytes)
static const char* Own(const UiDialog_t* d, const char* sjis, char* out, int n)
{
	if(!d->utf8)
		return sjis;
	Ui_FromSjis(sjis, out, n, 1);
	return out;
}

// the title: the given one, else the engine's default for the dialog
static void SetTitle(UiDialog_t* d, const char* title, const char* fallbackSjis)
{
	char own[0x200];
	snprintf(d->title, sizeof d->title, "%s", title ? title : Own(d, fallbackSjis, own, (int)sizeof own));
}

// a label of the engine's text
static int OwnLabel(UiDialog_t* d, int x, int y, const char* sjis)
{
	char own[0x200];
	return Ui_LabelAdd(d, x, y, Own(d, sjis, own, (int)sizeof own));
}

// the width of an engine's text in the dialog's font
static int OwnWidth(const UiDialog_t* d, const char* sjis)
{
	char own[0x200];
	return Ui_TextWidth(d, Own(d, sjis, own, (int)sizeof own));
}

// ---- the message box ----------------------------------------------------------------------

#define BOX_LINES 16 // lines of the text before wrapping

/* the longest start of `s` (whole characters, `len` bytes at most) no
 * wider than `width`; at a space when there is one (the rest starts after
 * it), else at the last character that fits, at least one character */
static int WrapAt(const UiDialog_t* d, const char* s, int len, int width, int* skip)
{
	int i = 0, fit = 0, space = -1;
	*skip = 0;
	while(i < len)
	{
		int n = Ui_CharLen(s + i, d->utf8);
		if(d->font && d->font->width && d->font->width(d->font->ctx, s, i + n) > width)
			break;
		if(s[i] == ' ')
			space = i;
		i += n;
		fit = i;
	}
	if(fit >= len)
		return len;
	if(space > 0)
	{
		*skip = 1;
		return space;
	}
	return fit > 0 ? fit : Ui_CharLen(s, d->utf8);
}

void Ui_BuildMessageBox(UiDialog_t* d, const char* text, const char* caption, uint32_t flags, int utf8, int maxW)
{
	char lines[UI_MAX_LABELS][0x200];
	int nLines = 0, i, textW = 0, textX, y, btnY, totalBtnW, bx, nSrc = 0;
	const char* btnText[2];
	int btnId[2], nBtn, defBtn;
	uint32_t type = flags & 0xf, icon = flags & 0xf0; // the MB_* button and icon fields
	const char* p = text ? text : "";

	d->utf8 = utf8;
	switch(type)
	{
		case OPENBGI_MB_OKCANCEL:
			btnText[0] = "OK", btnId[0] = OPENBGI_IDOK;
			btnText[1] = "Cancel", btnId[1] = OPENBGI_IDCANCEL;
			nBtn = 2;
			break;
		case OPENBGI_MB_YESNO:
			btnText[0] = "Yes", btnId[0] = OPENBGI_IDYES;
			btnText[1] = "No", btnId[1] = OPENBGI_IDNO;
			nBtn = 2;
			break;
		case OPENBGI_MB_RETRYCANCEL:
			btnText[0] = "Retry", btnId[0] = OPENBGI_IDRETRY;
			btnText[1] = "Cancel", btnId[1] = OPENBGI_IDCANCEL;
			nBtn = 2;
			break;
		default:
			btnText[0] = "OK", btnId[0] = OPENBGI_IDOK;
			nBtn = 1;
			break;
	}
	defBtn = (flags & OPENBGI_MB_DEFBUTTON2) && nBtn > 1 ? 1 : 0;
	switch(icon)
	{ // the icon's letter and disc colour: red error, blue question and information, amber warning
		case OPENBGI_MB_ICONHAND: d->icon = 'x', d->iconRgb = 0xd02020; break;
		case OPENBGI_MB_ICONQUESTION: d->icon = '?', d->iconRgb = 0x2060d0; break;
		case OPENBGI_MB_ICONWARNING: d->icon = '!', d->iconRgb = 0xe0a000; break;
		case OPENBGI_MB_ICONINFO: d->icon = 'i', d->iconRgb = 0x2060d0; break;
		default: break;
	}
	textX = UI_MARGIN + (d->icon ? 32 + UI_MARGIN : 0);
	totalBtnW = nBtn * UI_BTN_W + (nBtn - 1) * 8;

	// split the text into lines at '\n' ('\r' dropped), keeping characters whole; wrapped to the width allowed
	while(*p && nSrc < BOX_LINES && nLines < UI_MAX_LABELS)
	{
		char line[0x200];
		int n = 0, start = 0;
		while(*p && *p != '\n')
		{
			int k = Ui_CharLen(p, utf8);
			if(*p == '\r')
			{
				p++;
				continue;
			}
			if(n + k >= (int)sizeof line - 1)
				break; // the rest is a line of its own
			memcpy(line + n, p, (size_t)k);
			n += k;
			p += k;
		}
		line[n] = 0;
		nSrc++;
		if(*p == '\n')
			p++;
		do
		{
			int skip = 0, take = n - start;
			if(maxW > 0)
				take = WrapAt(d, line + start, n - start, maxW - textX - UI_MARGIN, &skip);
			memcpy(lines[nLines], line + start, (size_t)take);
			lines[nLines][take] = 0;
			nLines++;
			start += take + skip;
		} while(start < n && nLines < UI_MAX_LABELS);
	}
	if(nLines == 0)
	{
		lines[0][0] = 0;
		nLines = 1;
	}
	for(i = 0; i < nLines; i++)
	{
		int w = Ui_TextWidth(d, lines[i]);
		if(w > textW)
			textW = w;
	}
	// the text right of the 32-pixel icon, the buttons centred below with 8 pixels between them, 220 pixels wide at least
	d->w = textX + textW + UI_MARGIN;
	if(d->w < totalBtnW + 2 * UI_MARGIN)
		d->w = totalBtnW + 2 * UI_MARGIN;
	if(d->w < 220)
		d->w = 220;
	y = UI_MARGIN + (d->icon && nLines * (UI_FONT_H + 2) < 32 ? (32 - nLines * (UI_FONT_H + 2)) / 2 : 0); // short text centred on the icon
	for(i = 0; i < nLines; i++)
		Ui_LabelAdd(d, textX, y + i * (UI_FONT_H + 2), lines[i]);
	y += nLines * (UI_FONT_H + 2);
	if(d->icon && y < UI_MARGIN + 32)
		y = UI_MARGIN + 32;
	btnY = y + UI_MARGIN;
	d->h = btnY + UI_BTN_H + UI_MARGIN;
	bx = (d->w - totalBtnW) / 2;
	for(i = 0; i < nBtn; i++)
		Ui_ButtonAdd(d, bx + i * (UI_BTN_W + 8), btnY, btnText[i], btnId[i]);
	d->defButton = defBtn;
	d->escButton = nBtn == 2 ? (type == OPENBGI_MB_YESNO ? -1 : 1) : 0; // MB_YESNO has no close box (nothing to cancel with)
	d->focus = d->nFields + defBtn;
	d->result = btnId[defBtn];
	snprintf(d->title, sizeof d->title, "%s", caption ? caption : "");
}

// ---- the input dialogs ----------------------------------------------------------------------

void Ui_BuildInput1(UiDialog_t* d, const char* title, const char* initial, int maxLen, int digits, int utf8)
{
	int y = UI_MARGIN;
	d->utf8 = utf8;
	d->w = DLG_FIELD_W + 2 * UI_MARGIN;
	Ui_FieldAdd(d, UI_MARGIN, y, DLG_FIELD_W, initial, maxLen);
	d->fields[0].f.selAll = 0; // the original leaves the caret at the end (EM_SETSEL len, len)
	d->fields[0].digits = digits;
	y += UI_FIELD_H + UI_MARGIN;
	Ui_ButtonAdd(d, d->w - UI_MARGIN - 2 * UI_BTN_W - 8, y, "OK", 1);
	Ui_ButtonAdd(d, d->w - UI_MARGIN - UI_BTN_W, y, "Cancel", 0);
	d->h = y + UI_BTN_H + UI_MARGIN;
	d->defButton = 0;
	d->escButton = 1;
	d->focus = 0;
	d->result = 0;
	SetTitle(d, title, MSG_DLG_COMMENT_INPUT); // a NULL title selects the default caption, as the original does
}

void Ui_BuildInput2(UiDialog_t* d, const char* title, const char* label1, const char* initial1, int maxLen1, int digits1,
	const char* label2, const char* initial2, int maxLen2, int digits2, int compact, int utf8)
{
	int y = UI_MARGIN;
	d->utf8 = utf8;
	d->w = DLG_FIELD_W + 2 * UI_MARGIN;
	if(compact)
	{
		const int fw = DLG_FIELD_W / 2 - 8;
		label1 ? Ui_LabelAdd(d, UI_MARGIN, y, label1) : OwnLabel(d, UI_MARGIN, y, MSG_DLG_STRING1); // NULL pointers select the defaults
		label2 ? Ui_LabelAdd(d, UI_MARGIN + fw + 16, y, label2) : OwnLabel(d, UI_MARGIN + fw + 16, y, MSG_DLG_STRING2);
		y += UI_FONT_H + 4;
		Ui_FieldAdd(d, UI_MARGIN, y, fw, initial1, maxLen1);
		Ui_FieldAdd(d, UI_MARGIN + fw + 16, y, fw, initial2, maxLen2);
		y += UI_FIELD_H + UI_MARGIN;
	}
	else
	{
		label1 ? Ui_LabelAdd(d, UI_MARGIN, y, label1) : OwnLabel(d, UI_MARGIN, y, MSG_DLG_STRING1);
		y += UI_FONT_H + 4;
		Ui_FieldAdd(d, UI_MARGIN, y, DLG_FIELD_W, initial1, maxLen1);
		y += UI_FIELD_H + 8;
		label2 ? Ui_LabelAdd(d, UI_MARGIN, y, label2) : OwnLabel(d, UI_MARGIN, y, MSG_DLG_STRING2);
		y += UI_FONT_H + 4;
		Ui_FieldAdd(d, UI_MARGIN, y, DLG_FIELD_W, initial2, maxLen2);
		y += UI_FIELD_H + UI_MARGIN;
	}
	d->fields[0].digits = digits1;
	d->fields[1].digits = digits2;
	Ui_ButtonAdd(d, d->w - UI_MARGIN - 2 * UI_BTN_W - 8, y, "OK", 1);
	Ui_ButtonAdd(d, d->w - UI_MARGIN - UI_BTN_W, y, "Cancel", 0);
	d->h = y + UI_BTN_H + UI_MARGIN;
	d->defButton = 0;
	d->escButton = 1;
	// the focus goes to the first field without an initial text (a NULL pointer)
	d->focus = !initial1 ? 0 : !initial2 ? 1
										 : 0;
	d->result = 0;
	SetTitle(d, title, MSG_DLG_DATA_INPUT);
}

// ---- dialog 0x77: the player's name and birthday ------------------------------------------------

static const int kDaysOfMonth[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; // February always 29, as the original's list

// the profile's fields: the names first, then the pickers (their indexes in aux)
#define PROFILE_MONTH 0
#define PROFILE_DAY   1

/* every character of a name field must be full width (double-byte in
 * Shift-JIS; in UTF-8 neither ASCII nor half-width katakana); an empty
 * field passes */
static int AllFullWidth(const char* s, int utf8)
{
	while(*s)
	{
		const uint8_t* u = (const uint8_t*)s;
		int n = Ui_CharLen(s, utf8);
		if(utf8 ? (n == 1 || (n == 3 && u[0] == 0xef && ((u[1] == 0xbd && u[2] >= 0xa1) || (u[1] == 0xbe && u[2] <= 0x9f)))) : n != 2)
			return 0;
		s += n;
	}
	return 1;
}

/* the OK button: a name field with a half-width character is refused
 * with the complaint MSG_DLG_HALFWIDTH naming the field (caption
 * MSG_DLG_INPUT_ERROR) and the dialog stays open */
static int ProfileOk(UiDialog_t* d, int id, void* ctx)
{
	static const char* const kNames[4] = {MSG_DLG_SURNAME, MSG_DLG_GIVEN_NAME, MSG_DLG_NICKNAME, MSG_DLG_PRONOUN};
	int i;
	BGI_UNUSED(ctx);
	if(id != 1)
		return 1;
	for(i = 0; i < 4; i++)
	{
		if(!AllFullWidth(d->fields[i].f.text, d->utf8))
		{
			char msg[0x200], own[0x200];
			snprintf(msg, sizeof msg, MSG_DLG_HALFWIDTH, kNames[i]);
			snprintf(d->complaint, sizeof d->complaint, "%s", Own(d, msg, own, (int)sizeof own));
			snprintf(d->complaintCaption, sizeof d->complaintCaption, "%s", Own(d, MSG_DLG_INPUT_ERROR, own, (int)sizeof own));
			d->complaintFlags = OPENBGI_MB_ICONHAND;
			return 0;
		}
	}
	return 1;
}

// the month picker changed: the day range follows (the original's CBN_SELENDOK handler of combo box 0x409)
static void ProfileChange(UiDialog_t* d, int field, void* ctx)
{
	BGI_UNUSED(ctx);
	if(field == d->aux[PROFILE_MONTH])
	{
		int m = atoi(d->fields[field].f.text);
		UiFieldW_t* day = &d->fields[d->aux[PROFILE_DAY]];
		if(m >= 1 && m <= 12)
		{
			day->max = kDaysOfMonth[m - 1];
			if(atoi(day->f.text) > day->max)
			{ // the original resets a day past the new range to the first
				snprintf(day->f.text, sizeof day->f.text, "1");
				day->f.caret = 1;
			}
		}
	}
}

void Ui_BuildProfile(UiDialog_t* d, const char* surname, const char* givenName, const char* nickname, const char* pronoun,
	int month, int day, int utf8)
{
	int y = UI_MARGIN, labelW, fieldX, given = 0, monthW;
	char num[16];
	d->utf8 = utf8;
	labelW = OwnWidth(d, MSG_DLG_NICKNAME) + 16; // the nickname label sets the width of the label column
	given = OwnWidth(d, MSG_DLG_GIVEN_NAME);
	monthW = OwnWidth(d, MSG_DLG_MONTH);
	fieldX = UI_MARGIN + labelW;
	d->w = fieldX + 200 + UI_MARGIN;

	// surname / given name on one row, like the resource
	OwnLabel(d, UI_MARGIN, y + 3, MSG_DLG_SURNAME);
	Ui_FieldAdd(d, fieldX, y, 90, surname, 10);
	OwnLabel(d, fieldX + 100, y + 3, MSG_DLG_GIVEN_NAME);
	Ui_FieldAdd(d, fieldX + 100 + given + 8, y, 90 - given + 2, givenName, 10);
	y += UI_FIELD_H + 8;
	OwnLabel(d, UI_MARGIN, y + 3, MSG_DLG_NICKNAME);
	Ui_FieldAdd(d, fieldX, y, 200, nickname, 10);
	y += UI_FIELD_H + 8;
	OwnLabel(d, UI_MARGIN, y + 3, MSG_DLG_PRONOUN);
	Ui_FieldAdd(d, fieldX, y, 200, pronoun, 10);
	y += UI_FIELD_H + 8;
	OwnLabel(d, UI_MARGIN, y + 3, MSG_DLG_BIRTHDAY);
	snprintf(num, sizeof num, "%d", month + 1); // the engine holds 0-based selections
	d->aux[PROFILE_MONTH] = Ui_FieldAdd(d, fieldX, y, 44, num, 2);
	d->fields[d->aux[PROFILE_MONTH]].numeric = 1;
	d->fields[d->aux[PROFILE_MONTH]].min = 1;
	d->fields[d->aux[PROFILE_MONTH]].max = 12;
	OwnLabel(d, fieldX + 50, y + 3, MSG_DLG_MONTH);
	snprintf(num, sizeof num, "%d", day + 1);
	d->aux[PROFILE_DAY] = Ui_FieldAdd(d, fieldX + 50 + monthW + 12, y, 44, num, 2);
	d->fields[d->aux[PROFILE_DAY]].numeric = 1;
	d->fields[d->aux[PROFILE_DAY]].min = 1;
	d->fields[d->aux[PROFILE_DAY]].max = 31; // 1..31 until the month changes, as the resource lists them
	OwnLabel(d, fieldX + 50 + monthW + 12 + 50, y + 3, MSG_DLG_DAY);
	y += UI_FIELD_H + UI_MARGIN;
	Ui_ButtonAdd(d, (d->w - UI_BTN_W) / 2, y, "OK", 1);
	d->h = y + UI_BTN_H + UI_MARGIN;
	d->defButton = 0;
	d->escButton = -1;     // OK only: the dialog cannot be cancelled
	d->focus = d->nFields; // SetFocus(GetDlgItem(hwnd, IDOK))
	d->onButton = ProfileOk;
	d->onChange = ProfileChange;
	d->result = 0;
	SetTitle(d, NULL, MSG_DLG_PROFILE_TITLE);
}

void Ui_ProfileResult(const UiDialog_t* d, char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month,
	int32_t* day)
{
	int m = atoi(d->fields[d->aux[PROFILE_MONTH]].f.text), dd = atoi(d->fields[d->aux[PROFILE_DAY]].f.text);
	snprintf(surname, 0x100, "%s", d->fields[0].f.text);
	snprintf(givenName, 0x100, "%s", d->fields[1].f.text);
	snprintf(nickname, 0x100, "%s", d->fields[2].f.text);
	snprintf(pronoun, 0x100, "%s", d->fields[3].f.text);
	*month = (m < 1 ? 1 : m) - 1;
	*day = (dd < 1 ? 1 : dd) - 1;
}

// ---- OpenBGI's own --------------------------------------------------------------------------------

static const char* const kSettingsSize[] = {"Automatic", "100 %", "150 %", "200 %", "Fit the screen"};
static const char* const kSettingsFilter[] = {"The game's", "Bicubic", "Bilinear", "Nearest"};
static const char* const kSettingsOffOn[] = {"Off", "On"};
static const char* const kSettingsQuality[] = {"Fast", "High"};

// a picker that only steps, with its label in front; the next row's y
static int SettingsRow(UiDialog_t* d, int y, int fieldX, int fieldW, const char* label, const char* const* choices, int count, int value)
{
	int f;
	Ui_LabelAdd(d, UI_MARGIN, y + (UI_FIELD_H - UI_FONT_H) / 2, label);
	f = Ui_FieldAdd(d, fieldX, y, fieldW, choices[value >= 0 && value < count ? value : 0], 0);
	if(f >= 0)
	{
		d->fields[f].choices = choices;
		d->fields[f].nChoices = count;
		d->fields[f].pick = 1;
		d->fields[f].f.selAll = 0;
	}
	return y + UI_FIELD_H + 6;
}

void Ui_BuildDisplaySettings(UiDialog_t* d, int size, const char* const* filters, int nFilters, int filter, int quality, int whole,
	int fullscreen, int utf8)
{
	static const char* const kLabels[] = {"Window size:", "Filter:", "Filter quality:", "Whole steps only:", "Full screen:"};
	const char* hint = "Click a setting to change it.";
	const int fieldW = 160;
	int y = UI_MARGIN, labelW = 0, i, reset, close, resetW;
	d->utf8 = utf8;
	d->aux[0] = quality >= 0;
	if(!filters || nFilters < 1)
	{
		filters = kSettingsFilter;
		nFilters = 4;
	}
	for(i = 0; i < 5; i++)
		if(Ui_TextWidth(d, kLabels[i]) > labelW)
			labelW = Ui_TextWidth(d, kLabels[i]);
	labelW += UI_MARGIN + 12;
	y = SettingsRow(d, y, labelW, fieldW, kLabels[0], kSettingsSize, 5, size);
	y = SettingsRow(d, y, labelW, fieldW, kLabels[1], filters, nFilters, filter);
	if(quality >= 0)
		y = SettingsRow(d, y, labelW, fieldW, kLabels[2], kSettingsQuality, 2, quality != 0);
	y = SettingsRow(d, y, labelW, fieldW, kLabels[3], kSettingsOffOn, 2, whole != 0);
	y = SettingsRow(d, y, labelW, fieldW, kLabels[4], kSettingsOffOn, 2, fullscreen != 0);
	y += 4;
	Ui_LabelAdd(d, UI_MARGIN, y, hint);
	y += UI_FONT_H + UI_MARGIN;
	resetW = Ui_TextWidth(d, "Reset window size") + 24;
	if(resetW < UI_BTN_W)
		resetW = UI_BTN_W;
	d->w = labelW + fieldW + UI_MARGIN;
	if(d->w < UI_MARGIN + resetW + 8 + UI_BTN_W + UI_MARGIN)
		d->w = UI_MARGIN + resetW + 8 + UI_BTN_W + UI_MARGIN;
	if(d->w < Ui_TextWidth(d, hint) + 2 * UI_MARGIN)
		d->w = Ui_TextWidth(d, hint) + 2 * UI_MARGIN;
	reset = Ui_ButtonAdd(d, UI_MARGIN, y, "Reset window size", UI_SETTINGS_RESET);
	close = Ui_ButtonAdd(d, d->w - UI_MARGIN - UI_BTN_W, y, "Close", 0);
	if(reset >= 0)
		d->buttons[reset].w = resetW;
	d->h = y + UI_BTN_H + UI_MARGIN;
	d->defButton = close;
	d->escButton = close;
	d->focus = 0;
	UiField_SelectAll(&d->fields[0].f);
	d->result = 0;
	snprintf(d->title, sizeof d->title, "%s", "Display settings");
}

int Ui_PickValue(const UiDialog_t* d, int field)
{
	const UiFieldW_t* f;
	int k;
	if(field < 0 || field >= d->nFields)
		return 0;
	f = &d->fields[field];
	for(k = 0; k < f->nChoices; k++)
		if(strcmp(f->choices[k], f->f.text) == 0)
			return k;
	return 0;
}

void Ui_PickSet(UiDialog_t* d, int field, int value)
{
	UiFieldW_t* f;
	if(field < 0 || field >= d->nFields)
		return;
	f = &d->fields[field];
	if(value < 0 || value >= f->nChoices)
		return;
	UiField_SetText(&f->f, f->choices[value]);
	if(d->focus == field)
		UiField_SelectAll(&f->f);
	else
		f->f.selAll = 0;
}
