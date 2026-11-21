/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ops_ext0.c - the "B0 xx" extension instruction family: direct window
 *              blits, the mouse cursor sprite, the screen quake, child
 *              windows, the text entry control, message boxes, the input
 *              dialogs, the desktop wallpaper and the font registry.
 *              Interface: bgi/vm.h (o->vm.optableB0, Vm_OptableB0Init).
 *
 * Same conventions as ops_gfx0.c: every handler pops its operands in the
 * order the original does, validates them with the Check* routines (which
 * raise a script error and never return), calls the service and maps the
 * service's result code to a script error.  The stack comments on the
 * signature lines use the notation "a, b → r": b is on top of the stack.
 * Most handlers are thin: pop, call the module, map its result to a
 * ScriptError message.
 */
#include "bgi/vm.h"
#include "bgi/wait.h"
#include "bgi/waitobj.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/sys.h"
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/edit.h"
#include "bgi/dialog.h"
#include "bgi/gfx/bmpops.h" // the font registry
#include "bgi/gfx/typeface.h"
#include "bgi/core.h"

// raise a script error with a printf-formatted message; never returns
#define EXT_ERROR(o, t, ...)        \
	do                              \
	{                               \
		char msg_[0x104];           \
		sprintf(msg_, __VA_ARGS__); \
		ScriptError(o, msg_, (t));  \
	} while(0)

// -------------------------------------------------------------------------
// window
// -------------------------------------------------------------------------

/* 1.69 build 472 on: move the window to its centred default position;
 * does nothing and pushes 0 in full screen */
static int Opcode_Ext0_WindowCentre(OpenBGI_t* o, Thread_t* t) // B0 02 (1.69/472 on): → ok
{
	int x, y, ok = !o->display.fullscreen;
	if(ok)
	{
		Display_DefaultWindowPos(o, &x, &y);
		MainWin_Move(o, x, y);
	}
	Thread_Push(t, (uint32_t)ok);
	return 0;
}

// 1.69 build 451 (the retail Tayutama): move the window's outer rectangle to (x, y), in any mode, without a result
static int Opcode_Ext0_WindowSetPos_451(OpenBGI_t* o, Thread_t* t) // B0 03 (1.69/451): x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	MainWin_Move(o, x, y);
	return 0;
}

/* 1.69 build 472 on: move the window's outer rectangle to (x, y) when
 * windowed (nothing in full screen), then report whether the picture area
 * lies entirely on the screen at that position; the frame may hang over
 * the edge.  The test is made in both modes. */
static int Opcode_Ext0_WindowSetPos(OpenBGI_t* o, Thread_t* t) // B0 03 (1.69/472 on): x, y → inRange
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int sw, sh, fw, fh, pw, ph, inRange;
	if(!o->display.fullscreen)
		MainWin_Move(o, x, y);
	MainWin_ScreenSize(o, &sw, &sh);
	MainWin_FrameMetrics(o, &fw, &fh);
	Display_GetPictureSize(o, &pw, &ph);
	// the original's bounds from SM_CXFIXEDFRAME / SM_CXSCREEN (the caption height for y): the client area, not the frame, must be inside the screen
	inRange = x >= -fw / 2 && x <= sw - pw - fw / 2 && y >= -MainWin_FrameTop(o) && y <= sh - ph - MainWin_FrameTop(o);
	BGI_UNUSED(fh);
	Thread_Push(t, (uint32_t)inRange);
	return 0;
}

// draw a managed bitmap directly onto the window at (x, y), outside the compositor
static int Opcode_Ext0_WindowBlit(OpenBGI_t* o, Thread_t* t) // B0 00: x, y, bmp →
{
	int bmp = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	CheckBitmapNo(o, bmp, t);
	if(Window_BlitBitmap(o, x, y, bmp) == 2) // result 1 (no window) is silent
		EXT_ERROR(o, t, MSG_BMP_NOT_EXIST, bmp);
	return 0;
}

/* show a sprite as the mouse cursor, placed at the pointer position plus
 * (dx, dy); sprite 0 restores the system cursor.  A sprite that cannot be
 * shown is a script error. */
static int Opcode_Ext0_CursorSprite(OpenBGI_t* o, Thread_t* t) // B0 04: sprite, dx, dy →
{
	int dy = (int)Thread_Pop(t);
	int dx = (int)Thread_Pop(t);
	uint32_t sprite = Thread_Pop(t);
	if(SetCursorSprite(o, sprite, dx, dy) == -1)
		ScriptError(o, MSG_BAD_SPRITE_HANDLE, t);
	return 0;
}

