/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wait_text.c - the text output wait classes
 *
 * Interface: gfx/text.h (the classes, their vtable and the step functions)
 * and waitobj.h (the StartTextOut starters).
 *
 *   WaitTextOut    plain, one character per step ("90 90")
 *   WaitTextRich   pre-laid-out, revealed with per-character delay and
 *                  fade ("91 90", "91 92", "92 90")
 *   WaitTextRichV  the same for vertical writing (a window with draw style 1)
 *
 * OpenBGI adds one thing the original does not have: with "EF 08" a
 * horizontal WaitTextRich whose text is longer than the window scrolls.
 * That lives in wait_text_scroll.c; the hooks here are marked "(OpenBGI)".
 *
 * A text wait owns the window's text layer while it runs: it pushes an input
 * layer (so clicks and keys reach it), draws into the window, marks the
 * touched rectangles dirty, and finishes when the string is exhausted (and,
 * with waitAtEnd, after the cursor animation saw input or the auto-advance
 * timer ran out).  When the wait ends it pushes one value: whether input
 * made the text finish early (the `fast` flag; 1 / 0).  "80 4C"
 * notifications can finish it from another thread (WaitTextOut_OnMessage).
 *
 * Pacing goes through the virtual setTimer: the base Wait timer is only armed
 * while the output is not in "fast" or "instant" mode (scrolling is always
 * timed), and step() runs as many steps as the timer allows per poll.  The
 * global settings the pacing reads (o->text.charInterval,
 * o->text.scrollSteps, o->text.fadeSteps, o->text.autoAdvance*, o->text.startDelay*, the
 * cursor pictures) are declared in gfx/text.h and set by the "90 94" ..
 * "90 9F" instructions.
 */
#include <string.h>

#include "bgi/gfx/text.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/strutil.h"
#include "bgi/input.h"
#include "bgi/inputmap.h"
#include "bgi/sys.h"
#include "bgi/core.h"

// ---- forward declarations of the vtable entries -----------------------------------------
static void WaitTextOut_Destroy(OpenBGI_t* o, Wait_t* w);
static int WaitTextOut_Poll(OpenBGI_t* o, Wait_t* w);
static void WaitTextOut_SetTimer(OpenBGI_t* o, Wait_t* w, uint32_t ms);
int WaitTextOut_IsDirty(OpenBGI_t* o, Wait_t* w); // shared with the menu classes (waitobj.h)
static void WaitTextOut_OnMessage(OpenBGI_t* o, Wait_t* w, WaitMsg_t* m);
static void WaitTextOut_SetText(OpenBGI_t* o, WaitTextOut_t* w, const char* p);
static void WaitTextOut_SetParam(OpenBGI_t* o, WaitTextOut_t* w, uint32_t v);
static int WaitTextOut_Step(OpenBGI_t* o, WaitTextOut_t* w);
static int WaitTextOut_PageBreak(OpenBGI_t* o, WaitTextOut_t* w);
static void WaitTextOut_CursorNewLine(OpenBGI_t* o, WaitTextOut_t* w);
static void WaitTextOut_CursorOffset(OpenBGI_t* o, WaitTextOut_t* w, int32_t out[2]);

static void WaitTextRich_Destroy(OpenBGI_t* o, Wait_t* w);
static void WaitTextRich_SetText(OpenBGI_t* o, WaitTextOut_t* w, const char* p);
static void WaitTextRich_SetParam(OpenBGI_t* o, WaitTextOut_t* w, uint32_t v);
static int WaitTextRich_Step(OpenBGI_t* o, WaitTextOut_t* w);
static void WaitTextRich_CursorNewLine(OpenBGI_t* o, WaitTextOut_t* w);
static void WaitTextRich_CursorOffset(OpenBGI_t* o, WaitTextOut_t* w, int32_t out[2]);
static int WaitTextRich_LayoutCore(OpenBGI_t* o, WaitTextOut_t* w, RichState_t* rich, int32_t* outLines, const char* str, int rubyOn,
	RubyDict_t* dict, int32_t cur[2], const Rect_t* area, int adv, int font, int proportional,
	int hang, uint32_t colour, const int32_t style[5]);
static int WaitTextRich_RubyPlace(OpenBGI_t* o, WaitTextOut_t* w, RichState_t* rich, int font, uint32_t colour,
	const int32_t style[5], RubyDict_t* dict);
static void WaitTextRich_Align(OpenBGI_t* o, WaitTextOut_t* w, RichState_t* rich, int32_t cur[2], const Rect_t* area, int font,
	int hang, const int32_t style[5], int swing);

static void WaitTextRichV_Destroy(OpenBGI_t* o, Wait_t* w);
static void WaitTextRichV_CursorOffset(OpenBGI_t* o, WaitTextOut_t* w, int32_t out[2]);
static int WaitTextRichV_LayoutCore(OpenBGI_t* o, WaitTextOut_t* w, RichState_t* rich, int32_t* outLines, const char* str,
	int rubyOn, RubyDict_t* dict, int32_t cur[2], const Rect_t* area, int adv, int font, int proportional,
	int hang, uint32_t colour, const int32_t style[5]);
static int WaitTextRichV_RubyPlace(OpenBGI_t* o, WaitTextOut_t* w, RichState_t* rich, int font, uint32_t colour,
	const int32_t style[5], RubyDict_t* dict);
static void WaitTextRichV_Align(OpenBGI_t* o, WaitTextOut_t* w, RichState_t* rich, int32_t cur[2], const Rect_t* area, int font,
	int hang, const int32_t style[5], int swing);

const WaitTextVtbl_t WaitTextOut_Vtbl = {
	{WaitTextOut_Destroy, WaitTextOut_Poll, WaitTextOut_SetTimer, WaitTextOut_IsDirty, WaitTextOut_OnMessage},
	WaitTextOut_SetText,
	WaitTextOut_SetParam,
	WaitTextOut_Step,
	WaitTextOut_PageBreak,
	WaitTextOut_CursorNewLine,
	WaitTextOut_CursorOffset,
	NULL,
	NULL,
	NULL,
};

