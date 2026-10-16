/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_common.c - OS layer code shared by all the back ends (inc/bgi/os_common.h)
 *
 * Nothing here touches the host; it is the platform-independent part of
 * the services declared in os.h: DOS-style wildcard matching for the
 * directory enumeration, the Shift-JIS lead-byte test the back ends need
 * when they walk text, the text encoding switch ("81 00") with the UTF-8
 * decoding the font back ends share, the Shift-JIS to UTF-8 conversion
 * around a back-end supplied code page table, and the text file format
 * of the registry emulation.
 */
#include "bgi/os.h"
#include "bgi/os_common.h"

#include <stdio.h>

// ---- Shift-JIS --------------------------------------------------------------------

// 1 for the first byte of a two-byte Shift-JIS character (0x81 .. 0x9f, 0xe0 .. 0xfc)
int OsCommon_SjisIsLead(uint8_t c)
{
	return (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc);
}

// ---- the text encoding ("81 00") ------------------------------------------------------

int gOsTextUtf8; // OS_SetTextEncoding: 1 when the script text is UTF-8

// "81 00" of 1.653 on: the script text is UTF-8 (1) or Shift-JIS (0); see os.h
void OS_SetTextEncoding(int utf8)
{
	gOsTextUtf8 = utf8 != 0;
}

/* a well-formed UTF-8 sequence at `s` (a lead byte with its continuation
 * bytes): its length, else 0.  Used to pass script text through unchanged
 * in UTF-8 mode while the engine's own Shift-JIS messages still convert. */
int OsCommon_Utf8SeqLen(const uint8_t* s)
{
	int n, i;
	if(s[0] < 0xc2 || s[0] > 0xf4)
		return 0;
	n = s[0] >= 0xf0 ? 4 : s[0] >= 0xe0 ? 3
										: 2;
	for(i = 1; i < n; i++)
		if((s[i] & 0xc0) != 0x80)
			return 0;
	return n;
}

/* one UTF-8 sequence to its code point (its lead byte decides the length,
 * which is not validated: call OsCommon_Utf8SeqLen first); *outLen = the
 * bytes taken */
uint32_t OsCommon_Utf8Decode(const uint8_t* u, int* outLen)
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

/* the code point of the character at `s` (`len` bytes, as the engine's
 * TextGetChar counted it) in the text encoding: the font back ends look
 * the glyph up by it.  In Shift-JIS mode the two-byte codes go through
 * `lookup` (0xfffd without one), 0xa1 .. 0xdf are the half-width katakana
 * (U+FF61 ..) and 0x5c stays the backslash, not the yen sign. */
uint32_t OsCommon_CodePoint(const char* s, int len, OsCommon_SjisLookup_t lookup)
{
	const uint8_t* u = (const uint8_t*)s;
	if(gOsTextUtf8)
	{
		int n;
		uint32_t cp = OsCommon_Utf8Decode(u, &n);
		if(cp - 0xd800u < 0x400u && len > n)
		{
			// a surrogate pair encoded as two three-byte sequences (CESU-8): combine them
			uint32_t lo = ((u[n] & 0x0f) << 12) | ((u[n + 1] & 0x3f) << 6) | (u[n + 2] & 0x3f);
			if(lo - 0xdc00u < 0x400u)
				cp = (((cp - 0xd800u) << 10) | (lo - 0xdc00u)) + 0x10000u;
		}
		return cp;
	}
	if(len >= 2 && OsCommon_SjisIsLead(u[0]))
		return lookup ? lookup((uint16_t)((u[0] << 8) | u[1])) : 0xfffd;
	if(u[0] >= 0xa1 && u[0] <= 0xdf)
		return 0xff61 + (u[0] - 0xa1); // half-width katakana
	return u[0];                       // (0x5c stays the backslash glyph, not the yen sign)
}

// ---- wildcards --------------------------------------------------------------------

// ASCII lower case: the comparisons below fold only the ASCII letters
static int OsCommon_Lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* FindFirstFile semantics, as far as the engine relies on them: '*' matches
 * any run (including none), '?' one character, the comparison ignores
 * ASCII case and double-byte characters match as units.  "*.*" matches
 * every name, with or without a dot. */
