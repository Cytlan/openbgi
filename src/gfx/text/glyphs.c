/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * glyphs.c - the pictures the text engine draws besides font glyphs: the
 *            custom glyph sheet ("90 9E": a bitmap cut into equal cells
 *            that replaces the glyphs of the private codes 0xFF01..0xFFFF),
 *            the per-code character images ("92 98"), and the cursor
 *            pictures shown while a text output waits for input ("90 98")
 *            (src/include/bgi/gfx/text.h)
 *
 * Text_PaintGlyph is the painter the layout and the plain output call for
 * every character: a private code is served from its picture or the sheet,
 * everything else goes to Font_PaintGlyph.  Bmp_RotateSquare, used by the
 * vertical layout, lives here as well.
 */
#include "text_internal.h"
#include "bgi/core.h"

// ---- the glyph sheet ---------------------------------------------------------------------------

/* "90 9E": take bitmap `bmp` as a strip of `count` (0..0xFF) equal cells
 * that stand for the codes 0xFF01 .. 0xFF00 + count; the sheet is copied.
 * count == 0 releases the sheet.  0 ok, 0x80000001 bad count, 0x80000002
 * no such bitmap, 0x80000003 the width is not a multiple of the count. */
int Text_SetGlyphSheet(OpenBGI_t* o, int count, int bmp)
{
	Bmp_t info;

	if(count < 0 || count >= 0x100)
		return (int)0x80000001;
	if(count <= 0)
	{
		BGI_Free(o->text.glyphSheet.pixels);
		memset(&o->text.glyphSheet, 0, sizeof o->text.glyphSheet);
		o->text.glyphCount = 0;
		return 0;
	}
	if(!BmpMgr_GetInfo(o->text.textBmpMgr, &info, bmp))
		return (int)0x80000002;
	if((uint32_t)info.w % (uint32_t)count != 0)
		return (int)0x80000003;
	BGI_Free(o->text.glyphSheet.pixels);
	Bmp_Alloc(&o->text.glyphSheet, info.w, info.h, info.mode);
	Bmp_CopyRect(&o->text.glyphSheet, &info);
	o->text.glyphCount = count;
	o->text.glyphCellW = (int32_t)((uint32_t)info.w / (uint32_t)count);
	return 0;
}

// release the sheet and every "92 98" picture (engine shutdown)
void Text_FreeGlyphSheet(OpenBGI_t* o)
{
	int i;
	Text_SetGlyphSheet(o, 0, 0);
	for(i = 0; i < 0x100; i++)
		Text_SetCharImage(o, 0xff00 + i, -1, 0, 0, 0, 0);
}

/* "92 98" of 1.588 on: give the private code 0xFF01 .. 0xFFFF its own
 * picture, the w x h region of bitmap `bmp` at (x, y) copied into a bitmap
 * of the region's size; bmp == -1 drops the code's picture.  0 ok,
 * 0x80000002 no such bitmap, 0x80000006 code out of range, 0x80000007 an
 * empty size.  The picture takes precedence over the glyph sheet's cell
 * when the text is drawn. */
int Text_SetCharImage(OpenBGI_t* o, uint32_t code, int bmp, int x, int y, int w, int h)
{
	Bmp_t* slot;
	Bmp_t info;
	if(code - 0xff01u > 0xfeu)
		return (int)0x80000006;
	slot = &o->text.charImages[code & 0xff];
	if(bmp == -1)
	{
		BGI_Free(slot->pixels);
		memset(slot, 0, sizeof *slot);
		return 0;
	}
	if(!BmpMgr_GetInfo(o->text.textBmpMgr, &info, bmp))
		return (int)0x80000002;
	if(w == 0 || h == 0)
		return (int)0x80000007;
	BGI_Free(slot->pixels);
	Bmp_Alloc(slot, w, h, info.mode);
	Bmp_Fill(slot, NULL, 0);
	Bmp_Blit(slot, -x, -y, &info, 0x80, 0); // the region at (x, y) lands at the picture's origin
	return 0;
}

const Bmp_t* Text_CharImage(OpenBGI_t* o, uint32_t code) // the picture of a private code, NULL when it has none
{
	if(code - 0xff01u > 0xfeu || !o->text.charImages[code & 0xff].pixels)
		return NULL;
	return &o->text.charImages[code & 0xff];
}

/* like Font_PaintGlyph, but the private codes 0xFF01 .. 0xFFFF are taken
 * from their picture or cut out of the glyph sheet instead of being
 * rasterised.  A gray (mode 3) picture is used as coverage for `colour`,
 * anything else is copied with its alpha.  `g` describes the painted
 * extent; for a sheet code it describes a full cell even when the cell
 * number is out of range (nothing is painted then). */
