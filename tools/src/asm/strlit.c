/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * strlit.c - quoted string literals, shared by the assembler, the
 *            disassembler and the scenario tools (declared in asm.h)
 *
 * A literal holds Shift-JIS bytes written as UTF-8 text with C-like
 * escapes.  Asm_WriteStringLiteral chooses between UTF-8 and \xNN bytes so
 * that the reader gets the original bytes back exactly, whatever CP932's
 * duplicate codes do to the conversion; Asm_ReadStringLiteral is the
 * reader.
 *
 * For a game whose text is UTF-8 (gAsmUtf8: the `-u` option or an
 * `#encoding utf-8` line) nothing is converted: the bytes of the source
 * are the bytes of the string.
 */
#include "bgi/asm.h"
#include "bgi/os.h"

#include <ctype.h>

int gAsmUtf8;               // string literals are kept in UTF-8
static int gAsmUtf8Default; // what a file starts with (the -u option)

void Asm_SetDefaultStringEncoding(int utf8)
{
	gAsmUtf8Default = utf8 != 0;
	gAsmUtf8 = gAsmUtf8Default;
}

void Asm_ResetStringEncoding(void)
{
	gAsmUtf8 = gAsmUtf8Default;
}

// See asm.h.
const char* Asm_EncodingDirective(const char* p, const char* end, int* ok)
{
	static const struct
	{
		const char* name;
		int utf8;
	} kNames[] = {{"utf-8", 1}, {"utf8", 1}, {"sjis", 0}, {"shift-jis", 0}, {"shift_jis", 0}, {"cp932", 0}};
	const char* s;
	size_t i, n;
	while(p < end && (*p == ' ' || *p == '\t'))
		p++;
	s = p;
	while(p < end && !isspace((unsigned char)*p))
		p++;
	n = (size_t)(p - s);
	*ok = 0;
	for(i = 0; i < sizeof kNames / sizeof kNames[0]; i++)
	{
		size_t k;
		if(strlen(kNames[i].name) != n)
			continue;
		for(k = 0; k < n && tolower((unsigned char)s[k]) == kNames[i].name[k]; k++)
			;
		if(k == n)
		{
			gAsmUtf8 = kNames[i].utf8;
			*ok = 1;
		}
	}
	return p;
}

