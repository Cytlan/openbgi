/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bmpops.c - the bitmap operations behind the "90 1x" / "91 1x" instructions:
 *            creating and freeing slots, the blits between slots, the
 *            resampling and transforms (inc/bgi/gfx/bmpops.h)
 *
 * The operations fetch the bitmaps of the slots they are given with
 * BmpMgr_GetInfo (a slot that is not in use is error 1 for the destination,
 * 2 for the source, 3 for a third bitmap) and translate the blitters'
 * result codes into the instruction's own numbering.  A few unexpected
 * blitter codes end in "return dst" - the original returns that dead
 * argument there, and so does this code.
 */
#include <string.h>
#include <math.h>

#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/font.h"
#include "bgi/gfx/text.h"
#include "bgi/gfx/dispobj.h"
#include "bgi/file.h"
#include "bgi/codec.h"
#include "bgi/strutil.h"
#include "bgi/msg.h"
#include "bgi/os.h"

// ---- the manager forwarders --------------------------------------------------------------------
// (the manager's own result codes; see bmpmgr.h)

int BmpOp_Put(int slot, int w, int h, int mode, const void* data)
{
	return BmpMgr_PutPixels(gBmpMgr, slot, w, h, mode, data);
}

int BmpOp_Get(void* dst, int unused, int size, int slot)
{
	return BmpMgr_GetPixels(gBmpMgr, dst, unused, size, slot);
}

int BmpOp_Create(int slot, int w, int h, int mode)
{
	return BmpMgr_Create(gBmpMgr, slot, w, h, mode);
}

int BmpOp_Clear(int slot, uint32_t colour)
{
	return BmpMgr_Clear(gBmpMgr, slot, colour);
}

int BmpOp_Free(int slot)
{
	return BmpMgr_FreeSlot(gBmpMgr, slot);
}

int BmpOp_GetInfo(Bmp_t* out, int slot)
{
	return BmpMgr_GetInfo(gBmpMgr, out, slot);
}

int BmpOp_ToGray(int dst, int src)
{
	return BmpMgr_ToGray(gBmpMgr, dst, src);
}

int BmpOp_InvertGray(int slot)
{
	return BmpMgr_InvertGray(gBmpMgr, slot);
}

// ---- blits -------------------------------------------------------------------------------------

/* "90 18": Bmp_Blit of src at (x, y) of dst with the effect and level.
 * 0 ok, 1 / 2 no destination / source; the Bmp_Blit codes renumbered: 3
 * incompatible modes, 4 unknown effect, 5 bad level, 6 no overlap, 7 any
 * other. */
int BmpOp_Blit(int dst, int x, int y, int src, int effect, int level)
{
	Bmp_t d, s;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, dst))
		return 1;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 2;
	switch(Bmp_Blit(&d, x, y, &s, effect, level))
	{
		case 0: return 0;
		case 1: return 3;
		case 2: return 4;
		case 3: return 5;
		case 4: return 6;
		default: return 7;
	}
}

/* "92 1A" of 1.494 on: dst becomes a
 * grayscale bitmap of the back buffer's size holding, at (x, y), the alpha
 * of src (a 32-bit bitmap) - blended with the alpha of src2 by ratio / 256
 * when src2 is given (-1: none) - and 0 everywhere else.  0 ok,
 * 0x80000009 dst cannot be made, 0x8000000a / 0x8000000b src / src2
 * unknown, 0x8000000c their modes differ. */
uint32_t BmpOp_AlphaToScreenGray(int dst, int src, int src2, int x, int y, int ratio)
{
	Bmp_t s, s2, d, back;
	Rect_t r, sr;
	int yy, xx, has2 = src2 != -1;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 0x8000000au;
	if(has2 && !BmpMgr_GetInfo(gBmpMgr, &s2, src2))
		return 0x8000000bu;
	Gfx_GetBackBmp(&back); // the display manager's back buffer
	if(!BmpMgr_Create(gBmpMgr, dst, back.w, back.h, 3))
		return 0x80000009u;
	BmpMgr_GetInfo(gBmpMgr, &d, dst);
	if(has2 && s2.mode != s.mode)
		return 0x8000000cu;
	Bmp_Fill(&d, NULL, 0);
	if(s.mode != 2)
		return 0;
	// the overlap of the source placed at (x, y) with the destination
	Rect_FromBmp(&sr, &s);
	Rect_Offset(&sr, x, y);
	Rect_FromBmp(&r, &d);
	if(!Rect_Clip(&r, &sr))
		return 0;
	for(yy = r.t; yy <= r.b; yy++)
	{
		const uint8_t* sp = s.pixels + (size_t)(yy - y) * (size_t)s.pitch + (size_t)(r.l - x) * 4 + 3;
		const uint8_t* sp2 = has2 ? s2.pixels + (size_t)(yy - y) * (size_t)s2.pitch + (size_t)(r.l - x) * 4 + 3 : NULL;
		uint8_t* dp = d.pixels + (size_t)yy * (size_t)d.pitch + (size_t)r.l;
		for(xx = r.l; xx <= r.r; xx++, sp += 4, dp++)
		{
			uint32_t a = *sp;
			if(sp2)
			{
				a += (uint32_t)(((int32_t)*sp2 - (int32_t)a) * ratio) >> 8;
				sp2 += 4;
			}
			*dp = (uint8_t)a;
		}
	}
	return 0;
}