int OsCommon_WildcardMatch(const char* pattern, const char* name)
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
				if(OsCommon_WildcardMatch(pattern, name))
					return 1;
				if(!*name)
					return 0;
				name += OsCommon_SjisIsLead((uint8_t)*name) && name[1] ? 2 : 1;
			}
		}
		if(!*name)
			return 0;
		if(OsCommon_SjisIsLead((uint8_t)*pattern) && pattern[1])
		{
			if(pattern[0] != name[0] || pattern[1] != name[1])
				return 0;
			pattern += 2;
			name += 2;
			continue;
		}
		if(*pattern != '?' && OsCommon_Lower((uint8_t)*pattern) != OsCommon_Lower((uint8_t)*name))
			return 0;
		pattern++;
		name += OsCommon_SjisIsLead((uint8_t)*name) && name[1] ? 2 : 1;
	}
	return *name == 0;
}

/* the file part of a Windows path: what follows the last '\\' or '/'
 * (the whole path when there is none); a separator byte that is the
 * trail byte of a Shift-JIS character does not count */
const char* OsCommon_FilePart(const char* path)
{
	const char* p = path;
	const char* last = path;
	while(*p)
	{
		if((*p == '\\' || *p == '/') && !(p > path && OsCommon_SjisIsLead((uint8_t)p[-1])))
			last = p + 1;
		p += OsCommon_SjisIsLead((uint8_t)*p) && p[1] ? 2 : 1;
	}
	return last;
}

// the main window's title as shown: "<game title> - OpenBGI", or "OpenBGI" alone for an empty or NULL one
void OsCommon_MainTitle(char* out, size_t n, const char* sjisTitle)
{
	if(sjisTitle && *sjisTitle)
		snprintf(out, n, "%s - OpenBGI", sjisTitle);
	else
		snprintf(out, n, "OpenBGI");
}

// ---- registry emulation (used by the POSIX back end; the format is shared) ---------
/* The handful of registry values the engine writes (the install folder,
 * the file association) are kept in a plain text file of
 * "key\value=data" lines, one value per line; keys are compared without
 * regard to case and with '/' and '\\' as the same separator.  Every write
 * rewrites the whole file. */

// 1 when two "key\value" names are the same, ignoring ASCII case and the separator's spelling
static int OsCommon_KeyEq(const char* a, const char* b)
{
	while(*a && *b)
	{
		int ca = OsCommon_Lower((uint8_t)*a), cb = OsCommon_Lower((uint8_t)*b);
		if(ca == '/')
			ca = '\\';
		if(cb == '/')
			cb = '\\';
		if(ca != cb)
			return 0;
		a++;
		b++;
	}
	return *a == *b;
}

/* read one value (NULL = the key's default value) into buf (n bytes); 1
 * when the line exists, 0 when it does not or the file cannot be opened */