// 1 when the n bytes are well-formed UTF-8 (no overlong forms, no surrogates)
static int IsUtf8(const uint8_t* s, uint32_t n)
{
	uint32_t i = 0;
	while(i < n)
	{
		uint8_t c = s[i];
		uint32_t len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2
			: (c & 0xf0) == 0xe0                         ? 3
			: (c & 0xf8) == 0xf0                         ? 4
														 : 0,
				 k, cp;
		if(len == 0 || i + len > n)
			return 0;
		cp = len == 1 ? c : c & (0xff >> (len + 1));
		for(k = 1; k < len; k++)
		{
			if((s[i + k] & 0xc0) != 0x80)
				return 0;
			cp = (cp << 6) | (s[i + k] & 0x3f);
		}
		if((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10ffff ||
			(cp >= 0xd800 && cp <= 0xdfff))
			return 0;
		i += len;
	}
	return 1;
}

/* Write `n` bytes of Shift-JIS as a quoted literal the reader turns back
 * into the same bytes: in UTF-8 when the conversion there and back is
 * exact (CP932 has duplicate codes for a few characters, and an
 * unmappable one is substituted), else every non-ASCII byte as \xNN;
 * quotes, backslashes and control characters are escaped either way. */
void Asm_WriteStringLiteral(FILE* out, const uint8_t* s, uint32_t n)
{
	char* sjis = (char*)malloc(n + 1);
	char* utf = (char*)malloc(n * 3 + 1);
	char* back = (char*)malloc(n * 2 + 2);
	uint32_t i;
	int exact;
	memcpy(sjis, s, n);
	sjis[n] = 0;
	if(gAsmUtf8)
	{ // the bytes are the text: written as they are when they are valid UTF-8 (and hold no NUL, which would end `utf`)
		memcpy(utf, s, n);
		utf[n] = 0;
		exact = IsUtf8(s, n) && !memchr(s, 0, n);
	}
	else
	{
		OS_SjisToUtf8(sjis, utf, n * 3 + 1);
		OS_Utf8ToSjis(utf, back, n * 2 + 2);
		// a '?' in the UTF-8 that the source does not have is a substituted character
		exact = strlen(back) == n && memcmp(back, sjis, n) == 0 && !strchr(utf, '?') == !memchr(s, '?', n);
	}
	fputc('"', out);
	if(exact)
	{
		const uint8_t* u = (const uint8_t*)utf;
		for(; *u; u++)
		{
			if(*u == '"' || *u == '\\')
				fprintf(out, "\\%c", *u);
			else if(*u == '\n')
				fputs("\\n", out);
			else if(*u == '\r')
				fputs("\\r", out);
			else if(*u == '\t')
				fputs("\\t", out);
			else if(*u < 0x20)
				fprintf(out, "\\x%02x", *u);
			else
				fputc(*u, out);
		}
	}
	else
	{
		for(i = 0; i < n; i++)
		{
			uint8_t c = s[i];
			if(c == '"' || c == '\\')
				fprintf(out, "\\%c", c);
			else if(c >= 0x20 && c < 0x7f)
				fputc(c, out);
			else
				fprintf(out, "\\x%02x", c);
		}
	}
	fputc('"', out);
	free(sjis);
	free(utf);
	free(back);
}

/* Read the literal that starts at `p` (the opening quote) and ends before
 * `end`: UTF-8 text becomes Shift-JIS (or stays UTF-8, with gAsmUtf8), \xNN inserts a byte, \0 a NUL,
 * \n \r \t \\ \" as usual, any other escaped character itself.  The bytes
 * go to a malloc'ed buffer in `*outBytes` (no NUL added), `*outLen` is
 * their count and `*next` points after the closing quote.  Returns NULL on
 * success, else a message ("unterminated string", ..) with nothing
 * allocated. */
const char* Asm_ReadStringLiteral(const char* p, const char* end, uint8_t** outBytes, uint32_t* outLen, const char** next)
{
	size_t cap = (size_t)(end - p) * 2 + 16;
	uint8_t* buf = (uint8_t*)malloc(cap);
	char* run = (char*)malloc(cap);
	uint32_t n = 0, r = 0;
	if(p >= end || *p != '"')
	{
		free(buf);
		free(run);
		return "a string literal was expected";
	}
	p++;
	for(;;)
	{
		char c;
		if(p >= end)
		{
			free(buf);
			free(run);
			return "unterminated string";
		}
		c = *p++;
		if(c == '"' || c == '\\')
		{
			// a run of text ends here: convert it to Shift-JIS as a whole
			if(r && gAsmUtf8)
			{ // a UTF-8 game: the text is taken as it is
				memcpy(buf + n, run, r);
				n += r;
				r = 0;
			}
			else if(r)
			{
				char* sj = (char*)malloc(cap);
				size_t m;
				run[r] = 0;
				m = OS_Utf8ToSjis(run, sj, cap);
				memcpy(buf + n, sj, m);
				n += (uint32_t)m;
				r = 0;
				free(sj);
			}
			if(c == '"')
				break;
			if(p >= end)
			{
				free(buf);
				free(run);
				return "bad escape";
			}
			c = *p++;
			switch(c)
			{
				case 'n': buf[n++] = '\n'; break;
				case 'r': buf[n++] = '\r'; break;
				case 't': buf[n++] = '\t'; break;
				case '0': buf[n++] = 0; break;
				case 'x':
				{
					char hex[3];
					if(p + 2 > end || !isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]))
					{
						free(buf);
						free(run);
						return "\\x needs two hex digits";
					}
					hex[0] = p[0];
					hex[1] = p[1];
					hex[2] = 0;
					buf[n++] = (uint8_t)strtol(hex, NULL, 16);
					p += 2;
					break;
				}
				default: buf[n++] = (uint8_t)c; break;
			}
			continue;
		}
		run[r++] = c;
	}
	free(run);
	*outBytes = buf;
	*outLen = n;
	*next = p;
	return NULL;
}
