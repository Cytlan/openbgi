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
typedef int (*PoolFn_t)(void* arg); // returns 0 when there is no more work

typedef struct WorkerPool
{
	int threads; // the thread count the pool was made for (1: nothing to lock)
	PoolFn_t fn; // the job callback
	void* arg;
	int running; // inside WorkerPool_Run
} WorkerPool_t;

WorkerPool_t* WorkerPool_New(int threads);
void WorkerPool_Delete(WorkerPool_t* p);
void WorkerPool_SetCallback(WorkerPool_t* p, PoolFn_t fn, void* arg);
void WorkerPool_Run(WorkerPool_t* p, int parallel); // fn(arg) until it returns 0
int WorkerPool_Lock(WorkerPool_t* p);               // 1 if a lock was taken
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
} Compositor_t;

void Compositor_Ctor(Compositor_t* c, int maxDirty, ScreenBmp_t* screen, uint32_t bandPixels, WorkerPool_t* sharedPool);
void Compositor_Dtor(Compositor_t* c);
void Compositor_RemoveAll(Compositor_t* c);
void Compositor_Add(Compositor_t* c, DispObj_t* o);
int Compositor_Remove(Compositor_t* c, DispObj_t* o); // 1 if found
int Compositor_Resort(Compositor_t* c, DispObj_t* o); // after a key change; 1 if found
// `r` is clipped to the screen in place; `minKey` the key of the object that dirtied it
void Compositor_AddDirty(Compositor_t* c, uint32_t minKey, Rect_t* r);
void Compositor_ClearDirty(Compositor_t* c);
void Compositor_InvalidateAll(Compositor_t* c);
void Compositor_SetDrawLimit(Compositor_t* c, int prio); // "90 09"
void Compositor_SetBgType(Compositor_t* c, int type);
void Compositor_Snapshot(Compositor_t* c, const Bmp_t* dst, int prio); // "90 05": the objects up to `prio` into dst
int Compositor_CanBand(Compositor_t* c);                               // 0 while something reads the screen
void Compositor_RenderAll(Compositor_t* c);                            // the whole screen
int Compositor_Render(Compositor_t* c, Rect_t* outRects);              // one frame; the repainted rectangles' count, -1 = everything
void Compositor_NotifyAll(Compositor_t* c, uint32_t what, int a, int b);
int Compositor_SplitBands(Compositor_t* c, Rect_t* outOrNull, const Rect_t* r); // band count (and the bands)

// does any effector show? Defined with the effector class.
int Effector_AnyVisible(void);

#endif // BGI_GFX_COMPOSITOR_H_