// hide the cursor after `ms` milliseconds without mouse movement (0 turns the feature off)
static int Opcode_Ext0_CursorAutoHide(OpenBGI_t* o, Thread_t* t) // B0 05: ms →
{
	SetCursorAutoHide(o, Thread_Pop(t));
	return 0;
}

/* 1.529 on: 1 while the cursor is visible, 0 while the auto-hide has it
 * hidden; the system programs count a hover only while it is visible */
static int Opcode_Ext0_CursorIsVisible(OpenBGI_t* o, Thread_t* t) // B0 06 (1.529 on): → visible
{
	Thread_Push(t, (uint32_t)Cursor_IsVisible(o));
	return 0;
}

/* start the screen quake and block the thread on its wait object (scheduler
 * code 2).  StartQuake allocates the wait before validating the parameters;
 * a rejected one is raised as a script error, which does not return, so the
 * object is never installed in that case. */
static int Opcode_Ext0_Quake(OpenBGI_t* o, Thread_t* t) // B0 08: pattern, amplitude, frequency, repeat, decay, fps, skip →
{
	Wait_t* w;
	int skip = (int)Thread_Pop(t);
	int fps = (int)Thread_Pop(t);
	int decay = (int)Thread_Pop(t);
	int repeat = (int)Thread_Pop(t);
	int frequency = (int)Thread_Pop(t);
	int amplitude = (int)Thread_Pop(t);
	int pattern = (int)Thread_Pop(t);
	switch((uint32_t)StartQuake(o, t, pattern, amplitude, frequency, repeat, decay, fps, skip, &w))
	{
		case 0x80000001u: EXT_ERROR(o, t, MSG_BAD_QUAKE_PATTERN, pattern); break;
		case 0x80000002u: EXT_ERROR(o, t, MSG_BAD_FREQUENCY, frequency); break;
		case 0x80000003u: EXT_ERROR(o, t, MSG_BAD_REPEAT_COUNT, repeat); break;
		case 0x80000004u:
			if(fps < 1)
				EXT_ERROR(o, t, MSG_BAD_FRAME_RATE, fps);
			else
				EXT_ERROR(o, t, MSG_BAD_FRAME_RATE_ABOVE_FREQ, fps);
			break;
		default: break;
	}
	Thread_SetWait(o, t, w);
	return 2;
}

// -------------------------------------------------------------------------
// child windows
// -------------------------------------------------------------------------

/* create a hidden pop-up window with a w x h client area at the screen
 * position (x, y) and push its handle; a size outside 0x20 .. 0x400 or all
 * eight slots in use is a script error */
static int Opcode_Ext0_ChildCreate(OpenBGI_t* o, Thread_t* t) // B0 10: title, x, y, w, h → handle
{
	uint32_t handle = 0;
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	const char* title = (const char*)PopPtr(o, t);
	switch(Child_Create(o, &handle, title, x, y, w, h))
	{
		case 0x80000001u: EXT_ERROR(o, t, MSG_BAD_CHILD_SIZE, w, h); break;
		case 0x80000002u: ScriptError(o, MSG_NO_MORE_CHILDREN, t); break;
		default: break;
	}
	Thread_Push(t, handle);
	return 0;
}

static int Opcode_Ext0_ChildDestroy(OpenBGI_t* o, Thread_t* t) // B0 11: handle →
{
	if(!Child_Destroy(o, Thread_Pop(t)))
		ScriptError(o, MSG_BAD_CHILD_HANDLE2, t); // the original's Japanese message here reads "child child window"
	return 0;
}

static int Opcode_Ext0_ChildShow(OpenBGI_t* o, Thread_t* t) // B0 14: handle, show →
{
	int show = (int)Thread_Pop(t);
	uint32_t handle = Thread_Pop(t);
	if(!Child_Show(o, handle, show))
		ScriptError(o, MSG_BAD_CHILD_HANDLE, t);
	return 0;
}

static int Opcode_Ext0_ChildSetTitle(OpenBGI_t* o, Thread_t* t) // B0 15: handle, title →
{
	const char* title = (const char*)PopPtr(o, t);
	uint32_t handle = Thread_Pop(t);
	if(!Child_SetTitle(o, handle, title))
		ScriptError(o, MSG_BAD_CHILD_HANDLE, t);
	return 0;
}

