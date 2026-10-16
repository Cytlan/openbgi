/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/text.c - unit test of the glyph rasteriser, the font cache, the
 *                ruby dictionary and the tag parser of the text engine
 *                (src/gfx/font.c and src/gfx/text/, declared in
 *                inc/bgi/gfx/font.h and inc/bgi/gfx/text.h); built as
 *                bin/test_text by `make test`
 *
 * The glyph tests need a Japanese font on the machine (the POSIX back end
 * finds one through fontconfig or the usual directories); when none can be
 * opened they are skipped with a note.  The checks are structural - the
 * glyph cell has ink inside the reported columns and none outside, a
 * full-width character is wider than a half-width one, the LRU cache
 * recycles - since the exact pixels depend on the installed font.  Strings
 * are Shift-JIS, written as hex escapes with the text in a comment.
 *
 * The groups of tests, in the order they run:
 *
 *   raster     - FontRaster_Init's result codes, the glyph record of a
 *                full-width, a half-width and a blank character, the
 *                recycling of the glyph cache, a bold face
 *   font cache - FontCache_Request / AmountFor / Open / GetInfo: the
 *                result codes, requested cache amounts, the same font
 *                found again and another size opened as a new handle
 *   ruby       - the ruby dictionary (parse, find, prefix match, remove,
 *                clear), the "91 95" tag list of a message, SjisCodes
 *   markup     - the inline markup of 1.494 on in the horizontal layout:
 *                ruby tags, colours, bold / italic, links with their
 *                table, delays, the "91 9E" / "91 9F" string helpers, and
 *                the tags treated as plain text in an older build
 *   extensions - the layout that OpenBGI adds (docs/obgi.md): lines broken
 *                between words in an old generation, the width of a
 *                string, right-to-left and mixed-direction lines, the
 *                font's own width for characters that are not East Asian,
 *                and the full-width forms in UTF-8 text
 */
#include "bgi/gfx/font.h"
#include "bgi/gfx/text.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/strutil.h"
#include "bgi/version.h"
#include "bgi/os.h"

#include <stdio.h>

static int gFails; // the number of failed checks so far
// evaluate a condition; print it with its location and count a failure when it is false
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

/* ink columns of a glyph cell (2 * size wide, size high): the first and last column with
 * a non-zero byte, -1 when blank */
static void InkColumns(const Glyph_t* g, int size, int* first, int* last)
{
	int x, y, w = 2 * size;
	*first = -1;
	*last = -1;
	for(x = 0; x < w; x++)
		for(y = 0; y < size; y++)
			if(g->pixels[y * w + x])
			{
				if(*first < 0)
					*first = x;
				*last = x;
				break;
			}
}

/* The rasteriser: FontRaster_Init refuses a capacity below 2
 * (0x80000001), a size outside 4..200 (0x80000002) and a width outside
 * 25..200 (0x80000003); with a Japanese face the glyph record of a
 * full-width character has to agree with its pixels (left / right are the
 * inked columns, top / bottom the whole cell), a half-width one has to be
 * narrower and a space blank; a cache of 4 glyphs serves 6 different ones
 * and still returns the right code; a bold face at another size opens.
 * Skipped when no Japanese font can be opened.  A failure means the glyph
 * bounds, the cache or the parameter checks changed. */
