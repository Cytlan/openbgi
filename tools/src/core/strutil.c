/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * strutil.c - Shift-JIS string helpers, the string primitives of the
 *             "6x" instructions and the script text encoding
 *             (inc/bgi/strutil.h)
 *
 * The Sjis* functions work on Shift-JIS bytes only.  The Text* functions
 * are the engine's character decoder of 1.653 on, which follows gTextUtf8
 * ("81 00"): the rest of the engine goes through them so that one switch
 * moves everything between the two encodings.
 */
#include "bgi/strutil.h"
#include "bgi/os.h"

// 1 for the first byte of a double-byte Shift-JIS character (0x80 .. 0x9F, 0xE0 .. 0xFF)
int SjisIsLead(uint8_t c)
{
	if(c < 0x80)
		return 0;
	if(c < 0xa0)
		return 1;
	return c >= 0xe0;
}

// the character at s as lead << 8 | trail (or the single byte); 1 when it is double byte
int SjisGetChar(uint32_t* out, const char* s)
{
	uint32_t c = (uint8_t)s[0];
	int dbcs = SjisIsLead((uint8_t)c);
	if(dbcs)
		c = (c << 8) + (uint8_t)s[1];
	*out = c;
	return dbcs;
}

// the line-head prohibition set (kinsoku): the characters a line must not start with
static const char kKinsoku[] =
	",.\xa4\xa1:;?!\xde\xdf\xa5"                                       // , . ､ ｡ : ; ? ! ﾞ ﾟ ･ (ASCII and half-width punctuation, sound marks, middle dot)
	"\x81\x41\x81\x42\x81\x43\x81\x44\x81\x46\x81\x47\x81\x48\x81\x49" // 、。，．：；？！ (full-width comma, full stop, comma, period, colon, semicolon, ?, !)
	"\x81\x4a\x81\x4b\x81\x5d"                                         // ゛ ゜ ‐ (voiced and semi-voiced sound marks, hyphen)
	"]})"
	"\x81\x6a\x81\x6c\x81\x6e\x81\x70\x81\x72\x81\x74\x81\x76\x81\x78\x81\x7a" // ）〕］｝〉》」』】 (full-width closing brackets)
	"\x81\x66\x81\x68";                                                        // ’ ” (closing single and double quotation marks)

// 1 when `ch` (a TextGetChar code) is in the kinsoku set
int IsKinsokuChar(uint32_t ch)
{
	// the table in the current code space, rebuilt when the encoding changes
	static uint32_t codes[64];
	static int count = 0, mode = -1;
	int i;
	if(mode != gTextUtf8)
	{
		const char* p = kKinsoku;
		count = 0;
		while(*p && count < (int)BGI_COUNTOF(codes))
			codes[count++] = TextTableChar(&p);
		mode = gTextUtf8;
	}
	for(i = 0; i < count; i++)
		if(codes[i] == ch)
			return 1;
	return 0;
}

// "6D": ASCII upper case to lower case in place; multi-byte characters (either encoding) are skipped whole
void SjisStrLwr(char* s)
{
	while(*s)
	{
		int len = TextCharLen(s);
		if(len == 1 && *s >= 'A' && *s <= 'Z')
			*s = (char)(*s + 0x20);
		s += len;
	}
}

/*
 * Find `pat` in `s`, comparing characters of the current encoding (one
 * or two bytes in Shift-JIS) one at a time.  Returns the byte offset of
 * the match or -1.
 *
 * The scan is the original's naive one: after a mismatch the pattern index
 * is reset to 0 but the current character is *not* re-examined as a
 * possible start of a new match, so "aab" is not found in "aaab" at offset
 * 1 when the first 'a' already started a partial match.  Scripts rely on the
 * engine's behaviour, so it is reproduced as is.
 */
int StrFindSjis(const char* s, const char* pat)
{
	size_t plen = strlen(pat);
	uint32_t* pc = (uint32_t*)BGI_Alloc(plen * 4 + 4);
	int pcount = 0, pi = 0, off = 0, start = -1;
	const char* p;

	for(p = pat; *p;)
	{
		p += TextGetChar(&pc[pcount++], p);
	}
	p = s;
	while(*p)
	{
		uint32_t c;
		int len = TextGetChar(&c, p);
		if(c == pc[pi])
		{
			if(pi == 0)
				start = off;
			pi++;
			if(pi >= pcount)
				break; // complete match, p stays on its last char
		}
		else
		{
			pi = 0;
		}
		p += len;
		off += len;
	}
	BGI_Free(pc);
	return *p ? start : -1; // the loop ran off the end: no match
}

