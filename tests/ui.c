/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/ui.c - unit test of the dialog toolkit (src/ui/, src/include/bgi/ui.h);
 *              built as test_ui by `make test`
 *
 * The dialogs get a font of the test's: every byte 8 pixels wide, drawn
 * as a block, so that where a text lands is known to the pixel.  The
 * groups, in the order they run:
 *
 *   text    - typed UTF-8 into Shift-JIS and UTF-8: control characters
 *             and characters without a code left out, a character beyond
 *             the Basic Multilingual Plane as a surrogate pair; the length
 *             of a character in either encoding
 *   field   - the text cut to the limit by whole characters; the editing
 *             keys by whole characters, the selection all or nothing; typed
 *             text replacing the selection, cut to the limit, refused by a
 *             "B0 29" field; the keys that are not the field's
 *   dialog  - the input dialogs as the original lays them out: Tab, Enter,
 *             Escape and the close box; a digits-only field, the number
 *             pickers and their range; the profile's complaint about a
 *             half-width name and the day range after the month; a list
 *   box     - the message box's buttons, default and cancel button for
 *             each type, its width, the icon, the text wrapped to the width
 *             allowed by whole characters and at spaces
 *   pointer - a button fires when released over itself, not elsewhere; a
 *             click focuses a field; the wheel scrolls a list
 *   settings - the display settings: four pickers that only step - by a
 *             click, the wheel, the arrows and Space - wrapping at the
 *             ends, telling the handler; nothing is typed into them; the
 *             reset button's handler refuses, Close and Escape end it
 *   paint   - the face, a field's caret where the font puts it, the
 *             default button's outline; the frame's caption bar, close box
 *             and what a point of it is
 */
#include "bgi/ui.h"
#include "host/os.h"
#include "bgi/msg.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

// ---- the test's font: 8 pixels a byte, a 6 x 10 block for each ----------------------------

static int FakeDraw(void* ctx, uint32_t* pix, int pitch, int w, int h, int x, int y, const char* text, uint32_t rgb)
{
	BGI_UNUSED(ctx);
	for(; *text; text++, x += 8)
		Ui_FillRect(pix, pitch, w, h, x + 1, y + 3, 6, 10, rgb);
	return x;
}

static int FakeWidth(void* ctx, const char* text, int bytes)
{
	int n = (int)strlen(text);
	BGI_UNUSED(ctx);
	return 8 * (bytes >= 0 && bytes < n ? bytes : n);
}

static const UiFont_t kFont = {NULL, UI_FONT_H, FakeDraw, FakeWidth};

// a dialog zeroed with the test's font
static void Begin(UiDialog_t* d)
{
	memset(d, 0, sizeof *d);
	d->font = &kFont;
}

// ---- the groups -----------------------------------------------------------------------

static void TestText(void)
{
	char out[64];
	printf("text\n");
	// "aあ\tb" with a character Shift-JIS lacks (U+0E01, Thai) and one beyond the BMP (U+1F600)
	CHECK(Ui_FromUtf8("a\xe3\x81\x82\tb\xe0\xb8\x81\xf0\x9f\x98\x80", out, sizeof out, 0) == 4 && memcmp(out, "a\202\240b", 5) == 0);
	CHECK(Ui_FromUtf8("a\xf0\x9f\x98\x80", out, sizeof out, 1) == 7 && memcmp(out, "a\xed\xa0\xbd\xed\xb8\x80", 8) == 0);
	CHECK(Ui_FromUtf8("\177\001\377c", out, sizeof out, 1) == 1 && strcmp(out, "c") == 0); // control characters, a malformed byte
	CHECK(Ui_FromUtf8("\xe3\x81\x82\xe3\x81\x84", out, 4, 0) == 2);                        // whole characters only
	CHECK(Ui_CharLen("\x82\xa0", 0) == 2 && Ui_CharLen("\x82", 0) == 1 && Ui_CharLen("\xb1", 0) == 1 && Ui_CharLen("", 0) == 0);
	CHECK(Ui_CharLen("\xe3\x81\x82", 1) == 3 && Ui_CharLen("\xc3\xa9", 1) == 2 && Ui_CharLen("\xe3\x81", 1) == 1);
}

