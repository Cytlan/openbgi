/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sjis.c - Shift-JIS (code page 932) and Unicode (src/include/bgi/sjis.h)
 *
 * The tables are cp932.inc's: a direct table for the two-byte codes and a
 * sorted list of scalars for the reverse direction.
 */
#include "bgi/sjis.h"

#include "cp932.inc"

uint32_t Sjis_CodePoint(uint16_t code)
{
	uint32_t lead = code >> 8, trail = code & 0xff;
	uint32_t row, cp;
	if(trail < 0x40)
		return 0xfffd;
	if(lead >= 0x81 && lead <= 0x9f)
		row = lead - 0x81;
	else if(lead >= 0xe0 && lead <= 0xfc)
		row = lead - 0xe0 + 0x1f;
	else
		return 0xfffd;
	cp = kCp932Decode[row][trail - 0x40];
	return cp ? cp : 0xfffd;
}

int Sjis_FromCodePoint(uint32_t cp, char out[2])
{
	size_t lo = 0, hi = CP932_ENC_COUNT;
	if(cp < 0x80)
	{
		out[0] = (char)cp;
		return 1;
	}
	if(cp >= 0xff61 && cp <= 0xff9f) // the half-width katakana are single bytes 0xA1 .. 0xDF
	{
		out[0] = (char)(cp - 0xff61 + 0xa1);
		return 1;
	}
	if(cp > 0xffff)
		return 0;
	while(lo < hi)
	{
		size_t mid = (lo + hi) / 2;
		if(kCp932EncUnicode[mid] < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	if(lo == CP932_ENC_COUNT || kCp932EncUnicode[lo] != cp)
		return 0;
	if(kCp932EncCode[lo] < 0x100) // the yen sign and the overline share the ASCII codes
	{
		out[0] = (char)kCp932EncCode[lo];
		return 1;
	}
	out[0] = (char)(kCp932EncCode[lo] >> 8);
	out[1] = (char)kCp932EncCode[lo];
	return 2;
}

int Sjis_IsLead(uint8_t c)
{
	return (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc);
}

int Utf8_SeqLen(const uint8_t* s)
{
	int n, i;
	if(s[0] < 0xc2 || s[0] > 0xf4)
		return 0;
	n = s[0] >= 0xf0 ? 4 : (s[0] >= 0xe0 ? 3 : 2);
	for(i = 1; i < n; i++)
		if((s[i] & 0xc0) != 0x80)
			return 0;
	return n;
}

uint32_t Utf8_Decode(const uint8_t* u, int* outLen)
{
	uint32_t cp;
	int n = 1;
	if(u[0] < 0x80)
	{
		*outLen = 1;
		return u[0];
	}
	cp = 0;
	while(u[0] & (0x80u >> n))
	{
		cp = (cp << 6) | (u[n] & 0x3f);
		n++;
	}
	cp |= (u[0] & (0xffu >> (n + 1))) << (6 * (n - 1));
	*outLen = n;
	return cp;
}

/* The two-byte range goes through the table; ASCII and the half-width
 * katakana are done here.  Whole characters only, with the terminator's
 * place kept. */
size_t Sjis_ToUtf8(const char* sjis, char* out, size_t n, int utf8)
{
	size_t o = 0;
	const uint8_t* s = (const uint8_t*)sjis;
	if(n == 0)
		return 0;
	while(*s)
	{
		const uint8_t* next;
		uint32_t cp;
		size_t len;
		int u8 = utf8 ? Utf8_SeqLen(s) : 0;
		if(u8)
		{ // script text in UTF-8 mode: already what the caller wants
			if(o + (size_t)u8 >= n)
				break;
			memcpy(out + o, s, (size_t)u8);
			o += (size_t)u8;
			s += u8;
			continue;
		}
		if(*s < 0x80)
		{
			cp = *s; // the backslash stays U+005C (the paths need the separator)
			next = s + 1;
		}
		else if(*s >= 0xa1 && *s <= 0xdf)
		{
			cp = 0xff61 + (*s - 0xa1); // half-width katakana
			next = s + 1;
		}
		else if(Sjis_IsLead(*s) && s[1])
		{
			cp = Sjis_CodePoint((uint16_t)((s[0] << 8) | s[1]));
			next = s + 2;
		}
		else
		{
			cp = 0xfffd;
			next = s + 1;
		}
		len = cp < 0x80 ? 1 : (cp < 0x800 ? 2 : 3);
		if(o + len >= n)
			break;
		s = next;
		if(cp < 0x80)
			out[o++] = (char)cp;
		else if(cp < 0x800)
		{
			out[o++] = (char)(0xc0 | (cp >> 6));
			out[o++] = (char)(0x80 | (cp & 0x3f));
		}
		else
		{
			out[o++] = (char)(0xe0 | (cp >> 12));
			out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
			out[o++] = (char)(0x80 | (cp & 0x3f));
		}
	}
	out[o] = 0;
	return o;
}

size_t Sjis_FromUtf8(const char* utf8, char* out, size_t n)
{
	const uint8_t* u = (const uint8_t*)utf8;
	size_t off = 0;
	if(n == 0)
		return 0;
	while(*u && off + 2 < n) // room for a two-byte code and the terminator
	{
		int len, m;
		uint32_t cp = Utf8_SeqLen(u) ? Utf8_Decode(u, &len) : (len = 1, (uint32_t)*u);
		m = Sjis_FromCodePoint(cp, out + off);
		if(m == 0)
		{
			out[off] = '?';
			m = 1;
		}
		off += (size_t)m;
		u += len;
	}
	out[off] = 0;
	return off;
}

// ASCII lower case: the wildcard comparison folds only the ASCII letters
static int LowerAscii(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int Sjis_WildcardMatch(const char* pattern, const char* name)
{
	if(strcmp(pattern, "*.*") == 0 || strcmp(pattern, "*") == 0)
		return 1;
	while(*pattern)
	{
		if(*pattern == '*')
		{
			pattern++;
			if(!*pattern)
				return 1;
			for(;;)
			{
				if(Sjis_WildcardMatch(pattern, name))
					return 1;
				if(!*name)
					return 0;
				name += Sjis_IsLead((uint8_t)*name) && name[1] ? 2 : 1;
			}
		}
		if(!*name)
			return 0;
		if(Sjis_IsLead((uint8_t)*pattern) && pattern[1])
		{
			if(pattern[0] != name[0] || pattern[1] != name[1])
				return 0;
			pattern += 2;
			name += 2;
			continue;
		}
		if(*pattern != '?' && LowerAscii((uint8_t)*pattern) != LowerAscii((uint8_t)*name))
			return 0;
		pattern++;
		name += Sjis_IsLead((uint8_t)*name) && name[1] ? 2 : 1;
	}
	return *name == 0;
}
