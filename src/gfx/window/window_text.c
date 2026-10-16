/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window_text.c - the text layer of a message window: drawing text into
 *                 it, the text area, the font settings and the cursor
 *                 (inc/bgi/gfx/window.h)
 *
 * Text is drawn through the text engine (text.h) into the text layer at
 * the window's cursor, which advances along the writing direction and
 * wraps at the end of the text area; the layout settings are what "91 88"
 * (font, proportional placement, ruby reserve), "91 89" (line spacing),
 * "91 8A" (draw style: horizontal or vertical) and "91 8B" (swing) set.
 * The text area ("90 88") is where characters may go; the usable area is
 * one font size wider on the right when ruby space is reserved, which is
 * where the plain output lets a closing character overhang.
 */
#include "window_internal.h"

// ---- text layer --------------------------------------------------------------------------------

void Window_ShowText(Window_t* w, int f) // "92 8C" (the caller recomposites)
{
	w->textVisible = f;
}

void Window_SetTextLevel(Window_t* w, int lvl) // the text layer's transparency, 0 opaque .. 0x100
{
	w->textLevel = lvl;
}

// "92 8D": Window_DrawBmpToText with script bitmap `bmp`; 2 when it does not exist
int Window_DrawToText(Window_t* w, Rect_t* outScreen, int x, int y, int bmp, int effect, int level)
{
	Bmp_t info;

	if(!BmpMgr_GetInfo(gDispBmpMgr, &info, bmp))
		return 2;
	return Window_DrawBmpToText(w, outScreen, x, y, &info, effect, level);
}

/* like Window_DrawToFrame but into the text layer, clipped to the usable
 * text area; (x, y) are text-layer coordinates.  The touched rectangle is
 * recomposited and returned in *outScreen in screen coordinates.  0 ok,
 * 4..7 the blit failed (Window_BlitResult). */
int Window_DrawBmpToText(Window_t* w, Rect_t* outScreen, int x, int y, const Bmp_t* src, int effect, int level)
{
	Bmp_t t = w->text;
	Rect_t area, r2;
	int32_t pos[2];
	int r;

	Window_UsableArea(w, &area);
	Bmp_Crop(&t, &area);
	r = Bmp_Blit(&t, x - area.l, y - area.t, src, effect, level);
	if(r == 0)
	{
		*outScreen = area;
		Rect_FromBmp(&r2, src);
		Rect_Offset(&r2, x, y);
		Rect_Clip(outScreen, &r2);
		w->obj.vt->getPos(&w->obj, pos);
		Rect_Offset(outScreen, pos[0], pos[1]);
		Window_RecompositeAt(w, x, y, src);
		return 0;
	}
	return Window_BlitResult(r, 0);
}

/* "91 91" / "91 93": immediate string output.  The text layer is made
 * opaque and visible and the items are switched off; the text engine lays
 * the string out from the cursor (with the ruby readings of its own
 * dictionary when `parseTags` is set, hanging punctuation with `hang`, in
 * `colour` / `rubyColour` with the shadow `style`), reports the touched
 * rectangles, which are recomposited one by one, and the cursor moves
 * behind the text.  Returns the layout's result: 1 drawn, 0 when the font
 * is unknown. */
int Window_DrawText(Window_t* w, const char* str, int parseTags, int hang, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5])
{
	char tags[0x400];
	int32_t cur[2];
	Rect_t area;
	Rect_t* rects;
	int font, proportional, swing, spacing;
	// `extra` and `ok` are left uninitialised by the original for draw
	// styles other than 0 / 1 (which cannot be set)
	int rectCount = 0, extra = 0, ok = 0;
	size_t i;

	Window_SetTextLevel(w, 0);
	Window_ShowText(w, 1);
	Window_ItemsDisableAll(w);
	Window_GetCursor(w, cur);
	Window_GetTextArea(w, &area);

	// the original seeds tags[0] from the engine's always-empty 8-byte
	// string (gEmptyPath) and zero-fills the rest
	memset(tags, 0, sizeof tags);
	tags[0] = gEmptyPath[0];
	if(parseTags)
		Text_ParseTags(tags, str);
	if(Window_DocActive(w)) // OpenBGI "EF 08": the glyphs are kept, so that the text can scroll
		return Window_DocDrawText(w, str, parseTags, tags, hang, colour, rubyColour, style);

	font = Window_GetFont(w);
	proportional = Window_GetProportional(w);
	swing = Window_GetSwingStyle(w);
	spacing = Window_GetSpacing(w);
	rects = (Rect_t*)BGI_Alloc(strlen(str) << 6); // four rectangles per byte

	switch(Window_GetDrawStyle(w))
	{
		case 0:
			ok = Text_LayoutH(&w->text, &rectCount, rects, &extra, cur, &area, str, parseTags, tags,
				font, proportional, hang, swing, spacing, colour, rubyColour, style, 0, 0);
			break;
		case 1:
			ok = Text_LayoutV(&w->text, &rectCount, rects, &extra, cur, &area, str, parseTags, tags,
				font, proportional, hang, swing, spacing, colour, rubyColour, style, 0, 0);
			break;
		default:
			ok = extra;
			break;
	}
	if(ok)
	{
		for(i = 0; i < (size_t)(uint32_t)rectCount; i++)
			Window_Recomposite(w, &rects[i]);
		Window_SetCursor(w, cur[0], cur[1]);
	}
	BGI_Free(rects);
	return ok;
}

