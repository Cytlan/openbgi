/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * present.c - getting the back buffer onto the window; interface in
 *             bgi/display.h
 *
 * The compositor renders the display objects into the back buffer; this
 * file decides when the result is copied to the window's picture
 * (screen.c), which the host shows after the frame.  A present is
 * requested (whole picture or dirty rectangles) by whatever changed the
 * picture and carried out once per frame interval ("90 02") by
 * PresentFrame, which the scheduler calls every pass; "90 01" and the
 * screen quake can hold presenting off.  Also here: the direct blits that
 * bypass the compositor ("B0 00", the movie player's frames) and the
 * display scaling they and the child windows' pictures go through, what
 * an uncovered window shows, the render-time profiler ("80 06" / "80 07")
 * and the end-of-pass sleep ("80 52").
 */
#include "bgi/display.h"
#include "bgi/engine.h"
#include "bgi/gfx.h"
#include "bgi/sys.h"
#include "bgi/core.h"

// Allow or suspend presenting (the screen quake presents on its own while it runs).
void Present_Allow(OpenBGI_t* o, int on)
{
	o->present.presentAllowed = on;
}

int Present_IsAllowed(OpenBGI_t* o)
{
	return o->present.presentAllowed;
}

int Present_GetInterval(OpenBGI_t* o)
{
	return (int)o->present.frameInterval;
}

// "90 02": the frame rate; the interval between presents becomes 1000 / fps ms (at least 1).
void Present_SetRate(OpenBGI_t* o, int fps)
{
	if(fps > 0 && 1000u / (uint32_t)fps > 0)
		o->present.frameInterval = 1000u / (uint32_t)fps;
	else
		o->present.frameInterval = 1;
	o->present.nextPresentTime = 0;
}

// note a request; a full present subsumes the dirty ones
static void Present_Request(OpenBGI_t* o, int full)
{
	if(o->present.presentPending)
	{
		o->present.presentPendingFull |= full;
	}
	else
	{
		o->present.presentPending = 1;
		o->present.presentPendingFull = full;
	}
}

void Present_RequestFull(OpenBGI_t* o)
{
	Present_Request(o, 1);
}

void Present_RequestDirty(OpenBGI_t* o)
{
	Present_Request(o, 0);
}

/* Present the whole back buffer shifted by (x, y) - the screen quake's
 * frame ("B0 08").  Only the vertical offset goes through the display
 * scaling. */
void Window_PresentOffset(OpenBGI_t* o, int x, int y)
{
	if(!o->display.windowAlive)
		return;
	Gfx_PresentAll(o, o->gfx.gfx, x, (int)(((int64_t)Gfx_ScaleFactor(o->gfx.gfx) * y) >> 16));
}

/* Where a bitmap blitted at (x, y) lands in the window: on a scaled
 * display the point is mapped into the window, the height is stretched by
 * the scale factor (the width never is: the display scaling is vertical
 * only) and the output is clipped to the mapped picture.  0 when the
 * display is not scaled (the bitmap lands as it is).  The original does
 * the same for its child windows' pictures. */
int Window_MapBlit(OpenBGI_t* o, int* x, int* y, int* dstH, int clip[4])
{
	Bmp_t scaled;
	int32_t pt[2], origin[2], far[2];
	if(!o->gfx.gfx || !Gfx_GetScaledBmp(o->gfx.gfx, &scaled))
		return 0;
	Gfx_MapPoint(o->gfx.gfx, pt, *x, *y, 0);
	*x = pt[0];
	*y = pt[1];
	*dstH = (int)(((uint32_t)Gfx_ScaleFactor(o->gfx.gfx) * (uint32_t)*dstH) >> 16);
	// the clip region spans the mapped picture: (0, 0) .. (w - 1, h - 1), exclusive
	Gfx_MapPoint(o->gfx.gfx, origin, 0, 0, 0);
	Gfx_MapPoint(o->gfx.gfx, far, o->display.screenWidths[o->display.sizeIndex] - 1, o->display.screenHeights[o->display.sizeIndex] - 1, 0);
	clip[0] = origin[0];
	clip[1] = origin[1];
	clip[2] = far[0];
	clip[3] = far[1];
	return 1;
}

/* Blit a bitmap at (x, y) of the window's picture through the display
 * scaling ("B0 00", the movie player's frames). */
void Window_Blit(OpenBGI_t* o, int x, int y, const Bmp_t* b)
{
	int clip[4], dstH = b->h;
	int scaled = Window_MapBlit(o, &x, &y, &dstH, clip);
	Screen_Stretch(o, x, y, b->w, dstH, b, scaled ? clip : NULL);
}

// "B0 00": copy managed bitmap `bmp` straight to the window at (x, y), bypassing the compositor; 0 ok, 1 no window, 2 no bitmap.
int Window_BlitBitmap(OpenBGI_t* o, int x, int y, int bmp)
{
	Bmp_t b;
	if(!o->display.windowAlive)
		return 1;
	if(!BmpMgr_GetInfo(o->gfx.bmpMgr, &b, bmp))
		return 2;
	Window_Blit(o, x, y, &b);
	return 0;
}

/* What the window shows when it is uncovered (OpenBGI_Frame_t.expose), as
 * the original's WM_PAINT has it: nothing while presenting is disabled or
 * suspended, black while the engine is not up yet or a movie draws over
 * the window, else the picture. */
int Present_Expose(OpenBGI_t* o)
{
	if(!o->display.displayEnabled || !Present_IsAllowed(o))
		return OPENBGI_EXPOSE_NONE;
	return o->engine.engineUp && !o->movie.moviePlaying ? OPENBGI_EXPOSE_PICTURE : OPENBGI_EXPOSE_BLACK;
}

// Copy the whole back buffer to the window without rendering ("90 F1", the quake's end, the end of a movie).
void Window_Repaint(OpenBGI_t* o)
{
	if(!o->display.windowAlive)
		return;
	Gfx_PresentAllAt0(o, o->gfx.gfx);
}

// copy everything (rects NULL) or the dirty rectangles to the window's picture
static void Present_Blit(OpenBGI_t* o, int count, const Rect_t* rects)
{
	if(rects)
	{
		if(!o->display.windowAlive)
			return;
		Gfx_Present(o, o->gfx.gfx, rects, count);
	}
	else
	{
		Window_Repaint(o);
	}
}

/* ---- the render-time profiler of 1.535 on ("80 06" / "80 07") --------------------------
 *
 * While it is on, every compositor pass is timed: the total, the number
 * of passes and the number of passes that took at least 96 % of a display
 * period.  "80 07" reports the count, the mean time in milliseconds, the
 * budget ratio and the squared slow-pass ratio, as percentages. */

// "80 06": switch the profiler on (resetting its counts) or off.
void Profile_Enable(OpenBGI_t* o, int on)
{
	int64_t freq = 1000000000; // the host's counter counts nanoseconds
	o->present.profileOn = on;
	if(!on)
		return;
	o->present.profileBudget = freq / 60 * 96 / 100; // 60 Hz assumed (the original measures the refresh rate)
	o->present.profileTotal = 0;
	o->present.profilePasses = o->present.profileSlow = 0;
}

// the start of a render pass; a host without a performance counter switches the profiler off
static void Profile_Begin(OpenBGI_t* o)
{
	if(o->present.profileOn && !Clock_PerfNs(o, &o->present.profileStart))
		o->present.profileOn = 0;
}

// the end of a render pass: account its time
static void Profile_End(OpenBGI_t* o)
{
	int64_t now;
	if(!o->present.profileOn || !Clock_PerfNs(o, &now))
		return;
	o->present.profileTotal += now - o->present.profileStart;
	if(now - o->present.profileStart >= o->present.profileBudget)
		o->present.profileSlow++;
	o->present.profilePasses++;
}

/* "80 07": a profiler figure.  mode 0 the number of passes, 1 the mean
 * milliseconds per pass, 2 the budget over the mean in percent (100 =
 * exactly one display period per pass), 3 the squared slow-pass ratio in
 * percent; 0 for any other mode or before a pass was timed. */
int32_t Profile_Query(OpenBGI_t* o, int mode)
{
	double r;
	switch(mode)
	{
		case 0: return (int32_t)o->present.profilePasses;
		case 1: // the mean milliseconds per pass
			if(o->present.profilePasses == 0)
				return 0;
			r = (double)o->present.profileTotal / (1e9 * (double)o->present.profilePasses) * 1000.0;
			return BGI_Ftol(r);
		case 2: // the budget over the mean: 100 = exactly one period's worth
			if(o->present.profileTotal <= 0)
				return 0;
			r = (double)o->present.profileBudget * (double)o->present.profilePasses / (double)o->present.profileTotal * 100.0;
			return BGI_Ftol(r);
		case 3: // (slow / passes)^2 as a percentage
			if(o->present.profilePasses == 0)
				return 0;
			r = (double)o->present.profileSlow * (double)o->present.profileSlow / ((double)o->present.profilePasses * (double)o->present.profilePasses) * 100.0;
			return BGI_Ftol(r);
		default: return 0;
	}
}

// Render everything and copy the whole picture to the window (nothing is copied when the render failed).
void Present_Full(OpenBGI_t* o)
{
	int ok;
	Profile_Begin(o);
	ok = Gfx_RenderAll(o, o->gfx.gfx);
	Profile_End(o);
	if(ok)
		Present_Blit(o, 1, NULL);
}

// Render the dirty rectangles and copy them (everything when the compositor reports -1 rectangles).
void Present_Dirty(OpenBGI_t* o)
{
	Rect_t* rects = o->present.dirtyRects;
	int count = 0, ok;
	Profile_Begin(o);
	ok = Gfx_Render(o, o->gfx.gfx, &count, rects);
	Profile_End(o);
	if(!ok || count == 0)
		return;
	if(count == -1)
		Present_Blit(o, 1, NULL);
	else
		Present_Blit(o, count, rects);
}

/* OpenBGI_SetDrawing: with painting off the presents go by without the
 * compositor (the picture of the window stays what it was); the first
 * present after it is on again paints everything, and one is asked for at
 * once. */
void Present_SetDrawing(OpenBGI_t* o, int on)
{
	if(on && o->present.drawOff)
	{
		Present_RequestFull(o);
		o->present.nextPresentTime = 0;
	}
	o->present.drawOff = !on;
}

/* The back buffer as the objects show it now, for an instruction that
 * reads it ("90 04") while painting is off: painted once, out of turn. */
void Present_Freshen(OpenBGI_t* o)
{
	if(o->present.drawStale && o->gfx.gfx && Gfx_RenderAll(o, o->gfx.gfx))
		o->present.drawStale = 0;
}

/* Once per pass: present when requested, allowed, enabled ("90 01") and
 * the frame interval has elapsed, keeping the next time on the interval
 * grid.  -1 when nothing was presented, 0 otherwise. */
int PresentFrame(OpenBGI_t* o)
{
	uint32_t now;
	if(!o->present.presentPending || !o->display.displayEnabled || !o->present.presentAllowed)
		return -1;
	now = GetTicks(o);
	if(o->present.nextPresentTime > now)
		return -1;
	if(o->present.drawOff)
		o->present.drawStale = 1; // (the frame counts as presented: the scripts go on as they would)
	else if(o->present.presentPendingFull || o->present.drawStale)
	{
		Present_Full(o);
		o->present.drawStale = 0;
	}
	else
		Present_Dirty(o);
	o->present.frameCount++;
	o->present.presentPending = 0;
	o->present.nextPresentTime += ((now - o->present.nextPresentTime) / o->present.frameInterval + 1) * o->present.frameInterval;
	return 0;
}

/* The sleep that ends a pass of the main loop.  A pass that presented a
 * frame sleeps the half-millisecond idle wait; one that presented nothing
 * sleeps the "80 52" time (1.535 on) when the script set one, else the
 * idle wait as well. */
void Display_SetIdleSleep(OpenBGI_t* o, int ms)
{
	o->present.idleSleepMs = ms;
}

/* The host's wait after a frame (OpenBGI_Frame_t.idleMs): 0 for the
 * half-millisecond idle wait, else the "80 52" time for a frame that
 * presented nothing.  `presented` is PresentFrame's result: -1 nothing
 * presented, 0 a frame presented.  The original sleeps nothing after a
 * positive result (a present that waited for the vertical blank);
 * PresentFrame never returns one here. */
int Display_IdleMs(OpenBGI_t* o, int presented)
{
	if(presented < 0 && o->present.idleSleepMs != 0)
		return o->present.idleSleepMs;
	return 0;
}

// "80 0B": the compositor's band size in pixels.
uint32_t Sys_GetGfxProp(OpenBGI_t* o)
{
	return Gfx_GetBandPixels(o->gfx.gfx);
}