void Text_PaintGlyph(OpenBGI_t* o, Bmp_t* cell, Glyph_t* g, int code, FontRaster_t* font, uint32_t colour)
{
	int cellNo;
	Bmp_t src;
	Rect_t r;

	if((uint32_t)code < 0xff00u || (uint32_t)code > 0xffffu)
	{
		Font_PaintGlyph(o, cell, g, code, font, colour);
		return;
	}
	/* With UTF-8 text a code is a Unicode code point, and U+FF01 .. U+FFEE
	 * are the full-width forms ("！", "，", "（"): a code that has neither a
	 * picture nor a cell of the sheet is a character of the text there, not
	 * a private code that draws nothing. */
	if(o->enc.textUtf8 && !Text_CharImage(o, (uint32_t)code) && (code & 0xff) - 1 >= o->text.glyphCount)
	{
		Font_PaintGlyph(o, cell, g, code, font, colour);
		return;
	}
	if(Text_CharImage(o, (uint32_t)code))
	{
		// a picture of its own ("92 98"): clipped to the cell
		src = *Text_CharImage(o, (uint32_t)code);
		r.l = 0;
		r.t = 0;
		r.r = (src.w <= cell->w ? src.w : cell->w) - 1;
		r.b = (src.h <= cell->h ? src.h : cell->h) - 1;
		Bmp_Crop(&src, &r);
		if(src.mode == 3)
			Blit_ColourThroughGray(cell, &src, colour);
		else
			Blit_AlphaCopy(cell, &src);
		g->code = code;
		g->dbcs = 1;
		g->pixels = NULL;
		g->left = 0;
		g->top = 0;
		g->right = r.r;
		g->bottom = r.b;
		return;
	}
	cellNo = (code & 0xff) - 1;
	if(cellNo < o->text.glyphCount)
	{
		src = o->text.glyphSheet;
		r.l = o->text.glyphCellW * cellNo;
		r.t = 0;
		r.r = (o->text.glyphCellW <= cell->w ? o->text.glyphCellW : cell->w) + r.l - 1;
		r.b = ((uint32_t)src.h <= (uint32_t)cell->h ? src.h : cell->h) - 1;
		Bmp_Crop(&src, &r);
		if(src.mode == 3)
			Blit_ColourThroughGray(cell, &src, colour);
		else
			Blit_AlphaCopy(cell, &src);
	}
	g->code = code;
	g->dbcs = 1;
	g->pixels = NULL;
	g->left = 0;
	g->top = 0;
	g->right = o->text.glyphCellW - 1;
	g->bottom = o->text.glyphSheet.h - 1;
}

// ---- the cursor pictures -----------------------------------------------------------------------

/* "90 98": copy the `n` listed bitmaps (-1 = empty frame) into private
 * screen-format copies that the text output cycles through while it waits.
 * A single entry (n <= 1) means "no cursor".  1 ok; 0 and *outBad = the
 * bitmap number when one of them does not exist or cannot be converted. */
int Text_SetItemBitmaps(OpenBGI_t* o, int n, const int* bmpList, int* outBad)
{
	int i;

	for(i = 0; i < o->text.textItemCount; i++)
		BGI_Free(o->text.textItems[i].pixels);
	if(o->text.textItemCount > 0)
		BGI_Free(o->text.textItems);
	o->text.textItemCount = n;
	o->text.textItems = NULL;
	if(n <= 1)
		return 1;
	o->text.textItems = (Bmp_t*)BGI_Calloc((size_t)n * sizeof(Bmp_t));
	for(i = 0; i < n; i++)
	{
		Bmp_t info;

		if(bmpList[i] == -1)
		{
			memset(&o->text.textItems[i], 0, sizeof(Bmp_t));
			continue;
		}
		if(!BmpMgr_GetInfo(o->text.textBmpMgr, &info, bmpList[i]))
		{
			*outBad = bmpList[i];
			return 0;
		}
		Bmp_AllocScreen(o, &o->text.textItems[i], info.w, info.h, 1);
		if(Bmp_BlitEffect(&o->text.textItems[i], &info, 0x80, 0) == 1)
		{
			*outBad = bmpList[i];
			return 0;
		}
	}
	return 1;
}

void Text_FreeItemBitmaps(OpenBGI_t* o) // release the cursor pictures (engine shutdown)
{
	Text_SetItemBitmaps(o, 0, NULL, NULL);
}

// ---- bitmap helper of the vertical layout ------------------------------------------------------

// rotate a square bitmap 90 degrees clockwise in place; 0 if not square
int Bmp_RotateSquare(Bmp_t* b)
{
	Bmp_t tmp;
	uint32_t x, y;

	if(b->w != b->h)
		return 0;
	Bmp_Alloc(&tmp, b->w, b->h, b->mode);
	for(y = 0; y < (uint32_t)b->h; y++)
	{
		const uint8_t* src = b->pixels + (size_t)y * (size_t)b->pitch;
		// row y of the source becomes column (w - 1 - y) of the result
		uint8_t* dst = tmp.pixels + (size_t)(tmp.w - 1 - y) * (size_t)tmp.bpp;
		for(x = 0; x < (uint32_t)b->w; x++)
		{
			switch(b->bpp)
			{
				case 4: *(uint32_t*)dst = *(const uint32_t*)src; break;
				case 2: *(uint16_t*)dst = *(const uint16_t*)src; break;
				default: *dst = *src; break;
			}
			dst += tmp.pitch;
			src += b->bpp;
		}
	}
	Bmp_CopyRect(b, &tmp);
	BGI_Free(tmp.pixels);
	return 1;
}
