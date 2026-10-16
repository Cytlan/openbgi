/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * layout.c - the entry points of the rich text layout (inc/bgi/gfx/text.h):
 *            Text_LayoutH / Text_LayoutV lay a string out into a text layer
 *            at the cursor, Text_DrawToBitmap into a whole bitmap; and the
 *            RichState list every layout produces
 *
 * These are the immediate layouts: the string is laid out, the readings
 * placed, the lines aligned and everything blitted in one call.  The paced
 * output (src/wait/wait_text.c) calls the same cores through its vtable
 * and reveals the nodes itself.
 */
#include "layout_internal.h"

/* the immediate layout used by Window_DrawText: parse the "word\reading"
 * lines of `tags` into a private dictionary, lay out (with the readings
 * when `parseTags` is set), align, and blit everything into `text` with
 * `effect` / `level`.  `spacing` is the line gap in percent of the font
 * size.  The touched rectangles land in `rects` (one per glyph), their
 * count in *outRectCount, the line count in *outLines; `cur` is advanced.
 * 1 ok, 0 when the font is unknown (nothing is written then). */
static int LayoutAndBlit(int vertical, Bmp_t* text, int* outRectCount, Rect_t* rects, int* outLines, int32_t cur[2],
	const Rect_t* area, const char* str, int parseTags, const char* tags, int font,
	int proportional, int hang, int swing, int spacing, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5], int effect, int level)
{
	FontInfo_t fi;
	RichState_t rich;
	RubyDict_t dict;
	int ok, adv;

	ok = BmpMgr_FontInfo(gTextBmpMgr, &fi, font);
	if(!ok)
		return ok;
	memset(&rich, 0, sizeof rich);
	memset(&dict, 0, sizeof dict);
	RubyDict_ParseList(&dict, tags);
	adv = Font_Percent(fi.size, spacing) + fi.size;
	if(vertical)
	{
		Text_LayoutCoreV(&rich, outLines, str, parseTags, &dict, cur, area, adv, font, proportional, hang, colour, style);
		if(parseTags)
			Text_RubyPlaceV(&rich, font, rubyColour, style, &dict);
		Text_AlignV(&rich, cur, area, font, hang, style, swing);
	}
	else
	{
		Text_LayoutCoreH(&rich, outLines, str, parseTags, &dict, cur, area, adv, font, proportional, hang, colour, style);
		if(parseTags)
			Text_RubyPlaceH(&rich, font, rubyColour, style, &dict);
		Text_AlignH(&rich, cur, area, font, hang, style, swing);
	}
	*outRectCount = Text_Collect(text, rects, &rich, effect, level);
	RichState_Free(&rich);
	RubyDict_Clear(&dict);
	return ok;
}

/* OpenBGI ("EF 08"): the horizontal immediate layout up to the blit - the
 * dictionary from `tags`, the layout, the readings, the alignment - with
 * the nodes left in `rich` for the caller (the text document of a
 * scrolling window, src/gfx/window/window_doc.c).  1 ok, 0 for an unknown
 * font (rich stays empty). */
int Text_LayoutNodes(RichState_t* rich, int32_t* outLines, int32_t cur[2], const Rect_t* area, const char* str,
	int parseTags, const char* tags, int font, int proportional, int hang, int swing, int spacing, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5])
{
	FontInfo_t fi;
	RubyDict_t dict;
	int adv;

	if(!BmpMgr_FontInfo(gTextBmpMgr, &fi, font))
		return 0;
	memset(&dict, 0, sizeof dict);
	RubyDict_ParseList(&dict, tags);
	adv = Font_Percent(fi.size, spacing) + fi.size;
	Text_LayoutCoreH(rich, outLines, str, parseTags, &dict, cur, area, adv, font, proportional, hang, colour, style);
	if(parseTags)
		Text_RubyPlaceH(rich, font, rubyColour, style, &dict);
	Text_AlignH(rich, cur, area, font, hang, style, swing);
	RubyDict_Clear(&dict);
	return 1;
}

// the horizontal immediate layout (see LayoutAndBlit)
int Text_LayoutH(Bmp_t* text, int* outRectCount, Rect_t* rects, int* outLines, int32_t cur[2],
	const Rect_t* area, const char* str, int parseTags, const char* tags, int font,
	int proportional, int hang, int swing, int spacing, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5], int effect, int level)
{
	return LayoutAndBlit(0, text, outRectCount, rects, outLines, cur, area, str, parseTags, tags, font,
		proportional, hang, swing, spacing, colour, rubyColour, style, effect, level);
}

// the vertical immediate layout (see LayoutAndBlit)
int Text_LayoutV(Bmp_t* text, int* outRectCount, Rect_t* rects, int* outLines, int32_t cur[2],
	const Rect_t* area, const char* str, int parseTags, const char* tags, int font,
	int proportional, int hang, int swing, int spacing, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5], int effect, int level)
{
	return LayoutAndBlit(1, text, outRectCount, rects, outLines, cur, area, str, parseTags, tags, font,
		proportional, hang, swing, spacing, colour, rubyColour, style, effect, level);
}

/* "91 9C" / "91 9D" / "92 9C" (BmpOp_DrawText): the horizontal layout of
 * `str` into a whole bitmap starting at (x, y), the bitmap being the
 * area; the rectangle list is discarded.  The Text_LayoutH result. */
int Text_DrawToBitmap(Bmp_t* dst, int* outLines, int x, int y, const char* str, int parseTags,
	const char* tags, int font, int proportional, int hang, int swing, int spacing,
	uint32_t colour, uint32_t rubyColour, const int32_t style[5], int effect, int level)
{
	Rect_t* rects = (Rect_t*)BGI_Alloc(strlen(str) << 6); // four rectangles per byte of the string
	Rect_t area;
	int32_t cur[2];
	int rectCount, r;

	Rect_FromBmp(&area, dst);
	cur[0] = x;
	cur[1] = y;
	gTextLastPos[0] = x; // the original keeps the pen in the globals "92 9B" reads
	gTextLastPos[1] = y;
	r = Text_LayoutH(dst, &rectCount, rects, outLines, cur, &area, str, parseTags, tags, font, proportional,
		hang, swing, spacing, colour, rubyColour, style, effect, level);
	gTextLastPos[0] = cur[0];
	gTextLastPos[1] = cur[1];
	BGI_Free(rects);
	return r;
}

// release every node of a layout (their bitmaps and words) and empty the state
void RichState_Free(RichState_t* r)
{
	RichChar_t* n = r->head;
	while(n)
	{
		RichChar_t* next = n->next;
		BGI_Free(n->rubyWord);
		BGI_Free(n->glyph.pixels);
		BGI_Free(n);
		n = next;
	}
	memset(r, 0, sizeof *r);
}

int FontInfo_GlyphW(const FontInfo_t* fi) // the advance of a full-width glyph
{
	return fi->size * fi->widthPct / 100;
}

// blit every laid-out glyph into `dst` with `effect` / `level`, reporting each rectangle; the count
int Text_Collect(Bmp_t* dst, Rect_t* rects, RichState_t* rich, int effect, int level)
{
	int n = 0;
	RichChar_t* c;

	for(c = rich->head; c; c = c->next, n++)
	{
		Bmp_Blit(dst, c->x, c->y, &c->glyph, effect, level);
		Rect_FromBmp(&rects[n], &c->glyph);
		Rect_Offset(&rects[n], c->x, c->y);
	}
	return n;
}
