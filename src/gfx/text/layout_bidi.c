/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * layout_bidi.c - the rich text layout for right-to-left and
 *                 mixed-direction text (inc/bgi/gfx/text.h)
 *
 * An OpenBGI extension ("EF 01", docs/obgi.md): Text_LayoutCoreH hands the
 * string to Text_LayoutCoreBidi while gTextDirection is not 0, and gets
 * back the same kind of node list, so the paced output, the immediate
 * drawing and the bitmap text all work unchanged.  Direction 2 is also
 * the layout for left-to-right languages other than Japanese and Chinese
 * (step 2 below).
 *
 * What it does, per paragraph (the text between line breaks):
 *
 *   1. joins Arabic letters (Arabic_Shape: each letter becomes its
 *      isolated, initial, medial or final presentation form) and resolves
 *      the direction of every character (Bidi_Levels);
 *   2. builds the glyph bitmaps and measures them.  Every character that
 *      is not an East Asian full-width one is placed by the font's own
 *      advance: Arabic letters must touch, and the original's rule - one
 *      byte is half a cell, anything else a full one - would give every
 *      accented Latin letter and every Cyrillic one a full cell.  (In a
 *      Japanese fixed-pitch font an ASCII letter advances half a cell, so
 *      plain Latin text looks as it does in the original layout.)  East
 *      Asian characters keep their full cell;
 *   3. breaks the paragraph into lines between words;
 *   4. puts each line's characters into visual order (Bidi_Reorder) and
 *      places them - from the right edge leftwards in a right-to-left
 *      paragraph (direction 1), from the left edge in direction 2.
 *
 * The nodes are linked in reading order and get their reveal delays in
 * that order, so the paced output "types" Arabic from the right.  Not
 * supported here: the readings of the ruby dictionary, the leading
 * control bytes (underline, hung bracket), the kinsoku rules and the
 * inline markup of 1.494 on.
 *
 * The cursor.  In a right-to-left paragraph the text grows to the left,
 * and the cursor that the layout returns is where the click-wait picture
 * goes: one glyph width left of the text's end.  A layout that starts
 * with the cursor anywhere but at the area's left edge (where a fresh
 * line leaves it) continues that line from there.
 */
#include "layout_internal.h"
#include "bgi/bidi.h"
#include "bgi/os.h"

typedef struct BidiChar // one character of the string
{
	uint32_t code;      // what is drawn, as a code of the text encoding: the character, its joined form or its mirror image; 0 = nothing
	int32_t ofs;        // its byte offset in the string
	uint8_t wide;       // a full-width character (fixed-pitch cell)
	uint8_t natural;    // placed by the font's advance: every character that is not an East Asian full-width one
	uint8_t mark;       // a combining mark: no advance, centred over the letter before it
	uint8_t space;      // a space: a line may end here
	uint8_t breakable;  // a full-width character: a line may break before or after it
	int32_t advance;    // pixels the pen moves
	int32_t dx;         // where the node's bitmap starts, relative to the pen
	int32_t inkL, inkR; // the ink's first and last column relative to the pen
	int32_t penX;       // the pen position it was placed at
	RichChar_t* node;   // its glyph (NULL: none)
} BidiChar_t;

typedef struct BidiCtx // what the steps of one layout share
{
	const FontInfo_t* fi;
	int size, glyphW; // the font size and the full-width advance
	int proportional;
	int sdx, sdy, sdxOn, sdyOn; // the shadow offset and the room it takes on a glyph bitmap
	uint32_t colour;
	const int32_t* style;
	Bmp_t cell; // scratch bitmap glyphs are painted into
} BidiCtx_t;

/* an East Asian full-width character (kana, ideographs, Hangul, their
 * punctuation and the full-width forms): it keeps the fixed cell of the
 * original layout.  Everything else - Latin with its accents, Cyrillic,
 * Greek, Arabic, Hebrew, the typographic punctuation - is as wide as its
 * font makes it, which for a Japanese fixed-pitch font is half a cell for
 * ASCII, as before. */
static int IsEastAsianWide(uint32_t cp)
{
	return (cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) ||
		(cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60) ||
		(cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x20000 && cp <= 0x3fffd);
}

// the Unicode code point of a code of the text encoding (the same number in UTF-8 mode)
static uint32_t CodePointOf(uint32_t code)
{
	if(gTextUtf8 || code < 0x80)
		return code;
	if(code < 0x100)
		return 0xff61 + (code - 0xa1); // the half-width katakana
	return OS_SjisToCodePoint((uint16_t)code);
}

