/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wait.h - wait objects: how a script thread blocks
 *
 * A handler that has to wait creates a Wait subclass, hands it to the thread
 * with Thread_SetWait() and returns scheduler code 2.  The scheduler polls the
 * object at the start of each of the thread's turns (Thread_PollWait): 0 keeps
 * waiting, 1 finishes (the object is destroyed and the thread continues), -1
 * aborts the machine.  Several classes push results onto the thread's stack
 * when they finish; the concrete classes and their starters are declared in
 * waitobj.h (and, for the text output, gfx/text.h).
 *
 * Besides the poll every wait carries a timer (deadline), a dirty flag that
 * Wait_Flush turns into a present request, and a queue of notifications sent
 * by "80 4C" (Thread_WaitNotify -> Wait_Notify); Wait_Tick delivers them to
 * the class's onMessage, a notification with a == 0 cancels the wait.
 * "80 50" switches blocking off: Wait_IsBlocking then reports 0 and the
 * interactive classes end at their next poll with their "abandoned" result
 * (the background loads and the encoders still wait for their job).
 */
#ifndef BGI_WAIT_H_
#define BGI_WAIT_H_

#include "bgi/common.h"
#include "bgi/vm.h"

typedef struct Wait Wait_t;

typedef struct WaitMsg // 16 bytes: one notification from "80 4C"
{
	uint32_t a, b, c;     // the three words of the notification; a selects the action
	struct WaitMsg* next; // queue link
} WaitMsg_t;

typedef struct WaitVtbl // the base table; the subclasses' tables extend it
{
	void (*destroy)(Wait_t* w);                 // scalar deleting destructor
	int (*poll)(Wait_t* w);                     // 0 waiting / 1 done / -1 failed
	void (*setTimer)(Wait_t* w, uint32_t ms);   // arm the deadline `ms` from now
	int (*isDirty)(Wait_t* w);                  // something to present
	void (*onMessage)(Wait_t* w, WaitMsg_t* m); // a notification with a != 0
} WaitVtbl_t;

struct Wait
{ // 0x20 bytes
	const WaitVtbl_t* vt;
	Thread_t* thread;   // owning thread
	uint32_t deadline;  // timer (GetTicks units, ms)
	int32_t dirty;      // something was drawn since the last flush
	int32_t cancelled;  // a notification with a == 0 arrived
	WaitMsg_t* msgHead; // notification queue
	WaitMsg_t* msgTail;
	uint32_t serial; // from gWaitSerial; key of the input-focus list
};

extern const WaitVtbl_t Wait_Vtbl;
extern int gWaitBlocking;    // 0 = every wait ends at once ("80 50")
extern int gPresentOnWait;   // a dirty wait requests a present ("90 0A")
extern int gPresentFull;     // ... of the whole picture rather than the dirty rectangles
extern uint32_t gWaitSerial; // the next serial handed out by Wait_Ctor

void Wait_Ctor(Wait_t* w, Thread_t* t);
void Wait_Dtor(Wait_t* w);    // frees the queue, leaves the input-focus list
void Wait_Destroy(Wait_t* w); // base deleting destructor
// "delete w": run the virtual deleting destructor of the actual class
static inline void Wait_Release(Wait_t* w)
{
	w->vt->destroy(w);
}

void Wait_SetBlocking(int on);                  // "80 50"
int Wait_IsBlocking(Wait_t* w);                 // blocking is on and the wait was not cancelled
void Wait_SetPresentMode(int onWait, int full); // "90 0A"
void Wait_Cancel(Wait_t* w);
void Wait_SetTimer(Wait_t* w, uint32_t ms); // deadline = now + ms
void Wait_TimerAdd(Wait_t* w, uint32_t ms); // deadline += ms
int Wait_TimerExpired(Wait_t* w);
void Wait_MarkDirty(Wait_t* w);
void Wait_Flush(Wait_t* w); // request a present when dirty, clear the flag
void Wait_SetDirty(Wait_t* w, int d);
void Wait_Tick(Wait_t* w);                                       // deliver queued notifications
void Wait_OnMessage(Wait_t* w, WaitMsg_t* m);                    // base does nothing
void Wait_Notify(Wait_t* w, uint32_t a, uint32_t b, uint32_t c); // queue a notification
int Wait_PopMsg(Wait_t* w, WaitMsg_t* out);                      // 1 and *out when one was queued
int Wait_IsDirtyBase(Wait_t* w);                                 // vtable slot 3 of the base: the dirty flag

/* The name of a wait object's class ("WaitTimerKey", "WaitMenuBmp" ..), for
 * the debugger; the classes have no names in the original.  Each file with
 * wait classes answers for its own vtables (NULL: not one of its classes). */
const char* Wait_ClassName(const Wait_t* w);
const char* WaitSys_ClassName(const WaitVtbl_t* vt);
const char* WaitTween_ClassName(const WaitVtbl_t* vt);
const char* WaitIcon_ClassName(const WaitVtbl_t* vt);
const char* WaitLoad_ClassName(const WaitVtbl_t* vt);
const char* WaitMenu_ClassName(const WaitVtbl_t* vt);
const char* WaitText_ClassName(const WaitVtbl_t* vt);
const char* WaitQuake_ClassName(const WaitVtbl_t* vt);

/* counted waits: objects with work on another OS thread (loads, encoders).
 * A subclass increments gWaitCounted while its job is outstanding; the
 * scheduler does not kill the threads at shutdown while the count is not 0. */
extern const WaitVtbl_t WaitCounted_Vtbl;
void WaitCounted_Ctor(Wait_t* w, Thread_t* t);
void WaitCounted_Dtor(Wait_t* w);
extern int32_t gWaitCounted;
void WaitCount_Inc(void);
void WaitCount_Dec(void);
int32_t WaitCount_Get(void);

#endif // BGI_WAIT_H_
