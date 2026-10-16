/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * layout_internal.h - shared between the files of the rich text layout
 *                     (layout.c, layout_common.c, markup.c, layout_h.c,
 *                     layout_v.c); the public entry points are declared in
 *                     inc/bgi/gfx/text.h
 *
 * A layout produces a list of RichChar nodes hanging off RichState.head,
 * one per glyph, each with a ready-made bitmap (glyph plus optional
 * shadow or outline) and a position inside the text layer.  Ruby readings
 * are laid out afterwards (Text_RubyPlace*) and spliced in behind their
 * base character; the alignment pass (Text_Align*) shifts whole lines for
 * centred or right-aligned ("swing") text.  The horizontal and the
 * vertical variants mirror each other with x / y swapped and a few extra
 * rules for vertical writing (rotated brackets, shifted small kana).
 *
 * Measures used throughout:
 *   size      the font's pixel size (also the line height without spacing)
 *   glyphW    size * widthPct / 100: the advance of a full-width glyph
 *   gap       proportional slack after a glyph (Font_GapFor2)
 *   pitch     gCharPitch, extra pixels after every fixed-width glyph
 *   sdx, sdy  shadow offset in pixels (style[1] / style[2] percent of size)
 *   hangSize  the width reserved on the right for overhanging punctuation
 */
#ifndef BGI_GFX_TEXT_LAYOUT_INTERNAL_H
#define BGI_GFX_TEXT_LAYOUT_INTERNAL_H

#include "text_internal.h"
#include "bgi/gfx/bmpops.h"

// ---- layout_common.c: glyph bitmaps and measures ---------------------------------------

// The opening brackets a leading control byte 4 .. 8 of the text stands for.
extern const char* const gLeadingBracket[5];

int Layout_Pct(int v, int pct); // v * pct / 100, rounded towards zero
int Layout_Min1(int v);         // shadow offsets are never less than one pixel

// A fresh RichChar node with the reveal delay `delay` and the ruby fade default.
RichChar_t* Layout_NewNode(int delay);

/* Paint the glyph `g` (code `code`, rasterised by `font`) into a new w x h
 * bitmap `dst` with its shadow or outline per `style`, offset by (sdx,
 * sdy); `view` is the painted cell to copy from and `rotate` turns the
 * glyph's recoloured shadow for vertical writing. */
void Layout_GlyphBitmap(Bmp_t* dst, int w, int h, const Bmp_t* view, const Glyph_t* g, int code,
	FontRaster_t* font, const int32_t style[5], int sdx, int sdy, int rotate);

// The pixels a style's shadow adds to a glyph bitmap in one direction (twice for the outline style).
int Layout_ShadowReserve(const int32_t style[5], int sd);

// The pixels added after a fixed-pitch glyph of `fi`; 0 for proportional text.
int Layout_Pitch(int proportional, const FontInfo_t* fi);

// ---- layout_h.c: the line records of the alignment pass ---------------------------------

typedef struct LineRec // one laid-out line (column) for the alignment pass
{
	int32_t start;  // x of the first glyph (y in vertical text)
	int32_t extent; // right edge (bottom)
	int32_t key;    // the y (x) all glyphs of the line share
	struct LineRec* next;
} LineRec_t;

void Layout_FreeLineRecs(LineRec_t* r);

// ---- markup.c: the inline markup of 1.494 on ------------------------------------------

typedef struct TagDef
{
	const char* name; // the tag text, lower case, without the brackets
	EngineGen_t from; // the generation that introduced the tag
} TagDef_t;

enum
{
	TAG_LITERAL, // "</>": the next '<' is text
	TAG_BOLD,    // <b> .. </b>
	TAG_BOLD_END,
	TAG_ITALIC, // <i> .. </i>
	TAG_ITALIC_END,
	TAG_RUBY, // <ruby word,reading>: a dictionary entry
	TAG_R,    // <r reading>word</r>
	TAG_R_END,
	TAG_CR,     // <cr>: back to the left edge
	TAG_COLOUR, // <c rrggbb> .. </c>
	TAG_COLOUR_END,
	TAG_LINK, // <l> .. </l>: underlined, in the link colour / font, registered
	TAG_LINK_END,
	TAG_TIME, // <t n>: the reveal delay restarts at n characters' worth
	TAG_COUNT
};

// The tags in the order they are tried; a tag matches exactly or as a prefix ("<r まお>").
extern const TagDef_t gMarkupTags[TAG_COUNT];

/* The state of the markup inside one layout: the font in use (the window's,
 * or a temporary rasteriser for <b> / <i> / <l>), the colour stack and the
 * open link. */
typedef struct Markup
{
	int on;                 // the generation has the markup at all
	const FontInfo_t* base; // the window's font
	FontInfo_t cur;         // copy of it; .font is the temporary rasteriser or NULL
	const FontInfo_t* fi;   // base or &cur: what glyphs are drawn and measured with
	uint32_t colour;        // the current text colour
	uint32_t* stack;        // pushed colours of <c> and <l>
	int stackCount, stackCap;
	const char* linkStart; // text after the <l> tag, NULL when no link is open
	const char* linkEnd;   // where a line break cut the link text, or NULL
	int linkX, linkY;      // the cursor where the link began
	int literal;           // "</>" seen
	int delay;             // the reveal delay of the next glyph ("<t>" restarts it)
} Markup_t;

// Release the colour stack and the temporary font of a markup state.
void Markup_Free(Markup_t* m);

/* Copy the `len` bytes of tag text between the brackets into buf
 * (lower-cased) and find the tag; its index or -1. */
int Markup_Match(char* buf, size_t cap, const char* text, int len);

/* Apply tag `tag` with its argument text `args`; `after` is the text behind
 * the closing bracket, `p` the '<'.  Always 1: the tag was consumed and the
 * layout continues behind it. */
int Markup_Apply(Markup_t* m, int tag, const char* args, const char* after, const char* p, RubyDict_t* dict,
	int32_t cur[2], const Rect_t* area);

/* The no-break run starting at `p` (a Latin word) copied to out; the
 * byte count, 0 when `p` does not start one. */
int Markup_WordRun(char* out, const char* p);

#endif
