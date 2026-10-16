/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sjis.c - the Shift-JIS conversions of the POSIX back end
 *          (inc/bgi/os_posix.h, the OS_Sjis* / OS_Utf8ToSjis services of
 *          inc/bgi/os.h): CP932 <-> UTF-8 through iconv, with a lazily
 *          built table for single characters
 *
 * The engine's strings are Shift-JIS throughout (the scripts, file names,
 * window titles); everything this back end hands to the system - paths,
 * fontconfig, Xlib - wants UTF-8.  The string conversion itself is
 * OsCommon_SjisToUtf8; this file supplies its two-byte lookup, which
 * asks iconv once per code and remembers the answer.  Without the
 * converters (iconv_open failed) every two-byte code is U+FFFD and no
 * code point beyond ASCII has a Shift-JIS form.
 */
#include "posix_internal.h"

static iconv_t gSjisToUtf8 = (iconv_t)-1; // the CP932 -> UTF-8 converter, (iconv_t)-1 when not open
static iconv_t gUtf8ToSjis = (iconv_t)-1; // and the reverse one
static uint32_t gSjisTable[0x10000];      // two-byte Shift-JIS -> Unicode, 0 = not asked yet, 0xfffd = unmapped

// Open the CP932 converters (OS_Init).
void OsPosix_SjisInit(void)
{
	gSjisToUtf8 = iconv_open("UTF-8", "CP932");
	gUtf8ToSjis = iconv_open("CP932", "UTF-8");
}

// Close them (OS_Shutdown).
void OsPosix_SjisShutdown(void)
{
	if(gSjisToUtf8 != (iconv_t)-1)
		iconv_close(gSjisToUtf8);
	if(gUtf8ToSjis != (iconv_t)-1)
		iconv_close(gUtf8ToSjis);
	gSjisToUtf8 = (iconv_t)-1;
	gUtf8ToSjis = (iconv_t)-1;
}

// the code point of a two-byte code from the table, converting it on the first request; 0xfffd when unmapped
static uint32_t SjisLookup(uint16_t code)
{
	if(gSjisTable[code] == 0 && gSjisToUtf8 != (iconv_t)-1)
	{
		char in[2] = {(char)(code >> 8), (char)code};
		char out[8];
		char* ip = in;
		char* op = out;
		size_t il = 2, ol = sizeof out;
		iconv(gSjisToUtf8, NULL, NULL, NULL, NULL); // reset the shift state
		if(iconv(gSjisToUtf8, &ip, &il, &op, &ol) != (size_t)-1 && il == 0)
		{
			// decode the UTF-8 just produced (CP932 maps into the BMP: at most three bytes)
			const uint8_t* u = (const uint8_t*)out;
			size_t len = sizeof out - ol;
			uint32_t cp = 0xfffd;
			if(len == 1)
				cp = u[0];
			else if(len == 2)
				cp = ((u[0] & 0x1f) << 6) | (u[1] & 0x3f);
			else if(len == 3)
				cp = ((u[0] & 0x0f) << 12) | ((u[1] & 0x3f) << 6) | (u[2] & 0x3f);
			gSjisTable[code] = cp;
		}
		else
			gSjisTable[code] = 0xfffd;
	}
	return gSjisTable[code] ? gSjisTable[code] : 0xfffd;
}

uint32_t OsPosix_SjisChar(uint16_t code)
{
	return SjisLookup(code);
}

uint32_t OS_SjisToCodePoint(uint16_t sjis)
{
	return SjisLookup(sjis);
}

// UTF-8 -> Shift-JIS character by character ('?' for a character without a code, a stray byte taken as Latin-1); the length written
size_t OS_Utf8ToSjis(const char* utf8, char* out, size_t n)
{
	const uint8_t* u = (const uint8_t*)utf8;
	size_t o = 0;
	while(*u && o + 2 < n)
	{
		int len, m;
		uint32_t cp = OsCommon_Utf8SeqLen(u) ? OsCommon_Utf8Decode(u, &len) : (len = 1, (uint32_t)*u);
		m = OsPosix_UnicodeToSjis(cp, out + o);
		if(m == 0)
		{
			out[o] = '?';
			m = 1;
		}
		o += (size_t)m;
		u += len;
	}
	out[o] = 0;
	return o;
}

// the shared conversion with this back end's lookup (the two entry points differ only in returning the length)
void OsPosix_SjisToUtf8(const char* sjis, char* out, size_t n)
{
	OsCommon_SjisToUtf8(sjis, out, n, SjisLookup);
}

size_t OS_SjisToUtf8(const char* sjis, char* out, size_t n)
{
	return OsCommon_SjisToUtf8(sjis, out, n, SjisLookup);
}

/* the reverse direction for keyboard input and face names: one code point
 * -> 1 or 2 Shift-JIS bytes in `out`; the byte count, 0 when CP932 has no
 * code (or the code point is outside the BMP, or there is no converter) */
int OsPosix_UnicodeToSjis(uint32_t cp, char out[2])
{
	char in[4];
	char* ip = in;
	char* op = out;
	size_t il, ol = 2;
	if(cp < 0x80)
	{
		out[0] = (char)cp;
		return 1;
	}
	if(gUtf8ToSjis == (iconv_t)-1)
		return 0;
	if(cp < 0x800)
	{
		in[0] = (char)(0xc0 | (cp >> 6));
		in[1] = (char)(0x80 | (cp & 0x3f));
		il = 2;
	}
	else if(cp < 0x10000)
	{
		in[0] = (char)(0xe0 | (cp >> 12));
		in[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		in[2] = (char)(0x80 | (cp & 0x3f));
		il = 3;
	}
	else
		return 0;
	iconv(gUtf8ToSjis, NULL, NULL, NULL, NULL); // reset the shift state
	if(iconv(gUtf8ToSjis, &ip, &il, &op, &ol) == (size_t)-1 || il != 0)
		return 0;
	return (int)(2 - ol); // the bytes it took of the two available
}
