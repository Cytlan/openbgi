/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * layout_h.c - the horizontal rich text layout: the layout core, the ruby
 *              placement above the base characters, and the alignment
 *              pass (layout_internal.h)
 */
#include "layout_internal.h"

/* The layout core: one node per glyph, left to right, from the cursor
 * `cur` inside `area`, lines `adv` pixels apart.  A control byte at the
 * start of the string selects underlining (2), the leading bracket to hang
 * (4 .. 8) or, from 1.529, no measuring of that bracket (3); 0x0A in the
 * text is a line break, other control bytes are skipped.  With `hang` the
 * closing punctuation after a character stays on its line (it may run
 * into the right-hand reserve of one glyph size), an opening bracket never
 * ends a line, and a leading bracket hangs into the left margin
 * (gHangBrackets).  With `rubyOn` every line keeps the ruby size above it
 * and starts at the indent; a dictionary word is kept together and its
 * base node remembers the word (Text_RubyPlaceH fills the readings in).
 * The nodes are built for `colour` and the shadow `style`, each with the
 * reveal delay of its position.  From 1.494 on the walk also interprets
 * the inline markup (tags in angle brackets: bold / italic / link fonts,
 * colours, ruby, delays), keeps Latin words together, swallows a space
 * that would end a line, and knows the outline style; those parts are
 * gated on the generation.  *outLines receives the line count, `cur` the
 * cursor behind the text.  1 ok, 0 when the font is unknown.
 *
 * OpenBGI extensions (docs/obgi.md): with gTextWordWrap ("EF 03") the word
 * keeping and the swallowed space also apply before 1.494, and while
 * gTextDirection ("EF 01") is not 0 the whole layout is done by
 * Text_LayoutCoreBidi instead. */