// move the child's outer rectangle to the screen position (x, y)
static int Opcode_Ext0_ChildMove(OpenBGI_t* o, Thread_t* t) // B0 16: handle, x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t handle = Thread_Pop(t);
	if(!Child_Move(o, handle, x, y))
		ScriptError(o, MSG_BAD_CHILD_HANDLE, t);
	return 0;
}

// the child's screen position as the window manager reports it
static int Opcode_Ext0_ChildGetPos(OpenBGI_t* o, Thread_t* t) // B0 17: handle → x, y
{
	int32_t pos[2] = {0, 0};
	if(!Child_GetPos(o, pos, Thread_Pop(t)))
		ScriptError(o, MSG_BAD_CHILD_HANDLE, t);
	Thread_Push(t, (uint32_t)pos[0]);
	Thread_Push(t, (uint32_t)pos[1]);
	return 0;
}

// fill the child's back buffer with a colour and present it
static int Opcode_Ext0_ChildFill(OpenBGI_t* o, Thread_t* t) // B0 18: handle, colour →
{
	uint32_t colour = Thread_Pop(t);
	uint32_t handle = Thread_Pop(t);
	if(!Child_Fill(o, handle, colour))
		ScriptError(o, MSG_BAD_CHILD_HANDLE, t);
	return 0;
}

/* blit a managed bitmap into the child's back buffer at (x, y) with an
 * effect mode and a level, then present it */
static int Opcode_Ext0_ChildDrawBitmap(OpenBGI_t* o, Thread_t* t) // B0 19: handle, x, y, bmp, effect, level →
{
	int level = (int)Thread_Pop(t);
	int effect = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t handle = Thread_Pop(t);
	CheckBitmapNo(o, bmp, t);
	CheckEffectMode(o, (uint32_t)effect, t);
	CheckAlpha(o, (uint32_t)level, t);
	switch(Child_DrawBitmap(o, handle, x, y, bmp, effect, level))
	{
		case 0xffffffffu: ScriptError(o, MSG_BAD_CHILD_HANDLE, t); break;
		case 0x80000003u: EXT_ERROR(o, t, MSG_BMP_NOT_EXIST, bmp); break;
		case 0x80000004u: EXT_ERROR(o, t, MSG_CHILD_BMP_MODE, bmp); break;
		case 0x80000007u: EXT_ERROR(o, t, MSG_CHILD_OUT_OF_CLIENT, x, y); break;
		default: break; // the effect and the level were validated above; the other results are silent
	}
	return 0;
}

/* draw text into the child window with the bitmap manager's font cache and
 * present it; pushes the width of the widest line.  `widthPct` is the
 * horizontal scale in per cent, `prop` selects proportional spacing. */
