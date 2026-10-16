/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * present.c - getting the back buffer onto the window; interface in
 *             bgi/display.h
 *
 * The compositor renders the display objects into the back buffer; this
 * file decides when the result is copied to the window.  A present is
 * requested (whole picture or dirty rectangles) by whatever changed the
 * picture and carried out once per frame interval ("90 02") by
 * PresentFrame, which the scheduler calls every pass; "90 01" and the
 * screen quake can hold presenting off.  Also here: the direct blits that
 * bypass the compositor ("B0 00", the child windows), the repaint on
 * expose, the render-time profiler ("80 06" / "80 07") and the
 * end-of-pass sleep ("80 52").
 */
#include "bgi/display.h"
#include "bgi/edit.h"
#include "bgi/engine.h"
#include "bgi/gfx.h"
#include "bgi/sys.h"
#include "bgi/os.h"

static int gPresentPending;         // a present was requested
static int gPresentPendingFull;     // ... of the whole picture
static int gPresentAllowed = 1;     // Present_Allow: 0 while the screen quake presents on its own
static uint32_t gFrameInterval = 4; // ms between presents
static uint32_t gNextPresentTime;   // the tick of the next present (on the interval grid)
static uint32_t gFrameCount;        // frames presented; nothing reads it

// Allow or suspend presenting (the screen quake presents on its own while it runs).
void Present_Allow(int on)
{
	gPresentAllowed = on;
}

int Present_IsAllowed(void)
{
	return gPresentAllowed;
}

int Present_GetInterval(void)
{
	return (int)gFrameInterval;
}

// "90 02": the frame rate; the interval between presents becomes 1000 / fps ms (at least 1).
void Present_SetRate(int fps)
{
	if(fps > 0 && 1000u / (uint32_t)fps > 0)
		gFrameInterval = 1000u / (uint32_t)fps;
	else
		gFrameInterval = 1;
	gNextPresentTime = 0;
}

// note a request; a full present subsumes the dirty ones
static void Present_Request(int full)
{
	if(gPresentPending)
	{
		gPresentPendingFull |= full;
	}
	else
	{
		gPresentPending = 1;
		gPresentPendingFull = full;
	}
}

void Present_RequestFull(void)
{
	Present_Request(1);
}

void Present_RequestDirty(void)
{
	Present_Request(0);
}

/* Present the whole back buffer shifted by (x, y) - the screen quake's
 * frame ("B0 08").  Only the vertical offset goes through the display
 * scaling. */
void Window_PresentOffset(int x, int y)
{
	OsDc_t* dc;
	if(!gWindowAlive)
		return;
	dc = OS_WindowGetDc();
	Gfx_PresentAll(gGfx, dc, x, (int)(((int64_t)Gfx_ScaleFactor(gGfx) * y) >> 16));
	OS_WindowReleaseDc(dc);
}

/* Blit a bitmap to a device context at (x, y).  On a scaled display the
 * point is mapped into the window, the height is stretched by the scale
 * factor (the width never is: the display scaling is vertical only) and
 * the output is clipped to the mapped picture.  Shared by the child
 * windows and "B0 00". */
void Window_DcBlit(OsDc_t* dc, int x, int y, const Bmp_t* b)
{
	OsSurface_t s;
	Bmp_t scaled;
	int clip[4];
	int dstH = b->h;
	const int* clipPtr = NULL;

	s.pixels = b->pixels;
	s.pitch = b->pitch;
	s.width = b->w;
	s.height = b->h;
	s.bpp = ModeBits(b->mode);
	if(Gfx_GetScaledBmp(gGfx, &scaled))
	{
		int32_t pt[2], origin[2], far[2];
		Gfx_MapPoint(gGfx, pt, x, y, 0);
		x = pt[0];
		y = pt[1];
		dstH = (int)(((uint32_t)Gfx_ScaleFactor(gGfx) * (uint32_t)b->h) >> 16);
		// the clip region spans the mapped picture: (0, 0) .. (w - 1, h - 1), exclusive
		Gfx_MapPoint(gGfx, origin, 0, 0, 0);
		Gfx_MapPoint(gGfx, far, gScreenWidths[gSizeIndex] - 1, gScreenHeights[gSizeIndex] - 1, 0);
		clip[0] = origin[0];
		clip[1] = origin[1];
		clip[2] = far[0];
		clip[3] = far[1];
		clipPtr = clip;
	}
	OS_StretchToWindow(dc, x, y, b->w, dstH, &s, clipPtr);
}

