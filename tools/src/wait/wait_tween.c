/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wait_tween.c - the display-object animation waits and their cdecl starters
 *
 * Interface: waitobj.h (the StartObj* starters).
 *
 *   WaitTween     position / level / progress tween ("90 20" .. "90 23", "90 28")
 *   WaitPath      motion along a spline path ("90 29")
 *   WaitMove2     motion through a via point on a cubic curve ("90 24")
 *   WaitObjQuake  per-object shake ("90 2C")
 *   StartObj*     the starters called by the handlers
 *
 * All four share WaitTween's layout and poll routine: the object is looked
 * up again every pass (a deleted handle is a script error), the optional
 * skip handling watches the mouse / key layers the wait pushed, and the
 * virtual step() moves the object.  WaitTween and WaitPath run on the clock
 * (GetTicks), WaitMove2 and WaitObjQuake step frame by frame on the wait
 * timer.  When the wait ends it pushes two results: the number of updates
 * per second it achieved and whether it was interrupted (by input, or by a
 * "80 4C" notification with a = 1).
 */
#include "bgi/wait.h"
#include "bgi/waitobj.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/sys.h"
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/gfx.h"
#include "bgi/gfx/text.h"
#include "bgi/gfx/path.h"

typedef struct WaitTween // 0xb0 bytes
{
	Wait_t w;
	uint32_t handle;        // the animated object's handle
	DispObj_t* obj;         // refreshed by every poll
	int32_t breakReason;    // 0, 1 mouse click, 0x100 key, 0x80000000 skip
	int32_t cur;            // elapsed: ms (clock classes) or frames (frame classes)
	int32_t duration;       // total in the same unit
	int32_t frameMs;        // timer step of the frame classes
	int32_t fps;            // frame rate (quake: frequency check)
	int32_t maxSkip;        // frames the clock may jump per update (0 = unlimited)
	int32_t updates;        // steps performed
	int32_t startX, startY; // the position at the start (pixels)
	int32_t dx, dy;         // the way to go
	int32_t curve;          // Ease curve of the position
	int32_t startLevel;     // the level at the start
	int32_t dLevel;         // the level change
	int32_t levelCurve;     // Ease curve of the level
	int32_t startProg;      // the progress at the start (integer)
	int32_t dProg;          // the progress change
	int32_t lastX, lastY;   // last values applied (0x80000000 = none)
	int32_t lastLevel;      // (-1 = none)
	int32_t lastProg;       // 16.16 (-1 = none)
	int32_t layersPushed;   // the input layers are on the stacks
	int32_t skipOn;         // "skip" argument: input ends the wait
	int32_t prio;           // input layer priority
	int32_t prevMouseTop;   // \ edge detection of the
	int32_t prevKeyTop;     // | click / key counters
	int32_t prevClicks;     // | (previous poll's values)
	int32_t prevKeys;       // /
	int32_t finished;       // the last step was run; the wait lingers one frame
	int32_t cancelMsg;      // a "80 4C" message asked to end
	int32_t interrupted;    // second result (-1 until finished)
	uint32_t startTicks;    // GetTicks at the start (clock classes)
	int32_t stepMs;         // maxSkip frames in ms
	int32_t limitMs;        // the clock may not pass this before the next update
} WaitTween_t;

typedef struct WaitPath // 0xd4 bytes
{
	WaitTween_t t;
	Path_t* path;   // the spline: the start position followed by the control points
	int32_t lastFx; // last fixed position applied (16.16)
	int32_t lastFy;
	int32_t lastFz;
	int32_t pad;
	int32_t end[4]; // the last control point (x, y, z, pad in 16.16)
} WaitPath_t;

typedef struct WaitMove2 // 0xc0 bytes
{
	WaitTween_t t;
	int32_t* curve;  // (x, y) per frame, duration + 1 entries
	int32_t targetX; // the end position
	int32_t targetY;
	int32_t targetLevel; // the end level
} WaitMove2_t;

typedef struct WaitObjQuake // 0xd0 bytes
{
	WaitTween_t t;
	int32_t pattern;     // 0 .. 5 (see WaitObjQuake_Start)
	int32_t amplitude;   // pixels
	int32_t frequency;   // cycles per second
	int32_t repeat;      // cycles
	int32_t decay;       // amplitude loss per cycle in percent
	int32_t cycleFrames; // frames per cycle
	int32_t* amps;       // amplitude of every cycle
	int32_t* wave;       // offset per frame of one cycle, 16.16 of the amplitude
} WaitObjQuake_t;

// the input layer key of priority `prio`: the low 20 bits set sort it above anything of that priority
#define LAYER_KEY(prio) (((uint32_t)(prio) << 20) | 0xfffff)

/* the extra slot of the tween tables: advance the object; 1 when the
 * animation is over.  Dispatched through the vtable, so the table is one
 * entry longer than the base one. */
