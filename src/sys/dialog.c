/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dialog.c - the script's modal dialogs ("B0 84" / "B0 85" / "B0 87" /
 *            "B0 8F") and the desktop wallpaper ("B0 F0"); interface in
 *            bgi/dialog.h
 *
 * The original stores the arguments in globals, runs DialogBoxParam with
 * a dialog procedure per template and then clears the key states.  The
 * procedures only shuffle text between the arguments and the controls
 * (openbgi.h says what each request does); here each
 * dialog is a request to the host (bgi/request.h, REQUEST_TREAT_DIALOG):
 * the key states are cleared after it so that the Enter that closed it is
 * not seen by the script, and from 1.547 on the engine's clock stands
 * still while it is up (Clock_BoxBegin / Clock_BoxEnd).
 */
#include "bgi/dialog.h"
#include "bgi/input.h"
#include "bgi/request.h"
#include "bgi/sys.h"
#include "bgi/fs.h"
#include "bgi/version.h"
#include "bgi/host.h"
#include "bgi/core.h"

/* "B0 84": one text field; 1 ok (the entry in `out`), 0 cancelled.  From
 * 1.616 on a negative maxLen makes a digits-only field of -maxLen
 * characters. */
uint32_t Dialog_AskInput1(OpenBGI_t* o, char* out, const char* title, const char* initial, int maxLen)
{
	OpenBGI_Request_t q;
	int digits = 0;
	if(maxLen < 0 && o->engine.profile->gen >= GEN_1_616)
	{
		digits = 1;
		maxLen = -maxLen;
	}
	memset(&q, 0, sizeof q);
	q.kind = OPENBGI_ASK_INPUT;
	q.u.input.title = title;
	q.u.input.initial = initial;
	q.u.input.maxLen = maxLen;
	q.u.input.digits = digits;
	q.u.input.out = out;
	return Request_Post(o, &q, REQUEST_TREAT_DIALOG);
}

/* "B0 85" / "B0 87": two labelled text fields, one above the other or
 * (`compact`) side by side, either digits-only; 1 ok (the entries in out1
 * / out2), 0 cancelled */
uint32_t Dialog_AskInput2(OpenBGI_t* o, char* out1, char* out2, const char* title, const char* label1, const char* initial1, int maxLen1,
	int digits1, const char* label2, const char* initial2, int maxLen2, int digits2, int compact)
{
	OpenBGI_Request_t q;
	memset(&q, 0, sizeof q);
	q.kind = OPENBGI_ASK_INPUT2;
	q.u.input2.title = title;
	q.u.input2.label[0] = label1;
	q.u.input2.label[1] = label2;
	q.u.input2.initial[0] = initial1;
	q.u.input2.initial[1] = initial2;
	q.u.input2.maxLen[0] = maxLen1;
	q.u.input2.maxLen[1] = maxLen2;
	q.u.input2.digits[0] = digits1;
	q.u.input2.digits[1] = digits2;
	q.u.input2.out[0] = out1;
	q.u.input2.out[1] = out2;
	q.u.input2.compact = compact;
	return Request_Post(o, &q, REQUEST_TREAT_DIALOG);
}

/* "B0 8F": the player profile - the four name buffers hold the initial
 * texts and receive the entries, month / day are 0-based indices in and
 * out.  1 ok (the dialog has no cancel). */
uint32_t Dialog_AskProfile(OpenBGI_t* o, char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month, int32_t* day)
{
	OpenBGI_Request_t q;
	memset(&q, 0, sizeof q);
	q.kind = OPENBGI_ASK_PROFILE;
	q.u.profile.surname = surname;
	q.u.profile.givenName = givenName;
	q.u.profile.nickname = nickname;
	q.u.profile.pronoun = pronoun;
	q.u.profile.month = month;
	q.u.profile.day = day;
	return Request_Post(o, &q, REQUEST_TREAT_DIALOG);
}

// "B0 F0": set the desktop wallpaper to the image file at `path`; returns the host's result
int SetWallpaper(OpenBGI_t* o, const char* path, int stretch, int tile)
{
	char local[0x400];
	Fs_LocalPath(o, path, local, sizeof local); // a zip game's file is extracted for the system
	return Host_SetWallpaper(o, local, stretch, tile);
}