// "B0 00": copy managed bitmap `bmp` straight to the window at (x, y), bypassing the compositor; 0 ok, 1 no window, 2 no bitmap.
int Window_BlitBitmap(int x, int y, int bmp)
{
	Bmp_t b;
	OsDc_t* dc;
	if(!gWindowAlive)
		return 1;
	if(!BmpMgr_GetInfo(gBmpMgr, &b, bmp))
		return 2;
	dc = OS_WindowGetDc();
	Window_DcBlit(dc, x, y, &b);
	OS_WindowReleaseDc(dc);
	return 0;
}

// Re-present the back buffer under the text entry control before it paints itself (its background is transparent).
void Present_EditPaint(void)
{
	Rect_t r;
	OsDc_t* dc;
	if(!gWindowAlive)
		return;
	Edit_GetRect(&r);
	dc = OS_WindowGetDc();
	Gfx_Present(gGfx, dc, &r, 1);
	OS_WindowReleaseDc(dc);
}

// Copy the whole back buffer to the window without rendering ("90 F1", the repaint on expose, the quake's end).
void Window_Repaint(void)
{
	if(!gWindowAlive)
		return;
	Gfx_PresentAllAt0(gGfx, NULL);
	OS_EditRefresh();
}

// blit everything (rects NULL) or the dirty rectangles; the text entry control is repainted over them
static void Present_Blit(int count, const Rect_t* rects)
{
	if(rects)
	{
		if(!gWindowAlive)
			return;
		Gfx_Present(gGfx, NULL, rects, count);
		OS_EditRefresh();
	}
	else
	{
		Window_Repaint();
	}
}

/* ---- the render-time profiler of 1.535 on ("80 06" / "80 07") --------------------------
 *
 * While it is on, every compositor pass is timed: the total, the number
 * of passes and the number of passes that took at least 96 % of a display
 * period.  "80 07" reports the count, the mean time in milliseconds, the
 * budget ratio and the squared slow-pass ratio, as percentages. */
static int gProfileOn;
static int64_t gProfileBudget; // 96 % of a display period, in nanoseconds
static int64_t gProfileTotal;  // the render time of all passes, nanoseconds
static uint32_t gProfilePasses;
static uint32_t gProfileSlow; // passes that took at least the budget
static int64_t gProfileStart; // the performance counter at the start of the pass

// "80 06": switch the profiler on (resetting its counts) or off.
void Profile_Enable(int on)
{
	int64_t freq = 1000000000; // OS_PerfCounterNs counts nanoseconds
	gProfileOn = on;
	if(!on)
		return;
	gProfileBudget = freq / 60 * 96 / 100; // 60 Hz assumed (the original measures the refresh rate)
	gProfileTotal = 0;
	gProfilePasses = gProfileSlow = 0;
}

// the start of a render pass; a host without a performance counter switches the profiler off
static void Profile_Begin(void)
{
	if(gProfileOn && !OS_PerfCounterNs(&gProfileStart))
		gProfileOn = 0;
}

// the end of a render pass: account its time
static void Profile_End(void)
{
	int64_t now;
	if(!gProfileOn || !OS_PerfCounterNs(&now))
		return;
	gProfileTotal += now - gProfileStart;
	if(now - gProfileStart >= gProfileBudget)
		gProfileSlow++;
	gProfilePasses++;
}