typedef struct TweenVtbl
{
	WaitVtbl_t base;
	int (*step)(WaitTween_t* w);
} TweenVtbl_t;
#define STEP(w) (((const TweenVtbl_t*)(w)->w.vt)->step(w))

static void WaitTween_Destroy(Wait_t* w);
static int WaitTween_Poll(Wait_t* w);
static void WaitTween_OnMessage(Wait_t* w, WaitMsg_t* m);
static int WaitTween_Step(WaitTween_t* w);
static void WaitPath_Destroy(Wait_t* w);
static int WaitPath_Step(WaitTween_t* w);
static void WaitMove2_Destroy(Wait_t* w);
static int WaitMove2_Step(WaitTween_t* w);
static void WaitObjQuake_Destroy(Wait_t* w);
static int WaitObjQuake_Step(WaitTween_t* w);

static const TweenVtbl_t WaitTween_Vtbl = {
	{WaitTween_Destroy, WaitTween_Poll, Wait_SetTimer, Wait_IsDirtyBase, WaitTween_OnMessage}, WaitTween_Step};
static const TweenVtbl_t WaitPath_Vtbl = {
	{WaitPath_Destroy, WaitTween_Poll, Wait_SetTimer, Wait_IsDirtyBase, WaitTween_OnMessage}, WaitPath_Step};
static const TweenVtbl_t WaitMove2_Vtbl = {
	{WaitMove2_Destroy, WaitTween_Poll, Wait_SetTimer, Wait_IsDirtyBase, WaitTween_OnMessage}, WaitMove2_Step};
static const TweenVtbl_t WaitObjQuake_Vtbl = {
	{WaitObjQuake_Destroy, WaitTween_Poll, Wait_SetTimer, Wait_IsDirtyBase, WaitTween_OnMessage}, WaitObjQuake_Step};

// ========================================================================
// WaitTween
// ========================================================================

// the shared constructor: resolves the handle in the text manager (the starters check it exists)
void WaitTween_Ctor(WaitTween_t* w, Thread_t* t, uint32_t h)
{
	Wait_Ctor(&w->w, t);
	w->w.vt = &WaitTween_Vtbl.base;
	w->handle = h;
	w->obj = Gfx_FindObject(gTextGfx, h);
	w->skipOn = 0;
	w->layersPushed = 0;
	w->prevMouseTop = 0;
	w->prevKeyTop = 0;
	w->finished = 0;
	w->cancelMsg = 0;
	w->interrupted = -1;
}

// pops the input layers the skip option pushed
void WaitTween_Dtor(WaitTween_t* w)
{
	if(w->layersPushed)
	{
		MouseLayer_Pop(LAYER_KEY(w->prio));
		KeyLayer_Pop(LAYER_KEY(w->prio));
	}
	Wait_Dtor(&w->w);
}

static void WaitTween_Destroy(Wait_t* w)
{
	WaitTween_Dtor((WaitTween_t*)w);
	BGI_Free(w);
}

/* the general start.  cur runs 0 .. duration (ms); the deltas are taken
 * from the object's current position, level and progress towards (x, y),
 * level and progress.  The object is made visible; the first step runs
 * after 1 ms. */
static void WaitTween_Start(WaitTween_t* w, int32_t x, int32_t y, int curve, int level, int levelCurve, int progress,
	int duration, int fps, int maxSkip)
{
	int32_t pos[2];
	w->cur = 0;
	w->duration = duration ? duration : 1;
	w->frameMs = 1;
	w->obj->vt->getPos(w->obj, pos);
	w->startX = pos[0];
	w->startY = pos[1];
	w->dx = x - pos[0];
	w->dy = y - pos[1];
	w->curve = curve;
	w->startLevel = w->obj->vt->getLevel(w->obj);
	w->dLevel = level - w->startLevel;
	w->levelCurve = levelCurve;
	w->startProg = DispObj_GetProgressInt(w->obj);
	w->dProg = progress < 0 ? 0 : progress - w->startProg; // -1 (1.69/472 on): keep the progress
	w->lastX = w->lastY = (int32_t)0x80000000;
	w->lastLevel = w->lastProg = -1;
	w->obj->vt->setVisible(w->obj, 1);
	w->obj->vt->invalidate(w->obj);
	w->fps = fps;
	w->maxSkip = maxSkip;
	w->updates = 0;
	w->startTicks = GetTicks();
	w->stepMs = w->limitMs = maxSkip * 1000 / fps;
	w->w.vt->setTimer(&w->w, 1);
}

// position + level, the progress stays
static void WaitTween_StartMove(WaitTween_t* w, int32_t x, int32_t y, int curve, int level, int duration, int fps,
	int maxSkip)
{
	WaitTween_Start(w, x, y, curve, level, 0, DispObj_GetProgressInt(w->obj), duration, fps, maxSkip);
}

