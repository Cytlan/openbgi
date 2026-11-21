/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wait_step.c - the wait of an instruction that waited inside itself in
 *               the original (src/include/bgi/waitobj.h); not part of the original
 *               engine
 *
 * The original's message boxes, dialogs, disc prompts and program waits
 * ran inside the instruction that asked for them, with every other thread
 * standing still.  Here such an instruction gives its thread a WaitStep
 * and returns: the step function does the waiting one frame at a time
 * (taking a request's answer, polling a drive or a process), and while the
 * step is not done its thread holds the engine (o->vm.holder): the
 * scheduler gives no other thread a turn and the frame presents nothing
 * (src/vm/sched.c).  Once the step is done the thread goes on with its
 * turn, and the pass with the threads after it.
 */
#include "bgi/waitobj.h"
#include "bgi/vm.h"
#include "bgi/request.h"
#include "bgi/error.h"
#include "bgi/core.h"

#include <string.h>

static void WaitStep_Destroy(OpenBGI_t* o, Wait_t* base)
{
	WaitStep_t* w = (WaitStep_t*)base;
	if(o->vm.holder == base->thread)
		o->vm.holder = NULL;
	if(w->release)
		w->release(o, w);
	Wait_Dtor(o, base);
	BGI_Free(w);
}

static int WaitStep_Poll(OpenBGI_t* o, Wait_t* base)
{
	WaitStep_t* w = (WaitStep_t*)base;
	return w->step(o, w);
}

const WaitVtbl_t WaitStep_Vtbl = {WaitStep_Destroy, WaitStep_Poll, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage};

WaitStep_t* WaitStep_New(OpenBGI_t* o, Thread_t* t, size_t size, const char* name, WaitStepFn_t step, WaitStepFreeFn_t release)
{
	WaitStep_t* w = (WaitStep_t*)BGI_Calloc(size < sizeof(WaitStep_t) ? sizeof(WaitStep_t) : size);
	Wait_Ctor(o, &w->w, t);
	w->w.vt = &WaitStep_Vtbl;
	w->name = name;
	w->step = step;
	w->release = release;
	return w;
}

int WaitStep_Begin(OpenBGI_t* o, WaitStep_t* w)
{
	Thread_t* t = w->w.thread;
	int r = w->step(o, w);
	if(r != 0)
	{
		Wait_Release(o, &w->w);
		if(r > 0)
			return 0;            // done at once: the instruction is over
		o->vm.sched.running = 0; // as a wait that fails
		return 1;
	}
	Thread_SetWait(o, t, &w->w);
	o->vm.holder = t;
	return 2;
}

void WaitStep_Discard(OpenBGI_t* o, WaitStep_t* w)
{
	Wait_Release(o, &w->w);
}

uint32_t WaitStep_IdleMs(OpenBGI_t* o)
{
	Thread_t* t = o->vm.holder;
	if(!t || !t->wait || t->wait->vt != &WaitStep_Vtbl)
		return 0;
	return ((WaitStep_t*)t->wait)->idleMs;
}

const char* WaitStep_ClassName(const Wait_t* w)
{
	if(w->vt != &WaitStep_Vtbl)
		return NULL;
	return ((const WaitStep_t*)w)->name;
}

// ---- one question to the host -------------------------------------------------

static int WaitAsk_Step(OpenBGI_t* o, WaitStep_t* base)
{
	WaitAsk_t* w = (WaitAsk_t*)base;
	int answer;
	if(!Request_Take(o, w->req, &answer))
		return 0;
	w->req = 0;
	if(w->done)
		w->done(o, w, answer);
	else
		Thread_Push(base->w.thread, (uint32_t)answer);
	return 1;
}

static void WaitAsk_Release(OpenBGI_t* o, WaitStep_t* base)
{
	WaitAsk_t* w = (WaitAsk_t*)base;
	Request_Forget(o, w->req); // a question the wait does not see answered (the thread ended)
	w->req = 0;
	if(w->release)
		w->release(o, base);
}

WaitAsk_t* WaitAsk_New(OpenBGI_t* o, Thread_t* t, size_t size, const char* name, WaitAskDoneFn_t done, WaitStepFreeFn_t release)
{
	WaitAsk_t* w = (WaitAsk_t*)WaitStep_New(o, t, size < sizeof(WaitAsk_t) ? sizeof(WaitAsk_t) : size, name, WaitAsk_Step, WaitAsk_Release);
	w->done = done;
	w->release = release;
	return w;
}

int WaitAsk_Begin(OpenBGI_t* o, WaitAsk_t* w, uint32_t req)
{
	w->req = req;
	return WaitStep_Begin(o, &w->s);
}

void WaitAsk_Is(OpenBGI_t* o, WaitAsk_t* w, int answer)
{
	(void)o;
	Thread_Push(w->s.w.thread, answer == w->arg);
}

void WaitAsk_Chosen(OpenBGI_t* o, WaitAsk_t* w, int answer)
{
	(void)o;
	Thread_Push(w->s.w.thread, answer ? 0u : 0xffffffffu);
}

int WaitAsk_Box(OpenBGI_t* o, Thread_t* t, const char* text, const char* caption, uint32_t flags, int yes, int script)
{
	WaitAsk_t* w = WaitAsk_New(o, t, sizeof *w, "WaitBox", WaitAsk_Is, NULL);
	w->arg = yes;
	return WaitAsk_Begin(o, w, script ? MsgBoxScript_Ask(o, text, caption, flags) : MsgBox_Ask(o, text, caption, flags));
}