int Text_LayoutCoreH(RichState_t* rich, int32_t* outLines, const char* str, int rubyOn, RubyDict_t* dict,
	int32_t cur[2], const Rect_t* area, int adv, int font, int proportional,
	int hang, uint32_t colour, const int32_t style[5])
{
	FontInfo_t fi;
	Markup_t m;
	Bmp_t cell;
	RichChar_t** link = &rich->head;
	int underline = 0, hasLeading = 0, noHangMeasure = 0;
	uint32_t leadingCode = 0;
	int size, glyphW, rubySize, sdx, sdy, sdxOn, sdyOn;
	int lineStartOffset = 0, hangSize = 0;
	int rubySkip = 0, wordSkip = 0, punctSkip = 0, lineStart = 1;
	uint32_t pendingClose = 0;
	int pos = 0;
	char word[0x400], run[0x100], tagBuf[0x100], buf[TEXT_CHAR_MAX];
	char* wordRun;

	if(gTextDirection != 0) // OpenBGI "EF 01": bidirectional text has a layout of its own
		return Text_LayoutCoreBidi(rich, outLines, str, rubyOn, dict, cur, area, adv, font, proportional, hang, colour, style);
	if(!BmpMgr_FontInfo(gTextBmpMgr, &fi, font))
		return 0;
	memset(&m, 0, sizeof m);
	m.on = gEngine->gen >= GEN_1_494;
	m.base = &fi;
	m.cur = fi;
	m.cur.font = NULL;
	m.fi = &fi;
	m.colour = colour;
	if(m.on)
		TextLinks_Clear();

	// a control byte in front of the text: 2 = underline, 4..8 = a bracket
	// hung into the margin; 3 (1.529 on) keeps a leading bracket from being
	// measured for the hang.  Earlier builds leave 3 in place and skip it
	// like any control.
	if((uint8_t)str[0] < 0x20)
	{
		int c = str[0];
		if(c == 2)
		{
			underline = 1;
			str++;
		}
		else if(c == 3 && gEngine->gen >= GEN_1_529)
		{
			noHangMeasure = 1;
			str++;
		}
		else if(c >= 4 && c <= 8)
		{
			const char* lb = gLeadingBracket[c - 4];
			leadingCode = TextTableChar(&lb);
			hasLeading = 1;
			str++;
		}
	}

	size = fi.size;
	glyphW = FontInfo_GlyphW(&fi);
	rubySize = rubyOn ? Text_RubySize(size) : 0;
	sdx = Layout_Min1(Layout_Pct(size, style[1]));
	sdy = Layout_Min1(Layout_Pct(size, style[2]));
	sdxOn = Layout_ShadowReserve(style, sdx);
	sdyOn = Layout_ShadowReserve(style, sdy);
	// the painting cell: 3 x 2 sizes up to 1.494, 5/4 glyph widths by one
	// size from then on (room for the italic shear)
	if(m.on)
		Bmp_AllocScreen(&cell, (glyphW * 5) / 4, size, 1);
	else
		Bmp_AllocScreen(&cell, 3 * size, 2 * size, 1);
	wordRun = (char*)BGI_Alloc(strlen(str) + 1);

	if(hang)
	{
		hangSize = size;
		if(gHangBrackets && !noHangMeasure)
		{
			// a paragraph that starts with an opening bracket (or carries a
			// leading control byte) hangs it: later lines start that much in
			int isBracket = Text_IsHangBracket(str);
			const char* measured = NULL;
			if(hasLeading)
			{
				buf[TextPutChar(buf, leadingCode)] = 0;
				measured = buf;
			}
			else if(isBracket)
			{
				int len = TextCharLen(str);
				memcpy(buf, str, (size_t)len);
				buf[len] = 0;
				measured = buf;
			}
			if(measured)
			{
				int32_t mm[3];
				Text_MeasureInfo(mm, measured, &fi, proportional);
				lineStartOffset = mm[0] + Layout_Pitch(proportional, &fi);
			}
		}
	}
	if(rubyOn)
	{
		// the ruby margin ("91 98"): the first line only when it starts at the edge
		if(cur[0] == area->l)
			cur[0] += Text_GetIndent();
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
		const FontInfo_t* cf;
		uint32_t code;
		int len, dbcs, width, gap, extra, advanceX, nextPos, closingFollows = 0, limit, skipChars = 0, cw;
		uint8_t c = (uint8_t)*p;

		if(c < 0x20)
		{
			if(c == 0xa)
			{
				cur[1] += adv;
				cur[0] = area->l + lineStartOffset;
				(*outLines)++;
				lineStart = 1;
			}
			pos++;
			continue;
		}

		if(m.on && c == '<' && p[1])
		{
			// "<tag args>": a known tag is applied and skipped, an unknown one
			// skipped; without a closing bracket the '<' is text
			int close = 1;
			while(p[close] && p[close] != '>')
				close++;
			if(close > 1 && p[close] == '>')
			{
				if(m.literal)
					m.literal = 0; // "</>" before it: the '<' is drawn
				else
				{
					int tag = Markup_Match(tagBuf, sizeof tagBuf, p + 1, close - 1);
					if(tag >= 0)
						Markup_Apply(&m, tag, tagBuf + strlen(gMarkupTags[tag].name), p + close + 1, p, dict, cur, area);
					pos += close + 1;
					continue;
				}
			}
		}

		cf = m.fi;
		cw = FontInfo_GlyphW(cf);
		len = TextGetChar(&code, p);
		dbcs = TextIsWide(code, len);
		if((m.on || gTextWordWrap) && hang)
		{
			// a space that would end the line is dropped with a line break
			// instead, unless the one before it was a space too
			int spaceW = dbcs ? cw : cw / 2;
			const char* sp = MSG_TXT_BRACKET_SPACE;
			if((code == 0x20 || code == TextTableChar(&sp)) && pos >= len &&
				memcmp(str + pos - len, p, (size_t)len) != 0 && cur[0] + spaceW > area->r - size + 1)
			{
				lineStart = 1;
				cur[0] = area->l + lineStartOffset;
				cur[1] += adv;
				(*outLines)++;
				pos += len;
				continue;
			}
		}

		node = Layout_NewNode(m.delay);
		node->srcOfs = pos;
		Bmp_Fill(&view, NULL, 0);
		Text_PaintGlyph(&view, &g, (int)code, cf->font, m.colour);
		if(underline || m.linkStart)
		{
			// the underline: the cell's bottom row in the text colour
			Rect_t r = {0, size - 1, view.w - 1, size - 1};
			Bmp_Fill(&view, &r, m.colour | 0xff000000u);
		}
		if(proportional)
		{
			Bmp_CropGlyph(&view, &g);
			gap = Font_GapFor2(&g, cw);
			width = view.w;
		}
		else
		{
			width = dbcs ? cw : cw / 2;
			gap = 0;
		}
		// 1.494 on: the font's extra width, and half a glyph more in italics
		extra = 0;
		if(m.on)
			extra = cf->italic ? (width >> 1) + cf->font->extraW[1] : cf->font->extraW[0];
		Layout_GlyphBitmap(&node->glyph, width + extra + sdxOn, size + sdyOn, &view, &g, (int)code, cf->font, style, sdx,
			sdy, 0);

		// how far the cursor moves; a word, the ruby word and trailing
		// punctuation may widen it
		advanceX = width + gap - (gap >> 1) + Layout_Pitch(proportional, cf);
		nextPos = pos + len;
		nextPtr = str + nextPos;
		if(m.on || gTextWordWrap)
		{
			/* the word that starts here; `chars` is its length in characters.
			 * The original's runs are single-byte text, where that is the
			 * byte count n; the runs of the "EF 03" word wrap for the older
			 * generations may hold multi-byte letters. */
			int n = m.on ? Markup_WordRun(wordRun, p) : Text_WrapWordRun(wordRun, p);
			int chars = n;
			if(!m.on)
			{
				int k;
				for(chars = 0, k = 0; k < n; chars++)
					k += TextCharLen(wordRun + k);
			}
			if(n < 1)
				lineStart = 0;
			else if(wordSkip > 0)
				wordSkip--;
			else if(!lineStart && chars >= 2)
			{
				// a Latin word is kept together: it needs its whole width
				// now (not at the start of a line, where it cannot move)
				int32_t mm[3];
				Text_MeasureInfo(mm, wordRun, cf, proportional);
				if(advanceX < mm[0])
					advanceX = mm[0];
				wordSkip = skipChars = chars - 1;
				nextPtr = p + n;
			}
		}
		if(rubyOn)
		{
			if(rubySkip > 0)
				rubySkip--;
			else if(RubyDict_MatchPrefix(word, p, dict))
			{
				// a dictionary word is kept together too; its base node
				// carries the word for Text_RubyPlaceH
				int32_t mm[3];
				int n;
				Text_MeasureInfo(mm, word, cf, proportional);
				if(advanceX < mm[m.on ? 0 : 2])
					advanceX = mm[m.on ? 0 : 2];
				node->rubyWord = BGI_Strdup(word);
				node->rubyExtent = mm[1];
				n = SjisCodes(NULL, word) - 1;
				rubySkip = wordSkip = skipChars = n;
				nextPtr = p + strlen(word); // kinsoku looks past the whole word
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
					// closing punctuation must stay on this line: reserve its
					// width now and remember the last one so that it may
					// still be placed when the line is full
					int32_t mm[3];
					char ch[TEXT_CHAR_MAX];
					Text_MeasureInfo(mm, run, cf, proportional);
					advanceX += mm[m.on ? 0 : 2];
					punctSkip = n + skipChars;
					Text_NthChar(ch, run, n - 1);
					// (text function 0 of 1.573 on switches this rule off)
					closingFollows = gTextFunc[0] ? Text_IsClosing(ch) : 0;
					if(closingFollows)
						TextGetChar(&pendingClose, ch);
				}
				else if(Text_IsOpening(p) && *nextPtr)
				{
					// an opening bracket never ends a line: it needs room
					// for the character after it as well
					int32_t mm[3];
					int nl = TextCharLen(nextPtr);
					memcpy(run, nextPtr, (size_t)nl);
					run[nl] = 0;
					Text_MeasureInfo(mm, run, cf, proportional);
					advanceX += mm[m.on ? 0 : 2];
				}
			}
		}

		// the right limit: the reserve is given up when closing punctuation follows
		limit = area->r - (closingFollows ? 0 : hangSize) + 1;
		if(cur[0] + advanceX > limit)
		{
			if(code != pendingClose)
			{
				if(m.linkStart)
					m.linkEnd = p; // the link's text ends with its first line
				cur[0] = area->l + lineStartOffset;
				cur[1] += adv;
				(*outLines)++;
				lineStart = 1;
			}
			else
				pendingClose = 0; // the remembered closing character overhangs
		}
		node->x = cur[0] + (gap >> 1);
		node->y = cur[1] + rubySize; // the reading's room is above the glyph
		cur[0] += width + gap + Layout_Pitch(proportional, cf);
		*link = node;
		link = &node->next;
		m.delay += gRubyCharDelay;
		pos = nextPos;
	}
	BGI_Free(wordRun);
	BGI_Free(cell.pixels);
	gTextLastColour = m.colour; // for "92 9B"
	Markup_Free(&m);
	return 1;
}