// level only, the position stays
static void WaitTween_StartInPlace(WaitTween_t* w, int level, int duration, int fps, int maxSkip)
{
	int32_t pos[2];
	w->obj->vt->getPos(w->obj, pos);
	WaitTween_StartMove(w, pos[0], pos[1], 0, level, duration, fps, maxSkip);
}

/* the skip option: with `on` the wait pushes a mouse and a key layer at
 * `prio` and remembers the click / key counters so poll() can see a new
 * press */
static void WaitTween_SetSkip(WaitTween_t* w, int on, uint32_t prio)
{
	w->skipOn = on;
	if(!on)
		return;
	if(w->layersPushed)
	{
		MouseLayer_Pop(LAYER_KEY(w->prio));
		KeyLayer_Pop(LAYER_KEY(w->prio));
	}
	w->prio = (int32_t)prio;
	MouseLayer_Push(LAYER_KEY(prio));
	KeyLayer_Push(LAYER_KEY(prio));
	w->prevClicks = CountKeysByMask(1);
	w->prevKeys = CountKeysByMask(Input_TweenKeyMask()); // 0x2180 in the reference
	w->layersPushed = 1;
}

/* the shared poll.  A click, a standard key or the skip state sets
 * breakReason (a press counts only while the wait's layer is on top in
 * this and the previous poll); the step runs when the timer expired or the
 * wait is being broken; after the last step the wait lingers for one more
 * frame interval and then pushes (updates * 1000 / duration, interrupted).
 * In non-blocking mode it ends at once with the results so far
 * (interrupted = -1). */
static int WaitTween_Poll(Wait_t* base)
{
	WaitTween_t* w = (WaitTween_t*)base;
	int blocking;
	w->obj = Gfx_FindObject(gTextGfx, w->handle);
	if(!w->obj)
		ScriptError(MSG_OBJ_HANDLE_DELETED, base->thread);
	Wait_Tick(base);
	w->breakReason = 0;
	if(w->skipOn)
	{
		uint32_t key = LAYER_KEY(w->prio);
		int clicks = CountKeysByMask(1);
		int mouseTop = MouseLayer_Hit(key);
		int keys, keyTop;
		if(mouseTop && w->prevMouseTop && (uint32_t)clicks > (uint32_t)w->prevClicks)
			w->breakReason = 1;
		keys = CountKeysByMask(Input_TweenKeyMask());
		keyTop = KeyLayer_IsTop(key);
		if(keyTop && w->prevKeyTop && (uint32_t)keys > (uint32_t)w->prevKeys)
			w->breakReason = 0x100;
		if(Input_CheckSkip())
			w->breakReason = (int32_t)0x80000000;
		if(w->breakReason)
			Input_Poll(key, key); // consume the triggers
		w->prevKeys = keys;
		w->prevMouseTop = mouseTop;
		w->prevKeyTop = keyTop;
		w->prevClicks = clicks;
	}
	blocking = Wait_IsBlocking(base);
	if(!w->finished && blocking)
	{
		if(Wait_TimerExpired(base) || w->breakReason || w->cancelMsg)
		{
			if(STEP(w))
			{
				w->finished = 1;
				w->interrupted = (w->breakReason || w->cancelMsg) ? 1 : 0;
			}
			Wait_Flush(base);
			w->updates++;
		}
		return 0;
	}
	if(!Wait_TimerExpired(base) && blocking)
		return 0;
	Thread_Push(base->thread, (uint32_t)(w->updates * 1000 / w->duration));
	Thread_Push(base->thread, (uint32_t)w->interrupted);
	return 1;
}

// "80 4C" with a = 1 ends the wait (b != 0, or any b while skipping is on)
static void WaitTween_OnMessage(Wait_t* base, WaitMsg_t* m)
{
	WaitTween_t* w = (WaitTween_t*)base;
	if(m->a == 1 && (m->b != 0 || w->skipOn))
		w->cancelMsg = 1;
}

/* the frame-stepped classes' clock.  Every expired frame interval adds
 * frameMs to the timer and counts a frame, at most maxSkip frames and
 * never past duration; 1 when cur reached duration (the timer is re-armed
 * then so poll() lingers for one frame). */
static int WaitTween_AdvanceFrames(WaitTween_t* w)
{
	int n = 0;
	if(Wait_TimerExpired(&w->w))
	{
		while(w->cur + n < w->duration)
		{
			Wait_TimerAdd(&w->w, (uint32_t)w->frameMs);
			n++;
			if(Wait_TimerExpired(&w->w) && n == w->maxSkip)
			{ // the clock fell too far behind: drop the backlog and start afresh
				w->w.vt->setTimer(&w->w, (uint32_t)w->frameMs);
				break;
			}
			if(!Wait_TimerExpired(&w->w))
				break;
		}
	}
	w->cur += n;
	if(Wait_TimerExpired(&w->w) && w->cur == w->duration)
		w->w.vt->setTimer(&w->w, (uint32_t)w->frameMs);
	return w->cur == w->duration;
}

