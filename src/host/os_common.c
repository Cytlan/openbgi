/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_common.c - OS layer code shared by all the back ends (src/host/os_common.h)
 *
 * Nothing here touches the system; it is the platform-independent part of
 * the services declared in os.h: the text encoding switch ("81 00") with
 * the code points the font back ends look glyphs up by, the file part of
 * a path, the main window's title, the Shift-JIS to UTF-8 conversion in
 * that encoding, and the text file format of the registry emulation.  The
 * Shift-JIS and UTF-8 primitives (Sjis_IsLead, Utf8_Decode, ..., bgi/sjis.h)
 * and the wildcards (Sjis_WildcardMatch) are the core's.
 */
#include "host/os.h"
#include "host/os_common.h"
#include "bgi/release.h"
#include "bgi/sjis.h"

#include <stdio.h>

// ---- the text encoding ("81 00") ------------------------------------------------------

int gOsTextUtf8; // OS_SetTextEncoding: 1 when the script text is UTF-8

// "81 00" of 1.653 on: the script text is UTF-8 (1) or Shift-JIS (0); see os.h
void OS_SetTextEncoding(int utf8)
{
	gOsTextUtf8 = utf8 != 0;
}

/* the code point of the character at `s` (`len` bytes, as the engine's
 * TextGetChar counted it) in the text encoding: the font back ends look
 * the glyph up by it.  In Shift-JIS mode the two-byte codes go through
 * the code page 932 table (Sjis_CodePoint), 0xa1 .. 0xdf are the
 * half-width katakana (U+FF61 ..) and 0x5c stays the backslash, not the
 * yen sign. */
uint32_t OsCommon_CodePoint(const char* s, int len)
{
	const uint8_t* u = (const uint8_t*)s;
	if(gOsTextUtf8)
	{
		int n;
		uint32_t cp = Utf8_Decode(u, &n);
		if(cp - 0xd800u < 0x400u && len > n)
		{
			// a surrogate pair encoded as two three-byte sequences (CESU-8): combine them
			uint32_t lo = ((u[n] & 0x0f) << 12) | ((u[n + 1] & 0x3f) << 6) | (u[n + 2] & 0x3f);
			if(lo - 0xdc00u < 0x400u)
				cp = (((cp - 0xd800u) << 10) | (lo - 0xdc00u)) + 0x10000u;
		}
		return cp;
	}
	if(len >= 2 && Sjis_IsLead(u[0]))
		return Sjis_CodePoint((uint16_t)((u[0] << 8) | u[1]));
	if(u[0] >= 0xa1 && u[0] <= 0xdf)
		return 0xff61 + (u[0] - 0xa1); // half-width katakana
	return u[0];                       // (0x5c stays the backslash glyph, not the yen sign)
}

// ---- paths ------------------------------------------------------------------------

/* the file part of a Windows path: what follows the last '\\' or '/'
 * (the whole path when there is none); a separator byte that is the
 * trail byte of a Shift-JIS character does not count */
const char* OsCommon_FilePart(const char* path)
{
	const char* p = path;
	const char* last = path;
	while(*p)
	{
		if((*p == '\\' || *p == '/') && !(p > path && Sjis_IsLead((uint8_t)p[-1])))
			last = p + 1;
		p += Sjis_IsLead((uint8_t)*p) && p[1] ? 2 : 1;
	}
	return last;
}

/* the main window's title as shown: "<game title> - OpenBGI v1.0"; a title
 * that begins with the engine's name (the ones the window is made with,
 * "OpenBGI v1.0" and "OpenBGI in Launcher mode") as it is, and "OpenBGI
 * v1.0" for an empty or NULL one */
void OsCommon_MainTitle(char* out, size_t n, const char* sjisTitle)
{
	if(!sjisTitle || !*sjisTitle)
		snprintf(out, n, "%s", OPENBGI_TITLE);
	else if(strncmp(sjisTitle, OPENBGI_NAME, sizeof OPENBGI_NAME - 1) == 0)
		snprintf(out, n, "%s", sjisTitle);
	else
		snprintf(out, n, "%s - %s", sjisTitle, OPENBGI_TITLE);
}

// ---- registry emulation (used by the POSIX back end; the format is shared) ---------
/* The handful of registry values the engine writes (the install folder,
 * the file association) are kept in a plain text file of
 * "key\value=data" lines, one value per line; keys are compared without
 * regard to case and with '/' and '\\' as the same separator.  Every write
 * rewrites the whole file. */

// ASCII lower case: the key comparison folds only the ASCII letters
static int OsCommon_Lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

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

// the core's conversion (Sjis_ToUtf8) in the text encoding the host was told
size_t OsCommon_SjisToUtf8(const char* sjis, char* out, size_t n)
{
	return Sjis_ToUtf8(sjis, out, n, gOsTextUtf8);
}
