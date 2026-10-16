/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ops_ext0.c - the "B0 xx" extension instruction family: direct window
 *              blits, the mouse cursor sprite, the screen quake, child
 *              windows, the text entry control, message boxes, the input
 *              dialogs, the desktop wallpaper and the font registry.
 *              Interface: bgi/vm.h (vm_optable_B0, Vm_OptableB0Init).
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
#include "bgi/os.h"

VmHandler_t vm_optable_B0[256]; // filled by Vm_OptableB0Init for the selected engine profile

// raise a script error with a printf-formatted message; never returns
#define EXT_ERROR(t, ...)           \
	do                              \
	{                               \
		char msg_[0x104];           \
		sprintf(msg_, __VA_ARGS__); \
		ScriptError(msg_, (t));     \
	} while(0)

// -------------------------------------------------------------------------
// window
// -------------------------------------------------------------------------

/* 1.69 build 472 on: move the window to its centred default position;
 * does nothing and pushes 0 in full screen */
static int Opcode_Ext0_WindowCentre(Thread_t* t) // B0 02 (1.69/472 on): → ok
{
	int x, y, ok = !gFullscreen;
	if(ok)
	{
		Display_DefaultWindowPos(&x, &y);
		OS_WindowMove(x, y);
	}
	Thread_Push(t, (uint32_t)ok);
	return 0;
}

// 1.69 build 451 (the retail Tayutama): move the window's outer rectangle to (x, y), in any mode, without a result
static int Opcode_Ext0_WindowSetPos_451(Thread_t* t) // B0 03 (1.69/451): x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	OS_WindowMove(x, y);
	return 0;
}

/* 1.69 build 472 on: move the window's outer rectangle to (x, y) when
 * windowed (nothing in full screen), then report whether the picture area
 * lies entirely on the screen at that position; the frame may hang over
 * the edge.  The test is made in both modes. */
static int Opcode_Ext0_WindowSetPos(Thread_t* t) // B0 03 (1.69/472 on): x, y → inRange
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int sw, sh, fw, fh, pw, ph, inRange;
	if(!gFullscreen)
		OS_WindowMove(x, y);
	OS_ScreenSize(&sw, &sh);
	OS_FrameMetrics(&fw, &fh);
	Display_GetPictureSize(&pw, &ph);
	// the original's bounds from SM_CXFIXEDFRAME / SM_CXSCREEN (the caption height for y): the client area, not the frame, must be inside the screen
	inRange = x >= -fw / 2 && x <= sw - pw - fw / 2 && y >= -OS_FrameTop() && y <= sh - ph - OS_FrameTop();
	BGI_UNUSED(fh);
	Thread_Push(t, (uint32_t)inRange);
	return 0;
}

// draw a managed bitmap directly onto the window at (x, y), outside the compositor
static int Opcode_Ext0_WindowBlit(Thread_t* t) // B0 00: x, y, bmp →
{
	int bmp = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	if(Window_BlitBitmap(x, y, bmp) == 2) // result 1 (no window) is silent
		EXT_ERROR(t, MSG_BMP_NOT_EXIST, bmp);
	return 0;
}

/* show a sprite as the mouse cursor, placed at the pointer position plus
 * (dx, dy); sprite 0 restores the system cursor.  A sprite that cannot be
 * shown is a script error. */
static int Opcode_Ext0_CursorSprite(Thread_t* t) // B0 04: sprite, dx, dy →
{
	int dy = (int)Thread_Pop(t);
	int dx = (int)Thread_Pop(t);
	uint32_t sprite = Thread_Pop(t);
	if(SetCursorSprite(sprite, dx, dy) == -1)
		ScriptError(MSG_BAD_SPRITE_HANDLE, t);
	return 0;
}

// hide the cursor after `ms` milliseconds without mouse movement (0 turns the feature off)
static int Opcode_Ext0_CursorAutoHide(Thread_t* t) // B0 05: ms →
{
	SetCursorAutoHide(Thread_Pop(t));
	return 0;
}

