/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dialog.h - the script's modal dialogs of "B0 84", "B0 85" and "B0 8F"
 *            and the desktop wallpaper of "B0 F0" (src/sys/dialog.c)
 *
 * The dialogs are resources of the original (templates 0x71, 0x72, 0x77)
 * driven by DialogBoxParam; here each is a request to the host
 * (bgi/request.h, openbgi.h's OPENBGI_ASK_*), and the request's
 * answer is the dialog's result.  The buffers must last until the answer.
 * The key states are cleared after the answer (Input_ClearStates) so that
 * the Enter that closed the dialog is not seen by the script.
 */
#ifndef BGI_DIALOG_H_
#define BGI_DIALOG_H_

#include "bgi/common.h"

#define DIALOG_TEXT_MAX 0x100 // GetDlgItemText limit of the string fields, bytes

/* "B0 84": one text field; `out` (0x100 bytes) receives the entry when
 * confirmed.  NULL title / initial text are allowed (title: "comment
 * input"); from 1.616 on a negative maxLen asks for a digits-only field
 * of -maxLen characters.  1 ok, 0 cancelled. */
uint32_t Dialog_AskInput1(OpenBGI_t* o, char* out, const char* title, const char* initial, int maxLen);
/* "B0 85" / "B0 87": two labelled text fields (defaults: title "data
 * input", labels "first string" / "second string"); "B0 87" (1.573 on)
 * can place them side by side (`compact`) and make either digits-only.
 * 1 ok, 0 cancelled. */
uint32_t Dialog_AskInput2(OpenBGI_t* o, char* out1, char* out2, const char* title, const char* label1, const char* initial1, int maxLen1,
	int digits1, const char* label2, const char* initial2, int maxLen2, int digits2, int compact);
/* "B0 8F": the player profile - surname, given name, nickname, first
 * person pronoun (0x100 byte buffers holding the initial texts, at most 10
 * bytes are used) and a birthday as month / day indices (0 based, in and
 * out).  1 ok (the dialog has no cancel). */
uint32_t Dialog_AskProfile(OpenBGI_t* o, char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month, int32_t* day);

// "B0 F0": the desktop wallpaper from an image file, stretched or tiled; returns the OS result
int SetWallpaper(OpenBGI_t* o, const char* path, int stretch, int tile);

#endif // BGI_DIALOG_H_
