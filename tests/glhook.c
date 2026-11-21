/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/glhook.c - unit test of the reader of shader files in mpv's
 *                  user-shader format and of the plan of their passes
 *                  (src/host/os_glhook.c); built as test_glhook by
 *                  `make test`
 *
 * The groups, in the order they run:
 *
 *   parse  - a file of three passes: the directives of each, CR LF line
 *            ends, the code between them; a directive that is not known
 *            takes its pass out; too many passes
 *   eval   - expressions in reverse Polish: numbers, sizes by name, the
 *            operators; what is malformed or names nothing
 *   plan   - which passes run for a picture and a place of some size, in
 *            which order (MAIN before PREKERNEL), into which textures: one
 *            is used again once nothing reads it, never while a pass reads
 *            it; a pass that binds what was never made is left out
 *   anime4k - the tree's Anime4K files (when the test is run from the
 *            tree): mode A of the high set for four sizes of the place
 */
#include "host/os_glhook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

static const char kSample[] = "// a licence\n"
							  "\n"
							  "//!DESC first\r\n"
							  "//!HOOK MAIN\r\n"
							  "//!BIND HOOKED\r\n"
							  "//!SAVE STATS\r\n"
							  "//!COMPONENTS 1\r\n"
							  "\r\n"
							  "vec4 hook() { return HOOKED_tex(HOOKED_pos); }\r\n"
							  "//!DESC second \n"
							  "//!HOOK MAIN\n"
							  "//!BIND MAIN\n"
							  "//!BIND STATS\n"
							  "//!WIDTH MAIN.w 2 *\n"
							  "//!HEIGHT MAIN.h 2 *\n"
							  "//!WHEN OUTPUT.w MAIN.w / 1.200 > OUTPUT.h MAIN.h / 1.200 > *\n"
							  "vec4 hook() { return MAIN_tex(MAIN_pos); }\n"
							  "//!HOOK PREKERNEL\n"
							  "//!BIND HOOKED\n"
							  "//!BIND STATS\n"
							  "vec4 hook() { return HOOKED_tex(HOOKED_pos); }\n";

static int Sizes(void* ctx, const char* name, int* w, int* h)
{
	(void)ctx;
	if(strcmp(name, "MAIN") == 0)
	{
		*w = 1280;
		*h = 720;
		return 1;
	}
	if(strcmp(name, "OUTPUT") == 0)
	{
		*w = 1920;
		*h = 1080;
		return 1;
	}
	return 0;
}

static void TestParse(void)
{
	OsGlPass_t p[4];
	int n;
	printf("parse\n");
	n = OsGlHook_Parse(kSample, sizeof kSample - 1, p, 4);
	CHECK(n == 3);
	if(n != 3)
		return;
	CHECK(strcmp(p[0].desc, "first") == 0 && p[0].hook == OS_GLHOOK_MAIN && p[0].binds == 1 && strcmp(p[0].bind[0], "HOOKED") == 0);
	CHECK(strcmp(p[0].save, "STATS") == 0 && p[0].components == 1 && !p[0].width[0] && !p[0].when[0]);
	CHECK(p[0].bodyLen > 20 && strstr(p[0].body, "vec4 hook()") != NULL && p[0].body + p[0].bodyLen <= p[1].body);
	CHECK(memchr(p[0].body, '!', p[0].bodyLen) == NULL); // (no directive in the code)
	CHECK(strcmp(p[1].desc, "second") == 0 && p[1].binds == 2 && strcmp(p[1].bind[1], "STATS") == 0 && !p[1].save[0]);
	CHECK(strcmp(p[1].width, "MAIN.w 2 *") == 0 && strcmp(p[1].height, "MAIN.h 2 *") == 0 && p[1].when[0] == 'O');
	CHECK(!p[2].desc[0] && p[2].hook == OS_GLHOOK_PREKERNEL && p[2].binds == 2);
	CHECK(p[2].body + p[2].bodyLen == kSample + sizeof kSample - 1);
	CHECK(OsGlHook_Parse(kSample, sizeof kSample - 1, p, 2) == -1); // more passes than room
	CHECK(OsGlHook_Parse("void f() {}\n", 12, p, 4) == 0);
	n = OsGlHook_Parse("//!HOOK LUMA\nx\n//!HOOK MAIN\n//!COMPUTE 1 1\nx\n", 44, p, 4);
	CHECK(n == 2 && p[0].hook == 0 && p[1].hook == 0); // another hook; a directive that is not known
}