static void TestRaster(void)
{
	FontRaster_t* f = FontRaster_New();
	Glyph_t g, g2;
	int r, first, last, i;
	const int size = 24;
	printf("raster\n");
	CHECK(FontRaster_Init(f, "\x82\x6c\x82\x72 \x83\x53\x83\x56\x83\x62\x83\x4e", size, 100, 0, 0, 1) == (int)0x80000001); // capacity < 2
	CHECK(FontRaster_Init(f, "x", 3, 100, 0, 0, 8) == (int)0x80000002);                                                    // size 4..200
	CHECK(FontRaster_Init(f, "x", size, 10, 0, 0, 8) == (int)0x80000003);                                                  // width 25..200
	r = FontRaster_Init(f, "\x82\x6c\x82\x72 \x83\x53\x83\x56\x83\x62\x83\x4e", size, 100, 0, 0, 4);                       // ＭＳ ゴシック (MS Gothic)
	if(r != 0)
	{
		printf("  (no Japanese font available: glyph checks skipped, %08x)\n", (unsigned)r);
		FontRaster_Delete(f);
		return;
	}
	CHECK(f->open && f->size == size && f->glyphW == size && f->cellW == 2 * size);

	// a full-width character: inked, bounds inside the cell and consistent with the
	// pixels
	CHECK(FontRaster_Glyph(f, &g, 0x82a0) != NULL); // あ (the hiragana "a")
	CHECK(g.code == 0x82a0 && g.dbcs == 1);
	CHECK(g.top == 0 && g.bottom == size - 1);
	InkColumns(&g, size, &first, &last);
	CHECK(first >= 0);
	CHECK(g.left == first && g.right == last);
	CHECK(g.right - g.left > size / 2); // wider than half a cell
	// a half-width character is narrower, and a space has no ink at all
	CHECK(FontRaster_Glyph(f, &g2, 'i') != NULL);
	CHECK(g2.dbcs == 0);
	CHECK(g2.right - g2.left < g.right - g.left);
	CHECK(FontRaster_Glyph(f, &g2, ' ') != NULL);
	InkColumns(&g2, size, &first, &last);
	CHECK(first == -1);

	// the cache holds 4 glyphs: asking for 6 different ones recycles the oldest, the
	// newest stay valid
	for(i = 0; i < 6; i++)
		CHECK(FontRaster_Glyph(f, &g2, 0x82a0 + 2 * i) != NULL);
	CHECK(f->used <= f->capacity);
	CHECK(FontRaster_Glyph(f, &g2, 0x82a0 + 10) != NULL && g2.code == 0x82a0 + 10);

	// bold is a separate face setting that still opens; at size 16 and width 50 the glyph
	// is 8 columns wide
	FontRaster_Close(f);
	CHECK(FontRaster_Init(f, "\x82\x6c\x82\x72 \x83\x53\x83\x56\x83\x62\x83\x4e", 16, 50, 1, 0, 8) == 0);
	CHECK(f->glyphW == 8);
	CHECK(FontRaster_Glyph(f, &g, 'W') != NULL);
	FontRaster_Delete(f);
}

/* The font cache: FontCache_Request has the rasteriser's result codes
 * for the amount, size and width; a requested amount is returned by
 * FontCache_AmountFor for exactly that font and the default for any
 * other; FontCache_Open hands out handle 1 for the first font, the same
 * handle for the same request and 2 for another size; FontCache_GetInfo
 * copies the entry (with the rasteriser's capacity) and returns 0 for an
 * unknown handle.  Skipped when no Japanese font can be opened. */
static void TestCache(void)
{
	FontCache_t* c = FontCache_New();
	FontInfo_t info;
	int h1 = 0, h2 = 0, h3 = 0, r;
	printf("font cache\n");
	CHECK(FontCache_Request(c, "x", 20, 100, 0, 1) == (int)0x80000001);
	CHECK(FontCache_Request(c, "x", 300, 100, 0, 8) == (int)0x80000002);
	CHECK(FontCache_Request(c, "x", 20, 300, 0, 8) == (int)0x80000003);
	CHECK(FontCache_Request(c, "\x82\x6c\x82\x72 \x83\x53\x83\x56\x83\x62\x83\x4e", 20, 100, 0, 7) == 0); // ＭＳ ゴシック (MS Gothic)
	CHECK(FontCache_AmountFor(c, "\x82\x6c\x82\x72 \x83\x53\x83\x56\x83\x62\x83\x4e", 20, 100, 0) == 7);
	CHECK(FontCache_AmountFor(c, "\x82\x6c\x82\x72 \x83\x53\x83\x56\x83\x62\x83\x4e", 21, 100, 0) == c->defaultAmount);
	r = FontCache_Open(c, &h1, "\x82\x6c\x82\x72 \x83\x53\x83\x56\x83\x62\x83\x4e", 20, 100, 0);
	if(r != 0)
	{
		printf("  (no Japanese font available: cache checks skipped)\n");
		FontCache_Delete(c);
		return;
	}
	CHECK(h1 == 1);
	// the same request finds the open font; a different size opens another
	CHECK(FontCache_Open(c, &h2, "\x82\x6c\x82\x72 \x83\x53\x83\x56\x83\x62\x83\x4e", 20, 100, 0) == 0 && h2 == h1);
	CHECK(FontCache_Open(c, &h3, "\x82\x6c\x82\x72 \x83\x53\x83\x56\x83\x62\x83\x4e", 24, 100, 0) == 0 && h3 == 2);
	CHECK(FontCache_GetInfo(c, &info, h3) == 1);
	CHECK(info.size == 24 && info.font != NULL && info.font->capacity == c->defaultAmount);
	CHECK(FontCache_GetInfo(c, &info, 99) == 0);
	CHECK(FontInfo_GlyphW(&info) == 24);
	FontCache_Delete(c);
}