/* lay the reading of one ruby word out above its base character: the
 * `n` small glyphs are spread evenly over `extent` (at least one glyph
 * width apart), centred, starting size / 8 to the right of `x`, at `y`.
 * Each gets a share of the word's reveal delay, starting at `delay`.  The
 * new nodes (kind 2) are spliced in behind `base`. */
static void RubyEmitH(RichChar_t* base, const RubyNode_t* entry, const FontInfo_t* rfi, uint32_t colour,
	const int32_t style[5], int x, int y, int extent, int delay)
{
	int size = rfi->size;
	int glyphW = FontInfo_GlyphW(rfi);
	int sdx = Layout_Min1(Layout_Pct(size, style[1]));
	int sdy = Layout_Min1(Layout_Pct(size, style[2]));
	int sdxOn = style[0] ? sdx : 0;
	int sdyOn = style[0] ? sdy : 0;
	int n = entry->readingChars;
	int perChar = extent / n;
	int startX, delayStep, curDelay, i;
	Bmp_t cell;
	RichChar_t* head = NULL;
	RichChar_t** link = &head;

	if(perChar < glyphW)
		perChar = glyphW;
	startX = x + size / 8 + (extent - (n - 1) * perChar - glyphW) / 2;
	delayStep = (int)((uint32_t)(entry->wordChars * gRubyCharDelay) / (uint32_t)n);
	curDelay = delay + (int)((uint32_t)delayStep >> 1);
	Bmp_AllocScreen(&cell, 3 * size, 2 * size, 1);
	for(i = 0; i < n; i++)
	{
		int code = entry->readingCodes[i];
		int w = code >= 0x100 ? glyphW : glyphW / 2;
		Glyph_t g;
		RichChar_t* node = Layout_NewNode(curDelay);

		node->x = startX;
		node->y = y;
		node->kind = 2;
		node->srcOfs = base->srcOfs;
		Bmp_Fill(&cell, NULL, 0);
		Font_PaintGlyph(&cell, &g, code, rfi->font, colour);
		Layout_GlyphBitmap(&node->glyph, w + sdxOn, size + sdyOn, &cell, &g, code, rfi->font, style, sdx, sdy, 0);
		*link = node;
		link = &node->next;
		curDelay += delayStep;
		startX += perChar;
	}
	*link = base->next;
	base->next = head;
	BGI_Free(cell.pixels);
}