/* 1.529 on: 1 while the cursor is visible, 0 while the auto-hide has it
 * hidden; the system programs count a hover only while it is visible */
static int Opcode_Ext0_CursorIsVisible(Thread_t* t) // B0 06 (1.529 on): → visible
{
	Thread_Push(t, (uint32_t)Cursor_IsVisible());
	return 0;
}

/* start the screen quake and block the thread on its wait object (scheduler
 * code 2).  StartQuake allocates the wait before validating the parameters;
 * a rejected one is raised as a script error, which does not return, so the
 * object is never installed in that case. */
static int Opcode_Ext0_Quake(Thread_t* t) // B0 08: pattern, amplitude, frequency, repeat, decay, fps, skip →
{
	Wait_t* w;
	int skip = (int)Thread_Pop(t);
	int fps = (int)Thread_Pop(t);
	int decay = (int)Thread_Pop(t);
	int repeat = (int)Thread_Pop(t);
	int frequency = (int)Thread_Pop(t);
	int amplitude = (int)Thread_Pop(t);
	int pattern = (int)Thread_Pop(t);
	switch((uint32_t)StartQuake(t, pattern, amplitude, frequency, repeat, decay, fps, skip, &w))
	{
		case 0x80000001u: EXT_ERROR(t, MSG_BAD_QUAKE_PATTERN, pattern); break;
		case 0x80000002u: EXT_ERROR(t, MSG_BAD_FREQUENCY, frequency); break;
		case 0x80000003u: EXT_ERROR(t, MSG_BAD_REPEAT_COUNT, repeat); break;
		case 0x80000004u:
			if(fps < 1)
				EXT_ERROR(t, MSG_BAD_FRAME_RATE, fps);
			else
				EXT_ERROR(t, MSG_BAD_FRAME_RATE_ABOVE_FREQ, fps);
			break;
		default: break;
	}
	Thread_SetWait(t, w);
	return 2;
}

// -------------------------------------------------------------------------
// child windows
// -------------------------------------------------------------------------

/* create a hidden pop-up window with a w x h client area at the screen
 * position (x, y) and push its handle; a size outside 0x20 .. 0x400 or all
 * eight slots in use is a script error */
static int Opcode_Ext0_ChildCreate(Thread_t* t) // B0 10: title, x, y, w, h → handle
{
	uint32_t handle = 0;
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	const char* title = (const char*)PopPtr(t);
	switch(Child_Create(&handle, title, x, y, w, h))
	{
		case 0x80000001u: EXT_ERROR(t, MSG_BAD_CHILD_SIZE, w, h); break;
		case 0x80000002u: ScriptError(MSG_NO_MORE_CHILDREN, t); break;
		default: break;
	}
	Thread_Push(t, handle);
	return 0;
}

static int Opcode_Ext0_ChildDestroy(Thread_t* t) // B0 11: handle →
{
	if(!Child_Destroy(Thread_Pop(t)))
		ScriptError(MSG_BAD_CHILD_HANDLE2, t); // the original's Japanese message here reads "child child window"
	return 0;
}

static int Opcode_Ext0_ChildShow(Thread_t* t) // B0 14: handle, show →
{
	int show = (int)Thread_Pop(t);
	uint32_t handle = Thread_Pop(t);
	if(!Child_Show(handle, show))
		ScriptError(MSG_BAD_CHILD_HANDLE, t);
	return 0;
}

static int Opcode_Ext0_ChildSetTitle(Thread_t* t) // B0 15: handle, title →
{
	const char* title = (const char*)PopPtr(t);
	uint32_t handle = Thread_Pop(t);
	if(!Child_SetTitle(handle, title))
		ScriptError(MSG_BAD_CHILD_HANDLE, t);
	return 0;
}

// move the child's outer rectangle to the screen position (x, y)
static int Opcode_Ext0_ChildMove(Thread_t* t) // B0 16: handle, x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t handle = Thread_Pop(t);
	if(!Child_Move(handle, x, y))
		ScriptError(MSG_BAD_CHILD_HANDLE, t);
	return 0;
}