/* "90 19": Blit_ThroughGray of src at (x, y) of dst through the gray map
 * in slot `gray`.  0 ok, 1 / 2 / 3 no destination / source / gray; the
 * blitter's codes renumbered: 4 modes differ, 5 gray not GRAY8, 6 gray
 * and src differ in size, 7 no overlap, 8 bad level, 9 any other. */
int BmpOp_BlitMask(int dst, int x, int y, int src, int gray, int param, int level)
{
	Bmp_t d, s, g;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, dst))
		return 1;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 2;
	if(!BmpMgr_GetInfo(gBmpMgr, &g, gray))
		return 3;
	switch(Blit_ThroughGray(&d, x, y, &s, &g, param, level))
	{
		case 0: return 0;
		case 1: return 4;
		case 3: return 8;
		case 4: return 7;
		case 7: return 5;
		case 8: return 6;
		default: return 9;
	}
}

/* "90 1A": Blit_Displace of src through the vector maps in slots vec1 and
 * vec2 (vec2 may be -1: none) into dst.  0 ok, 1 / 2 / 4 no destination /
 * source / vec1, 6 vec2 missing; the blitter's codes renumbered: 3 modes
 * differ, 5 / 7 vec1 / vec2 not a vector map of src's size, 8 bad level;
 * any other code is 0. */
int BmpOp_Displace(int dst, int src, int vec1, int vec2, int level, int amount)
{
	Bmp_t d, s, v1, v2;
	Bmp_t* pv2 = vec2 != -1 ? &v2 : NULL;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, dst))
		return 1;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 2;
	if(!BmpMgr_GetInfo(gBmpMgr, &v1, vec1))
		return 4;
	if(pv2 && !BmpMgr_GetInfo(gBmpMgr, &v2, vec2))
		return 6;
	switch(Blit_Displace(&d, &s, &v1, pv2, level, amount))
	{
		case 1: return 3;
		case 3: return 8;
		case 12: return 5;
		case 13: return 7;
		default: return 0; // 0 and the unmapped codes alike
	}
}

/* "90 1B": Blit_Gradient (the box blur) of src into dst.  0 ok, 1 / 2 no
 * destination / source; the blitter's codes renumbered: 3 mode or size
 * mismatch, 4 unknown type, 5 bad level. */
int BmpOp_Gradient(int dst, int src, int type, int level)
{
	Bmp_t d, s;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, dst))
		return 1;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 2;
	switch(Blit_Gradient(&d, &s, type, level))
	{
		case 0: return 0;
		case 3: return 5;
		case 14: return 4;
		case 15: return 3;
		default: return dst; // the original returns the dead `dst` argument here
	}
}

// the two scalers behind Resample: filtered or nearest neighbour
static void ScaleInto(Bmp_t* dst, const Bmp_t* src, int32_t sx16, int32_t sy16, int smooth)
{
	if(smooth)
		Blit_Scale(dst, src, sx16, sy16);
	else
		Blit_ScaleNearest(dst, src, sx16, sy16);
}

/* "91 1C": create `dst` as `src` scaled by the 16.16 factors (the result
 * size is truncated).  Only RGB32 / ARGB32 sources (3); a zero result size
 * is 4; 1 when the slot could not be created, 2 when there is no source. */
int BmpOp_Resample(int dst, int src, int32_t sx16, int32_t sy16, int smooth)
{
	Bmp_t s, d;
	uint32_t w, h;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 2;
	if(s.mode != PM_RGB32 && s.mode != PM_ARGB32)
		return 3;
	w = (uint32_t)(s.w * sx16) >> 16;
	h = (uint32_t)(s.h * sy16) >> 16;
	if(w == 0 || h == 0)
		return 4;
	if(!BmpMgr_Create(gBmpMgr, dst, (int)w, (int)h, s.mode))
		return 1;
	BmpMgr_GetInfo(gBmpMgr, &d, dst);
	ScaleInto(&d, &s, sx16, sy16, smooth);
	return 0;
}