/* build the glyph of one character and measure it (step 2 above): the
 * node with its bitmap, the advance and where the bitmap sits relative to
 * the pen */
static void BuildGlyph(BidiCtx_t* x, BidiChar_t* ch, int delay)
{
	Bmp_t view = x->cell;
	Glyph_t g;
	int width;

	ch->node = Layout_NewNode(delay);
	ch->node->srcOfs = ch->ofs;
	Bmp_Fill(&view, NULL, 0);
	Text_PaintGlyph(&view, &g, (int)ch->code, x->fi->font, x->colour);
	if(ch->natural)
	{
		// the ink where the font puts it, the pen moved by the font's advance
		Bmp_CropGlyph(&view, &g);
		width = view.w;
		ch->dx = g.left;
		ch->advance = ch->mark ? 0 : FontRaster_Advance(x->fi->font, (int)ch->code);
		if(!ch->mark && ch->advance <= 0)
			ch->advance = g.right + 1; // the font cannot measure: the ink's extent
	}
	else if(x->proportional)
	{
		// as the left-to-right layout: the ink with its gap split to both sides
		int gap;
		Bmp_CropGlyph(&view, &g);
		gap = Font_GapFor2(&g, x->glyphW);
		width = view.w;
		ch->dx = gap >> 1;
		ch->advance = width + gap;
	}
	else
	{
		// fixed pitch: a full or half cell
		width = ch->wide ? x->glyphW : x->glyphW / 2;
		ch->dx = 0;
		ch->advance = width + Layout_Pitch(0, x->fi);
	}
	ch->inkL = ch->dx;
	ch->inkR = ch->dx + (g.right - g.left);
	Layout_GlyphBitmap(&ch->node->glyph, width + x->sdxOn, x->size + x->sdyOn, &view, &g, (int)ch->code, x->fi->font,
		x->style, x->sdx, x->sdy, 0);
}

/* place the characters s .. e-1 (one line) in visual order starting at pen
 * position `x0`, at height `y`; the marks are then centred over the letter
 * they follow.  Returns the pen position after the line. */
static int PlaceLine(BidiChar_t* c, const uint32_t* cp, const uint8_t* levels, int s, int e, int base, int x0, int y)
{
	int n = e - s, v, k, x = x0;
	int* order;

	if(n <= 0)
		return x0;
	order = (int*)BGI_Alloc((size_t)n * sizeof(int));
	Bidi_Reorder(cp + s, levels + s, n, base, order);
	for(v = 0; v < n; v++)
	{
		BidiChar_t* ch = &c[s + order[v]];
		ch->penX = x;
		if(ch->node)
		{
			ch->node->x = x + ch->dx;
			ch->node->y = y;
		}
		x += ch->advance;
	}
	for(k = s; k < e; k++)
	{
		int b;
		if(!c[k].mark || !c[k].node)
			continue;
		for(b = k - 1; b >= s && (c[b].mark || !c[b].node); b--)
			;
		if(b >= s)
		{
			int centre = c[b].penX + (c[b].inkL + c[b].inkR + 1) / 2;
			c[k].node->x = centre - (c[k].inkR - c[k].inkL + 1) / 2;
		}
	}
	BGI_Free(order);
	return x;
}