const WaitTextVtbl_t WaitTextRich_Vtbl = {
	{WaitTextRich_Destroy, WaitTextOut_Poll, WaitTextOut_SetTimer, WaitTextOut_IsDirty, WaitTextOut_OnMessage},
	WaitTextRich_SetText,
	WaitTextRich_SetParam,
	WaitTextRich_Step,
	WaitTextOut_PageBreak,
	WaitTextRich_CursorNewLine,
	WaitTextRich_CursorOffset,
	WaitTextRich_LayoutCore,
	WaitTextRich_RubyPlace,
	WaitTextRich_Align,
};

const WaitTextVtbl_t WaitTextRichV_Vtbl = {
	{WaitTextRichV_Destroy, WaitTextOut_Poll, WaitTextOut_SetTimer, WaitTextOut_IsDirty, WaitTextOut_OnMessage},
	WaitTextRich_SetText,
	WaitTextRich_SetParam,
	WaitTextRich_Step,
	WaitTextOut_PageBreak,
	WaitTextRich_CursorNewLine,
	WaitTextRichV_CursorOffset,
	WaitTextRichV_LayoutCore,
	WaitTextRichV_RubyPlace,
	WaitTextRichV_Align,
};

#define VT(self)               ((const WaitTextVtbl_t*)(self)->w.vt)
#define SET_TIMER(o, self, ms) VT(self)->base.setTimer(o, &(self)->w, (uint32_t)(ms)) // the class's (pacing-aware) timer

// a screen rectangle of the window becomes dirty in the compositor, at the window's sort key
static void DirtyRect(OpenBGI_t* o, WaitTextOut_t* w, const Rect_t* r)
{
	Gfx_AddDirty(o->text.textGfx, w->win->obj.vt->sortKey(o, &w->win->obj), r);
}

// the window's whole text area
static void DirtyTextArea(OpenBGI_t* o, WaitTextOut_t* w)
{
	Rect_t r;
	Window_TextScreenRect(o, w->win, &r);
	DirtyRect(o, w, &r);
}

// the cursor item (window item 0), when it has a rectangle
static void DirtyItem0(OpenBGI_t* o, WaitTextOut_t* w)
{
	Rect_t r;
	if(Window_ItemRect(o, w->win, &r, 0))
		DirtyRect(o, w, &r);
}

// ---- WaitTextOut -----------------------------------------------------------------------------

/* the shared constructor: takes the input layers, resets the window's text
 * layer to opaque and visible, switches the item layers off and starts the
 * start delay ("90 9B").  The input layer key follows
 * o->text.textLayerMode ("90 91"): the standard layer 2, the configured
 * layer number, or the window's priority (the high 12 bits; the low 20 are
 * all set so that the key sorts above anything on the same priority). */
static void WaitTextOut_Ctor(OpenBGI_t* o, WaitTextOut_t* w, Thread_t* t, Window_t* win)
{
	Wait_Ctor(o, &w->w, t);
	w->w.vt = &WaitTextOut_Vtbl.base;
	w->win = win;
	WaitTextOut_SetFast(w, 0);
	w->scrolling = 0;
	w->phase = 0;
	w->overhang = 0;
	w->clearing = 0;
	w->finishNow = 0;
	w->autoAdvanceAt = 0;
	switch(o->text.textLayerMode)
	{
		case 0: w->layerKey = 2; break;
		case 1: w->layerKey = ((uint32_t)o->text.textLayerNo << 20) | 0xfffff; break;
		case 2: w->layerKey = (DispObj_GetPriority(&win->obj) << 20) | 0xfffff; break;
		default: break; // the key keeps whatever the memory held (zero here)
	}
	MouseLayer_Push(o, w->layerKey);
	KeyLayer_Push(o, w->layerKey);
	Input_Poll(o, w->layerKey, w->layerKey); // swallow the input that started us
	Window_SetTextLevel(win, 0);
	Window_ShowText(win, 1);
	Window_RecompositeAll(o, win);
	Window_ItemsDisableAll(o, win);
	w->startDelayOn = o->text.startDelayOn;
	w->startDelayUntil = GetTicks(o) + (uint32_t)o->text.startDelayMs;
}

// pops the input layers; the window keeps what was drawn
static void WaitTextOut_Dtor(OpenBGI_t* o, WaitTextOut_t* w)
{
	w->w.vt = &WaitTextOut_Vtbl.base;
	MouseLayer_Pop(o, w->layerKey);
	KeyLayer_Pop(o, w->layerKey);
	Wait_Dtor(o, &w->w);
}

WaitTextOut_t* WaitTextOut_New(OpenBGI_t* o, Thread_t* t, Window_t* win)
{
	WaitTextOut_t* w = (WaitTextOut_t*)BGI_Calloc(sizeof(WaitTextOut_t));
	WaitTextOut_Ctor(o, w, t, win);
	return w;
}

static void WaitTextOut_Destroy(OpenBGI_t* o, Wait_t* w)
{
	WaitTextOut_Dtor(o, (WaitTextOut_t*)w);
	BGI_Free(w);
}

void WaitTextOut_SetInstant(WaitTextOut_t* w, int f)
{
	w->instant = f;
}

void WaitTextOut_SetWaitAtEnd(WaitTextOut_t* w, int f)
{
	w->waitAtEnd = f;
}

void WaitTextOut_SetAllowSkip(WaitTextOut_t* w, int f)
{
	w->allowSkip = f;
}

void WaitTextOut_SetAllowKeys(WaitTextOut_t* w, int f)
{
	w->allowKeys = f;
}

void WaitTextOut_SetFast(WaitTextOut_t* w, int f)
{
	w->fast = f;
}

// end the wait at the next step, whatever is left of the text
void WaitTextOut_FinishNow(WaitTextOut_t* w)
{
	w->finishNow = 1;
}

