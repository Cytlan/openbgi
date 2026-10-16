/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wait_sys.c - the wait classes of the system-control instructions
 *
 * Interface: waitobj.h (the constructors) and wait.h (WaitCounted).
 *
 *   WaitCounted   base of waits whose work runs on another thread
 *   WaitTimer     "80 5A": the thread timer; pushes nothing
 *   WaitTimerKey  "80 5C": a timer that input can cut short; pushes 1 / 0
 *   WaitWinMsg    "80 54": a window message; pushes wParam, lParam
 *   WaitLock      "80 B4": a named lock; pushes 0 / -1 / 0x80000001
 *   WaitEncode    "80 C0": SDC encoding; pushes the encoded size
 *   WaitEncode2   "80 C4": DCFS + SDC encoding; pushes the encoded size
 *
 * The encoders run on a worker thread in the original (started with
 * _beginthread at priority 2).  Here they run synchronously inside the
 * constructor: the only thing a script can observe is the result pushed by
 * poll(), which is identical, and the VM never gets a turn between the two
 * in the original either (the wait returns scheduler code 2 at once).
 */
#include "bgi/waitobj.h"
#include "bgi/wait.h"
#include "bgi/vm.h"
#include "bgi/sys.h"
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/sysobj.h"
#include "bgi/codec.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/os.h"

// =========================================================================
// WaitCounted
// =========================================================================

static void WaitCounted_Destroy(Wait_t* w);
// the base poll: done at once (in the original the same routine served as "80 5F")
static int Wait_PollDone(Wait_t* w)
{
	BGI_UNUSED(w);
	return 1;
}

const WaitVtbl_t WaitCounted_Vtbl = {
	WaitCounted_Destroy, Wait_PollDone, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage};

// the subclass increments the counter itself once its job is queued
void WaitCounted_Ctor(Wait_t* w, Thread_t* t)
{
	Wait_Ctor(w, t);
	w->vt = &WaitCounted_Vtbl;
}

void WaitCounted_Dtor(Wait_t* w)
{
	w->vt = &WaitCounted_Vtbl;
	Wait_Dtor(w);
}

static void WaitCounted_Destroy(Wait_t* w)
{
	WaitCounted_Dtor(w);
	BGI_Free(w);
}

// =========================================================================
// WaitTimer
// =========================================================================

static void WaitTimer_Destroy(Wait_t* w);
static int WaitTimer_Poll(Wait_t* w);
static const WaitVtbl_t WaitTimer_Vtbl = {
	WaitTimer_Destroy, WaitTimer_Poll, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage};

// block for `ms` milliseconds (the remainder of the thread timer in "80 5A")
Wait_t* WaitTimer_New(Thread_t* t, uint32_t ms)
{
	Wait_t* w = (Wait_t*)BGI_Alloc(sizeof(Wait_t));
	Wait_Ctor(w, t);
	w->vt = &WaitTimer_Vtbl;
	Wait_SetTimer(w, ms);
	return w;
}

static void WaitTimer_Destroy(Wait_t* w)
{
	w->vt = &WaitTimer_Vtbl;
	Wait_Dtor(w);
	BGI_Free(w);
}

// done when the time is up or waits stopped blocking; pushes nothing
static int WaitTimer_Poll(Wait_t* w)
{
	Wait_Tick(w);
	if(Wait_TimerExpired(w) || !Wait_IsBlocking(w))
		return 1;
	return 0;
}

// =========================================================================
// WaitTimerKey
// =========================================================================

typedef struct WaitTimerKey // 0x2c bytes
{
	Wait_t base;
	int32_t keyBreak;    // input may end the wait
	uint32_t prio;       // input layer priority (the layer key is (prio << 20) | 0xfffff)
	int32_t interrupted; // notification 1 arrived
} WaitTimerKey_t;

static void WaitTimerKey_Destroy(Wait_t* w);
static int WaitTimerKey_Poll(Wait_t* w);
static void WaitTimerKey_OnMessage(Wait_t* w, WaitMsg_t* m);
static const WaitVtbl_t WaitTimerKey_Vtbl = {
	WaitTimerKey_Destroy, WaitTimerKey_Poll, Wait_SetTimer, Wait_IsDirtyBase, WaitTimerKey_OnMessage};