/* "80 07": a profiler figure.  mode 0 the number of passes, 1 the mean
 * milliseconds per pass, 2 the budget over the mean in percent (100 =
 * exactly one display period per pass), 3 the squared slow-pass ratio in
 * percent; 0 for any other mode or before a pass was timed. */
int32_t Profile_Query(int mode)
{
	double r;
	switch(mode)
	{
		case 0: return (int32_t)gProfilePasses;
		case 1: // the mean milliseconds per pass
			if(gProfilePasses == 0)
				return 0;
			r = (double)gProfileTotal / (1e9 * (double)gProfilePasses) * 1000.0;
			return BGI_Ftol(r);
		case 2: // the budget over the mean: 100 = exactly one period's worth
			if(gProfileTotal <= 0)
				return 0;
			r = (double)gProfileBudget * (double)gProfilePasses / (double)gProfileTotal * 100.0;
			return BGI_Ftol(r);
		case 3: // (slow / passes)^2 as a percentage
			if(gProfilePasses == 0)
				return 0;
			r = (double)gProfileSlow * (double)gProfileSlow / ((double)gProfilePasses * (double)gProfilePasses) * 100.0;
			return BGI_Ftol(r);
		default: return 0;
	}
}

// Render everything and copy the whole picture to the window (nothing is copied when the render failed).
void Present_Full(void)
{
	int ok;
	Profile_Begin();
	ok = Gfx_RenderAll(gGfx);
	Profile_End();
	if(ok)
		Present_Blit(1, NULL);
}

// Render the dirty rectangles and copy them (everything when the compositor reports -1 rectangles).
void Present_Dirty(void)
{
	static Rect_t rects[0x400]; // the compositor holds at most 0x400 dirty rectangles
	int count = 0, ok;
	Profile_Begin();
	ok = Gfx_Render(gGfx, &count, rects);
	Profile_End();
	if(!ok || count == 0)
		return;
	if(count == -1)
		Present_Blit(1, NULL);
	else
		Present_Blit(count, rects);
}

/* Once per pass: present when requested, allowed, enabled ("90 01") and
 * the frame interval has elapsed, keeping the next time on the interval
 * grid.  -1 when nothing was presented, 0 otherwise. */
int PresentFrame(void)
{
	uint32_t now;
	if(!gPresentPending || !gDisplayEnabled || !gPresentAllowed)
		return -1;
	now = GetTicks();
	if(gNextPresentTime > now)
		return -1;
	if(gPresentPendingFull)
		Present_Full();
	else
		Present_Dirty();
	gFrameCount++;
	gPresentPending = 0;
	gNextPresentTime += ((now - gNextPresentTime) / gFrameInterval + 1) * gFrameInterval;
	return 0;
}

/* The sleep that ends a pass of the main loop.  A pass that presented a
 * frame sleeps the half-millisecond idle wait; one that presented nothing
 * sleeps the "80 52" time (1.535 on) when the script set one, else the
 * idle wait as well. */
static int gIdleSleepMs; // "80 52": ms; 0 = the idle wait

void Display_SetIdleSleep(int ms)
{
	gIdleSleepMs = ms;
}

int Display_GetIdleSleep(void)
{
	return gIdleSleepMs;
}

/* `presented` is PresentFrame's result: -1 nothing presented, 0 a frame
 * presented.  A positive value (a present that waited for the vertical
 * blank in the original) sleeps nothing; PresentFrame never returns one
 * here. */
void Display_FrameIdle(int presented)
{
	if(presented > 0)
		return;
	if(presented < 0 && gIdleSleepMs != 0)
	{
		OS_IdleSleepMs(gIdleSleepMs);
		return;
	}
	Sys_Idle();
}

// "80 0B": the compositor's band size in pixels.
uint32_t Sys_GetGfxProp(void)
{
	return Gfx_GetBandPixels(gGfx);
}
