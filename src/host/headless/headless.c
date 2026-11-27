/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * headless.c - a window, input and dialogs nobody sees (src/host/os.h):
 *              the Android build's until it has a display of its own, and
 *              the test host's (main.c)
 *
 * Both builds are the POSIX back end's system services (files, processes,
 * threads, time, the registry file, fonts, sound) without its X11 files.
 * In their place this file has no window: the host's table of it is NULL
 * (src/host/os_window.c), and the engine keeps the window nobody sees
 * (openbgi.h, "The main window").  Every presentation goes nowhere,
 * message boxes go to stderr and answer with their default button, and
 * the script's dialogs, the file and folder pickers and the game chooser
 * report that they are not available; there is no debugger's window.
 * The text entry and the child windows are the engine's, drawn in the
 * frame.  The pump raises nothing but the quit a signal asks for
 * (OS_SetHostHandlers); the test host hands the engine its script's input
 * itself (OpenBGI_Key and its kin), and on Android nothing does.  The engine therefore runs a game's programs to the point where they wait
 * for the player, which is what the command line (adb shell) can use; the
 * NativeActivity back end (the screen, touch input, AAudio) will replace
 * this file on Android.
 *
 * The test host's machine runs on its own clock (os_headless.h): the time
 * stamp counter counts it, so that what the engine measures does not
 * depend on how fast the frames run.
 */
#include "host/os.h"
#include "host/os_common.h"
#include "host/os_posix.h"
#include "host/os_headless.h"
#include "bgi/sjis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the test host's clock (os_headless.h) -------------------------------------------

static uint32_t gNowMs; // set by the test host before each frame

void OsHeadless_SetNow(uint32_t ms)
{
	gNowMs = ms;
}

uint32_t OsHeadless_Now(void)
{
	return gNowMs;
}

// ---- the window: none (the engine keeps the window nobody sees) ----------------------------

static OsHostHandlers_t gInput;  // the host's handlers (OS_SetHostHandlers)
static volatile int gQuitPosted; // a signal asked for the quit: the next pump raises `quit`

void OS_SetHostHandlers(const OsHostHandlers_t* h)
{
	if(h)
		gInput = *h;
	else
		memset(&gInput, 0, sizeof gInput);
}

// SIGTERM / SIGINT (process.c): `quit` from the next pump, so that the engine shuts down in order
void OsPosix_RequestClose(void)
{
	gQuitPosted = 1;
}

// the quit a signal asked for is the only message there is; 1 when it was delivered
int OS_PumpMessages(void)
{
	if(!gQuitPosted)
		return 0;
	gQuitPosted = 0;
	if(gInput.quit)
		gInput.quit(gInput.user);
	return 1;
}

// no input method: the test host hands the engine its text itself
void OS_TextInput(int on, int x, int y, int w, int h)
{
	BGI_UNUSED(on);
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(w);
	BGI_UNUSED(h);
}

// the boxes go to stderr and the dialogs answer "cancelled": nobody is asked
uint32_t OS_HostCaps(void)
{
	return OPENBGI_CAP_DIALOGS;
}

// ---- presentation: nowhere to go -------------------------------------------------------

OsDc_t* OS_WindowGetDc(void)
{
	return NULL;
}

void OS_WindowReleaseDc(OsDc_t* dc)
{
	BGI_UNUSED(dc);
}

void OS_BlitToWindow(OsDc_t* dc, int dstX, int dstY, int w, int h, const OsSurface_t* src, int srcX, int srcY)
{
	BGI_UNUSED(dc);
	BGI_UNUSED(dstX);
	BGI_UNUSED(dstY);
	BGI_UNUSED(w);
	BGI_UNUSED(h);
	BGI_UNUSED(src);
	BGI_UNUSED(srcX);
	BGI_UNUSED(srcY);
}

void OS_WindowFillBlack(void)
{
}

void OS_WindowPresent(void)
{
}

// no surface is said here: no layer
void OS_WindowLayer(const unsigned char* pixels, int w, int h, int pitch, int x, int y, int cw, int ch)
{
	BGI_UNUSED(pixels);
	BGI_UNUSED(w);
	BGI_UNUSED(h);
	BGI_UNUSED(pitch);
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(cw);
	BGI_UNUSED(ch);
}

int OS_ClipboardSetText(const char* text)
{
	BGI_UNUSED(text);
	return 0;
}

// ---- the pointer: none (it rests at the client origin; the test host tells the engine its own) -----

void OS_CursorGetPos(int* x, int* y)
{
	*x = 0;
	*y = 0;
}

