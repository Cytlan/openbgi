/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * waitobj.h - the concrete wait classes
 *
 * Each "80 xx" / "90 xx" instruction that blocks a thread allocates one of
 * these, hands it to Thread_SetWait() and returns scheduler code 2.  The
 * scheduler polls the object once per pass; when poll() reports completion
 * the object pushes its result(s) onto the thread's stack and is destroyed.
 * The common base is in wait.h; only the constructors are public.
 *
 * Two kinds of entry point: the WaitXxx_New constructors return the object
 * for the handler to install, the Start* functions (src/wait/wait_tween.c,
 * wait_text.c, wait_menu.c, wait_icon.c) validate their arguments, install
 * the wait themselves and return 0 or an error code the handler maps to a
 * script error.  The text wait classes themselves are declared in
 * gfx/text.h.
 */
#ifndef BGI_WAITOBJ_H_
#define BGI_WAITOBJ_H_

#include "bgi/wait.h"

struct Thread;
struct SemMgr;

/* ---- the step waits (src/wait/wait_step.c): an instruction that waited
 * inside itself in the original - for a message box, a dialog, a disc, a
 * program - gives its thread one of these.  The step function is called
 * once a frame (first by WaitStep_Begin) and returns 0 while waiting, 1
 * when done (it has pushed the instruction's results), -1 to stop the
 * machine.  Until it is done the thread holds the engine: no other thread
 * runs and nothing is presented, as in the original.  A subclass is a
 * struct whose first member is WaitStep_t, allocated by WaitStep_New with
 * its size; `release` frees what it holds. */
typedef struct WaitStep WaitStep_t;
typedef int (*WaitStepFn_t)(OpenBGI_t* o, WaitStep_t* w);
typedef void (*WaitStepFreeFn_t)(OpenBGI_t* o, WaitStep_t* w);
struct WaitStep
{
	Wait_t w;
	const char* name;         // the class's name for the debugger ("WaitDialog", ..)
	WaitStepFn_t step;        // the work of one frame
	WaitStepFreeFn_t release; // NULL when there is nothing to free
	uint32_t idleMs;          // how long the host may wait between two steps (OpenBGI_Frame_t.idleMs; 0 the idle wait)
};
extern const WaitVtbl_t WaitStep_Vtbl;
// a zeroed step of `size` bytes for thread t (not yet its wait)
WaitStep_t* WaitStep_New(OpenBGI_t* o, struct Thread* t, size_t size, const char* name, WaitStepFn_t step, WaitStepFreeFn_t release);
/* Run the first step and return the scheduler code for the handler to
 * return: done at once, the step is freed and 0 returned (the instruction
 * goes on); failed, the machine stops and 1 is returned; else the step
 * becomes the thread's wait, the thread holds the engine and 2 is
 * returned. */
int WaitStep_Begin(OpenBGI_t* o, WaitStep_t* w);
void WaitStep_Discard(OpenBGI_t* o, WaitStep_t* w); // free a step that was not begun
uint32_t WaitStep_IdleMs(OpenBGI_t* o);             // the idleMs of the step holding the engine, 0 when none
const char* WaitStep_ClassName(const Wait_t* w);

/* The step of an instruction that asks the host one question
 * (bgi/request.h) and waits for the answer: once it arrives `done` turns it
 * into the instruction's results (pushes them, copies the entries) - NULL
 * pushes the answer as it is.  `arg` is the subclass's to use (WaitAsk_Is:
 * the answer that counts as yes).  A subclass begins with WaitAsk_t;
 * `release` frees what it holds besides. */
typedef struct WaitAsk WaitAsk_t;
typedef void (*WaitAskDoneFn_t)(OpenBGI_t* o, WaitAsk_t* w, int answer);
struct WaitAsk
{
	WaitStep_t s;
	uint32_t req; // the question; 0 once answered
	WaitAskDoneFn_t done;
	WaitStepFreeFn_t release;
	int32_t arg;
};
WaitAsk_t* WaitAsk_New(OpenBGI_t* o, struct Thread* t, size_t size, const char* name, WaitAskDoneFn_t done, WaitStepFreeFn_t release);
int WaitAsk_Begin(OpenBGI_t* o, WaitAsk_t* w, uint32_t req); // as WaitStep_Begin, for the request made
void WaitAsk_Is(OpenBGI_t* o, WaitAsk_t* w, int answer);     // a `done`: pushes 1 when the answer is `arg`, else 0
void WaitAsk_Chosen(OpenBGI_t* o, WaitAsk_t* w, int answer); // a `done` of the file dialogs: pushes 0 when one was chosen (answer 1), else -1
/* A message box whose button is the instruction's result: pushes 1 when
 * the button is `yes`; the script's own boxes ("78", "B0 81", "B0 82")
 * when `script`.  The scheduler code for the handler to return. */
int WaitAsk_Box(OpenBGI_t* o, struct Thread* t, const char* text, const char* caption, uint32_t flags, int yes, int script);

