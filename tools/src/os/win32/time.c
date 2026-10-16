/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * time.c - clocks and sleeping for the Win32 back end (inc/bgi/os.h):
 *          timeGetTime, the multimedia timer period, the local time and
 *          the idle sleeps of the main loop
 *
 * The idle sleep is the original's: a waitable timer set to half a
 * millisecond and waited for (at most 10 ms) on NT, Sleep(1) on Windows
 * 9x where CreateWaitableTimer does not exist.  OS_IdleInit decides which
 * once, from the platform id.
 */
#include "sys_internal.h"

static UINT gTimerPeriod;   // the period passed to timeBeginPeriod (ms)
static int gTimerPeriodSet; // timeBeginPeriod is in effect: timeEndPeriod must undo it
static HANDLE gIdleTimer;   // the waitable timer of the idle sleeps; NULL on Windows 9x (Sleep(1) instead)

uint32_t OS_TicksMs(void)
{
	return timeGetTime();
}

void OS_SleepMs(uint32_t ms)
{
	Sleep(ms);
}

// timeBeginPeriod(the device's minimum period) when that minimum is below 10 ms; nothing otherwise
void OS_TimerBeginPeriod(void)
{
	TIMECAPS tc;
	timeGetDevCaps(&tc, sizeof tc);
	gTimerPeriod = tc.wPeriodMin;
	if(gTimerPeriod < 10)
	{
		timeBeginPeriod(gTimerPeriod);
		gTimerPeriodSet = 1;
	}
}

void OS_TimerEndPeriod(void)
{
	if(gTimerPeriodSet)
	{
		timeEndPeriod(gTimerPeriod);
		gTimerPeriodSet = 0;
	}
}

void OS_LocalTime(OsLocalTime_t* t) // GetLocalTime ("80 0C")
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	t->year = st.wYear;
	t->month = st.wMonth;
	t->dayOfWeek = st.wDayOfWeek;
	t->day = st.wDay;
	t->hour = st.wHour;
	t->minute = st.wMinute;
	t->second = st.wSecond;
	t->millis = st.wMilliseconds;
}

// a manual-reset waitable timer on NT; on Windows 9x the idle sleeps fall back to Sleep(1)
void OS_IdleInit(void)
{
	OSVERSIONINFOW vi;
	vi.dwOSVersionInfoSize = sizeof vi;
	GetVersionExW(&vi);
	if(vi.dwPlatformId == VER_PLATFORM_WIN32_NT)
		gIdleTimer = CreateWaitableTimerW(NULL, TRUE, NULL);
}

void OS_IdleShutdown(void)
{
	if(gIdleTimer)
		CloseHandle(gIdleTimer);
	gIdleTimer = NULL;
}

// the timer set 0.5 ms ahead and waited for (at most 10 ms); Sleep(1) without a timer
void OS_IdleSleep(void)
{
	if(gIdleTimer)
	{
		LARGE_INTEGER due;
		due.QuadPart = -5000; // 100 ns units, relative
		SetWaitableTimer(gIdleTimer, &due, 0, NULL, NULL, FALSE);
		WaitForSingleObject(gIdleTimer, 10);
	}
	else
		Sleep(1);
}

/* The 1.535+ variant: ms = 1 is the same half-millisecond wait as
 * OS_IdleSleep, any other value sets the timer that many ms ahead but
 * waits at most 10 ms for it (WaitForSingleObjectEx, alertable).  Without
 * a timer, or for ms = 0, a plain Sleep(ms). */
void OS_IdleSleepMs(int ms)
{
	if(gIdleTimer && ms != 0)
	{
		LARGE_INTEGER due;
		due.QuadPart = ms == 1 ? -5000 : -(LONGLONG)ms * 10000;
		SetWaitableTimer(gIdleTimer, &due, 0, NULL, NULL, FALSE);
		WaitForSingleObjectEx(gIdleTimer, 10, TRUE);
	}
	else
		Sleep((DWORD)ms);
}
