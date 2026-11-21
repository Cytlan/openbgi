/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ftfont.h - glyphs with GDI's sizing, rasterised by FreeType
 *            (src/core/ftfont.c); not part of the original engine
 *
 * The engine draws its text with this when its host has no renderer of
 * its own (gfx/typeface.c), and the X11 host draws the text of its own
 * windows with it.  A library (FtFonts_t) is one FreeType instance: the
 * faces it has open, the fonts it was handed as data, and the host's
 * services that find font files (OpenBGI_FontFiles_t, openbgi.h).
 *
 * A font is opened as CreateFontA would open it: a face name, the cell
 * height (ascent and descent) and the width of a half-width character, a
 * full-width one twice as wide, and each character drawn at the top-left
 * corner of a buffer as TextOut does - the cell's top at row 0, the
 * baseline at the font's ascent.  The face name is resolved to a file in
 * this order: a file the host forces for it, a font handed over as data
 * that has the family, the host's best match.  A character the face has no
 * glyph for is drawn from another font the host finds (findChar), scaled
 * to the same cell; one that no font has becomes a box, as with GDI.
 *
 * Without FreeType (a build without BGI_HAVE_FREETYPE) FtFonts_New returns
 * NULL and nothing else can be reached.
 */
#ifndef BGI_FTFONT_H_
#define BGI_FTFONT_H_

#include "openbgi.h"

typedef struct FtFonts FtFonts_t;
typedef struct FtFont FtFont_t;

// a rendered glyph, valid until the next call on its font
typedef struct FtGlyph
{
	const uint8_t* bits; // the bitmap: 8-bit coverage, or 1 bit a pixel (most significant first) when `mono`
	int pitch;           // bytes a row
	int width, rows;     // its size in pixels
	int x, y;            // its top left pixel relative to the pen and to the cell's top
	int advance;         // how far the pen moves, 26.6 fixed point
	int mono;
} FtGlyph_t;

/* a library that finds font files through `files` (may be NULL), passing
 * `user`, and reads them through the host's files of the engine `o` (NULL:
 * FreeType opens them, for a back end's own library); NULL when FreeType
 * cannot start */
FtFonts_t* FtFonts_New(OpenBGI_t* o, const OpenBGI_FontFiles_t* files, void* user);
// close the fonts still open and free the library; accepts NULL
void FtFonts_Delete(FtFonts_t* l);
/* A font file's data (each face of a collection), found by its family
 * names from then on and before the host's files; with `copy` 0 the data
 * must stay as it is for as long as the library lives.  1 when it names a
 * family. */
int FtFonts_AddMemory(FtFonts_t* l, const void* data, uint32_t size, int copy);
// 1 when a font handed over as data has the family (UTF-8, ASCII letters in either case)
int FtFonts_HasFamily(const FtFonts_t* l, const char* family);

/* Open a font as CreateFontA would (above): the face a UTF-8 family name,
 * bold and italic made up when the face has no such style of its own.
 * NULL for a height of 0, or when no file can be found or loaded. */
FtFont_t* FtFont_Open(FtFonts_t* l, const char* face, int height, int width, int bold, int italic);
void FtFont_Close(FtFont_t* f); // accepts NULL
int FtFont_Height(const FtFont_t* f);
/* The glyph of code point `cp`, antialiased or in black and white, from
 * the font's face or the one standing in for it; 1 ok, 0 when it cannot be
 * loaded. */
int FtFont_Glyph(FtFont_t* f, uint32_t cp, int antialiased, FtGlyph_t* out);
/* Clear the w x h buffer (pitch bytes a row) and draw `cp` in black and
 * white (0 / 255) with its cell's top left at the buffer's, clipped. */
void FtFont_Render(FtFont_t* f, uint32_t cp, uint8_t* buf, int pitch, int w, int h);
// the same into a 1-bit bitmap, most significant bit first: a set bit is ink
void FtFont_RenderMono(FtFont_t* f, uint32_t cp, uint8_t* bits, int pitch, int w, int h);
// the advance of the glyph FtFont_Render draws, in whole pixels; 0 when it cannot be loaded
int FtFont_Advance(FtFont_t* f, uint32_t cp);
// the advance of `cp` in the font's own face alone (no stand-in), 26.6 fixed point; -1 when it cannot be loaded
int FtFont_FaceAdvance(FtFont_t* f, uint32_t cp);

#endif // BGI_FTFONT_H_
