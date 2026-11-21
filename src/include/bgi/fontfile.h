/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fontfile.h - the family names of a font file (src/base/fontfile.c); not
 *              part of the original engine
 *
 * A TrueType or OpenType font, or a collection of them (.ttc), names its
 * families in the "name" table, often in several languages: a Japanese
 * font answers to its Japanese and its English name alike.  The engine
 * asks for faces by name, so a font it is handed as data - one built into
 * the binary (gfx/bmptext.c), one a game adds ("B0 C2", "B0 C3") - is
 * known by these names (gfx/typeface.c, ftfont.c).  The "OS/2" table says
 * which Windows code pages a face serves, which decides the charsets it
 * is listed under ("B0 C4", "B0 C5").
 */
#ifndef BGI_FONTFILE_H_
#define BGI_FONTFILE_H_

#include "bgi/common.h"

#define FONTFILE_NAME_MAX 128 // bytes of a family name, UTF-8 with its NUL

/* The faces in the font in `data`: 1 for a single font, the number a
 * collection holds; 0 when the data is no font. */
int FontFile_Faces(const uint8_t* data, size_t size);
/* The family names (name ID 1) of face `face` as UTF-8, each once, the
 * English one first: up to `max` of them into `out`.  The number found
 * (more than `max` when they did not all fit), 0 when there is no such
 * face or it names no family. */
int FontFile_Families(const uint8_t* data, size_t size, int face, char (*out)[FONTFILE_NAME_MAX], int max);
/* The code pages face `face` serves: ulCodePageRange1 of its "OS/2" table
 * (bit 0 Latin 1, 17 Japanese (932), 18 simplified Chinese (936), 19
 * Korean (949), 20 traditional Chinese (950), ...); 0 when it has none. */
uint32_t FontFile_CodePages(const uint8_t* data, size_t size, int face);

#endif // BGI_FONTFILE_H_
