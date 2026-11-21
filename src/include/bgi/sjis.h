/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sjis.h - Shift-JIS and Unicode (src/base/sjis.c)
 *
 * The engine's strings are Shift-JIS - code page 932, Windows' Shift-JIS
 * with the NEC and IBM extensions - and every platform needs them as
 * Unicode for file names, window titles and the font renderer.  The table
 * of the code page is built into the engine (src/base/cp932.inc), so the
 * conversions are the same everywhere and no platform needs iconv or a
 * system code page.
 *
 * Single bytes: ASCII stays as it is (0x5C is the backslash, which the
 * paths need, though JIS X 0201 calls it the yen sign) and 0xA1 .. 0xDF
 * are the half-width katakana U+FF61 .. U+FF9F.  Towards Shift-JIS a scalar
 * with several codes takes the one Windows takes, and a character without
 * a code becomes '?' (no "best fit" to a similar letter).
 */
#ifndef BGI_SJIS_H_
#define BGI_SJIS_H_

#include "bgi/common.h"

/* the Unicode scalar of a two-byte code (the lead byte in the high half);
 * U+FFFD when the code is no character */
uint32_t Sjis_CodePoint(uint16_t code);
/* the code of a scalar in `out`: the bytes written (1 or 2), 0 when code
 * page 932 has none */
int Sjis_FromCodePoint(uint32_t cp, char out[2]);
/* a Shift-JIS string as UTF-8 in `out` (n bytes, NUL-terminated, whole
 * characters only); with `utf8` (the scripts' UTF-8 text mode) a
 * well-formed UTF-8 sequence passes through as it is, the engine's own
 * Shift-JIS messages being converted around it.  A lone lead byte at the
 * end is U+FFFD.  The length written. */
size_t Sjis_ToUtf8(const char* sjis, char* out, size_t n, int utf8);
/* UTF-8 to Shift-JIS ("81 20" of 1.653 on): a character without a code
 * becomes '?', a malformed byte is taken as a scalar of its own; `out` (n
 * bytes) is NUL-terminated.  The length written. */
size_t Sjis_FromUtf8(const char* utf8, char* out, size_t n);

/* 1 for the first byte of a two-byte character of the code page (0x81 ..
 * 0x9f, 0xe0 .. 0xfc; the original's own text code takes 0x80 and 0xfd ..
 * 0xff too, SjisIsLead of bgi/strutil.h) */
int Sjis_IsLead(uint8_t c);
/* the length of a well-formed UTF-8 sequence at `s` (a lead byte with its
 * continuation bytes), else 0 */
int Utf8_SeqLen(const uint8_t* s);
/* one UTF-8 sequence to its scalar, the length taken from its lead byte
 * and not checked (Utf8_SeqLen first); *outLen = the bytes taken */
uint32_t Utf8_Decode(const uint8_t* u, int* outLen);

/* FindFirstFile's matching of a name to a pattern, as far as the engine
 * relies on it: '*' any run (none too), '?' one character, ASCII case
 * ignored, double-byte characters as units; "*.*" every name, with a dot
 * or without */
int Sjis_WildcardMatch(const char* pattern, const char* name);

#endif // BGI_SJIS_H_
