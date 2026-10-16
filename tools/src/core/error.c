/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * error.c - script errors, the setjmp/longjmp "exception" mechanism and the
 *           message-box wrapper (inc/bgi/error.h)
 *
 * A script error ends in ScriptError or ThrowScriptError: the message is
 * shown in a box, written to BGIError.txt (with the thread context) and
 * BGI_SCRIPT_ERROR_VALUE is thrown to the innermost BGI_TRY frame - the
 * pass loop of VmMain (src/vm/sched.c), which stops the machine, unless
 * the background loader has set up its own around a load.  Every message
 * box of the engine goes through MsgBox so that the cursor is visible and
 * the dismissing click does not reach the script.
 */
#include <stdio.h>
#include <string.h>

#include "bgi/error.h"
#include "bgi/dbg.h"
#include "bgi/vm.h"
#include "bgi/file.h"
#include "bgi/input.h"
#include "bgi/os.h"

// ---- exception frames ---------------------------------------------------

static BgiTryFrame_t* gTryTop; // the innermost active frame

void BGI_TryPush(BgiTryFrame_t* f)
{
	f->value = 0;
	f->prev = gTryTop;
	gTryTop = f;
}

void BGI_TryPop(BgiTryFrame_t* f)
{
	// frames are popped in LIFO order; a throw unwinds to the frame it lands
	// in, so everything above it is gone by construction
	gTryTop = f->prev;
}

// unwind to the innermost frame with `value`; aborts the process when there is none
void BGI_ThrowInt(int value)
{
	BgiTryFrame_t* f = gTryTop;
	if(!f)
	{
		fprintf(stderr, "bgi: unhandled exception %d\n", value);
		abort();
	}
	gTryTop = f->prev;
	f->value = value;
	longjmp(f->jb, 1);
}

// ---- the language of the messages ----------------------------------------

/* The original's messages are Japanese; the engine here shows their English
 * translations unless --lang=ja / BGI_LANG=ja asks for the originals (both
 * are in the binary: inc/bgi/msg.h, from tools/messages.txt).  The caption
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

void ShowErrorBox(const char* text)
{
	MsgBox(text, "Error!!", OS_MB_ICONHAND | OS_MB_SYSTEMMODAL);
}

// show the box, then throw the script error (no file, no thread context)
void ThrowScriptError(const char* text)
{
	ShowErrorBox(text);
	BGI_ThrowInt(BGI_SCRIPT_ERROR_VALUE);
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
void ScriptError(const char* msg, Thread_t* t)
{
	char text[0x400];
	FormatErrorContext(text, msg, t);
	WriteTextFile("BGIError.txt", text, (uint32_t)strlen(text));
	ThrowScriptError(text);
}

/* The engine's message box: the cursor is made visible around the box and
 * the key states are cleared afterwards so that the dismissing click is
 * not seen by the script.  `flags` are OS_MB_*; the OS_ID* button pressed
 * is returned.  The box is also written to the debugger's log. */
int MsgBox(const char* text, const char* caption, uint32_t flags)
{
	int prev, r;
	Vm_TraceDump(); // BGI_TRACE_LAST (a no-op without it)
	{               // the debugger's log gets the box on one line
		char line[0x100];
		size_t i, o = 0;
		for(i = 0; text[i] && o + 1 < sizeof line; i++)
		{
			if(text[i] == '\r')
				continue;
			if(text[i] == '\n')
			{
				if(o && line[o - 1] != ' ')
					line[o++] = ' ';
				continue;
			}
			line[o++] = text[i];
		}
		line[o] = 0;
		Dbg_Log("[%s] %s", caption ? caption : "", line);
	}
	Cursor_Refresh();
	prev = ShowCursorCount(1);
	r = OS_MessageBox(text, caption, flags);
	ShowCursorCount(prev);
	Input_ClearStates();
	return r;
}

/* A message box a script asked for (its own text: the notices and
 * questions of "B0 80 .. 82", the confirmation and debugging boxes of
 * "78 .. 7B"): the
 * caption marks it as the script's, so that it is not taken for one of the
 * engine's own messages, which --lang translates and the script's are not.
 * (Not part of the original.) */
int MsgBoxScript(const char* text, const char* caption, uint32_t flags)
{
	char marked[0x120];
	snprintf(marked, sizeof marked, "%s (script)", caption ? caption : "");
	return MsgBox(text, marked, flags);
}