static void TestField(void)
{
	UiField_t f;
	printf("field\n");
	memset(&f, 0, sizeof f);
	f.limit = 5;
	UiField_SetText(&f, "ab\x82\xa0\x82\xa2"); // a, b, あ, い: い does not fit
	CHECK(strcmp(f.text, "ab\x82\xa0") == 0 && f.caret == 4 && !f.selAll);
	f.limit = 0;
	UiField_SetText(&f, "ab\x82"); // a lone lead byte at the end goes
	CHECK(strcmp(f.text, "ab") == 0);
	f.utf8 = 1;
	f.limit = 4;
	UiField_SetText(&f, "a\xe3\x81\x82\xe3\x81\x84"); // a, あ: い does not fit
	CHECK(strcmp(f.text, "a\xe3\x81\x82") == 0);
	f.limit = 0;
	UiField_SetText(&f, "a\xe3\x81"); // a cut-off sequence goes
	CHECK(strcmp(f.text, "a") == 0);

	// the keys by whole characters
	f.utf8 = 0;
	UiField_SetText(&f, "a\202\240b");
	CHECK(UiField_Key(&f, 0x25, 0, 0) == 0 && f.caret == 3); // Left over b
	CHECK(UiField_Key(&f, 0x25, 0, 0) == 0 && f.caret == 1); // over あ
	CHECK(UiField_Key(&f, 0x2e, 0, 0) == 1 && strcmp(f.text, "ab") == 0 && f.caret == 1);
	CHECK(UiField_Key(&f, 0x24, 0, 0) == 0 && f.caret == 0);              // Home
	CHECK(UiField_Key(&f, 0x08, 0, 0) == 0 && strcmp(f.text, "ab") == 0); // nothing before the caret
	CHECK(UiField_Key(&f, 0x23, 0, 0) == 0 && f.caret == 2);              // End
	CHECK(UiField_Key(&f, 0x08, 0, 0) == 1 && strcmp(f.text, "a") == 0);
	CHECK(UiField_Key(&f, 'A', 1, 0) == 0 && f.selAll);                   // Ctrl+A
	CHECK(UiField_Key(&f, 0x27, 0, 0) == 0 && !f.selAll && f.caret == 1); // Right drops the selection
	UiField_SelectAll(&f);
	CHECK(UiField_Key(&f, 0x2e, 0, 0) == 1 && f.text[0] == 0 && !f.selAll); // Delete over it
	UiField_SelectAll(&f);
	CHECK(!f.selAll); // nothing to select
	CHECK(UiField_Key(&f, 0x09, 0, 0) == -1 && UiField_Key(&f, 0x0d, 0, 0) == -1 && UiField_Key(&f, 0x1b, 0, 0) == -1);
	CHECK(UiField_Key(&f, 'C', 1, 0) == -1 && UiField_Key(&f, 'X', 0, 1) == -1); // other Ctrl / Alt combinations
	CHECK(UiField_Key(&f, 'Q', 0, 0) == UI_FIELD_TYPES && UiField_Key(&f, 0, 0, 0) == UI_FIELD_TYPES);

	// typing
	UiField_SetText(&f, "xy");
	UiField_SelectAll(&f);
	CHECK(UiField_Type(&f, "\xe3\x81\x82") == 1 && strcmp(f.text, "\x82\xa0") == 0 && f.caret == 2); // replaces the selection
	f.limit = 5;
	CHECK(UiField_Type(&f, "a\xe3\x81\x84\xe3\x81\x86") == 1 && strcmp(f.text, "\202\240a\202\242") == 0); // う does not fit
	CHECK(UiField_Type(&f, "b") == 0);                                                                     // full
	f.limit = 0;
	f.noAscii = 1;
	CHECK(UiField_Type(&f, "z") == 0 && UiField_Type(&f, "\x01") == 0);            // "B0 29" refuses a single-byte printable character
	CHECK(UiField_Type(&f, "\xef\xbd\xb1") == 1 && f.text[f.caret - 1] == '\xb1'); // half-width katakana is not ASCII
	f.noAscii = 0;
	f.caret = 0;
	CHECK(UiField_Type(&f, "q") == 1 && f.text[0] == 'q' && f.caret == 1); // at the caret
	f.utf8 = 1;
	UiField_SetText(&f, "");
	CHECK(UiField_Type(&f, "\xe3\x81\x82") == 1 && strcmp(f.text, "\xe3\x81\x82") == 0 && f.caret == 3);
	CHECK(UiField_Key(&f, 0x25, 0, 0) == 0 && f.caret == 0);
}