int OsCommon_RegFileRead(const char* file, const char* key, const char* value, char* buf, size_t n)
{
	char line[0x400], want[0x300];
	FILE* f = fopen(file, "rb");
	int found = 0;
	if(!f)
		return 0;
	snprintf(want, sizeof want, "%s\\%s", key, value ? value : "");
	while(fgets(line, sizeof line, f))
	{
		char* eq = strchr(line, '=');
		char* nl;
		if(!eq)
			continue;
		*eq = 0;
		if((nl = strchr(eq + 1, '\n')) != NULL)
			*nl = 0;
		if((nl = strchr(eq + 1, '\r')) != NULL)
			*nl = 0;
		if(OsCommon_KeyEq(line, want))
		{
			snprintf(buf, n, "%s", eq + 1);
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

/* write or replace one value (data NULL removes it); `key` alone with
 * value NULL and data "" registers the key's existence (OS_RegCreateKey).
 * 1 when the file was rewritten, 0 when it could not be opened for
 * writing */
int OsCommon_RegFileWrite(const char* file, const char* key, const char* value, const char* data)
{
	char want[0x300];
	char* keep = NULL;
	size_t keepLen = 0;
	FILE* f;
	snprintf(want, sizeof want, "%s\\%s", key, value ? value : "");
	f = fopen(file, "rb");
	if(f)
	{
		char line[0x400];
		while(fgets(line, sizeof line, f))
		{
			char probe[0x400];
			char* eq;
			strcpy(probe, line);
			eq = strchr(probe, '=');
			if(eq)
			{
				*eq = 0;
				if(OsCommon_KeyEq(probe, want))
					continue; // dropped: rewritten below
			}
			keep = (char*)realloc(keep, keepLen + strlen(line) + 1);
			strcpy(keep + keepLen, line);
			keepLen += strlen(line);
		}
		fclose(f);
	}
	f = fopen(file, "wb");
	if(!f)
	{
		free(keep);
		return 0;
	}
	if(keep)
		fwrite(keep, 1, keepLen, f);
	if(data)
		fprintf(f, "%s=%s\n", want, data);
	fclose(f);
	free(keep);
	return 1;
}

/* remove every value under `key` (and the key itself: every "key\..."
 * line); 1 when at least one line went, 0 when there was none or the
 * file cannot be read */
int OsCommon_RegFileDeleteKey(const char* file, const char* key)
{
	char* keep = NULL;
	size_t keepLen = 0, klen = strlen(key);
	FILE* f = fopen(file, "rb");
	int any = 0;
	if(!f)
		return 0;
	{
		char line[0x400];
		while(fgets(line, sizeof line, f))
		{
			char probe[0x400];
			strcpy(probe, line);
			if(strlen(probe) > klen && probe[klen] == '\\')
			{
				probe[klen] = 0;
				if(OsCommon_KeyEq(probe, key))
				{
					any = 1;
					continue;
				}
			}
			keep = (char*)realloc(keep, keepLen + strlen(line) + 1);
			strcpy(keep + keepLen, line);
			keepLen += strlen(line);
		}
	}
	fclose(f);
	f = fopen(file, "wb");
	if(f)
	{
		if(keep)
			fwrite(keep, 1, keepLen, f);
		fclose(f);
	}
	free(keep);
	return any;
}

// ---- Shift-JIS -> UTF-8 ---------------------------------------------------------
/* The back ends need the engine's Shift-JIS text as UTF-8 (POSIX file
 * names, window titles) or UTF-16 (Win32 W APIs, the font renderer).  The
 * conversion of the two-byte range is done through a table built once from
 * the CP932 code page by the back end (iconv / MultiByteToWideChar); this
 * helper only handles the ASCII and half-width katakana parts and defers
 * the rest to `lookup` (0xfffd without one or for an unmapped code; a lone
 * lead byte at the end likewise).  `out` (n bytes) is NUL-terminated; the
 * length written is returned. */
size_t OsCommon_SjisToUtf8(const char* sjis, char* out, size_t n, OsCommon_SjisLookup_t lookup)
{
	size_t o = 0;
	const uint8_t* s = (const uint8_t*)sjis;
	while(*s && o + 4 < n)
	{
		uint32_t cp;
		int u8 = gOsTextUtf8 ? OsCommon_Utf8SeqLen(s) : 0;
		if(u8)
		{
			// script text in UTF-8 mode: already what the caller wants
			memcpy(out + o, s, (size_t)u8);
			o += (size_t)u8;
			s += u8;
			continue;
		}
		if(*s < 0x80)
		{
			cp = *s++;
			if(cp == '\\')
				cp = 0x5c; // the backslash stays U+005C (Shift-JIS 0x5c is the yen sign in JIS X 0201; the paths need the separator)
		}
		else if(*s >= 0xa1 && *s <= 0xdf)
		{
			cp = 0xff61 + (*s - 0xa1); // half-width katakana
			s++;
		}
		else if(OsCommon_SjisIsLead(*s) && s[1])
		{
			cp = lookup ? lookup((uint16_t)((s[0] << 8) | s[1])) : 0xfffd;
			s += 2;
		}
		else
		{
			cp = 0xfffd;
			s++;
		}
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
