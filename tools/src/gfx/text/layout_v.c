/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * layout_v.c - the vertical rich text layout (columns right to left): the
 *              layout core, the ruby placement beside the base characters,
 *              and the alignment pass (layout_internal.h)
 *
 * The vertical layout is the horizontal one with x and y swapped and a
 * few differences: every glyph takes a square cell of the font size (no
 * proportional placement), brackets and dashes are rotated and small kana
 * and punctuation shifted towards the top right (Text_VertClass), the
 * reading of a ruby word runs down beside the word, and there is no
 * inline markup, underline or Latin word handling.
 */
#include "layout_internal.h"

/* The layout core: one node per glyph, top to bottom, columns `adv`
 * pixels apart from right to left, from the cursor `cur` (the column's x
 * is its right edge) inside `area`.  A control byte 4 .. 8
 * at the start is consumed (with `hang` and gHangBrackets it, or an
 * opening bracket as the first character, hangs by one cell: later
 * columns start that much down); 0x0A in the text is a column break,
 * other control bytes are skipped.  With `hang` closing punctuation stays
 * in its column (it may run into the bottom reserve of one size) and an
 * opening bracket never ends one.  With `rubyOn` every column keeps the
 * ruby size on its right and starts at the indent, and a dictionary word
 * is kept together.  *outLines receives the column count, `cur` the
 * cursor behind the text.  1 ok, 0 when the font is unknown. */
int Text_LayoutCoreV(RichState_t* rich, int32_t* outLines, const char* str, int rubyOn, RubyDict_t* dict,
	int32_t cur[2], const Rect_t* area, int adv, int font, int proportional,
	int hang, uint32_t colour, const int32_t style[5])
{
	FontInfo_t fi;
	Bmp_t cell;
	RichChar_t** link = &rich->head;
	int hasLeading = 0;
	int size, half, rubySize, sdx, sdy, sdxOn, sdyOn;
	int lineStartOffset = 0, hangSize = 0;
	int rubySkip = 0, rubyCharsLeft = 0, punctSkip = 0, delay = 0;
	uint32_t pendingClose = 0;
	int pos = 0;
	char word[0x400], run[0x100];

	if(!BmpMgr_FontInfo(gTextBmpMgr, &fi, font))
		return 0;
	// only the bracket control bytes matter here; the bracket itself is not needed
	if((uint8_t)str[0] >= 4 && (uint8_t)str[0] <= 8)
	{
		hasLeading = 1;
		str++;
	}
	size = fi.size;
	half = (size + 1) >> 1;
	rubySize = rubyOn ? Text_RubySize(size) : 0;
	sdx = Layout_Min1(Layout_Pct(size, style[1]));
	sdy = Layout_Min1(Layout_Pct(size, style[2]));
	sdxOn = style[0] ? sdx : 0;
	sdyOn = style[0] ? sdy : 0;
	Bmp_AllocScreen(&cell, size, size, 1);

	if(hang)
	{
		hangSize = size;
		if(gHangBrackets)
		{
			// a leading control byte hangs a full cell; a bracket as the first
			// character hangs its own width (a full or a half cell)
			int isBracket = Text_IsHangBracket(str);
			if(!isBracket)
			{
				if(hasLeading)
					lineStartOffset = size + gCharPitch;
			}
			else if(hasLeading)
				lineStartOffset = size + gCharPitch;
			else
			{
				uint32_t code;
				int l = TextGetChar(&code, str);
				lineStartOffset = (TextIsWide(code, l) ? size : half) + gCharPitch;
			}
		}
	}
	if(rubyOn)
	{
		// the ruby margin ("91 98"): the first column only when it starts at the top
		if(cur[1] == area->t)
			cur[1] += Text_GetIndent();
		lineStartOffset += Text_GetIndent();
	}

	*outLines = 1;
	while(str[pos])
	{
		Bmp_t view = cell;
		Glyph_t g;
		RichChar_t* node;
		const char* p = str + pos;
		const char* nextPtr;
		uint32_t code;
		int32_t cls[3];
		int len, step, nextPos, closingFollows = 0, limit;
		uint8_t c = (uint8_t)*p;

		if(c < 0x20)
		{
			if(c == 0xa)
			{
				cur[0] -= adv;
				cur[1] = area->t + lineStartOffset;
				(*outLines)++;
			}
			pos++;
			continue;
		}

		node = Layout_NewNode(delay);
		len = TextGetChar(&code, p); // (every vertical cell is a full square: the width flag is not needed)
		Bmp_Fill(&view, NULL, 0);
		Text_PaintGlyph(&view, &g, (int)code, fi.font, colour);
		Text_VertClass(cls, p);
		if(cls[0])
			Bmp_RotateSquare(&view);
		node->kind = cls[2] != 0; // shifted small kana / punctuation
		Layout_GlyphBitmap(&node->glyph, size + sdxOn, size + sdyOn, &view, &g, (int)code, fi.font, style, sdx, sdy, cls[0]);

		step = size + gCharPitch;
		nextPos = pos + len;
		nextPtr = str + nextPos;
		if(rubyOn)
		{
			if(rubySkip > 0)
				rubySkip--;
			else if(RubyDict_MatchPrefix(word, p, dict))
			{
				// the reading needs (half the byte count) full cells
				int ext = (int)((uint32_t)strlen(word) >> 1) * (size + gCharPitch);
				int n;
				if(step < ext)
					step = ext;
				node->rubyWord = BGI_Strdup(word);
				node->rubyExtent = ext;
				n = SjisCodes(NULL, word) - 1;
				rubySkip = rubyCharsLeft = n;
				nextPtr = p + strlen(word);
			}
		}
		if(hang)
		{
			if(punctSkip > 0)
				punctSkip--;
			else
			{
				int n = Text_CollectClosing(run, nextPtr);
				if(n > 0)
				{
					// closing punctuation must stay in this column: reserve its
					// cells now and remember the last one so that it may still
					// be placed when the column is full
					char ch[TEXT_CHAR_MAX];
					// (the original counts the run's bytes / 2: its closers are all double-byte)
					int chars = gTextUtf8 ? SjisCodes(NULL, run) : (int)((uint32_t)strlen(run) >> 1);
					step += chars * (size + gCharPitch);
					punctSkip = n + rubyCharsLeft;
					Text_NthChar(ch, run, n - 1);
					// (text function 0 of 1.573 on switches this rule off)
					closingFollows = gTextFunc[0] ? Text_IsClosing(ch) : 0;
					if(closingFollows)
						TextGetChar(&pendingClose, ch);
				}
				else if(Text_IsOpening(p) && *nextPtr)
				{
					// an opening bracket never ends a column: room for the next character too
					uint32_t nc;
					int nl = TextGetChar(&nc, nextPtr);
					step += TextIsWide(nc, nl) ? size : half;
				}
			}
		}

		// the bottom limit: the reserve is given up when closing punctuation follows
		limit = area->b - (closingFollows ? 0 : hangSize) + 1;
		if(cur[1] + step > limit)
		{
			if(code != pendingClose)
			{
				cur[0] -= adv;
				cur[1] = area->t + lineStartOffset;
				(*outLines)++;
			}
			else
				pendingClose = 0; // the remembered closing character overhangs
		}
		// the glyphs sit left of the cursor x, leaving the ruby size free on
		// the right; a shifted character moves by its class percentages
		node->x = cur[0] + Layout_Pct(size, cls[1]) - rubySize - size + 1;
		node->y = cur[1] + Layout_Pct(size, cls[2]);
		cur[1] += size + gCharPitch;
		*link = node;
		link = &node->next;
		delay += gRubyCharDelay;
		pos = nextPos;
	}
	BGI_Free(cell.pixels);
	return 1;
}