// progress 8.16 of cur / duration, through the curve, as a 16.16 factor
static int32_t TweenFactor(const WaitTween_t* w, int curve)
{
	return Ease((int32_t)(((int64_t)w->cur * 0x1000000) / w->duration), curve);
}

/* the clock classes' time keeping: cur = elapsed ms, limited to maxSkip
 * frames past the previous update and to duration; 1 when the end is
 * reached */
static int WaitTween_Clock(WaitTween_t* w)
{
	int32_t elapsed = (int32_t)(GetTicks() - w->startTicks);
	if(w->maxSkip == 0)
		w->cur = elapsed;
	else
	{
		w->cur = (uint32_t)elapsed < (uint32_t)w->limitMs ? elapsed : w->limitMs;
		w->limitMs = w->stepMs + w->cur;
	}
	if(w->cur >= w->duration)
		w->cur = w->duration;
	return w->cur == w->duration;
}

/* the level / progress part of the clock classes' step: the level through
 * its curve, the progress linearly (16.16); the exact end values when done */
static void TweenLevelProgress(WaitTween_t* w, int done, int32_t* level, int32_t* prog)
{
	if(done)
	{
		*level = w->startLevel + w->dLevel;
		*prog = (int32_t)((uint32_t)(w->startProg + w->dProg) << 16);
		return;
	}
	*level = (int32_t)(((int64_t)w->dLevel * TweenFactor(w, w->levelCurve)) >> 16) + w->startLevel;
	*prog = (int32_t)(((int64_t)(w->dProg * w->cur) * 0x10000) / w->duration) + (int32_t)((uint32_t)w->startProg << 16);
}

// the step of a tween after it was broken: jump to the end; 0 when it was not
static int TweenBroken(WaitTween_t* w)
{
	if(w->breakReason == 0 && w->cancelMsg == 0)
		return 0;
	w->cur = w->duration;
	return 1;
}

// the timer after a step: one frame interval at the end, 1 ms otherwise
static void TweenRearm(WaitTween_t* w, int done)
{
	w->w.vt->setTimer(&w->w, done ? (uint32_t)Present_GetInterval() : 1u);
}

// apply the position, level and progress of the current time; the object is touched only when something changed
static int WaitTween_Step(WaitTween_t* w)
{
	int done;
	int32_t x, y, level, prog;
	if(TweenBroken(w))
		done = 1;
	else
		done = WaitTween_Clock(w);
	if(done)
	{
		x = w->startX + w->dx;
		y = w->startY + w->dy;
	}
	else
	{
		int32_t f = TweenFactor(w, w->curve);
		x = (int32_t)(((int64_t)w->dx * f) >> 16) + w->startX;
		y = (int32_t)(((int64_t)w->dy * f) >> 16) + w->startY;
	}
	TweenLevelProgress(w, done, &level, &prog);
	if(x != w->lastX || y != w->lastY || level != w->lastLevel || prog != w->lastProg)
	{
		w->lastX = x;
		w->lastY = y;
		w->lastLevel = level;
		w->lastProg = prog;
		w->obj->vt->invalidate(w->obj);
		w->obj->vt->setPos(w->obj, x, y);
		w->obj->vt->setLevel(w->obj, level);
		w->obj->vt->setProgress(w->obj, 1, prog);
		w->obj->vt->invalidate(w->obj);
		Wait_MarkDirty(&w->w);
	}
	TweenRearm(w, done);
	return done;
}

// ========================================================================
// WaitPath
// ========================================================================

static void WaitPath_Ctor(WaitPath_t* w, Thread_t* t, uint32_t h)
{
	WaitTween_Ctor(&w->t, t, h);
	w->t.w.vt = &WaitPath_Vtbl.base;
	w->path = Path_New();
}

static void WaitPath_Destroy(Wait_t* base)
{
	WaitPath_t* w = (WaitPath_t*)base;
	if(w->path)
		Path_Delete(w->path);
	WaitTween_Dtor(&w->t);
	BGI_Free(w);
}

/* the tween runs towards the last control point; the path starts at the
 * object's current fixed position followed by the n points (x, y, z, pad -
 * 16.16 each), parametrised 0 .. 0x10000.  0x80000001 without points. */
