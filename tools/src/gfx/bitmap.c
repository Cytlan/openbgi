/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bitmap.c - rectangles, bitmap descriptors, fills and the blit entry
 * points (inc/bgi/gfx/bitmap.h).  The effect routines themselves live in
 * blit.c (copies, blends, add, punch, dim, tint), blit_modes.c (the
 * arithmetic blend modes), blit_filter.c, blit_displace.c, blit_ripple.c,
 * blit_rotate.c, blit_scale.c and blit_xform.c.
 */
#include "bgi/gfx/bitmap.h"

int gScreenMode;                             // pixel mode of the back buffer
int gBmpOption;                              // "90 0F": the composition key colour of put ARGB32 pictures
static int gBlitSse, gBlitSse2, gBlitTuning; // the CPU switches of the original's blitter selection
static int gBlitTablesReady;                 // Blit_InitTables was called

static const int gModeBytes[6] = {2, 4, 4, 1, 4, 4}; // bytes per pixel for the modes 0 .. 5; PM_VECDIST (6, 6-byte pixels) has no entry

// ---- rectangles -----------------------------------------------------------

int Rect_Inside(const Rect_t* a, const Rect_t* b)
{
	return b->l <= a->l && a->r <= b->r && b->t <= a->t && a->b <= b->b;
}

int Rect_Intersects(const Rect_t* a, const Rect_t* b)
{
	return a->l <= b->r && b->l <= a->r && a->t <= b->b && b->t <= a->b;
}

// intersect a with b in place; 1 when something is left
int Rect_Clip(Rect_t* a, const Rect_t* b)
{
	if(a->l < b->l)
		a->l = b->l;
	if(a->t < b->t)
		a->t = b->t;
	if(a->r > b->r)
		a->r = b->r;
	if(a->b > b->b)
		a->b = b->b;
	return a->l <= a->r && a->t <= a->b;
}

void Rect_Offset(Rect_t* r, int32_t dx, int32_t dy)
{
	r->l += dx;
	r->r += dx;
	r->t += dy;
	r->b += dy;
}

// the whole bitmap as a rectangle
void Rect_FromBmp(Rect_t* out, const Bmp_t* b)
{
	out->l = 0;
	out->t = 0;
	out->r = b->w - 1;
	out->b = b->h - 1;
}

// ---- modes -----------------------------------------------------------------

int ScreenMode(void)
{
	return gScreenMode;
}

int ScreenBytesPerPixel(void)
{
	return ModeBytes(ScreenMode());
}

int ModeBytes(int mode)
{
	return gModeBytes[mode];
}

int ModeBits(int mode)
{
	return ModeBytes(mode) << 3;
}

void Gfx_SetScreenMode(int m)
{
	gScreenMode = m;
}

void Gfx_SetBmpOption(int v)
{
	gBmpOption = v;
}

int Gfx_GetBmpOption(void)
{
	return gBmpOption;
}

void Blit_SetSse(int on)
{
	gBlitSse = on;
}

void Blit_SetSse2(int on)
{
	gBlitSse2 = on;
}

void Blit_SetTuning(int v)
{
	gBlitTuning = v;
}

/* Prepare the multiply tables of the MMX blitters (entry i holds
 * i * 0x100010001 for i = 0 .. 0x100).  The C blitters compute the products
 * directly; only the flag is kept so that the start-up call sequence is the
 * same as the original's. */
void Blit_InitTables(void)
{
	gBlitTablesReady = 1;
}

// equal modes, or RGB32 <-> ARGB32
int ModesCompatible(int dstMode, int srcMode)
{
	if(dstMode == srcMode)
		return 1;
	if(dstMode == PM_RGB32)
		return srcMode == PM_ARGB32;
	if(dstMode == PM_ARGB32)
		return srcMode == PM_RGB32;
	return 0;
}

// does a's mode accept b's mode for the same operation? (the two 32-bit modes accept each other)
int Bmp_ModesMatch(const Bmp_t* a, const Bmp_t* b)
{
	switch(a->mode)
	{
		case 0: return b->mode == 0;
		case 1:
		case 2: return b->mode == 1 || b->mode == 2;
		case 3: return b->mode == 3;
		case 4: return b->mode == 4;
		case 5: return b->mode == 5;
		case 6: return b->mode == 6;
		default: return 0;
	}
}