/* The ruby dictionary and the tag list: RubyDict_ParseList reads
 * "word\reading\n" lines, RubyDict_Find looks a word up (whole words
 * only), RubyDict_MatchPrefix finds the longest registered word at the
 * start of a text, RubyDict_Remove answers 0 the second time and
 * RubyDict_Clear empties the dictionary.  Text_ParseTags ("91 95") lists
 * every registered word of a message as "word\reading" lines, and
 * SjisCodes counts characters and returns their codes.  A failure means
 * ruby readings would attach to the wrong words. */
static void TestRuby(void)
{
	RubyDict_t d;
	RubyNode_t n;
	char word[0x100], tags[0x400];
	int count;
	printf("ruby\n");
	memset(&d, 0, sizeof d);
	// "word\reading\n" lines; 1 when the whole list was consumed
	count = RubyDict_ParseList(&d, "\x8a\xbf\x8e\x9a\\\x82\xa9\x82\xf1\x82\xb6\n\x93\xfa\x96\x7b\\\x82\xc9\x82\xd9\x82\xf1\n"); // 漢字\かんじ 日本\にほん (kanji, Japan, with their readings)
	CHECK(count == 1);
	CHECK(RubyDict_Find(&n, "\x8a\xbf\x8e\x9a", &d) == 1); // 漢字
	CHECK(RubyDict_Find(&n, "\x93\xfa\x96\x7b", &d) == 1); // 日本
	CHECK(strcmp(n.reading, "\x82\xc9\x82\xd9\x82\xf1") == 0);
	CHECK(RubyDict_Find(&n, "\x93\xfa", &d) == 0); // 日 alone: a prefix is not a word
	// the longest registered word at the start of a text
	RubyDict_Insert(&d, "\x93\xfa\x96\x7b\x8c\xea", "\x82\xc9\x82\xd9\x82\xf1\x82\xb2");    // 日本語 (Japanese)
	CHECK(RubyDict_MatchPrefix(word, "\x93\xfa\x96\x7b\x8c\xea\x82\xc5\x82\xb7", &d) == 1); // 日本語です (it is Japanese)
	CHECK(strcmp(word, "\x93\xfa\x96\x7b\x8c\xea") == 0);
	CHECK(RubyDict_MatchPrefix(word, "\x82\xa0\x93\xfa\x96\x7b", &d) == 0); // あ日本: the word does not start the text
	CHECK(RubyDict_Remove(&d, "\x93\xfa\x96\x7b") == 1);
	CHECK(RubyDict_Remove(&d, "\x93\xfa\x96\x7b") == 0);
	CHECK(RubyDict_Find(&n, "\x93\xfa\x96\x7b\x8c\xea", &d) == 1);
	RubyDict_Clear(&d);
	CHECK(RubyDict_Find(&n, "\x93\xfa\x96\x7b\x8c\xea", &d) == 0);

	// the ruby "tags" of a message: every registered word in the text, as "word\reading"
	// lines
	Ruby_Clear();
	Ruby_Add("\x8a\xbf\x8e\x9a", "\x82\xa9\x82\xf1\x82\xb6");                         // 漢字 かんじ
	count = Text_ParseTags(tags, "\x82\xa0\x8a\xbf\x8e\x9a\x82\xa2\x8a\xbf\x8e\x9a"); // あ漢字い漢字 (the word twice between hiragana)
	CHECK(count == 2);
	CHECK(strcmp(tags, "\x8a\xbf\x8e\x9a\\\x82\xa9\x82\xf1\x82\xb6\n\x8a\xbf\x8e\x9a\\\x82\xa9\x82\xf1\x82\xb6\n") == 0);
	CHECK(Text_ParseTags(tags, "\x82\xa0\x82\xa2") == 0); // あい: no registered word
	Ruby_Clear();

	// character counting and codes (the literal is split so that the hex escape does not
	// swallow the 'b')
	{
		int32_t codes[8];
		CHECK(SjisCodes(NULL, "a\x82\xa0"
							  "b") == 3);
		CHECK(SjisCodes(codes, "a\x82\xa0"
							   "b") == 3);
		CHECK(codes[0] == 'a' && codes[1] == 0x82a0 && codes[2] == 'b');
	}
}


