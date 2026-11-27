/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * async.c - jobs beside the engine's thread; interface in bgi/async.h
 *
 * The state lives in a block of its own, not in the engine's struct: the
 * helpers know only that block, so an engine that starts again (and
 * clears its struct) takes nothing from under them.  The host's table is
 * copied into it for the same reason.
 */
#include "bgi/async.h"
#include "bgi/core.h"

enum
{
	ASYNC_THREADS = 2
};

struct AsyncJob
{
	AsyncFn_t fn;
	void* arg;
	int done; // fn has returned (under the lock)
	AsyncJob_t* next;
};

struct Async
{
	OpenBGI_Threads_t t; // the host's functions, and what it wants handed back
	void* user;
	void* lock;
	void* wake; // a job was queued, or the helpers are to end
	void* done; // a job is done
	void* thread[ASYNC_THREADS];
	int threads;
	AsyncJob_t *first, *last; // the jobs no helper has taken yet
	int quit;
};

static void Helper(void* arg)
{
	Async_t* a = (Async_t*)arg;
	a->t.lock(a->user, a->lock);
	for(;;)
	{
		AsyncJob_t* j;
		while(!a->quit && !a->first)
			a->t.wait(a->user, a->wake, a->lock);
		if(a->quit)
			break;
		j = a->first;
		a->first = j->next;
		if(!a->first)
			a->last = NULL;
		a->t.unlock(a->user, a->lock);
		j->fn(j->arg);
		a->t.lock(a->user, a->lock);
		j->done = 1;
		a->t.wake(a->user, a->done, 1);
	}
	a->t.unlock(a->user, a->lock);
}

// the helpers, at the first job; NULL where the host has no threads or none could be made
static Async_t* Helpers(OpenBGI_t* o)
{
	const OpenBGI_Threads_t* t = o->host.threads;
	Async_t* a;
	if(o->async)
		return o->async->threads ? o->async : NULL;
	a = (Async_t*)BGI_Calloc(sizeof *a);
	if(!a)
		return NULL;
	o->async = a; // (kept even when it has no threads: nothing is tried a second time)
	if(!t || !t->start || !t->join || !t->lockNew || !t->lockFree || !t->lock || !t->unlock || !t->signalNew || !t->signalFree || !t->wait || !t->wake)
		return NULL;
	a->t = *t;
	a->user = o->host.user;
	a->lock = t->lockNew(a->user);
	a->wake = t->signalNew(a->user);
	a->done = t->signalNew(a->user);
	if(!a->lock || !a->wake || !a->done)
		return NULL;
	while(a->threads < ASYNC_THREADS)
	{
		void* h = t->start(a->user, Helper, a);
		if(!h)
			break;
		a->thread[a->threads++] = h;
	}
	return a->threads ? a : NULL;
}

AsyncJob_t* Async_Start(OpenBGI_t* o, AsyncFn_t fn, void* arg)
{
	AsyncJob_t* j = (AsyncJob_t*)BGI_Calloc(sizeof *j);
	Async_t* a;
	if(!j)
		return NULL;
	j->fn = fn;
	j->arg = arg;
	a = Helpers(o);
	if(!a)
	{
		fn(arg);
		j->done = 1;
		return j;
	}
	a->t.lock(a->user, a->lock);
	if(a->last)
		a->last->next = j;
	else
		a->first = j;
	a->last = j;
	a->t.wake(a->user, a->wake, 0);
	a->t.unlock(a->user, a->lock);
	return j;
}

int Async_Done(OpenBGI_t* o, AsyncJob_t* j)
{
	Async_t* a = o->async;
	int done;
	if(!j)
		return 1;
	if(!a || !a->threads)
		return j->done;
	a->t.lock(a->user, a->lock);
	done = j->done;
	a->t.unlock(a->user, a->lock);
	return done;
}

void Async_Finish(OpenBGI_t* o, AsyncJob_t* j)
{
	Async_t* a = o->async;
	if(!j)
		return;
	if(a && a->threads)
	{
		Perf_PartBegin(o, PERF_PART_JOB);
		a->t.lock(a->user, a->lock);
		while(!j->done)
			a->t.wait(a->user, a->done, a->lock);
		a->t.unlock(a->user, a->lock);
		Perf_PartEnd(o, PERF_PART_JOB);
	}
	BGI_Free(j);
}

void Async_Free(OpenBGI_t* o)
{
	Async_t* a = o->async;
	int i;
	if(!a)
		return;
	o->async = NULL;
	if(a->threads)
	{
		a->t.lock(a->user, a->lock);
		a->quit = 1;
		a->t.wake(a->user, a->wake, 1);
		a->t.unlock(a->user, a->lock);
		for(i = 0; i < a->threads; i++)
			a->t.join(a->user, a->thread[i]);
	}
	if(a->lock)
		a->t.lockFree(a->user, a->lock);
	if(a->wake)
		a->t.signalFree(a->user, a->wake);
	if(a->done)
		a->t.signalFree(a->user, a->done);
	BGI_Free(a);
}
