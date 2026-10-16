/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cursor.c - the mouse cursor: position queries in back-buffer
 *            coordinates, the scripted cursor move ("80 1F"), auto-hide
 *            ("B0 05"), the sprite that replaces the system cursor
 *            ("B0 04") and the engine's own notion of whether the system
 *            cursor is shown; interface in bgi/input.h
 *
 * Positions the scripts see are in back-buffer coordinates: the screen
 * position mapped through the display scaling (display.c) and relative
 * to the client origin.  The scripted move and the auto-hide are advanced
 * once per pass by Cursor_Update, the cursor sprite by CursorSprite_Update
 * (both from the scheduler's frame services).
 */
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/gfx.h"
#include "bgi/sys.h"
#include "bgi/os.h"
#include <math.h>

int gCursorShape; // "80 67": 0 the arrow, 1 the resource cursor, 2 .. 4 the class arrow

// ---- position ---------------------------------------------------------

/*
 * The cursor in back-buffer coordinates: the screen position mapped
 * through the display scaling (GetCursorBufferPos) minus the client
 * origin.  (The original subtracts the window rectangle plus the fixed
 * frame and caption metrics in windowed mode.)  0, 0 when there is no
 * window.  "80 08" reports this.
 */
void GetMouseClientPos(int xy[2])
{
	int ox = 0, oy = 0;
	if(!gWindowAlive)
	{
		xy[0] = xy[1] = 0;
		return;
	}
	OS_ClientToScreen(&ox, &oy); // the client origin (GetWindowRect + frame in the original)
	GetCursorBufferPos(xy);
	xy[0] -= ox;
	xy[1] -= oy;
}

// the inverse of GetMouseClientPos (screen coordinates); `out` is left alone without a window
void ClientToScreenPos(int out[2], const int in[2])
{
	int ox = 0, oy = 0;
	if(!gWindowAlive)
		return;
	OS_ClientToScreen(&ox, &oy);
	out[0] = in[0] + ox;
	out[1] = in[1] + oy;
}

// ---- scripted move ("80 1F") ------------------------------------------
static int gMvActive;         // a move is in progress
static int gMvStart[2];       // start (screen)
static int gMvDelta[2];       // target - start (screen)
static int gMvPos[2];         // last position set
static int gMvCurve;          // 1 = cosine ease, else linear
static uint32_t gMvStep;      // steps done
static uint32_t gMvSteps;     // steps in all
static uint32_t gMvDuration;  // ms
static uint32_t gMvStartTick; // GetTicks at the start
static uint32_t gMvNextTick;  // when the next step is due
static int gMvCancelOnMove;   // stop when the user moves the mouse

/*
 * "80 1F": start moving the cursor to back-buffer position (x, y) over
 * `duration` ms in duration * fps / 1000 steps (at least one).  curve 1
 * eases in and out with a cosine, anything else is linear; with
 * cancelOnMove the move stops as soon as the user moves the mouse.
 * Nothing happens while the window is minimised.  The move itself is
 * carried out by Cursor_Update, one step per due tick; the menu waits use
 * it to glide the cursor onto the selected item.
 */
void MouseMove_Start(int x, int y, int curve, int duration, int fps, int cancelOnMove)
{
	int target[2], in[2];
	uint32_t steps;
	if(OS_WindowMinimized())
		return;
	gMvActive = 1;
	GetCursorBufferPos(gMvStart);
	in[0] = x;
	in[1] = y;
	target[0] = x; // ClientToScreenPos leaves the target alone when the window
	target[1] = y; // is gone; the original reads it uninitialised then
	ClientToScreenPos(target, in);
	gMvDelta[0] = target[0] - gMvStart[0];
	gMvDelta[1] = target[1] - gMvStart[1];
	gMvPos[0] = gMvStart[0];
	gMvPos[1] = gMvStart[1];
	gMvCurve = curve;
	steps = (uint32_t)(duration * fps) / 1000u;
	if(steps == 0)
		steps = 1;
	gMvStep = 0;
	gMvSteps = steps;
	gMvDuration = (uint32_t)duration;
	gMvStartTick = GetTicks();
	gMvNextTick = gMvStartTick + gMvDuration / steps;
	gMvCancelOnMove = cancelOnMove;
}

// the offset after `step` of `steps` along `delta`, linear or cosine eased
static int Move_Interp(int delta, int curve, uint32_t step, uint32_t steps)
{
	if(curve != 1)
		return (int)(((int64_t)((step << 16) / steps) * delta) >> 16);
	{
		// the angle runs from 180 degrees down to 0 in 1/256 degree units (0xb400 = 180 * 256);
		// (cos + 1) / 2 then goes from 0 to 1, slow at both ends, as a 16.16 fraction of delta
		uint32_t angle = 0xb400u - (step * 45u * 1024u) / steps;
		double f = (cos((double)angle * (3.14159265358979323846 / 180.0 / 256.0)) + 1.0) * 0.5 * 65536.0;
		return (int)(((int64_t)(int32_t)f * delta) >> 16);
	}
}

// ---- auto-hide ("B0 05") ----------------------------------------------
static int gHideEnabled;    // auto-hide is on
static int gHideShown;      // the cursor is currently shown by the auto-hide
static uint32_t gHideDelay; // ms of rest before the cursor is hidden
static uint32_t gHideAt;    // the tick at which it will be hidden
static int gHideLast[2];    // the position last seen (0x80000000: none yet)

// show or hide the system cursor (the OS layer keeps the show count)
static void CursorShowSys(int show)
{
	OS_CursorShow(show);
}

/* "B0 05": hide the cursor after `ms` of rest over the picture; 0 switches
 * the auto-hide off (showing the cursor again if it was hidden), a later
 * non-zero value only changes the delay. */
void SetCursorAutoHide(uint32_t ms)
{
	if(!gHideEnabled)
	{
		if(ms == 0)
			return;
		gHideEnabled = 1;
		gHideShown = 1;
		gHideDelay = ms;
		gHideAt = GetTicks() + ms;
		gHideLast[0] = gHideLast[1] = (int)0x80000000;
	}
	else if(ms != 0)
	{
		gHideDelay = ms;
		gHideAt = GetTicks() + ms;
	}
	else
	{
		gHideEnabled = 0;
		if(!gHideShown)
			CursorShowSys(1);
	}
}

/* "B0 06" (1.529 on): whether the cursor is visible - not hidden by the
 * auto-hide (the sprite cursor of "B0 04" counts as visible).  The 1.616
 * and later originals also ask Windows whether touch input suppressed the
 * cursor; there is no such state here. */
int Cursor_IsVisible(void)
{
	return !gHideEnabled || gHideShown;
}

// Show an auto-hidden cursor again (before a dialog or message box).
void Cursor_Refresh(void)
{
	if(gHideEnabled && !gHideShown)
		CursorShowSys(1);
}

/* Once per pass: advance the scripted move (every step whose tick has
 * come; the move is abandoned when the window loses the focus or, with
 * cancelOnMove, when the user moved the mouse), then the auto-hide (the
 * cursor is hidden after resting `gHideDelay` ms inside the picture and
 * shown again as soon as it moves, leaves the picture, or the window is
 * inactive or minimised). */
void Cursor_Update(void)
{
	uint32_t now;
	if(gMvActive)
	{
		if(!Window_IsActive())
		{
			gMvActive = 0;
		}
		else
		{
			now = GetTicks();
			while(gMvActive && gMvNextTick <= now)
			{
				int pos[2];
				GetCursorBufferPos(pos);
				if((pos[0] != gMvPos[0] || pos[1] != gMvPos[1]) && gMvCancelOnMove)
				{
					gMvActive = 0; // the user took over
					break;
				}
				gMvStep++;
				if(gMvStep < gMvSteps)
				{
					gMvPos[0] = gMvStart[0] + Move_Interp(gMvDelta[0], gMvCurve, gMvStep, gMvSteps);
					gMvPos[1] = gMvStart[1] + Move_Interp(gMvDelta[1], gMvCurve, gMvStep, gMvSteps);
					gMvNextTick = gMvStartTick + ((gMvStep + 1) * gMvDuration) / gMvSteps;
				}
				else
				{
					gMvPos[0] = gMvStart[0] + gMvDelta[0]; // the last step lands exactly on the target
					gMvPos[1] = gMvStart[1] + gMvDelta[1];
					gMvActive = 0;
				}
				SetCursorBufferPos(gMvPos[0], gMvPos[1]);
			}
		}
	}

	if(!gHideEnabled)
		return;
	if(Window_IsActive())
	{
		int pos[2];
		GetMouseClientPos(pos);
		if(pos[0] == gHideLast[0] && pos[1] == gHideLast[1] &&
			pos[0] >= 0 && pos[0] < gScreenWidths[gSizeIndex] &&
			pos[1] >= 0 && pos[1] < gScreenHeights[gSizeIndex])
		{
			// resting inside the picture: hide after the delay
			if(OS_WindowMinimized())
				goto reshow;
			if(gHideShown && GetTicks() >= gHideAt)
			{
				CursorShowSys(0);
				gHideShown = 0;
			}
			return;
		}
		gHideLast[0] = pos[0];
		gHideLast[1] = pos[1];
		if(gHideShown)
		{
			gHideAt = GetTicks() + gHideDelay; // moved: the rest starts over
			return;
		}
		CursorShowSys(1);
		gHideShown = 1;
		gHideAt = GetTicks() + gHideDelay;
		return;
	}
reshow:
	if(!gHideShown)
	{
		CursorShowSys(1);
		gHideShown = 1;
		gHideAt = GetTicks() + gHideDelay;
	}
}

// ---- system state -----------------------------------------------------
static int gSysSwap; // the system's SwapMouseButton setting, as probed at start-up

// Cache whether the system has the mouse buttons swapped (start-up).
void Mouse_ProbeSwap(void)
{
	gSysSwap = OS_MouseButtonsSwapped();
}

int Mouse_SysSwapped(void)
{
	return gSysSwap;
}

static int gCursorShown = 1; // the engine's own ShowCursor state

/* Show or hide the system cursor, changing the OS show count only on a
 * transition so that the engine's state never drifts from it; returns
 * the previous state. */
int ShowCursorCount(int show)
{
	int old = gCursorShown;
	if(show)
	{
		if(old)
			return old;
	}
	else if(!old)
	{
		return old;
	}
	gCursorShown = show;
	OS_CursorShow(show);
	return old;
}

int Cursor_IsShown(void)
{
	return gCursorShown;
}

// ---- cursor sprite ("B0 04") ------------------------------------------
static uint32_t gCurSprite; // sprite id, 0 = none
static int gCurDx, gCurDy;  // the sprite's offset from the cursor position, pixels
static int gCurLast[2];     // the cursor position the sprite was last placed at

/* "B0 04": let sprite `sprite` follow the cursor at offset (dx, dy) and
 * hide the system cursor over the picture; 0 restores the system cursor.
 * 0 ok, -1 when the sprite cannot be shown. */
int SetCursorSprite(uint32_t sprite, int dx, int dy)
{
	if(sprite)
	{
		if(!GfxCall_SpriteShow(sprite, 1))
			return -1;
		gCurSprite = sprite;
		gCurDx = dx;
		gCurDy = dy;
		CursorSprite_Update();
		return 0;
	}
	gCurSprite = 0;
	ShowCursorCount(1);
	return 0;
}

/* Once per pass: move the cursor sprite to the cursor when it moved, the
 * system cursor hidden while the cursor is inside the picture and shown
 * outside it.  A sprite that can no longer be positioned (deleted) ends
 * the arrangement and restores the system cursor. */
void CursorSprite_Update(void)
{
	int pos[2], inside;
	Rect_t rect;
	if(!gCurSprite)
		return;
	GetMouseClientPos(pos);
	if(pos[0] == gCurLast[0] && pos[1] == gCurLast[1])
		return;
	gCurLast[0] = pos[0];
	gCurLast[1] = pos[1];
	Gfx_GetScreenRect(gGfx, &rect);
	inside = pos[0] >= rect.l && pos[0] <= rect.r && pos[1] >= rect.t && pos[1] <= rect.b;
	ShowCursorCount(!inside);
	if(!GfxCall_ObjSetPos(gCurSprite, pos[0] + gCurDx, pos[1] + gCurDy))
	{
		gCurSprite = 0;
		ShowCursorCount(1);
	}
	Present_RequestDirty();
}
