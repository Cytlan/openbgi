/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sysutil.c - the clocks, the CRT random generator and the scrambler's
 *             engine-wide state (src/include/bgi/common.h for the random
 *             functions, src/include/bgi/codec.h for the scrambler's,
 *             src/include/bgi/sys.h for the rest)
 */
#include "bgi/sys.h"
#include "bgi/codec.h"
#include "bgi/display.h"
#include "bgi/core.h"

// ---- the clocks -----------------------------------------------------------

/* Every clock the engine reads is the host's (openbgi.h).  The original
 * queries timeGetDevCaps and, when the minimum period is below 10 ms,
 * calls timeBeginPeriod(min) and switches GetTicks to timeGetTime; the
 * timer resolution is the host's business, and its millisecond clock
 * makes the switch implicit. */

// the host's millisecond clock, wrapping at 2^32; any thread
uint32_t Clock_HostMs(OpenBGI_t* o)
{
	return o->host.ticksMs(o->host.user);
}

// the host's nanosecond counter; 0 when it has none
int Clock_PerfNs(OpenBGI_t* o, int64_t* out)
{
	return o->host.perfNs(o->host.user, out);
}

// the local date and time
void Clock_LocalTime(OpenBGI_t* o, OpenBGI_DateTime_t* out)
{
	o->host.localTime(o->host.user, out);
}

/* The engine's clock: milliseconds, wrapping at 2^32.  Up to 1.529 it is
 * the system clock.  From 1.535 on it can be stopped (Clock_Stop /
 * Clock_Resume): while stopped it reads the value it had when it stopped,
 * and the stopped time is taken out of it afterwards.  From 1.573 on it
 * does not see jumps either: a reading below the previous one, or more
 * than the step limit ("81 04", 500 ms by default) above it, moves the
 * base by the difference, so the clock starts at 0 on its first reading
 * and stands still across a stall such as a long load. */
uint32_t GetTicks(OpenBGI_t* o)
{
	uint32_t now = Clock_HostMs(o);
	if(o->engine.profile->gen < GEN_1_535)
		return now;
	if(o->engine.profile->gen >= GEN_1_573)
	{
		if(now < o->util.clockLast || o->util.clockLast + o->util.clockMaxStep < now)
			o->util.clockBase += now - o->util.clockLast;
		o->util.clockLast = now;
	}
	return o->util.clockFrozen ? o->util.clockFrozen : now - o->util.clockBase;
}

/* Stop the clock (the window lost the focus, or a message box or a
 * dialog is up).  `force` (1.547 on, the boxes and dialogs) stops it
 * whatever "81 68" says; the deactivation stops it only when "81 68" asked
 * for that.  From 1.547 on a second stop before the resume changes
 * nothing; from 1.588 on nothing happens before the window exists.  1 when
 * the clock was stopped here. */
int Clock_Stop(OpenBGI_t* o, int force)
{
	int stopped = 0;
	if(o->engine.profile->gen < GEN_1_535)
		return 0;
	if(o->engine.profile->gen < GEN_1_547)
	{
		if(o->util.clockStopInactive && !o->util.clockFrozen)
		{
			o->util.clockFrozen = GetTicks(o);
			return 1;
		}
		return 0;
	}
	if((o->engine.profile->gen >= GEN_1_588 && !o->display.windowAlive) || o->util.clockStopped)
		return 0;
	if((o->util.clockStopInactive || force) && !o->util.clockFrozen)
	{
		o->util.clockFrozen = GetTicks(o);
		stopped = 1;
	}
	o->util.clockStopped = 1;
	return stopped;
}

/* Let the clock run on, the stopped time taken out of it (1.535: only
 * while "81 68" is set).  1 when it had been stopped. */
int Clock_Resume(OpenBGI_t* o)
{
	uint32_t at = o->util.clockFrozen;
	if(o->engine.profile->gen < GEN_1_535)
		return 0;
	if(o->engine.profile->gen < GEN_1_547)
	{
		if(!o->util.clockStopInactive || !at)
			return 0;
	}
	else
	{
		if((o->engine.profile->gen >= GEN_1_588 && !o->display.windowAlive) || !o->util.clockStopped)
			return 0;
		o->util.clockStopped = 0;
		if(!at)
			return 0;
	}
	o->util.clockFrozen = 0;
	o->util.clockBase += GetTicks(o) - at;
	return 1;
}

// 1.547 on: a message box or a dialog is up - the clock stops whatever "81 68" says, and runs on after it
void Clock_BoxBegin(OpenBGI_t* o)
{
	if(o->engine.profile->gen >= GEN_1_547)
		Clock_Stop(o, 1);
}

void Clock_BoxEnd(OpenBGI_t* o)
{
	if(o->engine.profile->gen >= GEN_1_547)
		Clock_Resume(o);
}

// "81 68" (1.535 on): stop the clock (and from 1.547 on the sound) while the window is inactive; the previous setting
int Clock_SetStopWhenInactive(OpenBGI_t* o, int on)
{
	int prev = o->util.clockStopInactive;
	o->util.clockStopInactive = on;
	return prev;
}

int Clock_StopsWhenInactive(OpenBGI_t* o)
{
	return o->util.clockStopInactive;
}

// "81 04" (1.616 on): the step limit, 50 .. 60000 ms; 1 when accepted
int Clock_SetMaxStep(OpenBGI_t* o, uint32_t ms)
{
	if(ms - 50 > 60000 - 50)
		return 0;
	o->util.clockMaxStep = ms;
	return 1;
}

// ---- the CRT random generator -----------------------------------------------------------

// "80 00": srand
void BGI_Srand(OpenBGI_t* o, uint32_t seed)
{
	o->util.randSeed = seed;
}

// "80 01" / "80 02": rand, the MSVC sequence (seed * 214013 + 2531011, bits 16 .. 30)
int BGI_Rand(OpenBGI_t* o)
{
	o->util.randSeed = o->util.randSeed * 214013u + 2531011u;
	return (int)((o->util.randSeed >> 16) & 0x7fffu);
}

// ---- the scrambler's state ---------------------------------------------------

// the generator of src/base/hash.c on the engine's own state: seed it, then take one output a byte
void BGI_HashSeed(OpenBGI_t* o, uint32_t key)
{
	o->util.hashKey = key;
}

uint32_t BGI_HashNext(OpenBGI_t* o)
{
	return BGI_HashUpdate(&o->util.hashKey);
}