// See text.h and the top of this file.
int Text_LayoutCoreBidi(RichState_t* rich, int32_t* outLines, const char* str, int rubyOn, RubyDict_t* dict,
	int32_t cur[2], const Rect_t* area, int adv, int font, int proportional,
	int hang, uint32_t colour, const int32_t style[5])
{
	FontInfo_t fi;
	BidiCtx_t x;
	BidiChar_t* c;
	uint32_t *cp, *shaped;
	uint8_t* levels;
	RichChar_t** link = &rich->head;
	int base = gTextDirection == 1; // the paragraph level: 1 = right to left
	int count = 0, pos, a, i, delay = 0, rubySize, indent, reserve;
	int left, right, lineL, lineR, full, pen, avail, partial;
	size_t cap = strlen(str) + 1;

	(void)dict;
	if(!BmpMgr_FontInfo(gTextBmpMgr, &fi, font))
		return 0;
	memset(&x, 0, sizeof x);
	x.fi = &fi;
	x.size = fi.size;
	x.glyphW = FontInfo_GlyphW(&fi);
	x.proportional = proportional;
	x.sdx = Layout_Min1(Layout_Pct(fi.size, style[1]));
	x.sdy = Layout_Min1(Layout_Pct(fi.size, style[2]));
	x.sdxOn = Layout_ShadowReserve(style, x.sdx);
	x.sdyOn = Layout_ShadowReserve(style, x.sdy);
	x.colour = colour;
	x.style = style;
	Bmp_AllocScreen(&x.cell, 3 * fi.size, 2 * fi.size, 1);
	rubySize = rubyOn ? Text_RubySize(fi.size) : 0; // the lines keep the room above them that the left-to-right layout leaves for readings
	indent = rubyOn ? Text_GetIndent() : 0;
	reserve = hang ? fi.size : 0; // free room at the line's end, where the click cursor goes

	c = (BidiChar_t*)BGI_Calloc(cap * sizeof(BidiChar_t));
	cp = (uint32_t*)BGI_Alloc(cap * sizeof(uint32_t));
	shaped = (uint32_t*)BGI_Alloc(cap * sizeof(uint32_t));
	levels = (uint8_t*)BGI_Alloc(cap);

	// the string as characters; a line break is kept as a character of its own (code 0x0A), other control bytes are dropped
	for(pos = 0; str[pos];)
	{
		uint32_t code;
		int len = TextGetChar(&code, str + pos);
		if(code >= 0x20 || code == 0x0a)
		{
			c[count].code = code;
			c[count].ofs = pos;
			c[count].wide = (uint8_t)TextIsWide(code, len);
			cp[count] = CodePointOf(code);
			count++;
		}
		pos += len;
	}

	// the line geometry: text runs from lineL to lineR (exclusive), starting at the paragraph's leading edge
	left = area->l;
	right = area->r + 1;
	lineL = base ? left + reserve : left + indent;
	lineR = base ? right - indent : right - reserve;
	full = lineR - lineL;
	if(base)
	{
		pen = cur[0] == left ? lineR : cur[0] + x.glyphW; // the right end of the room left on the cursor's line
		if(pen > lineR)
			pen = lineR;
		avail = pen - lineL;
	}
	else
	{
		pen = cur[0] == left ? lineL : cur[0];
		avail = lineR - pen;
	}
	partial = avail < full; // the first line continues earlier text
	*outLines = 1;

	for(a = 0; a <= count;)
	{
		int b, s;
		// the paragraph a .. b-1
		for(b = a; b < count && c[b].code != 0x0a; b++)
			;
		if(gTextUtf8)
			Arabic_Shape(cp + a, b - a, shaped + a);
		else
			memcpy(shaped + a, cp + a, (size_t)(b - a) * sizeof(uint32_t)); // code page 932 has no letters that join
		Bidi_Levels(cp + a, b - a, base, levels + a);
		for(i = a; i < b; i++)
		{
			BidiChar_t* ch = &c[i];
			BidiClass_t cls = Bidi_ClassOf(cp[i]);
			uint32_t drawn = shaped[i];
			if((levels[i] & 1) && (gTextUtf8 || drawn < 0x80))
				drawn = Bidi_Mirror(drawn); // a bracket in right-to-left text opens the other way
			ch->space = cp[i] == 0x20 || cp[i] == 0x3000;
			ch->breakable = !Bidi_IsRtlScript(cp[i]) && (ch->wide && cp[i] >= 0x2e80);
			ch->natural = (uint8_t)!IsEastAsianWide(cp[i]);
			ch->mark = ch->natural && cls == BIDI_NSM;
			if(drawn == 0 || cls == BIDI_BN)
			{ // an alef inside a lam-alef ligature, or an invisible character: nothing to draw
				ch->code = 0;
				continue;
			}
			if(gTextUtf8)
				ch->code = drawn;
			else if(drawn != cp[i])
				ch->code = drawn; // a mirrored ASCII bracket
			BuildGlyph(&x, ch, delay);
			delay += gRubyCharDelay;
			*link = ch->node;
			link = &ch->node->next;
		}

		// break the paragraph into lines and place them
		for(s = a;;)
		{
			int j, width = 0, lastBreak = -1, e, next, x0, end;
			for(j = s; j < b; j++)
			{
				BidiChar_t* ch = &c[j];
				if(ch->space)
				{ // a space may stick out past the margin: it is dropped when the line ends at it
					width += ch->advance;
					lastBreak = j + 1;
					continue;
				}
				if(ch->breakable && j > s)
					lastBreak = j;
				if(width + ch->advance > avail && (j > s || partial))
					break;
				width += ch->advance;
				if(ch->breakable)
					lastBreak = j + 1;
			}
			if(j >= b)
				e = next = b;
			else if(lastBreak > s && lastBreak <= j)
				e = next = lastBreak;
			else if(partial)
			{ // the first word does not fit behind the earlier text: start it on a line of its own
				cur[1] += adv;
				(*outLines)++;
				pen = base ? lineR : lineL;
				avail = full;
				partial = 0;
				continue;
			}
			else
				e = next = j; // a word longer than a line is cut where the line ends
			if(next < b)
			{ // a wrapped line: the spaces it ends with take no room
				int k;
				for(k = e - 1; k >= s && c[k].space; k--)
					c[k].advance = 0;
			}
			for(width = 0, j = s; j < e; j++)
				width += c[j].advance;
			x0 = base ? pen - width : pen;
			end = PlaceLine(c, cp, levels, s, e, base, x0, cur[1] + rubySize);
			cur[0] = base ? x0 - x.glyphW : end;
			if(base && cur[0] <= left)
				cur[0] = left + 1; // "at the left edge" means a fresh line
			if(next >= b)
				break;
			// the next line of the paragraph
			s = next;
			cur[1] += adv;
			(*outLines)++;
			pen = base ? lineR : lineL;
			avail = full;
			partial = 0;
		}
		if(b >= count)
			break;
		// the line break that ended the paragraph
		cur[1] += adv;
		(*outLines)++;
		pen = base ? lineR : lineL;
		avail = full;
		partial = 0;
		cur[0] = base ? lineR - x.glyphW : lineL;
		a = b + 1;
	}

	BGI_Free(levels);
	BGI_Free(shaped);
	BGI_Free(cp);
	BGI_Free(c);
	BGI_Free(x.cell.pixels);
	gTextLastColour = colour; // for "92 9B"
	return 1;
}

