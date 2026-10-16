/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * markup.c - the inline markup of message text from 1.494 on: "<b>",
 *            "<i>", "<c rrggbb>", "<l>" links, "<ruby>" / "<r>" readings,
 *            "<cr>", "<t n>" and the "</>" literal (layout_internal.h)
 *
 * The horizontal layout (layout_h.c) calls Markup_Match on every '<' and
 * Markup_Apply on a hit; the state - the current font and colour, the
 * colour stack, the open link - lives in a Markup_t for the duration of
 * one layout.  The clickable links of the last layout are kept in
 * gTextLinks for "92 9E"; the font and the colour the links are drawn in
 * come from "92 9D" and "92 9F".
 */
#include "layout_internal.h"

/* The tags in the order they are tried: the lower-cased tag text is
 * matched exactly, or by prefix so that arguments may follow ("<r まお>");
 * "/" matches exactly only.  Each tag is accepted from its generation on. */
const TagDef_t gMarkupTags[TAG_COUNT] = {
	{"/", GEN_1_494},
	{"b", GEN_1_494},
	{"/b", GEN_1_494},
	{"i", GEN_1_494},
	{"/i", GEN_1_494},
	{"ruby", GEN_1_494},
	{"r", GEN_1_529},
	{"/r", GEN_1_529},
	{"cr", GEN_1_547},
	{"c", GEN_1_529},
	{"/c", GEN_1_529},
	{"l", GEN_1_529},
	{"/l", GEN_1_529},
	{"t", GEN_1_547},
};

/* The clickable "<l>" sections of the last layout: up to TEXT_LINKS_MAX
 * entries holding the text and the position of its first glyph; "92 9E"
 * reads and clears them. */
TextLink_t gTextLinks[TEXT_LINKS_MAX];
int32_t gTextLinkCount;

void TextLinks_Clear(void)
{
	gTextLinkCount = 0;
}

// register a link; 0 when the table is full (the link is dropped)
static int TextLinks_Add(const char* text, int x, int y)
{
	TextLink_t* l;
	if(gTextLinkCount >= TEXT_LINKS_MAX)
		return 0;
	l = &gTextLinks[gTextLinkCount++];
	memset(l, 0, sizeof *l);
	snprintf(l->text, sizeof l->text, "%s", text);
	l->x = x;
	l->y = y;
	return 1;
}

int TextLinks_Take(TextLink_t* out) // "92 9E": copy them out and clear; the count
{
	int n = gTextLinkCount;
	if(n > 0)
	{
		memcpy(out, gTextLinks, (size_t)n * sizeof *out);
		TextLinks_Clear();
	}
	return n;
}

// the link style of "92 9D" / "92 9F"
char gLinkFace[0x34];
int32_t gLinkSize, gLinkWidthPct, gLinkBold, gLinkItalic;
int32_t gLinkColour = -1;

/* "92 9D": the font "<l>" switches to.  0 ok; with a face given, the
 * size and width are checked first: 0x80000004 (size not 4 .. 200),
 * 0x80000005 (width not 25 .. 200).  An empty face keeps the text's own
 * font (the size and width are stored unchecked then).  The face itself is
 * only opened by a layout; 0x80000006 (the face cannot be opened) is never
 * returned here. */
int Text_SetLinkFont(const char* face, int size, int widthPct, int bold, int italic)
{
	if(face && *face)
	{
		if(size < 4 || size > 200)
			return (int)0x80000004;
		if(widthPct < 25 || widthPct > 200)
			return (int)0x80000005;
	}
	memset(gLinkFace, 0, sizeof gLinkFace);
	if(face)
		snprintf(gLinkFace, sizeof gLinkFace, "%s", face);
	gLinkSize = size;
	gLinkWidthPct = widthPct;
	gLinkBold = bold;
	gLinkItalic = italic;
	return 0;
}

void Text_SetLinkColour(int32_t colour) // "92 9F": -1 = the text's colour
{
	gLinkColour = colour;
}

// ---- the markup state of one layout -----------------------------------------------------

// push the current colour (for "</c>" / "</l>" to restore)
static void Markup_Push(Markup_t* m)
{
	if(m->stackCount == m->stackCap)
	{
		uint32_t* grown;
		m->stackCap = m->stackCap ? 2 * m->stackCap : 16;
		grown = (uint32_t*)BGI_Alloc((size_t)m->stackCap * sizeof *grown);
		if(m->stackCount)
			memcpy(grown, m->stack, (size_t)m->stackCount * sizeof *grown);
		BGI_Free(m->stack);
		m->stack = grown;
	}
	m->stack[m->stackCount++] = m->colour;
}

