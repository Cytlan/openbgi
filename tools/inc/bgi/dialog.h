/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dialog.h - the script's modal dialogs of "B0 84", "B0 85" and "B0 8F"
 *            and the desktop wallpaper of "B0 F0" (src/sys/dialog.c)
 *
 * The dialogs are resources of the original (templates 0x71, 0x72, 0x77)
 * driven by DialogBoxParam; the OS layer provides them (OS_Dialog*).  Each
 * call clears the key states afterwards (Input_ClearStates) so that the
 * Enter that closed the dialog is not seen by the script.
 */
#ifndef BGI_DIALOG_H_
#define BGI_DIALOG_H_

#include "bgi/common.h"

#define DIALOG_TEXT_MAX 0x100 // GetDlgItemText limit of the string fields, bytes

/* "B0 84": one text field; `out` (0x100 bytes) receives the entry when
 * confirmed.  NULL title / initial text are allowed (title: "comment
 * input").  1 ok, 0 cancelled. */
int Dialog_Input1(char* out, const char* title, const char* initial, int maxLen);
/* "B0 85": two labelled text fields (defaults: title "data input", labels
 * "first string" / "second string").  1 ok, 0 cancelled. */
int Dialog_Input2(char* out1, char* out2, const char* title, const char* label1, const char* initial1, int maxLen1,
	const char* label2, const char* initial2, int maxLen2);
/* "B0 8F": the player profile - surname, given name, nickname, first
 * person pronoun (0x100 byte buffers holding the initial texts, at most 10
 * bytes are used) and a birthday as month / day indices (0 based, in and
 * out).  1 ok (the dialog has no cancel). */
int Dialog_Profile(char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month, int32_t* day);

// "B0 F0": the desktop wallpaper from an image file, stretched or tiled; returns the OS result
int SetWallpaper(const char* path, int stretch, int tile);

#endif // BGI_DIALOG_H_