/* The "swing" alignment (Text_AlignH) for a right-to-left paragraph, whose
 * lines end at the area's right edge: swing 1 moves every line to the
 * middle of the area; swing 2, right-aligned, is how they already stand.
 * A line is the run of nodes sharing a y.  The cursor moves with the last
 * line. */
void Text_AlignBidi(RichState_t* rich, int32_t cur[2], const Rect_t* area, int swing)
{
	RichChar_t *c, *lineStart;
	int shift = 0;

	if(swing != 1)
		return;
	for(lineStart = rich->head; lineStart;)
	{
		int minX = lineStart->x, maxX = lineStart->x + lineStart->glyph.w;
		for(c = lineStart; c && c->y == lineStart->y; c = c->next)
		{
			if(c->x < minX)
				minX = c->x;
			if(c->x + c->glyph.w > maxX)
				maxX = c->x + c->glyph.w;
		}
		shift = ((area->l + area->r + 1) - (minX + maxX)) / 2;
		for(; lineStart != c; lineStart = lineStart->next)
			lineStart->x += shift;
	}
	if(cur[0] != area->l)
		cur[0] += shift;
}

/* The plain one-line text of the bitmap manager ("92 1C") for
 * bidirectional text: lay the string out without a margin to wrap at and
 * blit the glyphs so that the leftmost one starts at x.  A string with
 * line breaks comes out as several lines, `size` pixels apart, aligned on
 * the side the text starts from.  See text.h. */
int Text_DrawBidiLine(Bmp_t* dst, int* measure, int x, int y, const char* str, int font, uint32_t colour, int effect,
	int level, int proportional)
{
	RichState_t rich;
	RichChar_t* c;
	FontInfo_t fi;
	Rect_t area = {0, 0, 0xfffff, 0xfffff};
	int32_t cur[2] = {0, 0}, lineCount, style[5];
	uint32_t savedColour = gTextLastColour;
	int left = 0x7fffffff, right = -0x7fffffff;

	*measure = 0;
	if(!BmpMgr_FontInfo(gTextBmpMgr, &fi, font))
		return 0;
	memset(&rich, 0, sizeof rich);
	Text_MakeStyle(style, 0, 0, 0, 0, 0);
	Text_LayoutCoreBidi(&rich, &lineCount, str, 0, NULL, cur, &area, fi.size, font, proportional, 0, colour, style);
	gTextLastColour = savedColour;
	for(c = rich.head; c; c = c->next)
	{
		if(c->x < left)
			left = c->x;
		if(c->x + c->glyph.w > right)
			right = c->x + c->glyph.w;
	}
	for(c = rich.head; c; c = c->next)
		Bmp_Blit(dst, x + c->x - left, y + c->y, &c->glyph, effect, level);
	if(right > left)
		*measure = right - left;
	RichState_Free(&rich);
	return 1;
}
