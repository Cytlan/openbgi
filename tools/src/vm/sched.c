/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sched.c - the main loop of the script VM: the scheduler that gives every
 *           thread its turn, the per-pass services, thread creation and
 *           the exclusive-thread switch (inc/bgi/vm.h)
 *
 * VmMain runs until the thread list empties: every pass gives each thread
 * a turn (Vm_RunThread executes its instructions until one yields), then
 * the frame services run (particles, panels, input, the display, the
 * window's message pump).  The scheduler codes the handlers return are
 * listed in docs/vm.md ("Threads and the scheduler").  The original wraps
 * the boot, the pass and the message pump in three C++ try blocks whose
 * catches set the flags of gSched; they are rendered with BGI_TRY frames
 * (inc/bgi/error.h), and a script error (ScriptError, a longjmp to the
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
#include "bgi/os.h"
#include "bgi/dbg.h"

uint32_t gSwitchThreadId;   // the thread that gets the turn after scheduler code 3 ("80 5E")
int gExclusiveOn;           // 1 while only gExclusiveThread runs ("80 5D")
Thread_t* gExclusiveThread; // that thread

// locals of VmMain that the "catch" blocks modify
static struct
{
	int restart;   // boot again when the list empties (scheduler code 5)
	int running;   // threads may execute instructions; 0 ends the machine once the counted waits are done
	Thread_t* cur; // thread whose turn it is (NULL between passes)
	int bootOk;    // the boot thread was created
} gSched;

/* make `t` the only thread the scheduler gives turns to ("80 5D"); NULL
 * lifts the restriction.  `on` is accepted for the caller's convenience
 * and ignored: the thread pointer alone decides. */
void SetExclusiveThread(Thread_t* t, int on)
{
	BGI_UNUSED(on);
	gExclusiveThread = t;
	gExclusiveOn = t != NULL;
}

/*
 * Load program `name` of archive `arc` into a fresh thread appended to the
 * list `parent` is part of ("80 44"; the boot thread), with the stack,
 * code and data sizes of Thread_New.  Returns the new thread's id.  A
 * program that loads as 0 bytes, or one too large for the code area, is a
 * script error raised in parent's context (a missing file makes LoadFile
 * itself prompt for the disc and throw when the user gives up).
 */
uint32_t SpawnThread(const char* arc, const char* name, uint32_t stackEntries,
	uint32_t codeSize, uint32_t dataSize, Thread_t* parent)
{
	uint8_t* image = (uint8_t*)BGI_Alloc(0x20000);
	uint32_t id = 0;
	char msg[0x104];

	if(LoadFile(image, arc, name) == 0)
	{
		sprintf(msg, MSG_PROGRAM_NOT_FOUND, arc, name);
		BGI_Free(image);
		ScriptError(msg, parent);
	}
	else
	{
		Thread_t* t = Thread_SpawnChild(parent, stackEntries, codeSize, dataSize);
		if(Thread_LoadModule(t, image, name) == 0x80000000u)
		{
			sprintf(msg, MSG_THREAD_CODE_FULL, arc, name);
			BGI_Free(image);
			ScriptError(msg, parent);
		}
		id = Thread_GetId(t);
	}
	BGI_Free(image);
	return id;
}

/* the definition of "fresh state" for a script: every subsystem back to
 * its defaults, before each boot (the first one and every reboot of
 * scheduler code 5) */
void Vm_ResetSubsystems(void)
{
	SemMgr_Clear(gSemMgr);
	Set_CloseMode(1);
	SetStyleChangeKeys(0, 0);
	Display_Enable(1);
	Present_SetRate(250);
	BmpMgr_SetOption(0);
	Sound_ResetVolumes();
	Sound_ClearTable();
	DynMem_FreeAll();
	UserOp_Clear();
	Knob_UnregisterAll();
	Knob_ReleaseAll();
	Knob_SetGlobal(1);
	Target_Clear();
	Input_ResetLayers();
	WinMsgWait_Clear();
	MsgQueue_Clear();
	KeyState_Clear();
	Input_SetSwapButtons(0);
	Gfx_ResetAll();
	Gfx_SetBmpOption(0); // the bitmap option of "90 0F"
	Set_SaveEncode(1);
	Set_InputEnable(1);
	Set_SkipKeyEnable(1);
	Set_SkipLatch(0);
	History_Reset(0);
	Ring_DeleteAll();
	StrTab_Clear(0);
	Dict_DeleteAll();
	Panel_DeleteAll();
	Ptcl_AutoClear();
	Set_FileSearch(1);
	FileSearch_Clear();
	Select_RequireActive(0);
	EnableDragDrop(0);
	Wait_SetBlocking(1);
	Wait_SetPresentMode(1, 0);
}

// body of the first try block: create the boot thread (a stack of 0x1000 entries, the profile's code and data sizes)
static void Vm_BootThread(void)
{
	char arc[0x104], name[0x104];
	GetBootProgram(arc, name);
	gSched.bootOk = SpawnThread(arc, name, 0x1000, gEngine->bootCode, gEngine->bootData, gRootThread) != 0;
}

/*
 * One turn of one thread: poll its wait object when it is blocked, then
 * execute instructions until a handler returns a code other than 0 or the
 * turn limit is reached.  Returns the scheduler code of the last
 * instruction (0 when the limit ended the turn), or 1 when the thread did
 * not run: another thread is exclusive, the wait is still pending, the
 * machine is stopping, or the debugger hit a breakpoint.
 */
static int Vm_RunThread(Thread_t* t)
{
	uint32_t count = 0;
	int rc = 0;

	if(gExclusiveOn && t != gExclusiveThread)
		return 1;
	if(Thread_GetFlags(t) & THR_FLAG_WAITING)
	{
		int w = Thread_PollWait(t);
		if(w == 0)
			return 1; // still blocked
		if(w == -1)
		{
			gSched.running = 0; // the wait failed: stop everything
			return 1;
		}
	}
	if(!gSched.running)
		return 1;

	while(rc == 0 && count < gDbgInsnLimit) // 0x100000, or 1 while the debugger steps by instruction
	{
		uint8_t op = Thread_FetchOp(t);
		if(gDbgHooked && Dbg_OnInsn(t, t->opIP, op))
		{ // a breakpoint: the instruction is not executed, the turn ends here
			Thread_SetIP(t, t->opIP);
			return 1;
		}
		if(gVmTraceOn)
			Vm_TraceBefore(t, op);
		if(!vm_optable_main[op])
		{
			char msg[0x100];
			sprintf(msg, MSG_UNDEFINED_OP, op);
			ScriptError(msg, t);
		}
		rc = vm_optable_main[op](t);
		if(gVmTraceOn)
			Vm_TraceAfter(t);
		count++;
	}
	return rc;
}

/* body of the second try block: one pass over all threads in list order,
 * acting on the scheduler code each turn ends with */
static void Vm_RunPass(void)
{
	Thread_t* t = Thread_Next(gRootThread);
	while(t)
	{
		gSched.cur = t;
		switch(Vm_RunThread(t))
		{
			case 3: // switch to another thread at once (an unknown id ends the pass)
				t = Thread_FindById(gRootThread, gSwitchThreadId);
				gSched.cur = t;
				continue;
			case 4: // the thread ends: start the pass over
				Thread_KillChild(gRootThread, t);
				t = gRootThread;
				gSched.cur = t;
				break;
			case 5: // reboot
				gSched.restart = 1;
				gSched.running = 0;
				break;
			case 6: // quit
				gSched.running = 0;
				break;
			default: // 0 (turn limit), 1, 2, > 6
				break;
		}
		t = Thread_Next(t);
	}
}

/* the per-pass services, after the threads had their turns; the message
 * pump is inside its own try block (WM_QUIT throws 0, which stops the
 * machine) */
void Vm_FrameServices(void)
{
	BgiTryFrame_t f;
	int presented;

	Ptcl_UpdateAll();
	Ptcl_AutoTick();
	Rain_UpdateAll();
	Rain_FrameTick();
	if(Panel_GetPollMode() == 0)
		Panel_PollAll();
	else
		Panel_PumpAll(); // "80 AF" mode 1 (1.573 on): the poll moves to the end of the pass
	Target_UpdateAll();
	Cursor_Update();
	CursorSprite_Update();
	Knob_Update();
	Input_Poll(1, 1);
	presented = PresentFrame();
	Movie_Tick();         // the ffmpeg player's frame, when a movie plays that way
	Dbg_OverlayPresent(); // the debugger over the picture, when it has no window of its own
	if(BGI_TRY(f))
	{
		while(PumpMessages())
			;
		BGI_END_TRY(f);
	}
	else
	{
		gSched.running = 0; // catch (int): WM_QUIT
	}
	Display_FrameIdle(presented); // the half-millisecond wait of 1.69; "80 52" from 1.535 on
	if(Panel_GetPollMode() == 1)
		Panel_PollAll(); // after the knobs and the message pump
}

/*
 * The main loop.  Creates the root thread and the semaphore manager, then
 * boots: reset every subsystem, load the boot program into the first
 * thread, and run passes (Vm_RunPass, then Vm_FrameServices and the
 * debugger's hook) until the thread list is empty.  A script error or a
 * quit stops the threads; they are destroyed once no counted wait (a load
 * on another OS thread) is outstanding, which empties the list.  A reboot
 * (scheduler code 5) goes through the boot again.  Returns 0.
 */
int VmMain(void)
{
	BgiTryFrame_t f;

	Vm_TraceInit();
	gRootThread = Thread_New(0, 0, 0);
	gSemMgr = SemMgr_New();

	gSched.restart = 1;
	while(gSched.restart)
	{
		gSched.restart = 0;
		Vm_ResetSubsystems();

		gSched.bootOk = 0;
		if(BGI_TRY(f))
		{
			Vm_BootThread();
			BGI_END_TRY(f);
		}
		else
		{
			gSched.bootOk = 0; // catch (...)
		}
		if(!gSched.bootOk)
			continue; // restart is 0 here: leaves the loop

		gSched.running = 1;
		while(Thread_Next(gRootThread) != NULL)
		{
			gSched.cur = NULL;
			if(BGI_TRY(f))
			{
				if(Dbg_GateRun()) // 0 while the debugger holds the threads
					Vm_RunPass();
				BGI_END_TRY(f);
			}
			else
			{
				// catch (int): a script error, already reported.  The original
				// also has a catch (...) for hardware faults that shows
				// "何らかの問題が発生しました" ("Some problem has occurred",
				// MSG_SOMETHING_WRONG); C has no equivalent.
				gSched.running = 0;
			}

			Vm_FrameServices();
			Dbg_Pass();

			if(!gSched.running && WaitCount_Get() == 0)
				Thread_KillAllChildren(gRootThread);
		}
	}

	Vm_TraceShutdown();
	Thread_Delete(gRootThread);
	gRootThread = NULL;
	SemMgr_Destroy(gSemMgr);
	gSemMgr = NULL;
	return 0;
}