/* "91 1B" of 1.494 on: Blit_GatherAdd of src into dst with the 16.16
 * gathering rates cx, cy and the attenuation level.  0 ok, 1 / 2 no
 * destination / source, 4 a rate above 1.0, 5 a level above 0x100. */
int BmpOp_GatherAdd(int dst, int src, uint32_t cx, uint32_t cy, uint32_t level)
{
	Bmp_t d, s;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, dst))
		return 1;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 2;
	if(cx > 0x10000 || cy > 0x10000)
		return 4;
	if(level > 0x100)
		return 5;
	Blit_GatherAdd(&d, &s, cx, cy, level);
	return 0;
}

/* "91 1D" of 1.494 on: src into dst through colour mode 0 (Blit_Copy), 1
 * (Blit_FilterKind3), 2 (Blit_FilterKind2), 3 (Blit_TintKeepAlpha) or 4
 * (Blit_AddColour) with the colour and level.  0 ok, 1 / 2 no destination
 * / source, 6 unknown mode. */
int BmpOp_ColourMode(int dst, int src, int mode, uint32_t colour, int level)
{
	Bmp_t d, s;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, dst))
		return 1;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 2;
	switch(mode)
	{
		case 0: Blit_Copy(&d, &s); break;
		case 1: Blit_FilterKind3(&d, &s, colour, level); break;
		case 2: Blit_FilterKind2(&d, &s, colour, level); break;
		case 3: Blit_TintKeepAlpha(&d, &s, colour, level); break;
		case 4: Blit_AddColour(&d, &s, colour, level); break;
		default: return 6;
	}
	return 0;
}

/* "90 1C": draw the (sx, sy, sw, sh) part of `src` scaled (smoothly) into
 * the (x, y, w, h) rectangle of `dst`, clipped to the destination.  Both
 * sizes must be at least 2 (5 / 6); the scale factors use 65540 rather
 * than 65536 per unit.  0 ok, 1 / 2 no destination / source; the
 * Blit_Scale2 codes renumbered: 6 full size below 2.0, 7 zero scale, 8
 * modes differ. */
int BmpOp_Stretch(int dst, int x, int y, int w, int h, int src, int sx, int sy, int sw, int sh)
{
	Bmp_t d, s;
	Rect_t r, full;
	int32_t fx, fy;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, dst))
		return 1;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 2;
	if((uint32_t)w < 2 || (uint32_t)h < 2)
		return 5;
	if((uint32_t)sw < 2 || (uint32_t)sh < 2)
		return 6;
	r.l = x;
	r.t = y;
	r.r = x + w - 1;
	r.b = y + h - 1;
	Rect_FromBmp(&full, &d);
	Rect_Clip(&full, &r);
	Bmp_Crop(&d, &full);
	fy = BGI_Ftol((double)h * 65540.0 / (double)sh);
	fx = BGI_Ftol((double)w * 65540.0 / (double)sw);
	switch(Blit_Scale2(&d, &s, sx << 16, sy << 16, sw << 16, sh << 16, fx, fy, 1))
	{
		case 0: return 0;
		case 1: return 8;
		case 19: return 7;
		case 20: return 6;
		default: return dst; // the original returns the dead `dst` argument here
	}
}

/* "90 1D": src rotated by angle (16.16 degrees) and zoomed (16.16) about
 * its centre into dst, nearest sampling (Blit_RotateCentred).  0 ok, 1 /
 * 2 no destination / source, 3 modes differ, 4 zoom <= 0. */
int BmpOp_Rotate(int dst, int src, int32_t zoom, int32_t angle)
{
	Bmp_t d, s;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, dst))
		return 1;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 2;
	switch(Blit_RotateCentred(&d, &s, zoom, angle, 0))
	{
		case 0: return 0;
		case 1: return 3;
		case 0x13: return 4;
		default: return dst; // the original returns the dead `dst` argument here
	}
}

/* "91 18": Blit_Xform of src onto dst (the arguments as there).  0 ok, 1
 * / 2 no destination / source, 3 zero scale. */
int BmpOp_Xform(int dst, int32_t x16, int32_t y16, int src, int32_t cx16, int32_t cy16, int32_t angle,
	int32_t sx, int32_t sy, int level, int smooth)
{
	Bmp_t d, s;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, dst))
		return 1;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 2;
	switch(Blit_Xform(&d, x16, y16, &s, cx16, cy16, angle, sx, sy, level, smooth))
	{
		case 0: return 0;
		case 0x13: return 3;
		default: return dst; // the original returns the dead `dst` argument here
	}
}

