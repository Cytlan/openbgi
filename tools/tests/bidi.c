/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/bidi.c - unit test of the bidirectional text functions and the
 *                Arabic letter joining (src/core/bidi.c, inc/bgi/bidi.h);
 *                built as bin/test_bidi by `make test`
 *
 * The strings are arrays of Unicode code points in reading order.  A
 * visual order is checked as the list of logical indices from left to
 * right, the way Bidi_Reorder returns it.
 *
 * The groups of tests, in the order they run:
 *
 *   classes  - the character types of a few representative characters
 *   levels   - the embedding levels of Arabic and Hebrew text with Latin
 *              words, numbers and punctuation inside, in a right-to-left
 *              and in a left-to-right paragraph
 *   reorder  - the visual order of a line, the white space at its end
 *   mirror   - the mirrored brackets
 *   shaping  - the presentation forms of joined Arabic letters, letters
 *              that only join backwards, marks between letters, the
 *              lam-alef ligature, Persian letters, non-Arabic text
 */
#include "bgi/bidi.h"

#include <stdio.h>
#include <string.h>

static int gFails; // the number of failed checks so far
// evaluate a condition; print it with its location and count a failure when it is false
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

#define COUNT(a) ((int)(sizeof(a) / sizeof(a)[0]))

// the levels of a paragraph as a string of digits, for comparing at a glance
static const char* Levels(const uint32_t* cp, int n, int base)
{
	static char buf[128];
	uint8_t lv[128];
	int i;
	Bidi_Levels(cp, n, base, lv);
	for(i = 0; i < n; i++)
		buf[i] = (char)('0' + lv[i]);
	buf[n] = 0;
	return buf;
}

// the visual order of a one-line paragraph as a string of the logical indices (one hex digit each)
static const char* Order(const uint32_t* cp, int n, int base)
{
	static char buf[128];
	uint8_t lv[128];
	int order[128], i;
	Bidi_Levels(cp, n, base, lv);
	Bidi_Reorder(cp, lv, n, base, order);
	for(i = 0; i < n; i++)
		buf[i] = "0123456789abcdef"[order[i] & 15];
	buf[n] = 0;
	return buf;
}

static void TestClasses(void)
{
	CHECK(Bidi_ClassOf('a') == BIDI_L);
	CHECK(Bidi_ClassOf(0x3042) == BIDI_L);  // hiragana a
	CHECK(Bidi_ClassOf(0x05d0) == BIDI_R);  // Hebrew alef
	CHECK(Bidi_ClassOf(0x0627) == BIDI_AL); // Arabic alef
	CHECK(Bidi_ClassOf(0xfe8d) == BIDI_AL); // its presentation form
	CHECK(Bidi_ClassOf('7') == BIDI_EN);
	CHECK(Bidi_ClassOf(0x0667) == BIDI_AN); // Arabic-Indic seven
	CHECK(Bidi_ClassOf(0x06f7) == BIDI_EN); // Persian seven
	CHECK(Bidi_ClassOf('+') == BIDI_ES);
	CHECK(Bidi_ClassOf('%') == BIDI_ET);
	CHECK(Bidi_ClassOf(',') == BIDI_CS);
	CHECK(Bidi_ClassOf(0x060c) == BIDI_CS);  // Arabic comma
	CHECK(Bidi_ClassOf(0x064e) == BIDI_NSM); // fatha
	CHECK(Bidi_ClassOf(0x05b0) == BIDI_NSM); // sheva
	CHECK(Bidi_ClassOf(0x200d) == BIDI_BN);  // zero-width joiner
	CHECK(Bidi_ClassOf(' ') == BIDI_WS);
	CHECK(Bidi_ClassOf(0x3000) == BIDI_WS);
	CHECK(Bidi_ClassOf('!') == BIDI_ON);
	CHECK(Bidi_ClassOf(0x00ab) == BIDI_ON);
	CHECK(Bidi_ClassOf(0x3001) == BIDI_ON); // ideographic comma
	CHECK(Bidi_IsRtlScript(0x0628) && Bidi_IsRtlScript(0x05d1) && Bidi_IsRtlScript(0xfef5) && Bidi_IsRtlScript(0x064e));
	CHECK(!Bidi_IsRtlScript('a') && !Bidi_IsRtlScript('1') && !Bidi_IsRtlScript(0x3042) && !Bidi_IsRtlScript(' '));
}