static int Opcode_Ext0_ChildDrawText(OpenBGI_t* o, Thread_t* t) // B0 1A: handle, x, y, str, fontNo, size, widthPct, bold, prop, colour → width
{
	int32_t measure = 0;
	uint32_t colour = Thread_Pop(t);
	int prop = (int)Thread_Pop(t);
	int bold = (int)Thread_Pop(t);
	int widthPct = (int)Thread_Pop(t);
	int size = (int)Thread_Pop(t);
	int fontNo = (int)Thread_Pop(t);
	const char* str = (const char*)PopPtr(o, t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t handle = Thread_Pop(t);
	CheckFontNo(o, fontNo, t);
	switch(Child_DrawText(o, handle, x, y, str, fontNo, size, widthPct, bold, prop, colour, &measure))
	{
		case 0xffffffffu: ScriptError(o, MSG_BAD_CHILD_HANDLE, t); break;
		case 0x80000009u: EXT_ERROR(o, t, MSG_FONT_SIZE_INVALID, size); break;
		case 0x8000000au: EXT_ERROR(o, t, MSG_FONT_WIDTH_INVALID, widthPct); break;
		case 0x8000000bu: EXT_ERROR(o, t, MSG_FONT_NO_INVALID, fontNo); break;
		default: break;
	}
	Thread_Push(t, (uint32_t)measure);
	return 0;
}

// -------------------------------------------------------------------------
// the text entry control
// -------------------------------------------------------------------------

/* 1.69 build 472 on: that build makes the edit control at each "B0 20"
 * and this instruction destroys it (the subclass, the font and the
 * window).  Pushes 0. */
static int Opcode_Ext0_EditDestroy(OpenBGI_t* o, Thread_t* t) // B0 21 (1.69/472 on): → 0
{
	Thread_Push(t, (uint32_t)Edit_Remove(o));
	return 0;
}

/* 1.69 build 472 on: the state the control's input method status window
 * was last given, which is whether the control is shown; Tab and Enter
 * may have hidden it since */
static int Opcode_Ext0_EditImeOpen(OpenBGI_t* o, Thread_t* t) // B0 23 (1.69/472 on): → open
{
	Thread_Push(t, (uint32_t)Edit_Shown(o));
	return 0;
}

// 1.69 build 472 on: whether Enter, besides selecting the text and giving the focus back, hides the control
static int Opcode_Ext0_EditImeCloseOnEnter(OpenBGI_t* o, Thread_t* t) // B0 28 (1.69/472 on): on →
{
	Edit_SetCloseOnEnter(o, (int)Thread_Pop(t));
	return 0;
}

// 1.636 on: the alignment of the controls the next "B0 20" makes: 0 left, 1 centred, 2 right; another value is not taken
static int Opcode_Ext0_EditAlign(OpenBGI_t* o, Thread_t* t) // B0 2A (1.636 on): align → ok
{
	Thread_Push(t, (uint32_t)Edit_SetAlign(o, Thread_Pop(t)));
	return 0;
}

// 1.69 build 472 on: swallow the single-byte printable characters (WM_CHAR 0x20 .. 0x7F) typed into the control
static int Opcode_Ext0_EditRejectAscii(OpenBGI_t* o, Thread_t* t) // B0 29 (1.69/472 on): on →
{
	Edit_RejectAscii(o, (int)Thread_Pop(t));
	return 0;
}

// the glyph width of the entry control's font from the next "B0 20" on, in percent (25 .. 200; 100 the normal); ok 0 out of range
static int Opcode_Ext0_EditFontWidth(OpenBGI_t* o, Thread_t* t) // B0 22 (1.529 on): pct → ok
{
	Thread_Push(t, (uint32_t)Edit_SetFontWidthPct(o, (int)Thread_Pop(t)));
	return 0;
}

/* position, size and show the text entry control with its current text
 * selected; `focus` gives it the keyboard.  A size outside 8 .. the picture
 * size, an unknown font number, a font size outside 8 .. 0x40 and a maxLen
 * outside 1 .. 0x100 are script errors. */
static int Opcode_Ext0_EditSetup(OpenBGI_t* o, Thread_t* t) // B0 20: x, y, w, h, fontNo, size, maxLen, focus →
{
	int focus = (int)Thread_Pop(t);
	int maxLen = (int)Thread_Pop(t);
	int size = (int)Thread_Pop(t);
	int fontNo = (int)Thread_Pop(t);
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	switch(Edit_Setup(o, x, y, w, h, fontNo, size, maxLen, focus))
	{
		case 1: EXT_ERROR(o, t, MSG_BAD_EDIT_SIZE, w, h); break;
		case 2: EXT_ERROR(o, t, MSG_BAD_FONT_NO, fontNo); break;
		case 3: EXT_ERROR(o, t, MSG_BAD_FONT_SIZE, size); break;
		case 4: EXT_ERROR(o, t, MSG_BAD_EDIT_LENGTH, maxLen); break;
		default: break;
	}
	return 0;
}

static int Opcode_Ext0_EditShow(OpenBGI_t* o, Thread_t* t) // B0 24: show →
{
	Edit_Show(o, (int)Thread_Pop(t));
	return 0;
}

// the text colour as 0x00RRGGBB
static int Opcode_Ext0_EditSetColour(OpenBGI_t* o, Thread_t* t) // B0 25: colour →
{
	Edit_SetColour(o, Thread_Pop(t));
	return 0;
}

// the text the next "B0 20" puts into the control
static int Opcode_Ext0_EditSetText(OpenBGI_t* o, Thread_t* t) // B0 26: str →
{
	Edit_SetText(o, (const char*)PopPtr(o, t));
	return 0;
}

// the control's current text into buf (0x100 bytes); pushes its length
static int Opcode_Ext0_EditGetText(OpenBGI_t* o, Thread_t* t) // B0 27: buf → length
{
	Thread_Push(t, (uint32_t)Edit_GetText(o, (char*)PopPtr(o, t)));
	return 0;
}

// -------------------------------------------------------------------------
// message boxes and dialogs
// -------------------------------------------------------------------------

// an information box with the script's text
static int Opcode_Ext0_MsgBoxNotice(OpenBGI_t* o, Thread_t* t) // B0 80: text →
{
	MsgBoxScript(o, (const char*)PopPtr(o, t), MSG_NOTICE, OPENBGI_MB_ICONINFO);
	return 0;
}

/* the yes / no question box of "B0 81" and "B0 82": `defaultYes` selects
 * the default button; pushes 1 when "yes" was chosen */
static int Ext0_AskYesNo(OpenBGI_t* o, Thread_t* t, const char* text, int defaultYes)
{
	uint32_t flags = OPENBGI_MB_YESNO | OPENBGI_MB_ICONQUESTION | (defaultYes ? 0 : OPENBGI_MB_DEFBUTTON2);
	return WaitAsk_Box(o, t, text, MSG_CONFIRM, flags, OPENBGI_IDYES, 1);
}

static int Opcode_Ext0_MsgBoxYesNo(OpenBGI_t* o, Thread_t* t) // B0 81: text, defaultYes → yes
{
	int defaultYes = (int)Thread_Pop(t);
	const char* text = (const char*)PopPtr(o, t);
	return Ext0_AskYesNo(o, t, text, defaultYes);
}

// style 1 asks OK / cancel, anything else yes / no; pushes 1 when confirmed
static int Opcode_Ext0_MsgBoxConfirm(OpenBGI_t* o, Thread_t* t) // B0 82: text, style, defaultYes → confirmed
{
	int defaultYes = (int)Thread_Pop(t);
	int style = (int)Thread_Pop(t);
	const char* text = (const char*)PopPtr(o, t);
	if(style == 1)
	{
		uint32_t flags = OPENBGI_MB_OKCANCEL | OPENBGI_MB_ICONQUESTION | (defaultYes ? 0 : OPENBGI_MB_DEFBUTTON2);
		return WaitAsk_Box(o, t, text, MSG_CONFIRM, flags, OPENBGI_IDOK, 1);
	}
	return Ext0_AskYesNo(o, t, text, defaultYes);
}

// a dialog's request: its answer is the instruction's result
static int Ext0_AskDialog(OpenBGI_t* o, Thread_t* t, uint32_t req)
{
	return WaitAsk_Begin(o, WaitAsk_New(o, t, sizeof(WaitAsk_t), "WaitDialog", NULL, NULL), req);
}

/* a dialog with one text field of at most maxLen characters, preset to
 * `initial` (from 1.616 on a negative maxLen: a digits-only field of
 * -maxLen); `out` (0x100 bytes) receives the entry.  1 ok, 0 cancelled. */
static int Opcode_Ext0_DialogInput1(OpenBGI_t* o, Thread_t* t) // B0 84: out, title, initial, maxLen → ok
{
	int maxLen = (int)Thread_Pop(t);
	const char* initial = (const char*)PopPtr(o, t);
	const char* title = (const char*)PopPtr(o, t);
	char* out = (char*)PopPtr(o, t);
	return Ext0_AskDialog(o, t, Dialog_AskInput1(o, out, title, initial, maxLen));
}

/* a dialog with two labelled text fields; out1 and out2 (0x100 bytes each)
 * receive the entries.  1 ok, 0 cancelled. */
static int Opcode_Ext0_DialogInput2(OpenBGI_t* o, Thread_t* t) // B0 85: out1, out2, title, label1, initial1, maxLen1, label2, initial2, maxLen2 → ok
{
	int maxLen2 = (int)Thread_Pop(t);
	const char* initial2 = (const char*)PopPtr(o, t);
	const char* label2 = (const char*)PopPtr(o, t);
	int maxLen1 = (int)Thread_Pop(t);
	const char* initial1 = (const char*)PopPtr(o, t);
	const char* label1 = (const char*)PopPtr(o, t);
	const char* title = (const char*)PopPtr(o, t);
	char* out2 = (char*)PopPtr(o, t);
	char* out1 = (char*)PopPtr(o, t);
	return Ext0_AskDialog(o, t, Dialog_AskInput2(o, out1, out2, title, label1, initial1, maxLen1, 0, label2, initial2, maxLen2, 0, 0));
}

/* "B0 85" with a layout (non-zero: the two fields side by side, resource
 * 0x7E) and a digits-only switch per field (only digits are typed in, and
 * a '-' while the field is empty).  1 ok, 0 cancelled. */
static int Opcode_Ext0_DialogInput2Ex(OpenBGI_t* o, Thread_t* t) // B0 87 (1.573 on): layout, out1, out2, title, label1, initial1, maxLen1, digits1, label2, initial2, maxLen2, digits2 → ok
{
	int digits2 = (int)Thread_Pop(t);
	int maxLen2 = (int)Thread_Pop(t);
	const char* initial2 = (const char*)PopPtr(o, t);
	const char* label2 = (const char*)PopPtr(o, t);
	int digits1 = (int)Thread_Pop(t);
	int maxLen1 = (int)Thread_Pop(t);
	const char* initial1 = (const char*)PopPtr(o, t);
	const char* label1 = (const char*)PopPtr(o, t);
	const char* title = (const char*)PopPtr(o, t);
	char* out2 = (char*)PopPtr(o, t);
	char* out1 = (char*)PopPtr(o, t);
	int layout = (int)Thread_Pop(t);
	return Ext0_AskDialog(o, t,
		Dialog_AskInput2(o, out1, out2, title, label1, initial1, maxLen1, digits1 != 0, label2, initial2, maxLen2, digits2 != 0, layout != 0));
}

/* 1.616 on: a dialog (resource 0x79 of those builds) that offers the
 * entries of `items` (a text, one entry per line) under `caption` and
 * copies the chosen one into `out` (`size` bytes); 1 chosen, 0 cancelled.
 * The title program of Haruka Ao no Hanayome ni picks the game's
 * language with it after the "change the language?" question.  The
 * dialog is not provided: the operands are dropped and 0 (cancelled) is
 * pushed, so the game keeps its language. */
static int Opcode_Ext0_DialogSelect(OpenBGI_t* o, Thread_t* t) // B0 8C (1.616 on): out, size, caption, items → chosen
{
	PopPtr(o, t); // items
	PopPtr(o, t); // caption
	Thread_Pop(t);
	PopPtr(o, t);
	Thread_Push(t, 0);
	return 0;
}

/* the player profile dialog: four 0x100-byte text buffers that hold the
 * initial texts and receive the entries (surname, given name, nickname,
 * first-person pronoun) and the birthday as 0-based month and day indices,
 * in and out.  Always pushes 1: the dialog has no cancel. */
static int Opcode_Ext0_DialogProfile(OpenBGI_t* o, Thread_t* t) // B0 8F: surname, givenName, nickname, pronoun, &month, &day → ok
{
	int32_t* day = (int32_t*)PopPtr(o, t);
	int32_t* month = (int32_t*)PopPtr(o, t);
	char* pronoun = (char*)PopPtr(o, t);
	char* nickname = (char*)PopPtr(o, t);
	char* givenName = (char*)PopPtr(o, t);
	char* surname = (char*)PopPtr(o, t);
	return Ext0_AskDialog(o, t, Dialog_AskProfile(o, surname, givenName, nickname, pronoun, month, day));
}

// set the desktop wallpaper to the image file at `path`, stretched and / or tiled; the OS result is ignored
static int Opcode_Ext0_SetWallpaper(OpenBGI_t* o, Thread_t* t) // B0 F0: path, stretch, tile →
{
	int tile = (int)Thread_Pop(t);
	int stretch = (int)Thread_Pop(t);
	const char* path = (const char*)PopPtr(o, t);
	SetWallpaper(o, path, stretch, tile);
	return 0;
}

// -------------------------------------------------------------------------
// later additions: the child window's copy text and the font registry
// -------------------------------------------------------------------------

/* 1.69 build 472 on: the text that Ctrl+C in the child window copies to the
 * clipboard.  The handle is popped first, then the text. */
static int Opcode_Ext0_ChildSetCopyText(OpenBGI_t* o, Thread_t* t) // B0 1C (1.69/472 on): text, h →
{
	uint32_t h = Thread_Pop(t);
	const char* text = (const char*)PopPtr(o, t);
	if(!Child_SetCopyText(o, h, text))
		ScriptError(o, MSG_BAD_CHILD_HANDLE, t);
	return 0;
}

/* 1.69 build 472 on: register a font face name; pushes its font number, as
 * the fontNo operands of the text instructions take it */
static int Opcode_Ext0_FontRegister(OpenBGI_t* o, Thread_t* t) // B0 C0 (1.69/472 on): name → no
{
	const char* name = (const char*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)FontNames_Register(o, name, -1));
	return 0;
}