// the system waits (src/wait/wait_sys.c); what each pushes is at its poll
Wait_t* WaitWinMsg_New(OpenBGI_t* o, struct Thread* t, uint32_t msg);                                        // "80 54": wParam, lParam
Wait_t* WaitTimer_New(OpenBGI_t* o, struct Thread* t, uint32_t ms);                                          // "80 5A": nothing
Wait_t* WaitTimerKey_New(OpenBGI_t* o, struct Thread* t, uint32_t ms, int keyBreak, uint32_t prio);          // "80 5C": 1 input ended it / 0
Wait_t* WaitLock_New(OpenBGI_t* o, struct Thread* t, struct SemMgr* mgr, const char* name, uint32_t prio);   // "80 B4": 0 / -1 / 0x80000001
Wait_t* WaitEncode_New(OpenBGI_t* o, struct Thread* t, void* dst, const void* src, uint32_t size);           // "80 C0": the encoded size
Wait_t* WaitEncode2_New(OpenBGI_t* o, struct Thread* t, void* dst, const void* src, uint32_t w, uint32_t h); // "80 C4": the encoded size (w = frame size, h = frame count)
// the background loads (src/wait/wait_load.c); errors are raised as script errors
Wait_t* WaitBmpLoad_New(OpenBGI_t* o, struct Thread* t, int bmpNo, const char* arc, const char* name); // "90 10"
Wait_t* WaitSeLoad_New(OpenBGI_t* o, struct Thread* t, int slot, const char* arc, const char* name, int fadeInMs, double gain,
	double rate); // "A0 20" / "A0 21" / "A0 27"
Wait_t* WaitSeMem_New(OpenBGI_t* o, struct Thread* t, int slot, const void* data, int fadeInMs, double gain,
	double rate); // "A0 28" (1.547 on): a BW file in memory
// 1.69/472 on: "90 F4" (pushes 0 ok / 1 / 2 not a movie) and the deferred "90 F6" (pushes 0 / 3 / 5 / 8)
Wait_t* WaitSeqLoad_New(OpenBGI_t* o, struct Thread* t, const char* arc, const char* name, int32_t* outNo, int32_t* info);
Wait_t* WaitSeqDecode_New(OpenBGI_t* o, struct Thread* t, int bmpNo, int no, uint32_t frame);

/* "B0 08" (src/wait/wait_quake.c): the screen quake.  Allocates the wait
 * into *out before validating the parameters (as the original does; the
 * handler raises a script error for a rejected one) and returns 0 or the
 * setup's codes: 0x80000001 bad pattern, 0x80000002 frequency < 1,
 * 0x80000003 repeat < 1, 0x80000004 fps < 1 or below the frequency.  The
 * wait pushes nothing. */
int StartQuake(OpenBGI_t* o, struct Thread* t, int pattern, int amplitude, int frequency, int repeat, int decay, int fps, int skip,
	Wait_t** out);

/* ---- the cdecl starters of the object animation waits
 * (src/wait/wait_tween.c).  Each one resolves the handle, allocates the wait
 * object, starts it and hands it to the thread; 0 ok, -1 no such object,
 * 0x8000000n argument errors (the handler maps them to script errors).
 * duration is in ms, fps the update rate, maxSkip the frames the clock may
 * jump per update (0 = unlimited), curve an Ease() curve number, skip
 * whether a click / key ends the wait (with an input layer at prio).  When
 * the animation ends the wait pushes two values: the update rate it
 * achieved (updates * 1000 / duration) and 1 when it was interrupted. */
// "90 20" .. "90 23": level (and position when `move`) tween; 0x80000001 fps < 1
int StartObjTween(OpenBGI_t* o, struct Thread* t, uint32_t h, int32_t x, int32_t y, int curve, int move, int level, int duration,
	int fps, int maxSkip, int skip, uint32_t prio);
// "90 24": move through a via point; 0x80000001 fps < 1, 0x80000002 x not monotonic
int StartObjMove2(OpenBGI_t* o, struct Thread* t, uint32_t h, int32_t viaX, int32_t viaY, int32_t x, int32_t y, int curve, int level,
	int duration, int fps, int maxSkip, int skip, uint32_t prio);
// "90 28": position, level and progress with their curves; 0x80000001 fps < 1
int StartObjTweenFull(OpenBGI_t* o, struct Thread* t, uint32_t h, int32_t x, int32_t y, int curve, int level, int levelCurve,
	int progress, int duration, int fps, int maxSkip, int skip, uint32_t prio);
// "90 29": along `n` control points (x, y, z, pad in 16.16); 0x80000001 fps < 1, 0x80000003 n < 1
int StartObjPath(OpenBGI_t* o, struct Thread* t, uint32_t h, int n, const int32_t* points, int curve, int level, int levelCurve,
	int progress, int duration, int fps, int maxSkip, int skip, uint32_t prio);
/* "90 2C": per-object shake; 0x80000001 bad pattern, 0x80000002
 * frequency < 1, 0x80000003 repeat < 1, 0x80000004 fps < 1 or below the frequency */
