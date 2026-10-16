/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * time.c - clocks and sleeping for the POSIX back end (inc/bgi/os.h): the
 *          millisecond tick, the local time, and the idle sleeps of the
 *          main loop (the high-resolution counter is in machine.c)
 *
 * The timer resolution calls are empty: nanosleep is as fine as the
 * kernel allows without them.
 */
#include "posix_internal.h"

// the monotonic clock in milliseconds (wraps as timeGetTime does)
uint32_t OS_TicksMs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

// nanosleep; a signal cuts it short
void OS_SleepMs(uint32_t ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

void OS_TimerBeginPeriod(void)
{
}

void OS_TimerEndPeriod(void)
{
}

// GetLocalTime from gettimeofday and localtime_r
void OS_LocalTime(OsLocalTime_t* t)
{
	struct timeval tv;
	struct tm tm;
	gettimeofday(&tv, NULL);
	localtime_r(&tv.tv_sec, &tm);
	t->year = (uint16_t)(tm.tm_year + 1900);
	t->month = (uint16_t)(tm.tm_mon + 1);
	t->dayOfWeek = (uint16_t)tm.tm_wday;
	t->day = (uint16_t)tm.tm_mday;
	t->hour = (uint16_t)tm.tm_hour;
	t->minute = (uint16_t)tm.tm_min;
	t->second = (uint16_t)tm.tm_sec;
	t->millis = (uint16_t)(tv.tv_usec / 1000);
}

// nothing to create for the idle sleep: it is a plain nanosleep
void OS_IdleInit(void)
{
}

void OS_IdleShutdown(void)
{
}

// the original waits 0.5 ms on a waitable timer; a nanosleep of the same length here
void OS_IdleSleep(void)
{
	struct timespec ts = {0, 500000L};
	nanosleep(&ts, NULL);
}

// 1 ms = the half millisecond of OS_IdleSleep; longer waits are capped at 10 ms like the original's timer wait
void OS_IdleSleepMs(int ms)
{
	struct timespec ts;
	if(ms <= 0)
		return;
	if(ms == 1)
	{
		OS_IdleSleep();
		return;
	}
	if(ms > 10)
		ms = 10;
	ts.tv_sec = 0;
	ts.tv_nsec = (long)ms * 1000000L;
	nanosleep(&ts, NULL);
}