static void TestDialog(void)
{
	UiDialog_t d;
	int r;
	char s[0x100], g[0x100], n[0x100], p[0x100];
	int32_t month = -1, day = -1;
	printf("dialog\n");

	// 0x71: the field, OK / Cancel; Enter confirms
	Begin(&d);
	Ui_BuildInput1(&d, NULL, "ab", 4, 0, 0);
	CHECK(d.nFields == 1 && d.nButtons == 2 && d.focus == 0 && !d.fields[0].f.selAll && d.fields[0].f.caret == 2);
	CHECK(strcmp(d.title, MSG_DLG_COMMENT_INPUT) == 0 && d.w == 344 && d.h == 12 + 22 + 12 + 24 + 12);
	CHECK(Ui_Key(&d, 'C', 0, 0, 0) == UI_KEY_TYPE);
	CHECK(Ui_Type(&d, 'C', "cde") == UI_KEY_DONE && strcmp(d.fields[0].f.text, "abcd") == 0 && d.redraw); // the limit
	CHECK(Ui_Key(&d, 0x0d, 0, 0, 0) == UI_KEY_DONE && d.done && d.result == 1);
	CHECK(Ui_Key(&d, 0x1b, 0, 0, 0) == UI_KEY_NONE && d.result == 1); // nothing after the end

	// Tab to Cancel, Space; Escape; the close box
	Begin(&d);
	Ui_BuildInput1(&d, "t", "", 0, 0, 0);
	CHECK(strcmp(d.title, "t") == 0);
	Ui_Key(&d, 0x09, 0, 0, 0);
	Ui_Key(&d, 0x09, 0, 0, 0);
	CHECK(d.focus == 2 && Ui_FocusedField(&d) == -1);
	CHECK(Ui_Key(&d, 'Q', 0, 0, 0) == UI_KEY_NONE && Ui_Type(&d, 'Q', "q") == UI_KEY_NONE); // a button takes no text
	Ui_Key(&d, 0x25, 0, 0, 0);                                                              // Left: OK
	CHECK(d.focus == 1);
	Ui_Key(&d, 0x09, 1, 0, 0); // Shift+Tab: the field, its text selected
	CHECK(d.focus == 0);
	Ui_Key(&d, 0x09, 0, 0, 0);
	Ui_Key(&d, 0x27, 0, 0, 0);
	CHECK(Ui_Key(&d, 0x20, 0, 0, 0) == UI_KEY_DONE && d.done && d.result == 0);
	Begin(&d);
	Ui_BuildInput1(&d, "t", "", 0, 0, 0);
	Ui_Key(&d, 0x1b, 0, 0, 0);
	CHECK(d.done && d.result == 0);
	Begin(&d);
	Ui_BuildInput1(&d, "t", "", 0, 0, 0);
	Ui_Close(&d);
	CHECK(d.done && d.result == 0);

	// 0x72: the focus on the first field without an initial text; digits only
	Begin(&d);
	Ui_BuildInput2(&d, NULL, "L1", "x", 0, 0, NULL, NULL, 0, 1, 0, 0);
	CHECK(d.nFields == 2 && d.nLabels == 2 && d.focus == 1 && strcmp(d.labels[1].text, MSG_DLG_STRING2) == 0);
	CHECK(Ui_Type(&d, 0, "-") == UI_KEY_DONE && strcmp(d.fields[1].f.text, "-") == 0);    // a minus into the empty field
	CHECK(Ui_Type(&d, 0, "1a-2") == UI_KEY_DONE && strcmp(d.fields[1].f.text, "-") == 0); // anything but digits swallows the lot
	Ui_Type(&d, 0, "12");
	CHECK(strcmp(d.fields[1].f.text, "-12") == 0);
	Ui_Key(&d, 0x08, 0, 0, 0);
	CHECK(strcmp(d.fields[1].f.text, "-1") == 0);
	Begin(&d);
	Ui_BuildInput2(&d, NULL, NULL, "a", 0, 0, NULL, "b", 0, 0, 1, 0);
	CHECK(d.focus == 0 && d.fields[1].y == d.fields[0].y && d.fields[1].x > d.fields[0].x); // side by side

	// 0x77: a half-width name is refused with a complaint; the day follows the month
	Begin(&d);
	Ui_BuildProfile(&d, "\x82\xa0", "a", "", "", 1, 30, 0);
	CHECK(d.nFields == 6 && d.escButton == -1 && d.focus == d.nFields);
	CHECK(strcmp(d.fields[d.aux[0]].f.text, "2") == 0 && strcmp(d.fields[d.aux[1]].f.text, "31") == 0);
	Ui_Key(&d, 0x0d, 0, 0, 0);
	CHECK(!d.done && d.complaint[0] && strstr(d.complaint, MSG_DLG_GIVEN_NAME) && strcmp(d.complaintCaption, MSG_DLG_INPUT_ERROR) == 0 &&
		d.complaintFlags == OPENBGI_MB_ICONHAND && d.redraw);
	d.complaint[0] = 0;
	UiField_SetText(&d.fields[1].f, "\x82\xa2");
	Ui_Close(&d); // no cancel button: nothing
	CHECK(!d.done);
	d.focus = d.aux[0];
	CHECK(Ui_Key(&d, 0x26, 0, 0, 0) == UI_KEY_DONE && strcmp(d.fields[d.aux[0]].f.text, "3") == 0); // March
	CHECK(strcmp(d.fields[d.aux[1]].f.text, "31") == 0 && d.fields[d.aux[1]].max == 31);
	Ui_Key(&d, 0x26, 0, 0, 0); // April: 30 days, the 31st becomes the 1st
	CHECK(d.fields[d.aux[1]].max == 30 && strcmp(d.fields[d.aux[1]].f.text, "1") == 0);
	d.focus = d.aux[1];
	Ui_Key(&d, 0x28, 0, 0, 0); // down from 1 wraps to 30
	CHECK(strcmp(d.fields[d.aux[1]].f.text, "30") == 0);
	Ui_Key(&d, 0x08, 0, 0, 0);
	Ui_Type(&d, 0, "9"); // 39 is past the range
	CHECK(strcmp(d.fields[d.aux[1]].f.text, "30") == 0);
	d.focus = d.aux[0];
	UiField_SelectAll(&d.fields[d.aux[0]].f);
	Ui_Type(&d, 0, "x"); // no digit to go by yet: left while typing
	CHECK(strcmp(d.fields[d.aux[0]].f.text, "x") == 0);
	UiField_SelectAll(&d.fields[d.aux[0]].f);
	Ui_Type(&d, 0, "13"); // kept within the range
	CHECK(strcmp(d.fields[d.aux[0]].f.text, "12") == 0);
	d.focus = d.nFields;
	Ui_Key(&d, 0x0d, 0, 0, 0);
	CHECK(d.done && d.result == 1);
	Ui_ProfileResult(&d, s, g, n, p, &month, &day);
	CHECK(strcmp(s, "\x82\xa0") == 0 && strcmp(g, "\x82\xa2") == 0 && !n[0] && !p[0] && month == 11 && day == 29);
	// UTF-8: ASCII and half-width katakana are half width, the rest full
	Begin(&d);
	Ui_BuildProfile(&d, "\xe3\x81\x82", "\xef\xbd\xb1", "", "", 0, 0, 1);
	Ui_Key(&d, 0x0d, 0, 0, 0);
	CHECK(!d.done && d.complaint[0]);
	d.complaint[0] = 0;
	UiField_SetText(&d.fields[1].f, "\xef\xbc\xa1"); // Ａ
	Ui_Key(&d, 0x0d, 0, 0, 0);
	CHECK(d.done && !d.complaint[0]);

	// a list: the keys move the selection and scroll it into view
	{
		static const char* const kItems[5] = {"a", "b", "c", "d", "e"};
		int li;
		Begin(&d);
		d.w = d.h = 200;
		li = Ui_ListAdd(&d, 0, 0, 100, 2, kItems, 5, 0);
		Ui_ButtonAdd(&d, 0, 150, "OK", 7);
		d.defButton = 0;
		d.escButton = -1;
		d.focus = d.nFields + d.nButtons + li;
		Ui_Key(&d, 0x28, 0, 0, 0);
		Ui_Key(&d, 0x28, 0, 0, 0);
		CHECK(d.lists[li].selected == 2 && d.lists[li].top == 1);
		Ui_Key(&d, 0x23, 0, 0, 0);
		CHECK(d.lists[li].selected == 4 && d.lists[li].top == 3);
		Ui_Key(&d, 0x21, 0, 0, 0);
		CHECK(d.lists[li].selected == 2 && d.lists[li].top == 2);
		CHECK(Ui_Key(&d, 'Q', 0, 0, 0) == UI_KEY_NONE);
		r = d.lists[li].h;
		CHECK(r == 2 * UI_LIST_ROW_H + 4);
	}
}

