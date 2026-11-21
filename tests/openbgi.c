/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/openbgi.c - unit test of the engine struct and the host's side of
 *                   it (src/core/openbgi.c, src/include/bgi/core.h,
 *                   include/openbgi.h); built as test_openbgi by `make test`
 *
 * The groups of tests, in the order they run:
 *
 *   start - a new engine starts with the original's starting values and
 *           zero everywhere else
 *   two   - two engines side by side: what one is told through the
 *           engine's functions the other does not see, and each keeps its
 *           own while the other runs
 *   fresh - an engine made after one was freed starts from nothing
 *   host  - the engine's clocks are the host's: the scripts' clock
 *           follows a host clock the test moves (from 1.573 on starting
 *           at 0 and standing still across a jump), the date is the
 *           host's; no engine without a host; an engine that was not
 *           started has no frame to run and no picture
 */
#include "bgi/common.h"
#include "bgi/core.h"
#include "host/os.h"
#include "bgi/strutil.h"
#include "bgi/sysobj/flags.h"
#include "bgi/sys.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

static void TestStart(void)
{
	OpenBGI_t* o = OpenBGI_New(OS_Host());
	CHECK(o != NULL);
	if(!o)
		return;
	CHECK(o->engine.profile != NULL && o->engine.profile->gen == GEN_1_69_444);
	CHECK(strcmp(o->files.bootArc, "system.arc") == 0);
	CHECK(o->wait.waitBlocking == 1);
	CHECK(o->input.stdKeys[0][0] == 0x01 && o->input.skipKeys[0] == 0x11);
	CHECK(o->text.linkColour == -1 && o->text.textFunc[0] == 1);
	CHECK(o->vm.dbgInsnLimit == 0x100000);
	CHECK(o->enc.textUtf8 == 0 && o->files.baseDir[0] == 0 && o->sysobj.flagMgr == NULL);
	OpenBGI_Delete(o);
	OpenBGI_Delete(NULL);
}

static void TestTwo(void)
{
	OpenBGI_t* a = OpenBGI_New(OS_Host());
	OpenBGI_t* b = OpenBGI_New(OS_Host());
	uint32_t code, ca, cb, bit = 0;
	const char* hiragana = "\xe3\x81\x82"; // あ in UTF-8; in Shift-JIS a double-byte character and a stray byte
	CHECK(a != NULL && b != NULL && a != b);
	if(!a || !b)
		return;

	// a: UTF-8 text, a base directory, a flag group with bit 3 set
	a->enc.textUtf8 = 1;
	strcpy(a->files.baseDir, "game_a\\");
	a->sysobj.flagMgr = FlagMgr_New(16);
	CHECK(GFlag_RegisterGroup(a, "route", 16) == 1);
	CHECK(GFlag_SetBit(a, "route", 3, 1) == FLAG_OK);

	// b: none of it
	CHECK(b->enc.textUtf8 == 0);
	CHECK(b->files.baseDir[0] == 0);
	CHECK(b->sysobj.flagMgr == NULL);
	b->sysobj.flagMgr = FlagMgr_New(16);
	CHECK(GFlag_GetBit(b, &bit, "route", 3) == FLAG_ERR_UNKNOWN);
	CHECK(GFlag_RegisterGroup(b, "route", 16) == 1);
	CHECK(GFlag_GetBit(b, &bit, "route", 3) == FLAG_OK && bit == 0);
	strcpy(b->files.baseDir, "game_b\\");

	// the text functions follow each engine's encoding, and each keeps its own kinsoku table
	CHECK(TextGetChar(b, &code, hiragana) == 2); // Shift-JIS in b
	TextGetChar(b, &cb, "\x81\x41");             // 、in Shift-JIS
	CHECK(IsKinsokuChar(b, cb));
	CHECK(TextGetChar(a, &code, hiragana) == 3);
	TextGetChar(a, &ca, "\xe3\x80\x81"); // 、in UTF-8
	CHECK(IsKinsokuChar(a, ca));
	CHECK(IsKinsokuChar(b, cb)); // b's table was not rebuilt for a's encoding
	CHECK(IsKinsokuChar(a, ca));

	// a kept what it had
	CHECK(strcmp(a->files.baseDir, "game_a\\") == 0);
	CHECK(GFlag_GetBit(a, &bit, "route", 3) == FLAG_OK && bit != 0);
	CHECK(strcmp(b->files.baseDir, "game_b\\") == 0);

	FlagMgr_Delete(a->sysobj.flagMgr);
	FlagMgr_Delete(b->sysobj.flagMgr);
	OpenBGI_Delete(a);
	OpenBGI_Delete(b);
}

