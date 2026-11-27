/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * async.h - work that runs beside the engine's thread: a job is started,
 *           asked about, and finished (src/core/async.c)
 *
 * The original has a loader thread: a program asks for a file, waits on a
 * wait object, and the machine goes on meanwhile.  Here the read stays on
 * the engine's thread - the host's file functions are only ever called
 * there - and what follows it, the unpacking, is such a job: it touches
 * nothing but the buffers it was given.
 *
 * The threads are the host's (OpenBGI_Host_t.threads), two of them,
 * started with the first job and kept until the engine is deleted.  A
 * host without threads runs each job at once, on the engine's thread:
 * the owner sees no difference but the time.
 */
#ifndef BGI_ASYNC_H_
#define BGI_ASYNC_H_

#include "openbgi.h"

typedef void (*AsyncFn_t)(void* arg);
typedef struct AsyncJob AsyncJob_t;
typedef struct Async Async_t; // the helpers and their queue (o->async; made by the first job)

/* fn(arg) on a helper thread, or - without threads - before this returns.
 * The job belongs to the caller, who finishes it.  NULL: no memory. */
AsyncJob_t* Async_Start(OpenBGI_t* o, AsyncFn_t fn, void* arg);
int Async_Done(OpenBGI_t* o, AsyncJob_t* j);    // 1 once fn has returned
void Async_Finish(OpenBGI_t* o, AsyncJob_t* j); // wait until it has, and free the job; NULL is nothing
void Async_Free(OpenBGI_t* o);                  // the helpers end (every job was finished): the engine is deleted

#endif // BGI_ASYNC_H_