/* "67": copy src to dst replacing every occurrence of `from` by `to`
 * (found with StrFindSjis, so its quirk applies); returns the number of
 * replacements. */
int StrReplace(char* dst, const char* src, const char* from, const char* to)
{
	size_t nf = strlen(from), nt = strlen(to);
	int count = 0;
	int off = StrFindSjis(src, from);
	while(off >= 0)
	{
		if(off > 0)
		{
			memcpy(dst, src, (size_t)off);
			dst += off;
			src += off;
		}
		strcpy(dst, to);
		dst += nt;
		src += nf;
		count++;
		off = StrFindSjis(src, from);
	}
	strcpy(dst, src);
	return count;
}

/* the C runtime's _splitpath as the original uses it ("80 2B" of 1.69/472
 * on): drive = "x:" when the path has one, dir = everything up to the last
 * separator ('\' or '/'), fname = the base name without its last
 * extension, ext = that extension with the dot.  Shift-JIS lead bytes
 * protect their trail byte.  Any destination may be NULL; each is written
 * at most 255 characters. */
void SplitPath(const char* path, char* drive, char* dir, char* fname, char* ext)
{
	const char* p = path;
	const char* lastSep = NULL;
	const char* lastDot = NULL;
	const char* end;
	size_t n;
	if(p[0] && p[1] == ':')
	{
		if(drive)
		{
			drive[0] = p[0];
			drive[1] = ':';
			drive[2] = 0;
		}
		p += 2;
	}
	else if(drive)
		drive[0] = 0;
	for(end = p; *end; end++)
	{
		if(SjisIsLead((uint8_t)*end) && end[1])
		{
			end++;
			continue;
		}
		if(*end == '\\' || *end == '/')
			lastSep = end + 1;
		else if(*end == '.')
			lastDot = end;
	}
	if(dir)
	{
		n = lastSep ? (size_t)(lastSep - p) : 0;
		if(n > 255)
			n = 255;
		memcpy(dir, p, n);
		dir[n] = 0;
	}
	if(lastSep)
		p = lastSep;
	if(lastDot && lastDot < p) // a dot in the directory part is not an extension
		lastDot = NULL;
	if(fname)
	{
		n = (size_t)((lastDot ? lastDot : end) - p);
		if(n > 255)
			n = 255;
		memcpy(fname, p, n);
		fname[n] = 0;
	}
	if(ext)
	{
		n = lastDot ? (size_t)(end - lastDot) : 0;
		if(n > 255)
			n = 255;
		memcpy(ext, lastDot ? lastDot : end, n);
		ext[n] = 0;
	}
}


// ---- the text encoding -------------------------------------------------------------

int gTextUtf8;

// "81 00": select TEXT_SJIS or TEXT_UTF8 for the script text; the OS layer follows (window titles, message boxes, font rendering)
void Text_SetEncoding(int mode)
{
	gTextUtf8 = mode == TEXT_UTF8;
	OS_SetTextEncoding(gTextUtf8);
}

// one UTF-8 sequence, trusting its continuation bytes; the byte count
static int Utf8Decode(uint32_t* out, const char* s)
{
	uint32_t b = (uint8_t)s[0], cp;
	int n = 1;
	if(b < 0x80)
	{
		*out = b;
		return 1;
	}
	cp = 0;
	while(b & (0x80u >> n)) // one continuation byte per leading 1 bit after the first
	{
		cp = (cp << 6) | ((uint8_t)s[n] & 0x3f);
		n++;
	}
	// the payload bits of the lead byte: 7 - n of them
	cp |= (b & (0xffu >> (n + 1))) << (6 * (n - 1));
	*out = cp;
	return n;
}

/* The character at s decoded as `mode` says: TEXT_RAW takes one byte as
 * it is, TEXT_UTF8 one sequence (a surrogate pair written as two
 * sequences is joined), TEXT_SJIS one or two bytes.  *out receives the
 * code, *wide (when given) whether it occupies a full-width cell (as
 * TextIsWide); the byte length is returned. */
int TextGetCharMode(uint32_t* out, int* wide, const char* s, uint32_t mode)
{
	uint32_t c;
	int len;
	if(mode == TEXT_RAW)
	{
		*out = (uint8_t)s[0];
		if(wide)
			*wide = 0;
		return 1;
	}
	if(mode == TEXT_UTF8)
	{
		len = Utf8Decode(&c, s);
		if(c - 0xd800u < 0x400u)
		{
			// a surrogate pair written as two sequences (the original's
			// encoder never produces one, but its decoder accepts it)
			uint32_t lo;
			int n = Utf8Decode(&lo, s + len);
			if(lo - 0xdc00u < 0x400u)
			{
				c = (((c - 0xd800u) << 10) | (lo - 0xdc00u)) + 0x10000u;
				len += n;
			}
		}
		*out = c;
		if(wide)
			*wide = c >= 0x80 && !(c >= 0xff61 && c < 0xffa0);
		return len;
	}
	len = SjisGetChar(&c, s) ? 2 : 1;
	*out = c;
	if(wide)
		*wide = len == 2;
	return len;
}

