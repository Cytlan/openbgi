/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * thread.c - the threads and the processor count of the POSIX back end
 *            (src/host/os.h), on pthreads
 */
#include "posix_internal.h"

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
long syscall(long number, ...); // (unistd.h shows it only with the system's extensions asked for)
#endif

#define THREAD_STACK (8u << 20) // as the first thread's usually is: an engine may run on any (os_preview.c)

struct OsThread
{
	pthread_t t;     // the thread
	OsThreadFn_t fn; // what it runs, with `arg`
	void* arg;
	int low; // OS_PRIO_LOW
};

// the pthread entry: run the engine's function and end
static void* ThreadMain(void* p)
{
	OsThread_t* t = (OsThread_t*)p;
#if defined(__linux__)
	if(t->low) // (Linux keeps a nice value for each thread; it may always be raised)
		setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
	t->fn(t->arg);
	return NULL;
}

/* A joinable thread with a stack of THREAD_STACK; NULL when
 * pthread_create fails.  Raising a priority needs privileges on Linux:
 * OS_PRIO_HIGHEST is left at normal.  OS_PRIO_LOW steps back on Linux
 * and is normal elsewhere. */
OsThread_t* OS_ThreadStart(OsThreadFn_t fn, void* arg, int priority)
{
	OsThread_t* t = (OsThread_t*)calloc(1, sizeof *t);
	pthread_attr_t attr;
	int made;
	t->fn = fn;
	t->arg = arg;
	t->low = priority == OS_PRIO_LOW;
	if(pthread_attr_init(&attr) != 0)
		made = pthread_create(&t->t, NULL, ThreadMain, t) == 0;
	else
	{
		pthread_attr_setstacksize(&attr, THREAD_STACK); // (refused: the system's size)
		made = pthread_create(&t->t, &attr, ThreadMain, t) == 0;
		pthread_attr_destroy(&attr);
	}
	if(!made)
	{
		free(t);
		return NULL;
	}
	return t;
}

void OS_ThreadJoin(OsThread_t* t)
{
	if(!t)
		return;
	pthread_join(t->t, NULL);
	free(t);
}

// the processors online, at least 1
int OS_CpuCount(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return n > 0 ? (int)n : 1;
}
