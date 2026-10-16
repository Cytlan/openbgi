/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * thread.c - threads, critical sections and atomics of the Win32 back
 *            end (inc/bgi/os.h): OsLock_t over a CRITICAL_SECTION,
 *            OsThread_t over CreateThread, the Interlocked* counters
 *
 * The locks are recursive, as the engine's callers expect; a thread is
 * joined (waited for and freed) with OS_ThreadJoin and must have returned
 * from its function by then.
 */
#include "sys_internal.h"

struct OsLock
{
	CRITICAL_SECTION cs;
};

OsLock_t* OS_LockCreate(void)
{
	OsLock_t* l = (OsLock_t*)calloc(1, sizeof *l);
	InitializeCriticalSection(&l->cs);
	return l;
}

void OS_LockDestroy(OsLock_t* l)
{
	if(!l)
		return;
	DeleteCriticalSection(&l->cs);
	free(l);
}

void OS_LockEnter(OsLock_t* l)
{
	EnterCriticalSection(&l->cs);
}

void OS_LockLeave(OsLock_t* l)
{
	LeaveCriticalSection(&l->cs);
}

struct OsThread
{
	HANDLE h;        // the thread handle, closed by OS_ThreadJoin
	OsThreadFn_t fn; // the function the thread runs
	void* arg;       // its argument
};

// the thread entry: run the function once, then end the thread
static DWORD WINAPI ThreadMain(LPVOID p)
{
	OsThread_t* t = (OsThread_t*)p;
	t->fn(t->arg);
	return 0;
}

// CreateThread running fn(arg); OS_PRIO_HIGHEST raises it to THREAD_PRIORITY_HIGHEST, any other priority is left at the default; NULL on failure
OsThread_t* OS_ThreadStart(OsThreadFn_t fn, void* arg, int priority)
{
	OsThread_t* t = (OsThread_t*)calloc(1, sizeof *t);
	DWORD id;
	t->fn = fn;
	t->arg = arg;
	t->h = CreateThread(NULL, 0, ThreadMain, t, 0, &id);
	if(!t->h)
	{
		free(t);
		return NULL;
	}
	if(priority == OS_PRIO_HIGHEST)
		SetThreadPriority(t->h, THREAD_PRIORITY_HIGHEST);
	return t;
}

// wait for the thread to end, then close the handle and free the record
void OS_ThreadJoin(OsThread_t* t)
{
	if(!t)
		return;
	WaitForSingleObject(t->h, INFINITE);
	CloseHandle(t->h);
	free(t);
}

int OS_CpuCount(void)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return (int)si.dwNumberOfProcessors;
}

int32_t OS_AtomicInc(volatile int32_t* p)
{
	return (int32_t)InterlockedIncrement((volatile LONG*)p);
}

int32_t OS_AtomicDec(volatile int32_t* p)
{
	return (int32_t)InterlockedDecrement((volatile LONG*)p);
}