static void TestBox(void)
{
	UiDialog_t d;
	int i;
	printf("box\n");
	Begin(&d);
	Ui_BuildMessageBox(&d, "hi", "cap", OPENBGI_MB_OK, 0, 0);
	CHECK(d.nButtons == 1 && d.buttons[0].id == OPENBGI_IDOK && d.escButton == 0 && d.defButton == 0 && d.result == OPENBGI_IDOK);
	CHECK(d.w == 220 && d.icon == 0 && strcmp(d.title, "cap") == 0 && d.nLabels == 1 && d.labels[0].x == UI_MARGIN);
	Begin(&d);
	Ui_BuildMessageBox(&d, "q", NULL, OPENBGI_MB_YESNO | OPENBGI_MB_ICONQUESTION | OPENBGI_MB_DEFBUTTON2, 0, 0);
	CHECK(d.nButtons == 2 && d.buttons[0].id == OPENBGI_IDYES && d.buttons[1].id == OPENBGI_IDNO && d.escButton == -1 && d.defButton == 1);
	CHECK(d.result == OPENBGI_IDNO && d.focus == 1 && d.icon == '?' && d.labels[0].x == 2 * UI_MARGIN + 32 && d.title[0] == 0);
	Ui_Close(&d);
	CHECK(!d.done); // Yes / No has no close box
	Ui_Key(&d, 0x0d, 0, 0, 0);
	CHECK(d.done && d.result == OPENBGI_IDNO);
	Begin(&d);
	Ui_BuildMessageBox(&d, "r", "", OPENBGI_MB_RETRYCANCEL | OPENBGI_MB_ICONHAND, 0, 0);
	CHECK(d.buttons[0].id == OPENBGI_IDRETRY && d.buttons[1].id == OPENBGI_IDCANCEL && d.escButton == 1 && d.icon == 'x');
	Begin(&d);
	Ui_BuildMessageBox(&d, "o", "", OPENBGI_MB_OKCANCEL | OPENBGI_MB_ICONWARNING, 0, 0);
	CHECK(d.buttons[1].id == OPENBGI_IDCANCEL && d.escButton == 1 && d.icon == '!');

	// the lines at their breaks, '\r' dropped; a wide one sets the width
	Begin(&d);
	Ui_BuildMessageBox(&d, "one\r\n0123456789012345678901234567890123456789\n\nlast", "", OPENBGI_MB_OK, 0, 0);
	CHECK(d.nLabels == 4 && strcmp(d.labels[0].text, "one") == 0 && d.labels[2].text[0] == 0 && strcmp(d.labels[3].text, "last") == 0);
	CHECK(d.w == UI_MARGIN + 40 * 8 + UI_MARGIN);

	// wrapped to 200 pixels: 176 for the text, 22 bytes a line; at spaces, else by characters
	Begin(&d);
	Ui_BuildMessageBox(&d, "aaaa bbbb cccc dddd eeee ffff", "", OPENBGI_MB_OK, 0, 200);
	CHECK(d.nLabels == 2 && strcmp(d.labels[0].text, "aaaa bbbb cccc dddd") == 0 && strcmp(d.labels[1].text, "eeee ffff") == 0);
	Begin(&d);
	Ui_BuildMessageBox(&d, "\x82\xa0\x82\xa0\x82\xa0\x82\xa0\x82\xa0\x82\xa0\x82\xa0\x82\xa0\x82\xa0\x82\xa0\x82\xa0\x82\xa0", "", OPENBGI_MB_OK, 0, 200);
	CHECK(d.nLabels == 2 && strlen(d.labels[0].text) == 22 && strlen(d.labels[1].text) == 2); // 11 + 1 characters, none split
	for(i = 0; i < d.nLabels; i++)
		CHECK(Ui_TextWidth(&d, d.labels[i].text) <= 176);
	Begin(&d);
	{
		char many[200];
		memset(many, '\n', 40);
		many[40] = 0;
		Ui_BuildMessageBox(&d, many, "", OPENBGI_MB_OK, 0, 0);
		CHECK(d.nLabels == 16); // at most 16 lines
	}
}