// the string to output (not copied: it has to outlive the wait); the first step runs at once
static void WaitTextOut_SetText(OpenBGI_t* o, WaitTextOut_t* w, const char* p)
{
	w->text = p;
	SET_TIMER(o, w, 0);
}

// the parameter word of the plain output: the glyph colour
static void WaitTextOut_SetParam(OpenBGI_t* o, WaitTextOut_t* w, uint32_t v)
{
	w->param = v;
}

/* one poll.  The input mask keeps the skip bit (31), the left button (bit
 * 0), wheel down (0x80), decide (0x100) and the down key (0x2000);
 * allowSkip strips the skip bit, allowKeys the down key and wheel down
 * (decide and the button always count).  Any input switches to fast mode
 * (and finishes at once when o->text.textInputFinishes, "90 9F", is set).
 * The class's step then runs; the wait ends when it reports completion or
 * waits stopped blocking, pushing the `fast` flag. */
static int WaitTextOut_Poll(OpenBGI_t* o, Wait_t* base)
{
	WaitTextOut_t* w = (WaitTextOut_t*)base;
	int done, scrolling;

	Wait_Tick(o, base);
	w->rawInput = Input_Poll(o, w->layerKey, w->layerKey);
	w->input = w->rawInput & 0x80002181u;
	if(!w->allowSkip)
		w->input &= 0x7fffffffu;
	if(!w->allowKeys)
		w->input &= ~0x2080u;
	scrolling = WaitTextScroll_Active(w); // OpenBGI "EF 08": a long message scrolls
	if(scrolling)
		WaitTextScroll_Input(o, w); // the wheel, the up / down keys and the arrows move the view
	if(w->input)
	{
		WaitTextOut_SetFast(w, 1);
		// (with text still below the view, input turns the page instead of finishing)
		if(o->text.textInputFinishes && !(scrolling && WaitTextScroll_HasMore(w)))
			WaitTextOut_FinishNow(w);
	}
	done = VT(w)->step(o, w);
	Wait_Flush(o, base);
	if(done || !Wait_IsBlocking(o, base))
	{
		Thread_Push(base->thread, (uint32_t)w->fast);
		return 1;
	}
	return 0;
}

// start a line scroll: the following steps scroll until it is complete
static void ScrollLine(WaitTextOut_t* w)
{
	w->scrolling = 1;
}

/* the plain class's step: run text steps while the pacing allows.  A step
 * is one scroll increment, one control byte, or one character (two calls
 * per character: the preview on the cursor item, then the real draw).  1
 * when the end of the text finished the wait. */
static int WaitTextOut_Step(OpenBGI_t* o, WaitTextOut_t* w)
{
	int result = 0;

	if(!(Wait_TimerExpired(o, &w->w) || w->fast || w->finishNow || w->autoAdvanceAt != 0))
		return 0;
	for(;;)
	{
		int more = 1;
		if(w->scrolling)
			WaitTextOut_ScrollStep(o, w);
		else
		{
			switch((uint8_t)*w->text)
			{
				case 0:
					result = WaitTextOut_EndOfText(o, w);
					more = 0;
					break;
				case 1: w->text += WaitTextOut_CtlClear(w); break;
				case 0xa: w->text += WaitTextOut_CtlNewLine(w); break;
				case 0xc: w->text += VT(w)->pageBreak(o, w); break;
				default: w->text += WaitTextOut_DrawChar(o, w, w->text); break;
			}
		}
		if(!(Wait_TimerExpired(o, &w->w) || w->fast || w->instant) || !more)
			break;
	}
	return result;
}

/* the class's setTimer: `ms` is added to the deadline (so a backlog of
 * steps catches up), except that pacing is suspended in fast mode and
 * (except while scrolling) in instant mode, where the timer is left
 * expired */
static void WaitTextOut_SetTimer(OpenBGI_t* o, Wait_t* base, uint32_t ms)
{
	WaitTextOut_t* w = (WaitTextOut_t*)base;
	if(ms && !w->fast && (!w->instant || w->scrolling))
		Wait_TimerAdd(base, ms);
	else
		Wait_SetTimer(o, base, 0);
}

/* control byte 1 "clear": scroll the text out line by line (one line per
 * step), then reset the cursor.  Returns the bytes consumed: 0 while lines
 * are left, 1 when finished. */
int WaitTextOut_CtlClear(WaitTextOut_t* w)
{
	if(!w->clearing)
	{
		Rect_t area;
		int32_t cur[2];
		Window_GetTextArea(w->win, &area);
		Window_GetCursor(w->win, cur);
		// lines still to scroll away: the full count, one less when the
		// cursor sits at the line start (nothing on the current line)
		w->linesLeft = Window_LineCount(w->win) - (cur[0] == area.l);
		while(Window_NewLine(w->win))
			w->linesLeft--;
		w->clearing = 1;
	}
	if(w->linesLeft > 0)
	{
		w->linesLeft--;
		ScrollLine(w);
		return 0;
	}
	Window_ResetCursor(w->win);
	w->overhang = 0;
	w->clearing = 0;
	return 1;
}

/* control byte 0x0C "page break": fade the text layer out over
 * o->text.fadeSteps steps of o->text.fadeInterval ms ("90 96"), then
 * clear it and reset the cursor.  Returns the bytes consumed: 0 while
 * fading, 1 when done. */
static int WaitTextOut_PageBreak(OpenBGI_t* o, WaitTextOut_t* w)
{
	int result;

	if(w->phase == 0)
	{
		w->fadeSteps = o->text.fadeSteps;
		w->fadeInterval = o->text.fadeInterval;
	}
	SET_TIMER(o, w, w->fadeInterval);
	if(w->phase < w->fadeSteps - 1)
	{
		w->phase++;
		Window_SetTextLevel(w->win, (w->phase << 8) / w->fadeSteps);
		Window_RecompositeAll(o, w->win);
		result = 0;
	}
	else
	{
		w->phase = 0;
		Window_ClearText(o, w->win);
		Window_SetTextLevel(w->win, 0);
		Window_RecompositeAll(o, w->win);
		Window_ResetCursor(w->win);
		w->overhang = 0;
		result = 1;
	}
	DirtyTextArea(o, w);
	Wait_MarkDirty(&w->w);
	return result;
}

