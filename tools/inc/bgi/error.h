/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * error.h - script errors, the exception mechanism and the message boxes
 *           (src/core/error.c)
 *
 * The original is C++ and reports fatal script faults by throwing an `int`
 * (value 0x7FFFFFFF) with _CxxThrowException; `VmMain` catches it and stops
 * the machine.  The message pump throws `(int)0` on WM_QUIT.  C99 has no
 * exceptions, so the same control flow is built on setjmp/longjmp:
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
void BGI_TryPush(BgiTryFrame_t* f);
void BGI_TryPop(BgiTryFrame_t* f);

#define BGI_TRY(f)     (BGI_TryPush(&(f)), setjmp((f).jb) == 0)
#define BGI_END_TRY(f) BGI_TryPop(&(f))

// _CxxThrowException(int): never returns.  Aborts the process when no frame is active.
BGI_NORETURN void BGI_ThrowInt(int value);
#define BGI_SCRIPT_ERROR_VALUE 0x7fffffff // the value every script error throws

struct Thread;

// message box captioned "Error!!" (MB_ICONHAND | MB_SYSTEMMODAL)
void ShowErrorBox(const char* text);
/* the language of the engine's messages (inc/bgi/msg.h): "en" (the
 * default) or "ja", the originals; 1 when `lang` was understood */
int Msg_SetLanguage(const char* lang);
// ShowErrorBox + throw BGI_SCRIPT_ERROR_VALUE
BGI_NORETURN void ThrowScriptError(const char* text);
/* the "Thread No. [ %d ], Program [ %s ]..." context of the thread's
 * current instruction followed by `msg`, into dst (not bounded; ScriptError
 * gives it 0x400 bytes) */
void FormatErrorContext(char* dst, const char* msg, struct Thread* t);
// FormatErrorContext, write the text to BGIError.txt (replacing it), ThrowScriptError
BGI_NORETURN void ScriptError(const char* msg, struct Thread* t);

/* MessageBox wrapper that keeps the mouse cursor visible and clears the key
 * states afterwards; `flags` are OS_MB_*, the result an OS_ID* button code */
int MsgBox(const char* text, const char* caption, uint32_t flags);
// the same for a box whose text comes from a script: the caption gets " (script)"
int MsgBoxScript(const char* text, const char* caption, uint32_t flags);

#endif // BGI_ERROR_H_
