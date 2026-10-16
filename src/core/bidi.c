/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bidi.c - bidirectional text and Arabic letter joining (inc/bgi/bidi.h)
 *
 * An OpenBGI extension; no original build has right-to-left text.  The
 * text layout (src/gfx/text/layout_bidi.c) calls these functions with the
 * code points of a paragraph in reading order.
 *
 * The level resolution follows the Unicode bidirectional algorithm
 * (UAX #9) for a paragraph without explicit embedding controls: the rules
 * W1 .. W7 for the "weak" types (digits, separators, marks), N1 / N2 for
 * the neutral ones (spaces, punctuation), I1 / I2 for the levels, and
 * L1 / L2 for a line's visual order.  Left out are the embedding and
 * isolate controls (they are treated as invisible) and the bracket-pair
 * rule N0, so a bracket takes its direction from its neighbours like any
 * other punctuation.
 *
 * The joining follows the Arabic joining rules of the Unicode standard
 * (chapter 9) and writes the result as characters of the Arabic
 * Presentation Forms blocks, which most fonts with Arabic letters carry as
 * ordinary glyphs.  That needs no shaping tables of the font, at the price
 * of the font's finer ligatures and mark placement.
 */
#include <string.h>

#include "bgi/bidi.h"

// ---- character classes -----------------------------------------------------------------------

typedef struct ClassRange // the characters lo .. hi have class cls
{
	uint32_t lo, hi;
	uint8_t cls;
} ClassRange_t;

/* the classes that are not L, in ascending order; where ranges overlap
 * the first one that contains the character wins, so the marks inside a
 * script are listed before the script itself */
static const ClassRange_t kClasses[] = {
	{0x0000, 0x0008, BIDI_BN},
	{0x0009, 0x0009, BIDI_S},
	{0x000a, 0x000a, BIDI_B},
	{0x000b, 0x000b, BIDI_S},
	{0x000c, 0x000c, BIDI_WS},
	{0x000d, 0x000d, BIDI_B},
	{0x000e, 0x001b, BIDI_BN},
	{0x001c, 0x001e, BIDI_B},
	{0x001f, 0x001f, BIDI_S},
	{0x0020, 0x0020, BIDI_WS},
	{0x0021, 0x0022, BIDI_ON},
	{0x0023, 0x0025, BIDI_ET},
	{0x0026, 0x002a, BIDI_ON},
	{0x002b, 0x002b, BIDI_ES},
	{0x002c, 0x002c, BIDI_CS},
	{0x002d, 0x002d, BIDI_ES},
	{0x002e, 0x002f, BIDI_CS},
	{0x0030, 0x0039, BIDI_EN},
	{0x003a, 0x003a, BIDI_CS},
	{0x003b, 0x0040, BIDI_ON},
	{0x005b, 0x0060, BIDI_ON},
	{0x007b, 0x007e, BIDI_ON},
	{0x007f, 0x0084, BIDI_BN},
	{0x0085, 0x0085, BIDI_B},
	{0x0086, 0x009f, BIDI_BN},
	{0x00a0, 0x00a0, BIDI_CS},
	{0x00a1, 0x00a1, BIDI_ON},
	{0x00a2, 0x00a5, BIDI_ET},
	{0x00a6, 0x00a9, BIDI_ON},
	{0x00ab, 0x00ac, BIDI_ON},
	{0x00ad, 0x00ad, BIDI_BN},
	{0x00ae, 0x00af, BIDI_ON},
	{0x00b0, 0x00b1, BIDI_ET},
	{0x00b2, 0x00b3, BIDI_EN},
	{0x00b4, 0x00b4, BIDI_ON},
	{0x00b6, 0x00b8, BIDI_ON},
	{0x00b9, 0x00b9, BIDI_EN},
	{0x00bb, 0x00bf, BIDI_ON},
	{0x00d7, 0x00d7, BIDI_ON},
	{0x00f7, 0x00f7, BIDI_ON},
	{0x02b9, 0x02ba, BIDI_ON},
	{0x02c2, 0x02cf, BIDI_ON},
	{0x02d2, 0x02df, BIDI_ON},
	{0x02e5, 0x02ed, BIDI_ON},
	{0x02ef, 0x02ff, BIDI_ON},
	{0x0300, 0x036f, BIDI_NSM},
	{0x0374, 0x0375, BIDI_ON},
	{0x037e, 0x037e, BIDI_ON},
	{0x0384, 0x0385, BIDI_ON},
	{0x0387, 0x0387, BIDI_ON},
	{0x03f6, 0x03f6, BIDI_ON},
	{0x0483, 0x0489, BIDI_NSM},
	{0x058a, 0x058a, BIDI_ON},
	// Hebrew: the points, then the letters and punctuation
	{0x0591, 0x05bd, BIDI_NSM},
	{0x05bf, 0x05bf, BIDI_NSM},
	{0x05c1, 0x05c2, BIDI_NSM},
	{0x05c4, 0x05c5, BIDI_NSM},
	{0x05c7, 0x05c7, BIDI_NSM},
	{0x0590, 0x05ff, BIDI_R},
	// Arabic: number signs, marks and digits, then the letters
	{0x0600, 0x0605, BIDI_AN},
	{0x0606, 0x0607, BIDI_ON},
	{0x0609, 0x060a, BIDI_ET},
	{0x060c, 0x060c, BIDI_CS},
	{0x060e, 0x060f, BIDI_ON},
	{0x0610, 0x061a, BIDI_NSM},
	{0x064b, 0x065f, BIDI_NSM},
	{0x0660, 0x0669, BIDI_AN},
	{0x066a, 0x066a, BIDI_ET},
	{0x066b, 0x066c, BIDI_AN},
	{0x0670, 0x0670, BIDI_NSM},
	{0x06d6, 0x06dc, BIDI_NSM},
	{0x06dd, 0x06dd, BIDI_AN},
	{0x06de, 0x06de, BIDI_ON},
	{0x06df, 0x06e4, BIDI_NSM},
	{0x06e7, 0x06e8, BIDI_NSM},
	{0x06e9, 0x06e9, BIDI_ON},
	{0x06ea, 0x06ed, BIDI_NSM},
	{0x06f0, 0x06f9, BIDI_EN},
	{0x0600, 0x06ff, BIDI_AL},
	// Syriac, Thaana, NKo and the later Arabic blocks
	{0x0711, 0x0711, BIDI_NSM},
	{0x0730, 0x074a, BIDI_NSM},
	{0x07a6, 0x07b0, BIDI_NSM},
	{0x0700, 0x07bf, BIDI_AL},
	{0x07eb, 0x07f3, BIDI_NSM},
	{0x07c0, 0x085f, BIDI_R},
	{0x08e2, 0x08e2, BIDI_AN},
	{0x08d3, 0x08ff, BIDI_NSM},
	{0x0860, 0x08ff, BIDI_AL},
	// general punctuation, super- and subscripts, currency, symbols
	{0x2000, 0x200a, BIDI_WS},
	{0x200b, 0x200d, BIDI_BN},
	{0x200f, 0x200f, BIDI_R},
	{0x2010, 0x2027, BIDI_ON},
	{0x2028, 0x2028, BIDI_WS},
	{0x2029, 0x2029, BIDI_B},
	{0x202a, 0x202e, BIDI_BN},
	{0x202f, 0x202f, BIDI_CS},
	{0x2030, 0x2034, BIDI_ET},
	{0x2035, 0x2043, BIDI_ON},
	{0x2044, 0x2044, BIDI_CS},
	{0x2045, 0x205e, BIDI_ON},
	{0x205f, 0x205f, BIDI_WS},
	{0x2060, 0x2069, BIDI_BN},
	{0x2070, 0x2070, BIDI_EN},
	{0x2074, 0x2079, BIDI_EN},
	{0x207a, 0x207b, BIDI_ES},
	{0x207c, 0x207e, BIDI_ON},
	{0x2080, 0x2089, BIDI_EN},
	{0x208a, 0x208b, BIDI_ES},
	{0x208c, 0x208e, BIDI_ON},
	{0x20a0, 0x20cf, BIDI_ET},
	{0x20d0, 0x20f0, BIDI_NSM},
	{0x2116, 0x2118, BIDI_ON},
	{0x212e, 0x212e, BIDI_ET},
	{0x2150, 0x215f, BIDI_ON},
	{0x2189, 0x218b, BIDI_ON},
	{0x2190, 0x2211, BIDI_ON},
	{0x2212, 0x2212, BIDI_ES},
	{0x2213, 0x2213, BIDI_ET},
	{0x2214, 0x2335, BIDI_ON},
	{0x237b, 0x2394, BIDI_ON},
	{0x2396, 0x244a, BIDI_ON},
	{0x2460, 0x26ab, BIDI_ON},
	{0x26ad, 0x27ff, BIDI_ON},
	{0x2900, 0x2b73, BIDI_ON},
	{0x2e00, 0x2e4f, BIDI_ON},
	{0x2e80, 0x2fff, BIDI_ON},
	// the punctuation of the CJK blocks (ideographs and kana are L)
	{0x3000, 0x3000, BIDI_WS},
	{0x3001, 0x3004, BIDI_ON},
	{0x3008, 0x3020, BIDI_ON},
	{0x302a, 0x302d, BIDI_NSM},
	{0x3030, 0x3030, BIDI_ON},
	{0x3036, 0x3037, BIDI_ON},
	{0x303d, 0x303f, BIDI_ON},
	{0x3099, 0x309a, BIDI_NSM},
	{0x309b, 0x309c, BIDI_ON},
	{0x30a0, 0x30a0, BIDI_ON},
	{0x30fb, 0x30fb, BIDI_ON},
	// the presentation forms and the full-width forms
	{0xfb1e, 0xfb1e, BIDI_NSM},
	{0xfb29, 0xfb29, BIDI_ES},
	{0xfb1d, 0xfb4f, BIDI_R},
	{0xfd3e, 0xfd3f, BIDI_ON},
	{0xfdfd, 0xfdfd, BIDI_ON},
	{0xfb50, 0xfdff, BIDI_AL},
	{0xfe00, 0xfe0f, BIDI_NSM},
	{0xfe10, 0xfe19, BIDI_ON},
	{0xfe20, 0xfe2f, BIDI_NSM},
	{0xfe30, 0xfe4f, BIDI_ON},
	{0xfe50, 0xfe50, BIDI_CS},
	{0xfe52, 0xfe52, BIDI_CS},
	{0xfe55, 0xfe55, BIDI_CS},
	{0xfe5f, 0xfe5f, BIDI_ET},
	{0xfe62, 0xfe63, BIDI_ES},
	{0xfe69, 0xfe6a, BIDI_ET},
	{0xfe50, 0xfe6b, BIDI_ON},
	{0xfe70, 0xfefc, BIDI_AL},
	{0xfeff, 0xfeff, BIDI_BN},
	{0xff03, 0xff05, BIDI_ET},
	{0xff0b, 0xff0b, BIDI_ES},
	{0xff0c, 0xff0c, BIDI_CS},
	{0xff0d, 0xff0d, BIDI_ES},
	{0xff0e, 0xff0f, BIDI_CS},
	{0xff10, 0xff19, BIDI_EN},
	{0xff1a, 0xff1a, BIDI_CS},
	{0xff01, 0xff20, BIDI_ON},
	{0xff3b, 0xff40, BIDI_ON},
	{0xff5b, 0xff65, BIDI_ON},
	{0xffe0, 0xffe1, BIDI_ET},
	{0xffe5, 0xffe6, BIDI_ET},
	{0xffe2, 0xffee, BIDI_ON},
	// the right-to-left scripts of the supplementary plane
	{0x1ee00, 0x1eeff, BIDI_AL},
	{0x10800, 0x10fff, BIDI_R},
	{0x1e800, 0x1efff, BIDI_R},
};

BidiClass_t Bidi_ClassOf(uint32_t cp)
{
	size_t i;
	for(i = 0; i < BGI_COUNTOF(kClasses); i++)
		if(cp >= kClasses[i].lo && cp <= kClasses[i].hi)
			return (BidiClass_t)kClasses[i].cls;
	return BIDI_L;
}

int Bidi_IsRtlScript(uint32_t cp)
{
	return (cp >= 0x0590 && cp <= 0x08ff) || (cp >= 0xfb1d && cp <= 0xfdff) || (cp >= 0xfe70 && cp <= 0xfefc) ||
		(cp >= 0x10800 && cp <= 0x10fff) || (cp >= 0x1e800 && cp <= 0x1efff);
}

// ---- levels ------------------------------------------------------------------------------------

// spaces, separators and other punctuation: the types rule N1 / N2 resolve
static int IsNeutral(int t)
{
	return t == BIDI_B || t == BIDI_S || t == BIDI_WS || t == BIDI_ON;
}

// what a type counts as on the edge of a run of neutrals: digits side with right-to-left text
static int StrongSide(int t)
{
	return (t == BIDI_EN || t == BIDI_AN) ? BIDI_R : t;
}

void Bidi_Levels(const uint32_t* cp, int n, int baseLevel, uint8_t* levels)
{
	int embed = (baseLevel & 1) ? BIDI_R : BIDI_L; // the paragraph's direction as a type: it also stands on both ends of the text
	uint8_t* t;
	int i, j, lastStrong;

	if(n <= 0)
		return;
	t = (uint8_t*)BGI_Alloc((size_t)n);
	for(i = 0; i < n; i++)
		t[i] = (uint8_t)Bidi_ClassOf(cp[i]);

	// W1: a combining mark (and an invisible character) takes the type of what it follows
	for(i = 0; i < n; i++)
		if(t[i] == BIDI_NSM || t[i] == BIDI_BN)
			t[i] = (uint8_t)(i ? t[i - 1] : embed);
	// W2: European digits after Arabic letters are Arabic digits
	lastStrong = embed;
	for(i = 0; i < n; i++)
	{
		if(t[i] == BIDI_L || t[i] == BIDI_R || t[i] == BIDI_AL)
			lastStrong = t[i];
		else if(t[i] == BIDI_EN && lastStrong == BIDI_AL)
			t[i] = BIDI_AN;
	}
	// W3: from here on an Arabic letter is just a right-to-left letter
	for(i = 0; i < n; i++)
		if(t[i] == BIDI_AL)
			t[i] = BIDI_R;
	// W4: one separator between two numbers of the same kind belongs to the number
	for(i = 1; i + 1 < n; i++)
	{
		if(t[i] == BIDI_ES && t[i - 1] == BIDI_EN && t[i + 1] == BIDI_EN)
			t[i] = BIDI_EN;
		else if(t[i] == BIDI_CS && t[i - 1] == t[i + 1] && (t[i - 1] == BIDI_EN || t[i - 1] == BIDI_AN))
			t[i] = t[i - 1];
	}
	// W5: currency and percent signs next to a European number belong to it
	for(i = 0; i < n; i++)
	{
		if(t[i] != BIDI_ET)
			continue;
		for(j = i; j < n && t[j] == BIDI_ET; j++)
			;
		if((i > 0 && t[i - 1] == BIDI_EN) || (j < n && t[j] == BIDI_EN))
			for(; i < j; i++)
				t[i] = BIDI_EN;
		i = j - 1;
	}
	// W6: the separators and terminators that are left are ordinary punctuation
	for(i = 0; i < n; i++)
		if(t[i] == BIDI_ES || t[i] == BIDI_ET || t[i] == BIDI_CS)
			t[i] = BIDI_ON;
	// W7: European digits in left-to-right text are part of it
	lastStrong = embed;
	for(i = 0; i < n; i++)
	{
		if(t[i] == BIDI_L || t[i] == BIDI_R)
			lastStrong = t[i];
		else if(t[i] == BIDI_EN && lastStrong == BIDI_L)
			t[i] = BIDI_L;
	}
	// N1 / N2: a run of neutrals between text of one direction takes it, otherwise the paragraph's
	for(i = 0; i < n; i++)
	{
		int lead, trail;
		if(!IsNeutral(t[i]))
			continue;
		for(j = i; j < n && IsNeutral(t[j]); j++)
			;
		lead = StrongSide(i > 0 ? t[i - 1] : embed);
		trail = StrongSide(j < n ? t[j] : embed);
		for(; i < j; i++)
			t[i] = (uint8_t)(lead == trail ? lead : embed);
		i = j - 1;
	}
	// I1 / I2: text against the paragraph's direction goes one level up, numbers one or two
	for(i = 0; i < n; i++)
	{
		int lv = baseLevel & 1;
		if(lv == 0)
			lv += t[i] == BIDI_R ? 1 : (t[i] == BIDI_AN || t[i] == BIDI_EN) ? 2
																			: 0;
		else
			lv += (t[i] == BIDI_L || t[i] == BIDI_AN || t[i] == BIDI_EN) ? 1 : 0;
		levels[i] = (uint8_t)lv;
	}
	BGI_Free(t);
}

void Bidi_Reorder(const uint32_t* cp, const uint8_t* levels, int n, int baseLevel, int* order)
{
	uint8_t* lv;
	int i, j, top = 0, lowestOdd = 255, level;

	if(n <= 0)
		return;
	lv = (uint8_t*)BGI_Alloc((size_t)n);
	memcpy(lv, levels, (size_t)n);
	// L1: the white space at the end of the line follows the paragraph, not the text before it
	for(i = n - 1; i >= 0; i--)
	{
		BidiClass_t c = Bidi_ClassOf(cp[i]);
		if(c != BIDI_WS && c != BIDI_BN && c != BIDI_S && c != BIDI_B)
			break;
		lv[i] = (uint8_t)(baseLevel & 1);
	}
	for(i = 0; i < n; i++)
	{
		order[i] = i;
		if(lv[i] > top)
			top = lv[i];
		if((lv[i] & 1) && lv[i] < lowestOdd)
			lowestOdd = lv[i];
	}
	// L2: from the highest level down to the lowest odd one, reverse every run at that level or above
	for(level = top; level >= lowestOdd; level--)
	{
		for(i = 0; i < n; i++)
		{
			int a, b;
			if(lv[order[i]] < level)
				continue;
			for(j = i; j < n && lv[order[j]] >= level; j++)
				;
			for(a = i, b = j - 1; a < b; a++, b--)
			{
				int tmp = order[a];
				order[a] = order[b];
				order[b] = tmp;
			}
			i = j - 1;
		}
	}
	BGI_Free(lv);
}

uint32_t Bidi_Mirror(uint32_t cp)
{
	// pairs of characters that are each other's mirror image
	static const uint16_t kPairs[][2] = {
		{'(', ')'},
		{'[', ']'},
		{'{', '}'},
		{'<', '>'},
		{0x00ab, 0x00bb},
		{0x2039, 0x203a},
		{0x2045, 0x2046},
		{0x2264, 0x2265},
		{0x3008, 0x3009},
		{0x300a, 0x300b},
		{0x300c, 0x300d},
		{0x300e, 0x300f},
		{0x3010, 0x3011},
		{0x3014, 0x3015},
		{0x3016, 0x3017},
		{0x3018, 0x3019},
		{0x301a, 0x301b},
		{0xff08, 0xff09},
		{0xff1c, 0xff1e},
		{0xff3b, 0xff3d},
		{0xff5b, 0xff5d},
		{0xff5f, 0xff60},
		{0xff62, 0xff63},
	};
	size_t i;
	for(i = 0; i < BGI_COUNTOF(kPairs); i++)
	{
		if(cp == kPairs[i][0])
			return kPairs[i][1];
		if(cp == kPairs[i][1])
			return kPairs[i][0];
	}
	return cp;
}

// ---- Arabic joining ----------------------------------------------------------------------------

enum // how a letter connects to its neighbours
{
	JOIN_NONE,  // not at all (hamza, digits, punctuation, other scripts)
	JOIN_RIGHT, // only to the letter before it (alef, dal, reh, waw ..): two forms
	JOIN_DUAL,  // to both (beh, seen, lam ..): four forms
	JOIN_CAUSE, // joins on both sides and has one shape (the tatweel, the zero-width joiner)
	JOIN_SKIP   // a mark: the letters on either side join across it
};

typedef struct ArabicForm // a letter and its presentation forms
{
	uint16_t letter;
	uint16_t isolated; // the first of its forms: isolated, final and, for a dual-joining letter, initial, medial
	uint8_t join;      // JOIN_RIGHT or JOIN_DUAL
} ArabicForm_t;

/* the Arabic letters with presentation forms: the basic alphabet (forms
 * in U+FE80 .. U+FEF4) and the additional letters of Persian, Urdu and
 * their neighbours (forms in U+FB50 .. U+FBFF) */
static const ArabicForm_t kForms[] = {
	{0x0622, 0xfe81, JOIN_RIGHT},
	{0x0623, 0xfe83, JOIN_RIGHT},
	{0x0624, 0xfe85, JOIN_RIGHT},
	{0x0625, 0xfe87, JOIN_RIGHT},
	{0x0626, 0xfe89, JOIN_DUAL},
	{0x0627, 0xfe8d, JOIN_RIGHT},
	{0x0628, 0xfe8f, JOIN_DUAL},
	{0x0629, 0xfe93, JOIN_RIGHT},
	{0x062a, 0xfe95, JOIN_DUAL},
	{0x062b, 0xfe99, JOIN_DUAL},
	{0x062c, 0xfe9d, JOIN_DUAL},
	{0x062d, 0xfea1, JOIN_DUAL},
	{0x062e, 0xfea5, JOIN_DUAL},
	{0x062f, 0xfea9, JOIN_RIGHT},
	{0x0630, 0xfeab, JOIN_RIGHT},
	{0x0631, 0xfead, JOIN_RIGHT},
	{0x0632, 0xfeaf, JOIN_RIGHT},
	{0x0633, 0xfeb1, JOIN_DUAL},
	{0x0634, 0xfeb5, JOIN_DUAL},
	{0x0635, 0xfeb9, JOIN_DUAL},
	{0x0636, 0xfebd, JOIN_DUAL},
	{0x0637, 0xfec1, JOIN_DUAL},
	{0x0638, 0xfec5, JOIN_DUAL},
	{0x0639, 0xfec9, JOIN_DUAL},
	{0x063a, 0xfecd, JOIN_DUAL},
	{0x0641, 0xfed1, JOIN_DUAL},
	{0x0642, 0xfed5, JOIN_DUAL},
	{0x0643, 0xfed9, JOIN_DUAL},
	{0x0644, 0xfedd, JOIN_DUAL},
	{0x0645, 0xfee1, JOIN_DUAL},
	{0x0646, 0xfee5, JOIN_DUAL},
	{0x0647, 0xfee9, JOIN_DUAL},
	{0x0648, 0xfeed, JOIN_RIGHT},
	{0x0649, 0xfeef, JOIN_RIGHT},
	{0x064a, 0xfef1, JOIN_DUAL},
	{0x0671, 0xfb50, JOIN_RIGHT},
	{0x0679, 0xfb66, JOIN_DUAL},
	{0x067e, 0xfb56, JOIN_DUAL},
	{0x0686, 0xfb7a, JOIN_DUAL},
	{0x0688, 0xfb88, JOIN_RIGHT},
	{0x0691, 0xfb8c, JOIN_RIGHT},
	{0x0698, 0xfb8a, JOIN_RIGHT},
	{0x06a4, 0xfb6a, JOIN_DUAL},
	{0x06a9, 0xfb8e, JOIN_DUAL},
	{0x06af, 0xfb92, JOIN_DUAL},
	{0x06ba, 0xfb9e, JOIN_RIGHT},
	{0x06be, 0xfbaa, JOIN_DUAL},
	{0x06c0, 0xfba4, JOIN_RIGHT},
	{0x06c1, 0xfba6, JOIN_DUAL},
	{0x06cc, 0xfbfc, JOIN_DUAL},
	{0x06d2, 0xfbae, JOIN_RIGHT},
	{0x06d3, 0xfbb0, JOIN_RIGHT},
};

// the table entry of a letter, NULL when it has no presentation forms
static const ArabicForm_t* FormOf(uint32_t cp)
{
	size_t i;
	if(cp < 0x0622 || cp > 0x06d3)
		return NULL;
	for(i = 0; i < BGI_COUNTOF(kForms); i++)
		if(kForms[i].letter == cp)
			return &kForms[i];
	return NULL;
}

static int JoinOf(uint32_t cp)
{
	const ArabicForm_t* f = FormOf(cp);
	if(f)
		return f->join;
	if(cp == 0x0640 || cp == 0x200d)
		return JOIN_CAUSE;
	if(Bidi_ClassOf(cp) == BIDI_NSM)
		return JOIN_SKIP;
	return JOIN_NONE;
}

// the lam-alef ligature (isolated form) for an alef after a lam, 0 for any other letter
static uint32_t LamAlef(uint32_t alef)
{
	switch(alef)
	{
		case 0x0622: return 0xfef5;
		case 0x0623: return 0xfef7;
		case 0x0625: return 0xfef9;
		case 0x0627: return 0xfefb;
		default: return 0;
	}
}

void Arabic_Shape(const uint32_t* cp, int n, uint32_t* out)
{
	int i, prev = -1; // prev: the last character before i that is not a mark, -1 for none

	for(i = 0; i < n; i++)
		out[i] = cp[i];
	for(i = 0; i < n; i++)
	{
		const ArabicForm_t* f = FormOf(cp[i]);
		int join = JoinOf(cp[i]);
		int next, joinsPrev, joinsNext, prevJoin, nextJoin;

		if(join == JOIN_SKIP)
			continue;
		if(out[i] == 0)
		{ // an alef already taken into a ligature: it ends the joining like the alef it was
			prev = i;
			continue;
		}
		for(next = i + 1; next < n && JoinOf(cp[next]) == JOIN_SKIP; next++)
			;
		// a letter joins the one before it when that one reaches forward (and was not swallowed by a ligature)
		prevJoin = prev >= 0 && out[prev] != 0 ? JoinOf(cp[prev]) : JOIN_NONE;
		nextJoin = next < n ? JoinOf(cp[next]) : JOIN_NONE;
		joinsPrev = (join == JOIN_RIGHT || join == JOIN_DUAL) && (prevJoin == JOIN_DUAL || prevJoin == JOIN_CAUSE);
		joinsNext = join == JOIN_DUAL && (nextJoin == JOIN_RIGHT || nextJoin == JOIN_DUAL || nextJoin == JOIN_CAUSE);
		if(cp[i] == 0x0644 && next < n && LamAlef(cp[next]))
		{ // lam + alef: one glyph, which only joins backwards
			out[i] = LamAlef(cp[next]) + (joinsPrev ? 1 : 0);
			out[next] = 0;
		}
		else if(f)
		{
			if(f->join == JOIN_DUAL)
				out[i] = f->isolated + (joinsPrev && joinsNext ? 3 : joinsNext ? 2
											   : joinsPrev                     ? 1
																			   : 0);
			else
				out[i] = f->isolated + (joinsPrev ? 1 : 0);
		}
		prev = i;
	}
}
