/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * embfonts.h - the fonts built into the binary; not part of the original
 *              engine
 *
 * The build compiles TrueType or OpenType fonts into the engine - the
 * four of fonts/ unless `make EMBED_FONTS="gothic=FILE mincho=FILE ..."`
 * names others (or none): tools/mkembfonts.py writes the table below into
 * the object directory (gen/embfonts.c, empty without fonts).  The engine
 * hands them to the system at its first font and opens one of them in
 * place of a face the game asks for that is not installed
 * (FontNames_Resolve, src/gfx/bmptext.c).  A font has one of four
 * classes: the serif ones (Mincho) stand in for a face whose name says
 * so, the sans ones (gothic) for the others; the proportional ones for a
 * face whose name says so ("ＭＳ Ｐゴシック", "MS UI Gothic"), the
 * fixed-pitch ones for the others.  A class without a font is served by
 * the same design at the other pitch, then by the other design.
 */
#ifndef BGI_EMBFONTS_H_
#define BGI_EMBFONTS_H_

#include "bgi/common.h"

// the classes: bit 0 the serif design (Mincho), bit 1 the proportional pitch
#define EMBFONT_GOTHIC  0
#define EMBFONT_MINCHO  1
#define EMBFONT_PGOTHIC 2
#define EMBFONT_PMINCHO 3
#define EMBFONT_CLASSES 4

typedef struct EmbeddedFont
{
	const char* file;    // the file it was made from, its name only (for the messages)
	int cls;             // EMBFONT_GOTHIC .. EMBFONT_PMINCHO
	const uint8_t* data; // the font file's bytes
	uint32_t size;
} EmbeddedFont_t;

extern const EmbeddedFont_t gEmbeddedFonts[]; // in the order EMBED_FONTS names them
extern const int gEmbeddedFontCount;

#endif // BGI_EMBFONTS_H_
