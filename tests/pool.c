/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/pool.c - the worker pool on the host's threads (src/gfx/compositor.c,
 *                OpenBGI_Host_t.threads); built as test_pool by `make test`
 *
 * A pool runs a queue of jobs on the caller's thread and on helpers the
 * host starts.  The test's jobs take a number from a queue under the
 * pool's lock and mark it: after a run every number is marked once,
 * whatever the count of threads and of jobs; a pool of a host without
 * threads, and one that is asked for one thread, runs everything on the
 * caller's.
 */
#include "bgi/gfx/compositor.h"
#include "bgi/async.h"
#include "bgi/core.h"
#include "host/os.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

enum
{
	JOBS = 4000
};

static WorkerPool_t* gPool;
static int gNext, gCount;
static unsigned char gDone[JOBS];

// one job: the next number, under the pool's lock; some work beside the lock
static int Job(OpenBGI_t* o, void* arg)
{
	int i, k, locked = WorkerPool_Lock(gPool);
	volatile unsigned work = 0;
	(void)o;
	(void)arg;
	i = gNext < gCount ? gNext++ : -1;
	WorkerPool_Unlock(gPool, locked);
	if(i < 0)
		return 0;
	for(k = 0; k < 200; k++)
		work += (unsigned)k;
	gDone[i]++;
	return 1;
}

// `count` jobs through the pool; 1 when each was done once
static int Run(OpenBGI_t* o, int count, int parallel)
{
	int i;
	gNext = 0;
	gCount = count;
	memset(gDone, 0, sizeof gDone);
	WorkerPool_SetCallback(gPool, Job, NULL);
	WorkerPool_Run(o, gPool, parallel, count);
	WorkerPool_SetCallback(gPool, NULL, NULL);
	for(i = 0; i < JOBS; i++)
		if(gDone[i] != (i < count))
			return 0;
	return gNext == count;
}

// ---- jobs beside the engine's thread (bgi/async.h) ----------------------------------------------

static int gSum[64];

static void Add(void* arg)
{
	int *p = (int*)arg, k;
	volatile unsigned work = 0;
	for(k = 0; k < 20000; k++)
		work += (unsigned)k;
	*p += 1;
}

// 64 jobs started, asked about and finished: each ran once; without threads each has run when Async_Start returns
static void Jobs(OpenBGI_t* o, int threads)
{
	AsyncJob_t* job[64];
	int i, early = 0;
	memset(gSum, 0, sizeof gSum);
	for(i = 0; i < 64; i++)
	{
		job[i] = Async_Start(o, Add, &gSum[i]);
		CHECK(job[i] != NULL);
		if(!threads)
			CHECK(Async_Done(o, job[i]) && gSum[i] == 1);
	}
	for(i = 0; i < 64; i++)
		early += Async_Done(o, job[i]);
	for(i = 63; i >= 0; i--)
	{ // (in another order than they were started)
		Async_Finish(o, job[i]);
		CHECK(gSum[i] == 1);
	}
	CHECK(early >= 0 && early <= 64);
	Async_Finish(o, NULL);
	CHECK(Async_Done(o, NULL));
}

int main(void)
{
	static OpenBGI_Host_t host;
	OpenBGI_t* o;
	int n, round;
	host = *OS_Host();
	o = OpenBGI_New(&host);
	if(!o)
	{
		printf("pool: no engine\n");
		return 1;
	}
	// the host's threads: as many as asked for, the caller's among them
	for(n = 1; n <= 8; n++)
	{
		gPool = WorkerPool_New(o, n);
		CHECK(gPool->threads == (host.threads ? n : 1));
		for(round = 0; round < 50; round++)
		{
			CHECK(Run(o, JOBS, 1));
			CHECK(Run(o, round % 7, 1)); // (few jobs: few helpers, or none)
			CHECK(Run(o, 100, 0));       // (not shared: the caller's thread alone)
		}
		WorkerPool_Delete(gPool);
	}
	// jobs: with the host's threads, and over an engine that starts again (the helpers are kept)
	Jobs(o, host.threads != NULL);
	Jobs(o, host.threads != NULL);
	// more than a pool takes
	gPool = WorkerPool_New(o, 1000);
	CHECK(gPool->threads == (host.threads ? POOL_THREADS_MAX : 1) && Run(o, JOBS, 1));
	WorkerPool_Delete(gPool);
	// no engine, and a host without threads: the caller's thread alone
	gPool = WorkerPool_New(NULL, 8);
	CHECK(gPool->threads == 1 && Run(o, JOBS, 1));
	WorkerPool_Delete(gPool);
	OpenBGI_Delete(o);
	host.threads = NULL;
	o = OpenBGI_New(&host);
	if(o)
	{
		gPool = WorkerPool_New(o, 8);
		CHECK(gPool->threads == 1 && Run(o, JOBS, 1));
		WorkerPool_Delete(gPool);
		Jobs(o, 0);
		OpenBGI_Delete(o);
	}
	if(gFails)
	{
		printf("pool: %d checks failed\n", gFails);
		return 1;
	}
	printf("pool: all tests passed\n");
	return 0;
}
