/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ops_obgi.c - the "EF xx" instruction family: OpenBGI's own extensions
 *
 * No original build has an instruction that starts with the byte EF, in
 * any generation, which is why the extensions live there: a program that
 * uses them cannot be mistaken for anything else, and one that does not is
 * unaffected.  The family exists in every engine profile (kOpSet, opset.c)
 * and is called "obgi" by the assembler and the compiler ("obgi.version",
 * "EF 00").  docs/obgi.md describes the instructions from a script
 * writer's point of view.
 *
 * A program built with these instructions does not run on the original
 * engine, which stops at the first EF byte with "undefined instruction".
 *
 * The conventions are those of ops_sys.c: the trailing comment of a
 * handler gives its stack effect, the inputs in the order the script
 * pushed them (the last one on top), then the outputs; every handler
 * returns scheduler code 0.  The interface (vm_optable_EF,
 * Vm_OptableEFInit) is declared in vm.h.
 */
#include <stdio.h>

#include "bgi/vm.h"
#include "bgi/error.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/strutil.h"
#include "bgi/gfx/text.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/bmpops.h"

#define OBGI_VERSION 1 // the level of the extension family: raised whenever instructions are added

// raise a script error on thread t with a formatted MSG_* text; does not return
#define GFX_ERROR(t, ...)           \
	do                              \
	{                               \
		char msg_[0x104];           \
		sprintf(msg_, __VA_ARGS__); \
		ScriptError(msg_, (t));     \
	} while(0)

// the level of the extensions this engine has, so that a program can ask before it uses a later addition
static int Opcode_Obgi_Version(Thread_t* t) // EF 00: → level
{
	Thread_Push(t, OBGI_VERSION);
	return 0;
}

/* the direction of rich text (message windows, "91 9C" bitmap text):
 * 0 the original left-to-right layout, 1 right-to-left paragraphs (Arabic,
 * Hebrew: lines start at the right edge, Arabic letters are joined, Latin
 * words and numbers inside keep their own direction), 2 left-to-right
 * paragraphs that may contain right-to-left text.  Any other value is 0.
 * In 1 and 2 a character that is not East Asian is as wide as its font
 * makes it - the original gives everything beyond ASCII a full cell - so 2
 * is the setting for European languages.  Text beyond the code page needs
 * the UTF-8 text encoding ("EF 02"). */
static int Opcode_Obgi_TextDirection(Thread_t* t) // EF 01: mode →
{
	Text_SetDirection((int)Thread_Pop(t));
	return 0;
}

/* the encoding of the script's strings: 0 Shift-JIS (code page 932), 1
 * UTF-8; other values change nothing.  This is "81 00" of 1.653 on made
 * available to every generation.  It belongs at the very start of the boot
 * program: every string the scripts hold must be in the encoding, and
 * glyphs drawn before a switch stay cached under their old codes. */
static int Opcode_Obgi_TextEncoding(Thread_t* t) // EF 02: mode →
{
	uint32_t mode = Thread_Pop(t);
	if(mode <= 1)
		Text_SetEncoding((int)mode);
	return 0;
}

/* break lines between words: the generations before 1.494 break a line
 * wherever it is full, also inside a Latin word.  With this on they keep a
 * word together and drop the space a line would end with, as the later
 * generations do (which this does not change). */
static int Opcode_Obgi_TextWordWrap(Thread_t* t) // EF 03: on →
{
	Text_SetWordWrap((int)Thread_Pop(t));
	return 0;
}

/* scrolling message text: with `on`, a message that is longer than the
 * text area of its window is no longer cut off.  The text scrolls up as
 * it is printed, pauses when a hurried output has filled the window, and
 * can be scrolled back and forth while the output waits for the click
 * (wheel, up / down keys, the arrows of "EF 09").  `msPerLine` is the time
 * a scroll of one line takes; 0 or less keeps the current one (120 at
 * start). */
static int Opcode_Obgi_TextScroll(Thread_t* t) // EF 08: on, msPerLine →
{
	int ms = (int)Thread_Pop(t);
	int on = (int)Thread_Pop(t);
	Text_SetScrolling(on, ms);
	return 0;
}

/* the two arrows a scrolling window shows while there is text above or
 * below the view: for each the bitmap number of its picture (-1: a
 * triangle in the text colour, sized by the font) and its position in the
 * window in pixels (a negative x or y: the end of the first line for "up",
 * of the last line for "down" - the right end, or the left end while the
 * text direction is right to left).  The pictures are copied, so the
 * bitmaps may be freed afterwards.  A bitmap that does not exist is a
 * script error. */
