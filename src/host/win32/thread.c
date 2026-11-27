/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * thread.c - the threads and the processor count of the Win32 back end
 *            (src/host/os.h): OsThread_t over CreateThread
 *
 * A thread is joined (waited for and freed) with OS_ThreadJoin and must
 * have returned from its function by then.
 */
#include "sys_internal.h"

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
	else if(priority == OS_PRIO_LOW)
		SetThreadPriority(t->h, THREAD_PRIORITY_BELOW_NORMAL);
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

// ---- the engine's own threads (OpenBGI_Host_t.threads) --------------------------------------------
//
// A lock is a critical section.  A signal is a semaphore and the count of the threads that wait for it: the
// engine wakes with the waiters' lock held, so the count is only ever changed under that lock, and a wake that
// comes between a waiter's giving up the lock and its sleep is kept by the semaphore.

typedef struct ThreadsSignal
{
	HANDLE sem;
	LONG waiting;
} ThreadsSignal_t;

static void* ThreadsStart(void* user, void (*fn)(void* arg), void* arg)
{
	(void)user;
	return OS_ThreadStart(fn, arg, OS_PRIO_NORMAL);
}

static void ThreadsJoin(void* user, void* thread)
{
	(void)user;
	OS_ThreadJoin((OsThread_t*)thread);
}

static void* ThreadsLockNew(void* user)
{
	CRITICAL_SECTION* cs = (CRITICAL_SECTION*)malloc(sizeof *cs);
	(void)user;
	if(cs)
		InitializeCriticalSection(cs);
	return cs;
}

static void ThreadsLockFree(void* user, void* lock)
{
	(void)user;
	if(!lock)
		return;
	DeleteCriticalSection((CRITICAL_SECTION*)lock);
	free(lock);
}

static void ThreadsLock(void* user, void* lock)
{
	(void)user;
	EnterCriticalSection((CRITICAL_SECTION*)lock);
}

static void ThreadsUnlock(void* user, void* lock)
{
	(void)user;
	LeaveCriticalSection((CRITICAL_SECTION*)lock);
}

static void* ThreadsSignalNew(void* user)
{
	ThreadsSignal_t* s = (ThreadsSignal_t*)calloc(1, sizeof *s);
	(void)user;
	if(!s)
		return NULL;
	s->sem = CreateSemaphoreW(NULL, 0, 0x7fffffff, NULL);
	if(!s->sem)
	{
		free(s);
		return NULL;
	}
	return s;
}

static void ThreadsSignalFree(void* user, void* signal)
{
	ThreadsSignal_t* s = (ThreadsSignal_t*)signal;
	(void)user;
	if(!s)
		return;
	CloseHandle(s->sem);
	free(s);
}

static void ThreadsWait(void* user, void* signal, void* lock)
{
	ThreadsSignal_t* s = (ThreadsSignal_t*)signal;
	(void)user;
	s->waiting++;
	LeaveCriticalSection((CRITICAL_SECTION*)lock);
	WaitForSingleObject(s->sem, INFINITE);
	EnterCriticalSection((CRITICAL_SECTION*)lock);
}

static void ThreadsWake(void* user, void* signal, int all)
{
	ThreadsSignal_t* s = (ThreadsSignal_t*)signal;
	LONG n = all ? s->waiting : s->waiting > 0;
	(void)user;
	if(n <= 0)
		return;
	s->waiting -= n;
	ReleaseSemaphore(s->sem, n, NULL);
}

const OpenBGI_Threads_t* OS_Threads(void)
{
	static const OpenBGI_Threads_t table = {sizeof(OpenBGI_Threads_t), ThreadsStart, ThreadsJoin, ThreadsLockNew, ThreadsLockFree, ThreadsLock, ThreadsUnlock,
		ThreadsSignalNew, ThreadsSignalFree, ThreadsWait, ThreadsWake};
	return &table;
}

int OS_CpuCount(void)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return (int)si.dwNumberOfProcessors;
}
