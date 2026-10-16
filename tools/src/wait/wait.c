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
#include "bgi/sys.h"
#include "bgi/input.h"
#include "bgi/display.h"

int gWaitBlocking = 1;    // "80 50": 0 makes every wait end at its next poll
int gPresentOnWait = 1;   // "90 0A": a dirty wait requests a present
int gPresentFull = 0;     // "90 0A": ... of the whole picture
uint32_t gWaitSerial = 0; // the next serial Wait_Ctor hands out
int32_t gWaitCounted = 0; // waits with work outstanding on another OS thread

static int Wait_PollBase(Wait_t* w) // slot 1 of the base: done at once
{
	BGI_UNUSED(w);
	return 1;
}

int Wait_IsDirtyBase(Wait_t* w) // slot 3 of the base: the dirty flag
{
	return w->dirty;
}

const WaitVtbl_t Wait_Vtbl = {
	Wait_Destroy, Wait_PollBase, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage};

void Wait_Ctor(Wait_t* w, Thread_t* t)
{
	w->vt = &Wait_Vtbl;
	w->thread = t;
	w->deadline = 0;
	w->dirty = 0;
	w->cancelled = 0;
	w->msgHead = w->msgTail = NULL;
	w->serial = gWaitSerial++;
}

// frees the queued notifications and takes the wait off the input-focus list
void Wait_Dtor(Wait_t* w)
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
	InputFocus_Remove(w->serial);
}

void Wait_Destroy(Wait_t* w)
{
	Wait_Dtor(w);
	BGI_Free(w);
}

void Wait_SetBlocking(int on)
{
	gWaitBlocking = on;
}

// 0 once "80 50" switched blocking off or a notification cancelled this wait
int Wait_IsBlocking(Wait_t* w)
{
	return gWaitBlocking && !w->cancelled;
}

void Wait_SetPresentMode(int onWait, int full)
{
	gPresentOnWait = onWait;
	gPresentFull = full;
}

void Wait_Cancel(Wait_t* w)
{
	w->cancelled = 1;
}

void Wait_SetTimer(Wait_t* w, uint32_t ms)
{
	w->deadline = GetTicks() + ms;
}

void Wait_TimerAdd(Wait_t* w, uint32_t ms)
{
	w->deadline += ms;
}

int Wait_TimerExpired(Wait_t* w) // unsigned compare: a deadline of 0 has always expired
{
	return GetTicks() >= w->deadline;
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
void Wait_Flush(Wait_t* w)
{
	if(w->vt->isDirty(w) && gPresentOnWait)
	{
		if(gPresentFull)
			Present_RequestFull();
		else
			Present_RequestDirty();
	}
	Wait_SetDirty(w, 0);
}

/* deliver the queued "80 4C" notifications to the class's onMessage, in
 * order; a notification with a == 0 cancels the wait instead and stops the
 * delivery (anything behind it stays queued) */
void Wait_Tick(Wait_t* w)
{
	WaitMsg_t m;
	while(Wait_PopMsg(w, &m))
	{
		if(m.a == 0)
		{ // notification 0: cancel the wait
			Wait_Cancel(w);
			return;
		}
		w->vt->onMessage(w, &m);
	}
}

void Wait_OnMessage(Wait_t* w, WaitMsg_t* m)
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

void WaitCount_Inc(void)
{
	gWaitCounted++;
}

void WaitCount_Dec(void)
{
	gWaitCounted--;
}

// the scheduler kills the threads at shutdown only once this reaches 0
int32_t WaitCount_Get(void)
{
	return gWaitCounted;
}

// ---- the names of the wait classes (the debugger's; nothing of this is in the original) ----

// the class name of `w` ("Wait" for the bare base, "Wait?" for an unknown vtable, "-" for NULL)
const char* Wait_ClassName(const Wait_t* w)
{
	static const char* (*const kFiles[])(const WaitVtbl_t*) = {WaitSys_ClassName, WaitTween_ClassName, WaitIcon_ClassName,
		WaitLoad_ClassName, WaitMenu_ClassName, WaitText_ClassName, WaitQuake_ClassName};
	size_t i;
	if(!w)
		return "-";
	if(w->vt == &Wait_Vtbl)
		return "Wait";
	for(i = 0; i < sizeof kFiles / sizeof kFiles[0]; i++)
	{
		const char* n = kFiles[i](w->vt);
		if(n)
			return n;
	}
	return "Wait?";
}