/* 1.529 on: the same with a charset selector (`kind` 0 / 1); the faces
 * are resolved by name, so the selector is passed through unused */
static int Opcode_Ext0_FontRegisterEx(OpenBGI_t* o, Thread_t* t) // B0 C1 (1.529 on): name, kind → no
{
	int kind = (int)Thread_Pop(t);
	const char* name = (const char*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)FontNames_Register(o, name, kind));
	return 0;
}

// 1.69 build 472 on: add a font file from the game directory to the installed faces; 1 when the system took it
static int Opcode_Ext0_FontAddFile(OpenBGI_t* o, Thread_t* t) // B0 C2 (1.69/472 on): file → ok
{
	const char* file = (const char*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)FontNames_AddFile(o, file));
	return 0;
}

// 1.492 on: the same for a font file inside an archive
static int Opcode_Ext0_FontAddFileArc(OpenBGI_t* o, Thread_t* t) // B0 C3 (1.492 on): arc, file → ok
{
	const char* file = (const char*)PopPtr(o, t);
	const char* arc = (const char*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)FontNames_AddFileFromArc(o, arc, file));
	return 0;
}

// 1.535 on: the face used in place of `face` when that one is not installed
static int Opcode_Ext0_FontSetFallback(OpenBGI_t* o, Thread_t* t) // B0 C7 (1.535 on): fallback, face →
{
	const char* face = (const char*)PopPtr(o, t);
	const char* fallback = (const char*)PopPtr(o, t);
	FontNames_SetFallback(o, face, fallback);
	return 0;
}