/* the nodes of a horizontal layout: count, and the ones of a kind (0 normal, 1 shifted
 * small, 2 ruby) */
static int CountNodes(const RichState_t* r, int kind, int* ofKind)
{
	const RichChar_t* c;
	int n = 0;
	*ofKind = 0;
	for(c = r->head; c; c = c->next, n++)
		if(c->kind == kind)
			(*ofKind)++;
	return n;
}

// the n-th node of a layout in reading order, NULL past the end
static RichChar_t* NthNode(const RichState_t* r, int n)
{
	RichChar_t* c = r->head;
	while(c && n-- > 0)
		c = c->next;
	return c;
}

/* the colour of the first fully opaque pixel (alpha >= 0xf0) of a glyph,
 * 0xffffffff when there is none */
static uint32_t FirstInkColour(const Bmp_t* b)
{
	int x, y;
	for(y = 0; y < b->h; y++)
	{
		const uint32_t* row = (const uint32_t*)(b->pixels + (size_t)y * b->pitch);
		for(x = 0; x < b->w; x++)
			if((row[x] >> 24) >= 0xf0)
				return row[x] & 0xffffffu;
	}
	return 0xffffffffu;
}

/* The inline markup of the 1.494+ horizontal layout (Text_LayoutCoreH
 * under the 1.553 profile): tags are consumed and produce no glyph, <R>
 * and <ruby> attach a reading to the base character and Text_RubyPlaceH
 * adds the ruby nodes, <c> colours the glyphs, <b> / <i> switch to
 * temporary fonts (an italic glyph bitmap is wider by half its width),
 * <l> registers a link with the position of its first glyph in the link
 * colour, <t n> restarts the per-character delay at n, and </> escapes a
 * '<'.  Then the string helpers of "91 9E" / "91 9F" (Text_CountLinks,
 * Text_StripTags), and the same text under the 1.69/444 profile, where
 * the tags are laid out as plain characters.  Skipped when no Japanese
 * font can be opened.  A failure means the markup parser or the node list
 * it builds changed. */
