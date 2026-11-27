/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * compositor.h - the compositor and the worker pool it paints with;
 * implemented in src/gfx/compositor.c.  See compositor.md.
 *
 * The compositor keeps the display objects sorted by key, collects dirty
 * rectangles without overlaps, and repaints them band by band through
 * DispObj_DrawClipped().  The original hands the bands to a small thread
 * pool; this implementation runs the pool callback on the calling thread,
 * which paints exactly the same pixels because bands never overlap and
 * banding is disabled whenever an object reads what others painted.
 * The graphics manager owns one compositor for the screen; a window owns
 * another for its sub-sprites.
 */
#ifndef BGI_GFX_COMPOSITOR_H_
#define BGI_GFX_COMPOSITOR_H_

#include "bgi/gfx/dispobj.h"

// ---- worker pool -------------------------------------------------------------
typedef int (*PoolFn_t)(OpenBGI_t* o, void* arg); // returns 0 when there is no more work

#define POOL_THREADS_MAX 32 // the threads of a pool at most, the caller's included

typedef struct WorkerPool
{
	int threads; // the threads that take jobs: the caller's and the helpers that could be started (1: nothing to lock)
	PoolFn_t fn; // the job callback
	void* arg;
	int running; // inside WorkerPool_Run, on more than one thread
	/* the helpers: threads of the host's (OpenBGI_Host_t.threads) that
	 * sleep until a run has work for them */
	OpenBGI_t* o;
	void* lock;                          // guards everything below, and the callback's own queue (WorkerPool_Lock)
	void* wake;                          // a helper may take a ticket, or is to end
	void* done;                          // the last helper of a run has finished
	void* helper[POOL_THREADS_MAX - 1];  // their handles
	int helpers;
	int tickets; // helpers that are still to join this run
	int busy;    // helpers that have not finished it
	int quit;
} WorkerPool_t;

/* A pool of up to `threads` threads for the engine `o`; without `o`, or
 * where its host has no threads, one of the caller's thread alone. */
WorkerPool_t* WorkerPool_New(OpenBGI_t* o, int threads);
void WorkerPool_Delete(WorkerPool_t* p);
void WorkerPool_SetCallback(WorkerPool_t* p, PoolFn_t fn, void* arg);
/* fn(o, arg) until it returns 0 - on the caller's thread, and with
 * `parallel` on as many helpers as `jobs` has use for (jobs - 1 at
 * most).  It returns when every thread has come back. */
void WorkerPool_Run(OpenBGI_t* o, WorkerPool_t* p, int parallel, int jobs);
int WorkerPool_Lock(WorkerPool_t* p);                // the callback's queue: 1 if a lock was taken (none on one thread)
void WorkerPool_Unlock(WorkerPool_t* p, int locked);

// ---- compositor --------------------------------------------------------------
typedef struct ObjNode // one listed object
{
	uint32_t key; // sort key at insertion time
	int32_t unused04;
	DispObj_t* obj;
	struct ObjNode* next;
} ObjNode_t;

typedef struct DirtyNode // one dirty rectangle; the list never holds two that overlap
{
	Rect_t r;
	uint32_t minKey; // lowest key that dirtied it
	struct DirtyNode* next;
} DirtyNode_t;

typedef struct BandJob // one band of a dirty rectangle, painted by one worker call
{
	Rect_t r;
	uint32_t minKey;
} BandJob_t;

/* What asked for the picture to be painted again, for the performance
 * display's log (BGI_PERF=2): each size of rectangle of each priority
 * and class with how often it came and where it was last, up to
 * COMP_WATCH different ones between two readings; how often the whole picture was asked for; and how many came
 * that found no room in the table. */
#define COMP_WATCH 16
typedef struct CompWatch
{
	uint32_t key; // the object's priority and class: the top 16 bits of its sort key
	Rect_t r;
	uint32_t count; // 0: free
} CompWatch_t;

typedef struct Compositor
{
	ObjNode_t* objects; // ascending by key
	int32_t objectCount;
	DirtyNode_t* dirty;
	int32_t dirtyCount;
	int32_t maxDirty;    // more than this many: a full redraw instead
	uint32_t drawLimit;  // priority << 20 ("90 09"): objects below it are not drawn
	int32_t fullRedraw;  // paint the whole screen next time
	int32_t bgType;      // current background type (some forbid banding)
	ScreenBmp_t* screen; // the back buffer + screen rect
	uint32_t bandPixels; // pixel budget per band
	int32_t jobCount;    // the band jobs of the frame being painted
	BandJob_t* jobs;
	int32_t nextJob; // the next job a worker takes
	WorkerPool_t* pool;
	int32_t ownsPool; // the pool is private and freed with the compositor
	int32_t watchOn;  // the changed rectangles are counted (below)
	CompWatch_t watch[COMP_WATCH];
	uint32_t watchAll, watchMore;
} Compositor_t;

void Compositor_Ctor(Compositor_t* c, int maxDirty, ScreenBmp_t* screen, uint32_t bandPixels, WorkerPool_t* sharedPool);
void Compositor_Dtor(Compositor_t* c);
void Compositor_RemoveAll(Compositor_t* c);
void Compositor_Add(OpenBGI_t* o, Compositor_t* c, DispObj_t* obj);
int Compositor_Remove(Compositor_t* c, DispObj_t* obj);               // 1 if found
int Compositor_Resort(OpenBGI_t* o, Compositor_t* c, DispObj_t* obj); // after a key change; 1 if found
// `r` is clipped to the screen in place; `minKey` the key of the object that dirtied it
void Compositor_AddDirty(Compositor_t* c, uint32_t minKey, Rect_t* r);
void Compositor_ClearDirty(Compositor_t* c);
void Compositor_InvalidateAll(Compositor_t* c);
void Compositor_SetDrawLimit(Compositor_t* c, int prio); // "90 09"
void Compositor_SetBgType(Compositor_t* c, int type);
void Compositor_Snapshot(OpenBGI_t* o, Compositor_t* c, const Bmp_t* dst, int prio); // "90 05": the objects up to `prio` into dst
int Compositor_Threads(OpenBGI_t* o);                                                // the threads a frame's jobs are painted on: at least 1
int Compositor_CanBand(OpenBGI_t* o, Compositor_t* c);                               // 0 while something reads the screen
void Compositor_RenderAll(OpenBGI_t* o, Compositor_t* c);                            // the whole screen
int Compositor_Render(OpenBGI_t* o, Compositor_t* c, Rect_t* outRects);              // one frame; the repainted rectangles' count, -1 = everything
void Compositor_NotifyAll(OpenBGI_t* o, Compositor_t* c, uint32_t what, int a, int b);
int Compositor_SplitBands(Compositor_t* c, Rect_t* outOrNull, const Rect_t* r); // band count (and the bands)

// does any effector show? Defined with the effector class.
int Effector_AnyVisible(OpenBGI_t* o);

#endif // BGI_GFX_COMPOSITOR_H_