// the child's screen position as the window manager reports it
static int Opcode_Ext0_ChildGetPos(Thread_t* t) // B0 17: handle → x, y
{
	int32_t pos[2] = {0, 0};
	if(!Child_GetPos(pos, Thread_Pop(t)))
		ScriptError(MSG_BAD_CHILD_HANDLE, t);
	Thread_Push(t, (uint32_t)pos[0]);
	Thread_Push(t, (uint32_t)pos[1]);
	return 0;
}

// fill the child's back buffer with a colour and present it
static int Opcode_Ext0_ChildFill(Thread_t* t) // B0 18: handle, colour →
{
	uint32_t colour = Thread_Pop(t);
	uint32_t handle = Thread_Pop(t);
	if(!Child_Fill(handle, colour))
		ScriptError(MSG_BAD_CHILD_HANDLE, t);
	return 0;
}

/* blit a managed bitmap into the child's back buffer at (x, y) with an
 * effect mode and a level, then present it */
static int Opcode_Ext0_ChildDrawBitmap(Thread_t* t) // B0 19: handle, x, y, bmp, effect, level →
{
	int level = (int)Thread_Pop(t);
	int effect = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t handle = Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	CheckEffectMode((uint32_t)effect, t);
	CheckAlpha((uint32_t)level, t);
	switch(Child_DrawBitmap(handle, x, y, bmp, effect, level))
	{
		case 0xffffffffu: ScriptError(MSG_BAD_CHILD_HANDLE, t); break;
		case 0x80000003u: EXT_ERROR(t, MSG_BMP_NOT_EXIST, bmp); break;
		case 0x80000004u: EXT_ERROR(t, MSG_CHILD_BMP_MODE, bmp); break;
		case 0x80000007u: EXT_ERROR(t, MSG_CHILD_OUT_OF_CLIENT, x, y); break;
		default: break; // the effect and the level were validated above; the other results are silent
	}
	return 0;
}

/* draw text into the child window with the bitmap manager's font cache and
 * present it; pushes the width of the widest line.  `widthPct` is the
 * horizontal scale in per cent, `prop` selects proportional spacing. */