/* "90 88": set the text area (inclusive rectangle in text-layer
 * coordinates) and put the cursor at its start.  1 ok; 0 when the area
 * does not lie inside the text layer or its right edge does not leave
 * room for the ruby reservation (nothing changes). */
int Window_SetTextAreaRect(Window_t* w, const Rect_t* r)
{
	uint32_t tw = (uint32_t)w->text.w, th = (uint32_t)w->text.h;

	if(r->l < 0 || (uint32_t)r->l >= tw)
		return 0;
	if(r->r < 0 || (uint32_t)r->r >= tw - (uint32_t)Window_RubyReserve(w))
		return 0;
	if(r->t < 0 || (uint32_t)r->t >= th)
		return 0;
	if(r->b < 0 || (uint32_t)r->b >= th)
		return 0;
	w->textArea = *r;
	Window_DocDrop(w); // (OpenBGI) a kept text document was laid out for the old area
	Window_ResetCursor(w);
	return 1;
}

// the same from a position and a size in pixels
int Window_SetTextArea(Window_t* w, int x, int y, int cw, int ch)
{
	Rect_t r;

	r.l = x;
	r.t = y;
	r.r = x + cw - 1;
	r.b = y + ch - 1;
	return Window_SetTextAreaRect(w, &r);
}

// clear the whole text layer and recomposite
void Window_ClearText(Window_t* w)
{
	Window_DocDrop(w); // (OpenBGI) the kept text document goes with the pixels
	Bmp_Clear(&w->text, NULL);
	Window_RecompositeAll(w);
}

/* move the usable area's contents up by n pixels and clear the
 * vacated strip (the whole area when n is not smaller than its height).
 * 1 when scrolled, 0 for n <= 0. */
int Window_Scroll(Window_t* w, int n)
{
	Rect_t area, r;
	Bmp_t src;

	if(n <= 0)
		return 0;
	Window_DocDrop(w); // (OpenBGI) the plain output moves pixels the kept text document knows nothing of
	Window_UsableArea(w, &area);
	r = area;
	if(n < area.b - area.t + 1)
	{
		src = w->text;
		r.t = area.t + n;
		Bmp_Crop(&src, &r);
		Bmp_Blit(&w->text, area.l, area.t, &src, 0x80, 0);
		r.t = area.b - n + 1;
	}
	Bmp_Clear(&w->text, &r);
	Window_RecompositeAll(w);
	return 1;
}

void Window_GetTextArea(Window_t* w, Rect_t* out) // "90 89"
{
	*out = w->textArea;
}

// the text area widened on the right by the ruby reservation
void Window_UsableArea(Window_t* w, Rect_t* out)
{
	Window_GetTextArea(w, out);
	out->r += Window_RubyReserve(w);
}

// the text layer's rectangle in screen coordinates
void Window_TextScreenRect(Window_t* w, Rect_t* out)
{
	int32_t pos[2];

	w->obj.vt->getPos(&w->obj, pos);
	Rect_FromBmp(out, &w->text);
	Rect_Offset(out, pos[0], pos[1]);
}

// ---- fonts and layout --------------------------------------------------------------------------

// the pixels kept free right of the text area: one font size when ruby space is reserved
int Window_RubyReserve(Window_t* w)
{
	return Window_GetRubyReserve(w) ? Window_GetFontSize(w) : 0;
}

/* "91 88": open the font through the bitmap manager's cache; on success
 * remember the size and the full-width glyph advance (size * widthPct /
 * 100) and, when ruby space is reserved, pull the text area's right edge
 * in so the reservation fits.  Returns the font cache result (0 ok,
 * 0x80000002 bad size, 0x80000003 bad width, 0x80000004 the font could
 * not be opened). */
int Window_SetFont(Window_t* w, const char* face, int size, int widthPct, int bold)
{
	Rect_t area, tr;
	int r, limit;

	r = BmpMgr_FontOpen(gDispBmpMgr, &w->font, face, size, widthPct, bold);
	if(r != 0)
		return r;
	w->fontSize = size;
	w->glyphW = (size * widthPct) / 100;
	if(Window_GetRubyReserve(w))
	{
		Window_GetTextArea(w, &area);
		Rect_FromBmp(&tr, &w->text);
		limit = tr.r - w->glyphW;
		if(area.r > limit)
		{
			area.r = limit;
			Window_SetTextAreaRect(w, &area);
		}
	}
	return r;
}

int Window_GetFont(Window_t* w)
{
	return w->font;
}

int Window_GetFontSize(Window_t* w)
{
	return w->fontSize;
}

// "91 89": the line gap in percent of the font size; 1 ok, 0 when not 0..800
int Window_SetSpacing(Window_t* w, int pct)
{
	if(pct < 0 || pct > 0x320)
		return 0;
	w->spacing = pct;
	return 1;
}

