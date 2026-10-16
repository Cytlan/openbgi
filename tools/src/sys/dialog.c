/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dialog.c - the script's modal dialogs ("B0 84" / "B0 85" / "B0 8F") and
 *            the desktop wallpaper ("B0 F0"); interface in bgi/dialog.h
 *
 * The original stores the arguments in globals, runs DialogBoxParam with
 * a dialog procedure per template and then clears the key states.  The
 * procedures only shuffle text between the arguments and the controls
 * (see the OS_Dialog* contract in os.h for what each does); the
 * reimplementation hands that to the OS layer.  The key states are
 * cleared after every dialog so that the Enter that closed it is not
 * seen by the script.
 */
#include "bgi/dialog.h"
#include "bgi/input.h"
#include "bgi/os.h"

// "B0 84": one text field; 1 ok (the entry in `out`), 0 cancelled
int Dialog_Input1(char* out, const char* title, const char* initial, int maxLen)
{
	int r = OS_DialogInput1(title, initial, maxLen, out);
	Input_ClearStates();
	return r;
}

// "B0 85": two labelled text fields; 1 ok (the entries in out1 / out2), 0 cancelled
int Dialog_Input2(char* out1, char* out2, const char* title, const char* label1, const char* initial1, int maxLen1,
	const char* label2, const char* initial2, int maxLen2)
{
	int r = OS_DialogInput2(title, label1, initial1, maxLen1, out1, label2, initial2, maxLen2, out2);
	Input_ClearStates();
	return r;
}

/* "B0 8F": the player profile - the four name buffers hold the initial
 * texts and receive the entries, month / day are 0-based indices in and
 * out.  1 ok (the dialog has no cancel). */
int Dialog_Profile(char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month,
	int32_t* day)
{
	int r = OS_DialogProfile(surname, givenName, nickname, pronoun, month, day);
	Input_ClearStates();
	return r;
}

// "B0 F0": set the desktop wallpaper to the image file at `path`; returns the OS layer's result
int SetWallpaper(const char* path, int stretch, int tile)
{
	return OS_SetWallpaper(path, stretch, tile);
}
