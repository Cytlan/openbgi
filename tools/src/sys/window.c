/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.c - the main window: its creation, its state as the engine sees
 *            it and the events the OS layer reports for it; interface in
 *            bgi/display.h
 *
 * The OS layer owns the native window; this file keeps the engine's view
 * of it (active, minimised, alive) and turns the window's events into
 * what the engine does with them: key and button presses go to the input
 * state (input.c) and the script event queue (sysobj/events.h), the raw
 * message numbers to the message waits ("80 54", winmsg.c), the display
 * changes to display.c.  The display mode lives in display.c, the
 * presenting in present.c.
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
#include "bgi/os.h"
#include "bgi/dbg.h"

int gWindowActive;       // the window has the focus
int gWindowRestored = 1; // the window is not minimised
int gMinimizedByScript;  // "80 65" minimised the window; the next activation undoes it
int gCloseMode = 1;      // "80 68": 1 the close box destroys the window, 0 posts event 2
int gWindowAlive;        // between the window's creation and its destruction
int gWindowExists;       // the same, as bgi/engine.h exports it

static int gKnobDragL, gKnobDragR; // a button press went to a knob: its release does not reach the input state

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
static void Wnd_KeyDown(int vk, int repeat)
{
	if(vk == 0x7b && !repeat)
	{ // F12: the debugger (not an input the scripts see)
		Dbg_Toggle();
		return;
	}
	if(Dbg_OverlayKey(vk, 1))
		return; // the debugger draws over the game and takes its keys
	WinMsgWait_Dispatch(WM_KEYDOWN_, (uint32_t)vk, 0);
	if(Input_OnKeyDown(vk, repeat))
	{
		InputSerial_Bump();
		MsgQueue_Post(3, (uint32_t)vk, 0);
	}
	if(Display_StyleKeyMatch((uint32_t)vk))
		Display_ToggleStyle();
}

// A key release: the message waits and the input state (F12 and the debugger's keys excepted).
static void Wnd_KeyUp(int vk)
{
	if(vk == 0x7b || Dbg_OverlayKey(vk, 0))
		return;
	WinMsgWait_Dispatch(WM_KEYUP_, (uint32_t)vk, 0);
	Input_OnKeyUp(vk);
}

/* Alt+key combinations: F10 counts as a key, Alt+Enter toggles the style
 * (not while minimised), Alt+F4 closes; anything else only bumps the
 * input serial. */
static void Wnd_SysKey(int vk)
{
	if(vk == 0x79)
	{
		if(Input_OnKeyDown(vk, 0))
		{
			InputSerial_Bump();
			MsgQueue_Post(3, (uint32_t)vk, 0);
		}
		return;
	}
	InputSerial_Bump();
	if(vk == 0x0d)
	{
		if(gWindowRestored)
			Display_ToggleStyle();
	}
	else if(vk == 0x73)
	{
		OS_WindowClose();
	}
}

/* A mouse button (0 left, 1 right, 2 middle, 3 / 4 the X buttons) went
 * down or up at client position (x, y).  Presses post event 3 with the
 * virtual key (1, 2, 4, 5, 6); a left press on a knob starts a drag, a
 * right press flags the knob, and either is counted but not seen as a
 * key.  With the buttons swapped ("80 1E") the right button is handled
 * as the left one; every right press posts event 0x81 regardless. */
