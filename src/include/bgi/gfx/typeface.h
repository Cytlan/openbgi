/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * typeface.h - the fonts the engine draws with (src/gfx/typeface.c): the
 *              host's renderer, or the engine's own (bgi/ftfont.h)
 *
 * A typeface is what CreateFontA gives the original: a face name, a cell
 * height and the width of a half-width character (openbgi.h, "The
 * fonts").  When the host brings a renderer (OpenBGI_FontRenderer_t) every
 * call goes to it; otherwise the engine rasterises with FreeType from the
 * fonts handed to it as data and the files its host finds.  Either way the
 * engine keeps the families of the fonts it was handed as data, since a
 * system that takes a font from memory opens it by name without listing
 * it.  Names and characters are in the scripts' text encoding (Shift-JIS,
 * or UTF-8 after "81 00"), paths as the engine spells them.
 */
#ifndef BGI_GFX_TYPEFACE_H_
#define BGI_GFX_TYPEFACE_H_

#include "bgi/common.h"

typedef struct Typeface Typeface_t;

// the glyph rasteriser's font (CreateFontA with PROOF_QUALITY): NULL when no font can be had
Typeface_t* Typeface_Open(OpenBGI_t* o, const char* face, int height, int width, int bold, int italic);
// the GDI text instruction's font (DRAFT_QUALITY, size x size / 2)
Typeface_t* Typeface_OpenMono(OpenBGI_t* o, const char* face, int size, int bold);
void Typeface_Close(Typeface_t* t); // accepts NULL
/* Clear the w x h buffer (pitch bytes a row) and draw the character (its
 * `len` bytes) with its cell's top left at the buffer's: 0 blank, 255 ink. */
void Typeface_Render(Typeface_t* t, const char* ch, int len, uint8_t* buf, int pitch, int w, int h);
// the same into a 1-bit bitmap, most significant bit first: a set bit is ink
void Typeface_RenderMono(Typeface_t* t, const char* ch, int len, uint8_t* bits, int pitch, int w, int h);
// the character's advance in pixels
int Typeface_Advance(Typeface_t* t, const char* ch, int len);

/* Smooth text for the engine's own user interface (the dialogs and the
 * text entry it draws in the frame): the face opened TYPEFACE_SMOOTH
 * times larger, every character rendered at that size at its
 * quarter-pixel position and filtered down, so that it comes out
 * antialiased from any renderer, the host's black-and-white one included.
 * Typeface_OpenSmooth opens it for a cell of `height` and a half-width
 * character of `width` pixels (CreateFontA with FW_THIN); the others work
 * on such a typeface only.  Typeface_SmoothText puts `text` with its
 * cell's top left at (x, y) into a 32-bit buffer (pitch in pixels, w x h)
 * in `rgb`: blended over 0x00RRGGBB pixels, or composited over 0xAARRGGBB
 * ones when `alpha` (the coverage becomes the alpha of a clear pixel); it
 * returns the pen position after the text.  Typeface_SmoothWidth is the
 * width of its first `bytes` bytes (all when < 0). */
#define TYPEFACE_SMOOTH 4
Typeface_t* Typeface_OpenSmooth(OpenBGI_t* o, const char* face, int height, int width);
int Typeface_SmoothText(Typeface_t* t, uint32_t* pix, int pitch, int w, int h, int x, int y, const char* text, uint32_t rgb, int alpha);
int Typeface_SmoothWidth(Typeface_t* t, const char* text, int bytes);

// "B0 C2": a font file (a path of the game) for the faces opened from then on, listed by Typefaces_Enum; 1 ok
int Typefaces_AddFile(OpenBGI_t* o, const char* path);
/* "B0 C3" and the fonts built into the binary: a font's data, opened by
 * its families from then on and not listed; `keep` 1 when the data stays
 * as it is for the engine's life (it is then not copied).  1 ok. */
int Typefaces_AddMemory(OpenBGI_t* o, const void* data, uint32_t size, int keep);
/* A font the engine opens in the place of the MS fonts (built into the
 * binary, or the player's): as Typefaces_AddMemory with `keep`, and
 * drawn at its own proportions by a renderer that would not otherwise
 * (OpenBGI_FontRenderer_t.addSubstitute).  1 ok. */
int Typefaces_AddSubstitute(OpenBGI_t* o, const void* data, uint32_t size);
// 1 when a face of exactly that name is installed ("B0 C4", "B0 C6"), or when that cannot be told
int Typefaces_Exists(OpenBGI_t* o, const char* face);
// 1 when Typeface_Open would open the face itself rather than another
int Typefaces_Installed(OpenBGI_t* o, const char* face);
/* "B0 C4" / "B0 C5": the faces of a Windows charset (0x80 Shift-JIS, 0
 * ANSI, 0x81 Hangul, 0x86 GB2312, 0x88 Big5), NUL-terminated one after the
 * other into `buf` when it is not NULL, the bytes they take into
 * *outBytes; the number of faces */
int Typefaces_Enum(OpenBGI_t* o, char* buf, int charset, int* outBytes);
// OpenBGI_Delete: close what is still open and forget the fonts
void Typefaces_Free(OpenBGI_t* o);

#endif // BGI_GFX_TYPEFACE_H_
