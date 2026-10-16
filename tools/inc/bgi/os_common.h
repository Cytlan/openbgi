/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_common.h - helpers shared by the OS back ends (src/os/os_common.c):
 *               the platform-independent parts of the services of os.h,
 *               which each back end calls from its own implementation.
 *               Nothing above the OS layer includes this.
 */
#ifndef BGI_OS_COMMON_H_
#define BGI_OS_COMMON_H_

#include "bgi/common.h"

int OsCommon_SjisIsLead(uint8_t c);                                // 1 for the first byte of a two-byte Shift-JIS character
int OsCommon_WildcardMatch(const char* pattern, const char* name); // FindFirstFile style ('*', '?'), case-insensitive
const char* OsCommon_FilePart(const char* path);                   // after the last '\\' or '/' (Shift-JIS aware)
/* the main window's title as shown: the game's title (Shift-JIS) with
 * " - OpenBGI" appended, "OpenBGI" alone for an empty one */
void OsCommon_MainTitle(char* out, size_t n, const char* sjisTitle);

/* the text file behind the POSIX registry emulation ("key\value=data"
 * lines, keys compared without regard to case); the back end names the
 * file and passes the key as the engine spells it ("HKLM\...") */
int OsCommon_RegFileRead(const char* file, const char* key, const char* value, char* buf, size_t n); // 1 = found
int OsCommon_RegFileWrite(const char* file, const char* key, const char* value, const char* data);   // data NULL removes; 1 = written
int OsCommon_RegFileDeleteKey(const char* file, const char* key);                                    // 1 = something was removed

/* Shift-JIS to UTF-8 with a back-end supplied lookup for the two-byte
 * codes (the CP932 table; 0 = unmapped, drawn as U+FFFD); the length
 * written.  In UTF-8 text mode a well-formed sequence passes through. */
typedef uint32_t (*OsCommon_SjisLookup_t)(uint16_t sjis);
size_t OsCommon_SjisToUtf8(const char* sjis, char* out, size_t n, OsCommon_SjisLookup_t lookup);

// the text encoding of OS_SetTextEncoding (1 = UTF-8), and the character decoding the font back ends share
extern int gOsTextUtf8;
int OsCommon_Utf8SeqLen(const uint8_t* s);                                         // the length of a well-formed UTF-8 sequence at s, else 0
uint32_t OsCommon_Utf8Decode(const uint8_t* u, int* outLen);                       // one sequence to its code point; *outLen = bytes taken
uint32_t OsCommon_CodePoint(const char* s, int len, OsCommon_SjisLookup_t lookup); // the code point of a `len`-byte character in the text encoding

#endif // BGI_OS_COMMON_H_
