/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wait.c - the Wait base class and the counted-wait helpers
 *
 * Interface: wait.h.  The base class holds what every wait object shares:
 * the owning thread, a timer, the dirty flag that Wait_Flush turns into a
 * present request, the "80 4C" notification queue and the serial that keys
 * the input-focus list.  Its own poll finishes at once; the concrete classes
 * (wait_sys.c, wait_load.c, wait_tween.c, wait_text.c, wait_menu.c,
 * wait_icon.c, wait_quake.c) replace the vtable slots they need.  The
 * counted-wait counter at the end tells the scheduler how many waits still
 * have work outstanding on another OS thread.
 */
#include "bgi/wait.h"
#include "bgi/waitobj.h" // WaitStep
#include "bgi/sys.h"
#include "bgi/input.h"
#include "bgi/inputmap.h"
#include "bgi/display.h"
#include "bgi/core.h"

static int Wait_PollBase(OpenBGI_t* o, Wait_t* w) // slot 1 of the base: done at once
{
	BGI_UNUSED(w);
	return 1;
}

int Wait_IsDirtyBase(OpenBGI_t* o, Wait_t* w) // slot 3 of the base: the dirty flag
{
	return w->dirty;
}

const WaitVtbl_t Wait_Vtbl = {
	Wait_Destroy, Wait_PollBase, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage};

void Wait_Ctor(OpenBGI_t* o, Wait_t* w, Thread_t* t)
{
	w->vt = &Wait_Vtbl;
	w->thread = t;
	w->deadline = 0;
	w->dirty = 0;
	w->cancelled = 0;
	w->msgHead = w->msgTail = NULL;
	w->serial = o->wait.waitSerial++;
}

// frees the queued notifications and takes the wait off the input-focus list
void Wait_Dtor(OpenBGI_t* o, Wait_t* w)
{
	WaitMsg_t *m = w->msgHead, *nx;
	w->vt = &Wait_Vtbl;
	while(m)
	{
		nx = m->next;
		BGI_Free(m);
		m = nx;
	}
	// the list is keyed by the serial (InputFocus_Add in the menu and icon
	// classes); removing an entry that was never added is harmless
	InputFocus_Remove(o, w->serial);
}

void Wait_Destroy(OpenBGI_t* o, Wait_t* w)
{
	Wait_Dtor(o, w);
	BGI_Free(w);
}

void Wait_SetBlocking(OpenBGI_t* o, int on)
{
	o->wait.waitBlocking = on;
}

// 0 once "80 50" switched blocking off or a notification cancelled this wait
int Wait_IsBlocking(OpenBGI_t* o, Wait_t* w)
{
	return o->wait.waitBlocking && !w->cancelled;
}

void Wait_SetPresentMode(OpenBGI_t* o, int onWait, int full)
{
	o->wait.presentOnWait = onWait;
	o->wait.presentFull = full;
}

void Wait_Cancel(Wait_t* w)
{
	w->cancelled = 1;
}

void Wait_SetTimer(OpenBGI_t* o, Wait_t* w, uint32_t ms)
{
	w->deadline = GetTicks(o) + ms;
}

void Wait_TimerAdd(Wait_t* w, uint32_t ms)
{
	w->deadline += ms;
}

int Wait_TimerExpired(OpenBGI_t* o, Wait_t* w) // unsigned compare: a deadline of 0 has always expired
{
	return GetTicks(o) >= w->deadline;
}

void Wait_MarkDirty(Wait_t* w)
{
	Wait_SetDirty(w, 1);
}

void Wait_SetDirty(Wait_t* w, int d)
{
	w->dirty = d;
}

/* the end of a poll that may have drawn: when the class's isDirty says so
 * (and "90 0A" allows it) the next PresentFrame shows the whole picture or
 * the dirty rectangles; the flag is cleared either way */