static void TestPointer(void)
{
	UiDialog_t d;
	UiButton_t* ok;
	printf("pointer\n");
	Begin(&d);
	Ui_BuildInput1(&d, "t", "abc", 0, 0, 0);
	CHECK(Ui_Alloc(&d) && d.pressed == -1);
	ok = &d.buttons[0];
	Ui_Press(&d, ok->x + 2, ok->y + 2, 1000);
	CHECK(d.pressed == 0 && d.focus == 1 && !d.done);
	Ui_Release(&d, 0, 0); // elsewhere: nothing
	CHECK(d.pressed == -1 && !d.done);
	Ui_Press(&d, d.fields[0].x + 3, d.fields[0].y + 3, 1100);
	CHECK(d.focus == 0 && d.fields[0].f.caret == 3 && !d.fields[0].f.selAll);
	Ui_Press(&d, ok->x + 2, ok->y + 2, 1200);
	Ui_Release(&d, ok->x + ok->w - 1, ok->y + ok->h - 1);
	CHECK(d.done && d.result == 1);
	Ui_Free(&d);
	CHECK(d.pix == NULL);

	// the list: a click selects, a second click soon after activates the default button, the wheel scrolls
	{
		static const char* const kItems[6] = {"a", "b", "c", "d", "e", "f"};
		UiList_t* l;
		Begin(&d);
		d.w = d.h = 200;
		Ui_ListAdd(&d, 10, 10, 100, 3, kItems, 6, -1);
		Ui_ButtonAdd(&d, 10, 150, "OK", 9);
		d.defButton = 0;
		d.escButton = -1;
		l = &d.lists[0];
		Ui_Press(&d, 20, 10 + 2 + UI_LIST_ROW_H + 1, 5000);
		CHECK(l->selected == 1 && !d.done);
		Ui_Press(&d, 20, 10 + 2 + UI_LIST_ROW_H + 1, 5900); // too late for a double click
		CHECK(!d.done);
		Ui_Wheel(&d, 20, 20, -1);
		CHECK(l->top == 3);
		Ui_Wheel(&d, 20, 20, 5);
		CHECK(l->top == 0);
		Ui_Wheel(&d, 150, 20, -1); // not over the list
		CHECK(l->top == 0);
		Ui_Press(&d, 20, 10 + 2 + UI_LIST_ROW_H + 1, 6100);
		CHECK(d.done && d.result == 9);
	}
}

