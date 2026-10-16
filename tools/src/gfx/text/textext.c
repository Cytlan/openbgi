/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * textext.c - the OpenBGI extensions of the text engine: their settings
 *             and the helpers the "EF xx" instructions call
 *             (inc/bgi/gfx/text.h, docs/obgi.md)
 *
 * No original build has any of this.  The settings are globals like the
 * engine's own (text.c) and start out off, so a game that never issues an
 * "EF" instruction is laid out exactly as before:
 *
 *   gTextDirection  right-to-left and mixed-direction text (layout_bidi.c)
 *   gTextWordWrap   line breaks between words for the generations whose
 *                   layout breaks anywhere (layout_h.c)
 *   gTextScroll     message text longer than its window scrolls
 *                   (src/gfx/window/window_doc.c, src/wait/wait_text_scroll.c)
 *   gTextArrows     the pictures of the two scroll arrows
 *
 * The helpers measure text with the same layout the message output uses:
 * Text_Fit tells how much of a string fills a number of lines (a script
 * splits a long message into backlog records with it), Text_LineWidth how
 * wide a string is (to centre it).
 */
#include "text_internal.h"

int32_t gTextDirection;
int32_t gTextWordWrap;
int32_t gTextScroll;
int32_t gTextScrollMs = 120;
TextArrow_t gTextArrows[2] = {{{0}, -1, -1}, {{0}, -1, -1}};

void Text_SetDirection(int mode) // "EF 01"
{
	gTextDirection = (mode == 1 || mode == 2) ? mode : 0;
}

void Text_SetWordWrap(int on) // "EF 03"
{
	gTextWordWrap = on != 0;
}

void Text_SetScrolling(int on, int msPerLine) // "EF 08"
{
	gTextScroll = on != 0;
	if(msPerLine > 0)
		gTextScrollMs = msPerLine;
}

/* "EF 09" (one arrow of the two): take a private copy of script bitmap
 * `bmp` as the picture of arrow `which` (0 up, 1 down), or go back to the
 * built-in triangle for -1, and remember where the arrow goes.  1 ok, 0
 * when the bitmap does not exist (the position is stored anyway). */
int Text_SetArrow(int which, int bmp, int x, int y)
{
	TextArrow_t* a = &gTextArrows[which & 1];
	Bmp_t src;

	a->x = x;
	a->y = y;
	BGI_Free(a->bmp.pixels);
	memset(&a->bmp, 0, sizeof a->bmp);
	if(bmp < 0)
		return 1;
	if(!BmpMgr_GetInfo(gTextBmpMgr, &src, bmp))
		return 0;
	Bmp_AllocScreen(&a->bmp, src.w, src.h, 1);
	Bmp_Fill(&a->bmp, NULL, 0);
	Bmp_Blit(&a->bmp, 0, 0, &src, 0x80, 0);
	return 1;
}

/* The picture of arrow `which`: the script's when one is set.  Otherwise
 * a filled triangle two thirds of the font size wide is painted into
 * *tmp in `colour`, row by row, with the two pixels at the ends of a row
 * half transparent to soften the edge; the caller frees tmp->pixels. */
const Bmp_t* Text_ArrowBitmap(int which, int size, uint32_t colour, Bmp_t* tmp)
{
	int w, h, row;

	memset(tmp, 0, sizeof *tmp);
	if(gTextArrows[which & 1].bmp.pixels)
		return &gTextArrows[which & 1].bmp;
	w = (size * 2 / 3) | 1; // odd, so that the tip is one pixel
	if(w < 7)
		w = 7;
	h = (w + 1) / 2;
	Bmp_AllocScreen(tmp, w, h, 1);
	Bmp_Fill(tmp, NULL, 0);
	colour &= 0xffffffu;
	for(row = 0; row < h; row++)
	{
		int k = which ? h - 1 - row : row; // the row's distance from the tip
		Rect_t r;
		r.t = r.b = row;
		r.l = w / 2 - k;
		r.r = w / 2 + k;
		Bmp_Fill(tmp, &r, colour | 0x80000000u);
		if(k > 0)
		{
			r.l++;
			r.r--;
			Bmp_Fill(tmp, &r, colour | 0xff000000u);
		}
	}
	return tmp;
}

// 1 for a character lines may break next to without a space: the full-width (CJK) characters
static int BreaksAnywhere(uint32_t code, int len)
{
	if(!gTextUtf8)
		return len > 1 || code >= 0x80; // double-byte characters and the half-width katakana
	return (code >= 0x2e80 && code <= 0xd7ff) || (code >= 0xf900 && code <= 0xfaff) || (code >= 0xfe30 && code <= 0xfe4f) ||
		(code >= 0xff00 && code <= 0xffef) || code >= 0x20000;
}