/* open the ruby font (the base face at Text_RubySize, or the "91 97" /
 * "92 97" overrides) and emit the reading of every node that starts a
 * dictionary word, in `colour` with the `style` shadow.  1 ok, 0 when the
 * base font is unknown or the ruby font cannot be opened. */
int Text_RubyPlaceH(RichState_t* rich, int font, uint32_t colour, const int32_t style[5], RubyDict_t* dict)
{
	FontInfo_t fi, rfi;
	int handle, rsz;
	int32_t rstyle[5];
	RichChar_t* c;

	if(!BmpMgr_FontInfo(gTextBmpMgr, &fi, font))
		return 0;
	// the ruby font: the base face at the ruby size, or the "91 97" override
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
		RubyEmitH(c, &entry, &rfi, colour, rstyle, c->x + gRubyDx, c->y - rsz + gRubyDy, c->rubyExtent, c->delay);
	}
	return 1;
}

// release a line record list (both alignment passes build one)
void Layout_FreeLineRecs(LineRec_t* r)
{
	while(r)
	{
		LineRec_t* n = r->next;
		BGI_Free(r);
		r = n;
	}
}

/* "swing" alignment of the laid-out lines: 1 centres every line in the
 * area (minus the overhang reserve when the line does not reach the
 * margin, plus the shadow offset, minus the indent), 2 right-aligns it
 * leaving room for the cursor picture; 0 leaves the lines alone.  Lines
 * are the runs of normal nodes sharing a y; ruby glyphs follow their line
 * (they shift with the shift in force when they are met).  The cursor
 * moves with the last line when it is not at the left edge. */
void Text_AlignH(RichState_t* rich, int32_t cur[2], const Rect_t* area, int font, int hang,
	const int32_t style[5], int swing)
{
	LineRec_t head = {0, 0x80000000, 0x80000000, NULL};
	LineRec_t* rec = &head;
	RichChar_t* c;
	FontInfo_t fi;
	int size = 0, cursorItemH, sdx, shift = 0;

	if(swing == 0)
		return;
	if(gTextDirection == 1)
	{ // (OpenBGI) right-to-left lines start at the right edge
		Text_AlignBidi(rich, cur, area, swing);
		return;
	}
	// pass 1: one record per line with its right edge
	for(c = rich->head; c; c = c->next)
	{
		int right;
		if(c->kind != 0)
			continue;
		if(rec->key != c->y)
		{
			rec->next = (LineRec_t*)BGI_Alloc(sizeof(LineRec_t));
			rec = rec->next;
			rec->start = c->x;
			rec->extent = 0x80000000;
			rec->key = c->y;
			rec->next = NULL;
		}
		right = c->x + c->glyph.w - 1;
		if(rec->extent < right)
			rec->extent = right;
	}
	if(BmpMgr_FontInfo(gTextBmpMgr, &fi, font))
		size = fi.size;
	cursorItemH = gTextItems ? gTextItems[0].h : 0; // the first cursor picture's height is what a right-aligned line leaves free
	sdx = Layout_Pct(size, style[1]);

	// pass 2: shift every node by its line's amount
	rec = &head;
	for(c = rich->head; c; c = c->next)
	{
		if(c->kind == 0 && rec->key != c->y)
		{
			rec = rec->next;
			if(swing == 1)
			{
				int reserve = hang && rec->extent < area->r ? size : 0;
				shift = (area->r - rec->extent - reserve + sdx - Text_GetIndent()) >> 1;
			}
			else if(swing == 2)
				shift = area->r - rec->extent - cursorItemH + sdx;
		}
		c->x += shift;
	}
	if(area->l < cur[0])
		cur[0] += shift;
	Layout_FreeLineRecs(head.next);
}
