/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * error.c - script errors, the setjmp/longjmp "exception" mechanism and the
 *           message-box wrapper (src/include/bgi/error.h)
 *
 * A script error ends in ScriptError or ThrowScriptError: the message is
 * shown in a box, written to BGIError.txt (with the thread context) and
 * BGI_SCRIPT_ERROR_VALUE is thrown to the innermost BGI_TRY frame - the
 * pass of the frame (Vm_Frame, src/vm/sched.c), which stops the machine,
 * unless the background loader has set up its own around a load.  Every
 * message box of the engine goes through MsgBox: a request to the host
 * (bgi/request.h) that keeps the cursor visible while the box is up and
 * keeps the dismissing click from the script; the engine holds still
 * until it is answered, so a box that only informs needs no one to wait
 * for it, and the error's throw follows its box at once.
 */
#include <stdio.h>
#include <string.h>

#include "bgi/error.h"
#include "bgi/dbg.h"
#include "bgi/vm.h"
#include "bgi/file.h"
#include "bgi/input.h"
#include "bgi/sys.h"
#include "bgi/request.h"
#include "bgi/core.h"

// ---- exception frames ---------------------------------------------------

void BGI_TryPush(OpenBGI_t* o, BgiTryFrame_t* f)
{
	f->value = 0;
	f->prev = o->err.tryTop;
	o->err.tryTop = f;
}

void BGI_TryPop(OpenBGI_t* o, BgiTryFrame_t* f)
{
	// frames are popped in LIFO order; a throw unwinds to the frame it lands
	// in, so everything above it is gone by construction
	o->err.tryTop = f->prev;
}

// unwind to the innermost frame with `value`; aborts the process when there is none
void BGI_ThrowInt(OpenBGI_t* o, int value)
{
	BgiTryFrame_t* f = o->err.tryTop;
	if(!f)
	{
		fprintf(stderr, "openbgi: unhandled exception %d\n", value);
		abort();
	}
	o->err.tryTop = f->prev;
	f->value = value;
	longjmp(f->jb, 1);
}

// ---- the language of the messages ----------------------------------------

/* The original's messages are Japanese; the engine here shows their English
 * translations unless --lang=ja / BGI_LANG=ja asks for the originals (both
 * are in the binary: src/include/bgi/msg.h, from tools/messages.txt).  The caption
 * "Error!!" and the context lines of a script error are ASCII in the
 * original already. */
int gMsgEnglish = 1;

// "en" / "english" or "ja" / "jp" / "japanese"; 1 when understood, 0 (nothing changed) otherwise
int Msg_SetLanguage(const char* lang)
{
	if(!lang || !*lang)
		return 0;
	if(strcmp(lang, "en") == 0 || strcmp(lang, "english") == 0)
		gMsgEnglish = 1;
	else if(strcmp(lang, "ja") == 0 || strcmp(lang, "jp") == 0 || strcmp(lang, "japanese") == 0)
		gMsgEnglish = 0;
	else
		return 0;
	return 1;
}

// ---- error reporting ---------------------------------------------------

void ShowErrorBox(OpenBGI_t* o, const char* text)
{
	MsgBox(o, text, "Error!!", OPENBGI_MB_ICONHAND | OPENBGI_MB_SYSTEMMODAL);
}

/* 1.547 on, "81 6A": fatal script errors are kept.  The box shows the
 * first error only; its message is kept for "81 6B", and the scheduler
 * ends the thread that raised it (posting event -1) instead of stopping
 * the engine (Vm_RunPass's catch in sched.c).  Before 1.547 nothing is
 * ever kept and every error shows its box. */
void Error_SetKeep(OpenBGI_t* o, int on)
{
	o->err.keepErrors = on;
}

int Error_Keeping(OpenBGI_t* o)
{
	return o->err.keepErrors;
}

// "81 6B": the kept message into `out` (when not NULL); its length + 1, 0 when none was kept
uint32_t Error_KeptMessage(OpenBGI_t* o, char* out)
{
	size_t n;
	if(!o->err.keptError)
		return 0;
	n = strlen(o->err.keptError) + 1;
	if(out)
		memcpy(out, o->err.keptError, n);
	return (uint32_t)n;
}

// the engine's shutdown: the kept message goes
void Error_FreeKept(OpenBGI_t* o)
{
	BGI_Free(o->err.keptError);
	o->err.keptError = NULL;
}

// show the box (the first error only while keeping), keep the message when asked, then throw the script error (no file, no thread context)
void ThrowScriptError(OpenBGI_t* o, const char* text)
{
	if(!o->err.keptError)
		ShowErrorBox(o, text);
	if(o->err.keepErrors && !o->err.keptError)
	{
		size_t n = strlen(text) + 1;
		o->err.keptError = (char*)BGI_Alloc(n);
		memcpy(o->err.keptError, text, n);
	}
	BGI_ThrowInt(o, BGI_SCRIPT_ERROR_VALUE);
}

/*
 * Write the error context of thread t's current instruction followed by
 * `msg` into dst.  The instruction number is one byte, or two when the
 * first byte is >= 0x80 (a sub-table opcode).  When the thread has modules
 * the program name and the IP relative to the module that contains opIP
 * are included; the frame pointer is printed under the label "SP", as in
 * the original.
 */