// "91 19": Blit_XformCopy of src into dst; the BmpOp_Xform codes
int BmpOp_XformCopy(int dst, int32_t x16, int32_t y16, int src, int32_t cx16, int32_t cy16, int32_t angle,
	int32_t sx, int32_t sy, int level, int smooth)
{
	Bmp_t d, s;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, dst))
		return 1;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 2;
	switch(Blit_XformCopy(&d, x16, y16, &s, cx16, cy16, angle, sx, sy, level, smooth))
	{
		case 0: return 0;
		case 0x13: return 3;
		default: return dst; // the original returns the dead `dst` argument here
	}
}

/* "91 1E": copy a gray bitmap into the alpha channel of `dst`; the gray is
 * placed so that dst pixel (px, py) takes gray pixel (px + x, py + y)
 * (Blit_ApplyMask over the overlap; an RGB32 dst is left alone).  0 ok,
 * 1 / 2 no destination / gray, 3 when `gray` is not a gray bitmap. */
int BmpOp_ApplyGray(int dst, int gray, int x, int y)
{
	Bmp_t d, g;
	Rect_t rd, rg;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, dst))
		return 1;
	if(!BmpMgr_GetInfo(gBmpMgr, &g, gray))
		return 2;
	if(g.mode != PM_GRAY8)
		return 3;
	Rect_FromBmp(&rd, &d);
	Rect_FromBmp(&rg, &g);
	Rect_Offset(&rg, -x, -y);
	if(Rect_Clip(&rd, &rg))
	{
		Bmp_Crop(&d, &rd);
		Rect_Offset(&rd, x, y);
		Bmp_Crop(&g, &rd);
		Blit_ApplyMask(&d, &d, &g);
	}
	return 0;
}

/* "90 1E": plain copy (effect 0x80) of the (sx, sy, w, h) part of `src`,
 * clipped to the source, to (dx, dy) of `dst`.  0 ok, 1 / 2 no destination
 * / source, 3 for an empty size. */
int BmpOp_CopyRect(int dst, int dx, int dy, int src, int sx, int sy, int w, int h)
{
	Bmp_t d, s;
	Rect_t r, full;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, dst))
		return 1;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 2;
	if((uint32_t)w == 0 || (uint32_t)h == 0)
		return 3;
	r.l = sx;
	r.t = sy;
	r.r = sx + w - 1;
	r.b = sy + h - 1;
	Rect_FromBmp(&full, &s);
	if(Rect_Clip(&full, &r))
	{
		Bmp_Crop(&s, &full);
		Bmp_Blit(&d, dx, dy, &s, 0x80, 0);
	}
	return 0;
}

/* "90 1F": `dst` becomes a new w x h bitmap of src's mode holding the part
 * of `src` at (x, y); pixels src does not cover stay uninitialised.  0 ok,
 * 1 the slot could not be created, 2 no source, 3 empty size. */
int BmpOp_CloneRect(int dst, int src, int x, int y, int w, int h)
{
	Bmp_t d, s;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 2;
	if((uint32_t)w == 0 || (uint32_t)h == 0)
		return 3;
	if(!BmpMgr_Create(gBmpMgr, dst, w, h, s.mode))
		return 1;
	BmpMgr_GetInfo(gBmpMgr, &d, dst);
	Bmp_Blit(&d, -x, -y, &s, 0x80, 0);
	return 0;
}

// "91 1F": `dst` becomes a copy of `src`; 0 ok, 1 the slot could not be created, 2 no source
int BmpOp_Duplicate(int dst, int src)
{
	Bmp_t d, s;
	if(!BmpMgr_GetInfo(gBmpMgr, &s, src))
		return 2;
	if(!BmpMgr_Create(gBmpMgr, dst, s.w, s.h, s.mode))
		return 1;
	BmpMgr_GetInfo(gBmpMgr, &d, dst);
	Bmp_CopyRect(&d, &s);
	{ // 1.529 on: the base size goes along
		int32_t base[2];
		BmpMgr_GetBaseSize(gBmpMgr, base, src);
		BmpMgr_SetBaseSize(gBmpMgr, dst, base[0], base[1]);
	}
	return 0;
}

/* "92 17" of 1.529 on: the pixel at (x, y) of
 * the bitmap into the 4 bytes at out (zeroed first; as many bytes as the
 * pixel has).  0 ok, 1 no bitmap, 2 more than 4 bytes per pixel, 3 (x, y)
 * outside. */
int BmpOp_GetPixel(void* out, int slot, int x, int y)
{
	Bmp_t b;
	int bytes;
	if(!BmpMgr_GetInfo(gBmpMgr, &b, slot))
		return 1;
	bytes = b.bpp; // bytes per pixel
	if(bytes > 4)
		return 2;
	if(x < 0 || x >= b.w || y < 0 || y >= b.h)
		return 3;
	memset(out, 0, 4);
	memcpy(out, (const uint8_t*)b.pixels + (size_t)b.pitch * y + (size_t)bytes * x, (size_t)bytes);
	return 0;
}
