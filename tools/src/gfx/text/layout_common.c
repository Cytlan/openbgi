/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * layout_common.c - what both directions of the rich text layout share:
 *                   the glyph bitmaps (glyph plus shadow or outline), the
 *                   measures, and the leading-bracket table
 *                   (layout_internal.h)
 */
#include "layout_internal.h"

// the opening brackets a leading control byte 4 .. 8 stands for
const char* const gLeadingBracket[5] = {
	MSG_TXT_BRACKET_KAGI,  // 4 「
	MSG_TXT_BRACKET_SPACE, // 5 (ideographic space)
	MSG_TXT_BRACKET_PAREN, // 6 （
	MSG_TXT_BRACKET_QUOTE, // 7 “
	MSG_TXT_BRACKET_KAGI2, // 8 『
};

int Layout_Pct(int v, int pct) // v * pct / 100, rounded towards zero
{
	return v * pct / 100;
}

int Layout_Min1(int v) // shadow offsets are never less than one pixel
{
	return v > 0 ? v : 1;
}

// a fresh node: reveal delay `delay`, the fade-in of gRubyFadeSteps steps
RichChar_t* Layout_NewNode(int delay)
{
	RichChar_t* n = (RichChar_t*)BGI_Calloc(sizeof(RichChar_t));
	n->delay = delay;
	n->fadeSteps = gRubyFadeSteps;
	return n;
}

/* the "outlined" text of style 2 (1.494 on): the glyph dilated by sdx /
 * sdy in every direction - each pixel of `dst` is the sum of the coverage
 * of the glyph's pixels in the (2*sdx+1) x (2*sdy+1) box around it,
 * clamped, in `colour`.  `glyph` is the full painted cell; the glyph itself
 * is drawn (sdx, sdy) into `dst` afterwards, so the box is placed to centre
 * the outline on it. */
static void Layout_PaintOutline(Bmp_t* dst, const Bmp_t* glyph, int sdx, int sdy, uint32_t colour)
{
	int x, y, i, j;
	colour &= 0xffffffu;
	for(y = 0; y < dst->h; y++)
	{
		uint32_t* d = (uint32_t*)(dst->pixels + (size_t)y * dst->pitch);
		for(x = 0; x < dst->w; x++)
		{
			int sum = 0;
			for(j = y - 2 * sdy; j <= y; j++)
			{
				const uint32_t* row;
				if(j < 0 || j >= glyph->h)
					continue;
				row = (const uint32_t*)(glyph->pixels + (size_t)j * glyph->pitch);
				for(i = x - 2 * sdx; i <= x; i++)
					if(i >= 0 && i < glyph->w)
						sum += (int)(row[i] >> 24);
			}
			if(sum > 0xff)
				sum = 0xff;
			d[x] = ((uint32_t)sum << 24) | colour;
		}
	}
}

/* Build a node's w x h bitmap from `view`, the painted glyph (possibly
 * cropped to its ink).  Style 1 (a shadow): a copy of the glyph is blended
 * in first at (sdx, sdy) with the style's density - a dimmed silhouette when
 * the shadow colour is 0, otherwise the glyph repainted in that colour
 * (and rotated when `rotate` is set, as the vertical layout rotated the
 * glyph itself) - and the glyph goes on top at (0, 0).  Style 2 (1.494
 * on, an outline): the dilated copy in the shadow colour goes at (0, 0)
 * and the glyph (sdx, sdy) into it.  Style 0 copies the glyph only. */
void Layout_GlyphBitmap(Bmp_t* dst, int w, int h, const Bmp_t* view, const Glyph_t* g, int code,
	FontRaster_t* font, const int32_t style[5], int sdx, int sdy, int rotate)
{
	Bmp_AllocScreen(dst, w, h, 1);
	Bmp_Fill(dst, NULL, 0);
	if(style[0] == 2 && gEngine->gen >= GEN_1_494)
	{
		Bmp_t cell, tmp, tmp2;
		Bmp_AllocScreen(&cell, view->w + 2 * sdx, view->h, 1);
		Bmp_Fill(&cell, NULL, 0);
		Bmp_Blit(&cell, 0, 0, view, 0, 0);
		Bmp_AllocScreen(&tmp, view->w + 2 * sdx, view->h + 2 * sdy, 1);
		Layout_PaintOutline(&tmp, &cell, sdx, sdy, (uint32_t)style[3]);
		tmp2 = tmp;
		if(rotate)
			Bmp_RotateSquare(&tmp2); // in place: only a square outline is rotated
		Bmp_Blit(dst, 0, 0, &tmp2, 1, 0x100 - style[4]);
		Bmp_Blit(dst, sdx, sdy, view, 0, 0);
		BGI_Free(tmp.pixels);
		BGI_Free(cell.pixels);
		return;
	}
	if(style[0])
	{
		Bmp_t tmp;
		Bmp_AllocScreen(&tmp, view->w, view->h, 1);
		Bmp_Fill(&tmp, NULL, 0);
		if(style[3] == 0)
			Bmp_BlitEffect(&tmp, view, 5, 0x100); // dimmed silhouette
		else
		{
			Text_PaintGlyph(&tmp, (Glyph_t*)g, code, font, (uint32_t)style[3]);
			if(rotate)
				Bmp_RotateSquare(&tmp);
		}
		Bmp_Blit(dst, sdx, sdy, &tmp, 1, 0x100 - style[4]);
		BGI_Free(tmp.pixels);
	}
	Bmp_Blit(dst, 0, 0, view, 0, 0);
}

/* The shadow offsets a style reserves on the glyph bitmaps: style 1 the
 * offset itself, style 2 (1.494 on) twice, the outline going both ways */
int Layout_ShadowReserve(const int32_t style[5], int sd)
{
	if(style[0] == 2 && gEngine->gen >= GEN_1_494)
		return 2 * sd;
	return style[0] ? sd : 0;
}

/* the pixels added after a fixed-pitch glyph: gCharPitch, plus the font's
 * own extra width ("91 0F") for its posture from 1.529 on; nothing for
 * proportional text */
int Layout_Pitch(int proportional, const FontInfo_t* fi)
{
	int extra = 0;
	if(proportional)
		return 0;
	if(gEngine->gen >= GEN_1_529 && fi && fi->font)
		extra = fi->font->extraW[fi->italic ? 1 : 0];
	return gCharPitch + extra;
}
