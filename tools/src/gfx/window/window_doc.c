/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window_doc.c - the text document of a message window: the glyphs of its
 *                text layer kept as a list, so that text which does not
 *                fit the text area can be scrolled (inc/bgi/gfx/window.h)
 *
 * An OpenBGI extension ("EF 08", docs/obgi.md).  The original draws
 * glyphs into the text layer and forgets them: a message longer than the
 * text area is cut off at its bottom.  With scrolling on, the rich text
 * outputs (the paced one of src/wait/wait_text.c and the immediate
 * Window_DrawText) lay their text out in document coordinates, hand the
 * nodes to this list and draw them through it; a document y appears at
 * y - scrollY in the layer, and moving the view repaints the text area
 * from the nodes.  The scrolling itself - following the text as it is
 * printed, the pause at a full view, the arrows - is the wait class's
 * (src/wait/wait_text_scroll.c).
 */
#include "window_internal.h"

static uint32_t gDocSerial; // a number per document ever created: tells a wait that its document was replaced

// scrolling is on ("EF 08") and the window writes horizontally: its text goes through a document
int Window_DocActive(Window_t* w)
{
	return gTextScroll && Window_GetDrawStyle(w) == 0;
}

// the window's document, created empty (view at the top) when it has none
TextDoc_t* Window_Doc(Window_t* w)
{
	if(!w->doc)
	{
		w->doc = (TextDoc_t*)BGI_Calloc(sizeof(TextDoc_t));
		w->doc->tail = &w->doc->head;
		w->doc->serial = ++gDocSerial;
	}
	return w->doc;
}

// free the nodes of a list
static void FreeNodes(RichChar_t* head)
{
	RichState_t tmp;
	memset(&tmp, 0, sizeof tmp);
	tmp.head = head;
	RichState_Free(&tmp);
}

/* forget the document: called when the text layer is cleared or its
 * geometry changes.  The pixels stay as they are; a text output that still
 * holds nodes of the document notices by the serial number. */
void Window_DocDrop(Window_t* w)
{
	if(!w->doc)
		return;
	FreeNodes(w->doc->head);
	BGI_Free(w->doc);
	w->doc = NULL;
}

int Window_DocScroll(Window_t* w)
{
	return w->doc ? w->doc->scrollY : 0;
}

/* move the nodes of a finished layout (in document coordinates) to the end
 * of the document; `rich` is left empty.  The result is the link the first
 * of them hangs on. */
RichChar_t** Window_DocAppend(Window_t* w, RichState_t* rich)
{
	TextDoc_t* d = Window_Doc(w);
	RichChar_t** link = d->tail;
	*link = rich->head;
	rich->head = NULL;
	while(*d->tail)
		d->tail = &(*d->tail)->next;
	return link;
}

// free the document's nodes from `link` to the end (an output that is laid out again)
void Window_DocCut(Window_t* w, RichChar_t** link)
{
	if(!w->doc || !link)
		return;
	FreeNodes(*link);
	*link = NULL;
	w->doc->tail = link;
}

/* how far down the view may go, in pixels: far enough that the last line
 * of text and the line the cursor is on are inside the text area, rounded
 * up to whole lines so that the lines stay where the window's rules are */
int Window_DocMaxScroll(Window_t* w)
{
	Rect_t area;
	RichChar_t* c;
	int adv, size, bottom, over;

	if(!w->doc)
		return 0;
	Window_GetTextArea(w, &area);
	adv = Window_LineAdvance(w);
	size = Window_GetFontSize(w);
	bottom = w->curY + w->doc->scrollY + adv; // just below the cursor's line, in document coordinates
	for(c = w->doc->head; c; c = c->next)
		if(c->kind != 2 && c->y + size > bottom)
			bottom = c->y + size;
	over = bottom - (area.b + 1);
	if(over <= 0 || adv <= 0)
		return 0;
	return (over + adv - 1) / adv * adv;
}

/* move the view to `y` pixels (not above the top) and repaint; the cursor,
 * which is kept in layer coordinates, moves with the text */