/* block for `ms` milliseconds; with keyBreak the wait pushes a mouse and a
 * key layer at `prio` and input on them ends it early (as does a "80 4C"
 * notification with a = 1).  Pending triggers are swallowed so a press that
 * started the instruction does not end it. */
Wait_t* WaitTimerKey_New(Thread_t* t, uint32_t ms, int keyBreak, uint32_t prio)
{
	WaitTimerKey_t* w = (WaitTimerKey_t*)BGI_Alloc(sizeof(WaitTimerKey_t));
	Wait_Ctor(&w->base, t);
	w->base.vt = &WaitTimerKey_Vtbl;
	Wait_SetTimer(&w->base, ms);
	w->keyBreak = keyBreak;
	if(keyBreak)
	{
		uint32_t key = (prio << 20) | 0xfffff;
		w->prio = prio;
		MouseLayer_Push(key);
		KeyLayer_Push(key);
		Input_Poll(key, key); // swallow pending triggers
	}
	w->interrupted = 0;
	return &w->base;
}

static void WaitTimerKey_Destroy(Wait_t* wb)
{
	WaitTimerKey_t* w = (WaitTimerKey_t*)wb;
	w->base.vt = &WaitTimerKey_Vtbl;
	if(w->keyBreak)
	{
		uint32_t key = (w->prio << 20) | 0xfffff;
		MouseLayer_Pop(key);
		KeyLayer_Pop(key);
	}
	Wait_Dtor(&w->base);
	BGI_Free(w);
}

/* pushes 1 when input ended the wait, 0 when the timer ran out, a
 * notification interrupted it or waits stopped blocking */
static int WaitTimerKey_Poll(Wait_t* wb)
{
	WaitTimerKey_t* w = (WaitTimerKey_t*)wb;
	Wait_Tick(wb);
	if(!Wait_TimerExpired(wb) && Wait_IsBlocking(wb) && !w->interrupted)
	{
		uint32_t key;
		if(!w->keyBreak)
			return 0;
		key = (w->prio << 20) | 0xfffff;
		// the input that ends the wait: skip (bit 31), the down key (0x2000),
		// decide (0x100), wheel down (0x80) and the left button (0x1)
		if(!(Input_Poll(key, key) & 0x80002181u))
			return 0;
		Thread_Push(wb->thread, 1);
		return 1;
	}
	Thread_Push(wb->thread, 0);
	return 1;
}

// "80 4C" with a = 1 ends the wait at the next poll (with result 0)
static void WaitTimerKey_OnMessage(Wait_t* wb, WaitMsg_t* m)
{
	if(m->a == 1)
		((WaitTimerKey_t*)wb)->interrupted = 1;
}

// =========================================================================
// WaitWinMsg
// =========================================================================

typedef struct WaitWinMsg // 0x24 bytes
{
	Wait_t base;
	uint32_t msg; // window message number
} WaitWinMsg_t;

static void WaitWinMsg_Destroy(Wait_t* w);
static int WaitWinMsg_Poll(Wait_t* w);
static const WaitVtbl_t WaitWinMsg_Vtbl = {
	WaitWinMsg_Destroy, WaitWinMsg_Poll, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage};

// block until the window receives message number `msg` (subscribed through WinMsgWait_Register)
Wait_t* WaitWinMsg_New(Thread_t* t, uint32_t msg)
{
	WaitWinMsg_t* w = (WaitWinMsg_t*)BGI_Alloc(sizeof(WaitWinMsg_t));
	Wait_Ctor(&w->base, t);
	w->base.vt = &WaitWinMsg_Vtbl;
	w->msg = msg;
	WinMsgWait_Register(t, msg); // keyed by the thread, as in the original
	return &w->base;
}

static void WaitWinMsg_Destroy(Wait_t* wb)
{
	WaitWinMsg_t* w = (WaitWinMsg_t*)wb;
	w->base.vt = &WaitWinMsg_Vtbl;
	WinMsgWait_Unregister(wb->thread, w->msg);
	Wait_Dtor(wb);
	BGI_Free(w);
}

