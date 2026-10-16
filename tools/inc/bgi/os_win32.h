/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_win32.h - declarations shared by the files of the Win32 back end
 *              (src/os/win32/, *.c); nothing above the OS layer includes
 *              this.  It pulls in windows.h, so it stays out of engine code.
 *
 * The back end implements inc/bgi/os.h with the calls the original makes,
 * in their wide-character (W) forms: the engine's strings are Shift-JIS
 * (code page 932) whatever the system's own code page is, so every string
 * is converted to UTF-16 on its way to Windows and back on its way to the
 * engine (src/os/win32/wide.c, declared below).  No ANSI (A) entry point is
 * called, which is what lets the games run on a system that is not set to
 * Japanese.
 *
 * What one of its files needs from another is declared here when every
 * file may need it (the string conversions, the instance, the main
 * window, the IPC message, the movie graph's notifications); the window
 * files share more through src/os/win32/win32_internal.h and the dialog
 * files through src/os/win32/ui_internal.h.
 */
#ifndef BGI_OS_WIN32_H_
#define BGI_OS_WIN32_H_

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501 // Windows XP: WM_XBUTTON*, the waitable timer, ChangeDisplaySettingsEx
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>

#include "bgi/common.h"

// ---- wide.c: the engine's strings as UTF-16 and back -----------------------
#define WIN32_CP_SJIS 932   // the code page of the engine's strings
#define WIN32_WPATH   0x400 // UTF-16 units of a converted path or short text on the stack

/* An engine string - text or a path: Shift-JIS, in UTF-8 text mode
 * (OS_SetTextEncoding) with every well-formed UTF-8 sequence taken as it
 * is - as UTF-16 into out (cap units, always NUL-terminated, cut short
 * when it does not fit); the units written without the NUL.  NULL gives
 * an empty string.  A path alias (below) is replaced by the path it
 * stands for. */
int Win32_ToWide(const char* s, WCHAR* out, int cap);
// the same into a malloc'ed buffer of the size the string needs; NULL for NULL, so an optional argument stays optional
WCHAR* Win32_ToWideDup(const char* s);
/* UTF-16 as Shift-JIS into out (n bytes, NUL-terminated, cut at a
 * character boundary when it does not fit); the bytes written without the
 * NUL.  A character without a Shift-JIS code becomes what the code page
 * substitutes (its best fit, else '?'). */
int Win32_FromWide(const WCHAR* w, char* out, int n);
// the same in the text encoding: UTF-8 in UTF-8 text mode, Shift-JIS otherwise (what the player typed, a font's name)
int Win32_TextFromWide(const WCHAR* w, char* out, int n);
// the bytes `units` UTF-16 units take in the text encoding (the length an ANSI control would count)
int Win32_TextBytes(const WCHAR* w, int units);
/* A path Windows reports, as the engine's Shift-JIS path into out (n
 * bytes); the bytes written.  The engine cannot hold a name outside code
 * page 932 ("C:\Users\Bjørn\..."), so such a component is written
 * with '_' for each character it lacks, and - for an absolute path - the
 * directory up to that component is remembered as a path alias: the
 * substitute spelling stands for the real directory in every string
 * Win32_ToWide converts from then on, which makes the files under it
 * reachable through the engine's own paths. */
int Win32_PathFromWide(const WCHAR* w, char* out, int n);
int Win32_Utf8ToWide(const char* utf8, WCHAR* out, int cap); // UTF-8 to UTF-16; the units written, "" when it fails
int Win32_WideToUtf8(const WCHAR* w, char* out, int n);      // and back; the bytes written
/* The command line for Engine_Main from the process's own (GetCommandLineW):
 * the program name is dropped and every argument converted as a path (so
 * an argument naming a file outside code page 932 gets its alias); a
 * "--option=value" has its value converted on its own.  The bytes written. */
int Win32_CmdLineFromWide(const WCHAR* cmd, char* out, int n);
// MessageBoxW over `owner` with the text and the caption converted; the button id
int Win32_MessageBox(HWND owner, const char* text, const char* caption, UINT flags);
/* What EM_LIMITTEXT meant to the original's ANSI controls: keep an EDIT
 * control's text within maxBytes bytes of the text encoding by cutting
 * characters off its end (the caret then goes behind the text); called
 * after the control changed its text.  Nothing for maxBytes <= 0. */
void Win32_EditClamp(HWND edit, int maxBytes);

// ---- process.c -----------------------------------------------------------
extern HINSTANCE gWin32Instance; // the module handle WinMain received (GetModuleHandle(NULL) when WinMain did not set it)
// the command line for Engine_Main (Win32_CmdLineFromWide of GetCommandLineW) into out (n bytes); WinMain's own is in the ANSI code page
void OsWin32_CommandLine(char* out, size_t n);

/* The receiving side of OS_IpcSendToRunning, called by the main window
 * procedure on BGI_WM_IPC: copy the string the other instance left in the
 * file mapping "FMO%.8xForBGI" named by `key` (wParam), `len` bytes with
 * the NUL (lParam), into out (n bytes); 1 = ok, 0 when len is 0, exceeds n
 * or the mapping cannot be opened. */
int OsWin32_IpcReceive(uint32_t key, uint32_t len, char* out, size_t n);
#define BGI_WM_IPC 0x9000 // the second instance's "here is your command line" message (raised as `ipc_string`)

// ---- window.c / wndproc.c ------------------------------------------------
extern HWND gWin32MainWnd; // the main window, NULL before creation / after WM_DESTROY

// the classes the original registers; the IPC sender looks the
// first one up with FindWindow
#define BGI_MAIN_CLASS  L"BGI - Main window"
#define BGI_CHILD_CLASS L"BGI - Child window"

/* a modal loop for the back end's own windows (dialogs run by DialogBox*
 * do not need it): pumps and dispatches one message, `dialog` getting
 * IsDialogMessage first; 1 to go on, 0 once WM_QUIT was seen (the message
 * is re-posted so the engine's pump sees it too) */
int OsWin32_PumpOnce(HWND dialog);

// ---- the dialog files (ui_internal.h) ------------------------------------
// the dialog font of the resource templates: 9 pt "MS P Gothic" (10 pt for the profile dialog)
#define BGI_DIALOG_FONT_PT 9

// ---- audio.c --------------------------------------------------------------
/* the movie graph's notification (WM_APP, IMediaEventEx::SetNotifyWindow)
 * arrives at the main window: drain the events (GetEvent / FreeEventParams
 * until GetEvent fails) and report whether an EC_COMPLETE was
 * among them; 0 without a graph */
int OsWin32_MovieDrainEvents(void);
// 1 while a movie's video window is shown over the picture (OS_MovieStart .. OS_MovieStop): WM_MOUSEACTIVATE then replays the click
int OsWin32_MovieWindowShown(void);

#endif // BGI_OS_WIN32_H_