static void TestMarkup(void)
{
	BmpMgr_t* mgr = BmpMgr_New(8);
	RichState_t rich;
	RubyDict_t dict;
	Rect_t area = {0, 0, 639, 199};
	int32_t cur[2], lines, style[5] = {0, 0, 0, 0, 0};
	int font = 0, n, k, w;
	const char* face = "\x82\x6c\x82\x72 \x83\x53\x83\x56\x83\x62\x83\x4e"; // ＭＳ ゴシック (MS Gothic)
	RichChar_t* c;
	TextLink_t links[TEXT_LINKS_MAX];
	char buf[0x100];

	printf("markup\n");
	Text_SetBmpMgrPtr(mgr);
	if(BmpMgr_FontOpen(mgr, &font, face, 24, 100, 0) != 0)
	{
		printf("  (no Japanese font available: layout checks skipped)\n");
		BmpMgr_Delete(mgr);
		return;
	}
	Engine_SelectProfile("1.553");
	CHECK(gEngine->gen == GEN_1_553);
	Gfx_SetScreenMode(PM_ARGB32);
	memset(&rich, 0, sizeof rich);
	memset(&dict, 0, sizeof dict);

	// "「突然どうしたの、<Rまお>真乎</R>先輩？」" ("What is the matter all of a
	// sudden, Mao-senpai?"): 15 visible characters, a ruby of two glyphs (まお)
	// over the name 真乎, nothing drawn for the tags
	cur[0] = 0;
	cur[1] = 0;
	CHECK(Text_LayoutCoreH(&rich, &lines, "\x81\x75\x93\xcb\x91\x52\x82\xc7\x82\xa4\x82\xb5\x82\xbd\x82\xcc\x81\x41<R\x82\xdc\x82\xa8>\x90\x5e\x8c\xc3</R>\x90\xe6\x94\x79\x81\x48\x81\x76",
			  1, &dict, cur, &area, 24, font, 0, 1, 0xffffff, style) == 1);
	n = CountNodes(&rich, 2, &k);
	CHECK(n == 15 && k == 0);
	c = NthNode(&rich, 9); // 真, the base character of the ruby word
	CHECK(c && c->rubyWord && strcmp(c->rubyWord, "\x90\x5e\x8c\xc3") == 0);
	CHECK(Text_RubyPlaceH(&rich, font, 0xffffff, style, &dict) == 1);
	n = CountNodes(&rich, 2, &k);
	CHECK(n == 17 && k == 2); // the two ruby glyphs were added
	CHECK(lines == 1);
	RichState_Free(&rich);
	RubyDict_Clear(&dict);

	// <ruby word,reading> registers a dictionary word; <c> colours; </> escapes a '<'
	cur[0] = 0;
	cur[1] = 0;
	CHECK(Text_LayoutCoreH(&rich, &lines, "<ruby \x93\xfa\x96\x7b,\x82\xc9\x82\xd9\x82\xf1>\x93\xfa\x96\x7b<c ff0000>a</c>b</><x>", // <ruby 日本,にほん>日本<c ff0000>a</c>b</><x>
			  1, &dict, cur, &area, 24, font, 0, 0, 0xffffff, style) == 1);
	n = CountNodes(&rich, 2, &k);
	CHECK(n == 7); // 日本ab<x>
	c = NthNode(&rich, 0);
	CHECK(c && c->rubyWord && strcmp(c->rubyWord, "\x93\xfa\x96\x7b") == 0);
	CHECK(FirstInkColour(&NthNode(&rich, 2)->glyph) == 0xff0000); // "a" in red
	CHECK(FirstInkColour(&NthNode(&rich, 3)->glyph) == 0xffffff); // "b" back in the text colour
	RichState_Free(&rich);
	RubyDict_Clear(&dict);

	// <b> / <i>: temporary fonts; an italic glyph bitmap is wider by half its width
	cur[0] = 0;
	cur[1] = 0;
	CHECK(Text_LayoutCoreH(&rich, &lines, "\x82\xa0<i>\x82\xa0</i><b>\x82\xa0</b>\x82\xa0", 0, &dict, cur, &area, 24, font, 0, 0, // あ<i>あ</i><b>あ</b>あ
			  0xffffff, style) == 1);
	n = CountNodes(&rich, 2, &k);
	CHECK(n == 4);
	w = NthNode(&rich, 0)->glyph.w;
	CHECK(w == 24);
	CHECK(NthNode(&rich, 1)->glyph.w == w + w / 2);
	CHECK(NthNode(&rich, 2)->glyph.w == w);
	CHECK(NthNode(&rich, 3)->glyph.w == w);
	RichState_Free(&rich);

	// <l>: the link is registered with the position of its first glyph; <t> restarts the
	// delay
	cur[0] = 10;
	cur[1] = 20;
	Text_SetLinkColour(0x00ff00);
	CHECK(Text_LayoutCoreH(&rich, &lines, "ab<l>cd</l>e<t 3>f", 0, &dict, cur, &area, 24, font, 0, 0, 0xffffff, style) == 1);
	n = CountNodes(&rich, 2, &k);
	CHECK(n == 6);
	CHECK(FirstInkColour(&NthNode(&rich, 2)->glyph) == 0x00ff00); // "c" in the link colour
	CHECK(FirstInkColour(&NthNode(&rich, 4)->glyph) == 0xffffff); // "e" in the text colour again
	CHECK(NthNode(&rich, 5)->delay == 3 * gRubyCharDelay);        // "f": the delay restarted at 3 by <t 3>
	CHECK(NthNode(&rich, 4)->delay == 4 * gRubyCharDelay);        // "e": the fifth character
	CHECK(gTextLinkCount == 1 && strcmp(gTextLinks[0].text, "cd") == 0 && gTextLinks[0].x == NthNode(&rich, 2)->x);
	CHECK(gTextLinks[0].y == 20);
	CHECK(TextLinks_Take(links) == 1 && strcmp(links[0].text, "cd") == 0 && gTextLinkCount == 0); // taking the table clears it
	Text_SetLinkColour(-1);
	RichState_Free(&rich);

	// the string helpers of "91 9E" / "91 9F" (an empty link is not counted; the
	// lower-case tag is looked for first, so a mix of cases skips sections)
	CHECK(Text_CountLinks(NULL, "x<l>ab</l>y<l>c</l><l></l>") == 2);
	CHECK(Text_CountLinks(links, "x<L>ab</L>y<L>c</L>") == 2 && strcmp(links[1].text, "c") == 0);
	CHECK(Text_CountLinks(NULL, "x<l>ab</l>y<L>c</L><l></l>") == 1);
	CHECK(Text_StripTags(buf, "a<b>b</b> <r x>c</r> 1<2 <") == 10 && strcmp(buf, "ab c 1<2 <") == 0); // a '<' that opens no tag is kept
	CHECK(Text_StripTags(buf, "a<b") == 3 && strcmp(buf, "a<b") == 0);                                // an unclosed tag is plain text

	// before 1.494 the tags are plain text: "a<b>b</b>" lays out as 9 characters
	Engine_SelectProfile("1.69/444");
	cur[0] = 0;
	cur[1] = 0;
	CHECK(Text_LayoutCoreH(&rich, &lines, "a<b>b</b>", 0, &dict, cur, &area, 24, font, 0, 0, 0xffffff, style) == 1);
	n = CountNodes(&rich, 2, &k);
	CHECK(n == 9);
	RichState_Free(&rich);
	Text_SetBmpMgrPtr(NULL);
	BmpMgr_Delete(mgr);
}

