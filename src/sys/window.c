/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.c - the main window: its creation, its state as the engine sees
 *            it, the input and the events the host hands over
 *            (OpenBGI_Key, OpenBGI_Close and their kin) and the end of
 *            the message pump the host runs between frames; interface in
 *            bgi/display.h and openbgi.h
 *
 * The host owns the native window (mainwin.c asks it for what the
 * original did with its own); this file keeps the engine's view of it
 * (active, minimised, alive), the messages the engine posts itself (a
 * close, the quit), and turns the window's input and events into what
 * the engine does with them: key and button presses go
 * to the input state (input.c) and the script event queue
 * (sysobj/events.h), the raw message numbers to the message waits ("80
 * 54", winmsg.c), the display changes to display.c.  The state of the
 * keys and the pointer is what the host reported, kept here
 * (o->input.keyboard, pointerX / pointerY); nothing is read from the
 * system.  The display mode lives in display.c, the presenting in
 * present.c.
 */
#include "bgi/display.h"
#include "bgi/edit.h"
#include "bgi/snd/wavemaster.h"
#include "bgi/engine.h"
#include "bgi/input.h"
#include "bgi/sysobj.h"
#include "bgi/sound.h"
#include "bgi/gfx.h"
#include "bgi/error.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/vm.h"
#include "bgi/dbg.h"
#include "openbgi.h"
#include "bgi/inframe.h"
#include "bgi/launcher.h"
#include "bgi/host.h"
#include "bgi/sjis.h"
#include "bgi/core.h"
#include "bgi/release.h"

// ---- the events ------------------------------------------------------------------------

// the Windows message numbers the scripts may wait for ("80 54")
#define WM_CLOSE_       0x010
#define WM_KEYDOWN_     0x100
#define WM_KEYUP_       0x101
#define WM_LBUTTONDOWN_ 0x201
#define WM_LBUTTONUP_   0x202
#define WM_RBUTTONDOWN_ 0x204
#define WM_RBUTTONUP_   0x205
#define WM_MBUTTONDOWN_ 0x207
#define WM_MBUTTONUP_   0x208
#define WM_MOUSEWHEEL_  0x20a
#define WM_XBUTTONDOWN_ 0x20b
#define WM_XBUTTONUP_   0x20c

/* A key press (WM_KEYDOWN / WM_SYSKEYDOWN; `repeat` for the keyboard's
 * auto-repeat): the message waits, the input state, event 3 for a fresh
 * press, and the display style toggle when the key is a style key. */
static void Wnd_KeyDown(OpenBGI_t* o, int vk, int repeat)
{
	if(vk == 0x7b && !repeat)
	{ // F12: the debugger (not an input the scripts see)
		Dbg_Toggle(o);
		return;
	}
	if(Dbg_OverlayKey(o, vk, 1))
		return; // the debugger draws over the game and takes its keys
	WinMsgWait_Dispatch(o, WM_KEYDOWN_, (uint32_t)vk, 0);
	if(Input_OnKeyDown(o, vk, repeat))
	{
		InputSerial_Bump(o);
		MsgQueue_Post(o, 3, (uint32_t)vk, 0);
	}
	if(Display_StyleKeyMatch(o, (uint32_t)vk))
		Display_ToggleStyle(o);
}

// A key release: the message waits and the input state (F12 and the debugger's keys excepted).
static void Wnd_KeyUp(OpenBGI_t* o, int vk)
{
	if(vk == 0x7b || Dbg_OverlayKey(o, vk, 0))
		return;
	WinMsgWait_Dispatch(o, WM_KEYUP_, (uint32_t)vk, 0);
	Input_OnKeyUp(o, vk);
}

/* Alt+key combinations: F10 counts as a key, Alt+Enter toggles the style
 * (not while minimised), Alt+F4 closes; anything else only bumps the
 * input serial. */
static void Wnd_SysKey(OpenBGI_t* o, int vk)
{
	if(vk == 0x79)
	{
		if(Input_OnKeyDown(o, vk, 0))
		{
			InputSerial_Bump(o);
			MsgQueue_Post(o, 3, (uint32_t)vk, 0);
		}
		return;
	}
	InputSerial_Bump(o);
	if(vk == 0x0d)
	{
		if(o->display.windowRestored)
			Display_ToggleStyle(o);
	}
	else if(vk == 0x73)
	{
		MainWindow_PostClose(o);
	}
}

/* A mouse button (0 left, 1 right, 2 middle, 3 / 4 the X buttons) went
 * down or up at client position (x, y).  Presses post event 3 with the
 * virtual key (1, 2, 4, 5, 6); a left press on a knob starts a drag, a
 * right press flags the knob, and either is counted but not seen as a
 * key.  With the buttons swapped ("80 1E") the right button is handled
 * as the left one; every right press posts event 0x81 regardless. */
static void Wnd_MouseButton(OpenBGI_t* o, int button, int down, int x, int y)
{
	uint32_t lp = ((uint32_t)y << 16) | (x & 0xffff);
	if(Dbg_OverlayMouse(o, x, y, button, down))
		return;
	switch(button)
	{
		case 0: // left
			if(down)
			{
				WinMsgWait_Dispatch(o, WM_LBUTTONDOWN_, 0, lp);
				InputSerial_Bump(o);
				MsgQueue_Post(o, 3, 1, 0);
				if(Knob_UnderMouse(o))
				{
					int pos[2];
					GetMouseClientPos(o, pos);
					Knob_BeginDrag(o, Knob_UnderMouse(o), pos[0], pos[1]);
					Input_CountPress(o, 1);
					o->display.knobDragL = 1;
				}
				else
				{
					Input_OnKeyDown(o, 1, 0);
					o->display.knobDragL = 0;
				}
			}
			else
			{
				WinMsgWait_Dispatch(o, WM_LBUTTONUP_, 0, lp);
				if(o->display.knobDragL)
					o->display.knobDragL = 0;
				else
					Input_OnKeyUp(o, 1);
			}
			break;
		case 1: // right
			if(down)
			{
				WinMsgWait_Dispatch(o, WM_RBUTTONDOWN_, 0, lp);
				MsgQueue_Post(o, 0x81, 0, 0);
				if(Input_SwapButtons(o) == 1)
				{ // right acts as left
					Wnd_MouseButton(o, 0, 1, x, y);
					break;
				}
				if(Input_SwapButtons(o) != 0)
					break;
				InputSerial_Bump(o);
				MsgQueue_Post(o, 3, 2, 0);
				if(Knob_UnderMouse(o))
				{
					Knob_MarkChanged(Knob_UnderMouse(o));
					Input_CountPress(o, 2);
					o->display.knobDragR = 1;
				}
				else
				{
					Input_OnKeyDown(o, 2, 0);
					o->display.knobDragR = 0;
				}
			}
			else
			{
				WinMsgWait_Dispatch(o, WM_RBUTTONUP_, 0, lp);
				if(Input_SwapButtons(o) == 1)
				{
					Wnd_MouseButton(o, 0, 0, x, y);
					break;
				}
				if(Input_SwapButtons(o) != 0)
					break;
				if(o->display.knobDragR)
					o->display.knobDragR = 0;
				else
					Input_OnKeyUp(o, 2);
			}
			break;
		case 2: // middle
			if(down)
			{
				WinMsgWait_Dispatch(o, WM_MBUTTONDOWN_, 0, lp);
				InputSerial_Bump(o);
				MsgQueue_Post(o, 3, 4, 0);
				Input_OnKeyDown(o, 4, 0);
			}
			else
			{
				WinMsgWait_Dispatch(o, WM_MBUTTONUP_, 0, lp);
				Input_OnKeyUp(o, 4);
			}
			break;
		case 3:
		case 4:
		{ // X buttons -> vk 5 / 6; the wait's wParam carries the button number in its high word
			int vk = button == 3 ? 5 : 6;
			if(down)
			{
				WinMsgWait_Dispatch(o, WM_XBUTTONDOWN_, (uint32_t)(button - 2) << 16, lp);
				InputSerial_Bump(o);
				MsgQueue_Post(o, 3, (uint32_t)vk, 0);
				Input_OnKeyDown(o, vk, 0);
			}
			else
			{
				WinMsgWait_Dispatch(o, WM_XBUTTONUP_, (uint32_t)(button - 2) << 16, lp);
				Input_OnKeyUp(o, vk);
			}
			break;
		}
		default:
			break;
	}
}

/* "81 1E" of 1.529 on: a click of a mouse button at the cursor, handled
 * as a press and a release of the button: kind 1 left, 2 right, 4
 * middle, 5 / 6 the X buttons; 1 when the kind is one of these, 0
 * otherwise. */
int Window_SimulateClick(OpenBGI_t* o, int kind)
{
	static const int buttons[7] = {-1, 0, 1, -1, 2, 3, 4};
	int pos[2];
	if((unsigned)kind >= 7 || buttons[kind] < 0)
		return 0;
	GetMouseClientPos(o, pos);
	Wnd_MouseButton(o, buttons[kind], 1, pos[0], pos[1]);
	Wnd_MouseButton(o, buttons[kind], 0, pos[0], pos[1]);
	return 1;
}

// A double click: event 0x80 for the left button, 0x81 for any other.
static void Wnd_DoubleClick(OpenBGI_t* o, int button)
{
	MsgQueue_Post(o, button == 0 ? 0x80u : 0x81u, 0, 0);
}

/* The wheel: a press and release of virtual key 0x0e (up) or 0x0f (down)
 * and event 3 with it, or a nudge of the captured knob while the "90 DD"
 * switch is on. */
static void Wnd_MouseWheel(OpenBGI_t* o, int delta)
{
	int vk;
	if(delta == 0 || Dbg_OverlayWheel(o, delta))
		return;
	WinMsgWait_Dispatch(o, WM_MOUSEWHEEL_, (uint32_t)(delta & 0xffff) << 16, 0);
	InputSerial_Bump(o);
	vk = delta < 0 ? 0x0f : 0x0e;
	MsgQueue_Post(o, 3, (uint32_t)vk, 0);
	if(Knob_GetGlobal(o) && Knob_TopCaptured(o))
	{
		Knob_NudgeCaptured(o, delta < 0);
		return;
	}
	Input_OnKeyDown(o, vk, 0);
	Input_OnKeyUp(o, vk);
}

/* The window gained or lost the focus.  From 1.535 on, once the engine is
 * up, the clock stops while the window is inactive when "81 68" asked for
 * that, and from 1.547 on the sound is held with it (Sound_HoldAll; the
 * activation lets whatever was held play on).  Coming back after a
 * scripted minimise ("80 65") resumes the sound and restores the display
 * mode. */
static void Wnd_Activate(OpenBGI_t* o, int active)
{
	o->display.windowActive = active != 0;
	if(o->engine.profile->gen >= GEN_1_535 && o->engine.engineUp)
	{
		if(active)
		{
			Clock_Resume(o);
			if(o->engine.profile->gen >= GEN_1_547)
				Sound_HoldAll(o, 0);
		}
		else
		{
			Clock_Stop(o, 0);
			if(o->engine.profile->gen >= GEN_1_547 && Clock_StopsWhenInactive(o))
				Sound_HoldAll(o, 1);
		}
	}
	if(active && o->display.minimizedByScript && !MainWin_Minimized(o))
	{
		Sound_ResumeAll(o);
		SetDisplayMode(o, o->display.sizeIndex, o->display.pixelMode, o->display.fullscreen);
		o->display.minimizedByScript = 0;
	}
}

// The window was minimised or restored.
static void Wnd_Sized(OpenBGI_t* o, int minimized)
{
	o->display.windowRestored = !minimized;
}

/* The close box (or Alt+F4): in close mode 0 the script decides (event
 * 2), otherwise the text entry control and the window are destroyed. */
static void Wnd_CloseRequest(OpenBGI_t* o)
{
	WinMsgWait_Dispatch(o, WM_CLOSE_, 0, 0);
	InputSerial_Bump(o);
	if(o->display.closeMode == 0)
	{
		MsgQueue_Post(o, 2, 0, 0); // the script decides
		return;
	}
	Edit_Destroy(o);
	MainWindow_Destroy(o);
}

/* A file was dropped on the window, `path` as the host spells it.  The
 * launcher takes it so (Launcher_Drop).  A game's scripts get event 0x10
 * and the path for "80 6D" in their own form: the same where the host's
 * paths are the engine's (Host_NativeSjis), else its Shift-JIS spelling. */
static void Wnd_DropFile(OpenBGI_t* o, const char* path)
{
	char engine[0x400];
	if(o->launcher.running)
	{
		Launcher_Drop(o, path);
		return;
	}
	if(Host_NativeSjis(o))
		snprintf(engine, sizeof engine, "%s", path);
	else
		Sjis_FromUtf8(path, engine, sizeof engine);
	MsgQueue_Post(o, 0x10, 0, 0);
	DragDrop_OnFile(o, engine);
}

// Any other message: only the message waits see it.
static void Wnd_RawMessage(OpenBGI_t* o, uint32_t msg, uint32_t wParam, uint32_t lParam)
{
	WinMsgWait_Dispatch(o, msg, wParam, lParam);
}

// The message loop ended: throw 0 out of the pump (OpenBGI_PumpEvents catches it).
static void Wnd_Quit(OpenBGI_t* o)
{
	BGI_ThrowInt(o, 0); // WM_QUIT: "throw 0" in the original's pump
}

static void Wnd_DisplayChange(OpenBGI_t* o)
{
	Display_OnDisplayChange(o);
}

// MM_MCINOTIFY (0x3b9): a CD track that was started with the notification ended: replay it
static void Wnd_MciNotify(OpenBGI_t* o, int status)
{
	if(status == 1)
		Cd_Replay(o);
}

// the pointer moved: the debugger hears of it when it draws over the game
static void Wnd_MouseMove(OpenBGI_t* o, int x, int y)
{
	Dbg_OverlayMouse(o, x, y, -1, 0);
}

// ---- the input (openbgi.h) ------------------------------------------------------------------

static const int kButtonVk[5] = {1, 2, 4, 5, 6}; // the virtual key of each button (VK_LBUTTON ..)

/* A key or button the host reported, into the keyboard state: bit 7 while
 * it is down, bit 0 flipped by every press (GetKeyboardState's "toggled",
 * which Windows keeps for every key). */
static void Key_Note(OpenBGI_t* o, int vk, int down)
{
	uint8_t* k;
	if(vk <= 0 || vk > 255)
		return;
	k = &o->input.keyboard[vk];
	if(down && !(*k & 0x80))
		*k = (uint8_t)((*k ^ 1) | 0x80);
	else if(!down)
		*k &= 0x7f;
}

// what the host hands over, one event of the calls below
enum
{
	IN_KEY,       // a: vk, b: down
	IN_POINTER,   // x, y; a: the place only, not a move of the player's (OpenBGI_PointerPlace)
	IN_BUTTON,    // a: button, b: down, x, y
	IN_WHEEL,     // a: delta
	IN_FOCUS,     // a: active
	IN_TEXT,      // text
	IN_CLOSE,     // WM_CLOSE
	IN_MINIMIZED, // a: minimised
	IN_DISPLAY,   // WM_DISPLAYCHANGE
	IN_DROP,      // text: the path
	IN_MESSAGE,   // msg, wParam, lParam
	IN_CD,        // a: MM_MCINOTIFY's status
	IN_MOVIE      // a: completed
};

typedef struct HostInput
{
	int kind, a, b, x, y, flags;
	const char* text;
	uint32_t msg, wParam, lParam;
} HostInput_t;

/* The settings key (F9, or what "settingsKey" of the input settings
 * names): its press puts the display settings up, or down again - unless
 * another dialog is up, or the host does not fit the window to its screen
 * (the key is the game's then, as every other).  1 when the key was taken:
 * its repeats and its release are taken with it. */
static int Wnd_SettingsKey(OpenBGI_t* o, int vk, int down, int flags)
{
	if(vk != o->input.settingsKey || !MainWin_ViewOn(o))
		return 0;
	if(!down)
	{
		int was = o->input.settingsHeld;
		o->input.settingsHeld = 0;
		return was;
	}
	if(o->input.settingsHeld)
		return 1; // (a repeat)
	if((flags & OPENBGI_KEY_TAKEN) || (Inframe_Up(o) && !Inframe_SettingsUp(o)))
		return 0;
	o->input.settingsHeld = 1;
	Inframe_Settings(o);
	return 1;
}

/* One event as the window procedure of the original handled the message:
 * a system key (WM_SYSKEYDOWN) to Wnd_SysKey, its release only for F10;
 * a double click (WM_?BUTTONDBLCLK, left and right) raised before the
 * press it also is.  While a dialog is up in the frame the keys, the
 * pointer, its buttons and the text are the dialog's, as the original's
 * window got no input while a box was up.  The child windows the engine
 * draws come next: the presses on them and the moves over them are
 * theirs, as they were the pop-ups', and Ctrl+C is the one a child window
 * was clicked last (its other keys went on to the main window).  While
 * the engine's text entry has the focus the keys and the text are the
 * entry's, and so are the presses on it and the moves over it, as they
 * were the edit control's.  The state of the keys and buttons
 * (GetAsyncKeyState) and the pointer's position still follow, as they do
 * for a key the host's entry took.  The on-screen keyboard lies over all
 * of these: the presses on it are its own.  A call that is not the input
 * mapper's own (steps.c) says which device the player uses
 * (Input_DeviceUsed). */
static void Input_Handle(OpenBGI_t* o, const HostInput_t* e)
{
	switch(e->kind)
	{
		case IN_KEY:
			if(e->a <= 0 || e->a > 255)
				break; // not a virtual key (the original's window gets none such)
			if(e->b && !o->input.steps.applying)
				Input_DeviceUsed(o, INPUT_DEV_KEYBOARD);
			if(Wnd_SettingsKey(o, e->a, e->b, e->flags))
				break; // the display settings' own key: the game never sees it
			Key_Note(o, e->a, e->b);
			if(e->flags & OPENBGI_KEY_TAKEN)
				break; // the host's entry took it
			if(Inframe_Up(o))
				Inframe_Key(o, e->a, e->b, e->flags);
			else if(Child_Key(o, e->a, e->b, e->flags) || Edit_Key(o, e->a, e->b, e->flags))
				break;
			else if(e->flags & OPENBGI_KEY_SYSTEM)
			{
				if(e->b)
					Wnd_SysKey(o, e->a);
				else if(e->a == 0x79)
					Wnd_KeyUp(o, e->a); // WM_SYSKEYUP: the original's window procedure only looks at F10
			}
			else if(e->b)
				Wnd_KeyDown(o, e->a, (e->flags & OPENBGI_KEY_REPEAT) != 0);
			else
				Wnd_KeyUp(o, e->a);
			break;
		case IN_POINTER:
			if(!o->input.steps.applying && (e->x != o->input.pointerX || e->y != o->input.pointerY))
			{
				if(Pad_NearPointer(o, e->x, e->y))
					break; // the host's pointer where a pad put it: not a move of the mouse
				if(!e->a)
					Input_DeviceUsed(o, INPUT_DEV_MOUSE);
			}
			if(Inframe_Up(o))
			{
				o->input.pointerX = e->x;
				o->input.pointerY = e->y;
				Inframe_Pointer(o, e->x, e->y);
			}
			else if(e->x != o->input.pointerX || e->y != o->input.pointerY)
			{
				o->input.pointerX = e->x;
				o->input.pointerY = e->y;
				// (over the on-screen keyboard, a child window or the entry the moves are theirs)
				if(!Osk_Over(o, e->x, e->y) && !Child_Pointer(o, e->x, e->y) && !Edit_Over(o, e->x, e->y))
					Wnd_MouseMove(o, e->x, e->y);
			}
			break;
		case IN_BUTTON:
			if((unsigned)e->a > 4u)
				break;
			if(!o->input.steps.applying)
				Input_DeviceUsed(o, INPUT_DEV_MOUSE);
			o->input.pointerX = e->x;
			o->input.pointerY = e->y;
			Key_Note(o, kButtonVk[e->a], e->b);
			if(Osk_Button(o, e->a, e->b, e->x, e->y))
				break; // the on-screen keyboard's, which lies over everything
			if(Inframe_Up(o))
			{
				Inframe_Button(o, e->a, e->b, e->x, e->y);
				break;
			}
			if(Child_Button(o, e->a, e->b, e->x, e->y) || Edit_Button(o, e->a, e->b, e->x, e->y))
				break;
			if(e->b && (e->flags & OPENBGI_CLICK_DOUBLE) && e->a <= OPENBGI_BUTTON_RIGHT)
				Wnd_DoubleClick(o, e->a);
			Wnd_MouseButton(o, e->a, e->b, e->x, e->y);
			break;
		case IN_WHEEL:
			if(!o->input.steps.applying)
				Input_DeviceUsed(o, INPUT_DEV_MOUSE);
			if(Inframe_Up(o))
				Inframe_Wheel(o, e->a);
			else
				Wnd_MouseWheel(o, e->a);
			break;
		case IN_TEXT:
			if(!o->input.steps.applying)
				Input_DeviceUsed(o, INPUT_DEV_KEYBOARD);
			if(Inframe_Up(o))
				Inframe_Text(o, e->text);
			else
				Edit_Text(o, e->text);
			break;
		case IN_FOCUS:
			if(!e->a)
			{ // nothing is held across a focus loss (the releases go to another window)
				int i;
				for(i = 0; i < 256; i++)
					o->input.keyboard[i] &= 0x7f;
			}
			o->input.hostFocus = e->a;
			if(!o->input.childFocus)
				Wnd_Activate(o, e->a); // (with a child window active the focus comes and goes with it)
			break;
		case IN_CLOSE: Wnd_CloseRequest(o); break;
		case IN_MINIMIZED: Wnd_Sized(o, e->a); break;
		case IN_DISPLAY: Wnd_DisplayChange(o); break;
		case IN_DROP: Wnd_DropFile(o, e->text); break;
		case IN_MESSAGE: Wnd_RawMessage(o, e->msg, e->wParam, e->lParam); break;
		case IN_CD: Wnd_MciNotify(o, e->a); break;
		case IN_MOVIE: Movie_OnEvent(o, e->a); break;
	}
}

/* An event in a try frame of its own, as the message pump has one: a quit
 * or a script error it throws stops the machine. */
static void Deliver(OpenBGI_t* o, const HostInput_t* e)
{
	BgiTryFrame_t f;
	if(BGI_TRY(o, f))
	{
		Input_Handle(o, e);
		BGI_END_TRY(o, f);
	}
	else
	{
		o->vm.sched.running = 0;
	}
}

static void Input_Deliver(OpenBGI_t* o, int kind, int a, int b, int x, int y, int flags, const char* text)
{
	HostInput_t e;
	memset(&e, 0, sizeof e);
	e.kind = kind;
	e.a = a;
	e.b = b;
	e.x = x;
	e.y = y;
	e.flags = flags;
	e.text = text;
	Deliver(o, &e);
}

void OpenBGI_Key(OpenBGI_t* o, int vk, int down, int flags)
{
	Input_Deliver(o, IN_KEY, vk, down != 0, 0, 0, flags, NULL);
}

void OpenBGI_Pointer(OpenBGI_t* o, int x, int y)
{
	Input_Deliver(o, IN_POINTER, 0, 0, x, y, 0, NULL);
}

void OpenBGI_PointerPlace(OpenBGI_t* o, int x, int y)
{
	Input_Deliver(o, IN_POINTER, 1, 0, x, y, 0, NULL);
}

void OpenBGI_Button(OpenBGI_t* o, int button, int down, int x, int y, int flags)
{
	Input_Deliver(o, IN_BUTTON, button, down != 0, x, y, flags, NULL);
}

void OpenBGI_Wheel(OpenBGI_t* o, int delta)
{
	Input_Deliver(o, IN_WHEEL, delta, 0, 0, 0, 0, NULL);
}

void OpenBGI_Focus(OpenBGI_t* o, int active)
{
	Input_Deliver(o, IN_FOCUS, active != 0, 0, 0, 0, 0, NULL);
}

void OpenBGI_Text(OpenBGI_t* o, const char* utf8)
{
	if(utf8 && *utf8)
		Input_Deliver(o, IN_TEXT, 0, 0, 0, 0, 0, utf8);
}

// ---- the window's events (openbgi.h) ---------------------------------------------------------

void OpenBGI_Close(OpenBGI_t* o)
{
	Input_Deliver(o, IN_CLOSE, 0, 0, 0, 0, 0, NULL);
}

void OpenBGI_Minimized(OpenBGI_t* o, int minimized)
{
	Input_Deliver(o, IN_MINIMIZED, minimized != 0, 0, 0, 0, 0, NULL);
}

void OpenBGI_WindowSize(OpenBGI_t* o, int w, int h)
{
	if(w <= 0 || h <= 0 || !o->display.windowExists || o->display.fullscreen)
		return;
	if(!MainWin_UserSized(o, &w, &h) || !o->ops.resizableWindow)
		return;                 // (the view's alone: the picture is shown at that size, the scripts keep theirs)
	o->display.windowSizeW = w; // the window's size from now on, as "81 64" sets one
	o->display.windowSizeH = h;
	o->window.w = w;
	o->window.h = h;
}

void OpenBGI_DisplayChanged(OpenBGI_t* o)
{
	MainWin_ViewRefresh(o);
	Input_Deliver(o, IN_DISPLAY, 0, 0, 0, 0, 0, NULL);
}

void OpenBGI_ScreenChanged(OpenBGI_t* o)
{
	MainWin_ViewRefresh(o);
}

void OpenBGI_DropFile(OpenBGI_t* o, const char* path)
{
	Input_Deliver(o, IN_DROP, 0, 0, 0, 0, 0, path ? path : "");
}

void OpenBGI_WindowMessage(OpenBGI_t* o, uint32_t msg, uint32_t wParam, uint32_t lParam)
{
	HostInput_t e;
	memset(&e, 0, sizeof e);
	e.kind = IN_MESSAGE;
	e.msg = msg;
	e.wParam = wParam;
	e.lParam = lParam;
	Deliver(o, &e);
}

void OpenBGI_CdNotify(OpenBGI_t* o, int status)
{
	Input_Deliver(o, IN_CD, status, 0, 0, 0, 0, NULL);
}

void OpenBGI_MovieEvent(OpenBGI_t* o, int completed)
{
	Input_Deliver(o, IN_MOVIE, completed != 0, 0, 0, 0, 0, NULL);
}

void OpenBGI_Quit(OpenBGI_t* o)
{
	o->window.quitPosted = 1;
}

// the debugger's window: queued for the debugger's next pass
void OpenBGI_DebugKey(OpenBGI_t* o, int vk, int down, int ch)
{
	Dbg_OnKey(o, vk, down, ch);
}

void OpenBGI_DebugMouse(OpenBGI_t* o, int x, int y, int button, int down)
{
	Dbg_OnMouse(o, x, y, button, down);
}

void OpenBGI_DebugWheel(OpenBGI_t* o, int delta)
{
	Dbg_OnWheel(o, delta);
}

void OpenBGI_DebugClose(OpenBGI_t* o)
{
	Dbg_OnClose(o);
}

// the key state as GetKeyboardState has it ("81 11"): bit 7 down, bit 0 toggled
void Input_KeyboardState(OpenBGI_t* o, uint8_t out[256])
{
	memcpy(out, o->input.keyboard, 256);
}

// whether the host reported key or button vk down (GetAsyncKeyState's bit 15)
int Input_KeyDown(OpenBGI_t* o, int vk)
{
	return vk > 0 && vk < 256 && (o->input.keyboard[vk] & 0x80) != 0;
}

/* The pointer as the frame reports it (OpenBGI_Frame_t): where the engine
 * moved it since the last frame, if it did. */
void Pointer_TakeFrame(OpenBGI_t* o, OpenBGI_Frame_t* f)
{
	f->pointerMoved = o->input.pointerMoved;
	f->pointerX = o->input.pointerX;
	f->pointerY = o->input.pointerY;
	o->input.pointerMoved = 0;
}

// ---- creation ------------------------------------------------------------------------------

/* Create the main window: 800 x 600, at the position the database
 * remembers when that is still on the screen, else centred (always
 * centred in launcher mode).  0 when the host could not create it
 * (an error box was shown).  Engine_Init switches to the real display
 * mode afterwards. */
int MainWindow_Create(OpenBGI_t* o)
{
	char title[0x200];
	int x, y;

	Display_MeasureScreen(o);
	if(!o->engine.launcherMode)
		sprintf(title, OPENBGI_NAME " v%s", OPENBGI_VERSION);
	else
		strcpy(title, OPENBGI_NAME " in Launcher mode");

	if(o->engine.launcherMode || !SavedWindowPos_Load(o, &x, &y))
		Display_CentredWindowPos(o, 800, 600, &x, &y);
	if(!MainWin_Create(o, title, x, y, 800, 600))
	{
		ShowErrorBox(o, MSG_WINDOW_CREATE_FAILED);
		return 0;
	}
	o->display.windowExists = 1;
	o->display.windowAlive = 1; // (WM_CREATE)
	return 1;
}

/* DestroyWindow: the window goes, and its WM_DESTROY posts the quit that
 * ends the run at the next pump. */
void MainWindow_Destroy(OpenBGI_t* o)
{
	if(!o->display.windowExists)
		return;
	MainWin_Destroy(o);
	o->display.windowAlive = 0;
	o->display.windowExists = 0;
	o->window.quitPosted = 1;
}

/* PostMessage(WM_CLOSE) ("80 69", Alt+F4): the next pump handles it as
 * the close box (Wnd_CloseRequest); nothing without a window. */
void MainWindow_PostClose(OpenBGI_t* o)
{
	if(o->display.windowExists)
		o->window.closePosted = 1;
}

// "80 64": show or hide the window (nothing without one).
void ShowMainWindow(OpenBGI_t* o, int show)
{
	if(!o->display.windowAlive)
		return;
	MainWin_Show(o, show);
}

// Minimise the window ("80 65").
void Window_Minimize(OpenBGI_t* o)
{
	MainWin_Minimize(o);
}

/* One of the messages the engine has for itself; 1 while there was one.
 * The close it posted comes first, then the focus changes of the window
 * nobody sees, then the quit, which throws 0 out to the caller's try
 * frame; the sound library's 20 ms fade timer is polled when nothing is
 * left, as a window timer would be. */
static int PumpOwn(OpenBGI_t* o)
{
	HostInput_t e;
	int active;
	if(o->window.closePosted)
	{
		o->window.closePosted = 0;
		Wnd_CloseRequest(o);
		return 1;
	}
	if(MainWin_TakeFocus(o, &active))
	{
		memset(&e, 0, sizeof e);
		e.kind = IN_FOCUS;
		e.a = active;
		Input_Handle(o, &e);
		return 1;
	}
	if(o->window.quitPosted)
	{
		o->window.quitPosted = 0;
		Wnd_Quit(o);
	}
	return Wm_TimerPoll(o);
}

/* The end of the host's message pump between two frames (openbgi.h): the
 * original's pump at the end of a pass, in its own try block - WM_QUIT
 * throws 0, which stops the machine. */
void OpenBGI_PumpEvents(OpenBGI_t* o)
{
	BgiTryFrame_t f;
	if(BGI_TRY(o, f))
	{
		while(PumpOwn(o))
			;
		BGI_END_TRY(o, f);
	}
	else
	{
		o->vm.sched.running = 0; // catch (int): WM_QUIT
	}
}

// "80 0F": the window has the focus
int Window_IsActive(OpenBGI_t* o)
{
	return o->display.windowActive;
}

/* A child window the engine draws (src/sys/child.c) was clicked and is
 * the active window now (1), or none is any more (0): the main window
 * goes inactive and active again, as the original's did when the player
 * clicked one of its pop-ups and then the game's window - while the
 * host's window has the focus. */
void Window_ChildFocus(OpenBGI_t* o, int child)
{
	child = child != 0;
	if(child == o->input.childFocus)
		return;
	o->input.childFocus = child;
	if(o->input.hostFocus)
		Wnd_Activate(o, !child);
}

int Window_IsRestored(OpenBGI_t* o)
{
	return o->display.windowRestored;
}

// "80 0E": "80 65" minimised the window
int Window_MinimizedByScript(OpenBGI_t* o)
{
	return o->display.minimizedByScript;
}

void Window_SetMinimizedByScript(OpenBGI_t* o, int v)
{
	o->display.minimizedByScript = v;
}

// "80 68": 1 closing destroys the window, 0 posts event 2 and lets the script decide
void Set_CloseMode(OpenBGI_t* o, int v)
{
	o->display.closeMode = v;
}
