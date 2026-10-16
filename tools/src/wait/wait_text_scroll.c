/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wait_text_scroll.c - the scrolling of message text that is longer than
 *                      its window, for the paced rich output
 *                      (WaitTextRich of wait_text.c)
 *
 * An OpenBGI extension ("EF 08", docs/obgi.md).  The original lays a
 * message out, reveals it glyph by glyph and simply does not show what
 * lies below the text area.  With scrolling on, the output's nodes live in
 * the window's text document (src/gfx/window/window_doc.c) and this file
 * moves the view over it.  The functions here are called from wait_text.c
 * in place of the original steps; an output whose text fits its window
 * goes through them too and behaves as before.
 *
 * The life of a long message:
 *
 *   printing   Glyphs appear at the pace of the text speed.  When the
 *              next one is due but lies below the view, the view moves up
 *              one line (taking gTextScrollMs) and printing goes on.
 *   hurried    Input, the "instant" flag or a text speed of zero show
 *              everything at once - but only what the view has room for.
 *              If more text follows, the output pauses ("more"): the down
 *              arrow blinks, and input (or the auto-advance timer) turns
 *              the page.  After a page turn caused by input the text is
 *              printed at its pace again.
 *   at rest    During such a pause and at the end of the text, while the
 *              output waits for the click, the player can move the view:
 *              the mouse wheel, the up / down keys and the two arrow
 *              pictures scroll by one line.  Scrolling down past the end
 *              of what may be shown is the usual "go on" input.
 *
 * Without a wait at the end (the message is followed at once by something
 * else), when the output was told to finish, or while the skip key is
 * down, nobody is reading: the view jumps to the end of the text.
 *
 * The click-wait cursor of the original is used unchanged; it is hidden
 * while its place is scrolled out of view.  The arrows are window items 6
 * (up) and 7 (down): their pictures and places come from "EF 09"
 * (gTextArrows), by default a triangle in the text colour at the end of
 * the first and the last line (the right end; the left end for
 * right-to-left text).
 */
#include <string.h>

#include "bgi/gfx/text.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/input.h"
#include "bgi/sys.h"

#define VT(self)            ((const WaitTextVtbl_t*)(self)->w.vt)
#define SET_TIMER(self, ms) VT(self)->base.setTimer(&(self)->w, (uint32_t)(ms)) // the class's (pacing-aware) timer

#define ARROW_BLINK_MS      500 // the down arrow's blink while more text waits
#define ARROW_REPEAT_MS     70  // one line per this while an arrow is held down
#define ARROW_SLACK         4   // pixels around an arrow that still count as a click on it

// ---- small helpers -----------------------------------------------------------------------------

// a screen rectangle of the window becomes dirty in the compositor, at the window's sort key
static void DirtyRect(WaitTextOut_t* t, const Rect_t* r)
{
	Gfx_AddDirty(gTextGfx, t->win->obj.vt->sortKey(&t->win->obj), r);
}

static void DirtyTextArea(WaitTextOut_t* t)
{
	Rect_t r;
	Window_TextScreenRect(t->win, &r);
	DirtyRect(t, &r);
	Wait_MarkDirty(&t->w);
}

// every glyph of this output has been drawn
static int AllShown(WaitTextRich_t* w)
{
	RichChar_t* c;
	for(c = w->rich.head; c; c = c->next)
		if(!c->shown)
			return 0;
	return 1;
}

// the output is hurried: everything that has room appears at once
static int FastMode(WaitTextOut_t* t)
{
	return t->fast || t->instant || gRubyCharDelay == 0;
}

/* 1 while the output's nodes are in the window's document.  The document
 * is dropped when the text layer is cleared; an output that outlives that
 * (another thread cleared the window) has nothing left to show and goes on
 * as a plain, finished output. */
int WaitTextScroll_Active(WaitTextOut_t* t)
{
	WaitTextRich_t* w = (WaitTextRich_t*)t;
	if(t->w.vt != &WaitTextRich_Vtbl.base || !w->scrollOn)
		return 0;
	if(!t->win->doc || t->win->doc->serial != w->docSerial)
	{
		w->scrollOn = 0;
		w->rich.head = NULL;
		return 0;
	}
	return 1;
}

// ---- moving the view ---------------------------------------------------------------------------