/* pushes wParam and lParam of the message, or -1, -1 when the wait was
 * abandoned (non-blocking mode).  A lost subscription ends the wait without
 * pushing anything, after a message box. */
static int WaitWinMsg_Poll(Wait_t* wb)
{
	WaitWinMsg_t* w = (WaitWinMsg_t*)wb;
	WinMsgRec_t rec;
	Wait_Tick(wb);
	if(!WinMsgWait_Poll(&rec, wb->thread, w->msg))
	{
		// the registration vanished: a bug in the original, reported as such
		OS_MessageBox(MSG_WINMSG_LOST, MSG_BUG_FOUND, 0);
		return 1;
	}
	if(!Wait_IsBlocking(wb))
	{
		Thread_Push(wb->thread, 0xffffffffu);
		Thread_Push(wb->thread, 0xffffffffu);
		return 1;
	}
	if(!rec.arrived)
		return 0;
	Thread_Push(wb->thread, rec.wParam);
	Thread_Push(wb->thread, rec.lParam);
	return 1;
}

// =========================================================================
// WaitLock
// =========================================================================

typedef struct WaitLock // 0x128 bytes
{
	Wait_t base;
	int32_t queued; // the lock exists and the thread is queued for it
	SemMgr_t* mgr;
	char name[0x100]; // the lock's name
} WaitLock_t;

static void WaitLock_Destroy(Wait_t* w);
static int WaitLock_Poll(Wait_t* w);
static const WaitVtbl_t WaitLock_Vtbl = {
	WaitLock_Destroy, WaitLock_Poll, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage};

/* queue the thread for lock `name` at priority `prio` (behind the waiters
 * of equal or higher priority) and block until it is admitted.  An unknown
 * lock is not an error here: the poll reports it. */
Wait_t* WaitLock_New(Thread_t* t, SemMgr_t* mgr, const char* name, uint32_t prio)
{
	WaitLock_t* w = (WaitLock_t*)BGI_Alloc(sizeof(WaitLock_t));
	Wait_Ctor(&w->base, t);
	w->base.vt = &WaitLock_Vtbl;
	w->mgr = mgr;
	strcpy(w->name, name);
	w->queued = (int32_t)SemMgr_Enqueue(mgr, name, Thread_GetId(t), prio);
	return &w->base;
}

static void WaitLock_Destroy(Wait_t* wb)
{
	wb->vt = &WaitLock_Vtbl;
	Wait_Dtor(wb);
	BGI_Free(wb);
}

/* pushes 0 when the lock was acquired, -1 when abandoned (non-blocking
 * mode), 0x80000001 when there is no such lock.  The place in the queue is
 * kept on abandonment; "80 B5" gives it back. */
static int WaitLock_Poll(Wait_t* wb)
{
	WaitLock_t* w = (WaitLock_t*)wb;
	Wait_Tick(wb);
	if(!w->queued)
	{
		Thread_Push(wb->thread, 0x80000001u);
		return 1;
	}
	if(!Wait_IsBlocking(wb))
	{
		Thread_Push(wb->thread, 0xffffffffu);
		return 1;
	}
	if(SemMgr_TryAcquire(w->mgr, w->name, Thread_GetId(wb->thread)) != 0)
		return 0;
	Thread_Push(wb->thread, 0);
	return 1;
}

// =========================================================================
// WaitEncode / WaitEncode2
// =========================================================================

typedef struct EncodeJob // 0x18 / 0x1c bytes: the worker's job record
{
	int32_t done;    // the worker has finished
	void* dst;       // the output buffer
	uint32_t result; // encoded size, 0 on failure
	const void* src; // the input
	uint32_t a, b;   // SDC: size in bytes; DCFS: frame size in bytes, frame count
} EncodeJob_t;

typedef struct WaitEncode // 0x24 bytes
{
	Wait_t base;
	EncodeJob_t* job;
} WaitEncode_t;

static void WaitEncode_Destroy(Wait_t* w);
static void WaitEncode2_Destroy(Wait_t* w);
static int WaitEncode_Poll(Wait_t* w);
static const WaitVtbl_t WaitEncode_Vtbl = {
	WaitEncode_Destroy, WaitEncode_Poll, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage};