// control byte 0x0A "new line": the next line, or a scroll when the area is full; consumes 1 byte
int WaitTextOut_CtlNewLine(WaitTextOut_t* w)
{
	if(!Window_NewLine(w->win))
		ScrollLine(w);
	w->overhang = 0;
	return 1;
}

/* one increment of a line scroll: the line advance is spread over
 * o->text.scrollSteps steps of o->text.scrollInterval ms ("90 95"; all
 * remaining ones at once in fast mode) */
void WaitTextOut_ScrollStep(OpenBGI_t* o, WaitTextOut_t* w)
{
	int step, adv, before, after;

	if(w->phase == 0)
	{
		w->scrollSteps = o->text.scrollSteps;
		w->scrollInterval = o->text.scrollInterval;
	}
	SET_TIMER(o, w, w->scrollInterval);
	step = w->fast ? w->scrollSteps - w->phase : 1;
	adv = Window_LineAdvance(w->win);
	after = (w->phase + step) * adv / w->scrollSteps;
	before = w->phase * adv / w->scrollSteps;
	Window_Scroll(o, w->win, after - before);
	w->phase += step;
	if(w->phase >= w->scrollSteps)
	{
		w->scrolling = 0;
		w->phase = 0;
	}
	DirtyTextArea(o, w);
	Wait_MarkDirty(&w->w);
}

/* one character at p.  Two calls per character: the first ("phase 0")
 * shows the glyph on cursor item 0 at level 0xAA as a preview and checks
 * that it fits (a closing character may overhang into the ruby reserve
 * once; otherwise a new line is started, or a scroll when there is none -
 * the original throws and catches an int there and returns 0 bytes).  The
 * second call draws the glyph (with the global shadow style,
 * o->text.textStyle) into the text layer and advances the cursor.  Returns
 * the bytes consumed (0 until the second call, or when the window has no
 * font). */
int WaitTextOut_DrawChar(OpenBGI_t* o, WaitTextOut_t* w, const char* p)
{
	FontInfo_t fi;
	Bmp_t cell, view;
	Glyph_t g;
	Rect_t r;
	int32_t cur[2];
	uint32_t code;
	int len, dbcs, size, glyphW, proportional, gap, advance, consumed = 0;

	len = TextGetChar(o, &code, p);
	dbcs = TextIsWide(o, code, len);
	SET_TIMER(o, w, o->text.charInterval); // "90 94"
	if(!BmpMgr_FontInfo(o->text.textBmpMgr, &fi, Window_GetFont(w->win)))
		return 0;
	size = fi.size;
	glyphW = size * fi.widthPct / 100;
	proportional = Window_GetProportional(w->win);
	Bmp_AllocScreen(o, &cell, 2 * size, size, 1); // room for a full-width glyph
	Font_PaintGlyph(o, &cell, &g, (int)code, fi.font, w->param);
	view = cell;
	if(proportional)
	{ // the ink's bounding box plus a gap each side
		Bmp_CropGlyph(&view, &g);
		gap = Font_GapFor(&g, size);
		advance = 2 * gap - g.left + g.right + 1;
	}
	else
	{ // fixed pitch: a half cell for a single-byte character
		gap = 0;
		advance = dbcs ? glyphW : glyphW / 2;
	}

	if(w->phase == 0)
	{
		if(!Window_CursorFits(w->win, advance))
		{
			if(Window_GetRubyReserve(w->win) && IsKinsokuChar(o, code) && !w->overhang)
				w->overhang = 1;
			else
			{
				w->overhang = 0;
				if(!Window_NewLine(w->win))
				{
					ScrollLine(w);
					BGI_Free(cell.pixels);
					return 0; // "throw 0" in the original, caught in this function
				}
			}
		}
		Window_GetCursor(w->win, cur);
		Window_ItemSetBitmap(o, w->win, 0, &view);
		Window_ItemSet(o, w->win, 0, cur[0] + gap, cur[1], 0xaa);
		Window_ItemEnable(o, w->win, 0, 1);
		Window_ItemRect(o, w->win, &r, 0);
		DirtyRect(o, w, &r);
		w->phase++;
		Wait_MarkDirty(&w->w);
	}
	else
	{
		int sdx = o->text.textStyle[1] * size / 100; // the shadow offset is a percentage of the font size
		int sdy = o->text.textStyle[2] * size / 100;
		Window_GetCursor(w->win, cur);
		cur[0] += gap;
		if(o->text.textStyle[0])
		{ // the shadow: the glyph recoloured (effect 5) and blended at the style's level
			Bmp_t tmp;
			Bmp_AllocScreen(o, &tmp, view.w, view.h, 1);
			Bmp_Fill(&tmp, NULL, 0);
			Bmp_BlitEffect(&tmp, &view, 5, 0x100);
			Window_DrawBmpToText(o, w->win, &r, cur[0] + sdx, cur[1] + sdy, &tmp, 1, 0x100 - o->text.textStyle[4]);
			BGI_Free(tmp.pixels);
		}
		Window_DrawBmpToText(o, w->win, &r, cur[0], cur[1], &view, 0, 0);
		Window_ItemEnable(o, w->win, 0, 0);
		if(o->text.textStyle[0])
		{
			r.r += sdx; // the dirty rectangle has to cover the shadow too
			r.b += sdy;
		}
		DirtyRect(o, w, &r);
		Window_CursorAdvance(w->win, advance);
		w->phase--;
		consumed = len;
		Wait_MarkDirty(&w->w);
	}
	BGI_Free(cell.pixels);
	return consumed;
}