// start moving the view to document position `to` over `ms` milliseconds (at once for 0)
static void ScrollStart(WaitTextRich_t* w, int to, int ms)
{
	Window_t* win = w->t.win;
	int max = Window_DocMaxScroll(win);
	if(to > max)
		to = max;
	if(to < 0)
		to = 0;
	w->scrollFrom = Window_DocScroll(win);
	w->scrollTo = to;
	w->scrollTime = (uint32_t)(ms > 0 ? ms : 0);
	w->scrollStart = GetTicks() | 1; // never 0, which means "no scroll running"
}

/* advance a running scroll to where it should be by now and repaint; the
 * click cursor is taken down while the text moves (the cursor animation
 * puts it back at its new place).  1 while the view is still moving. */
static int ScrollRun(WaitTextRich_t* w)
{
	WaitTextOut_t* t = &w->t;
	uint32_t elapsed;
	int y;

	if(!w->scrollStart)
		return 0;
	elapsed = GetTicks() - (w->scrollStart & ~1u);
	if(elapsed >= w->scrollTime)
	{
		y = w->scrollTo;
		w->scrollStart = 0;
	}
	else
		y = w->scrollFrom + (int)((int64_t)(w->scrollTo - w->scrollFrom) * (int64_t)elapsed / (int64_t)w->scrollTime);
	if(y != Window_DocScroll(t->win))
	{
		Rect_t r;
		if(Window_ItemRect(t->win, &r, 0))
		{
			DirtyRect(t, &r);
			Window_ItemEnable(t->win, 0, 0);
		}
		Window_DocSetScroll(t->win, y);
		DirtyTextArea(t);
		if(t->phase > 0 && AllShown(w))
			Wait_SetTimer(&t->w, 0); // the cursor animation redraws at the next step
	}
	return w->scrollStart != 0;
}

// ---- the arrows --------------------------------------------------------------------------------

// show or hide arrow `which` (0 up, 1 down) at transparency `level`; its picture is made when it is first shown
static void ArrowSet(WaitTextRich_t* w, int which, int show, int level)
{
	WaitTextOut_t* t = &w->t;
	Window_t* win = t->win;
	int item = which ? WIN_ITEM_ARROW_DOWN : WIN_ITEM_ARROW_UP;
	int bit = 1 << which;
	Rect_t r;

	if(!show)
	{
		if(!(w->arrowsOn & bit))
			return;
		if(Window_ItemRect(win, &r, item))
			DirtyRect(t, &r);
		Window_ItemEnable(win, item, 0);
		w->arrowsOn &= ~bit;
		Wait_MarkDirty(&t->w);
		return;
	}
	if(!(w->arrowsOn & bit))
	{
		const TextArrow_t* a = &gTextArrows[which];
		Bmp_t tmp;
		const Bmp_t* bmp = Text_ArrowBitmap(which, Window_GetFontSize(win), t->param, &tmp);
		Rect_t area;
		int x, y;
		Window_GetTextArea(win, &area);
		// by default at the end of the line, where the layout keeps room: the left end for right-to-left text
		x = a->x >= 0 ? a->x : gTextDirection == 1 ? area.l
												   : area.r + 1 - bmp->w;
		y = a->y >= 0 ? a->y : which ? area.b + 1 - bmp->h
									 : area.t;
		Window_ItemSetBitmap(win, item, bmp);
		Window_ItemSet(win, item, x, y, level);
		Window_ItemEnable(win, item, 1);
		BGI_Free(tmp.pixels);
		w->arrowsOn |= bit;
		w->arrowLevel[which] = level;
	}
	else if(w->arrowLevel[which] != level)
	{
		WinItem_t* it = &win->item[item];
		Window_ItemSet(win, item, it->x, it->y, level);
		w->arrowLevel[which] = level;
	}
	else
		return;
	if(Window_ItemRect(win, &r, item))
		DirtyRect(t, &r);
	Wait_MarkDirty(&t->w);
}

/* show the arrows that have somewhere to go.  They only appear while the
 * text stands still (a "more" pause, or the wait at the end): up when the
 * view is not at the top, down when text lies below it - blinking during
 * a pause, where "down" is also how the message goes on. */
static void ArrowsUpdate(WaitTextRich_t* w)
{
	WaitTextOut_t* t = &w->t;
	int atRest = w->morePause || (AllShown(w) && t->waitAtEnd && !t->finishNow);
	int limit = w->morePause ? w->pauseScroll : Window_DocMaxScroll(t->win);
	int downLevel = 0;

	if(w->morePause)
	{
		uint32_t now = GetTicks();
		if(now - w->arrowBlink >= ARROW_BLINK_MS)
		{
			w->arrowBlink = now;
			w->arrowPhase ^= 1;
		}
		downLevel = w->arrowPhase ? 0xa0 : 0;
	}
	ArrowSet(w, 0, atRest && w->scrollTo > 0, 0);
	ArrowSet(w, 1, atRest && (w->morePause || w->scrollTo < limit), downLevel);
}