static int WaitPath_Start(WaitPath_t* w, int n, const int32_t* points, int curve, int level, int levelCurve,
	int progress, int duration, int fps, int maxSkip)
{
	const int32_t* last = points + (n - 1) * 4;
	int32_t cur[4];
	int i;
	if(n <= 0)
		return (int)0x80000001;
	WaitTween_Start(&w->t, last[0], last[1], curve, level, levelCurve, progress, duration, fps, maxSkip);
	w->t.obj->vt->getFixedPos(w->t.obj, cur);
	Path_Clear(w->path);
	Path_AddPoint(w->path, cur[0], cur[1], cur[2]);
	for(i = 0; i < n; i++)
		Path_AddPoint(w->path, points[i * 4], points[i * 4 + 1], points[i * 4 + 2]);
	Path_SetRange(w->path, 0, 0x10000);
	memcpy(w->end, last, sizeof w->end);
	return 0;
}

// evaluate the path at the eased time and apply it as the fixed position (with level and progress)
static int WaitPath_Step(WaitTween_t* base)
{
	WaitPath_t* w = (WaitPath_t*)base;
	int done;
	int32_t p[3], level, prog;
	if(TweenBroken(base))
		done = 1;
	else
		done = WaitTween_Clock(base);
	if(done)
	{
		p[0] = w->end[0];
		p[1] = w->end[1];
		p[2] = w->end[2];
	}
	else
		Path_Eval(w->path, (uint32_t)TweenFactor(base, base->curve), p);
	TweenLevelProgress(base, done, &level, &prog);
	if(p[0] != w->lastFx || p[1] != w->lastFy || p[2] != w->lastFz || level != base->lastLevel ||
		prog != base->lastProg)
	{
		w->lastFx = p[0];
		w->lastFy = p[1];
		w->lastFz = p[2];
		base->lastLevel = level;
		base->lastProg = prog;
		base->obj->vt->invalidate(base->obj);
		base->obj->vt->setFixedPos(base->obj, p[0], p[1], p[2]);
		base->obj->vt->setLevel(base->obj, level);
		base->obj->vt->setProgress(base->obj, 1, prog);
		base->obj->vt->invalidate(base->obj);
		Gfx_Resort(gTextGfx, base->obj); // the depth may have changed
		Wait_MarkDirty(&base->w);
	}
	TweenRearm(base, done);
	return done;
}

// ========================================================================
// WaitMove2: the cubic curve through a via point
// ========================================================================

// round half away from zero
static int32_t RoundToInt(double v)
{
	return BGI_Ftol(v < 0.0 ? v - 0.5 : v + 0.5);
}

/* natural cubic spline through the three knots (X[i], Y[i]) evaluated at
 * t, in the original's operation order (so the rounding matches) */
static double Spline3(const double* X, const double* Y, double t)
{
	double h[2], F[3], b, d, dx;
	int seg = t < X[1] ? 0 : 1;
	h[0] = X[1] - X[0];
	h[1] = X[2] - X[1];
	F[0] = 0.0;
	F[1] = ((Y[2] - Y[1]) / h[1] - (Y[1] - Y[0]) / h[0]) * 3.0 / ((h[0] + h[1]) * 2.0);
	F[2] = 0.0;
	dx = t - X[seg];
	b = (Y[seg + 1] - Y[seg]) / h[seg] - (F[seg] + F[seg] + F[seg + 1]) * h[seg] * 0.3333333333333333;
	d = (F[seg + 1] - F[seg]) / (h[seg] * 3.0);
	return ((d * dx + F[seg]) * dx + b) * dx + Y[seg];
}

/* `count` points of the curve from (x0, y0) through (x1, y1) to (x2, y2),
 * y as a spline function of x, equally spaced in x into out (x, y pairs);
 * the x values must be monotonic (0 otherwise, 1 ok).  A decreasing run is
 * mirrored so the spline sees increasing x. */
static int Curve_Build(int32_t* out, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
	uint32_t count)
{
	double X[3], Y[3], cur, step;
	int increasing;
	uint32_t i;
	if(x0 < x1 ? x1 > x2 : (x0 > x1 && x1 < x2))
		return 0;
	if(x0 < x1 && x1 < x2)
		increasing = 1;
	else
	{
		x0 = -x0;
		x1 = -x1;
		x2 = -x2;
		increasing = 0;
	}
	X[0] = 0.0;
	X[1] = (double)(x1 - x0);
	X[2] = (double)(x2 - x0);
	Y[0] = 0.0;
	Y[1] = (double)(y1 - y0);
	Y[2] = (double)(y2 - y0);
	cur = X[0];
	step = (X[2] - X[0]) / (double)(count - 1);
	for(i = 0; i < count; i++)
	{
		int32_t xi = RoundToInt(cur);
		out[i * 2] = increasing ? xi + x0 : -(xi + x0);
		out[i * 2 + 1] = RoundToInt(Spline3(X, Y, cur)) + y0;
		cur += step;
	}
	return 1;
}

static void WaitMove2_Ctor(WaitMove2_t* w, Thread_t* t, uint32_t h)
{
	WaitTween_Ctor(&w->t, t, h);
	w->t.w.vt = &WaitMove2_Vtbl.base;
	w->curve = NULL;
}

