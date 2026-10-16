/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bidi.h - bidirectional text and Arabic letter joining (src/core/bidi.c)
 *
 * Not part of any original build: this is what the right-to-left text
 * layout of the OpenBGI extensions ("EF 01", docs/obgi.md) is built on.
 * The functions work on arrays of Unicode code points in logical (reading)
 * order and know nothing about fonts or pixels.
 *
 *   * Bidi_Levels resolves the embedding level of every character of one
 *     paragraph: even levels read left to right, odd levels right to left.
 *     It is the Unicode bidirectional algorithm (UAX #9) without the
 *     explicit embedding controls and without the bracket-pair rule.
 *   * Bidi_Reorder turns the levels of one line into the visual order.
 *   * Arabic_Shape replaces Arabic letters by their presentation forms
 *     (isolated, final, initial, medial; the lam-alef ligatures), which is
 *     all the "shaping" a font needs that has those forms as glyphs.
 */
#ifndef BGI_BIDI_H_
#define BGI_BIDI_H_

#include "bgi/common.h"

typedef enum BidiClass // the bidirectional character types of UAX #9 that the resolution uses
{
	BIDI_L,   // left-to-right letter
	BIDI_R,   // right-to-left letter (Hebrew)
	BIDI_AL,  // right-to-left Arabic letter
	BIDI_EN,  // European digit
	BIDI_ES,  // plus and minus
	BIDI_ET,  // currency and percent signs
	BIDI_AN,  // Arabic-Indic digit
	BIDI_CS,  // comma, full stop, colon, slash between digits
	BIDI_NSM, // combining mark
	BIDI_BN,  // invisible formatting character
	BIDI_B,   // paragraph separator
	BIDI_S,   // tab
	BIDI_WS,  // space
	BIDI_ON   // other punctuation and symbols
} BidiClass_t;

BidiClass_t Bidi_ClassOf(uint32_t cp);

/* 1 for the characters of the right-to-left scripts (Hebrew, Arabic and
 * their neighbours, with their marks, digits and presentation forms): the
 * ones a layout has to place by the font's own advance, because they join */
int Bidi_IsRtlScript(uint32_t cp);

/* the embedding levels of the `n` characters of one paragraph whose base
 * level is `baseLevel` (0 left to right, 1 right to left) */
void Bidi_Levels(const uint32_t* cp, int n, int baseLevel, uint8_t* levels);

/* the visual order of one line: order[v] is the index of the character
 * shown at position v, counted from the left.  `levels` are the line's
 * levels (Bidi_Levels, cut to the line); the white space at the end of
 * the line is put at the base level first, as the algorithm asks. */
void Bidi_Reorder(const uint32_t* cp, const uint8_t* levels, int n, int baseLevel, int* order);

// the mirror image of a bracket-like character, for text that reads right to left; `cp` itself when it has none
uint32_t Bidi_Mirror(uint32_t cp);

/* the presentation forms of the `n` characters of one paragraph: out[i]
 * is what to draw for cp[i] - the joined form of an Arabic letter, the
 * character itself when it has none, or 0 for an alef that the lam before
 * it took into a lam-alef ligature */
void Arabic_Shape(const uint32_t* cp, int n, uint32_t* out);

#endif // BGI_BIDI_H_