static void Markup_Pop(Markup_t* m) // "</c>" / "</l>": nothing to pop is ignored
{
	if(m->stackCount > 0)
		m->colour = m->stack[--m->stackCount];
}

static void Markup_DropFont(Markup_t* m) // delete the temporary rasteriser, if any
{
	if(m->cur.font)
	{
		FontRaster_Delete(m->cur.font);
		m->cur.font = NULL;
	}
}

static void Markup_UseBase(Markup_t* m) // back to the window's font
{
	Markup_DropFont(m);
	m->fi = m->base;
}

/* Switch to a new temporary font (a private rasteriser with the base
 * font's extra widths); 0 and nothing changes when it cannot be opened,
 * so the tag is ignored, like the original does.  The capacity of 0x40
 * glyphs is the original's. */
static int Markup_OpenFont(Markup_t* m, const char* face, int size, int widthPct, int bold, int italic)
{
	FontRaster_t* f = FontRaster_New();
	if(FontRaster_Init(f, FontNames_Resolve(face), size, widthPct, bold, italic, 0x40) != 0)
	{
		FontRaster_Delete(f);
		return 0;
	}
	Markup_DropFont(m);
	f->extraW[0] = m->base->font->extraW[0];
	f->extraW[1] = m->base->font->extraW[1];
	m->cur.font = f;
	m->fi = &m->cur;
	return 1;
}

void Markup_Free(Markup_t* m)
{
	Markup_DropFont(m);
	BGI_Free(m->stack);
	m->stack = NULL;
}

/* Copy the `len` bytes of tag text between the brackets into `buf` (at
 * most cap - 1) and lower-case its ASCII (Shift-JIS aware), then find the
 * tag: an exact match, or a prefix match for every tag but "/".  Returns
 * the tag's index or -1. */
int Markup_Match(char* buf, size_t cap, const char* text, int len)
{
	int i;
	if(len >= (int)cap)
		len = (int)cap - 1;
	memcpy(buf, text, (size_t)len);
	buf[len] = 0;
	SjisStrLwr(buf);
	for(i = 0; i < TAG_COUNT; i++)
	{
		const char* name = gMarkupTags[i].name;
		if(gEngine->gen < gMarkupTags[i].from)
			continue;
		if(strcmp(buf, name) == 0)
			return i;
		if(i != TAG_LITERAL && strncmp(buf, name, strlen(name)) == 0)
			return i;
	}
	return -1;
}

static const char* SkipSpaces(const char* p)
{
	while(*p == ' ')
		p++;
	return p;
}

// the characters of `p` up to `stop` (a byte) or the end, Shift-JIS aware; the length
static int CopyUntil(char* out, size_t cap, const char* p, char stop)
{
	size_t n = 0;
	while(*p && *p != stop)
	{
		int len = TextCharLen(p);
		if(n + (size_t)len >= cap)
			break;
		memcpy(out + n, p, (size_t)len);
		n += (size_t)len;
		p += len;
	}
	out[n] = 0;
	return (int)n;
}

// The six lower-case hex digits of "<c rrggbb>"; 1 ok, 0 when one is missing or not hex
static int ParseHexColour(const char* p, uint32_t* out)
{
	uint32_t v = 0;
	int i;
	for(i = 0; i < 6; i++)
	{
		const char* hex = "0123456789abcdef";
		const char* at = p[i] ? strchr(hex, p[i]) : NULL;
		if(!at)
			return 0;
		v = (v << 4) | (uint32_t)(at - hex);
	}
	*out = v;
	return 1;
}

/* Apply one tag.  `args` is the text after the tag name, `after` the text
 * behind the closing bracket, `p` the position of the '<'.  A tag that
 * cannot be applied (a font that does not open, a malformed argument, a
 * closing tag without its opening one) is skipped quietly.  Always 1: the
 * tag was consumed and the layout continues behind it. */
