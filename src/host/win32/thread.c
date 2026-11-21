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

int OS_CpuCount(void)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return (int)si.dwNumberOfProcessors;
}