static void WaitMove2_Destroy(Wait_t* base)
{
	WaitMove2_t* w = (WaitMove2_t*)base;
	BGI_Free(w->curve);
	WaitTween_Dtor(&w->t);
	BGI_Free(w);
}

/* frames = duration * fps / 1000 (at least 1), one curve point per frame
 * plus one, from the object's position through (viaX, viaY) to (x, y); the
 * level goes linearly to `level`.  0x80000001 fps < 1, 0x80000002 the
 * curve cannot be built (x not monotonic). */
static int WaitMove2_Start(WaitMove2_t* w, int32_t viaX, int32_t viaY, int32_t x, int32_t y, int curve, int level,
	int duration, int fps, int maxSkip)
{
	WaitTween_t* t = &w->t;
	int32_t pos[2];
	uint32_t frames;
	if((uint32_t)fps < 1)
		return (int)0x80000001;
	t->cur = 0;
	frames = (uint32_t)(duration * fps) / 1000u;
	t->duration = (int32_t)(frames ? frames : 1);
	t->frameMs = t->duration > 1 ? (int32_t)(1000u / (uint32_t)fps) : duration; // a single frame takes the whole duration
	t->obj->vt->getPos(t->obj, pos);
	BGI_Free(w->curve);
	w->curve = (int32_t*)BGI_Alloc((size_t)t->duration * 8 + 8);
	if(!Curve_Build(w->curve, pos[0], pos[1], viaX, viaY, x, y, (uint32_t)t->duration + 1))
		return (int)0x80000002;
	t->curve = curve;
	t->startLevel = t->obj->vt->getLevel(t->obj);
	t->dLevel = level - t->startLevel;
	t->lastLevel = -1;
	t->lastX = t->lastY = (int32_t)0x80000000;
	t->obj->vt->setVisible(t->obj, 1);
	t->obj->vt->invalidate(t->obj);
	t->w.vt->setTimer(&t->w, (uint32_t)t->frameMs);
	w->targetLevel = level;
	t->fps = fps;
	t->maxSkip = maxSkip;
	t->updates = 0;
	w->targetX = x;
	w->targetY = y;
	return 0;
}

// pick the curve point of the eased frame index and apply it with the linear level
static int WaitMove2_Step(WaitTween_t* base)
{
	WaitMove2_t* w = (WaitMove2_t*)base;
	int done;
	int32_t x, y, level;
	if(TweenBroken(base))
		done = 1;
	else
		done = WaitTween_AdvanceFrames(base);
	if(done)
	{
		x = w->targetX;
		y = w->targetY;
		level = w->targetLevel;
	}
	else
	{
		int32_t f = TweenFactor(base, base->curve);
		int32_t idx = (int32_t)(((int64_t)(base->duration + 1) * f) >> 16);
		x = w->curve[idx * 2];
		y = w->curve[idx * 2 + 1];
		level = base->dLevel * base->cur / base->duration + base->startLevel;
	}
	if(x != base->lastX || y != base->lastY || level != base->lastLevel)
	{
		base->lastX = x;
		base->lastY = y;
		base->lastLevel = level;
		base->obj->vt->invalidate(base->obj);
		base->obj->vt->setPos(base->obj, x, y);
		base->obj->vt->setLevel(base->obj, level);
		base->obj->vt->invalidate(base->obj);
		Wait_MarkDirty(&base->w);
	}
	return done;
}

// ========================================================================
// WaitObjQuake
// ========================================================================

static void WaitObjQuake_Ctor(WaitObjQuake_t* w, Thread_t* t, uint32_t h)
{
	WaitTween_Ctor(&w->t, t, h);
	w->t.w.vt = &WaitObjQuake_Vtbl.base;
	w->amps = NULL;
	w->wave = NULL;
}

static void WaitObjQuake_Destroy(Wait_t* base)
{
	WaitObjQuake_t* w = (WaitObjQuake_t*)base;
	BGI_Free(w->amps);
	BGI_Free(w->wave);
	WaitTween_Dtor(&w->t);
	BGI_Free(w);
}

/* pattern 0..5 (0, 1, 4 vertical, 2, 3, 5 horizontal; 0 and 2 start
 * towards the negative side, 4 and 5 start half a cycle in), the amplitude
 * decays by `decay` percent per cycle.  Precomputes one cycle of the wave
 * and the amplitude of every cycle.  0x80000001 bad pattern, 0x80000002
 * frequency < 1, 0x80000003 repeat < 1, 0x80000004 fps < 1 or below the
 * frequency. */
