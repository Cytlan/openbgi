/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * error.h - script errors, the exception mechanism and the message boxes
 *           (src/core/error.c)
 *
 * The original is C++ and reports fatal script faults by throwing an `int`
 * (value 0x7FFFFFFF) with _CxxThrowException; the main loop catches it and
 * stops the machine (OpenBGI_RunFrame).  The message pump throws `(int)0`
 * on WM_QUIT (OpenBGI_PumpEvents catches it).  C99 has no exceptions, so
 * the same control flow is built on setjmp/longjmp:
 *
 *      if (BGI_TRY(frame)) {            // "try"
 *          ... code that may BGI_ThrowInt() ...
 *          BGI_END_TRY(frame);
 *      } else {                         // "catch (int)"
 *          int value = frame.value;
 *      }
 *
 * Frames nest; a throw unwinds to the innermost active frame.  Code between
 * a throw and its handler must not hold resources that are not reachable
 * from the handler - the original has the same constraint (MSVC 6 does not
 * run destructors for C objects either, and the engine objects involved are
 * all globals or owned by the thread that is being destroyed).
 */
#ifndef BGI_ERROR_H_
#define BGI_ERROR_H_

#include "bgi/common.h"
#include <setjmp.h>

typedef struct BgiTryFrame
{
	jmp_buf jb;
	int value;                // the thrown int (0 until a throw lands here)
	struct BgiTryFrame* prev; // the next outer frame
} BgiTryFrame_t;

// internal: push / pop the handler stack (the macros below use them)
void BGI_TryPush(OpenBGI_t* o, BgiTryFrame_t* f);
void BGI_TryPop(OpenBGI_t* o, BgiTryFrame_t* f);

#define BGI_TRY(o, f)     (BGI_TryPush(o, &(f)), setjmp((f).jb) == 0)
#define BGI_END_TRY(o, f) BGI_TryPop(o, &(f))

// _CxxThrowException(int): never returns.  Aborts the process when no frame is active.
BGI_NORETURN void BGI_ThrowInt(OpenBGI_t* o, int value);
#define BGI_SCRIPT_ERROR_VALUE 0x7fffffff // the value every script error throws
/* not in the original: a load asks for the disc (RetryPrompt, bgi/file.h);
 * whoever catches it asks and tries the load again */
#define BGI_RETRY_VALUE        0x7ffffffe

struct Thread;

// message box captioned "Error!!" (MB_ICONHAND | MB_SYSTEMMODAL)
void ShowErrorBox(OpenBGI_t* o, const char* text);
/* the language of the engine's messages (src/include/bgi/msg.h): "en" (the
 * default) or "ja", the originals; 1 when `lang` was understood */
int Msg_SetLanguage(const char* lang);
// ShowErrorBox (while keeping: the first error only), keep the message ("81 6A"), throw BGI_SCRIPT_ERROR_VALUE
BGI_NORETURN void ThrowScriptError(OpenBGI_t* o, const char* text);
void Error_SetKeep(OpenBGI_t* o, int on);            // "81 6A" (1.547 on): fatal errors end only their thread, the first one's message is kept
int Error_Keeping(OpenBGI_t* o);                     // the "81 6A" setting
uint32_t Error_KeptMessage(OpenBGI_t* o, char* out); // "81 6B": the kept message into out (may be NULL); length + 1, 0 when none
void Error_FreeKept(OpenBGI_t* o);                   // shutdown
/* the "Thread No. [ %d ], Program [ %s ]..." context of the thread's
 * current instruction followed by `msg`, into dst (not bounded; ScriptError
 * gives it 0x400 bytes) */
void FormatErrorContext(char* dst, const char* msg, struct Thread* t);
// FormatErrorContext, write the text to BGIError.txt (replacing it), ThrowScriptError
BGI_NORETURN void ScriptError(OpenBGI_t* o, const char* msg, struct Thread* t);

/* The engine's message box: a request to the host (bgi/request.h) that
 * keeps the mouse cursor visible while it is up and clears the key states
 * after it; `flags` are OPENBGI_MB_*.  The engine holds still while it is up.
 * MsgBox does not wait for the button; MsgBox_Ask returns the request,
 * whose answer (an OPENBGI_ID* button code) Request_Take gives. */
void MsgBox(OpenBGI_t* o, const char* text, const char* caption, uint32_t flags);
uint32_t MsgBox_Ask(OpenBGI_t* o, const char* text, const char* caption, uint32_t flags);
// a box without that treatment (the original's direct MessageBox calls)
void MsgBox_Plain(OpenBGI_t* o, const char* text, const char* caption, uint32_t flags);
// the same for a box whose text comes from a script: the caption gets " (script)"
void MsgBoxScript(OpenBGI_t* o, const char* text, const char* caption, uint32_t flags);
uint32_t MsgBoxScript_Ask(OpenBGI_t* o, const char* text, const char* caption, uint32_t flags);
/* The treatment around a box shown some other way (the installer's
 * dialogs show theirs themselves): Begin before it, returning the
 * cursor's show count that End restores after it. */
int MsgBox_Begin(OpenBGI_t* o, const char* text, const char* caption);
void MsgBox_End(OpenBGI_t* o, int cursorPrev);

#endif // BGI_ERROR_H_
