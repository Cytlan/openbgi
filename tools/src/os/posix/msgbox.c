/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * msgbox.c - MessageBoxA for the POSIX back end (inc/bgi/os.h), built on
 *            the dialog toolkit; without a display (an error before the
 *            window exists, a headless run) the text goes to stderr and
 *            the default button is returned
 */
#include "ui_internal.h"

/* the message box: the buttons of the type (OK, OK / Cancel, Yes / No,
 * Retry / Cancel), the icon as a coloured disc with a letter, the text
 * split at its line breaks (at most 16 lines), laid out around the
 * longer of the text and the button row; the id of the button pressed */
int OS_MessageBox(const char* text, const char* caption, uint32_t flags)
{
	UiDialog_t d;
	char lines[16][0x200];
	int nLines = 0, i, maxW = 0, textX, y, btnY, totalBtnW, bx;
	const char* btnText[3];
	int btnId[3], nBtn = 0, defBtn;
	uint32_t type = flags & 0xf, icon = flags & 0xf0; // the MB_* button and icon fields

	switch(type)
	{
		case OS_MB_OKCANCEL:
			btnText[0] = "OK", btnId[0] = OS_IDOK;
			btnText[1] = "Cancel", btnId[1] = OS_IDCANCEL;
			nBtn = 2;
			break;
		case OS_MB_YESNO:
			btnText[0] = "Yes", btnId[0] = OS_IDYES;
			btnText[1] = "No", btnId[1] = OS_IDNO;
			nBtn = 2;
			break;
		case OS_MB_RETRYCANCEL:
			btnText[0] = "Retry", btnId[0] = OS_IDRETRY;
			btnText[1] = "Cancel", btnId[1] = OS_IDCANCEL;
			nBtn = 2;
			break;
		default:
			btnText[0] = "OK", btnId[0] = OS_IDOK;
			nBtn = 1;
			break;
	}
	defBtn = (flags & OS_MB_DEFBUTTON2) && nBtn > 1 ? 1 : 0;

	if(!X11_Open() || getenv("BGI_MSGBOX_STDERR"))
	{ // headless (or asked to with BGI_MSGBOX_STDERR, for unattended test runs): say it on stderr, take the default button
		char utf8[0x800];
		OsPosix_SjisToUtf8(caption ? caption : "", utf8, sizeof utf8);
		fprintf(stderr, "[%s] ", utf8);
		OsPosix_SjisToUtf8(text ? text : "", utf8, sizeof utf8);
		fprintf(stderr, "%s\n", utf8);
		return btnId[defBtn];
	}

	// split the text into lines at '\n' ('\r' dropped), keeping double-byte characters whole
	{
		const char* p = text ? text : "";
		while(*p && nLines < 16)
		{
			int n = 0;
			while(*p && *p != '\n' && n < (int)sizeof lines[0] - 2)
			{
				if(*p == '\r')
				{
					p++;
					continue;
				}
				if(OsCommon_SjisIsLead((uint8_t)*p) && p[1])
				{
					lines[nLines][n++] = *p++;
					lines[nLines][n++] = *p++;
				}
				else
					lines[nLines][n++] = *p++;
			}
			lines[nLines][n] = 0;
			nLines++;
			if(*p == '\n')
				p++;
		}
		if(nLines == 0)
		{
			lines[0][0] = 0;
			nLines = 1;
		}
	}
	for(i = 0; i < nLines; i++)
	{
		int w = Ui_TextWidth(lines[i]);
		if(w > maxW)
			maxW = w;
	}

	memset(&d, 0, sizeof d);
	switch(icon)
	{ // the icon's letter and disc colour: red error, blue question and information, amber warning
		case OS_MB_ICONHAND: d.icon = 'x', d.iconRgb = 0xd02020; break;
		case OS_MB_ICONQUESTION: d.icon = '?', d.iconRgb = 0x2060d0; break;
		case OS_MB_ICONWARNING: d.icon = '!', d.iconRgb = 0xe0a000; break;
		case OS_MB_ICONINFO: d.icon = 'i', d.iconRgb = 0x2060d0; break;
		default: break;
	}
	// layout: the text right of the 32-pixel icon, the buttons centred below with 8 pixels between them, 220 pixels wide at least
	textX = UI_MARGIN + (d.icon ? 32 + UI_MARGIN : 0);
	totalBtnW = nBtn * UI_BTN_W + (nBtn - 1) * 8;
	d.w = textX + maxW + UI_MARGIN;
	if(d.w < totalBtnW + 2 * UI_MARGIN)
		d.w = totalBtnW + 2 * UI_MARGIN;
	if(d.w < 220)
		d.w = 220;
	y = UI_MARGIN + (d.icon && nLines * (UI_FONT_H + 2) < 32 ? (32 - nLines * (UI_FONT_H + 2)) / 2 : 0); // short text centred on the icon
	for(i = 0; i < nLines; i++)
		Ui_LabelAdd(&d, textX, y + i * (UI_FONT_H + 2), lines[i]);
	y += nLines * (UI_FONT_H + 2);
	if(d.icon && y < UI_MARGIN + 32)
		y = UI_MARGIN + 32;
	btnY = y + UI_MARGIN;
	d.h = btnY + UI_BTN_H + UI_MARGIN;
	bx = (d.w - totalBtnW) / 2;
	for(i = 0; i < nBtn; i++)
		Ui_ButtonAdd(&d, bx + i * (UI_BTN_W + 8), btnY, btnText[i], btnId[i]);
	d.defButton = defBtn;
	d.escButton = nBtn == 2 ? (type == OS_MB_YESNO ? -1 : 1) : 0; // MB_YESNO has no close box (nothing to cancel with)
	d.focus = d.nFields + defBtn;
	d.result = btnId[defBtn];
	return Ui_Run(&d, caption ? caption : "");
}