static int Opcode_Ext0_ChildDrawText(Thread_t* t) // B0 1A: handle, x, y, str, fontNo, size, widthPct, bold, prop, colour → width
{
	int32_t measure = 0;
	uint32_t colour = Thread_Pop(t);
	int prop = (int)Thread_Pop(t);
	int bold = (int)Thread_Pop(t);
	int widthPct = (int)Thread_Pop(t);
	int size = (int)Thread_Pop(t);
	int fontNo = (int)Thread_Pop(t);
	const char* str = (const char*)PopPtr(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t handle = Thread_Pop(t);
	CheckFontNo(fontNo, t);
	switch(Child_DrawText(handle, x, y, str, fontNo, size, widthPct, bold, prop, colour, &measure))
	{
		case 0xffffffffu: ScriptError(MSG_BAD_CHILD_HANDLE, t); break;
		case 0x80000009u: EXT_ERROR(t, MSG_FONT_SIZE_INVALID, size); break;
		case 0x8000000au: EXT_ERROR(t, MSG_FONT_WIDTH_INVALID, widthPct); break;
		case 0x8000000bu: EXT_ERROR(t, MSG_FONT_NO_INVALID, fontNo); break;
		default: break;
	}
	Thread_Push(t, (uint32_t)measure);
	return 0;
}

// -------------------------------------------------------------------------
// the text entry control
// -------------------------------------------------------------------------

/* 1.69 build 472 on: that build creates the edit control on demand and this
 * instruction destroys it (the subclass, the font and the window); here the
 * control lives with the main window and is only hidden.  Always pushes 0. */
static int Opcode_Ext0_EditDestroy(Thread_t* t) // B0 21 (1.69/472 on): → 0
{
	Edit_Show(0);
	Thread_Push(t, 0);
	return 0;
}

// 1.69 build 472 on: the IME open state the control was given
static int Opcode_Ext0_EditImeOpen(Thread_t* t) // B0 23 (1.69/472 on): → open
{
	Thread_Push(t, (uint32_t)OS_EditImeOpen());
	return 0;
}

/* 1.69 build 472 on: whether the control's subclass closes the IME when
 * Enter ends the entry.  The flag is only stored here: the IME belongs to
 * the OS layer, which does not consult it. */
static int gEditImeCloseOnEnter;                        // "B0 28": stored, not acted on
static int Opcode_Ext0_EditImeCloseOnEnter(Thread_t* t) // B0 28 (1.69/472 on): on →
{
	gEditImeCloseOnEnter = (int)Thread_Pop(t);
	return 0;
}

// 1.69 build 472 on: swallow the single-byte printable characters (WM_CHAR 0x20 .. 0x7F) typed into the control
static int Opcode_Ext0_EditRejectAscii(Thread_t* t) // B0 29 (1.69/472 on): on →
{
	OS_EditRejectAscii((int)Thread_Pop(t));
	return 0;
}

/* position, size and show the text entry control with its current text
 * selected; `focus` gives it the keyboard.  A size outside 8 .. the picture
 * size, an unknown font number, a font size outside 8 .. 0x40 and a maxLen
 * outside 1 .. 0x100 are script errors. */
static int Opcode_Ext0_EditSetup(Thread_t* t) // B0 20: x, y, w, h, fontNo, size, maxLen, focus →
{
	int focus = (int)Thread_Pop(t);
	int maxLen = (int)Thread_Pop(t);
	int size = (int)Thread_Pop(t);
	int fontNo = (int)Thread_Pop(t);
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	switch(Edit_Setup(x, y, w, h, fontNo, size, maxLen, focus))
	{
		case 1: EXT_ERROR(t, MSG_BAD_EDIT_SIZE, w, h); break;
		case 2: EXT_ERROR(t, MSG_BAD_FONT_NO, fontNo); break;
		case 3: EXT_ERROR(t, MSG_BAD_FONT_SIZE, size); break;
		case 4: EXT_ERROR(t, MSG_BAD_EDIT_LENGTH, maxLen); break;
		default: break;
	}
	return 0;
}

static int Opcode_Ext0_EditShow(Thread_t* t) // B0 24: show →
{
	Edit_Show((int)Thread_Pop(t));
	return 0;
}

// the text colour as 0x00RRGGBB
static int Opcode_Ext0_EditSetColour(Thread_t* t) // B0 25: colour →
{
	Edit_SetColour(Thread_Pop(t));
	return 0;
}

// the text the next "B0 20" puts into the control
static int Opcode_Ext0_EditSetText(Thread_t* t) // B0 26: str →
{
	Edit_SetText((const char*)PopPtr(t));
	return 0;
}

// the control's current text into buf (0x100 bytes); pushes its length
static int Opcode_Ext0_EditGetText(Thread_t* t) // B0 27: buf → length
{
	Thread_Push(t, (uint32_t)Edit_GetText((char*)PopPtr(t)));
	return 0;
}

// -------------------------------------------------------------------------
// message boxes and dialogs
// -------------------------------------------------------------------------

// an information box with the script's text
static int Opcode_Ext0_MsgBoxNotice(Thread_t* t) // B0 80: text →
{
	MsgBoxScript((const char*)PopPtr(t), MSG_NOTICE, OS_MB_ICONINFO);
	return 0;
}

/* the yes / no question box of "B0 81" and "B0 82": `defaultYes` selects
 * the default button; 1 when "yes" was chosen */
static uint32_t Ext0_AskYesNo(const char* text, int defaultYes)
{
	uint32_t flags = OS_MB_YESNO | OS_MB_ICONQUESTION | (defaultYes ? 0 : OS_MB_DEFBUTTON2);
	return MsgBoxScript(text, MSG_CONFIRM, flags) == OS_IDYES;
}

static int Opcode_Ext0_MsgBoxYesNo(Thread_t* t) // B0 81: text, defaultYes → yes
{
	int defaultYes = (int)Thread_Pop(t);
	const char* text = (const char*)PopPtr(t);
	Thread_Push(t, Ext0_AskYesNo(text, defaultYes));
	return 0;
}

// style 1 asks OK / cancel, anything else yes / no; pushes 1 when confirmed
static int Opcode_Ext0_MsgBoxConfirm(Thread_t* t) // B0 82: text, style, defaultYes → confirmed
{
	int defaultYes = (int)Thread_Pop(t);
	int style = (int)Thread_Pop(t);
	const char* text = (const char*)PopPtr(t);
	if(style == 1)
	{
		uint32_t flags = OS_MB_OKCANCEL | OS_MB_ICONQUESTION | (defaultYes ? 0 : OS_MB_DEFBUTTON2);
		Thread_Push(t, MsgBoxScript(text, MSG_CONFIRM, flags) == OS_IDOK);
	}
	else
		Thread_Push(t, Ext0_AskYesNo(text, defaultYes));
	return 0;
}

/* a dialog with one text field of at most maxLen characters, preset to
 * `initial`; `out` (0x100 bytes) receives the entry.  1 ok, 0 cancelled. */
static int Opcode_Ext0_DialogInput1(Thread_t* t) // B0 84: out, title, initial, maxLen → ok
{
	int maxLen = (int)Thread_Pop(t);
	const char* initial = (const char*)PopPtr(t);
	const char* title = (const char*)PopPtr(t);
	char* out = (char*)PopPtr(t);
	Thread_Push(t, (uint32_t)Dialog_Input1(out, title, initial, maxLen));
	return 0;
}

/* a dialog with two labelled text fields; out1 and out2 (0x100 bytes each)
 * receive the entries.  1 ok, 0 cancelled. */
static int Opcode_Ext0_DialogInput2(Thread_t* t) // B0 85: out1, out2, title, label1, initial1, maxLen1, label2, initial2, maxLen2 → ok
{
	int maxLen2 = (int)Thread_Pop(t);
	const char* initial2 = (const char*)PopPtr(t);
	const char* label2 = (const char*)PopPtr(t);
	int maxLen1 = (int)Thread_Pop(t);
	const char* initial1 = (const char*)PopPtr(t);
	const char* label1 = (const char*)PopPtr(t);
	const char* title = (const char*)PopPtr(t);
	char* out2 = (char*)PopPtr(t);
	char* out1 = (char*)PopPtr(t);
	Thread_Push(t, (uint32_t)Dialog_Input2(out1, out2, title, label1, initial1, maxLen1, label2, initial2, maxLen2));
	return 0;
}

/* 1.616 on: a dialog (resource 0x79 of those builds) that offers the
 * entries of `items` (a text, one entry per line) under `caption` and
 * copies the chosen one into `out` (`size` bytes); 1 chosen, 0 cancelled.
 * The title program of Haruka Ao no Hanayome ni picks the game's
 * language with it after the "change the language?" question.  The
 * dialog is not provided: the operands are dropped and 0 (cancelled) is
 * pushed, so the game keeps its language. */
static int Opcode_Ext0_DialogSelect(Thread_t* t) // B0 8C (1.616 on): out, size, caption, items → chosen
{
	PopPtr(t); // items
	PopPtr(t); // caption
	Thread_Pop(t);
	PopPtr(t);
	Thread_Push(t, 0);
	return 0;
}

/* the player profile dialog: four 0x100-byte text buffers that hold the
 * initial texts and receive the entries (surname, given name, nickname,
 * first-person pronoun) and the birthday as 0-based month and day indices,
 * in and out.  Always pushes 1: the dialog has no cancel. */
static int Opcode_Ext0_DialogProfile(Thread_t* t) // B0 8F: surname, givenName, nickname, pronoun, &month, &day → ok
{
	int32_t* day = (int32_t*)PopPtr(t);
	int32_t* month = (int32_t*)PopPtr(t);
	char* pronoun = (char*)PopPtr(t);
	char* nickname = (char*)PopPtr(t);
	char* givenName = (char*)PopPtr(t);
	char* surname = (char*)PopPtr(t);
	Thread_Push(t, (uint32_t)Dialog_Profile(surname, givenName, nickname, pronoun, month, day));
	return 0;
}

// set the desktop wallpaper to the image file at `path`, stretched and / or tiled; the OS result is ignored
static int Opcode_Ext0_SetWallpaper(Thread_t* t) // B0 F0: path, stretch, tile →
{
	int tile = (int)Thread_Pop(t);
	int stretch = (int)Thread_Pop(t);
	const char* path = (const char*)PopPtr(t);
	SetWallpaper(path, stretch, tile);
	return 0;
}

// -------------------------------------------------------------------------
// later additions: the child window's copy text and the font registry
// -------------------------------------------------------------------------

/* 1.69 build 472 on: the text that Ctrl+C in the child window copies to the
 * clipboard.  The handle is popped first, then the text. */
static int Opcode_Ext0_ChildSetCopyText(Thread_t* t) // B0 1C (1.69/472 on): text, h →
{
	uint32_t h = Thread_Pop(t);
	const char* text = (const char*)PopPtr(t);
	if(!Child_SetCopyText(h, text))
		ScriptError(MSG_BAD_CHILD_HANDLE, t);
	return 0;
}

/* 1.69 build 472 on: register a font face name; pushes its font number, as
 * the fontNo operands of the text instructions take it */
static int Opcode_Ext0_FontRegister(Thread_t* t) // B0 C0 (1.69/472 on): name → no
{
	const char* name = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)FontNames_Register(name, -1));
	return 0;
}

