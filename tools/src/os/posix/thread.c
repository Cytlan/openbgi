/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * thread.c - threads, locks and the atomic counters of the POSIX back end
 *            (inc/bgi/os.h), on pthreads and the GCC builtins
 */
#include "posix_internal.h"

struct OsLock
{
	pthread_mutex_t m; // recursive
};

// a recursive mutex, since a CRITICAL_SECTION may be entered again by the thread that holds it
OsLock_t* OS_LockCreate(void)
{
	OsLock_t* l = (OsLock_t*)malloc(sizeof *l);
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); // CRITICAL_SECTIONs are recursive
	pthread_mutex_init(&l->m, &a);
	pthread_mutexattr_destroy(&a);
	return l;
}

void OS_LockDestroy(OsLock_t* l)
{
	if(!l)
		return;
	pthread_mutex_destroy(&l->m);
	free(l);
}

void OS_LockEnter(OsLock_t* l)
{
	pthread_mutex_lock(&l->m);
}

void OS_LockLeave(OsLock_t* l)
{
	pthread_mutex_unlock(&l->m);
}

struct OsThread
{
	pthread_t t;     // the thread
	OsThreadFn_t fn; // what it runs, with `arg`
	void* arg;
};

// the pthread entry: run the engine's function and end
static void* ThreadMain(void* p)
{
	OsThread_t* t = (OsThread_t*)p;
	t->fn(t->arg);
	return NULL;
}

// a joinable thread at the normal priority; NULL when pthread_create fails
OsThread_t* OS_ThreadStart(OsThreadFn_t fn, void* arg, int priority)
{
	OsThread_t* t = (OsThread_t*)calloc(1, sizeof *t);
	t->fn = fn;
	t->arg = arg;
	BGI_UNUSED(priority); // raising priorities needs privileges on Linux; left at normal
	if(pthread_create(&t->t, NULL, ThreadMain, t) != 0)
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

// InterlockedIncrement / InterlockedDecrement: the new value, with a full barrier
int32_t OS_AtomicInc(volatile int32_t* p)
{
	return __sync_add_and_fetch(p, 1);
}

int32_t OS_AtomicDec(volatile int32_t* p)
{
	return __sync_sub_and_fetch(p, 1);
}