// the arrow under the mouse pointer: 0 up, 1 down, -1 none (an arrow that is not shown cannot be hit)
static int ArrowHit(WaitTextRich_t* w)
{
	int pos[2], which;
	GetMouseClientPos(pos);
	for(which = 0; which < 2; which++)
	{
		Rect_t r;
		if(!(w->arrowsOn & (1 << which)))
			continue;
		if(!Window_ItemRect(w->t.win, &r, which ? WIN_ITEM_ARROW_DOWN : WIN_ITEM_ARROW_UP))
			continue;
		if(pos[0] >= r.l - ARROW_SLACK && pos[0] <= r.r + ARROW_SLACK && pos[1] >= r.t - ARROW_SLACK &&
			pos[1] <= r.b + ARROW_SLACK)
			return which;
	}
	return -1;
}

// ---- input -------------------------------------------------------------------------------------

/* Called by the poll with this pass's input in t->rawInput (everything)
 * and t->input (what would hurry or finish the output).  While the text
 * stands still the scroll input is taken out of t->input and moves the
 * view by a line instead: wheel up / the up key and a click on the up
 * arrow; wheel down / the down key and a click on the down arrow as long
 * as there is somewhere to go.  Holding the mouse button on an arrow
 * repeats.  While the text is still being printed all input hurries it, as
 * in the original. */
void WaitTextScroll_Input(WaitTextOut_t* t)
{
	WaitTextRich_t* w = (WaitTextRich_t*)t;
	Window_t* win = t->win;
	uint32_t raw = t->rawInput, now = GetTicks();
	int adv = Window_LineAdvance(win);
	int up = 0, down = 0, arrow, limit;

	if(!w->morePause && !AllShown(w))
		return;
	limit = w->morePause ? w->pauseScroll : Window_DocMaxScroll(win);
	if(!t->allowKeys)
		raw &= ~0x30c0u; // the wheel and the up / down keys are off
	arrow = ArrowHit(w);
	if((raw & 1) && arrow >= 0)
	{
		t->input &= ~1u;
		w->holdNext = now + 4 * ARROW_REPEAT_MS;
		if(arrow == 0)
			up = 1;
		else if(w->scrollTo < limit)
			down = 1;
		else
			t->input |= 1u; // the down arrow with nothing left to scroll to: go on
	}
	else if(arrow >= 0 && (Input_PollHeld(t->layerKey) & 1) && now >= w->holdNext)
	{
		w->holdNext = now + ARROW_REPEAT_MS;
		up = arrow == 0;
		down = arrow == 1;
	}
	if(raw & 0x1040u)
		up = 1;
	if((raw & 0x2080u) && w->scrollTo < limit)
	{
		down = 1;
		t->input &= ~0x2080u;
	}
	if(up && w->scrollTo > 0)
		ScrollStart(w, w->scrollTo - adv, gTextScrollMs);
	else if(down && w->scrollTo < limit)
		ScrollStart(w, w->scrollTo + adv > limit ? limit : w->scrollTo + adv, gTextScrollMs);
}

// 1 while text of this output lies below what the view shows: input then turns the page rather than ending the output
int WaitTextScroll_HasMore(WaitTextOut_t* t)
{
	WaitTextRich_t* w = (WaitTextRich_t*)t;
	RichChar_t* c;
	int y = Window_DocScroll(t->win);
	for(c = w->rich.head; c; c = c->next)
		if(!c->shown && !Window_DocFits(t->win, c, y))
			return 1;
	return 0;
}

// ---- printing ----------------------------------------------------------------------------------

/* The reveal of wait_text.c's RichShowStep with one more rule: a glyph
 * whose line reaches below the view is not drawn.  Its delay still counts
 * down, but stops at 1 - "due" - and once one glyph is due and held, the
 * ones behind it stop counting too, so that they do not all appear
 * together when the view has moved.  Returns 1 when printing is stuck:
 * every glyph that has room is shown and the next one is due (or the
 * output is hurried) but lies below the view. */
