/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_glhook.h - shader files in the format of mpv's user shaders, as far
 *               as the Anime4K shaders use it, and the plan of the passes
 *               that run for a picture; no OpenGL in here (os_gl.c runs
 *               the plan)
 *
 * A file is a row of passes.  Each begins with lines of directives:
 *
 *     //!DESC text            a name for the pass
 *     //!HOOK MAIN            what it works on: MAIN, the picture before it
 *                             is scaled to its place, or PREKERNEL, the
 *                             same after every MAIN pass of every file
 *     //!BIND name            a texture its code reads: MAIN, HOOKED (what
 *                             it hooks), NATIVE (the picture as it came),
 *                             or one an earlier pass saved
 *     //!SAVE name            where its result goes; without it (or with
 *                             the hooked texture's name) it becomes the
 *                             picture
 *     //!WIDTH expr           the result's size, in reverse Polish over
 *     //!HEIGHT expr          numbers and NAME.w, NAME.h (OUTPUT is the
 *                             place the picture is shown at); without
 *                             them the hooked texture's
 *     //!WHEN expr            the pass runs only where this is not 0
 *     //!COMPONENTS n         how many of the result's four values count
 *
 * and goes on with GLSL that defines `vec4 hook()`.  The operators of an
 * expression: + - * / % and > < = (1 or 0) and ! (not).  A pass whose
 * WHEN names a texture that is not there, or that binds one, is left out.
 *
 *     OsGlPass_t pass[OS_GLHOOK_PASSES];
 *     int n = OsGlHook_Parse(text, size, pass, OS_GLHOOK_PASSES);
 *     ...
 *     if(OsGlHook_Plan(files, counts, nFiles, 1280, 720, 1920, 1080, &plan))
 *         for(i = 0; i < plan.steps; i++) ...   // draw step i into its slot
 */
#ifndef BGI_HOST_OS_GLHOOK_H
#define BGI_HOST_OS_GLHOOK_H

#include <stddef.h>

#define OS_GLHOOK_PASSES    24 // the most passes of a file
#define OS_GLHOOK_BINDS     16 // the most textures a pass reads
#define OS_GLHOOK_NAME      32 // bytes of a texture's name
#define OS_GLHOOK_MAIN      1  // OsGlPass_t.hook
#define OS_GLHOOK_PREKERNEL 2
#define OS_GLPLAN_STEPS     96 // the most passes of a plan
#define OS_GLPLAN_SLOTS     64 // the most textures of a plan

typedef struct OsGlPass
{
	char desc[64];
	int hook; // OS_GLHOOK_*; 0: something this reader does not know (the pass never runs)
	char bind[OS_GLHOOK_BINDS][OS_GLHOOK_NAME];
	int binds;
	char save[OS_GLHOOK_NAME];               // "": the hooked texture
	char width[128], height[128], when[192]; // the expressions as written; "": none
	int components;                          // 0: not said
	const char* body;                        // the pass's GLSL, in the file's text (not copied)
	size_t bodyLen;
} OsGlPass_t;

/* The passes of a file's text (kept by the caller while the passes are
 * used); their number, 0 for a text without one, -1 when there are more
 * than `max` or a directive does not fit. */
int OsGlHook_Parse(const char* text, size_t n, OsGlPass_t* out, int max);

/* The size of a texture by its name for an expression: 1 when there is
 * one. */
typedef int (*OsGlSizeFn_t)(void* ctx, const char* name, int* w, int* h);

// an expression's value; 0 when it is malformed or names a texture that is not there
int OsGlHook_Eval(const char* expr, OsGlSizeFn_t size, void* ctx, double* out);

// a pass of a plan: which pass, the textures it reads and the one it draws
typedef struct OsGlStep
{
	int file, pass;          // files[file][pass]
	int in[OS_GLHOOK_BINDS]; // the slot of each texture it binds, in the order of its BIND lines
	int out;                 // the slot it draws into (never one it reads)
	int w, h;                // the result's size
} OsGlStep_t;

/* What runs for a picture of picW x picH that is shown at outW x outH: the
 * steps in their order, and the textures they need - slot 0 is the picture
 * as it came and is only read, a slot is used again once nothing reads
 * what it held.  `result` is the slot with the picture after the last
 * step (0 when no pass ran). */
typedef struct OsGlPlan
{
	OsGlStep_t step[OS_GLPLAN_STEPS];
	int steps;
	struct
	{
		int w, h;
	} slot[OS_GLPLAN_SLOTS];
	int slots;
	int result;
} OsGlPlan_t;

/* The plan for the files in their order (every file's MAIN passes, then
 * every file's PREKERNEL passes); 0 when it does not fit a plan. */
int OsGlHook_Plan(const OsGlPass_t* const* files, const int* counts, int nFiles, int picW, int picH, int outW, int outH,
	OsGlPlan_t* plan);

#endif // BGI_HOST_OS_GLHOOK_H