// 1.529 .. 1.573: 1 when a Shift-JIS face of that name is installed (a NULL name counts as installed)
static int Opcode_Ext0_FontExists(OpenBGI_t* o, Thread_t* t) // B0 C4 (1.529 .. 1.573): face → f
{
	const char* face = (const char*)PopPtr(o, t);
	Thread_Push(t, face ? (uint32_t)Typefaces_Exists(o, face) : 1u);
	return 0;
}

/* 1.588 on: the installed Shift-JIS faces (charset 0x80).  With a NULL
 * buffer the number of bytes their NUL-terminated names take is pushed;
 * otherwise the names are written to the buffer, one after the other, and
 * their count is pushed. */
static int Opcode_Ext0_FontEnum(OpenBGI_t* o, Thread_t* t) // B0 C4 (1.588 on): buf → n
{
	char* buf = (char*)PopPtr(o, t);
	int bytes = 0;
	int n = Typefaces_Enum(o, buf, 0x80, &bytes);
	Thread_Push(t, (uint32_t)(buf ? n : bytes));
	return 0;
}

/* 1.653 on: the same for a given charset (the Windows charset numbers:
 * 0x80 Shift-JIS, 0 ANSI, 0x86 GB2312, 0x88 Big5) */
static int Opcode_Ext0_FontEnumCharset(OpenBGI_t* o, Thread_t* t) // B0 C5 (1.653 on): buf, charset → n
{
	int charset = (int)Thread_Pop(t);
	char* buf = (char*)PopPtr(o, t);
	int bytes = 0;
	int n = Typefaces_Enum(o, buf, charset, &bytes);
	Thread_Push(t, (uint32_t)(buf ? n : bytes));
	return 0;
}

