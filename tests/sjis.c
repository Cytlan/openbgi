/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/sjis.c - unit test of the built-in code page 932 table
 *                (src/base/sjis.c, src/include/bgi/sjis.h); built as test_sjis
 *                by `make test`
 *
 * The groups of tests, in the order they run:
 *
 *   decode    - single codes of each part of the code page: JIS X 0208,
 *               the NEC row 13, the NEC-selected and IBM extensions, the
 *               user-defined area, codes that are no character
 *   encode    - the code Windows picks for a scalar with several, the
 *               JIS characters code page 932 decodes under another scalar,
 *               scalars without a code
 *   roundtrip - every code decodes to a scalar that encodes back to it or
 *               to the code of the same character Windows prefers
 *   strings   - Shift-JIS to UTF-8 and back
 */
#include "bgi/sjis.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

// the code of a scalar as one number (the lead byte high), 0 when there is none
static uint32_t Enc(uint32_t cp)
{
	char out[2];
	int n = Sjis_FromCodePoint(cp, out);
	if(n == 1)
		return (uint8_t)out[0];
	if(n == 2)
		return ((uint32_t)(uint8_t)out[0] << 8) | (uint8_t)out[1];
	return 0;
}

static void TestDecode(void)
{
	CHECK(Sjis_CodePoint(0x82a0) == 0x3042); // あ
	CHECK(Sjis_CodePoint(0x889f) == 0x4e9c); // 亜, the first kanji
	CHECK(Sjis_CodePoint(0x815c) == 0x2015); // the horizontal bar, not the em dash
	CHECK(Sjis_CodePoint(0x8160) == 0xff5e); // the full-width tilde, not the wave dash
	CHECK(Sjis_CodePoint(0x815f) == 0xff3c); // the full-width backslash
	CHECK(Sjis_CodePoint(0x8754) == 0x2160); // Ⅰ, NEC row 13
	CHECK(Sjis_CodePoint(0xfa40) == 0x2170); // ⅰ, IBM extension
	CHECK(Sjis_CodePoint(0xed40) == 0x7e8a); // 纊, NEC-selected IBM extension
	CHECK(Sjis_CodePoint(0xfa5c) == 0x7e8a); // the same character among the IBM extensions
	CHECK(Sjis_CodePoint(0xf040) == 0xe000); // the user-defined area starts at U+E000
	CHECK(Sjis_CodePoint(0xf9fc) == 0xe757);
	CHECK(Sjis_CodePoint(0x81ad) == 0xfffd); // a gap in row 2
	CHECK(Sjis_CodePoint(0x8130) == 0xfffd); // a trail byte below 0x40
	CHECK(Sjis_CodePoint(0xa0a0) == 0xfffd); // not a lead byte
	CHECK(Sjis_CodePoint(0x82f2) == 0xfffd); // past the hiragana
}

static void TestEncode(void)
{
	CHECK(Enc('A') == 'A');
	CHECK(Enc('\\') == 0x5c);
	CHECK(Enc(0xff71) == 0xb1);   // half-width ｱ
	CHECK(Enc(0x3042) == 0x82a0); // あ
	CHECK(Enc(0x2252) == 0x81e0); // ≒: JIS X 0208 before the NEC row 13 (0x8790)
	CHECK(Enc(0x2235) == 0x81e6); // ∵: JIS X 0208 before NEC (0x879a) and IBM (0xfa5b)
	CHECK(Enc(0xffe2) == 0x81ca); // ¬: JIS X 0208 before both extensions
	CHECK(Enc(0x2160) == 0x8754); // Ⅰ: NEC row 13 before IBM (0xfa4a)
	CHECK(Enc(0x2170) == 0xfa40); // ⅰ: IBM before NEC-selected (0xeeef)
	CHECK(Enc(0x7e8a) == 0xfa5c); // 纊: IBM before NEC-selected (0xed40)
	CHECK(Enc(0xe000) == 0xf040);
	// the JIS characters code page 932 decodes under another scalar
	CHECK(Enc(0x301c) == 0x8160); // the wave dash
	CHECK(Enc(0x2212) == 0x817c); // the minus sign
	CHECK(Enc(0x00a5) == 0x5c);   // the yen sign
	CHECK(Enc(0x203e) == 0x7e);   // the overline
	CHECK(Enc(0x00a2) == 0x8191);
	CHECK(Enc(0x2014) == 0x815c);
	// no code, and no best fit to a similar letter
	CHECK(Enc(0x00e5) == 0); // å
	CHECK(Enc(0x20ac) == 0); // €
	CHECK(Enc(0x1f600) == 0);
}

// the group of a code: JIS X 0208 0, NEC row 13 1, IBM extensions 2, NEC-selected IBM extensions 3
static int Group(uint32_t code)
{
	if(code >= 0x8740 && code <= 0x879c)
		return 1;
	if(code >= 0xfa40 && code <= 0xfc4b)
		return 2;
	if(code >= 0xed40 && code <= 0xeefc)
		return 3;
	return 0;
}

static void TestRoundTrip(void)
{
	uint32_t code;
	int codes = 0, bad = 0;
	for(code = 0x8140; code <= 0xfcfc; code++)
	{
		uint32_t cp = Sjis_CodePoint((uint16_t)code), back;
		if(cp == 0xfffd)
			continue;
		codes++;
		back = Enc(cp);
		// the same code, or one of a group Windows prefers that decodes to the same scalar
		if(back != code && (Sjis_CodePoint((uint16_t)back) != cp || Group(back) > Group(code) || (Group(back) == Group(code) && back > code)))
			bad++;
	}
	CHECK(codes == 9604); // every two-byte code of the code page
	CHECK(bad == 0);
	if(bad)
		printf("  %d codes do not round-trip\n", bad);
}

static void TestStrings(void)
{
	char utf8[64], sjis[64];
	// "あ亜Ⅰｱ\x" in Shift-JIS
	static const char kSjis[] = "\x82\xa0\x88\x9f\x87\x54\xb1\\x";
	size_t n = Sjis_ToUtf8(kSjis, utf8, sizeof utf8, 0);
	CHECK(n == 14);
	CHECK(strcmp(utf8, "\xe3\x81\x82\xe4\xba\x9c\xe2\x85\xa0\xef\xbd\xb1\\x") == 0);
	n = Sjis_FromUtf8(utf8, sjis, sizeof sjis);
	CHECK(n == strlen(kSjis));
	CHECK(strcmp(sjis, kSjis) == 0);
	// a character without a code becomes '?', a stray byte is taken as Latin-1 (no code either)
	n = Sjis_FromUtf8("a\xc3\xa5\x62\xff", sjis, sizeof sjis); // "aåb" and a stray byte
	CHECK(n == 4 && strcmp(sjis, "a?b?") == 0);
	// a lone lead byte at the end is U+FFFD; the output keeps whole characters only
	n = Sjis_ToUtf8("\x82", utf8, sizeof utf8, 0);
	CHECK(n == 3 && strcmp(utf8, "\xef\xbf\xbd") == 0);
	n = Sjis_ToUtf8("\x82\xa0\x82\xa2", utf8, 5, 0);
	CHECK(n == 3 && strcmp(utf8, "\xe3\x81\x82") == 0);
}

int main(void)
{
	TestDecode();
	TestEncode();
	TestRoundTrip();
	TestStrings();
	if(gFails)
	{
		printf("sjis: %d checks failed\n", gFails);
		return 1;
	}
	printf("sjis: all tests passed\n");
	return 0;
}