/* the "waiting for input" cursor: cycles through the pictures of "90 98"
 * at o->text.cursorInterval ms ("90 99") on window item 0, at the text
 * cursor (plus the class's offset) or at the fixed position of "90 9A".
 * The first call makes room for the widest picture (a new line, or a
 * scroll first) and drops the fast / instant mode.  Returns 1 once input
 * arrived (the item is hidden then). */
int WaitTextOut_CursorAnim(OpenBGI_t* o, WaitTextOut_t* w)
{
	int gotInput;

	if(w->phase == 0)
	{
		uint32_t maxW = 0;
		int i;
		for(i = 0; i < o->text.textItemCount; i++)
			if(maxW < (uint32_t)o->text.textItems[i].w)
				maxW = (uint32_t)o->text.textItems[i].w;
		if(!Window_CursorFits(w->win, (int)maxW))
		{
			if(!Window_NewLine(w->win))
			{
				ScrollLine(w);
				return 0;
			}
			VT(w)->cursorNewLine(o, w);
		}
		w->input &= 0x80000000u; // only the skip bit survives into the wait
		if(!w->input)
			WaitTextOut_SetFast(w, 0);
		WaitTextOut_SetInstant(w, 0);
		SET_TIMER(o, w, 0);
		w->phase++;
	}
	DirtyItem0(o, w);
	gotInput = w->input != 0;
	if(!gotInput)
	{
		SET_TIMER(o, w, o->text.cursorInterval);
		if(o->text.textItemCount > 0)
		{
			Bmp_t* item = &o->text.textItems[w->phase - 1];
			w->phase += w->phase < o->text.textItemCount ? 1 : 1 - o->text.textItemCount;
			if(item->pixels)
			{
				int x, y;
				if(o->text.cursorPosMode == 1)
				{
					x = o->text.cursorX;
					y = o->text.cursorY;
				}
				else
				{
					int32_t cur[2], off[2];
					Window_GetCursor(w->win, cur);
					VT(w)->cursorOffset(o, w, off);
					x = cur[0] + off[0];
					y = cur[1] + off[1];
				}
				Window_ItemSet(o, w->win, 0, x, y, 0);
				Window_ItemSetBitmap(o, w->win, 0, item);
				Window_ItemEnable(o, w->win, 0, 1);
			}
			else
				Window_ItemEnable(o, w->win, 0, 0);
		}
	}
	else
	{
		Window_ItemEnable(o, w->win, 0, 0);
		w->phase = 0;
	}
	DirtyItem0(o, w);
	Wait_MarkDirty(&w->w);
	return gotInput;
}

/* the string is exhausted.  Without waitAtEnd (or after FinishNow) the
 * cursor item is hidden and the wait ends.  Otherwise: start the
 * auto-advance timer ("90 97"), honour the start delay, run the cursor
 * animation, and finish on input or when the auto-advance time is up.
 * Returns 1 when the wait is over. */
int WaitTextOut_EndOfText(OpenBGI_t* o, WaitTextOut_t* w)
{
	int r;

	if(!w->waitAtEnd || w->finishNow)
	{
		Rect_t rc;
		if(Window_ItemRect(o, w->win, &rc, 0))
		{
			DirtyRect(o, w, &rc);
			Window_ItemEnable(o, w->win, 0, 0);
		}
		Wait_MarkDirty(&w->w);
		return 1;
	}
	if(o->text.autoAdvanceOn && w->phase == 0)
		w->autoAdvanceAt = GetTicks(o) + (uint32_t)o->text.autoAdvanceMs;
	if(w->startDelayOn)
	{
		if(w->startDelayUntil > GetTicks(o) && !w->fast)
			return 0;
		w->startDelayOn = 0;
	}
	r = (Wait_TimerExpired(o, &w->w) || w->fast) ? WaitTextOut_CursorAnim(o, w) : 0;
	if(o->text.autoAdvanceOn && !r && w->autoAdvanceAt <= GetTicks(o))
		WaitTextOut_FinishNow(w);
	return r;
}

// the plain output starts the cursor's new line at the margin
static void WaitTextOut_CursorNewLine(OpenBGI_t* o, WaitTextOut_t* w)
{
	(void)w;
}

// the plain output puts the cursor picture at the text cursor
static void WaitTextOut_CursorOffset(OpenBGI_t* o, WaitTextOut_t* w, int32_t out[2])
{
	(void)w;
	out[0] = 0;
	out[1] = 0;
}

/* notifications ("80 4C", by their a): 1 and 0x102 finish instantly,
 * 0x100 finishes at the next step (the remaining text is paced out),
 * 0x101 sets allowSkip to b */
static void WaitTextOut_OnMessage(OpenBGI_t* o, Wait_t* base, WaitMsg_t* m)
{
	WaitTextOut_t* w = (WaitTextOut_t*)base;
	switch(m->a)
	{
		case 1:
		case 0x102:
			WaitTextOut_FinishNow(w);
			WaitTextOut_SetInstant(w, 1);
			break;
		case 0x100: WaitTextOut_FinishNow(w); break;
		case 0x101: WaitTextOut_SetAllowSkip(w, (int)m->b); break;
		default: break;
	}
}

// the dirty flag, except that there is nothing to present while the window sits below the draw limit of "90 09" (it is not drawn then)
int WaitTextOut_IsDirty(OpenBGI_t* o, Wait_t* base)
{
	WaitTextOut_t* w = (WaitTextOut_t*)base;
	uint32_t key = w->win->obj.vt->sortKey(o, &w->win->obj);
	if(Gfx_GetDrawLimit(o->text.textGfx) > key)
		return 0;
	return base->dirty;
}

// ---- WaitTextRich ----------------------------------------------------------------------------

// the shadow style starts as the global one (o->text.textStyle); SetStyle replaces it
static void WaitTextRich_Ctor(OpenBGI_t* o, WaitTextRich_t* w, Thread_t* t, Window_t* win)
{
	WaitTextOut_Ctor(o, &w->t, t, win);
	w->t.w.vt = &WaitTextRich_Vtbl.base;
	memset(&w->rich, 0, sizeof w->rich);
	memcpy(w->style, o->text.textStyle, sizeof w->style);
}