static void TestLevels(void)
{
	// Arabic only: everything on level 1 in a right-to-left paragraph, the same in a left-to-right one except that it is an embedded run there too
	static const uint32_t ar[] = {0x645, 0x631, 0x62d, 0x628, 0x627};
	// "AB cd EF" (capitals standing for Arabic letters): a Latin word inside Arabic
	static const uint32_t mixed[] = {0x627, 0x628, ' ', 'c', 'd', ' ', 0x627, 0x628};
	// Arabic, a European number with a separator, Arabic: "AB 2:30 AB"
	static const uint32_t number[] = {0x627, 0x628, ' ', '2', ':', '3', '0', ' ', 0x627, 0x628};
	// a number after Latin text in a right-to-left paragraph: "ab 12"
	static const uint32_t latinNum[] = {'a', 'b', ' ', '1', '2'};
	// punctuation at the start and the end of Arabic: "<<A B>>." with guillemets
	static const uint32_t quoted[] = {0xab, 0x627, ' ', 0x628, 0xbb, '.'};
	// "abc (DEF) ghi." in a left-to-right paragraph
	static const uint32_t paren[] = {'a', ' ', '(', 0x5d0, 0x5d1, ')', ' ', 'b', '.'};
	// a mark takes the level of its letter
	static const uint32_t marks[] = {'a', 0x301, ' ', 0x628, 0x64e, 0x62a};
	// a percent sign after a number inside Arabic: the digits count as Arabic digits there, and the sign follows the text
	static const uint32_t percent[] = {0x628, ' ', '5', '0', '%', ' ', 0x628};

	CHECK(strcmp(Levels(ar, COUNT(ar), 1), "11111") == 0);
	CHECK(strcmp(Levels(ar, COUNT(ar), 0), "11111") == 0);
	CHECK(strcmp(Levels(mixed, COUNT(mixed), 1), "11122111") == 0);
	CHECK(strcmp(Levels(mixed, COUNT(mixed), 0), "11000011") == 0);
	CHECK(strcmp(Levels(number, COUNT(number), 1), "1112222111") == 0);
	CHECK(strcmp(Levels(latinNum, COUNT(latinNum), 1), "22222") == 0);
	CHECK(strcmp(Levels(latinNum, COUNT(latinNum), 0), "00000") == 0);
	CHECK(strcmp(Levels(quoted, COUNT(quoted), 1), "111111") == 0);
	CHECK(strcmp(Levels(paren, COUNT(paren), 0), "000110000") == 0);
	CHECK(strcmp(Levels(marks, COUNT(marks), 0), "000111") == 0);
	CHECK(strcmp(Levels(percent, COUNT(percent), 1), "1122111") == 0);
}

static void TestReorder(void)
{
	static const uint32_t ar[] = {0x645, 0x631, 0x62d};
	static const uint32_t mixed[] = {0x627, 0x628, ' ', 'c', 'd', ' ', 0x627, 0x628};
	static const uint32_t number[] = {0x627, ' ', '1', '2', ' ', 0x628};
	static const uint32_t trail[] = {'a', 'b', ' ', 0x627, 0x628, ' '};

	// right-to-left text is simply reversed
	CHECK(strcmp(Order(ar, COUNT(ar), 1), "210") == 0);
	CHECK(strcmp(Order(ar, COUNT(ar), 0), "210") == 0);
	// the Latin word keeps its direction inside the reversed line
	CHECK(strcmp(Order(mixed, COUNT(mixed), 1), "76534210") == 0);
	// in a left-to-right paragraph the two Arabic words are each reversed in place
	CHECK(strcmp(Order(mixed, COUNT(mixed), 0), "10234576") == 0);
	// a number reads left to right
	CHECK(strcmp(Order(number, COUNT(number), 1), "542310") == 0);
	// the space at the end of the line goes to the paragraph's end side: right of the Arabic in a left-to-right paragraph
	CHECK(strcmp(Order(trail, COUNT(trail), 0), "012435") == 0);
	// and to the far left in a right-to-left one
	CHECK(strcmp(Order(trail, COUNT(trail), 1), "543201") == 0);
}