void FormatErrorContext(char* dst, const char* msg, Thread_t* t)
{
	uint32_t opIP = Thread_CurOpIP(t);
	const uint8_t* code = t->code + opIP;
	uint32_t insn = code[0];
	ModuleNode_t* mods;
	uint32_t count;

	if(insn >= 0x80)
		insn = (insn << 8) + code[1];

	count = Thread_ExportModules(t, &mods);
	if(count > 0)
	{
		// the export is newest-first; find the module that holds opIP
		ModuleNode_t* m = mods;
		if(m->base > opIP)
		{
			do
			{
				m++;
			} while(m->base > opIP);
		}
		sprintf(dst,
			"Thread No. [ %d ] , Program [ %s ]\n\n"
			"IP [ Thread : $%.8X / Program : $%.8X ] , Instruction No. [ $%X ] , SP [ $%.8X ]\n\n%s",
			(int)Thread_GetId(t), m->name, (unsigned)opIP, (unsigned)(opIP - m->base),
			(unsigned)insn, (unsigned)Thread_GetFP(t), msg);
		BGI_Free(mods);
	}
	else
	{
		sprintf(dst, "Thread No. [ %d ] , IP [ $%.8X ] , Instruction No. [ $%X ]\n\n%s",
			(int)Thread_GetId(t), (unsigned)opIP, (unsigned)insn, msg);
	}
}

// the script error with context: format, write BGIError.txt in the base directory, show and throw
void ScriptError(OpenBGI_t* o, const char* msg, Thread_t* t)
{
	char text[0x400];
	FormatErrorContext(text, msg, t);
	WriteTextFile(o, "BGIError.txt", text, (uint32_t)strlen(text));
	ThrowScriptError(o, text);
}

/* The treatment the engine gives its message boxes (the original's
 * MessageBox wrapper): before the box the trace ring is dumped
 * (BGI_TRACE_LAST), the box goes to the debugger's log on one line, the
 * cursor is made visible and the clock stops; MsgBox_Begin returns the
 * cursor's previous show count.  After it (MsgBox_End) the count is
 * restored, the key states are cleared so that the click or the key that
 * closed the box is not seen by the script, and the clock runs on. */
int MsgBox_Begin(OpenBGI_t* o, const char* text, const char* caption)
{
	Vm_TraceDump(o); // BGI_TRACE_LAST (a no-op without it)
	{                // the debugger's log gets the box on one line
		char line[0x100];
		size_t i, off = 0;
		for(i = 0; text[i] && off + 1 < sizeof line; i++)
		{
			if(text[i] == '\r')
				continue;
			if(text[i] == '\n')
			{
				if(off && line[off - 1] != ' ')
					line[off++] = ' ';
				continue;
			}
			line[off++] = text[i];
		}
		line[off] = 0;
		Dbg_Log(o, "[%s] %s", caption ? caption : "", line);
	}
	Cursor_Refresh(o);
	Clock_BoxBegin(o);
	return ShowCursorCount(o, 1);
}

void MsgBox_End(OpenBGI_t* o, int cursorPrev)
{
	ShowCursorCount(o, cursorPrev);
	Input_ClearStates(o);
	Clock_BoxEnd(o);
}

/* The engine's message box, a request to the host (bgi/request.h) with
 * the treatment above.  `flags` are OPENBGI_MB_*.  MsgBox does not wait for the
 * button: the engine holds still while the box is up and goes on after
 * it.  MsgBox_Ask returns the request whose answer (the OPENBGI_ID* button)
 * the caller takes with Request_Take. */
void MsgBox(OpenBGI_t* o, const char* text, const char* caption, uint32_t flags)
{
	Request_Forget(o, MsgBox_Ask(o, text, caption, flags));
}

uint32_t MsgBox_Ask(OpenBGI_t* o, const char* text, const char* caption, uint32_t flags)
{
	return Request_Box(o, text, caption, flags, REQUEST_TREAT_BOX);
}

// a box without the treatment: the original's direct MessageBox calls (the start-up checks, a lost message wait)
void MsgBox_Plain(OpenBGI_t* o, const char* text, const char* caption, uint32_t flags)
{
	Request_Forget(o, Request_Box(o, text, caption, flags, REQUEST_TREAT_NONE));
}

/* A message box a script asked for (its own text: the notices and
 * questions of "B0 80 .. 82", the confirmation and debugging boxes of
 * "78 .. 7B"): the caption marks it as the script's, so that it is not
 * taken for one of the engine's own messages, which --lang translates and
 * the script's are not.  (Not part of the original.) */
uint32_t MsgBoxScript_Ask(OpenBGI_t* o, const char* text, const char* caption, uint32_t flags)
{
	char marked[0x120];
	snprintf(marked, sizeof marked, "%s (script)", caption ? caption : "");
	return MsgBox_Ask(o, text, marked, flags);
}

void MsgBoxScript(OpenBGI_t* o, const char* text, const char* caption, uint32_t flags)
{
	Request_Forget(o, MsgBoxScript_Ask(o, text, caption, flags));
}