static uint32_t Px(const UiDialog_t* d, int x, int y)
{
	return d->pix[(size_t)y * d->w + x];
}

static int gChanged, gChangedField, gResets;

static void OnChange(UiDialog_t* d, int field, void* ctx)
{
	BGI_UNUSED(d);
	BGI_UNUSED(ctx);
	gChanged++;
	gChangedField = field;
}

static int OnButton(UiDialog_t* d, int id, void* ctx)
{
	BGI_UNUSED(d);
	BGI_UNUSED(ctx);
	if(id != UI_SETTINGS_RESET)
		return 1;
	gResets++;
	return 0; // the box stays
}

static void TestSettings(void)
{
	UiDialog_t d;
	UiFieldW_t* f;
	printf("settings\n");
	Begin(&d);
	Ui_BuildDisplaySettings(&d, 2, NULL, 0, 1, -1, 0, 1, 0);
	d.onChange = OnChange;
	d.onButton = OnButton;
	CHECK(Ui_Alloc(&d) && d.nFields == 4 && d.nButtons == 2 && d.focus == 0 && UI_SETTINGS_QUALITY(&d) == -1);
	CHECK(Ui_PickValue(&d, UI_SETTINGS_SIZE) == 2 && Ui_PickValue(&d, UI_SETTINGS_FILTER) == 1);
	CHECK(Ui_PickValue(&d, UI_SETTINGS_WHOLE(&d)) == 0 && Ui_PickValue(&d, UI_SETTINGS_FULL(&d)) == 1);
	CHECK(strcmp(d.fields[0].f.text, "150 %") == 0 && d.fields[0].pick && d.fields[3].pick);
	// a click steps a picker on, past its last text to its first; the handler hears which
	f = &d.fields[UI_SETTINGS_FILTER];
	Ui_Press(&d, f->x + 3, f->y + 3, 100);
	Ui_Release(&d, f->x + 3, f->y + 3);
	CHECK(d.focus == UI_SETTINGS_FILTER && Ui_PickValue(&d, UI_SETTINGS_FILTER) == 2 && gChanged == 1 && gChangedField == UI_SETTINGS_FILTER);
	Ui_Press(&d, f->x + 3, f->y + 3, 200);
	Ui_Press(&d, f->x + 3, f->y + 3, 300);
	CHECK(Ui_PickValue(&d, UI_SETTINGS_FILTER) == 0 && gChanged == 3);
	// the wheel over one: back a text a notch away from the player, on a text towards
	f = &d.fields[UI_SETTINGS_SIZE];
	Ui_Wheel(&d, f->x + 3, f->y + 3, 1);
	CHECK(d.focus == UI_SETTINGS_SIZE && Ui_PickValue(&d, UI_SETTINGS_SIZE) == 1);
	Ui_Wheel(&d, f->x + 3, f->y + 3, -2);
	CHECK(Ui_PickValue(&d, UI_SETTINGS_SIZE) == 3);
	// the arrows and Space; a letter types nothing, and is taken
	CHECK(Ui_Key(&d, 0x28, 0, 0, 0) == UI_KEY_DONE && Ui_PickValue(&d, UI_SETTINGS_SIZE) == 4);
	CHECK(Ui_Key(&d, 0x27, 0, 0, 0) == UI_KEY_DONE && Ui_PickValue(&d, UI_SETTINGS_SIZE) == 0);
	CHECK(Ui_Key(&d, 0x25, 0, 0, 0) == UI_KEY_DONE && Ui_PickValue(&d, UI_SETTINGS_SIZE) == 4);
	CHECK(Ui_Key(&d, 0x26, 0, 0, 0) == UI_KEY_DONE && Ui_PickValue(&d, UI_SETTINGS_SIZE) == 3);
	CHECK(Ui_Key(&d, 0x20, 0, 0, 0) == UI_KEY_DONE && Ui_PickValue(&d, UI_SETTINGS_SIZE) == 4);
	CHECK(Ui_Key(&d, 'A', 0, 0, 0) == UI_KEY_DONE && Ui_Type(&d, 'A', "a") == UI_KEY_DONE);
	CHECK(strcmp(d.fields[0].f.text, "Fit the screen") == 0);
	Ui_PickSet(&d, UI_SETTINGS_FULL(&d), 0);
	Ui_PickSet(&d, UI_SETTINGS_FULL(&d), 7); // (no such text: left)
	CHECK(Ui_PickValue(&d, UI_SETTINGS_FULL(&d)) == 0);
	// "Reset window size" is refused by its handler; Enter and Escape are Close
	Ui_Press(&d, d.buttons[0].x + 2, d.buttons[0].y + 2, 400);
	Ui_Release(&d, d.buttons[0].x + 2, d.buttons[0].y + 2);
	CHECK(gResets == 1 && !d.done && d.buttons[0].w > UI_BTN_W);
	Ui_Key(&d, 0x1b, 0, 0, 0);
	CHECK(d.done && d.result == 0);
	Ui_Free(&d);
	// with filters of the host's own: their names after the four, and a picker for their quality after the filter's
	{
		static const char* const kFilters[6] = {"The game's", "Bicubic", "Bilinear", "Nearest", "Sharp A", "Sharp B"};
		Begin(&d);
		Ui_BuildDisplaySettings(&d, 0, kFilters, 6, 5, 1, 1, 0, 0);
		CHECK(Ui_Alloc(&d) && d.nFields == 5 && UI_SETTINGS_QUALITY(&d) == 2 && UI_SETTINGS_WHOLE(&d) == 3 && UI_SETTINGS_FULL(&d) == 4);
		CHECK(Ui_PickValue(&d, UI_SETTINGS_FILTER) == 5 && strcmp(d.fields[1].f.text, "Sharp B") == 0);
		CHECK(Ui_PickValue(&d, 2) == 1 && strcmp(d.fields[2].f.text, "High") == 0 && Ui_PickValue(&d, 3) == 1 && Ui_PickValue(&d, 4) == 0);
		f = &d.fields[UI_SETTINGS_FILTER];
		Ui_Press(&d, f->x + 3, f->y + 3, 500);
		CHECK(Ui_PickValue(&d, UI_SETTINGS_FILTER) == 0); // (past the last one: the first)
		Ui_Free(&d);
	}
}

