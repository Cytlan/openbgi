/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sched.c - the main loop of the script VM: the scheduler that gives every
 *           thread its turn, the per-pass services, thread creation and
 *           the exclusive-thread switch (src/include/bgi/vm.h)
 *
 * The original's main loop runs until the thread list empties: every
 * pass gives each thread a turn (Vm_RunThread executes its instructions
 * until one yields), then the frame services run (particles, panels,
 * input, the display, the window's message pump).  Here the host owns the
 * loop (openbgi.h): a frame (Vm_Frame, for OpenBGI_RunFrame) runs one
 * pass and its services, and the host answers the engine's requests,
 * pumps the messages and waits before the next.  The scheduler codes the
 * handlers return are listed in docs/dev/vm.md ("Threads and the scheduler").
 *
 * What the original waited for inside an instruction - a box, a dialog,
 * a disc, another program - stops the pass instead: after an instruction
 * that left a request unanswered or gave its thread a WaitStep that holds
 * the engine (src/include/bgi/waitobj.h), the pass ends early and the next frame
 * goes on with it where it stopped, once nothing holds it; the services
 * follow the whole pass, as in the original.  A load that asks for the
 * disc (RetryPrompt) leaves its instruction instead, which is started
 * over once the disc is there (Vm_RetryInstruction).
 *
 * The original wraps the boot, the pass and the message pump in three C++
 * try blocks whose catches set the flags of o->vm.sched; they are
 * rendered with BGI_TRY frames (src/include/bgi/error.h; the pump's is
 * OpenBGI_PumpEvents'), and a script error (ScriptError, a longjmp to the
 * innermost frame) lands in the same places.  The development tracing
 * aids (BGI_TRACE and friends) are in trace.c.
 */
#include "bgi/vm.h"
#include "bgi/wait.h"
#include "bgi/error.h"
#include "bgi/file.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/engine.h"
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/gfx.h"
#include "bgi/panel.h"
#include "bgi/sysobj.h"
#include "bgi/sound.h"
#include "bgi/dbg.h"
#include "openbgi.h"
#include "bgi/request.h"
#include "bgi/waitobj.h"
#include "bgi/core.h"

/* make `t` the only thread the scheduler gives turns to ("80 5D"); NULL
 * lifts the restriction.  `on` is accepted for the caller's convenience
 * and ignored: the thread pointer alone decides. */
void SetExclusiveThread(OpenBGI_t* o, Thread_t* t, int on)
{
	BGI_UNUSED(on);
	o->vm.exclusiveThread = t;
	o->vm.exclusiveOn = t != NULL;
}

// ---- the blocks an instruction holds across a load (Vm_TempAlloc, bgi/vm.h) ----

void* Vm_TempAlloc(OpenBGI_t* o, size_t size)
{
	void* p = BGI_Alloc(size);
	if(o->vm.tempCount < VM_TEMP_MAX)
		o->vm.temp[o->vm.tempCount++] = p;
	return p;
}

void Vm_TempKeep(OpenBGI_t* o, void* p)
{
	int i;
	for(i = o->vm.tempCount - 1; i >= 0; i--)
		if(o->vm.temp[i] == p)
		{
			o->vm.temp[i] = o->vm.temp[--o->vm.tempCount];
			return;
		}
}

void Vm_TempFree(OpenBGI_t* o, void* p)
{
	Vm_TempKeep(o, p);
	BGI_Free(p);
}

// an error left the instruction: what it held goes
static void Vm_TempDrop(OpenBGI_t* o)
{
	while(o->vm.tempCount > 0)
		BGI_Free(o->vm.temp[--o->vm.tempCount]);
}

/*
 * Load program `name` of archive `arc` into a fresh thread appended to the
 * list `parent` is part of ("80 44"; the boot thread), with the stack,
 * code and data sizes of Thread_New.  Returns the new thread's id.  A
 * program that loads as 0 bytes, or one too large for the code area, is a
 * script error raised in parent's context (a missing file makes LoadFile
 * itself prompt for the disc and throw when the user gives up).
 */
uint32_t SpawnThread(OpenBGI_t* o, const char* arc, const char* name, uint32_t stackEntries,
	uint32_t codeSize, uint32_t dataSize, Thread_t* parent)
{
	uint8_t* image = (uint8_t*)Vm_TempAlloc(o, 0x20000);
	uint32_t id = 0;
	char msg[0x104];

	if(LoadFile(o, image, arc, name) == 0)
	{
		sprintf(msg, MSG_PROGRAM_NOT_FOUND, arc, name);
		Vm_TempFree(o, image);
		ScriptError(o, msg, parent);
	}
	else
	{
		Thread_t* t = Thread_SpawnChild(o, parent, stackEntries, codeSize, dataSize);
		if(Thread_LoadModule(o, t, image, name) == 0x80000000u)
		{
			sprintf(msg, MSG_THREAD_CODE_FULL, arc, name);
			Vm_TempFree(o, image);
			ScriptError(o, msg, parent);
		}
		id = Thread_GetId(t);
	}
	Vm_TempFree(o, image);
	return id;
}

/* the definition of "fresh state" for a script: every subsystem back to
 * its defaults, before each boot (the first one and every reboot of
 * scheduler code 5) */
void Vm_ResetSubsystems(OpenBGI_t* o)
{
	SemMgr_Clear(o->sysobj.semMgr);
	Set_CloseMode(o, 1);
	SetStyleChangeKeys(o, 0, 0);
	Display_Enable(o, 1);
	Present_SetRate(o, 250);
	BmpMgr_SetOption(o, 0);
	Sound_ResetVolumes(o);
	Sound_ClearTable(o);
	DynMem_FreeAll(o);
	UserOp_Clear(o);
	Knob_UnregisterAll(o);
	Knob_ReleaseAll(o);
	Knob_SetGlobal(o, 1);
	Target_Clear(o);
	Input_ResetLayers(o);
	WinMsgWait_Clear(o);
	MsgQueue_Clear(o);
	KeyState_Clear(o);
	Input_SetSwapButtons(o, 0);
	Panel_DeleteAll(o); // before the windows they are attached to go
	Gfx_ResetAll(o);
	Gfx_SetBmpOption(o, 0); // the bitmap option of "90 0F"
	Set_SaveEncode(o, 1);
	Set_InputEnable(o, 1);
	Set_SkipKeyEnable(o, 1);
	Set_SkipLatch(o, 0);
	History_Reset(o, 0);
	Ring_DeleteAll(o);
	StrTab_Clear(o, 0);
	Dict_DeleteAll(o);
	Ptcl_AutoClear(o);
	Set_FileSearch(o, 1);
	FileSearch_Clear(o);
	Select_RequireActive(o, 0);
	EnableDragDrop(o, 0);
	Wait_SetBlocking(o, 1);
	Wait_SetPresentMode(o, 1, 0);
}

// body of the first try block: create the boot thread (a stack of 0x1000 entries, the profile's code and data sizes)
static void Vm_BootThread(OpenBGI_t* o)
{
	char arc[0x104], name[0x104];
	GetBootProgram(o, arc, name);
	o->vm.sched.bootOk = SpawnThread(o, arc, name, 0x1000, o->engine.profile->bootCode, o->engine.profile->bootData, o->vm.rootThread) != 0;
}

/*
 * One turn of one thread: poll its wait object when it is blocked, then
 * execute instructions until a handler returns a code other than 0 or the
 * turn limit is reached.  Returns the scheduler code of the last
 * instruction (0 when the limit ended the turn), or 1 when the thread did
 * not run: another thread is exclusive, the wait is still pending, the
 * machine is stopping, or the debugger hit a breakpoint.
 */
static int Vm_RunThread(OpenBGI_t* o, Thread_t* t)
{
	uint32_t count = 0;
	int rc = 0;

	if(o->vm.exclusiveOn && t != o->vm.exclusiveThread)
		return 1;
	if(Thread_GetFlags(t) & THR_FLAG_WAITING)
	{
		int w;
		o->vm.inPoll = 1; // a disc prompt here cannot restart an instruction (RetryPrompt)
		w = Thread_PollWait(o, t);
		o->vm.inPoll = 0;
		if(w == 0)
			return 1; // still blocked
		if(w == -1)
		{
			o->vm.sched.running = 0; // the wait failed: stop everything
			return 1;
		}
		if(o->req.pending || o->vm.holder)
			return SCHED_CUT; // the wait ended with a box (and the turn goes on after it)
	}
	if(!o->vm.sched.running)
		return 1;

	while(rc == 0 && count < o->vm.dbgInsnLimit) // 0x100000, or 1 while the debugger steps by instruction
	{
		uint8_t op = Thread_FetchOp(t);
		if(o->vm.dbgHooked && Dbg_OnInsn(o, t, t->opIP, op))
		{ // a breakpoint: the instruction is not executed, the turn ends here
			Thread_SetIP(t, t->opIP);
			return 1;
		}
		if(o->trace.vmTraceOn)
			Vm_TraceBefore(o, t, op);
		if(!o->vm.optablemain[op])
		{
			char msg[0x100];
			sprintf(msg, MSG_UNDEFINED_OP, op);
			ScriptError(o, msg, t);
		}
		rc = o->vm.optablemain[op](o, t);
		if(o->trace.vmTraceOn)
			Vm_TraceAfter(o, t);
		count++;
		if(rc == 0 && (o->req.pending || o->vm.holder))
			return SCHED_CUT; // the instruction asked the host or holds the engine: the rest of the turn afterwards
	}
	return rc;
}

/* body of the second try block: one pass over the threads in list order,
 * from `t` on, acting on the scheduler code each turn ends with.  When a
 * turn leaves a request unanswered or a thread holding the engine, the
 * pass stops there: o->vm.resumeAt is the thread it goes on with (the
 * holder, the thread whose turn was cut, or the next one), and 0 is
 * returned.  1 when the pass is over. */
static int Vm_RunPass(OpenBGI_t* o, Thread_t* t)
{
	while(t)
	{
		Thread_t* next;
		int rc;
		o->vm.sched.cur = t;
		rc = Vm_RunThread(o, t);
		switch(rc)
		{
			case 3: // switch to another thread at once (an unknown id ends the pass)
				next = Thread_FindById(o->vm.rootThread, o->vm.switchThreadId);
				break;
			case 4: // the thread ends: start the pass over
				Thread_KillChild(o, o->vm.rootThread, t);
				next = Thread_Next(o->vm.rootThread);
				break;
			case 5: // reboot
				o->vm.sched.restart = 1;
				o->vm.sched.running = 0;
				next = Thread_Next(t);
				break;
			case 6: // quit
				o->vm.sched.running = 0;
				next = Thread_Next(t);
				break;
			default: // 0 (turn limit), 1, 2, SCHED_CUT, > 6
				next = Thread_Next(t);
				break;
		}
		if(o->vm.holder)
			o->vm.resumeAt = o->vm.holder;
		else if(o->req.pending)
			o->vm.resumeAt = rc == SCHED_CUT ? t : next;
		if(o->vm.resumeAt)
			return 0;
		t = next;
	}
	return 1;
}

/* the per-pass services, after the threads had their turns.  Returns
 * PresentFrame's result (-1 nothing presented, 0 a frame presented).  In
 * the original the services go on with the message pump (in its own try
 * block), the idle wait and Vm_EndFrame's part; the first two are the
 * host's between two frames (OpenBGI_PumpEvents, OpenBGI_Frame_t.idleMs). */
static int Vm_FrameServices(OpenBGI_t* o)
{
	int presented;

	Ptcl_UpdateAll(o);
	Ptcl_AutoTick(o);
	Rain_UpdateAll(o);
	Rain_FrameTick(o);
	if(Panel_GetPollMode(o) == 0)
		Panel_PollAll(o);
	else
		Panel_PumpAll(o); // "80 AF" mode 1 (1.573 on): the poll moves to the end of the pass
	Target_UpdateAll(o);
	Cursor_Update(o);
	CursorSprite_Update(o);
	Knob_Update(o);
	Input_Poll(o, 1, 1);
	presented = PresentFrame(o);
	Movie_Tick(o);         // the ffmpeg player's frame, when a movie plays that way
	Dbg_OverlayPresent(o); // the debugger over the picture, when it has no window of its own
	return presented;
}

/* the end of a pass, after the host pumped the window's events and
 * waited: the late panel poll, the debugger's turn, and the threads'
 * teardown once the machine stopped and no counted wait (a load on
 * another OS thread) is outstanding, which empties the list */
static void Vm_EndFrame(OpenBGI_t* o)
{
	o->vm.frameOpen = 0;
	if(Panel_GetPollMode(o) == 1)
		Panel_PollAll(o); // after the knobs and the message pump
	Dbg_Pass(o);
	if(!o->vm.sched.running && WaitCount_Get(o) == 0)
		Thread_KillAllChildren(o, o->vm.rootThread);
}

/* The wait of a thread whose instruction asked for the disc
 * (RetryPrompt): the boxes, then 200 ms; then the thread runs the
 * instruction again (its position was put back to the instruction's
 * start), or, when the user gave up, the instruction's script error is
 * thrown as the original throws it from inside the instruction. */
typedef struct WaitRetry
{
	WaitStep_t s;
	Retry_t retry;
} WaitRetry_t;

static int WaitRetry_Step(OpenBGI_t* o, WaitStep_t* base)
{
	WaitRetry_t* w = (WaitRetry_t*)base;
	switch(Retry_Step(o, &w->retry))
	{
		case ASK_WAIT:
			base->idleMs = w->retry.pausing ? 10 : 0;
			return 0;
		case ASK_RETRY: return 1;
		default: BGI_ThrowInt(o, BGI_SCRIPT_ERROR_VALUE);
	}
}

/* An instruction of thread t threw BGI_RETRY_VALUE (RetryPrompt): put the
 * thread back to the start of the instruction (position and stack) and
 * let it wait for the disc, holding the engine; the pass goes on with it
 * afterwards. */
static void Vm_RetryInstruction(OpenBGI_t* o, Thread_t* t)
{
	WaitRetry_t* w = (WaitRetry_t*)WaitStep_New(o, t, sizeof *w, "WaitRetry", WaitRetry_Step, NULL);
	Vm_TempDrop(o);
	Thread_SetIP(t, t->opIP);
	t->sp = t->opSP;
	Retry_Begin(o, &w->retry);
	Thread_SetWait(o, t, &w->s.w);
	o->vm.holder = t;
	o->vm.resumeAt = t;
}

/* Boot while a boot is asked for (at the start, and after a reboot
 * emptied the list): reset every subsystem and load the boot program into
 * the first thread.  1 booted; 2 the boot program is missing and the disc
 * is being asked for (o->vm.bootRetry: once answered the load is tried
 * again, without a second reset, as the original's prompt inside the load
 * does); 0 when no boot was asked for or the boot failed, which ends the
 * run (the original leaves its main loop, whatever the failed boot left in
 * the list). */
static int Vm_Boot(OpenBGI_t* o)
{
	BgiTryFrame_t f;
	for(;;)
	{
		if(o->vm.bootRetry.active)
		{
			int r = Retry_Step(o, &o->vm.bootRetry);
			if(r == ASK_WAIT)
				return 2;
			if(r == ASK_ABORT)
			{
				o->vm.sched.bootOk = 0; // the script error the original throws: this boot failed
				continue;
			}
		}
		else
		{
			if(!o->vm.sched.restart)
				break;
			o->vm.sched.restart = 0;
			Vm_ResetSubsystems(o);
		}

		o->vm.sched.bootOk = 0;
		o->vm.booting = 1;
		if(BGI_TRY(o, f))
		{
			Vm_BootThread(o);
			BGI_END_TRY(o, f);
		}
		else if(f.value == BGI_RETRY_VALUE)
		{
			o->vm.booting = 0;
			Vm_TempDrop(o);
			Retry_Begin(o, &o->vm.bootRetry);
			return 2;
		}
		else
		{
			Vm_TempDrop(o);
			o->vm.sched.bootOk = 0; // catch (...)
		}
		o->vm.booting = 0;
		if(o->vm.sched.bootOk)
		{
			o->vm.sched.running = 1;
			return 1;
		}
	}
	o->vm.runEnded = 1;
	return 0;
}

/* The machine stopped while a thread held it (the window was closed, or
 * the holder's own wait raised a script error): its wait goes as the
 * original's instruction went when the error left it, and the pass that
 * waited for it is over. */
static void Vm_Unhold(OpenBGI_t* o)
{
	Thread_t* t = o->vm.holder;
	o->vm.holder = NULL;
	if(o->vm.resumeAt)
		o->vm.servicesDue = 1;
	o->vm.resumeAt = NULL;
	if(t)
		Thread_DropWait(o, t);
}

/*
 * The original's main loop, a pass per frame.  Vm_Start creates the root
 * thread and the semaphore manager and boots; then every Vm_Frame finishes
 * the previous pass (Vm_EndFrame), boots again when a reboot (scheduler
 * code 5) emptied the list, and runs a pass (Vm_RunPass, then
 * Vm_FrameServices) - until the thread list is empty.  A pass that left a
 * request unanswered or a thread holding the engine stops, and the frames
 * after it go on with it once nothing holds it any more; its services
 * come after the whole pass.  A script error or a quit stops the threads;
 * they are destroyed once no counted wait is outstanding.  Vm_Stop deletes
 * what Vm_Start created.
 */
void Vm_Start(OpenBGI_t* o)
{
	Vm_TraceInit(o);
	o->vm.rootThread = Thread_New(o, 0, 0, 0);
	o->sysobj.semMgr = SemMgr_New();
	o->vm.sched.restart = 1;
	o->vm.runEnded = 0;
	Vm_Boot(o);
}

int Vm_Frame(OpenBGI_t* o, OpenBGI_Frame_t* fr)
{
	BgiTryFrame_t f;
	Thread_t* from;
	int presented;
	volatile int done = 1; // read after a throw

	fr->presented = 0;
	fr->idleMs = 0;
	if(!o->vm.rootThread || o->vm.runEnded)
		return 0; // not started, stopped, or over
	if(!o->vm.sched.running && o->vm.holder)
		Vm_Unhold(o);
	Loader_Service(o); // a load that waits for its disc
	if(Request_Pending(o))
		return 1;
	from = o->vm.resumeAt;
	if(!from && !o->vm.servicesDue)
	{ // a new pass
		if(o->vm.frameOpen)
			Vm_EndFrame(o);
		if(Thread_Next(o->vm.rootThread) == NULL || o->vm.bootRetry.active)
		{
			int boot = Vm_Boot(o);
			if(boot == 0)
				return 0;
			if(boot == 2)
			{
				fr->idleMs = o->vm.bootRetry.pausing ? 10 : 0;
				return 1;
			}
		}
		if(Dbg_GateRun(o)) // 0 while the debugger holds the threads: the services only
			from = Thread_Next(o->vm.rootThread);
		if(from && Request_Pending(o))
		{
			o->vm.resumeAt = from; // what the end of the last frame asked first
			return 1;
		}
	}

	if(from)
	{
		o->vm.resumeAt = NULL;
		o->vm.sched.cur = NULL;
		if(BGI_TRY(o, f))
		{
			done = Vm_RunPass(o, from);
			BGI_END_TRY(o, f);
		}
		else if(f.value == BGI_RETRY_VALUE && o->vm.sched.cur)
		{
			o->vm.inPoll = 0;
			Vm_RetryInstruction(o, o->vm.sched.cur);
			done = 0;
		}
		else if(f.value == BGI_SCRIPT_ERROR_VALUE && Error_Keeping(o) && o->vm.sched.cur)
		{
			// 1.547 on, "81 6A": only the thread that raised the error
			// ends; event -1 tells the scripts (the message is kept for "81 6B")
			o->vm.inPoll = 0;
			Vm_TempDrop(o);
			MsgQueue_Post(o, 0xffffffff, 0, 0);
			Thread_KillChild(o, o->vm.rootThread, o->vm.sched.cur);
		}
		else
		{
			// catch (int): a script error, already reported.  The original
			// also has a catch (...) for hardware faults that shows
			// "何らかの問題が発生しました" ("Some problem has occurred",
			// MSG_SOMETHING_WRONG); C has no equivalent.
			o->vm.inPoll = 0;
			Vm_TempDrop(o);
			o->vm.sched.running = 0;
			if(o->vm.holder)
				Vm_Unhold(o);
		}
		o->vm.sched.cur = NULL;
		if(!done)
		{
			fr->idleMs = (int)WaitStep_IdleMs(o);
			return 1; // the pass waits; it goes on in a later frame
		}
		if(Request_Pending(o))
		{
			o->vm.servicesDue = 1; // a box the pass ended with: the services once it is answered
			return 1;
		}
	}

	o->vm.servicesDue = 0;
	presented = Vm_FrameServices(o);
	o->vm.frameOpen = 1;
	fr->presented = presented >= 0;
	fr->idleMs = Display_IdleMs(o, presented);
	return 1;
}

void Vm_Stop(OpenBGI_t* o)
{
	if(!o->vm.rootThread)
		return;
	if(o->vm.frameOpen)
		Vm_EndFrame(o);
	Vm_TraceShutdown(o);
	Thread_KillAllChildren(o, o->vm.rootThread); // (the original leaves them to the process's end)
	Thread_Delete(o, o->vm.rootThread);
	o->vm.rootThread = NULL;
	o->vm.runEnded = 0;
	o->vm.resumeAt = NULL;
	o->vm.holder = NULL;
	o->vm.servicesDue = 0;
	memset(&o->vm.bootRetry, 0, sizeof o->vm.bootRetry);
	Vm_TempDrop(o);
	SemMgr_Destroy(o->sysobj.semMgr);
	o->sysobj.semMgr = NULL;
}