static void WaitTextRich_Dtor(OpenBGI_t* o, WaitTextRich_t* w)
{
	w->t.w.vt = &WaitTextRich_Vtbl.base;
	if(w->scrollOn)
		WaitTextScroll_End(o, &w->t); // (OpenBGI) the nodes stay in the window's text document
	RichState_Free(&w->rich);
	WaitTextOut_Dtor(o, &w->t);
}

WaitTextRich_t* WaitTextRich_New(OpenBGI_t* o, Thread_t* t, Window_t* win)
{
	WaitTextRich_t* w = (WaitTextRich_t*)BGI_Calloc(sizeof(WaitTextRich_t));
	WaitTextRich_Ctor(o, w, t, win);
	return w;
}

static void WaitTextRich_Destroy(OpenBGI_t* o, Wait_t* w)
{
	WaitTextRich_Dtor(o, (WaitTextRich_t*)w);
	BGI_Free(w);
}

/* lay the whole string out from the window's cursor through the class's
 * layout slots (horizontal or vertical): the glyphs with their reveal
 * delays, the ruby when rubyOn, the alignment.  With plainStyle the shadow
 * is dropped, otherwise the object's style is used; `hang` turns on the
 * kinsoku handling (a margin at the line end that closing punctuation may
 * hang into).  The cursor ends up behind the text and the timer is
 * restarted.  1 ok, 0 when the layout failed. */
int WaitTextRich_Prepare(OpenBGI_t* o, WaitTextRich_t* w, const char* str, int rubyOn, int hang, int plainStyle)
{
	Window_t* win = w->t.win;
	int32_t cur[2], style[5], lines;
	Rect_t area;
	int adv, font, proportional, ok;
	/* OpenBGI "EF 08": with scrolling on, a horizontal output lays its text
	 * out in the coordinates of the window's text document and hands the
	 * nodes to it (window.h); wait_text_scroll.c then does the stepping */
	TextDoc_t* doc = (Window_DocActive(o, win) && w->t.w.vt == &WaitTextRich_Vtbl.base) ? Window_Doc(o, win) : NULL;

	Window_GetCursor(win, cur);
	if(doc)
		cur[1] += doc->scrollY;
	Window_GetTextArea(win, &area);
	adv = Window_LineAdvance(win);
	font = Window_GetFont(win);
	proportional = Window_GetProportional(win);
	if(plainStyle)
		Text_MakeStyle(o, style, 0, 0, 0, 0, 0);
	else
		memcpy(style, w->style, sizeof style);
	ok = VT(&w->t)->layoutCore(o, &w->t, &w->rich, &lines, str, rubyOn, &o->text.rubyDict, cur, &area, adv, font,
		proportional, hang, w->t.param, style);
	if(!ok)
		return 0;
	if(rubyOn)
		VT(&w->t)->rubyPlace(o, &w->t, &w->rich, font, w->rubyColour, style, &o->text.rubyDict);
	VT(&w->t)->align(o, &w->t, &w->rich, cur, &area, font, hang, style, Window_GetSwingStyle(win));
	Window_SetCursor(win, cur[0], cur[1] - (doc ? doc->scrollY : 0));
	if(doc)
	{
		w->docLink = Window_DocAppend(o, win, &w->rich);
		w->rich.head = *w->docLink; // this output's part of the document
		w->docSerial = doc->serial;
		w->scrollTo = doc->scrollY;
		w->scrollOn = 1;
	}
	w->rubyOn = rubyOn;
	SET_TIMER(o, &w->t, 0);
	return ok;
}

// replace the text (drops the previous layout first); 1 ok
int WaitTextRich_SetTextEx(OpenBGI_t* o, WaitTextRich_t* w, const char* str, int rubyOn, int hang, int plainStyle)
{
	if(w->scrollOn)
	{ // (OpenBGI) the previous layout is in the window's text document: take it out again
		if(WaitTextScroll_Active(&w->t))
			Window_DocCut(w->t.win, w->docLink);
		w->rich.head = NULL;
		w->scrollOn = 0;
	}
	RichState_Free(&w->rich);
	return WaitTextRich_Prepare(o, w, str, rubyOn, hang, plainStyle);
}

// the setText slot: no ruby, hanging punctuation, the object's style
static void WaitTextRich_SetText(OpenBGI_t* o, WaitTextOut_t* w, const char* p)
{
	WaitTextRich_SetTextEx(o, (WaitTextRich_t*)w, p, 0, 1, 0);
}

void WaitTextRich_SetColours(OpenBGI_t* o, WaitTextRich_t* w, uint32_t colour, uint32_t rubyColour)
{
	WaitTextOut_SetParam(o, &w->t, colour);
	w->rubyColour = rubyColour;
}

// the setParam slot: one colour for the text and the ruby
static void WaitTextRich_SetParam(OpenBGI_t* o, WaitTextOut_t* w, uint32_t v)
{
	WaitTextRich_SetColours(o, (WaitTextRich_t*)w, v, v);
}

// the shadow style: on, dx, dy, colour, level (Text_MakeStyle)
void WaitTextRich_SetStyle(OpenBGI_t* o, WaitTextRich_t* w, const int32_t s[5])
{
	Text_MakeStyle(o, w->style, s[0], s[1], s[2], (uint32_t)s[3], s[4]);
}

// every laid-out glyph has been drawn
static int RichAllShown(WaitTextRich_t* w)
{
	RichChar_t* c;
	for(c = w->rich.head; c; c = c->next)
		if(!c->shown)
			return 0;
	return 1;
}

/* reveal the laid-out glyphs.  Every timer tick (1 ms) is one step; a
 * step decrements the delay of every pending glyph, and a glyph whose delay
 * ran out fades in over o->text.rubyFadeSteps steps (drawn only on the last
 * step of the batch, so a backlog of ticks does not draw every intermediate
 * level).  Fast / instant mode, or a zero character delay
 * (o->text.rubyCharDelay), shows everything in one pass.  A glyph is drawn
 * twice: its shape punched out of the layer (effect 0x40), then blended in
 * at the fade's level. */
