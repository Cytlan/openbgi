/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sysutil.c - allocation helpers, the engine lock, the tick source, the
 *             idle sleep and the CRT random generator (inc/bgi/common.h
 *             for the allocation and random functions, inc/bgi/sys.h for
 *             the rest)
 */
#include "bgi/sys.h"
#include "bgi/os.h"

// ---- allocation ----------------------------------------------------------

// malloc that never returns NULL: the process is aborted when memory runs out (n = 0 gives a 1-byte block)
void* BGI_Alloc(size_t n)
{
	void* p = malloc(n ? n : 1);
	if(!p)
	{
		fprintf(stderr, "bgi: out of memory (%lu bytes)\n", (unsigned long)n);
		abort();
	}
	return p;
}

void* BGI_Calloc(size_t n)
{
	void* p = BGI_Alloc(n);
	memset(p, 0, n);
	return p;
}

void BGI_Free(void* p)
{
	free(p);
}

char* BGI_Strdup(const char* s)
{
	size_t n = strlen(s) + 1;
	char* d = (char*)BGI_Alloc(n);
	memcpy(d, s, n);
	return d;
}

//  --- engine lock ------------------------

/* The critical section held around the hash generator the DSC, SDC and
 * save-file codecs share, which the original's loader thread uses too.
 * Enter / Leave are no-ops before Init and after Delete. */
static OsLock_t* gEngineLock;

void EngineLock_Init(void)
{
	gEngineLock = OS_LockCreate();
}

// destroy the lock after waiting for whoever holds it
void EngineLock_Delete(void)
{
	OS_LockEnter(gEngineLock);
	OS_LockLeave(gEngineLock);
	OS_LockDestroy(gEngineLock);
	gEngineLock = NULL;
}

void EngineLock_Enter(void)
{
	if(gEngineLock)
		OS_LockEnter(gEngineLock);
}

void EngineLock_Leave(void)
{
	if(gEngineLock)
		OS_LockLeave(gEngineLock);
}

// ---- ticks ----------------------------------------------------------------

/* The original queries timeGetDevCaps and, when the minimum period is
 * below 10 ms, calls timeBeginPeriod(min) and switches GetTicks to
 * timeGetTime.  The OS layer always provides a millisecond clock, so the
 * switch is implicit here. */
static int gTimerPeriodOn; // between Timer_Begin and Timer_End

// raise the timer resolution (start-up)
void Timer_Begin(void)
{
	OS_TimerBeginPeriod();
	gTimerPeriodOn = 1;
}

// restore it (shutdown)
void Timer_End(void)
{
	if(gTimerPeriodOn)
	{
		OS_TimerEndPeriod();
		gTimerPeriodOn = 0;
	}
}

// the engine's clock: milliseconds, wrapping at 2^32
uint32_t GetTicks(void)
{
	return OS_TicksMs();
}

// ---- idle sleep ------------------------------------------------------------

void Idle_Init(void)
{
	OS_IdleInit();
}

void Idle_Shutdown(void)
{
	OS_IdleShutdown();
}

// the ~0.5 ms sleep at the end of a pass that presented nothing (Display_FrameIdle)
void Sys_Idle(void)
{
	OS_IdleSleep();
}

// ---- the CRT random generator -----------------------------------------------------------
static uint32_t gRandSeed = 1; // the CRT's holdrand, initially 1

// "80 00": srand
void BGI_Srand(uint32_t seed)
{
	gRandSeed = seed;
}

// "80 01" / "80 02": rand, the MSVC sequence (seed * 214013 + 2531011, bits 16 .. 30)
int BGI_Rand(void)
{
	gRandSeed = gRandSeed * 214013u + 2531011u;
	return (int)((gRandSeed >> 16) & 0x7fffu);
}