static int WaitObjQuake_Start(WaitObjQuake_t* w, int pattern, int amplitude, int frequency, int repeat, int decay,
	int fps)
{
	WaitTween_t* t = &w->t;
	int32_t pos[2], step, phase, phase0;
	int64_t amp;
	int i;
	if((uint32_t)pattern >= 6)
		return (int)0x80000001;
	if((uint32_t)frequency < 1)
		return (int)0x80000002;
	if((uint32_t)repeat < 1)
		return (int)0x80000003;
	if((uint32_t)fps < 1 || (uint32_t)fps < (uint32_t)frequency)
		return (int)0x80000004;
	w->pattern = pattern;
	w->decay = decay;
	w->amplitude = amplitude;
	t->fps = fps;
	t->frameMs = (int32_t)(1000u / (uint32_t)fps);
	w->frequency = frequency;
	w->repeat = repeat;
	t->maxSkip = 0;
	t->updates = 0;
	t->cur = 0;
	w->cycleFrames = (int32_t)(1000u / (uint32_t)(t->frameMs * frequency));
	t->duration = w->cycleFrames * repeat;
	t->obj->vt->getPos(t->obj, pos);
	t->startX = pos[0];
	t->startY = pos[1];
	t->lastX = t->lastY = (int32_t)0x80000000;
	t->obj->vt->setVisible(t->obj, 1);
	t->obj->vt->invalidate(t->obj);
	t->w.vt->setTimer(&t->w, (uint32_t)t->frameMs);

	// the amplitude of each cycle, decaying
	BGI_Free(w->amps);
	w->amps = (int32_t*)BGI_Alloc((size_t)repeat * 4);
	amp = (int64_t)((uint32_t)amplitude << 16); // zero-extended, as the original's 64-bit pair
	for(i = 0; (uint32_t)i < (uint32_t)repeat; i++)
	{
		w->amps[i] = (int32_t)(amp >> 16);
		amp = amp * (100 - decay) / 100;
	}
	// one cycle of a triangle wave, 0 .. 0x10000, as the offset's 16.16 factor
	BGI_Free(w->wave);
	w->wave = (int32_t*)BGI_Alloc((size_t)w->cycleFrames * 4);
	step = (int32_t)(0x20000u / (uint32_t)w->cycleFrames);
	phase0 = pattern < 4 ? 0 : 0x8000;
	phase = phase0;
	for(i = 0; (uint32_t)i < (uint32_t)w->cycleFrames; i++)
	{
		phase += step;
		if(phase >= 0x10000)
		{
			step = -step;
			phase = 0x20000 - phase;
		}
		if(phase <= 0)
		{
			step = -step;
			phase = -phase;
		}
		w->wave[i] = (pattern == 0 || pattern == 2) ? phase0 - phase : phase - phase0;
	}
	return 0;
}

// offset the start position by the current cycle's amplitude times the wave sample of the frame
static int WaitObjQuake_Step(WaitTween_t* base)
{
	WaitObjQuake_t* w = (WaitObjQuake_t*)base;
	int done;
	int32_t x = base->startX, y = base->startY;
	if(TweenBroken(base))
		done = 1;
	else
		done = WaitTween_AdvanceFrames(base);
	if(!done)
	{
		uint32_t cycle = (uint32_t)base->cur / (uint32_t)w->cycleFrames;
		uint32_t frame = (uint32_t)base->cur % (uint32_t)w->cycleFrames;
		// the amplitude enters the 64-bit product zero-extended, the wave sign-extended
		int32_t off = (int32_t)(((int64_t)(uint32_t)w->amps[cycle] * (int64_t)w->wave[frame]) >> 16);
		if(w->pattern == 2 || w->pattern == 3 || w->pattern == 5)
			x += off;
		else
			y += off;
	}
	if(x != base->lastX || y != base->lastY)
	{
		base->lastX = x;
		base->lastY = y;
		base->obj->vt->invalidate(base->obj);
		base->obj->vt->setPos(base->obj, x, y);
		base->obj->vt->invalidate(base->obj);
		Wait_MarkDirty(&base->w);
	}
	return done;
}

// ========================================================================
// the starters
// ========================================================================

/* "90 20" .. "90 23": tween the level to `level` over `duration` ms and,
 * with `move`, the position to (x, y) along `curve`.  0 ok, 0x80000001
 * fps < 1, -1 no such object. */
int StartObjTween(Thread_t* t, uint32_t h, int32_t x, int32_t y, int curve, int move, int level, int duration,
	int fps, int maxSkip, int skip, uint32_t prio)
{
	WaitTween_t* w;
	if((uint32_t)fps < 1)
		return (int)0x80000001;
	if(!Gfx_FindObject(gGfx, h))
		return -1;
	w = (WaitTween_t*)BGI_Alloc(sizeof *w);
	WaitTween_Ctor(w, t, h);
	if(move)
		WaitTween_StartMove(w, x, y, curve, level, duration, fps, maxSkip);
	else
		WaitTween_StartInPlace(w, level, duration, fps, maxSkip);
	WaitTween_SetSkip(w, skip, prio);
	Thread_SetWait(t, &w->w);
	return 0;
}