// the character at s in the current encoding: its code and byte length
int TextGetChar(uint32_t* out, const char* s)
{
	return TextGetCharMode(out, NULL, s, gTextUtf8 ? TEXT_UTF8 : TEXT_SJIS);
}

// the byte length of the character at s in the current encoding
int TextCharLen(const char* s)
{
	uint32_t c;
	return TextGetChar(&c, s);
}

// does a character (code and byte length from TextGetChar) occupy a full-width cell
int TextIsWide(uint32_t code, int len)
{
	if(gTextUtf8)
		return code >= 0x80 && !(code >= 0xff61 && code < 0xffa0);
	return len == 2;
}

/* the bytes of `code` in the current encoding, without a terminator: one
 * byte or lead + trail in Shift-JIS, 1 .. 4 bytes of UTF-8; the count */
int TextPutChar(char* dst, uint32_t code)
{
	if(!gTextUtf8)
	{
		if(code > 0xff)
		{
			dst[0] = (char)(code >> 8);
			dst[1] = (char)code;
			return 2;
		}
		dst[0] = (char)code;
		return 1;
	}
	if(code < 0x80)
	{
		dst[0] = (char)code;
		return 1;
	}
	if(code < 0x800)
	{
		dst[0] = (char)(0xc0 | (code >> 6));
		dst[1] = (char)(0x80 | (code & 0x3f));
		return 2;
	}
	if(code < 0x10000)
	{
		dst[0] = (char)(0xe0 | (code >> 12));
		dst[1] = (char)(0x80 | ((code >> 6) & 0x3f));
		dst[2] = (char)(0x80 | (code & 0x3f));
		return 3;
	}
	dst[0] = (char)(0xf0 | (code >> 18));
	dst[1] = (char)(0x80 | ((code >> 12) & 0x3f));
	dst[2] = (char)(0x80 | ((code >> 6) & 0x3f));
	dst[3] = (char)(0x80 | (code & 0x3f));
	return 4;
}

/* the next character of a Shift-JIS literal (the punctuation tables of
 * the text code) as a code of the current code space: in UTF-8 mode a
 * double-byte character is converted to its code point and a half-width
 * katakana byte to U+FF61 ..; *p advances past the character */
uint32_t TextTableChar(const char** p)
{
	uint32_t c;
	int dbcs = SjisGetChar(&c, *p);
	*p += dbcs ? 2 : 1;
	if(gTextUtf8)
	{
		if(dbcs)
			c = OS_SjisToCodePoint((uint16_t)c);
		else if(c >= 0xa1 && c <= 0xdf)
			c = 0xff61 + (c - 0xa1); // half-width katakana
	}
	return c;
}

// 1 when `code` (a TextGetChar code) is one of the characters of the Shift-JIS literal `sjisSet`
int TextCodeInSet(uint32_t code, const char* sjisSet)
{
	while(*sjisSet)
		if(TextTableChar(&sjisSet) == code)
			return 1;
	return 0;
}

/* Which decoder a string wants, judged from its bytes: TEXT_UTF8 when
 * every byte above 0x7F starts a well-formed UTF-8 sequence (decided as
 * soon as six such sequences have been seen), TEXT_SJIS at the first byte
 * that does not, TEXT_RAW when there is no byte above 0x7F at all.
 * Control bytes count as plain ASCII. */
uint32_t TextSniff(const char* s)
{
	int multi = 0;
	const uint8_t* p = (const uint8_t*)s;
	while(*p)
	{
		uint8_t b = *p;
		if(b < 0x80)
		{
			p++;
			continue;
		}
		if(b < 0xc2)
			return TEXT_SJIS; // a stray continuation byte or an overlong lead
		{
			int n = 1, i;
			while(b & (0x80u >> n))
				n++;
			if(n < 2)
				return TEXT_SJIS;
			for(i = 1; i < n; i++)
				if((p[i] & 0xc0) != 0x80)
					return TEXT_SJIS; // (a NUL inside the sequence ends here too)
			p += n;
			if(++multi >= 6)
				return TEXT_UTF8;
		}
	}
	return multi ? TEXT_UTF8 : TEXT_RAW;
}
