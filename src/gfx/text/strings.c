/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * strings.c - what the text engine knows about strings: Shift-JIS
 *             character runs, string measurement, the "<l>" link sections
 *             and tag stripping of the inline markup, and the character
 *             classes of Japanese typesetting (opening / closing brackets,
 *             the glyphs that rotate or shift in vertical writing)
 *             (inc/bgi/gfx/text.h)
 *
 * The class tables are the MSG_TXT_* strings of tools/messages.txt; a
 * character is tested by membership in the table (CharInSet).
 */
#include "text_internal.h"

// ---- Shift-JIS helpers -------------------------------------------------------------------------

// count the characters of `s`; with `out` also store their codes
int SjisCodes(int32_t* outOrNull, const char* s)
{
	int n = 0;
	while(*s)
	{
		uint32_t code;
		s += TextGetChar(&code, s);
		if(outOrNull)
			outOrNull[n] = (int32_t)code;
		n++;
	}
	return n;
}

// one character of `s` as a NUL-terminated string in `out` (TEXT_CHAR_MAX bytes)
static void CopyChar(char* out, const char* s)
{
	int len = TextCharLen(s);
	memcpy(out, s, (size_t)len);
	out[len] = 0;
}

// 1 when the first character of `p` is in the NUL-terminated Shift-JIS literal `set`
static int CharInSet(const char* p, const char* set)
{
	uint32_t c;
	TextGetChar(&c, p);
	return TextCodeInSet(c, set);
}

// ---- measuring ---------------------------------------------------------------------------------

/* measure `str` in the cache font `font`: out[0] is the advance of the
 * whole string, out[1] the ink extent (without half of the first and last
 * proportional gaps), out[2] the advance up to the middle of the last gap.
 * 1 ok, 0 when the font is unknown (nothing written). */
int Text_Measure(int32_t out[3], const char* str, int font, int proportional)
{
	FontInfo_t fi;
	if(!BmpMgr_FontInfo(gTextBmpMgr, &fi, font))
		return 0;
	return Text_MeasureInfo(out, str, &fi, proportional);
}

/* the measure with the font given (the markup's temporary fonts have no
 * handle).  Proportional text advances by the ink width plus the glyph's
 * gap (Font_GapFor2); fixed-pitch text by the full (half for a single-byte
 * character) glyph width plus gCharPitch, plus the font's "91 0F" extra
 * width from 1.529 on.  Always 1. */
int Text_MeasureInfo(int32_t out[3], const char* str, const FontInfo_t* fi, int proportional)
{
	Bmp_t cell;
	int glyphW, total = 0, firstHalf = 0, first = 1, gap = 0, pitch;

	pitch = proportional ? 0 : gCharPitch;
	if(!proportional && gEngine->gen >= GEN_1_529 && fi->font)
		pitch += fi->font->extraW[fi->italic ? 1 : 0];
	Bmp_AllocScreen(&cell, 2 * fi->size, fi->size, 1);
	glyphW = FontInfo_GlyphW(fi);
	while(*str)
	{
		Bmp_t view = cell;
		Glyph_t g;
		uint32_t code;
		int len = TextGetChar(&code, str);
		int dbcs = TextIsWide(code, len);

		Text_PaintGlyph(&view, &g, (int)code, fi->font, 0xffffff);
		if(proportional)
		{
			Bmp_CropGlyph(&view, &g);
			gap = Font_GapFor2(&g, glyphW);
			total += view.w + gap;
			if(first)
			{
				first = 0;
				firstHalf = gap >> 1;
			}
		}
		else
		{
			int w = dbcs ? glyphW : glyphW / 2;
			gap = 0;
			total += w + pitch;
		}
		str += len;
	}
	BGI_Free(cell.pixels);
	total -= pitch; // no pitch after the last character
	out[0] = total;
	out[1] = total + (gap >> 1) - firstHalf - gap;
	out[2] = total + (gap >> 1) - gap;
	return 1;
}

/* "92 99" (1.653 on): the ABC widths of every character of `str` in
 * font `font` - A the blank columns before the ink, B the ink width, C
 * the blank columns after it up to the fixed advance (the full glyph
 * width, half of it for a single-byte character) - as three words per
 * character into `out` (NULL counts only); *outCount receives the number
 * of characters.  The original asks GDI for the ABC of each glyph; here
 * the ink extent of the rasterised glyph stands in.  1 ok, 0 unknown
 * font. */