static int ShowStep(WaitTextRich_t* w)
{
	WaitTextOut_t* t = &w->t;
	Window_t* win = t->win;
	int fastMode = FastMode(t);
	int scrollY = Window_DocScroll(win);
	int blocked = 0, pending = 0, due = 0;
	uint32_t steps = 0;
	RichChar_t* c;
	Rect_t r;

	if(Wait_TimerExpired(&t->w) && !fastMode)
	{
		do
		{
			Wait_TimerAdd(&t->w, 1);
			steps++;
		} while(Wait_TimerExpired(&t->w));
	}
	while(steps > 0 || fastMode)
	{
		steps--;
		due = 0;
		for(c = w->rich.head; c; c = c->next)
			if(!c->shown && c->delay <= 1 && !Window_DocFits(win, c, scrollY))
			{
				due = 1;
				break;
			}
		for(c = w->rich.head; c; c = c->next)
		{
			if(c->shown)
				continue;
			if(!Window_DocFits(win, c, scrollY))
			{
				if(!due && !fastMode && c->delay > 1)
					c->delay--;
				continue;
			}
			if(c->delay != 0 && !fastMode)
			{
				if(--c->delay == 0)
				{
					c->fadeStep = 0;
					c->fadeSteps = gRubyFadeSteps;
				}
				continue;
			}
			if((uint32_t)c->fadeStep < (uint32_t)c->fadeSteps && !fastMode)
			{
				if(steps != 0)
				{
					c->fadeStep++;
					continue;
				}
				// the glyph's shape punched out of the layer, then blended in at the fade's level
				if(Window_DocDrawNode(win, &r, c, 0x40, 1))
				{
					Window_DocDrawNode(win, &r, c, 1, 0x100 - (int)(((uint32_t)c->fadeStep << 8) / (uint32_t)c->fadeSteps));
					DirtyRect(t, &r);
					Wait_MarkDirty(&t->w);
				}
				c->fadeStep++;
			}
			else
			{
				if(Window_DocDrawNode(win, &r, c, 0x40, 1))
				{
					Window_DocDrawNode(win, &r, c, 1, 0);
					DirtyRect(t, &r);
					Wait_MarkDirty(&t->w);
				}
				c->shown = 1;
			}
		}
		if(fastMode)
			break;
	}
	SET_TIMER(t, 0);

	due = 0;
	for(c = w->rich.head; c; c = c->next)
	{
		if(c->shown)
			continue;
		if(Window_DocFits(win, c, scrollY))
			pending = 1;
		else
		{
			blocked = 1;
			if(c->delay <= 1)
				due = 1;
		}
	}
	return blocked && !pending && (fastMode || due);
}

// nobody is reading: mark everything shown and put the end of the text in view
static void JumpToEnd(WaitTextRich_t* w)
{
	WaitTextOut_t* t = &w->t;
	RichChar_t* c;
	for(c = w->rich.head; c; c = c->next)
		c->shown = 1;
	w->scrollStart = 0;
	w->morePause = 0;
	w->scrollTo = Window_DocMaxScroll(t->win);
	if(w->scrollTo != Window_DocScroll(t->win))
		Window_DocSetScroll(t->win, w->scrollTo);
	else
		Window_DocRedraw(t->win);
	DirtyTextArea(t);
}

// the view is full and more text follows: wait for input (or the auto-advance time) before going on
static void PauseEnter(WaitTextRich_t* w)
{
	WaitTextOut_t* t = &w->t;
	w->morePause = 1;
	w->pauseScroll = w->scrollTo;
	w->moreUntil = gAutoAdvanceOn ? (GetTicks() + (uint32_t)gAutoAdvanceMs) | 1 : 0;
	w->arrowBlink = GetTicks();
	w->arrowPhase = 0;
	// the input that hurried the text is used up; only the skip key carries on
	t->input &= 0x80000000u;
	if(!t->input)
		WaitTextOut_SetFast(t, 0);
}

/* after a pause the glyphs that were held back still carry the delays of
 * the moment the text was hurried: take the wait before the first of them
 * off all of them, so that printing resumes at once and at its pace */
static void DelaysRebase(WaitTextRich_t* w)
{
	RichChar_t* c;
	int first = 0x7fffffff;
	for(c = w->rich.head; c; c = c->next)
		if(!c->shown && c->delay < first)
			first = c->delay;
	if(first == 0x7fffffff || first <= 1)
		return;
	for(c = w->rich.head; c; c = c->next)
		if(!c->shown)
			c->delay -= first - 1;
}

/* the pause is over: bring up the next page - as many lines as the view
 * holds, or less at the end of the text.  Unless the skip key is down the
 * text then prints at its pace again. */