/* The word starting at `p` for the word wrap of "EF 03": everything up to
 * a space, a control byte or a character lines may break next to.  It is
 * copied to `out` (which must hold strlen(p) + 1 bytes); the byte count, 0
 * when `p` does not start a word. */
int Text_WrapWordRun(char* out, const char* p)
{
	int n = 0;
	for(;;)
	{
		uint32_t code;
		int len = TextGetChar(&code, p + n);
		if(code <= 0x20 || BreaksAnywhere(code, len))
			break;
		n += len;
	}
	memcpy(out, p, (size_t)n);
	out[n] = 0;
	return n;
}

/* "EF 0A": see text.h.  The string is laid out by the message layout from
 * the start of a line of the window; the first glyph that lands on line
 * `lines` (counted from 0) marks the cut.  When that leaves more than
 * `maxBytes` bytes, the cut moves back to the start of the last line that
 * is within the limit, or - a single line being longer than the limit - to
 * the last character that is. */
int Text_Fit(Window_t* win, char* dst, const char* src, int lines, int maxBytes, int rubyOn, int hang)
{
	RichState_t rich;
	RichChar_t* c;
	Rect_t area;
	int32_t cur[2], lineCount, style[5];
	uint32_t savedColour = gTextLastColour;
	int total = (int)strlen(src), cut = total, adv, font, n;

	dst[0] = 0;
	font = Window_GetFont(win);
	if(!font || maxBytes < 1)
		return 0;
	if(lines < 1)
		lines = 1;
	Window_GetTextArea(win, &area);
	adv = Window_LineAdvance(win);
	cur[0] = area.l;
	cur[1] = area.t;
	memset(&rich, 0, sizeof rich);
	Text_MakeStyle(style, 0, 0, 0, 0, 0);
	if(!Text_LayoutCoreH(&rich, &lineCount, src, rubyOn, &gRubyDict, cur, &area, adv, font, Window_GetProportional(win),
		   hang, 0xffffff, style))
		return 0;
	gTextLastColour = savedColour; // measuring must not disturb what "92 9B" reports
	for(c = rich.head; c; c = c->next)
	{
		if(c->kind != 2 && adv > 0 && (c->y - area.t) / adv >= lines)
		{
			cut = c->srcOfs;
			break;
		}
	}
	if(cut > maxBytes)
	{
		int lineStart = 0, lastLine = -1, lastChar = 0;
		for(c = rich.head; c; c = c->next)
		{
			int line;
			if(c->kind == 2 || c->srcOfs > maxBytes)
				continue;
			line = adv > 0 ? (c->y - area.t) / adv : 0;
			if(line > lastLine)
			{
				lastLine = line;
				lineStart = c->srcOfs;
			}
			if(c->srcOfs > lastChar)
				lastChar = c->srcOfs;
		}
		cut = lineStart > 0 ? lineStart : lastChar;
		if(cut <= 0) // not even one character: hand the bytes over as they are rather than loop forever
			cut = maxBytes;
	}
	RichState_Free(&rich);

	// the part without the break that ended it (a line break, or the space the wrap swallowed)
	n = cut;
	while(n > 0 && (src[n - 1] == '\n' || src[n - 1] == ' ') && cut < total)
		n--;
	memcpy(dst, src, (size_t)n);
	dst[n] = 0;
	return cut;
}

/* "EF 0B": the width of `str` set on one line in font `font`: from the
 * left edge of its first glyph to the right edge of its last, in whatever
 * order the layout places them.  0 for an empty string or an unknown font. */
int Text_LineWidth(const char* str, int font, int proportional)
{
	RichState_t rich;
	RichChar_t* c;
	Rect_t area = {0, 0, 0xfffff, 0xfffff};
	int32_t cur[2] = {0, 0}, lineCount, style[5];
	uint32_t savedColour = gTextLastColour;
	int left = 0x7fffffff, right = -0x7fffffff;

	memset(&rich, 0, sizeof rich);
	Text_MakeStyle(style, 0, 0, 0, 0, 0);
	if(!Text_LayoutCoreH(&rich, &lineCount, str, 0, &gRubyDict, cur, &area, 1, font, proportional, 0, 0xffffff, style))
		return 0;
	gTextLastColour = savedColour;
	for(c = rich.head; c; c = c->next)
	{
		if(c->x < left)
			left = c->x;
		if(c->x + c->glyph.w > right)
			right = c->x + c->glyph.w;
	}
	RichState_Free(&rich);
	return right > left ? right - left : 0;
}