static void TestEval(void)
{
	double v = -1;
	printf("eval\n");
	CHECK(OsGlHook_Eval("2 3 +", NULL, NULL, &v) && v == 5);
	CHECK(OsGlHook_Eval(" 7 2 - 3 * 2 / ", NULL, NULL, &v) && v == 7.5);
	CHECK(OsGlHook_Eval("7 4 %", NULL, NULL, &v) && v == 3);
	CHECK(OsGlHook_Eval("1 2 >", NULL, NULL, &v) && v == 0 && OsGlHook_Eval("1 2 <", NULL, NULL, &v) && v == 1);
	CHECK(OsGlHook_Eval("2 2 =", NULL, NULL, &v) && v == 1 && OsGlHook_Eval("2 2 = !", NULL, NULL, &v) && v == 0);
	CHECK(OsGlHook_Eval("MAIN.w 2 *", Sizes, NULL, &v) && v == 2560 && OsGlHook_Eval("MAIN.h", Sizes, NULL, &v) && v == 720);
	CHECK(OsGlHook_Eval("OUTPUT.w MAIN.w / 1.200 > OUTPUT.h MAIN.h / 1.200 > *", Sizes, NULL, &v) && v == 1);
	CHECK(OsGlHook_Eval("OUTPUT.w MAIN.w / 2.0 < OUTPUT.w MAIN.w / 1.2 > *", Sizes, NULL, &v) && v == 1);
	CHECK(!OsGlHook_Eval("NOPE.w", Sizes, NULL, &v) && !OsGlHook_Eval("MAIN.w", NULL, NULL, &v));
	CHECK(!OsGlHook_Eval("1 +", NULL, NULL, &v) && !OsGlHook_Eval("1 2", NULL, NULL, &v) && !OsGlHook_Eval("", NULL, NULL, &v));
	CHECK(!OsGlHook_Eval("1 x +", NULL, NULL, &v) && OsGlHook_Eval("1 0 /", NULL, NULL, &v) && v == 0);
}

// every step draws into a texture of its result's size that it does not read, and reads only textures there are
static int PlanSound(const OsGlPlan_t* plan)
{
	int i, k;
	for(i = 0; i < plan->steps; i++)
	{
		const OsGlStep_t* st = &plan->step[i];
		if(st->out < 1 || st->out >= plan->slots || plan->slot[st->out].w != st->w || plan->slot[st->out].h != st->h)
			return 0;
		for(k = 0; k < OS_GLHOOK_BINDS; k++)
			if(st->in[k] == st->out || st->in[k] >= plan->slots)
				return 0;
	}
	return plan->result >= 0 && plan->result < plan->slots;
}

static void TestPlan(void)
{
	OsGlPass_t p[4];
	const OsGlPass_t* files[2];
	int counts[2], n;
	OsGlPlan_t plan;
	printf("plan\n");
	n = OsGlHook_Parse(kSample, sizeof kSample - 1, p, 4);
	CHECK(n == 3);
	files[0] = files[1] = p;
	counts[0] = counts[1] = n;
	// shown one and a half times as large: all three passes, the PREKERNEL one last
	CHECK(OsGlHook_Plan(files, counts, 1, 1280, 720, 1920, 1080, &plan));
	CHECK(plan.steps == 3 && plan.step[0].pass == 0 && plan.step[1].pass == 1 && plan.step[2].pass == 2);
	CHECK(plan.step[0].w == 1280 && plan.step[1].w == 2560 && plan.step[1].h == 1440 && plan.step[2].w == 2560);
	CHECK(plan.step[0].in[0] == 0 && plan.step[0].in[1] == -1 && plan.step[1].in[0] == 0 && plan.step[1].in[1] == plan.step[0].out);
	CHECK(plan.step[2].in[0] == plan.step[1].out && plan.step[2].in[1] == plan.step[0].out && plan.result == plan.step[2].out);
	CHECK(plan.slots == 4 && plan.slot[0].w == 1280 && PlanSound(&plan));
	// shown as large as it is: the second pass does not run
	CHECK(OsGlHook_Plan(files, counts, 1, 1280, 720, 1280, 720, &plan));
	CHECK(plan.steps == 2 && plan.step[1].pass == 2 && plan.step[1].w == 1280 && plan.result == plan.step[1].out && PlanSound(&plan));
	// the file twice: every MAIN pass of both before the PREKERNEL passes; textures are used again
	CHECK(OsGlHook_Plan(files, counts, 2, 640, 480, 1280, 960, &plan));
	CHECK(plan.steps == 5 && plan.step[0].file == 0 && plan.step[2].file == 1 && plan.step[2].pass == 0);
	CHECK(plan.step[3].pass == 2 && plan.step[3].file == 0 && plan.step[4].file == 1 && PlanSound(&plan));
	CHECK(plan.step[1].w == 1280 && plan.step[2].w == 1280 && plan.slots <= 5); // (the second file's upscale does not run: shown is 1 : 1 by then)
	// a pass that binds what was never made is left out
	p[0].hook = 0;
	CHECK(OsGlHook_Plan(files, counts, 1, 1280, 720, 1920, 1080, &plan));
	CHECK(plan.steps == 0 && plan.result == 0 && plan.slots == 1);
	CHECK(!OsGlHook_Plan(files, counts, 1, 0, 720, 1920, 1080, &plan));
}