void Window_DocSetScroll(Window_t* w, int y)
{
	TextDoc_t* d = w->doc;
	if(!d)
		return;
	if(y < 0)
		y = 0;
	if(y == d->scrollY)
		return;
	w->curY -= y - d->scrollY;
	d->scrollY = y;
	Window_DocRedraw(w);
}

/* paint the text area from the document: cleared, then every node that has
 * been shown, in the order drawn and the way the paced output draws a
 * glyph (its shape cut out of what is there, then blended in) */
void Window_DocRedraw(Window_t* w)
{
	Rect_t area;
	Bmp_t t;
	RichChar_t* c;
	int h;

	if(!w->doc)
		return;
	Window_UsableArea(w, &area);
	Bmp_Clear(&w->text, &area);
	t = w->text;
	Bmp_Crop(&t, &area);
	h = area.b - area.t + 1;
	for(c = w->doc->head; c; c = c->next)
	{
		int x = c->x - area.l, y = c->y - w->doc->scrollY - area.t;
		if(!c->shown || y + c->glyph.h <= 0 || y >= h)
			continue;
		Bmp_Blit(&t, x, y, &c->glyph, 0x40, 1);
		Bmp_Blit(&t, x, y, &c->glyph, 1, 0);
	}
	Window_Recomposite(w, &area);
}

// 1 when the glyph's line ends inside the text area with the view at `scrollY` (0: it reaches below the bottom)
int Window_DocFits(Window_t* w, const RichChar_t* c, int scrollY)
{
	Rect_t area;
	int size = Window_GetFontSize(w);
	Window_GetTextArea(w, &area);
	return c->y - scrollY + (c->glyph.h < size ? c->glyph.h : size) <= area.b + 1;
}

/* draw one node at its place in the view (Window_DrawBmpToText: clipped to
 * the usable text area, recomposited).  1 and the touched screen rectangle
 * in *outScreen, 0 when the node is outside the area or the blit failed. */
int Window_DocDrawNode(Window_t* w, Rect_t* outScreen, const RichChar_t* c, int effect, int level)
{
	Rect_t area, r;
	int y = c->y - Window_DocScroll(w);

	Window_UsableArea(w, &area);
	Rect_FromBmp(&r, &c->glyph);
	Rect_Offset(&r, c->x, y);
	if(!Rect_Intersects(&r, &area))
		return 0;
	return Window_DrawBmpToText(w, outScreen, c->x, y, &c->glyph, effect, level) == 0;
}

/* Window_DrawText for a window with a document: the string is laid out
 * from the cursor in document coordinates, its nodes join the document as
 * shown ones and are drawn; when the text ends below the text area the
 * view follows it to the end.  The layout's result: 1 drawn, 0 when the
 * font is unknown. */
int Window_DocDrawText(Window_t* w, const char* str, int parseTags, const char* tags, int hang, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5])
{
	TextDoc_t* d = Window_Doc(w);
	RichState_t rich;
	RichChar_t** link;
	RichChar_t* c;
	Rect_t area, r;
	int32_t cur[2], lines;
	int max;

	memset(&rich, 0, sizeof rich);
	Window_GetCursor(w, cur);
	Window_GetTextArea(w, &area);
	cur[1] += d->scrollY;
	if(!Text_LayoutNodes(&rich, &lines, cur, &area, str, parseTags, tags, Window_GetFont(w), Window_GetProportional(w),
		   hang, Window_GetSwingStyle(w), Window_GetSpacing(w), colour, rubyColour, style))
		return 0;
	Window_SetCursor(w, cur[0], cur[1] - d->scrollY);
	link = Window_DocAppend(w, &rich);
	for(c = *link; c; c = c->next)
		c->shown = 1;
	max = Window_DocMaxScroll(w);
	if(max > d->scrollY)
		Window_DocSetScroll(w, max); // repaints everything, the new nodes included
	else
	{
		for(c = *link; c; c = c->next)
		{
			Window_DocDrawNode(w, &r, c, 0x40, 1);
			Window_DocDrawNode(w, &r, c, 1, 0);
		}
	}
	return 1;
}