/* the vertical ruby emitter: square cells, the reading runs down the
 * column at `x` from `y`, its `n` glyphs spread evenly over `extent` (at
 * least one size apart) and rotated like the base text would be.  A
 * recoloured shadow is not rotated (as in the original).  Each glyph gets
 * a share of the word's reveal delay; the new nodes (kind 2) are spliced
 * in behind `base`. */
static void RubyEmitV(RichChar_t* base, const RubyNode_t* entry, const FontInfo_t* rfi, uint32_t colour,
	const int32_t style[5], int x, int y, int extent, int delay)
{
	int size = rfi->size;
	int sdx = Layout_Min1(Layout_Pct(size, style[1]));
	int sdy = Layout_Min1(Layout_Pct(size, style[2]));
	int sdxOn = style[0] ? sdx : 0;
	int sdyOn = style[0] ? sdy : 0;
	int n = entry->readingChars;
	int perChar = extent / n;
	int startY, delayStep, curDelay, i;
	Bmp_t cell;
	RichChar_t* head = NULL;
	RichChar_t** link = &head;

	if(perChar < size)
		perChar = size;
	startY = y + size / 8 + (extent - (n - 1) * perChar - size) / 2;
	delayStep = (int)((uint32_t)(entry->wordChars * gRubyCharDelay) / (uint32_t)n);
	curDelay = delay + (int)((uint32_t)delayStep >> 1);
	Bmp_AllocScreen(&cell, size, size, 1);
	for(i = 0; i < n; i++)
	{
		int code = entry->readingCodes[i];
		Glyph_t g;
		int32_t cls[3];
		char buf[3];
		RichChar_t* node = Layout_NewNode(curDelay);

		node->x = x;
		node->y = startY;
		node->kind = 2;
		Bmp_Fill(&cell, NULL, 0);
		Font_PaintGlyph(&cell, &g, code, rfi->font, colour);
		// the class tables want a string: rebuild the character's bytes
		buf[0] = (char)(code >= 0x100 ? code >> 8 : code);
		buf[1] = (char)(code >= 0x100 ? code & 0xff : 0);
		buf[2] = 0;
		Text_VertClass(cls, buf);
		if(cls[0])
			Bmp_RotateSquare(&cell);
		Layout_GlyphBitmap(&node->glyph, size + sdxOn, size + sdyOn, &cell, &g, code, rfi->font, style, sdx, sdy, 0);
		*link = node;
		link = &node->next;
		curDelay += delayStep;
		startY += perChar;
	}
	*link = base->next;
	base->next = head;
	BGI_Free(cell.pixels);
}