void OS_CursorSetPos(int x, int y)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
}

void OS_ScreenToClient(int* x, int* y)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
}

void OS_ClientToScreen(int* x, int* y)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
}

// ---- child windows and the text entry: the engine's, in the frame ------------------------------

const OpenBGI_ChildWindows_t* OS_ChildWindows(void)
{
	return NULL;
}

const OpenBGI_TextEntry_t* OS_TextEntry(void)
{
	return NULL;
}

// ---- message boxes and dialogs -------------------------------------------------------

// the boxes and dialogs are answered at once: no modal loop to tick
void OS_SetModalTick(void (*tick)(void* user), void* user)
{
	BGI_UNUSED(tick);
	BGI_UNUSED(user);
}

// on stderr, answered with the default button (as the POSIX back end does without a display)
int OS_MessageBox(const char* text, const char* caption, uint32_t flags)
{
	char utf8[0x800];
	uint32_t type = flags & 0xf;
	int second = (flags & OPENBGI_MB_DEFBUTTON2) != 0;
	OsCommon_SjisToUtf8(caption ? caption : "", utf8, sizeof utf8);
	fprintf(stderr, "[%s] ", utf8);
	OsCommon_SjisToUtf8(text ? text : "", utf8, sizeof utf8);
	fprintf(stderr, "%s\n", utf8);
	switch(type)
	{
		case OPENBGI_MB_OKCANCEL:
			return second ? OPENBGI_IDCANCEL : OPENBGI_IDOK;
		case OPENBGI_MB_YESNO:
			return second ? OPENBGI_IDNO : OPENBGI_IDYES;
		case OPENBGI_MB_RETRYCANCEL:
			return second ? OPENBGI_IDCANCEL : OPENBGI_IDRETRY;
		default:
			return OPENBGI_IDOK;
	}
}

// the script's dialogs are cancelled
int OS_DialogInput1(const char* title, const char* initial, int maxLen, int digits, char* out)
{
	BGI_UNUSED(title);
	BGI_UNUSED(initial);
	BGI_UNUSED(maxLen);
	BGI_UNUSED(digits);
	BGI_UNUSED(out);
	return 0;
}

int OS_DialogInput2(const char* title, const char* label1, const char* initial1, int maxLen1, int digits1, char* out1,
	const char* label2, const char* initial2, int maxLen2, int digits2, char* out2, int compact)
{
	BGI_UNUSED(title);
	BGI_UNUSED(label1);
	BGI_UNUSED(initial1);
	BGI_UNUSED(maxLen1);
	BGI_UNUSED(digits1);
	BGI_UNUSED(out1);
	BGI_UNUSED(label2);
	BGI_UNUSED(initial2);
	BGI_UNUSED(maxLen2);
	BGI_UNUSED(digits2);
	BGI_UNUSED(out2);
	BGI_UNUSED(compact);
	return 0;
}

// OK only: the fields come back as they went in
int OS_DialogProfile(char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month, int32_t* day)
{
	BGI_UNUSED(surname);
	BGI_UNUSED(givenName);
	BGI_UNUSED(nickname);
	BGI_UNUSED(pronoun);
	BGI_UNUSED(month);
	BGI_UNUSED(day);
	return 1;
}

int OS_FileDialog(int save, char* path, size_t n, const char* filter, const char* defExt, const char* initialDir, const char* title)
{
	BGI_UNUSED(save);
	BGI_UNUSED(path);
	BGI_UNUSED(n);
	BGI_UNUSED(filter);
	BGI_UNUSED(defExt);
	BGI_UNUSED(initialDir);
	BGI_UNUSED(title);
	return 0;
}

int OS_FileDialogN(int save, char* path, size_t n, const char* const* descs, const char* const* pats, int count,
	const char* defExt, const char* initialDir, const char* title)
{
	BGI_UNUSED(save);
	BGI_UNUSED(path);
	BGI_UNUSED(n);
	BGI_UNUSED(descs);
	BGI_UNUSED(pats);
	BGI_UNUSED(count);
	BGI_UNUSED(defExt);
	BGI_UNUSED(initialDir);
	BGI_UNUSED(title);
	return 0;
}

int OS_BrowseFolder(char* path, size_t n, const char* title, const char* initial)
{
	BGI_UNUSED(path);
	BGI_UNUSED(n);
	BGI_UNUSED(title);
	BGI_UNUSED(initial);
	return 0;
}

// no display for the game chooser: the game directory has to be given on the command line
int OS_GameChooserDialog(OpenBGI_GameChooser_t* l)
{
	BGI_UNUSED(l);
	return -1;
}
