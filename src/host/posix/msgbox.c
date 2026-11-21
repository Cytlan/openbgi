/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * msgbox.c - MessageBoxA for the POSIX back end (src/host/os.h): the
 *            dialog toolkit's message box (Ui_BuildMessageBox) in a window
 *            of its own; without a display (an error before the window
 *            exists, a headless run) the text goes to stderr and the
 *            default button is returned
 */
#include "ui_internal.h"
#include "bgi/sjis.h"

#include <stdlib.h>

// the id of the button pressed
int OS_MessageBox(const char* text, const char* caption, uint32_t flags)
{
	UiDialog_t d;
	UiFont_t font;
	if(!X11_Open() || getenv("BGI_MSGBOX_STDERR"))
	{ // headless (or asked to with BGI_MSGBOX_STDERR, for unattended test runs): say it on stderr, take the default button
		char utf8[0x800];
		memset(&d, 0, sizeof d);
		Ui_BuildMessageBox(&d, "", NULL, flags, 0, 0); // (for its default button)
		OsCommon_SjisToUtf8(caption ? caption : "", utf8, sizeof utf8);
		fprintf(stderr, "[%s] ", utf8);
		OsCommon_SjisToUtf8(text ? text : "", utf8, sizeof utf8);
		fprintf(stderr, "%s\n", utf8);
		return d.result;
	}
	X11Ui_Begin(&d, &font);
	Ui_BuildMessageBox(&d, text, caption, flags, gOsTextUtf8, 0);
	return X11Ui_Run(&d);
}