/* 1.529 on: the same with a charset selector (`kind` 0 / 1); the OS layer
 * resolves faces by name, so the selector is passed through unused */
static int Opcode_Ext0_FontRegisterEx(Thread_t* t) // B0 C1 (1.529 on): name, kind → no
{
	int kind = (int)Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)FontNames_Register(name, kind));
	return 0;
}

// 1.69 build 472 on: add a font file from the game directory to the installed faces; 1 when the system took it
static int Opcode_Ext0_FontAddFile(Thread_t* t) // B0 C2 (1.69/472 on): file → ok
{
	const char* file = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)FontNames_AddFile(file));
	return 0;
}

// 1.494 on: the same for a font file inside an archive
static int Opcode_Ext0_FontAddFileArc(Thread_t* t) // B0 C3 (1.494 on): arc, file → ok
{
	const char* file = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)FontNames_AddFileFromArc(arc, file));
	return 0;
}

// 1.535 on: the face used in place of `face` when that one is not installed
static int Opcode_Ext0_FontSetFallback(Thread_t* t) // B0 C7 (1.535 on): fallback, face →
{
	const char* face = (const char*)PopPtr(t);
	const char* fallback = (const char*)PopPtr(t);
	FontNames_SetFallback(face, fallback);
	return 0;
}

// 1.529 .. 1.573: 1 when a Shift-JIS face of that name is installed (a NULL name counts as installed)
static int Opcode_Ext0_FontExists(Thread_t* t) // B0 C4 (1.529 .. 1.573): face → f
{
	const char* face = (const char*)PopPtr(t);
	Thread_Push(t, face ? (uint32_t)OS_FontFaceExists(face) : 1u);
	return 0;
}