static void PauseLeave(WaitTextRich_t* w)
{
	WaitTextOut_t* t = &w->t;
	int page = Window_LineCount(t->win) * Window_LineAdvance(t->win);
	w->morePause = 0;
	ScrollStart(w, w->pauseScroll + page, gTextScrollMs);
	DelaysRebase(w);
	t->input &= 0x80000000u;
	if(!t->input)
		WaitTextOut_SetFast(t, 0);
}

/* before the click cursor appears: give it a line of its own when it does
 * not fit behind the text (the original would start a line scroll of the
 * plain output here, which the rich classes never carry out), and bring
 * the cursor's line into view */
static void CursorPlace(WaitTextRich_t* w)
{
	Window_t* win = w->t.win;
	uint32_t maxW = 0;
	int i, max;

	for(i = 0; i < gTextItemCount; i++)
		if(maxW < (uint32_t)gTextItems[i].w)
			maxW = (uint32_t)gTextItems[i].w;
	if(gCursorPosMode != 1 && !Window_CursorFits(win, (int)maxW))
	{
		Rect_t area;
		int32_t cur[2];
		Window_GetTextArea(win, &area);
		Window_GetCursor(win, cur);
		Window_SetCursor(win, area.l + Text_GetIndent(), cur[1] + Window_LineAdvance(win));
	}
	max = Window_DocMaxScroll(win);
	if(max > w->scrollTo)
		ScrollStart(w, max, gTextScrollMs);
}

// the click cursor is not drawn outside the text area (its line is scrolled out of view)
static void CursorClip(WaitTextRich_t* w)
{
	WaitTextOut_t* t = &w->t;
	Window_t* win = t->win;
	WinItem_t* it = &win->item[0];
	Rect_t area, r;

	if(gCursorPosMode == 1 || !it->enabled || !it->bmp.pixels)
		return;
	Window_UsableArea(win, &area);
	if(it->y >= area.t && it->y + Window_GetFontSize(win) <= area.b + 1)
		return;
	if(Window_ItemRect(win, &r, 0))
		DirtyRect(t, &r);
	Window_ItemEnable(win, 0, 0);
	Wait_MarkDirty(&t->w);
}

/* The step of a scrolling output (in place of WaitTextRich_Step): move the
 * view, keep the arrows up to date, then print, pause, or wait at the end
 * as described at the top of this file.  1 when the wait is over. */
int WaitTextScroll_Step(WaitTextOut_t* t)
{
	WaitTextRich_t* w = (WaitTextRich_t*)t;
	int noPause = !t->waitAtEnd || t->finishNow || (t->input & 0x80000000u) != 0;
	int moving, r;

	moving = ScrollRun(w);
	ArrowsUpdate(w);
	if(!AllShown(w))
	{
		if(noPause && FastMode(t))
		{
			JumpToEnd(w);
			return 0;
		}
		if(moving)
			return 0; // printing waits for the view
		if(w->morePause)
		{
			if(t->input || (w->moreUntil && GetTicks() >= (w->moreUntil & ~1u)))
				PauseLeave(w);
			return 0;
		}
		if(!(Wait_TimerExpired(&t->w) || t->fast || t->finishNow || t->autoAdvanceAt != 0))
			return 0;
		if(ShowStep(w))
		{
			if(FastMode(t))
				PauseEnter(w);
			else
				ScrollStart(w, w->scrollTo + Window_LineAdvance(t->win), gTextScrollMs); // printing reached the bottom: the next line comes up
		}
		return 0;
	}

	// everything is shown: the wait at the end of the text
	if(!w->cursorPlaced)
	{
		w->cursorPlaced = 1;
		if(t->waitAtEnd && !t->finishNow)
			CursorPlace(w);
		moving = w->scrollStart != 0;
	}
	if(moving && !t->finishNow)
		return 0;
	if(!(Wait_TimerExpired(&t->w) || t->fast || t->finishNow || t->autoAdvanceAt != 0))
		return 0;
	r = WaitTextOut_EndOfText(t);
	if(!r)
		CursorClip(w);
	return r;
}

/* the output ends (its destructor): finish a running scroll where it was
 * going, take the arrows down, and let go of the nodes, which stay in the
 * window's document */
void WaitTextScroll_End(WaitTextOut_t* t)
{
	WaitTextRich_t* w = (WaitTextRich_t*)t;
	if(WaitTextScroll_Active(t))
	{
		if(w->scrollStart)
		{
			w->scrollStart = 0;
			Window_DocSetScroll(t->win, w->scrollTo);
			DirtyTextArea(t);
		}
		ArrowSet(w, 0, 0, 0);
		ArrowSet(w, 1, 0, 0);
	}
	w->rich.head = NULL;
	w->scrollOn = 0;
}