static const WaitVtbl_t WaitEncode2_Vtbl = {
	WaitEncode2_Destroy, WaitEncode_Poll, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage};

// the SDC worker: encode `a` bytes of src into dst
static void SdcEncodeJob_Run(EncodeJob_t* j)
{
	j->result = SdcEncode(j->dst, j->src, j->a);
	j->done = 1;
}

/* the DCFS worker: difference-code the `b` frames of `a` bytes into a
 * temporary buffer of 1.5 x the frame data, then SDC-encode that into dst */
static void DcfsEncodeJob_Run(EncodeJob_t* j)
{
	uint32_t frameBytes = j->a * j->b;
	uint8_t* tmp = (uint8_t*)BGI_Alloc(frameBytes * 3 / 2 + 0x18);
	DcfsEncode(tmp, &j->result, j->src, j->a, j->b);
	j->result = SdcEncode(j->dst, tmp, j->result);
	BGI_Free(tmp);
	j->done = 1;
}

/* SDC-encode `size` bytes of src into dst ("80 C0"); dst needs room for
 * size + size / 128 + 1 bytes plus the header */
Wait_t* WaitEncode_New(Thread_t* t, void* dst, const void* src, uint32_t size)
{
	WaitEncode_t* w = (WaitEncode_t*)BGI_Alloc(sizeof(WaitEncode_t));
	WaitCounted_Ctor(&w->base, t);
	w->base.vt = &WaitEncode_Vtbl;
	w->job = (EncodeJob_t*)BGI_Calloc(sizeof(EncodeJob_t));
	w->job->dst = dst;
	w->job->src = src;
	w->job->a = size;
	WaitCount_Inc();
	SdcEncodeJob_Run(w->job);
	return &w->base;
}

// DCFS + SDC-encode frameCount frames of frameSize bytes from src into dst ("80 C4")
Wait_t* WaitEncode2_New(Thread_t* t, void* dst, const void* src, uint32_t frameSize, uint32_t frameCount)
{
	WaitEncode_t* w = (WaitEncode_t*)BGI_Alloc(sizeof(WaitEncode_t));
	WaitCounted_Ctor(&w->base, t);
	w->base.vt = &WaitEncode2_Vtbl;
	w->job = (EncodeJob_t*)BGI_Calloc(sizeof(EncodeJob_t));
	w->job->dst = dst;
	w->job->src = src;
	w->job->a = frameSize;
	w->job->b = frameCount;
	WaitCount_Inc();
	DcfsEncodeJob_Run(w->job);
	return &w->base;
}

// the shared destructor body: the job record, the counter, the base
static void WaitEncode_Free(WaitEncode_t* w)
{
	BGI_Free(w->job);
	WaitCount_Dec();
	WaitCounted_Dtor(&w->base);
	BGI_Free(w);
}

static void WaitEncode_Destroy(Wait_t* w)
{
	w->vt = &WaitEncode_Vtbl;
	WaitEncode_Free((WaitEncode_t*)w);
}

static void WaitEncode2_Destroy(Wait_t* w)
{
	w->vt = &WaitEncode2_Vtbl;
	WaitEncode_Free((WaitEncode_t*)w);
}

// pushes the encoded size (0 on failure) once the worker is done
static int WaitEncode_Poll(Wait_t* wb)
{
	WaitEncode_t* w = (WaitEncode_t*)wb;
	Wait_Tick(wb);
	if(!w->job->done)
		return 0;
	Thread_Push(wb->thread, w->job->result);
	return 1;
}

// the name of a wait class of this file, for the debugger (NULL: not one of these)
const char* WaitSys_ClassName(const WaitVtbl_t* vt)
{
	if(vt == &WaitCounted_Vtbl)
		return "WaitCounted";
	if(vt == &WaitTimer_Vtbl)
		return "WaitTimer";
	if(vt == &WaitTimerKey_Vtbl)
		return "WaitTimerKey";
	if(vt == &WaitWinMsg_Vtbl)
		return "WaitWinMsg";
	if(vt == &WaitLock_Vtbl)
		return "WaitLock";
	if(vt == &WaitEncode_Vtbl)
		return "WaitEncode";
	if(vt == &WaitEncode2_Vtbl)
		return "WaitEncode2";
	return NULL;
}