/* 1.588 on: the installed Shift-JIS faces (charset 0x80).  With a NULL
 * buffer the number of bytes their NUL-terminated names take is pushed;
 * otherwise the names are written to the buffer, one after the other, and
 * their count is pushed. */
static int Opcode_Ext0_FontEnum(Thread_t* t) // B0 C4 (1.588 on): buf → n
{
	char* buf = (char*)PopPtr(t);
	int bytes = 0;
	int n = OS_FontEnumFaces(buf, 0x80, &bytes);
	Thread_Push(t, (uint32_t)(buf ? n : bytes));
	return 0;
}

/* 1.653 on: the same for a given charset (the Windows charset numbers:
 * 0x80 Shift-JIS, 0 ANSI, 0x86 Big5, 0x88 GB2312) */
static int Opcode_Ext0_FontEnumCharset(Thread_t* t) // B0 C5 (1.653 on): buf, charset → n
{
	int charset = (int)Thread_Pop(t);
	char* buf = (char*)PopPtr(t);
	int bytes = 0;
	int n = OS_FontEnumFaces(buf, charset, &bytes);
	Thread_Push(t, (uint32_t)(buf ? n : bytes));
	return 0;
}

/* 1.547 on: 1 when a face of that name is installed for any of a list of
 * charsets (Shift-JIS then ANSI in 1.547; Shift-JIS, GB2312 and Big5 from
 * 1.653 on, for the Chinese editions), each tried with EnumFontFamiliesEx
 * in the original.  The found face's pitch (lfPitchAndFamily & 3: 0
 * default, 1 fixed, 2 variable) is written to `out`; the font back ends
 * here do not report it, so the default pitch goes there. */