static void TestFresh(void)
{
	OpenBGI_t* o = OpenBGI_New(OS_Host());
	if(!o)
		return;
	o->enc.textUtf8 = 1;
	strcpy(o->files.baseDir, "old\\");
	OpenBGI_Delete(o);
	o = OpenBGI_New(OS_Host());
	CHECK(o != NULL);
	if(!o)
		return;
	CHECK(o->enc.textUtf8 == 0);
	CHECK(o->files.baseDir[0] == 0);
	OpenBGI_Delete(o);
}

// the test's host: a clock the test sets, a fixed date, a counter that is missing
typedef struct TestClock
{
	uint32_t ms;
	int calls; // ticksMs calls
} TestClock_t;

static uint32_t TestTicksMs(void* user)
{
	TestClock_t* c = (TestClock_t*)user;
	c->calls++;
	return c->ms;
}

static int TestPerfNs(void* user, int64_t* out)
{
	BGI_UNUSED(user);
	BGI_UNUSED(out);
	return 0;
}

static void TestLocalTime(void* user, OpenBGI_DateTime_t* out)
{
	BGI_UNUSED(user);
	memset(out, 0, sizeof *out);
	out->year = 2026;
	out->month = 10;
	out->day = 23;
}

static void TestHost(void)
{
	TestClock_t clock = {1000, 0};
	OpenBGI_Host_t host = {.size = sizeof(OpenBGI_Host_t), .api = OPENBGI_API_VERSION, .user = &clock, .ticksMs = TestTicksMs, .perfNs = TestPerfNs, .localTime = TestLocalTime, .caps = OPENBGI_CAP_DIALOGS}; // (no fonts, no files)
	OpenBGI_Host_t partial = host;
	OpenBGI_DateTime_t dt;
	OpenBGI_Frame_t f = {.size = sizeof f};
	int64_t ns;
	OpenBGI_t* o;

	CHECK(OpenBGI_New(NULL) == NULL);
	partial.localTime = NULL;
	CHECK(OpenBGI_New(&partial) == NULL);

	o = OpenBGI_New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	// the reference build (1.69 build 444) reads the host's clock as it is
	CHECK(GetTicks(o) == 1000);
	clock.ms = 1100;
	CHECK(GetTicks(o) == 1100);
	// from 1.573 on the scripts' clock starts at 0 and stands still across a jump of more than 500 ms
	o->engine.profile = Engine_ProfileOfGen(GEN_1_573);
	clock.ms = 2000;
	CHECK(GetTicks(o) == 0);
	clock.ms = 2100;
	CHECK(GetTicks(o) == 100);
	clock.ms = 9000;
	CHECK(GetTicks(o) == 100);
	clock.ms = 9040;
	CHECK(GetTicks(o) == 140);
	CHECK(clock.calls == 6);
	CHECK(Clock_HostMs(o) == 9040);
	CHECK(Clock_PerfNs(o, &ns) == 0);
	Clock_LocalTime(o, &dt);
	CHECK(dt.year == 2026 && dt.month == 10 && dt.day == 23);

	// not started: no frame, no picture, nothing to stop
	f.presented = 7;
	f.idleMs = 7;
	f.changes = 7;
	CHECK(OpenBGI_RunFrame(o, &f) == OPENBGI_DONE);
	CHECK(f.presented == 0 && f.idleMs == 0);
	CHECK(f.pixels == NULL && f.changes == 0);
	CHECK(OpenBGI_Stop(o) == 0);
	OpenBGI_Delete(o);
}

int main(void)
{
	printf("start\n");
	TestStart();
	printf("two\n");
	TestTwo();
	printf("fresh\n");
	TestFresh();
	printf("host\n");
	TestHost();
	if(gFails)
	{
		printf("openbgi: %d checks failed\n", gFails);
		return 1;
	}
	printf("openbgi: all tests passed\n");
	return 0;
}
