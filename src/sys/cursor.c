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
#include "bgi/core.h"
#include <math.h>

// ---- position ---------------------------------------------------------

/*
 * The cursor in back-buffer coordinates: the pointer where the host says
 * it is, through the display scaling (GetCursorBufferPos).  (The original
 * reads the screen position and subtracts the window rectangle plus the
 * fixed frame and caption metrics in windowed mode; the host's position
 * is already the picture's.)  0, 0 when there is no window.  "80 08"
 * reports this.
 */
void GetMouseClientPos(OpenBGI_t* o, int xy[2])
{
	if(!o->display.windowAlive)
	{
		xy[0] = xy[1] = 0;
		return;
	}
	GetCursorBufferPos(o, xy);
}

// ---- scripted move ("80 1F") ------------------------------------------
/*
 * "80 1F": start moving the cursor to back-buffer position (x, y) over
 * `duration` ms in duration * fps / 1000 steps (at least one).  curve 1
 * eases in and out with a cosine, anything else is linear; with
 * cancelOnMove the move stops as soon as the user moves the mouse.
 * Nothing happens while the window is minimised.  The move itself is
 * carried out by Cursor_Update, one step per due tick; the menu waits use
 * it to glide the cursor onto the selected item.
 */
void MouseMove_Start(OpenBGI_t* o, int x, int y, int curve, int duration, int fps, int cancelOnMove)
{
	uint32_t steps;
	if(MainWin_Minimized(o))
		return;
	o->cursor.mvActive = 1;
	GetCursorBufferPos(o, o->cursor.mvStart);
	/* the original turns the target into screen coordinates (the client
	 * origin added) to move the system's cursor; the engine's pointer is
	 * in the picture's own */
	o->cursor.mvDelta[0] = x - o->cursor.mvStart[0];
	o->cursor.mvDelta[1] = y - o->cursor.mvStart[1];
	o->cursor.mvPos[0] = o->cursor.mvStart[0];
	o->cursor.mvPos[1] = o->cursor.mvStart[1];
	o->cursor.mvCurve = curve;
	steps = (uint32_t)(duration * fps) / 1000u;
	if(steps == 0)
		steps = 1;
	o->cursor.mvStep = 0;
	o->cursor.mvSteps = steps;
	o->cursor.mvDuration = (uint32_t)duration;
	o->cursor.mvStartTick = GetTicks(o);
	o->cursor.mvNextTick = o->cursor.mvStartTick + o->cursor.mvDuration / steps;
	o->cursor.mvCancelOnMove = cancelOnMove;
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

// show or hide the system cursor (the host's window keeps the show count)
static void CursorShowSys(OpenBGI_t* o, int show)
{
	MainWin_CursorShow(o, show);
}

/* "B0 05": hide the cursor after `ms` of rest over the picture; 0 switches
 * the auto-hide off (showing the cursor again if it was hidden), a later
 * non-zero value only changes the delay. */
void SetCursorAutoHide(OpenBGI_t* o, uint32_t ms)
{
	if(!o->cursor.hideEnabled)
	{
		if(ms == 0)
			return;
		o->cursor.hideEnabled = 1;
		o->cursor.hideShown = 1;
		o->cursor.hideDelay = ms;
		o->cursor.hideAt = GetTicks(o) + ms;
		o->cursor.hideLast[0] = o->cursor.hideLast[1] = (int)0x80000000;
	}
	else if(ms != 0)
	{
		o->cursor.hideDelay = ms;
		o->cursor.hideAt = GetTicks(o) + ms;
	}
	else
	{
		o->cursor.hideEnabled = 0;
		if(!o->cursor.hideShown)
			CursorShowSys(o, 1);
	}
}

/* "B0 06" (1.529 on): whether the cursor is visible - not hidden by the
 * auto-hide (the sprite cursor of "B0 04" counts as visible).  The 1.616
 * and later originals also ask Windows whether touch input suppressed the
 * cursor; there is no such state here. */
int Cursor_IsVisible(OpenBGI_t* o)
{
	return !o->cursor.hideEnabled || o->cursor.hideShown;
}

// Show an auto-hidden cursor again (before a dialog or message box).
void Cursor_Refresh(OpenBGI_t* o)
{
	if(o->cursor.hideEnabled && !o->cursor.hideShown)
		CursorShowSys(o, 1);
}

/* Once per pass: advance the scripted move (every step whose tick has
 * come; the move is abandoned when the window loses the focus or, with
 * cancelOnMove, when the user moved the mouse), then the auto-hide (the
 * cursor is hidden after resting `o->cursor.hideDelay` ms inside the
 * picture and shown again as soon as it moves, leaves the picture, or
 * the window is inactive or minimised). */
void Cursor_Update(OpenBGI_t* o)
{
	uint32_t now;
	if(o->cursor.mvActive)
	{
		if(!Window_IsActive(o))
		{
			o->cursor.mvActive = 0;
		}
		else
		{
			now = GetTicks(o);
			while(o->cursor.mvActive && o->cursor.mvNextTick <= now)
			{
				int pos[2];
				GetCursorBufferPos(o, pos);
				if((pos[0] != o->cursor.mvPos[0] || pos[1] != o->cursor.mvPos[1]) && o->cursor.mvCancelOnMove)
				{
					o->cursor.mvActive = 0; // the user took over
					break;
				}
				o->cursor.mvStep++;
				if(o->cursor.mvStep < o->cursor.mvSteps)
				{
					o->cursor.mvPos[0] = o->cursor.mvStart[0] + Move_Interp(o->cursor.mvDelta[0], o->cursor.mvCurve, o->cursor.mvStep, o->cursor.mvSteps);
					o->cursor.mvPos[1] = o->cursor.mvStart[1] + Move_Interp(o->cursor.mvDelta[1], o->cursor.mvCurve, o->cursor.mvStep, o->cursor.mvSteps);
					o->cursor.mvNextTick = o->cursor.mvStartTick + ((o->cursor.mvStep + 1) * o->cursor.mvDuration) / o->cursor.mvSteps;
				}
				else
				{
					o->cursor.mvPos[0] = o->cursor.mvStart[0] + o->cursor.mvDelta[0]; // the last step lands exactly on the target
					o->cursor.mvPos[1] = o->cursor.mvStart[1] + o->cursor.mvDelta[1];
					o->cursor.mvActive = 0;
				}
				SetCursorBufferPos(o, o->cursor.mvPos[0], o->cursor.mvPos[1]);
			}
		}
	}

	if(!o->cursor.hideEnabled)
		return;
	if(Window_IsActive(o))
	{
		int pos[2];
		GetMouseClientPos(o, pos);
		if(pos[0] == o->cursor.hideLast[0] && pos[1] == o->cursor.hideLast[1] &&
			pos[0] >= 0 && pos[0] < o->display.screenWidths[o->display.sizeIndex] &&
			pos[1] >= 0 && pos[1] < o->display.screenHeights[o->display.sizeIndex])
		{
			// resting inside the picture: hide after the delay
			if(MainWin_Minimized(o))
				goto reshow;
			if(o->cursor.hideShown && GetTicks(o) >= o->cursor.hideAt)
			{
				CursorShowSys(o, 0);
				o->cursor.hideShown = 0;
			}
			return;
		}
		o->cursor.hideLast[0] = pos[0];
		o->cursor.hideLast[1] = pos[1];
		if(o->cursor.hideShown)
		{
			o->cursor.hideAt = GetTicks(o) + o->cursor.hideDelay; // moved: the rest starts over
			return;
		}
		CursorShowSys(o, 1);
		o->cursor.hideShown = 1;
		o->cursor.hideAt = GetTicks(o) + o->cursor.hideDelay;
		return;
	}
reshow:
	if(!o->cursor.hideShown)
	{
		CursorShowSys(o, 1);
		o->cursor.hideShown = 1;
		o->cursor.hideAt = GetTicks(o) + o->cursor.hideDelay;
	}
}

// ---- system state -----------------------------------------------------

/* Show or hide the system cursor, changing the OS show count only on a
 * transition so that the engine's state never drifts from it; returns
 * the previous state. */
int ShowCursorCount(OpenBGI_t* o, int show)
{
	int old = o->cursor.cursorShown;
	if(show)
	{
		if(old)
			return old;
	}
	else if(!old)
	{
		return old;
	}
	o->cursor.cursorShown = show;
	MainWin_CursorShow(o, show);
	return old;
}

int Cursor_IsShown(OpenBGI_t* o)
{
	return o->cursor.cursorShown;
}

// ---- cursor sprite ("B0 04") ------------------------------------------
/* "B0 04": let sprite `sprite` follow the cursor at offset (dx, dy) and
 * hide the system cursor over the picture; 0 restores the system cursor.
 * 0 ok, -1 when the sprite cannot be shown. */
int SetCursorSprite(OpenBGI_t* o, uint32_t sprite, int dx, int dy)
{
	if(sprite)
	{
		if(!GfxCall_SpriteShow(o, sprite, 1))
			return -1;
		o->cursor.curSprite = sprite;
		o->cursor.curDx = dx;
		o->cursor.curDy = dy;
		CursorSprite_Update(o);
		return 0;
	}
	o->cursor.curSprite = 0;
	ShowCursorCount(o, 1);
	return 0;
}

/* Once per pass: move the cursor sprite to the cursor when it moved, the
 * system cursor hidden while the cursor is inside the picture and shown
 * outside it.  A sprite that can no longer be positioned (deleted) ends
 * the arrangement and restores the system cursor. */
void CursorSprite_Update(OpenBGI_t* o)
{
	int pos[2], inside;
	Rect_t rect;
	if(!o->cursor.curSprite)
		return;
	GetMouseClientPos(o, pos);
	if(pos[0] == o->cursor.curLast[0] && pos[1] == o->cursor.curLast[1])
		return;
	o->cursor.curLast[0] = pos[0];
	o->cursor.curLast[1] = pos[1];
	Gfx_GetScreenRect(o->gfx.gfx, &rect);
	inside = pos[0] >= rect.l && pos[0] <= rect.r && pos[1] >= rect.t && pos[1] <= rect.b;
	ShowCursorCount(o, !inside);
	if(!GfxCall_ObjSetPos(o, o->cursor.curSprite, pos[0] + o->cursor.curDx, pos[1] + o->cursor.curDy))
	{
		o->cursor.curSprite = 0;
		ShowCursorCount(o, 1);
	}
	Present_RequestDirty(o);
}