static void TestMirror(void)
{
	CHECK(Bidi_Mirror('(') == ')' && Bidi_Mirror(')') == '(');
	CHECK(Bidi_Mirror('[') == ']' && Bidi_Mirror('<') == '>');
	CHECK(Bidi_Mirror(0x00ab) == 0x00bb && Bidi_Mirror(0x00bb) == 0x00ab);
	CHECK(Bidi_Mirror(0x300c) == 0x300d);
	CHECK(Bidi_Mirror('a') == 'a' && Bidi_Mirror('.') == '.' && Bidi_Mirror(0x0628) == 0x0628);
}

static void TestShaping(void)
{
	uint32_t out[16];
	// beh beh beh: initial, medial, final
	static const uint32_t bbb[] = {0x628, 0x628, 0x628};
	// a single letter: isolated
	static const uint32_t one[] = {0x628};
	// dal joins only backwards: beh dal beh -> initial, final, isolated
	static const uint32_t bdb[] = {0x628, 0x62f, 0x628};
	// a mark between two letters does not break the joining
	static const uint32_t marked[] = {0x628, 0x64e, 0x628};
	// lam alef: one ligature; after a beh it is the final form and the next letter starts anew
	static const uint32_t la[] = {0x644, 0x627};
	static const uint32_t bla[] = {0x628, 0x644, 0x627, 0x628};
	// lam alef-with-hamza
	static const uint32_t lah[] = {0x644, 0x623};
	// a space separates words; Latin letters pass through
	static const uint32_t words[] = {0x628, 0x628, ' ', 'a', 0x628};
	// Persian peh and gaf have forms too; tatweel joins on both sides
	static const uint32_t persian[] = {0x67e, 0x6af};
	static const uint32_t tatweel[] = {0x628, 0x640, 0x628};
	// hamza does not join
	static const uint32_t hamza[] = {0x628, 0x621, 0x628};

	Arabic_Shape(bbb, 3, out);
	CHECK(out[0] == 0xfe91 && out[1] == 0xfe92 && out[2] == 0xfe90);
	Arabic_Shape(one, 1, out);
	CHECK(out[0] == 0xfe8f);
	Arabic_Shape(bdb, 3, out);
	CHECK(out[0] == 0xfe91 && out[1] == 0xfeaa && out[2] == 0xfe8f);
	Arabic_Shape(marked, 3, out);
	CHECK(out[0] == 0xfe91 && out[1] == 0x64e && out[2] == 0xfe90);
	Arabic_Shape(la, 2, out);
	CHECK(out[0] == 0xfefb && out[1] == 0);
	Arabic_Shape(bla, 4, out);
	CHECK(out[0] == 0xfe91 && out[1] == 0xfefc && out[2] == 0 && out[3] == 0xfe8f);
	Arabic_Shape(lah, 2, out);
	CHECK(out[0] == 0xfef7 && out[1] == 0);
	Arabic_Shape(words, 5, out);
	CHECK(out[0] == 0xfe91 && out[1] == 0xfe90 && out[2] == ' ' && out[3] == 'a' && out[4] == 0xfe8f);
	Arabic_Shape(persian, 2, out);
	CHECK(out[0] == 0xfb58 && out[1] == 0xfb93);
	Arabic_Shape(tatweel, 3, out);
	CHECK(out[0] == 0xfe91 && out[1] == 0x640 && out[2] == 0xfe90);
	Arabic_Shape(hamza, 3, out);
	CHECK(out[0] == 0xfe8f && out[1] == 0x621 && out[2] == 0xfe8f);
}

int main(void)
{
	TestClasses();
	TestLevels();
	TestReorder();
	TestMirror();
	TestShaping();
	if(gFails)
	{
		printf("bidi: %d checks failed\n", gFails);
		return 1;
	}
	printf("bidi: all tests passed\n");
	return 0;
}