// the node of the character at byte `ofs` of the laid-out string, NULL when it has none
static RichChar_t* NodeAt(const RichState_t* r, int ofs)
{
	RichChar_t* c;
	for(c = r->head; c; c = c->next)
		if(c->srcOfs == ofs && c->kind == 0)
			return c;
	return NULL;
}

/* The layout extensions.  All checks are about where the glyphs go, not
 * what they look like, so they hold for any font:
 *
 *   word wrap  - a line of 10 cells and words of 7 letters: the original
 *                1.64 layout fills the line and breaks inside the third
 *                word; with "EF 03" every line starts with a word
 *   width      - Text_LineWidth grows with the string, 0 for none
 *   direction  - "shalom abc 12" in Hebrew letters: in a right-to-left
 *                paragraph the Hebrew word starts at the right edge and
 *                runs leftwards, and "abc 12" stands left of it, read
 *                left to right; in a left-to-right paragraph the Hebrew
 *                word still runs leftwards but stands first
 *   own width  - an accented Latin letter takes a full cell in the
 *                original layout and at most that in direction 2
 *   full width - U+FF01 is drawn from the font in UTF-8 text (its code is
 *                in the range of the private glyph-sheet codes)
 *
 * Skipped when no Japanese font can be opened. */
static void TestLayoutExtensions(void)
{
	BmpMgr_t* mgr = BmpMgr_New(8);
	RichState_t rich;
	RubyDict_t dict;
	Rect_t area = {0, 0, 239, 199}; // ten full cells of 24 pixels
	int32_t cur[2], lines, style[5] = {0, 0, 0, 0, 0};
	int font = 0, mode, starts, inWord, y, gap0, gap2;
	const char* face = "\x82\x6c\x82\x72 \x83\x53\x83\x56\x83\x62\x83\x4e"; // ＭＳ ゴシック (MS Gothic)
	static const char words[] = "aaaaaaa bbbbbbb ccccccc ddddddd";
	static const char mixed[] = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d abc 12"; // שלום abc 12
	RichChar_t* c;
	RichChar_t *shin, *mem, *a, *b, *one, *two;

	printf("extensions\n");
	Text_SetBmpMgrPtr(mgr);
	if(BmpMgr_FontOpen(mgr, &font, face, 24, 100, 0) != 0)
	{
		printf("  (no Japanese font available: layout checks skipped)\n");
		BmpMgr_Delete(mgr);
		return;
	}
	Engine_SelectProfile("1.64");
	Gfx_SetScreenMode(PM_ARGB32);
	memset(&rich, 0, sizeof rich);
	memset(&dict, 0, sizeof dict);

	// word wrap off, then on: where the lines start
	for(mode = 0; mode < 2; mode++)
	{
		Text_SetWordWrap(mode);
		cur[0] = 0;
		cur[1] = 0;
		CHECK(Text_LayoutCoreH(&rich, &lines, words, 0, &dict, cur, &area, 24, font, 0, 1, 0xffffff, style) == 1);
		starts = 0;
		inWord = 0;
		y = -1;
		for(c = rich.head; c; c = c->next)
		{
			CHECK(c->x >= 0 && c->x + 12 <= 240);
			if(c->y != y)
			{
				y = c->y;
				starts++;
				if(c->srcOfs % 8 != 0) // the words start at 0, 8, 16, 24
					inWord++;
			}
		}
		CHECK(starts == 2);
		CHECK(mode ? inWord == 0 : inWord > 0);
		RichState_Free(&rich);
	}
	Text_SetWordWrap(0);

	CHECK(Text_LineWidth("", font, 0) == 0);
	CHECK(Text_LineWidth("ab", font, 0) > 0);
	CHECK(Text_LineWidth("abcd", font, 0) > Text_LineWidth("ab", font, 0));
	CHECK(Text_LineWidth("abcd", font, 0) <= 4 * 24);

	// the same string in a right-to-left and in a left-to-right paragraph
	Text_SetEncoding(TEXT_UTF8);
	for(mode = 1; mode <= 2; mode++)
	{
		Text_SetDirection(mode);
		cur[0] = 0;
		cur[1] = 0;
		CHECK(Text_LayoutCoreH(&rich, &lines, mixed, 0, &dict, cur, &area, 24, font, 0, 1, 0xffffff, style) == 1);
		CHECK(lines == 1);
		shin = NodeAt(&rich, 0);
		mem = NodeAt(&rich, 6);
		a = NodeAt(&rich, 9);
		b = NodeAt(&rich, 10);
		one = NodeAt(&rich, 13);
		two = NodeAt(&rich, 14);
		CHECK(shin && mem && a && b && one && two);
		if(shin && mem && a && b && one && two)
		{
			CHECK(mem->x < shin->x);               // the Hebrew word runs leftwards
			CHECK(a->x < b->x && one->x < two->x); // the Latin word and the number do not
			if(mode == 1)
			{
				CHECK(two->x < mem->x && b->x < one->x); // the Hebrew word at the right; "abc 12" is one left-to-right run (a number after a Latin word takes its direction)
				CHECK(shin->x + shin->glyph.w > 240 - 24 && shin->x + shin->glyph.w <= 240 + 4);
			}
			else
			{
				CHECK(shin->x < a->x && b->x < one->x); // ... and from the left
				CHECK(mem->x >= 0 && mem->x < 24);
			}
		}
		RichState_Free(&rich);
	}

	// "aåb": the cell of å in the original layout, its own width in direction 2
	gap0 = gap2 = 0;
	for(mode = 0; mode <= 2; mode += 2)
	{
		Text_SetDirection(mode);
		cur[0] = 0;
		cur[1] = 0;
		CHECK(Text_LayoutCoreH(&rich, &lines, "a\xc3\xa5"
											  "b",
				  0, &dict, cur, &area, 24, font, 0, 1, 0xffffff, style) == 1);
		a = NodeAt(&rich, 1);
		b = NodeAt(&rich, 3);
		CHECK(a && b);
		if(a && b)
			*(mode ? &gap2 : &gap0) = b->x - a->x;
		RichState_Free(&rich);
	}
	CHECK(gap0 == 24);
	CHECK(gap2 > 0 && gap2 <= 24);

	// "！" (U+FF01) has ink
	Text_SetDirection(0);
	cur[0] = 0;
	cur[1] = 0;
	CHECK(Text_LayoutCoreH(&rich, &lines, "\xef\xbc\x81", 0, &dict, cur, &area, 24, font, 0, 1, 0xffffff, style) == 1);
	c = NodeAt(&rich, 0);
	CHECK(c && FirstInkColour(&c->glyph) != 0xffffffffu);
	RichState_Free(&rich);

	Text_SetEncoding(TEXT_SJIS);
	BmpMgr_Delete(mgr);
}

// run every group; exit status 1 when any check failed
int main(void)
{
	OS_Init();
	TestRaster();
	TestCache();
	TestRuby();
	TestMarkup();
	TestLayoutExtensions();
	OS_Shutdown();
	if(gFails)
	{
		printf("%d failure(s)\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