static int Opcode_Ext0_FontExistsEx(Thread_t* t) // B0 C6 (1.547 on): out, face → f
{
	const char* face = (const char*)PopPtr(t);
	int32_t* out = (int32_t*)PopPtr(t);
	int found = OS_FontFaceExists(face);
	if(found)
		*out = 0;
	Thread_Push(t, (uint32_t)found);
	return 0;
}

// register the handlers of the family (Vm_FillTable picks those of the selected engine profile)
void Vm_OptableB0Init(void)
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
		{0xC3, Opcode_Ext0_FontAddFileArc, GEN_1_494, GEN_LAST},
		{0xC4, Opcode_Ext0_FontExists, GEN_1_529, GEN_1_573},
		{0xC4, Opcode_Ext0_FontEnum, GEN_1_588, GEN_LAST},
		{0xC5, Opcode_Ext0_FontEnumCharset, GEN_1_653, GEN_LAST},
		{0xC6, Opcode_Ext0_FontExistsEx, GEN_1_547, GEN_LAST},
		{0xC7, Opcode_Ext0_FontSetFallback, GEN_1_535, GEN_LAST},
		{0x20, Opcode_Ext0_EditSetup, GEN_FIRST, GEN_LAST},
		{0x21, Opcode_Ext0_EditDestroy, GEN_1_69_472, GEN_LAST},
		{0x23, Opcode_Ext0_EditImeOpen, GEN_1_69_472, GEN_LAST},
		{0x24, Opcode_Ext0_EditShow, GEN_FIRST, GEN_LAST},
		{0x25, Opcode_Ext0_EditSetColour, GEN_FIRST, GEN_LAST},
		{0x26, Opcode_Ext0_EditSetText, GEN_FIRST, GEN_LAST},
		{0x27, Opcode_Ext0_EditGetText, GEN_FIRST, GEN_LAST},
		{0x28, Opcode_Ext0_EditImeCloseOnEnter, GEN_1_69_472, GEN_LAST},
		{0x29, Opcode_Ext0_EditRejectAscii, GEN_1_69_472, GEN_LAST},
		{0x80, Opcode_Ext0_MsgBoxNotice, GEN_FIRST, GEN_LAST},
		{0x81, Opcode_Ext0_MsgBoxYesNo, GEN_FIRST, GEN_LAST},
		{0x82, Opcode_Ext0_MsgBoxConfirm, GEN_FIRST, GEN_LAST},
		{0x84, Opcode_Ext0_DialogInput1, GEN_FIRST, GEN_LAST},
		{0x85, Opcode_Ext0_DialogInput2, GEN_FIRST, GEN_LAST},
		{0x8C, Opcode_Ext0_DialogSelect, GEN_1_616, GEN_LAST},
		{0x8F, Opcode_Ext0_DialogProfile, GEN_FIRST, GEN_LAST},
		{0xF0, Opcode_Ext0_SetWallpaper, GEN_FIRST, GEN_LAST},
	};
	Vm_FillTable(vm_optable_B0, OPFAM_B0, ops, BGI_COUNTOF(ops));
}