int Bmp_ScreenCompatible(const Bmp_t* b)
{
	return ModesCompatible(ScreenMode(), b->mode);
}

// ---- descriptors ------------------------------------------------------------

/* Fill in the descriptor for a w x h picture in pixel mode `mode` and
 * allocate its pixels (packed rows).  1 ok; 0 for an empty size (the
 * descriptor is still filled in, with NULL pixels) or when the allocation
 * failed. */
int Bmp_Alloc(Bmp_t* b, int w, int h, int mode)
{
	b->w = w;
	b->h = h;
	b->mode = mode;
	b->bpp = ModeBytes(mode);
	b->pitch = b->bpp * w;
	if((uint32_t)w == 0 || (uint32_t)h == 0)
	{
		b->pixels = NULL;
		return 0;
	}
	b->pixels = (uint8_t*)BGI_Alloc((size_t)b->pitch * (size_t)h);
	return b->pixels != NULL;
}

// Bmp_Alloc in the back buffer's pixel mode; withAlpha asks for ARGB32 in place of RGB32
int Bmp_AllocScreen(Bmp_t* b, int w, int h, int withAlpha)
{
	int mode = ScreenMode();
	if(withAlpha && mode == PM_RGB32)
		mode = PM_ARGB32;
	return Bmp_Alloc(b, w, h, mode);
}

void Bmp_Free(Bmp_t* b)
{
	BGI_Free(b->pixels);
	b->pixels = NULL;
}

// narrow the view to the part of r inside the bitmap; the pixels are not touched
void Bmp_Crop(Bmp_t* b, const Rect_t* r)
{
	Rect_t c = {0, 0, b->w - 1, b->h - 1};
	Rect_Clip(&c, r);
	b->pixels += b->pitch * c.t + b->bpp * c.l;
	b->w = c.r - c.l + 1;
	b->h = c.b - c.t + 1;
}

// ---- fills --------------------------------------------------------------------

// zero the bytes of the rectangle (the whole bitmap when rOrNull is NULL), whatever the mode
void Bmp_Clear(Bmp_t* b, const Rect_t* rOrNull)
{
	Bmp_t v = *b;
	int y;
	size_t n;
	if(rOrNull)
		Bmp_Crop(&v, rOrNull);
	n = (size_t)v.bpp * (size_t)v.w;
	for(y = 0; y < v.h; y++)
		memset(v.pixels + (size_t)y * v.pitch, 0, n);
}

// RGB16: the 0x00RRGGBB colour packed to 5-5-5 (the low three bits of each channel dropped)
static void Bmp_Fill16(Bmp_t* v, uint32_t colour)
{
	uint16_t c = (uint16_t)(((colour >> 9) & 0x7c00) + ((colour >> 6) & 0x3e0) + ((colour >> 3) & 0x1f));
	int y, x;
	for(y = 0; y < v->h; y++)
	{
		uint16_t* p = (uint16_t*)(v->pixels + (size_t)y * v->pitch);
		for(x = 0; x < v->w; x++)
			p[x] = c;
	}
}

// 32-bit modes: the colour word as it is
static void Bmp_Fill32(Bmp_t* v, uint32_t colour)
{
	int y, x;
	for(y = 0; y < v->h; y++)
	{
		uint32_t* p = (uint32_t*)(v->pixels + (size_t)y * v->pitch);
		for(x = 0; x < v->w; x++)
			p[x] = colour;
	}
}

/* Fill the rectangle (the whole bitmap when rOrNull is NULL) with a
 * colour: 0x00RRGGBB for RGB16 and RGB32 (the top byte is cleared for
 * RGB32), 0xAARRGGBB for ARGB32.  Bitmaps of the other modes are left
 * alone. */