static void RichShowStep(OpenBGI_t* o, WaitTextRich_t* w)
{
	WaitTextOut_t* t = &w->t;
	int fastMode = t->fast || t->instant || o->text.rubyCharDelay == 0;
	uint32_t steps = 0;
	Rect_t r;

	if(Wait_TimerExpired(o, &t->w) && !fastMode)
	{
		do
		{
			Wait_TimerAdd(&t->w, 1);
			steps++;
		} while(Wait_TimerExpired(o, &t->w));
	}
	while(steps > 0 || fastMode)
	{
		RichChar_t* c;
		steps--;
		for(c = w->rich.head; c; c = c->next)
		{
			if(c->shown)
				continue;
			if(c->delay != 0 && !fastMode)
			{
				if(--c->delay == 0)
				{
					c->fadeStep = 0;
					c->fadeSteps = o->text.rubyFadeSteps;
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
				Window_DrawBmpToText(o, t->win, &r, c->x, c->y, &c->glyph, 0x40, 1);
				Window_DrawBmpToText(o, t->win, &r, c->x, c->y, &c->glyph, 1,
					0x100 - (int)(((uint32_t)c->fadeStep << 8) / (uint32_t)c->fadeSteps));
				DirtyRect(o, t, &r);
				Wait_MarkDirty(&t->w);
				c->fadeStep++;
			}
			else
			{
				Window_DrawBmpToText(o, t->win, &r, c->x, c->y, &c->glyph, 0x40, 1);
				Window_DrawBmpToText(o, t->win, &r, c->x, c->y, &c->glyph, 1, 0);
				DirtyRect(o, t, &r);
				Wait_MarkDirty(&t->w);
				c->shown = 1;
			}
		}
		if(fastMode)
			break;
	}
	SET_TIMER(o, t, 0);
}

// the rich classes' step: reveal glyphs until all are shown, then the shared end-of-text handling
static int WaitTextRich_Step(OpenBGI_t* o, WaitTextOut_t* t)
{
	WaitTextRich_t* w = (WaitTextRich_t*)t;
	if(WaitTextScroll_Active(t)) // OpenBGI "EF 08"
		return WaitTextScroll_Step(o, t);
	if(!(Wait_TimerExpired(o, &t->w) || t->fast || t->finishNow || t->autoAdvanceAt != 0))
		return 0;
	if(RichAllShown(w))
		return WaitTextOut_EndOfText(o, t);
	RichShowStep(o, w);
	return 0;
}

// the rich classes indent the cursor's new line like a text line
static void WaitTextRich_CursorNewLine(OpenBGI_t* o, WaitTextOut_t* w)
{
	Window_CursorAdvance(w->win, Text_GetIndent(o));
}

// the cursor picture goes below the ruby when ruby is on
static void WaitTextRich_CursorOffset(OpenBGI_t* o, WaitTextOut_t* t, int32_t out[2])
{
	WaitTextRich_t* w = (WaitTextRich_t*)t;
	FontInfo_t fi;
	int ok = BmpMgr_FontInfo(o->text.textBmpMgr, &fi, Window_GetFont(t->win));
	out[0] = 0;
	out[1] = (w->rubyOn && ok) ? Text_RubySize(o, fi.size) : 0;
}

// the three layout slots forward to the horizontal layout of gfx/text.h
static int WaitTextRich_LayoutCore(OpenBGI_t* o, WaitTextOut_t* w, RichState_t* rich, int32_t* outLines, const char* str, int rubyOn,
	RubyDict_t* dict, int32_t cur[2], const Rect_t* area, int adv, int font, int proportional,
	int hang, uint32_t colour, const int32_t style[5])
{
	(void)w;
	return Text_LayoutCoreH(o, rich, outLines, str, rubyOn, dict, cur, area, adv, font, proportional, hang, colour, style);
}

static int WaitTextRich_RubyPlace(OpenBGI_t* o, WaitTextOut_t* w, RichState_t* rich, int font, uint32_t colour,
	const int32_t style[5], RubyDict_t* dict)
{
	(void)w;
	return Text_RubyPlaceH(o, rich, font, colour, style, dict);
}

static void WaitTextRich_Align(OpenBGI_t* o, WaitTextOut_t* w, RichState_t* rich, int32_t cur[2], const Rect_t* area, int font,
	int hang, const int32_t style[5], int swing)
{
	(void)w;
	Text_AlignH(o, rich, cur, area, font, hang, style, swing);
}

// ---- WaitTextRichV ---------------------------------------------------------------------------

// the vertical variant: the rich object with the vertical layout slots
WaitTextRich_t* WaitTextRichV_New(OpenBGI_t* o, Thread_t* t, Window_t* win)
{
	WaitTextRich_t* w = WaitTextRich_New(o, t, win);
	w->t.w.vt = &WaitTextRichV_Vtbl.base;
	return w;
}

static void WaitTextRichV_Destroy(OpenBGI_t* o, Wait_t* w)
{
	w->vt = &WaitTextRichV_Vtbl.base;
	WaitTextRich_Dtor(o, (WaitTextRich_t*)w);
	BGI_Free(w);
}

/* the cursor picture goes left of the column, past the ruby, by the width
 * of the first picture.  The original reads o->text.textItems[0] without
 * a check; an empty item table counts as width 0 here. */
static void WaitTextRichV_CursorOffset(OpenBGI_t* o, WaitTextOut_t* t, int32_t out[2])
{
	WaitTextRich_t* w = (WaitTextRich_t*)t;
	FontInfo_t fi;
	int ok = BmpMgr_FontInfo(o->text.textBmpMgr, &fi, Window_GetFont(t->win));
	int ruby = (w->rubyOn && ok) ? Text_RubySize(o, fi.size) : 0;
	out[0] = -((o->text.textItems ? o->text.textItems[0].w : 0) + ruby);
	out[1] = 0;
}

// the three layout slots forward to the vertical layout of gfx/text.h
static int WaitTextRichV_LayoutCore(OpenBGI_t* o, WaitTextOut_t* w, RichState_t* rich, int32_t* outLines, const char* str,
	int rubyOn, RubyDict_t* dict, int32_t cur[2], const Rect_t* area, int adv, int font, int proportional,
	int hang, uint32_t colour, const int32_t style[5])
{
	(void)w;
	return Text_LayoutCoreV(o, rich, outLines, str, rubyOn, dict, cur, area, adv, font, proportional, hang, colour, style);
}

static int WaitTextRichV_RubyPlace(OpenBGI_t* o, WaitTextOut_t* w, RichState_t* rich, int font, uint32_t colour,
	const int32_t style[5], RubyDict_t* dict)
{
	(void)w;
	return Text_RubyPlaceV(o, rich, font, colour, style, dict);
}

static void WaitTextRichV_Align(OpenBGI_t* o, WaitTextOut_t* w, RichState_t* rich, int32_t cur[2], const Rect_t* area, int font,
	int hang, const int32_t style[5], int swing)
{
	(void)w;
	Text_AlignV(o, rich, cur, area, font, hang, style, swing);
}

// ========================================================================
// the starters of the text-output instructions
// ========================================================================

/* start message output in window h and hand the wait to the thread.  mode
 * -1 is the plain WaitTextOut (colourOrParam is its parameter word), 0 the
 * rich class (colourOrParam for text and ruby, the current style or none
 * with plainStyle), 1 the rich class or its vertical variant by the
 * window's draw style, with the colours and the `style` given.  instant,
 * waitAtEnd, allowSkip and allowKeys are the output flags (WaitTextOut_Set*).
 * 0 ok, -1 no such window, 0x80000001 no font. */
int StartTextOut(OpenBGI_t* o, Thread_t* t, uint32_t h, const char* str, uint32_t colourOrParam, int rubyOn, uint32_t rubyColour,
	int hang, int plainStyle, const int32_t* style, int instant, int waitAtEnd, int allowSkip, int allowKeys, int mode)
{
	Window_t* win = (Window_t*)Gfx_FindWindow(o->gfx.gfx, h);
	WaitTextOut_t* w;
	if(!win)
		return -1;
	if(!Window_GetFont(win))
		return (int)0x80000001;
	switch(mode)
	{
		case -1:
			w = WaitTextOut_New(o, t, win);
			VT(w)->setParam(o, w, colourOrParam);
			VT(w)->setText(o, w, str);
			break;
		case 1:
		{
			WaitTextRich_t* r = Window_GetDrawStyle(win) == 1 ? WaitTextRichV_New(o, t, win) : WaitTextRich_New(o, t, win);
			WaitTextRich_SetColours(o, r, colourOrParam, rubyColour);
			WaitTextRich_SetStyle(o, r, style);
			WaitTextRich_SetTextEx(o, r, str, rubyOn, hang, 0);
			w = &r->t;
			break;
		}
		case 0:
		{
			WaitTextRich_t* r = WaitTextRich_New(o, t, win);
			VT(&r->t)->setParam(o, &r->t, colourOrParam);
			WaitTextRich_SetTextEx(o, r, str, rubyOn, hang, plainStyle);
			w = &r->t;
			break;
		}
		default: // unreachable; the original uses the colour argument as the object pointer
			w = (WaitTextOut_t*)(uintptr_t)colourOrParam;
			break;
	}
	WaitTextOut_SetInstant(w, instant);
	WaitTextOut_SetWaitAtEnd(w, waitAtEnd);
	WaitTextOut_SetAllowSkip(w, allowSkip);
	WaitTextOut_SetAllowKeys(w, allowKeys);
	Thread_SetWait(o, t, &w->w);
	return 0;
}

// the 12-argument form ("90 90" with mode -1, "91 90" / "91 92" with mode 0): no style, the colour doubles as ruby colour
int StartTextOutW(OpenBGI_t* o, Thread_t* t, uint32_t h, const char* str, uint32_t colourOrParam, int plainStyle, int instant,
	int waitAtEnd, int allowSkip, int allowKeys, int rubyOn, int hang, int mode)
{
	return StartTextOut(o, t, h, str, colourOrParam, rubyOn, colourOrParam, hang, plainStyle, NULL, instant, waitAtEnd,
		allowSkip, allowKeys, mode);
}

// the name of a wait class of this file, for the debugger (NULL: not one of these)
/* The targets of the text outputs (bgi/inputmap.h): a click anywhere on
 * the output's layer hurries it, or goes on at its end; a long message
 * that scrolls has its arrows besides.  1 when `w` is a text output. */
int WaitText_Targets(OpenBGI_t* o, Wait_t* w, TargetSink_t* s)
{
	WaitTextOut_t* t = (WaitTextOut_t*)w;
	TargetDesc_t d;
	if(WaitText_ClassName(w->vt) == NULL)
		return 0;
	TargetDesc_Init(&d, OPENBGI_TARGET_TEXT, t->layerKey);
	TargetSink_Claim(s, d.layer);
	WaitTextScroll_Targets(o, t, s);
	d.flags = OPENBGI_TARGET_ANYWHERE;
	d.whole = 1;
	d.owner = Thread_GetId(w->thread);
	d.reach = Target_ReachLayer;
	TargetSink_Add(s, &d);
	return 1;
}

const char* WaitText_ClassName(const WaitVtbl_t* vt)
{
	if(vt == &WaitTextOut_Vtbl.base)
		return "WaitTextOut";
	if(vt == &WaitTextRich_Vtbl.base)
		return "WaitTextRich";
	if(vt == &WaitTextRichV_Vtbl.base)
		return "WaitTextRichV";
	return NULL;
}