/* open the ruby font (the base face at Text_RubySize, or the "91 97" /
 * "92 97" overrides) and emit the reading of every node that starts a
 * dictionary word, running down from the node's top, `rsz` right of its
 * left edge (plus the "91 97" offset).  1 ok, 0 when the base font is
 * unknown or the ruby font cannot be opened. */
int Text_RubyPlaceV(RichState_t* rich, int font, uint32_t colour, const int32_t style[5], RubyDict_t* dict)
{
	FontInfo_t fi, rfi;
	int handle, rsz;
	int32_t rstyle[5];
	RichChar_t* c;

	if(!BmpMgr_FontInfo(gTextBmpMgr, &fi, font))
		return 0;
	rsz = gRubySizeFixed > 0 ? gRubySizeFixed : Text_RubySize(fi.size);
	if(BmpMgr_FontOpen(gTextBmpMgr, &handle, gRubyFace[0] ? gRubyFace : fi.name, rsz,
		   gRubyWidthPct > 0 ? gRubyWidthPct : fi.widthPct, gRubyBold != -1 ? gRubyBold : fi.bold) != 0)
		return 0;
	BmpMgr_FontInfo(gTextBmpMgr, &rfi, handle);
	memcpy(rstyle, style, sizeof rstyle);
	if(gRubyShadow != -1)
		rstyle[3] = gRubyShadow; // the ruby's own shadow colour
	for(c = rich->head; c; c = c->next)
	{
		RubyNode_t entry;
		if(!c->rubyWord)
			continue;
		RubyDict_Find(&entry, c->rubyWord, dict);
		RubyEmitV(c, &entry, &rfi, colour, rstyle, c->x + rsz + gRubyDx, c->y + gRubyDy, c->rubyExtent, c->delay);
	}
	return 1;
}

/* the vertical "swing" alignment: columns are keyed by x, their extent is
 * the top plus one size per glyph (shifted kana count towards the current
 * column without opening a new one).  1 centres every column in the area
 * (minus the overhang reserve when it does not reach the bottom, minus the
 * indent), 2 aligns it to the bottom leaving room for the cursor picture;
 * 0 leaves the columns alone.  The cursor moves with the last column when
 * it is not at the top. */
void Text_AlignV(RichState_t* rich, int32_t cur[2], const Rect_t* area, int font, int hang,
	const int32_t style[5], int swing)
{
	LineRec_t head = {0, 0, 0x80000000, NULL};
	LineRec_t* rec = &head;
	RichChar_t* c;
	FontInfo_t fi;
	int size = 0, cursorItemH, shift = 0;

	if(swing == 0)
		return;
	if(BmpMgr_FontInfo(gTextBmpMgr, &fi, font))
		size = fi.size;
	// pass 1: one record per column with its bottom
	for(c = rich->head; c; c = c->next)
	{
		if(c->kind == 0 && rec->key != c->x)
		{
			rec->next = (LineRec_t*)BGI_Alloc(sizeof(LineRec_t));
			rec = rec->next;
			rec->start = c->y;
			rec->extent = c->y;
			rec->key = c->x;
			rec->next = NULL;
		}
		if(c->kind == 0 || c->kind == 1)
			rec->extent += size;
	}
	cursorItemH = gTextItems ? gTextItems[0].h : 0;

	// pass 2: shift every node by its column's amount
	rec = &head;
	for(c = rich->head; c; c = c->next)
	{
		if(c->kind == 0 && rec->key != c->x)
		{
			rec = rec->next;
			if(swing == 1)
			{
				int reserve = hang && rec->extent < area->b ? size : 0;
				shift = (area->b - rec->extent - reserve - Text_GetIndent()) >> 1;
			}
			else if(swing == 2)
				shift = area->b - rec->extent - cursorItemH;
		}
		c->y += shift;
	}
	if(area->t < cur[1])
		cur[1] += shift;
	Layout_FreeLineRecs(head.next);
}