// ---- the tree's files ---------------------------------------------------------------------------

static char* gText[8];
static OsGlPass_t gPass[8][OS_GLHOOK_PASSES];

static int Load(int slot, const char* name)
{
	static const char* const kDirs[] = {"shaders/anime4k/", "../shaders/anime4k/", "../../shaders/anime4k/"};
	char path[256];
	FILE* f = NULL;
	long size;
	size_t i;
	for(i = 0; i < 3 && !f; i++)
	{
		snprintf(path, sizeof path, "%sAnime4K_%s.glsl", kDirs[i], name);
		f = fopen(path, "rb");
	}
	if(!f)
		return -2;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	gText[slot] = (char*)malloc((size_t)size + 1);
	if(!gText[slot] || fread(gText[slot], 1, (size_t)size, f) != (size_t)size)
	{
		fclose(f);
		return -2;
	}
	fclose(f);
	gText[slot][size] = 0;
	return OsGlHook_Parse(gText[slot], (size_t)size, gPass[slot], OS_GLHOOK_PASSES);
}

static void TestAnime4K(void)
{
	static const char* const kModeA[6] = {"Clamp_Highlights", "Restore_CNN_VL", "Upscale_CNN_x2_VL", "AutoDownscalePre_x2", "AutoDownscalePre_x4",
		"Upscale_CNN_x2_M"};
	static const int kPasses[6] = {3, 17, 18, 1, 1, 9};
	const OsGlPass_t* files[6];
	int counts[7], i;
	OsGlPlan_t plan;
	printf("anime4k\n");
	for(i = 0; i < 6; i++)
	{
		counts[i] = Load(i, kModeA[i]);
		files[i] = gPass[i];
		if(counts[i] == -2)
		{
			printf("  (the tree's shaders are not here: skipped)\n");
			return;
		}
		CHECK(counts[i] == kPasses[i]);
	}
	// as large as it is: the highlights' statistics, the restore, the clamp; nothing is made larger
	CHECK(OsGlHook_Plan(files, counts, 6, 1280, 720, 1280, 720, &plan));
	CHECK(plan.steps == 20 && plan.slot[plan.result].w == 1280 && plan.step[19].file == 0 && plan.step[19].pass == 2 && PlanSound(&plan));
	// twice: the large network alone
	CHECK(OsGlHook_Plan(files, counts, 6, 1280, 720, 2560, 1440, &plan));
	CHECK(plan.steps == 38 && plan.slot[plan.result].w == 2560 && plan.slot[plan.result].h == 1440 && PlanSound(&plan));
	// one and a half times: made twice as large, then smaller to the place
	CHECK(OsGlHook_Plan(files, counts, 6, 1280, 720, 1920, 1080, &plan));
	CHECK(plan.steps == 39 && plan.slot[plan.result].w == 1920 && plan.slot[plan.result].h == 1080 && PlanSound(&plan));
	CHECK(plan.step[37].file == 3 && plan.step[38].file == 0);
	// four times: both networks
	CHECK(OsGlHook_Plan(files, counts, 6, 800, 600, 3200, 2400, &plan));
	CHECK(plan.steps == 47 && plan.slot[plan.result].w == 3200 && plan.slot[plan.result].h == 2400 && PlanSound(&plan));
	CHECK(plan.slots <= 40);
	// mode A+A: the middle restore between the two networks, at twice the size
	counts[6] = Load(6, "Restore_CNN_M");
	CHECK(counts[6] == 8);
	{
		const OsGlPass_t* aa[7] = {gPass[0], gPass[1], gPass[2], gPass[6], gPass[3], gPass[4], gPass[5]};
		const int n[7] = {counts[0], counts[1], counts[2], counts[6], counts[3], counts[4], counts[5]};
		CHECK(OsGlHook_Plan(aa, n, 7, 800, 600, 3200, 2400, &plan));
		CHECK(plan.steps == 55 && plan.slot[plan.result].w == 3200 && PlanSound(&plan) && plan.slots <= 48);
		CHECK(plan.step[37].file == 3 && plan.step[37].w == 1600); // (the restore works on the picture made twice as large)
	}
	for(i = 0; i < 7; i++)
		free(gText[i]);
}

int main(void)
{
	TestParse();
	TestEval();
	TestPlan();
	TestAnime4K();
	if(gFails)
	{
		printf("glhook: %d checks failed\n", gFails);
		return 1;
	}
	printf("glhook: all tests passed\n");
	return 0;
}
