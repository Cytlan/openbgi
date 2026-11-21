/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_posix.h - helpers shared by the files of the POSIX back end
 *              (src/host/posix/, *.c): the path conversion (paths.c),
 *              the configuration directory, the close
 *              request of the signal handler, and the antialiased text
 *              the back end's own windows draw with (font.c).  Nothing
 *              above the OS layer includes this.
 */
#ifndef BGI_OS_POSIX_H_
#define BGI_OS_POSIX_H_

#include "bgi/common.h"

// Shift-JIS Windows path -> UTF-8 POSIX path (drive letter = game directory, case-insensitive match of each component)
void OsPosix_Path(const char* win, char* out, size_t n);
// SIGTERM / SIGINT (process.c) -> the window's close request (window.c)
void OsPosix_RequestClose(void);

/* antialiased text for the back end's own windows (font.c, with the
 * engine's FreeType rasteriser of bgi/ftfont.h): open a face, draw into a
 * 32-bit 0x00RRGGBB buffer (pitch in pixels), measure, the UI font */
struct FtFont;
// a face (in the text encoding) as CreateFontA(height, width, .., FW_THIN, ..) would open it; NULL when there is none
struct FtFont* OsPosix_FontOpen(const char* face, int height, int width);
void OsPosix_FontClose(struct FtFont* f); // accepts NULL
// draw `sjis` with its cell top-left at (x, y), blended with `rgb`, clipped to w x h; the pen position after it
int OsPosix_FontDraw(struct FtFont* f, uint32_t* dst, int pitch, int w, int h, int x, int y, const char* sjis, uint32_t rgb);
int OsPosix_FontTextWidth(struct FtFont* f, const char* sjis, int bytes); // the advance in pixels of the first `bytes` bytes (< 0: all)
struct FtFont* OsPosix_FontUi(int height);                                // the gothic face at that cell height; OsPosix_FontClose() when done
struct UiFont;
// `f` as the dialog toolkit's font (bgi/ui.h): its context, height and the two functions above; a NULL font draws nothing
void OsPosix_UiFont(struct UiFont* out, struct FtFont* f);
void OsPosix_FontShutdown(void); // OS_Shutdown: release the windows' fonts and fontconfig

#endif // BGI_OS_POSIX_H_
