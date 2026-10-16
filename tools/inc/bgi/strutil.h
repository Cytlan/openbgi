/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * strutil.h - Shift-JIS aware string helpers, and the script text encoding
 *             (src/core/strutil.c)
 *
 * Up to 1.640 every script string is Shift-JIS (code page 932).  1.653
 * added "81 00", which switches the engine to UTF-8 (code page 65001;
 * arcana and haruyome boot with it), and from then on the original keeps
 * one "next character" decoder with a mode that the text renderer, the
 * window and the string instructions all go through.  The Text* functions
 * here are that decoder: a character code is the Shift-JIS code in
 * Shift-JIS mode and the Unicode scalar in UTF-8 mode, and the tables of
 * punctuation the text code keeps as Shift-JIS literals are read through
 * TextTableChar, which converts them to the current code space.
 */
#ifndef BGI_STRUTIL_H_
#define BGI_STRUTIL_H_

#include "bgi/common.h"

int SjisIsLead(uint8_t c);                                                    // 1 for the first byte of a double-byte character (0x80..0x9F, 0xE0..0xFF)
int SjisGetChar(uint32_t* out, const char* s);                                // the Shift-JIS code at s (lead << 8 | trail); 1 when double byte
int IsKinsokuChar(uint32_t ch);                                               // line-head prohibited (kinsoku) punctuation, in the current encoding
void SjisStrLwr(char* s);                                                     // ASCII upper case to lower case in place, multi-byte characters left alone
int StrFindSjis(const char* s, const char* pat);                              // byte offset of `pat` in `s`, or -1 (character-wise; see strutil.c for the quirk)
int StrReplace(char* dst, const char* src, const char* from, const char* to); // "67": copy with every `from` replaced; the count

// _splitpath ("80 2B" of 1.69/472 on); any destination may be NULL
void SplitPath(const char* path, char* drive, char* dir, char* fname, char* ext);

// ---- the text encoding ("81 00" of 1.653 on) ------------------------------------------

#define TEXT_SJIS 0 // code page 932
#define TEXT_UTF8 1 // code page 65001

extern int gTextUtf8; // the mode: 1 in UTF-8 mode

void Text_SetEncoding(int mode); // TEXT_SJIS / TEXT_UTF8 (also told to the OS layer)

/* the character at `s`: its byte length (1 or 2 in Shift-JIS, 1 .. 4 in
 * UTF-8; a surrogate pair written as two 3-byte sequences counts as one
 * character of 6) and its code.  A NUL is a character of length 1, code 0. */
int TextGetChar(uint32_t* out, const char* s);
int TextCharLen(const char* s);
#define TEXT_CHAR_MAX 8 // a buffer for one character and its terminator

/* does the character occupy a full-width cell: a double-byte character in
 * Shift-JIS; anything but ASCII and the half-width katakana U+FF61 ..
 * U+FF9F in UTF-8 */
int TextIsWide(uint32_t code, int len);

// the bytes of a code in the current encoding (no terminator); the count, 1 .. 4
int TextPutChar(char* dst, uint32_t code);

/* the next character of a Shift-JIS literal table (the punctuation sets of
 * the text code), as a code of the current code space; `*p` advances */
uint32_t TextTableChar(const char** p);

// 1 when `code` (a TextGetChar code) is in the Shift-JIS literal set
int TextCodeInSet(uint32_t code, const char* sjisSet);

/* 1.653 on: which decoder a string wants, judged from its bytes:
 * TEXT_UTF8 when it is well-formed UTF-8 with at least one multi-byte
 * sequence, TEXT_RAW when it is plain ASCII (control bytes included),
 * TEXT_SJIS otherwise.  "6C" sniffs each string this way, "81 20" uses it
 * to skip a conversion the text does not need. */
#define TEXT_RAW 0x80000000u
uint32_t TextSniff(const char* s);
int TextGetCharMode(uint32_t* out, int* wide, const char* s, uint32_t mode); // the decoder with an explicit mode (TEXT_SJIS / TEXT_UTF8 / TEXT_RAW: one byte); *wide as TextIsWide

#endif
