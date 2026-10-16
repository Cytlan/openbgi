/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_posix.h - helpers shared by the files of the POSIX back end
 *              (src/os/posix/, *.c): the path and character conversions
 *              (sjis.c, paths.c), the configuration directory, the close
 *              request of the signal handler, and the antialiased text
 *              the back end's own windows draw with (font.c).  Nothing
 *              above the OS layer includes this.
 */
#ifndef BGI_OS_POSIX_H_
#define BGI_OS_POSIX_H_

#include "bgi/common.h"

// Shift-JIS Windows path -> UTF-8 POSIX path (drive letter = game directory, case-insensitive match of each component)
void OsPosix_Path(const char* win, char* out, size_t n);
// Shift-JIS text -> UTF-8 (window titles, dialog labels, font lookups); NUL-terminated into out (n bytes)
void OsPosix_SjisToUtf8(const char* sjis, char* out, size_t n);
// one two-byte Shift-JIS code -> Unicode (0xfffd when unmapped)
uint32_t OsPosix_SjisChar(uint16_t code);
// one code point -> Shift-JIS bytes in out[]; the count (1 or 2), 0 when CP932 has no code for it
int OsPosix_UnicodeToSjis(uint32_t cp, char out[2]);
// ~/.config/bgi (created by OS_Init): the registry file and the instance lock files live there
const char* OsPosix_ConfigDir(void);
// SIGTERM / SIGINT (process.c) -> the window's close request (window.c)
void OsPosix_RequestClose(void);

/* antialiased text for the back end's own windows (font.c): draw into a
 * 32-bit 0x00RRGGBB buffer (pitch in pixels), measure, the UI font */
struct OsFont;
// draw `sjis` with its cell top-left at (x, y), blended with `rgb`, clipped to w x h; the pen position after it
int OsPosix_FontDraw(struct OsFont* f, uint32_t* dst, int pitch, int w, int h, int x, int y, const char* sjis, uint32_t rgb);
int OsPosix_FontTextWidth(struct OsFont* f, const char* sjis, int bytes); // the advance in pixels of the first `bytes` bytes (< 0: all)
int OsPosix_FontHeight(struct OsFont* f);                                 // the cell height in pixels
struct OsFont* OsPosix_FontUi(int height);                                // the gothic face at that cell height; OS_FontClose() when done
void OsPosix_FontShutdown(void);                                          // OS_Shutdown: release FreeType / fontconfig when no font is open

#endif // BGI_OS_POSIX_H_