static void Wnd_MouseButton(int button, int down, int x, int y)
{
	uint32_t lp = ((uint32_t)y << 16) | (x & 0xffff);
	if(Dbg_OverlayMouse(x, y, button, down))
		return;
	switch(button)
	{
		case 0: // left
			if(down)
			{
				WinMsgWait_Dispatch(WM_LBUTTONDOWN_, 0, lp);
				InputSerial_Bump();
				MsgQueue_Post(3, 1, 0);
				if(Knob_UnderMouse())
				{
					int pos[2];
					GetMouseClientPos(pos);
					Knob_BeginDrag(Knob_UnderMouse(), pos[0], pos[1]);
					Input_CountPress(1);
					gKnobDragL = 1;
				}
				else
				{
					Input_OnKeyDown(1, 0);
					gKnobDragL = 0;
				}
			}
			else
			{
				WinMsgWait_Dispatch(WM_LBUTTONUP_, 0, lp);
				if(gKnobDragL)
					gKnobDragL = 0;
				else
					Input_OnKeyUp(1);
			}
			break;
		case 1: // right
			if(down)
			{
				WinMsgWait_Dispatch(WM_RBUTTONDOWN_, 0, lp);
				MsgQueue_Post(0x81, 0, 0);
				if(Input_SwapButtons() == 1)
				{ // right acts as left
					Wnd_MouseButton(0, 1, x, y);
					break;
				}
				if(Input_SwapButtons() != 0)
					break;
				InputSerial_Bump();
				MsgQueue_Post(3, 2, 0);
				if(Knob_UnderMouse())
				{
					Knob_MarkChanged(Knob_UnderMouse());
					Input_CountPress(2);
					gKnobDragR = 1;
				}
				else
				{
					Input_OnKeyDown(2, 0);
					gKnobDragR = 0;
				}
			}
			else
			{
				WinMsgWait_Dispatch(WM_RBUTTONUP_, 0, lp);
				if(Input_SwapButtons() == 1)
				{
					Wnd_MouseButton(0, 0, x, y);
					break;
				}
				if(Input_SwapButtons() != 0)
					break;
				if(gKnobDragR)
					gKnobDragR = 0;
				else
					Input_OnKeyUp(2);
			}
			break;
		case 2: // middle
			if(down)
			{
				WinMsgWait_Dispatch(WM_MBUTTONDOWN_, 0, lp);
				InputSerial_Bump();
				MsgQueue_Post(3, 4, 0);
				Input_OnKeyDown(4, 0);
			}
			else
			{
				WinMsgWait_Dispatch(WM_MBUTTONUP_, 0, lp);
				Input_OnKeyUp(4);
			}
			break;
		case 3:
		case 4:
		{ // X buttons -> vk 5 / 6; the wait's wParam carries the button number in its high word
			int vk = button == 3 ? 5 : 6;
			if(down)
			{
				WinMsgWait_Dispatch(WM_XBUTTONDOWN_, (uint32_t)(button - 2) << 16, lp);
				InputSerial_Bump();
				MsgQueue_Post(3, (uint32_t)vk, 0);
				Input_OnKeyDown(vk, 0);
			}
			else
			{
				WinMsgWait_Dispatch(WM_XBUTTONUP_, (uint32_t)(button - 2) << 16, lp);
				Input_OnKeyUp(vk);
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
int Window_SimulateClick(int kind)
{
	static const int buttons[7] = {-1, 0, 1, -1, 2, 3, 4};
	int pos[2];
	if((unsigned)kind >= 7 || buttons[kind] < 0)
		return 0;
	GetMouseClientPos(pos);
	Wnd_MouseButton(buttons[kind], 1, pos[0], pos[1]);
	Wnd_MouseButton(buttons[kind], 0, pos[0], pos[1]);
	return 1;
}

// A double click: event 0x80 for the left button, 0x81 for any other.
static void Wnd_DoubleClick(int button)
{
	MsgQueue_Post(button == 0 ? 0x80u : 0x81u, 0, 0);
}

/* The wheel: a press and release of virtual key 0x0e (up) or 0x0f (down)
 * and event 3 with it, or a nudge of the captured knob while the "90 DD"
 * switch is on. */
static void Wnd_MouseWheel(int delta)
{
	int vk;
	if(delta == 0 || Dbg_OverlayWheel(delta))
		return;
	WinMsgWait_Dispatch(WM_MOUSEWHEEL_, (uint32_t)(delta & 0xffff) << 16, 0);
	InputSerial_Bump();
	vk = delta < 0 ? 0x0f : 0x0e;
	MsgQueue_Post(3, (uint32_t)vk, 0);
	if(Knob_GetGlobal() && Knob_TopCaptured())
	{
		Knob_NudgeCaptured(delta < 0);
		return;
	}
	Input_OnKeyDown(vk, 0);
	Input_OnKeyUp(vk);
}

/* The window gained or lost the focus.  Coming back after a scripted
 * minimise ("80 65") resumes the sound and restores the display mode. */
static void Wnd_Activate(int active)
{
	gWindowActive = active != 0;
	if(active && gMinimizedByScript && !OS_WindowMinimized())
	{
		Sound_ResumeAll();
		SetDisplayMode(gSizeIndex, gPixelMode, gFullscreen);
		gMinimizedByScript = 0;
	}
}

// The window was minimised or restored.
static void Wnd_Sized(int minimized)
{
	gWindowRestored = !minimized;
}

/* The close box (or Alt+F4): in close mode 0 the script decides (event
 * 2), otherwise the text entry control and the window are destroyed. */
static void Wnd_CloseRequest(void)
{
	WinMsgWait_Dispatch(WM_CLOSE_, 0, 0);
	InputSerial_Bump();
	if(gCloseMode == 0)
	{
		MsgQueue_Post(2, 0, 0); // the script decides
		return;
	}
	OS_EditDestroy();
	OS_WindowDestroy();
}

// The native window exists.
static void Wnd_Created(void)
{
	gWindowAlive = 1;
}

// The native window is gone: end the message loop (Wnd_Quit follows).
static void Wnd_Destroyed(void)
{
	gWindowAlive = 0;
	gWindowExists = 0;
	OS_PostQuit();
}

/* The window was uncovered: copy the back buffer to it, or black while
 * the engine is not up yet or a movie plays (the movie draws itself).
 * Nothing while presenting is disabled or suspended. */
static void Wnd_Paint(void)
{
	if(!gDisplayEnabled || !Present_IsAllowed())
		return;
	if(gEngineUp && !gMoviePlaying)
		Window_Repaint();
	else
		OS_WindowFillBlack();
}

// A file was dropped on the window: event 0x10, the path for "80 6D".
static void Wnd_DropFile(const char* path)
{
	MsgQueue_Post(0x10, 0, 0);
	DragDrop_OnFile(path);
}

// A second instance sent its boot path (message 0x9000): handled as a dropped file.
static void Wnd_IpcString(const char* s)
{
	MsgQueue_Post(0x10, 0, 0);
	DragDrop_OnFile(s);
}

// Any other message: only the message waits see it.
static void Wnd_RawMessage(uint32_t msg, uint32_t wParam, uint32_t lParam)
{
	WinMsgWait_Dispatch(msg, wParam, lParam);
}

// The message loop ended: throw 0 out of PumpMessages to the main loop.
static void Wnd_Quit(void)
{
	BGI_ThrowInt(0); // WM_QUIT: "throw 0" in PumpMessages
}

static void Wnd_DisplayChange(void)
{
	Display_OnDisplayChange();
}

// MM_MCINOTIFY (0x3b9): a CD track that was started with the notification ended: replay it
static void Wnd_MciNotify(int status)
{
	if(status == 1)
		Cd_Replay();
}

// the pointer's position, for the debugger when it draws over the game (the engine itself polls the cursor)
static void Wnd_MouseMove(int x, int y)
{
	Dbg_OverlayMouse(x, y, -1, 0);
}

// the handlers in the order of OsEventHandlers_t (bgi/os.h)
static const OsEventHandlers_t gHandlers = {
	Wnd_KeyDown, Wnd_KeyUp, Wnd_MouseMove, Wnd_MouseButton, Wnd_MouseWheel, Wnd_Activate,
	Wnd_CloseRequest, Wnd_Destroyed, Wnd_Paint, Wnd_DropFile, Wnd_IpcString,
	Child_OnPaint, Wnd_RawMessage, Wnd_SysKey, Wnd_Quit, Wnd_Created, Wnd_Sized,
	Wnd_DoubleClick, Wnd_DisplayChange, Wnd_MciNotify, Child_OnDestroyed, Present_EditPaint, Movie_OnEvent,
	DBG_EVENT_HANDLERS};

// ---- creation ------------------------------------------------------------------------------

/* Create the main window: 800 x 600, at the position the database
 * remembers when that is still on the screen, else centred (always
 * centred in launcher mode).  0 when the OS layer could not create it
 * (an error box was shown).  Engine_Init switches to the real display
 * mode afterwards. */
int MainWindow_Create(void)
{
	OsWindowDesc_t d;
	char title[0x200];
	int x, y;

	Display_MeasureScreen();
	if(!gLauncherMode)
		sprintf(title, "Ethornell - Buriko General Interpreter ver 1.69 ( build : 444.2 ) with Wave Master ver %s", Sound_LibVersion());
	else
		strcpy(title, "Buriko General Interpreter in Launcher mode");

	if(gLauncherMode || !SavedWindowPos_Load(&x, &y))
		Display_CentredWindowPos(800, 600, &x, &y);
	d.title = title;
	d.x = x;
	d.y = y;
	d.width = 800;
	d.height = 600;
	OS_SetEventHandlers(&gHandlers);
	if(!OS_WindowCreate(&d))
	{
		ShowErrorBox(MSG_WINDOW_CREATE_FAILED);
		return 0;
	}
	gWindowExists = 1;
	gWindowAlive = 1;
	// the hidden 1 x 1 text entry ("B0 2x") is created with the window by the OS layer
	return 1;
}

// "80 64": show or hide the window (nothing without one).
void ShowMainWindow(int show)
{
	if(!gWindowAlive)
		return;
	OS_WindowShow(show);
}

// Minimise the window ("80 65").
void Window_Minimize(void)
{
	OS_WindowMinimize();
}

/* Process one pending message of the window; 1 while there was one.  A
 * quit throws 0 out to the main loop.  The sound library's 20 ms fade
 * timer is polled when the queue is empty, as a window timer would be. */
int PumpMessages(void)
{
	if(OS_PumpMessages())
		return 1;
	return Wm_TimerPoll();
}