void Wait_Flush(OpenBGI_t* o, Wait_t* w)
{
	if(w->vt->isDirty(o, w) && o->wait.presentOnWait)
	{
		if(o->wait.presentFull)
			Present_RequestFull(o);
		else
			Present_RequestDirty(o);
	}
	Wait_SetDirty(w, 0);
}

/* deliver the queued "80 4C" notifications to the class's onMessage, in
 * order; a notification with a == 0 cancels the wait instead and stops the
 * delivery (anything behind it stays queued) */
void Wait_Tick(OpenBGI_t* o, Wait_t* w)
{
	WaitMsg_t m;
	while(Wait_PopMsg(w, &m))
	{
		if(m.a == 0)
		{ // notification 0: cancel the wait
			Wait_Cancel(w);
			return;
		}
		w->vt->onMessage(o, w, &m);
	}
}

void Wait_OnMessage(OpenBGI_t* o, Wait_t* w, WaitMsg_t* m)
{
	BGI_UNUSED(w);
	BGI_UNUSED(m);
}

// append a notification (a, b, c) to the queue; Wait_Tick delivers it at the next poll
void Wait_Notify(Wait_t* w, uint32_t a, uint32_t b, uint32_t c)
{
	WaitMsg_t* m = (WaitMsg_t*)BGI_Alloc(sizeof(WaitMsg_t));
	m->a = a;
	m->b = b;
	m->c = c;
	m->next = NULL;
	if(w->msgHead)
	{
		w->msgTail->next = m;
		w->msgTail = m;
	}
	else
	{
		w->msgHead = w->msgTail = m;
	}
}

// take the oldest notification off the queue into *out; 0 when the queue is empty
int Wait_PopMsg(Wait_t* w, WaitMsg_t* out)
{
	WaitMsg_t* m = w->msgHead;
	if(!m)
		return 0;
	w->msgHead = m->next;
	*out = *m;
	out->next = NULL;
	BGI_Free(m);
	return 1;
}

// ---- counted waits -----------------------------------------------------

void WaitCount_Inc(OpenBGI_t* o)
{
	o->wait.waitCounted++;
}

void WaitCount_Dec(OpenBGI_t* o)
{
	o->wait.waitCounted--;
}

// the scheduler kills the threads at shutdown only once this reaches 0
int32_t WaitCount_Get(OpenBGI_t* o)
{
	return o->wait.waitCounted;
}

// ---- the names of the wait classes (the debugger's; nothing of this is in the original) ----

// the class name of `w` ("Wait" for the bare base, "Wait?" for an unknown vtable, "-" for NULL)
const char* Wait_ClassName(const Wait_t* w)
{
	static const char* (*const kFiles[])(const WaitVtbl_t*) = {WaitSys_ClassName, WaitTween_ClassName, WaitIcon_ClassName,
		WaitLoad_ClassName, WaitMenu_ClassName, WaitText_ClassName, WaitQuake_ClassName, WaitJob_ClassName};
	size_t i;
	if(!w)
		return "-";
	if(w->vt == &Wait_Vtbl)
		return "Wait";
	if(w->vt == &WaitStep_Vtbl)
		return WaitStep_ClassName(w);
	for(i = 0; i < sizeof kFiles / sizeof kFiles[0]; i++)
	{
		const char* n = kFiles[i](w->vt);
		if(n)
			return n;
	}
	return "Wait?";
}

/* The clickable targets of a thread's wait (bgi/inputmap.h): the file of
 * the wait's class reports them; the classes that take no mouse input
 * have none. */
void Wait_Targets(OpenBGI_t* o, Wait_t* w, TargetSink_t* s)
{
	static int (*const kFiles[])(OpenBGI_t*, Wait_t*, TargetSink_t*) = {WaitSys_Targets, WaitTween_Targets, WaitIcon_Targets,
		WaitMenu_Targets, WaitText_Targets, WaitQuake_Targets};
	size_t i;
	for(i = 0; i < sizeof kFiles / sizeof kFiles[0]; i++)
		if(kFiles[i](o, w, s))
			return;
}