/* "90 24": move through (viaX, viaY) to (x, y) on a cubic curve while the
 * level goes to `level`.  0 ok, -1 no such object, 0x80000001 fps < 1,
 * 0x80000002 the x values are not monotonic. */
int StartObjMove2(Thread_t* t, uint32_t h, int32_t viaX, int32_t viaY, int32_t x, int32_t y, int curve, int level,
	int duration, int fps, int maxSkip, int skip, uint32_t prio)
{
	WaitMove2_t* w;
	int r;
	if(!Gfx_FindObject(gGfx, h))
		return -1;
	w = (WaitMove2_t*)BGI_Alloc(sizeof *w);
	WaitMove2_Ctor(w, t, h);
	r = WaitMove2_Start(w, viaX, viaY, x, y, curve, level, duration, fps, maxSkip);
	if(r != 0)
	{
		Wait_Release(&w->t.w);
		return r; // (the original hands back the object pointer for a code it does not recognise; the start has none)
	}
	WaitTween_SetSkip(&w->t, skip, prio);
	Thread_SetWait(t, &w->t.w);
	return 0;
}

/* "90 28": tween the position to (x, y) along `curve`, the level to
 * `level` along `levelCurve` and the progress to `progress` (-1 keeps it).
 * 0 ok, 0x80000001 fps < 1, -1 no such object. */
int StartObjTweenFull(Thread_t* t, uint32_t h, int32_t x, int32_t y, int curve, int level, int levelCurve, int progress,
	int duration, int fps, int maxSkip, int skip, uint32_t prio)
{
	WaitTween_t* w;
	if((uint32_t)fps < 1)
		return (int)0x80000001;
	if(!Gfx_FindObject(gGfx, h))
		return -1;
	w = (WaitTween_t*)BGI_Alloc(sizeof *w);
	WaitTween_Ctor(w, t, h);
	WaitTween_Start(w, x, y, curve, level, levelCurve, progress, duration, fps, maxSkip);
	WaitTween_SetSkip(w, skip, prio);
	Thread_SetWait(t, &w->w);
	return 0;
}

/* "90 29": move along the spline through the `n` control points (x, y, z,
 * pad in 16.16 each) with level and progress as "90 28".  0 ok, 0x80000001
 * fps < 1, -1 no such object, 0x80000003 n < 1. */
int StartObjPath(Thread_t* t, uint32_t h, int n, const int32_t* points, int curve, int level, int levelCurve,
	int progress, int duration, int fps, int maxSkip, int skip, uint32_t prio)
{
	WaitPath_t* w;
	int r;
	if((uint32_t)fps < 1)
		return (int)0x80000001;
	if(!Gfx_FindObject(gGfx, h))
		return -1;
	w = (WaitPath_t*)BGI_Alloc(sizeof *w);
	WaitPath_Ctor(w, t, h);
	r = WaitPath_Start(w, n, points, curve, level, levelCurve, progress, duration, fps, maxSkip);
	if(r == (int)0x80000001)
	{
		Wait_Release(&w->t.w);
		return (int)0x80000003;
	}
	if(r != 0)
		return r; // the original returns the object pointer here; unreachable, the start has no other code
	WaitTween_SetSkip(&w->t, skip, prio);
	Thread_SetWait(t, &w->t.w);
	return 0;
}

/* "90 2C": shake the object (see WaitObjQuake_Start for the patterns and
 * the codes); 0 ok, -1 no such object */
int StartObjQuake(Thread_t* t, uint32_t h, int pattern, int amplitude, int frequency, int repeat, int decay, int fps,
	int skip, uint32_t prio)
{
	WaitObjQuake_t* w;
	int r;
	if(!Gfx_FindObject(gGfx, h))
		return -1;
	w = (WaitObjQuake_t*)BGI_Alloc(sizeof *w);
	WaitObjQuake_Ctor(w, t, h);
	r = WaitObjQuake_Start(w, pattern, amplitude, frequency, repeat, decay, fps);
	if(r != 0)
	{
		Wait_Release(&w->t.w); // the original leaks the object on these errors
		return r;
	}
	WaitTween_SetSkip(&w->t, skip, prio);
	Thread_SetWait(t, &w->t.w);
	return 0;
}

// the name of a wait class of this file, for the debugger (NULL: not one of these)
const char* WaitTween_ClassName(const WaitVtbl_t* vt)
{
	if(vt == &WaitTween_Vtbl.base)
		return "WaitTween";
	if(vt == &WaitPath_Vtbl.base)
		return "WaitPath";
	if(vt == &WaitMove2_Vtbl.base)
		return "WaitMove2";
	if(vt == &WaitObjQuake_Vtbl.base)
		return "WaitObjQuake";
	return NULL;
}