void Bmp_Fill(Bmp_t* b, const Rect_t* rOrNull, uint32_t colour)
{
	Bmp_t v = *b;
	if(rOrNull)
		Bmp_Crop(&v, rOrNull);
	switch(v.mode)
	{
		case PM_RGB16: Bmp_Fill16(&v, colour); break;
		case PM_RGB32: Bmp_Fill32(&v, colour & 0xffffffu); break;
		case PM_ARGB32: Bmp_Fill32(&v, colour); break;
		default: break;
	}
}

// copy the rows of src into dst byte for byte; the views must have the same size and bytes per pixel
void Bmp_CopyRect(Bmp_t* dst, const Bmp_t* src)
{
	size_t n = (size_t)src->bpp * (size_t)src->w;
	int y;
	for(y = 0; y < src->h; y++)
		memcpy(dst->pixels + (size_t)y * dst->pitch, src->pixels + (size_t)y * src->pitch, n);
}

// ---- blit entry points -----------------------------------------------------------

/* Draw src with its top-left pixel at (x, y) of dst.  The overlap of the
 * two is computed, both views are narrowed to it and Bmp_BlitEffect does
 * the drawing; 4 when nothing overlaps (negative or off-bitmap positions
 * are fine), else its result. */
int Bmp_Blit(Bmp_t* dst, int x, int y, const Bmp_t* src, int effect, int level)
{
	Bmp_t d = *dst, s = *src;
	Rect_t dr = {0, 0, d.w - 1, d.h - 1};
	Rect_t sr = {0, 0, s.w - 1, s.h - 1};

	Rect_Offset(&dr, -x, -y); // destination in source coordinates
	if(!Rect_Clip(&sr, &dr))
		return 4;
	Bmp_Crop(&s, &sr);
	Rect_Offset(&sr, x, y); // back to destination coordinates
	Bmp_Crop(&d, &sr);
	return Bmp_BlitEffect(&d, &s, effect, level);
}

/* Dispatch an effect number (listed in bitmap.h) to its routine for two
 * views of the same size.  0 ok, 1 the pixel modes are not compatible
 * (ModesCompatible), 2 unknown effect, 3 level above 0x100.  The 0x2x
 * effects are the 0x0x ones with the level inverted. */
int Bmp_BlitEffect(Bmp_t* dst, const Bmp_t* src, int effect, int level)
{
	if(!ModesCompatible(dst->mode, src->mode))
		return 1;
	if((uint32_t)level > 0x100u)
		return 3;
	switch(effect)
	{
		case 0x00: Blit_AlphaCopy(dst, src); return 0;
		case 0x01:
		case 0x20: Blit_Blend(dst, src, level); return 0;
		case 0x02: Blit_Add(dst, src, level); return 0;
		case 0x03: Blit_Sub(dst, src, level); return 0;
		case 0x04: Blit_Mul(dst, src, level); return 0;
		case 0x05:
		case 0xC0: Blit_Dim(dst, src, level); return 0;
		case 0x06: Blit_Fx06(dst, src, level); return 0;
		case 0x07: Blit_Fx07(dst, src, level); return 0;
		case 0x08: Blit_Fx08(dst, src, level); return 0;
		case 0x09: Blit_Fx09(dst, src, level); return 0;
		case 0x21: Blit_Add(dst, src, 0x100 - level); return 0;
		case 0x22: Blit_Sub(dst, src, 0x100 - level); return 0;
		case 0x23: Blit_Mul(dst, src, 0x100 - level); return 0;
		case 0x24: Blit_Fx06(dst, src, 0x100 - level); return 0;
		case 0x25: Blit_Fx07(dst, src, 0x100 - level); return 0;
		case 0x26: Blit_Fx08(dst, src, 0x100 - level); return 0;
		case 0x27: Blit_Fx09(dst, src, 0x100 - level); return 0;
		case 0x40: Blit_Punch(dst, src, level); return 0;
		case 0x41: Blit_Fx41(dst, src); return 0;
		case 0x80: Blit_Copy(dst, src); return 0;
		case 0xC1: Blit_Tint(dst, src, 0xffffffu, level); return 0;
		case 0xF0: Blit_Over(dst, src, level); return 0;
		case 0xFF: Blit_FxFF(dst, src, level); return 0;
		default: return 2;
	}
}