int Markup_Apply(Markup_t* m, int tag, const char* args, const char* after, const char* p, RubyDict_t* dict,
	int32_t cur[2], const Rect_t* area)
{
	char word[0x100], reading[0x100];

	switch(tag)
	{
		case TAG_LITERAL:
			m->literal = 1;
			break;

		case TAG_BOLD:
			if(m->cur.bold)
				break;
			if(Markup_OpenFont(m, m->cur.name, m->cur.size, m->cur.widthPct, 1, m->cur.italic))
				m->cur.bold = 1;
			break;

		case TAG_BOLD_END:
			if(!m->cur.bold)
				break;
			if(m->cur.italic == m->base->italic)
			{ // back to the window's font (its name, size and width never change)
				m->cur.bold = 0;
				Markup_UseBase(m);
			}
			else if(Markup_OpenFont(m, m->cur.name, m->cur.size, m->cur.widthPct, 0, m->cur.italic))
				m->cur.bold = 0;
			break;

		case TAG_ITALIC:
			if(m->cur.italic)
				break;
			if(Markup_OpenFont(m, m->cur.name, m->cur.size, m->cur.widthPct, m->cur.bold, 1))
				m->cur.italic = 1;
			break;

		case TAG_ITALIC_END:
			if(!m->cur.italic)
				break;
			if(m->cur.bold == m->base->bold)
			{
				m->cur.italic = 0;
				Markup_UseBase(m);
			}
			else if(Markup_OpenFont(m, m->cur.name, m->cur.size, m->cur.widthPct, m->cur.bold, 0))
				m->cur.italic = 0;
			break;

		case TAG_RUBY: // "<ruby word,reading>": both into the layout's dictionary
		{
			const char* comma;
			args = SkipSpaces(args);
			CopyUntil(word, sizeof word, args, ',');
			comma = strchr(args, ',');
			if(!comma)
				break;
			CopyUntil(reading, sizeof reading, comma + 1, 0);
			RubyDict_Insert(dict, word, reading);
			break;
		}

		case TAG_R: // "<r reading>word</r>": the word runs to the next tag
		{
			const char* lt;
			args = SkipSpaces(args);
			CopyUntil(reading, sizeof reading, args, 0);
			if(!reading[0])
				break;
			lt = after;
			while(*lt && *lt != '<')
				lt += TextCharLen(lt);
			if(!*lt)
				break; // no closing tag: the word stays plain text
			CopyUntil(word, sizeof word, after, '<');
			if(!word[0])
				break;
			RubyDict_Insert(dict, word, reading);
			break;
		}

		case TAG_CR: // "<cr>": the cursor returns to the left edge, on the same line
			cur[0] = area->l;
			break;

		case TAG_COLOUR: // "<c rrggbb>": the colour until the matching "</c>"
		{
			uint32_t v;
			if(!ParseHexColour(SkipSpaces(args), &v))
				break;
			Markup_Push(m);
			m->colour = v;
			break;
		}

		case TAG_COLOUR_END:
			Markup_Pop(m);
			break;

		case TAG_LINK: // "<l>": the link colour and font until "</l>"; the start is remembered
			if(m->linkStart)
				break;
			m->linkX = cur[0];
			m->linkY = cur[1];
			m->linkStart = after;
			Markup_Push(m);
			if(gLinkColour != -1)
				m->colour = (uint32_t)gLinkColour;
			if(m->cur.bold || m->cur.italic)
				break; // the link font replaces the plain one only
			Markup_OpenFont(m, gLinkFace[0] ? gLinkFace : m->cur.name, gLinkSize > 0 ? gLinkSize : m->cur.size,
				gLinkWidthPct > 0 ? gLinkWidthPct : m->cur.widthPct, gLinkBold, gLinkItalic);
			break;

		case TAG_LINK_END: // "</l>": register the link's text (its first line) and position
		{
			int len;
			if(!m->linkStart)
				break;
			len = (int)((m->linkEnd ? m->linkEnd : p) - m->linkStart);
			m->linkEnd = NULL;
			if(len > 0)
			{ // (a link without text stays open: the original clears it here only)
				char text[0x60];
				if(len > (int)sizeof text - 1)
					len = (int)sizeof text - 1;
				memcpy(text, m->linkStart, (size_t)len);
				text[len] = 0;
				m->linkStart = NULL;
				TextLinks_Add(text, m->linkX, m->linkY);
			}
			Markup_Pop(m);
			if(m->cur.bold || m->cur.italic)
				break;
			Markup_UseBase(m);
			break;
		}

		case TAG_TIME: // "<t n>": the next glyph's reveal delay becomes n characters' worth
		{
			int n = 0;
			args = SkipSpaces(args);
			while(*args >= '0' && *args <= '9')
				n = n * 10 + (*args++ - '0');
			m->delay = n * gRubyCharDelay;
			break;
		}

		default:
			break; // "</r>" and unknown tags are skipped
	}
	return 1;
}

/* The no-break run starting at `p` (a Latin word) copied to `out`: 1.494
 * counts the bytes up to a space, 1.529 on the run of printable ASCII
 * (0x21 .. 0x7E).  Returns the byte count, 0 when `p` does not start one. */
int Markup_WordRun(char* out, const char* p)
{
	int n = 0;
	if(gEngine->gen >= GEN_1_529)
	{
		while(p[n] >= 0x21 && p[n] <= 0x7e)
			n++;
	}
	else if(*p != ' ')
	{
		while(p[n] && p[n] != ' ')
			n++;
	}
	memcpy(out, p, (size_t)n);
	out[n] = 0;
	return n;
}