int StartObjQuake(OpenBGI_t* o, struct Thread* t, uint32_t h, int pattern, int amplitude, int frequency, int repeat, int decay,
	int fps, int skip, uint32_t prio);

/* ---- text output and selections ------------------------------------------ */
/* start message output in window h (src/wait/wait_text.c).  mode -1 is the
 * plain WaitTextOut (colourOrParam is its parameter word), 0 the rich class,
 * 1 the rich class or its vertical variant by the window's draw style, with
 * the colours and style.  0 ok, -1 no such window, 0x80000001 no font.  The
 * wait pushes 1 when input made the text finish early ("fast"), else 0. */
int StartTextOut(OpenBGI_t* o, struct Thread* t, uint32_t h, const char* str, uint32_t colourOrParam, int rubyOn,
	uint32_t rubyColour, int hang, int plainStyle, const int32_t* style, int instant, int waitAtEnd, int allowSkip,
	int allowKeys, int mode);
// the 12-argument form ("90 90" mode -1, "91 90" / "91 92" mode 0): no style, the colour doubles as the ruby colour
int StartTextOutW(OpenBGI_t* o, struct Thread* t, uint32_t h, const char* str, uint32_t colourOrParam, int plainStyle, int instant,
	int waitAtEnd, int allowSkip, int allowKeys, int rubyOn, int hang, int mode);
/* "90 A0" (src/wait/wait_menu.c): text menu of n strings (tagged pointers
 * resolved through t) in `cols` columns, optionally centred, in `colour`
 * (unless the window has a colour table), cursor on `init`.  0 / -1 no
 * window / 0x80000001 bad item count / 0x80000002 bad initial item /
 * 0x80000003 bad column count / 0x80000004 no font.  The wait pushes the
 * selected index twice (-1 for cancel; the second copy is -1 when waits are
 * not blocking). */
int StartMenuSelect(OpenBGI_t* o, struct Thread* t, uint32_t h, int n, const void* items, int cols, int centre, uint32_t colour,
	int init, int allowCancel);
// "90 A1": lay the entries out without waiting; 0 / -1 no window / 0x80000001 bad item count / 0x80000003 bad column count
int Menu_Draw(OpenBGI_t* o, uint32_t h, int n, const void* items, struct Thread* t, int cols, int centre, uint32_t colour);
/* "90 A2" (mode 1) / "90 A3" (mode 0): menu with two decoration bitmaps
 * (-1 = none) at (dx, dy) from the selected entry's top-left (the second
 * also past its right edge); mode 0 only flashes `init` for a second.
 * Errors as above plus 0x80000005 .. 8 for the two bitmaps (unknown / not
 * usable in the window). */
int StartMenuSelectBmp(OpenBGI_t* o, struct Thread* t, uint32_t h, int n, const void* items, int cols, int centre, uint32_t colour,
	int init, int allowCancel, int bmp1, int dx1, int dy1, int bmp2, int dx2, int dy2, int mode);
/* "90 B0" / "90 B1" (src/wait/wait_icon.c): icon selection in a window;
 * hotkeys (one per icon) may be NULL, mode 0..3 picks the input layers.
 * 0 / -1 no window / 0x80000001 bad icon count / 0x80000002 bad mode /
 * 0x80000003 an icon bitmap is invalid.  The wait pushes the click position
 * inside the icon (x, y) and the icon index (-1 for none / cancel). */
int StartIconSelect(OpenBGI_t* o, struct Thread* t, uint32_t h, int n, const void* icons, const int32_t* hotkeys, int mode,
	int allowCancel, int useHitMask);
int StartIconSelect2(OpenBGI_t* o, struct Thread* t, uint32_t h, int n, const void* icons, const int32_t* hotkeys, int mode,
	int allowCancel, int useHitMask);
int Icon_Draw(OpenBGI_t* o, uint32_t h, int n, const void* icons); // "90 B4" / "90 B5": draw without waiting; 0 / -1 / 0x80000001
// the text menu's global settings
void Menu_SetBlinkInterval(OpenBGI_t* o, uint32_t ms);                          // "90 A5"; 600 by default
void Menu_SetCursorColours(OpenBGI_t* o, uint32_t c0, uint32_t c1);             // "90 A4": per blink phase, 0 = off
void Menu_SetWheelMove(OpenBGI_t* o, int on, int curve, int duration, int fps); // "90 A6": the wheel moves the pointer
int WaitTextOut_IsDirty(OpenBGI_t* o, Wait_t* w);                               // wait_text.c, also the menus' isDirty: nothing to present below the draw limit

/* The launcher's instructions that read from a drive ("EF 20" a folder's
 * listing, "EF 28" a look at a game) on a host that does such work beside
 * the frames (bgi/launcher.h, Launcher_JobFor): the thread waits for the
 * job and gets the instruction's result when it is back (vm/ops_obgi.c).
 * Launcher_JobGone tells the wait that its job went with the run. */
const char* WaitJob_ClassName(const WaitVtbl_t* vt);
void Launcher_JobGone(Wait_t* w);

#endif // BGI_WAITOBJ_H_