int Text_AbcWidths(int32_t* out, int32_t* outCount, const char* str, int font)
{
	FontInfo_t fi;
	Bmp_t cell;
	int glyphW, n = 0;
	if(!BmpMgr_FontInfo(gTextBmpMgr, &fi, font))
		return 0;
	Bmp_AllocScreen(&cell, 2 * fi.size, fi.size, 1);
	glyphW = FontInfo_GlyphW(&fi);
	while(*str)
	{
		Bmp_t view = cell;
		Glyph_t g;
		uint32_t code;
		int len = TextGetChar(&code, str);
		int adv = TextIsWide(code, len) ? glyphW : glyphW / 2;
		if(out)
		{
			int a, b;
			Text_PaintGlyph(&view, &g, (int)code, fi.font, 0xffffff);
			a = g.left;
			b = g.right >= g.left ? g.right - g.left + 1 : 0;
			if(!b)
				a = 0;
			out[n * 3] = a;
			out[n * 3 + 1] = b;
			out[n * 3 + 2] = adv - a - b;
		}
		n++;
		str += len;
	}
	BGI_Free(cell.pixels);
	if(outCount)
		*outCount = n;
	return 1;
}

// ---- the link sections and tags of the markup -------------------------------------------------

/* "91 9E" (1.529 on): the "<l>..</l>" (or "<L>..</L>") sections of `str`;
 * each non-empty one goes to `out` (its text, at most 0x5F bytes, position
 * 0) when `out` is given.  Returns their number. */
int Text_CountLinks(TextLink_t* out, const char* str)
{
	int n = 0;
	while(str)
	{
		const char* open = strstr(str, "<l>");
		const char* close;
		int len;
		if(!open)
			open = strstr(str, "<L>");
		if(!open)
			break;
		open += 3;
		close = strstr(open, "</l>");
		if(!close)
			close = strstr(open, "</L>");
		if(!close)
			break;
		len = (int)(close - open);
		if(len > 0)
		{
			if(out)
			{
				memset(&out[n], 0, sizeof out[n]);
				if(len > 0x5f)
					len = 0x5f;
				memcpy(out[n].text, open, (size_t)len);
			}
			n++;
		}
		str = close + 3;
	}
	return n;
}

/* "91 9F" (1.529 on): `str` without its markup into `out` - a '<' followed
 * by a letter or '/' starts a tag that runs to the next '>'; without one
 * the rest is copied as it is.  Returns the length written. */
int Text_StripTags(char* out, const char* str)
{
	char* d = out;
	while(*str)
	{
		if(*str == '<' && ((str[1] >= 'A' && str[1] <= 'Z') || (str[1] >= 'a' && str[1] <= 'z') || str[1] == '/'))
		{
			const char* gt = strchr(str + 2, '>');
			if(gt)
			{
				str = gt + 1;
				continue;
			}
			strcpy(d, str);
			return (int)(d - out) + (int)strlen(str);
		}
		*d++ = *str++;
	}
	*d = 0;
	return (int)(d - out);
}

// ---- the character classes ---------------------------------------------------------------------

/* copy the run of closing punctuation (MSG_TXT_CLOSING_RUN: the characters
 * that may not start a line) that starts at `p` to `out` (NUL-terminated);
 * returns the number of characters */
int Text_CollectClosing(char* out, const char* p)
{
	int n = 0;

	while(*p && CharInSet(p, MSG_TXT_CLOSING_RUN))
	{
		int len = TextCharLen(p);
		memcpy(out, p, (size_t)len);
		out += len;
		p += len;
		n++;
	}
	*out = 0;
	return n;
}

// is the first character of `p` an opening bracket that may hang into the left margin
int Text_IsHangBracket(const char* p)
{
	return CharInSet(p, MSG_TXT_HANG_BRACKETS);
}

// is the first character of `p` closing punctuation that may overhang the right margin
int Text_IsClosing(const char* p)
{
	return CharInSet(p, MSG_TXT_CLOSING);
}

// is the first character of `p` an opening bracket that may not end a line
int Text_IsOpening(const char* p)
{
	return CharInSet(p, MSG_TXT_OPENING);
}

// the n-th (0-based) character of `s` as a string; 1 when it exists
int Text_NthChar(char* out, const char* s, int n)
{
	int i;

	if(n < 0)
		return 0;
	for(i = 0; *s && i <= n; i++)
	{
		if(i < n)
			s += TextCharLen(s);
		else
		{
			CopyChar(out, s);
			return 1;
		}
	}
	return 0;
}

/* how the first character of `s` is placed in vertical text: out[0] =
 * rotate by 90 degrees, out[1] / out[2] = shift right / up in percent of
 * the size.  The first four entries of the shift table (，．、。) move
 * further (67%) than the small kana (20%).  1 when the character is in
 * either table. */
int Text_VertClass(int32_t out[3], const char* s)
{
	const char* set;
	uint32_t c;
	int idx = 0;

	out[0] = out[1] = out[2] = 0;
	if(CharInSet(s, MSG_TXT_VERT_ROTATE))
	{
		out[0] = 1;
		return 1;
	}
	TextGetChar(&c, s);
	for(set = MSG_TXT_VERT_SHIFT; *set; idx++)
	{
		if(TextTableChar(&set) == c)
		{
			out[1] = idx >= 4 ? 0x14 : 0x43;
			out[2] = idx >= 4 ? -0x14 : -0x43;
			return 1;
		}
	}
	return 0;
}