static void TestPaint(void)
{
	UiDialog_t d;
	uint32_t frame[400 * 200];
	int fw, fh, cx, cy;
	printf("paint\n");
	Begin(&d);
	Ui_BuildInput1(&d, "t", "abc", 0, 0, 0);
	CHECK(Ui_Alloc(&d));
	Ui_Draw(&d);
	CHECK(Px(&d, 0, 0) == UI_FACE && Px(&d, d.w - 1, d.h - 1) == UI_FACE);
	{
		UiFieldW_t* f = &d.fields[0];
		int ty = f->y + (f->h - UI_FONT_H) / 2;
		CHECK(Px(&d, f->x, f->y) == UI_SHADOW && Px(&d, f->x + f->w - 1, f->y + 5) == UI_LIGHT);
		CHECK(Px(&d, f->x + 3 + 2 + 1, ty + 3) == UI_TEXT);          // the first character's block
		CHECK(Px(&d, f->x + 3 + 2 + 24, ty + 1) == UI_TEXT);         // the caret after three characters
		CHECK(Px(&d, f->x + 3 + 2 + 24, ty + 1 - 2) == UI_FIELD_BG); // (only as high as the font)
	}
	{
		UiButton_t* ok = &d.buttons[0];
		CHECK(Px(&d, ok->x - 1, ok->y - 1) == UI_FRAME && Px(&d, ok->x, ok->y) == UI_LIGHT && Px(&d, ok->x + ok->w - 1, ok->y + 3) == UI_DARK);
		d.pressed = 0;
		Ui_Draw(&d);
		CHECK(Px(&d, ok->x, ok->y) == UI_DARK);
	}

	// the frame: the size, the parts, the client copied
	Ui_FrameSize(&d, &fw, &fh);
	CHECK(fw == d.w + 2 * UI_BORDER && fh == d.h + UI_CAPTION_H + 2 * UI_BORDER && fw * fh <= 400 * 200);
	Ui_FrameDraw(&d, frame, fw, 0);
	CHECK(frame[(UI_BORDER + UI_CAPTION_H) * fw + UI_BORDER] == Px(&d, 0, 0));
	CHECK(frame[(size_t)(fh - 1) * fw + fw - 1] == 0x404040 && frame[1 * fw + 1] == UI_LIGHT);
	CHECK(frame[(UI_BORDER + 2) * fw + UI_BORDER] == 0x0a246a); // the caption bar's left end
	CHECK(Ui_FrameHit(&d, UI_BORDER + 5, UI_BORDER + UI_CAPTION_H + 7, &cx, &cy) == UI_HIT_CLIENT && cx == 5 && cy == 7);
	CHECK(Ui_FrameHit(&d, 30, UI_BORDER + 3, &cx, &cy) == UI_HIT_CAPTION);
	CHECK(Ui_FrameHit(&d, UI_BORDER + d.w - 10, UI_BORDER + UI_CAPTION_H / 2, &cx, &cy) == UI_HIT_CLOSE);
	CHECK(Ui_FrameHit(&d, 0, 0, &cx, &cy) == UI_HIT_NONE && Ui_FrameHit(&d, fw, 5, &cx, &cy) == UI_HIT_NONE);
	d.escButton = -1; // no cancel button: no close box
	CHECK(Ui_FrameHit(&d, UI_BORDER + d.w - 10, UI_BORDER + UI_CAPTION_H / 2, &cx, &cy) == UI_HIT_CAPTION);
	Ui_Free(&d);
}

int main(void)
{
	TestText();
	TestField();
	TestDialog();
	TestBox();
	TestPointer();
	TestSettings();
	TestPaint();
	if(gFails)
	{
		printf("ui: %d checks failed\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
