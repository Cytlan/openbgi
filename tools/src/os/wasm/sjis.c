/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sjis.c - Shift-JIS for the WebAssembly back end
 *
 * No iconv here: the browser's TextDecoder("shift_jis") decodes every
 * two-byte code once into a table (WasmHost_SjisTable), and the reverse
 * direction - needed for typed text and "81 20" only - searches it.
 *
 * The interface is the conversion part of os.h (OS_SjisToUtf8,
 * OS_SjisToCodePoint, OS_Utf8ToSjis) and the OsWasm_Sjis* / OsWasm_UnicodeToSjis
 * entries of os_wasm.h, which the other files of the back end use for the
 * text they hand the page.  The table is built on the first conversion;
 * OsCommon_* (os_common.c) does the UTF-8 side.
 */
#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_wasm.h"

static uint32_t* gTable; // 0x10000 entries indexed by the two-byte code, 0 = unmapped; NULL until the first use

// build the table once; stays NULL when out of memory (every code is then unmapped)
static void Fill(void)
{
	if(gTable)
		return;
	gTable = (uint32_t*)calloc(0x10000, sizeof *gTable);
	if(gTable)
		WasmHost_SjisTable(gTable);
}

// one two-byte code to its Unicode scalar, U+FFFD when unmapped
uint32_t OsWasm_SjisChar(uint16_t code)
{
	Fill();
	return gTable && gTable[code] ? gTable[code] : 0xfffd;
}

uint32_t OS_SjisToCodePoint(uint16_t sjis)
{
	return OsWasm_SjisChar(sjis);
}

// a Shift-JIS string to UTF-8 in `out` (n bytes), NUL-terminated
void OsWasm_SjisToUtf8(const char* sjis, char* out, size_t n)
{
	OsCommon_SjisToUtf8(sjis, out, n, OsWasm_SjisChar);
}

size_t OS_SjisToUtf8(const char* sjis, char* out, size_t n)
{
	return OsCommon_SjisToUtf8(sjis, out, n, OsWasm_SjisChar);
}

/* The code of one scalar: ASCII and the single-byte katakana directly, the
 * two Shift-JIS specials (yen, overline) by hand, everything else by a
 * linear search of the table (the reverse direction is rare).  The bytes
 * written to `out` (1 or 2), 0 when CP932 has no code for it. */
int OsWasm_UnicodeToSjis(uint32_t cp, char out[2])
{
	uint32_t code;
	if(cp < 0x80)
	{
		out[0] = (char)cp;
		return 1;
	}
	// the JIS X 0201 katakana are single bytes 0xA1 .. 0xDF
	if(cp >= 0xff61 && cp <= 0xff9f)
	{
		out[0] = (char)(cp - 0xff61 + 0xa1);
		return 1;
	}
	if(cp == 0xa5) // the yen sign is the backslash's code
	{
		out[0] = 0x5c;
		return 1;
	}
	if(cp == 0x203e) // the overline is the tilde's
	{
		out[0] = 0x7e;
		return 1;
	}
	Fill();
	if(!gTable)
		return 0;
	for(code = 0x8100; code < 0x10000; code++) // the first lead byte is 0x81
	{
		if(gTable[code] == cp)
		{
			out[0] = (char)(code >> 8);
			out[1] = (char)code;
			return 2;
		}
	}
	return 0;
}

/* UTF-8 to Shift-JIS ("81 20"): a character without a code becomes '?', a
 * malformed byte is taken as a code point of its own; `out` (n bytes) is
 * NUL-terminated, the result its length */
size_t OS_Utf8ToSjis(const char* utf8, char* out, size_t n)
{
	const uint8_t* u = (const uint8_t*)utf8;
	size_t o = 0;
	while(*u && o + 2 < n) // room for a two-byte code and the NUL
	{
		int len, m;
		uint32_t cp = OsCommon_Utf8SeqLen(u) ? OsCommon_Utf8Decode(u, &len) : (len = 1, (uint32_t)*u);
		m = OsWasm_UnicodeToSjis(cp, out + o);
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