/* 1.547 on: 1 when a face of that name is installed for any of a list of
 * charsets (Shift-JIS then ANSI in 1.547; Shift-JIS, GB2312 and Big5 from
 * 1.653 on, for the Chinese editions), each tried with EnumFontFamiliesEx
 * in the original.  The found face's pitch (lfPitchAndFamily & 3: 0
 * default, 1 fixed, 2 variable) is written to `out`; the font back ends
 * here do not report it, so the default pitch goes there. */
static int Opcode_Ext0_FontExistsEx(OpenBGI_t* o, Thread_t* t) // B0 C6 (1.547 on): out, face → f
{
	const char* face = (const char*)PopPtr(o, t);
	int32_t* out = (int32_t*)PopPtr(o, t);
	int found = Typefaces_Exists(o, face);
	if(found)
		*out = 0;
	Thread_Push(t, (uint32_t)found);
	return 0;
}

// register the handlers of the family (Vm_FillTable picks those of the selected engine profile)
void Vm_OptableB0Init(OpenBGI_t* o)
{
	static const OpEntry_t ops[] = {
		{0x00, Opcode_Ext0_WindowBlit, GEN_FIRST, GEN_LAST},
		{0x02, Opcode_Ext0_WindowCentre, GEN_1_69_472, GEN_LAST},
		{0x03, Opcode_Ext0_WindowSetPos_451, GEN_1_69_451, GEN_1_69_451},
		{0x03, Opcode_Ext0_WindowSetPos, GEN_1_69_472, GEN_LAST},
		{0x04, Opcode_Ext0_CursorSprite, GEN_FIRST, GEN_LAST},
		{0x05, Opcode_Ext0_CursorAutoHide, GEN_FIRST, GEN_LAST},
		{0x06, Opcode_Ext0_CursorIsVisible, GEN_1_529, GEN_LAST},
		{0x08, Opcode_Ext0_Quake, GEN_FIRST, GEN_LAST},
		{0x10, Opcode_Ext0_ChildCreate, GEN_FIRST, GEN_LAST},
		{0x11, Opcode_Ext0_ChildDestroy, GEN_FIRST, GEN_LAST},
		{0x14, Opcode_Ext0_ChildShow, GEN_FIRST, GEN_LAST},
		{0x15, Opcode_Ext0_ChildSetTitle, GEN_FIRST, GEN_LAST},
		{0x16, Opcode_Ext0_ChildMove, GEN_FIRST, GEN_LAST},
		{0x17, Opcode_Ext0_ChildGetPos, GEN_FIRST, GEN_LAST},
		{0x18, Opcode_Ext0_ChildFill, GEN_FIRST, GEN_LAST},
		{0x19, Opcode_Ext0_ChildDrawBitmap, GEN_FIRST, GEN_LAST},
		{0x1A, Opcode_Ext0_ChildDrawText, GEN_FIRST, GEN_LAST},
		{0x1C, Opcode_Ext0_ChildSetCopyText, GEN_1_69_472, GEN_LAST},
		{0xC0, Opcode_Ext0_FontRegister, GEN_1_69_451, GEN_LAST},
		{0xC1, Opcode_Ext0_FontRegisterEx, GEN_1_529, GEN_LAST},
		{0xC2, Opcode_Ext0_FontAddFile, GEN_1_69_472, GEN_LAST},
		{0xC3, Opcode_Ext0_FontAddFileArc, GEN_1_492, GEN_LAST},
		{0xC4, Opcode_Ext0_FontExists, GEN_1_529, GEN_1_573},
		{0xC4, Opcode_Ext0_FontEnum, GEN_1_588, GEN_LAST},
		{0xC5, Opcode_Ext0_FontEnumCharset, GEN_1_653, GEN_LAST},
		{0xC6, Opcode_Ext0_FontExistsEx, GEN_1_547, GEN_LAST},
		{0xC7, Opcode_Ext0_FontSetFallback, GEN_1_535, GEN_LAST},
		{0x20, Opcode_Ext0_EditSetup, GEN_FIRST, GEN_LAST},
		{0x21, Opcode_Ext0_EditDestroy, GEN_1_69_472, GEN_LAST},
		{0x22, Opcode_Ext0_EditFontWidth, GEN_1_529, GEN_LAST},
		{0x23, Opcode_Ext0_EditImeOpen, GEN_1_69_472, GEN_LAST},
		{0x24, Opcode_Ext0_EditShow, GEN_FIRST, GEN_LAST},
		{0x25, Opcode_Ext0_EditSetColour, GEN_FIRST, GEN_LAST},
		{0x26, Opcode_Ext0_EditSetText, GEN_FIRST, GEN_LAST},
		{0x27, Opcode_Ext0_EditGetText, GEN_FIRST, GEN_LAST},
		{0x28, Opcode_Ext0_EditImeCloseOnEnter, GEN_1_69_472, GEN_LAST},
		{0x29, Opcode_Ext0_EditRejectAscii, GEN_1_69_472, GEN_LAST},
		{0x2A, Opcode_Ext0_EditAlign, GEN_1_636, GEN_LAST},
		{0x80, Opcode_Ext0_MsgBoxNotice, GEN_FIRST, GEN_LAST},
		{0x81, Opcode_Ext0_MsgBoxYesNo, GEN_FIRST, GEN_LAST},
		{0x82, Opcode_Ext0_MsgBoxConfirm, GEN_FIRST, GEN_LAST},
		{0x84, Opcode_Ext0_DialogInput1, GEN_FIRST, GEN_LAST},
		{0x85, Opcode_Ext0_DialogInput2, GEN_FIRST, GEN_LAST},
		{0x87, Opcode_Ext0_DialogInput2Ex, GEN_1_573, GEN_LAST},
		{0x8C, Opcode_Ext0_DialogSelect, GEN_1_616, GEN_LAST},
		{0x8F, Opcode_Ext0_DialogProfile, GEN_FIRST, GEN_LAST},
		{0xF0, Opcode_Ext0_SetWallpaper, GEN_FIRST, GEN_LAST},
	};
	Vm_FillTable(o, o->vm.optableB0, OPFAM_B0, ops, BGI_COUNTOF(ops));
}