static int Opcode_Obgi_TextScrollArrows(Thread_t* t) // EF 09: bmpUp, upX, upY, bmpDown, downX, downY →
{
	int downY = (int)Thread_Pop(t), downX = (int)Thread_Pop(t), bmpDown = (int)Thread_Pop(t);
	int upY = (int)Thread_Pop(t), upX = (int)Thread_Pop(t), bmpUp = (int)Thread_Pop(t);
	if(bmpUp >= 0)
		CheckBitmapNo(bmpUp, t);
	if(bmpDown >= 0)
		CheckBitmapNo(bmpDown, t);
	if(!Text_SetArrow(0, bmpUp, upX, upY))
		GFX_ERROR(t, MSG_BMP_NOT_EXIST, bmpUp);
	if(!Text_SetArrow(1, bmpDown, downX, downY))
		GFX_ERROR(t, MSG_BMP_NOT_EXIST, bmpDown);
	return 0;
}

/* how much of the string `src` fits into `lines` lines of window `h`,
 * laid out like a message ("91 92" with the same rubyOn and hang) from the
 * start of a line.  That part is copied to `dst` - at most `maxBytes`
 * bytes plus the terminator, cut at the end of a line, without the line
 * break or space it ended with - and the number of bytes of `src` it used
 * up is pushed (0 only for an empty string or a window without a font).
 * Calling it in a loop cuts a long text into pieces of a few lines each. */
static int Opcode_Obgi_TextFit(Thread_t* t) // EF 0A: h, dst, src, lines, maxBytes, rubyOn, hang → used
{
	int hang = (int)Thread_Pop(t), rubyOn = (int)Thread_Pop(t);
	int maxBytes = (int)Thread_Pop(t), lines = (int)Thread_Pop(t);
	const char* src = (const char*)PopPtr(t);
	char* dst = (char*)PopPtr(t);
	uint32_t h = Thread_Pop(t);
	Window_t* win = (Window_t*)Gfx_FindWindow(gGfx, h);
	if(!win)
	{
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
		Thread_Push(t, 0);
		return 0;
	}
	Thread_Push(t, (uint32_t)Text_Fit(win, dst, src, lines, maxBytes, rubyOn, hang));
	return 0;
}

/* the width in pixels of `str` drawn on one line in the numbered font, as
 * the rich text layout places it (so it is right for proportional fonts
 * and for joined Arabic too); 0 for an empty string.  For centring a name
 * or a caption. */
static int Opcode_Obgi_TextWidth(Thread_t* t) // EF 0B: str, fontNo, size, widthPct, bold, prop → width
{
	int prop = (int)Thread_Pop(t), bold = (int)Thread_Pop(t), widthPct = (int)Thread_Pop(t);
	int size = (int)Thread_Pop(t), fontNo = (int)Thread_Pop(t);
	const char* str = (const char*)PopPtr(t);
	int handle = 0;
	CheckFontNo(fontNo, t);
	if(BmpOp_FontOpen(&handle, fontNo, size, widthPct, bold) != 0)
	{
		Thread_Push(t, 0);
		return 0;
	}
	Thread_Push(t, (uint32_t)Text_LineWidth(str, handle, prop));
	return 0;
}

/* how far the text of window `h` is scrolled, in pixels: 0 while the
 * view is at the top (always, for a window whose text fits), and during a
 * scroll the position reached so far.  A program that gives the mouse
 * wheel a meaning of its own (wheel up opens the backlog) asks this to
 * leave the wheel to the text while there is something to scroll back to. */
static int Opcode_Obgi_TextScrollPos(Thread_t* t) // EF 0C: h → y
{
	uint32_t h = Thread_Pop(t);
	Window_t* win = (Window_t*)Gfx_FindWindow(gGfx, h);
	if(!win)
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	Thread_Push(t, win ? (uint32_t)Window_DocScroll(win) : 0);
	return 0;
}

/* Fill vm_optable_EF; every instruction of the family exists in every
 * generation (see Vm_Optable80Init in ops_sys.c for the table's rules).
 * The opcode list is repeated in internal/re_tools/mkopset.py, which
 * writes the family's row of kOpSet. */
void Vm_OptableEFInit(void)
{
	static const OpEntry_t ops[] = {
		{0x00, Opcode_Obgi_Version, GEN_FIRST, GEN_LAST},
		{0x01, Opcode_Obgi_TextDirection, GEN_FIRST, GEN_LAST},
		{0x02, Opcode_Obgi_TextEncoding, GEN_FIRST, GEN_LAST},
		{0x03, Opcode_Obgi_TextWordWrap, GEN_FIRST, GEN_LAST},
		{0x08, Opcode_Obgi_TextScroll, GEN_FIRST, GEN_LAST},
		{0x09, Opcode_Obgi_TextScrollArrows, GEN_FIRST, GEN_LAST},
		{0x0A, Opcode_Obgi_TextFit, GEN_FIRST, GEN_LAST},
		{0x0B, Opcode_Obgi_TextWidth, GEN_FIRST, GEN_LAST},
		{0x0C, Opcode_Obgi_TextScrollPos, GEN_FIRST, GEN_LAST},
	};
	Vm_FillTable(vm_optable_EF, OPFAM_EF, ops, BGI_COUNTOF(ops));
}