int Window_GetSpacing(Window_t* w)
{
	return w->spacing;
}

int Window_SpacingPixels(Window_t* w) // the line gap in pixels
{
	return Font_Percent(Window_GetFontSize(w), Window_GetSpacing(w));
}

int Window_LineAdvance(Window_t* w) // from one line to the next: the font size plus the gap
{
	int sp = Window_SpacingPixels(w);
	return Window_GetFontSize(w) + sp;
}

int Window_LineCount(Window_t* w) // whole lines that fit the text area's height
{
	Rect_t area;
	int adv;

	Window_GetTextArea(w, &area);
	adv = Window_LineAdvance(w);
	return (area.b - area.t + 1) / adv;
}

void Window_SetProportional(Window_t* w, int v)
{
	w->proportional = v;
}

int Window_GetProportional(Window_t* w)
{
	return w->proportional;
}

void Window_SetRubyReserve(Window_t* w, int f)
{
	w->reserveRuby = f;
}

int Window_GetRubyReserve(Window_t* w)
{
	return w->reserveRuby;
}

// "91 8A": 0 horizontal, 1 vertical; the cursor restarts.  1 ok, 0 for another value
int Window_SetDrawStyle(Window_t* w, int style)
{
	if((uint32_t)style > 1)
		return 0;
	w->drawStyle = style;
	Window_ResetCursor(w);
	return 1;
}

int Window_GetDrawStyle(Window_t* w)
{
	return w->drawStyle;
}

// "91 8B": 0 left, 1 centred, 2 right-aligned; 1 ok, 0 for another value
int Window_SetSwingStyle(Window_t* w, int style)
{
	if((uint32_t)style > 2)
		return 0;
	w->swingStyle = style;
	return 1;
}

int Window_GetSwingStyle(Window_t* w)
{
	return w->swingStyle;
}

// ---- cursor ------------------------------------------------------------------------------------

// horizontal text starts top-left, vertical text top-right
void Window_ResetCursor(Window_t* w)
{
	Rect_t area;

	Window_GetTextArea(w, &area);
	switch(Window_GetDrawStyle(w))
	{
		case 0:
			w->curX = area.l;
			w->curY = area.t;
			break;
		case 1:
			w->curX = area.r;
			w->curY = area.t;
			break;
		default: break;
	}
}

/* advance to the next line.  Horizontal: back to the left edge and down
 * one line advance - but only when a further full line would still fit
 * below (0 otherwise, the x move having happened anyway).  Vertical: one
 * advance to the left, back to the top; always 1. */
int Window_NewLine(Window_t* w)
{
	Rect_t area;
	int adv;

	Window_GetTextArea(w, &area);
	adv = Window_LineAdvance(w);
	switch(Window_GetDrawStyle(w))
	{
		case 0:
			w->curX = area.l;
			if(w->curY + 2 * adv > area.b + 1)
				return 0;
			w->curY += adv;
			return 1;
		case 1:
			w->curX -= adv;
			w->curY = area.t;
			return 1;
		default:
			return 0;
	}
}

/* does an n-pixel advance stay inside the text area?  (Returns n for
 * an unknown draw style - the original leaves the argument in eax.) */
int Window_CursorFits(Window_t* w, int n)
{
	Rect_t area;

	Window_GetTextArea(w, &area);
	switch(Window_GetDrawStyle(w))
	{
		case 0: return w->curX + n <= area.r + 1;
		case 1: return w->curY + n <= area.b + 1;
		default: return n;
	}
}

// move the cursor n pixels along the writing direction (right, or down)
void Window_CursorAdvance(Window_t* w, int n)
{
	switch(Window_GetDrawStyle(w))
	{
		case 0: w->curX += n; break;
		case 1: w->curY += n; break;
		default: break;
	}
}

void Window_SetCursor(Window_t* w, int x, int y) // "91 8C"
{
	w->curX = x;
	w->curY = y;
}

void Window_GetCursor(Window_t* w, int32_t out[2]) // "91 8D"
{
	out[0] = w->curX;
	out[1] = w->curY;
}

/* "91 8E": 1 when the cursor sits at the start of a line, i.e.
 * at the area's leading edge plus the text engine's indent. */
int Window_CursorAtLineStart(Window_t* w)
{
	int32_t cur[2];
	Rect_t area;

	Window_GetCursor(w, cur);
	Window_GetTextArea(w, &area);
	switch(Window_GetDrawStyle(w))
	{
		case 0: return cur[0] == area.l + Text_GetIndent();
		case 1: return cur[1] == area.t + Text_GetIndent();
		default: return 0;
	}
}

// "90 A7": the 16 per-item colours of the text menus; NULL drops the table
void Window_SetTable(Window_t* w, const int32_t* table)
{
	int i;

	w->hasTable = table != NULL;
	if(table)
		for(i = 0; i < 16; i++)
			w->table[i] = table[i];
}

// copy the table out; returns hasTable (nothing is copied without one)
int Window_GetTable(Window_t* w, int32_t out[16])
{
	int i;

	if(w->hasTable)
		for(i = 0; i < 16; i++)
			out[i] = w->table[i];
	return w->hasTable;
}
