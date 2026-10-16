/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bitmap.h - the pixel vocabulary of the renderer: the 24-byte bitmap
 * descriptor, inclusive rectangles, pixel modes and the blitters.
 * Implemented in src/gfx/bitmap.c (rectangles, descriptors, fills, the
 * blit entry points) and src/gfx/blit*.c (the effect routines).
 *
 * A Bmp is a *view*: it is copied by value and narrowed to a sub-rectangle
 * (Bmp_Crop) without touching the pixels.  Every blitter takes equal-sized
 * views; Bmp_Blit() does the clipping and cropping for a positioned blit.
 *
 * The original implements most routines three times (C, MMX, SSE).  Only
 * the arithmetic matters for the result; the MMX/SSE variants are the same
 * computation on several pixels at once, and this file implements each
 * routine once in plain C with exactly the same rounding, shifts and
 * saturation.  Where the original's table-driven MMX arithmetic quantises
 * a level to 7 bits (level >> 1) or scales through a 16-bit high multiply,
 * the C code does the same.
 */
#ifndef BGI_GFX_BITMAP_H_
#define BGI_GFX_BITMAP_H_

#include "bgi/common.h"

// ---- rectangles (inclusive on all four sides) ---------------------------
typedef struct Rect
{
	int32_t l, t, r, b; // pixels; r and b are the last pixel inside
} Rect_t;

int Rect_Inside(const Rect_t* a, const Rect_t* b);     // 1 when a lies completely in b
int Rect_Intersects(const Rect_t* a, const Rect_t* b); // 1 when a and b share a pixel
int Rect_Clip(Rect_t* a, const Rect_t* b);             // a &= b; 1 if not empty
void Rect_Offset(Rect_t* r, int32_t dx, int32_t dy);   // move r by (dx, dy)

// ---- pixel modes ----------------------------------------------------------
enum PixelMode
{
	PM_RGB16 = 0,  // 2 bytes: 5-5-5 (0x7C00 / 0x03E0 / 0x001F)
	PM_RGB32 = 1,  // 4 bytes: B G R x, the top byte is ignored
	PM_ARGB32 = 2, // 4 bytes: B G R A
	PM_GRAY8 = 3,  // 1 byte: masks, wipe patterns, filter maps
	PM_VECTOR = 4, // 4 bytes: per-pixel displacement (vector map), int16 dx, int16 dy in 12.4
	PM_HEIGHT = 5, // 4 bytes: unsigned height field, input of the vector generators
	PM_VECDIST = 6 // 6 bytes: vector + ring distance, the map of the ripple effects
};

extern int gScreenMode;                        // pixel mode of the back buffer
extern int gBmpOption;                         // "90 0F": the key colour put ARGB32 pictures were composited over, 0 = none
int ScreenMode(void);                          // gScreenMode
int ScreenBytesPerPixel(void);                 // ModeBytes of the back buffer's mode
int ModeBytes(int mode);                       // {2,4,4,1,4,4}[mode]
int ModeBits(int mode);                        // ModeBytes * 8
int ModesCompatible(int dstMode, int srcMode); // equal, or RGB32 <-> ARGB32

// ---- the descriptor -------------------------------------------------------
typedef struct Bmp
{
	uint8_t* pixels; // top-left pixel; NULL = virtual (size only)
	int32_t pitch;   // bytes per row
	int32_t w;       // width in pixels
	int32_t h;       // height in pixels
	int32_t mode;    // PixelMode
	int32_t bpp;     // bytes per pixel
} Bmp_t;

int Bmp_Alloc(Bmp_t* b, int w, int h, int mode);                 // allocate pitch * h bytes; 1 ok, 0 empty size or out of memory
int Bmp_AllocScreen(Bmp_t* b, int w, int h, int withAlpha);      // in the back buffer's mode; withAlpha turns RGB32 into ARGB32
void Bmp_Free(Bmp_t* b);                                         // release the pixels of a Bmp_Alloc'd bitmap
void Rect_FromBmp(Rect_t* out, const Bmp_t* b);                  // {0, 0, w - 1, h - 1}
void Bmp_Crop(Bmp_t* b, const Rect_t* r);                        // narrow the view to r (clipped to the bitmap)
int Bmp_ModesMatch(const Bmp_t* a, const Bmp_t* b);              // a's mode accepts b's: equal, or both 32-bit
int Bmp_ScreenCompatible(const Bmp_t* b);                        // ModesCompatible with the back buffer
void Bmp_Clear(Bmp_t* b, const Rect_t* rOrNull);                 // zero fill of the rectangle (or all)
void Bmp_Fill(Bmp_t* b, const Rect_t* rOrNull, uint32_t colour); // 0xAARRGGBB fill; RGB16 / RGB32 / ARGB32 only
void Bmp_CopyRect(Bmp_t* dst, const Bmp_t* src);                 // raw row copy of equal-sized views, no mode check

/* Positioned blit of `src` with its top-left pixel at (x, y) of `dst`,
 * clipped to the destination.  4 when nothing overlaps, else the result of
 * Bmp_BlitEffect on the overlapping views. */
int Bmp_Blit(Bmp_t* dst, int x, int y, const Bmp_t* src, int effect, int level);
/* Blit `src` onto the equal-sized view `dst` with one of the effects below.
 * `level` is 0 .. 0x100: the transparency for the blends (0 opaque) and
 * the strength for the arithmetic modes (0 draws nothing).  0 ok, 1
 * incompatible pixel modes, 2 unknown effect, 3 level above 0x100.
 *
 *   0x00 alpha copy        0x20 blend (same as 0x01)
 *   0x01 blend             0x21 .. 0x27 the effects 0x02 .. 0x04 and
 *   0x02 add                    0x06 .. 0x09 with 0x100 - level
 *   0x03 subtract          0x40 punch: clear where the source is set
 *   0x04 multiply          0x41 clear the whole area
 *   0x05 dim (= 0xC0)      0x80 raw copy
 *   0x06 screen            0xC0 dim toward black
 *   0x07 alpha erase       0xC1 tint toward white
 *   0x08 overlay           0xF0 blend without the colour key / alpha
 *   0x09 hard light        0xFF channel extraction (level = channel)
 */
int Bmp_BlitEffect(Bmp_t* dst, const Bmp_t* src, int effect, int level);

// ---- effect routines (the "Blit_*" family, equal-sized views) -------------
void Blit_Copy(Bmp_t* dst, const Bmp_t* src);                             // 0x80: raw copy, converting RGB32 <-> ARGB32
void Blit_AlphaCopy(Bmp_t* dst, const Bmp_t* src);                        // 0x00: copy honouring the colour key (RGB16) or the alpha
void Blit_Blend(Bmp_t* dst, const Bmp_t* src, int level);                 // 0x01 / 0x20: alpha blend at transparency `level`
void Blit_Over(Bmp_t* dst, const Bmp_t* src, int level);                  // 0xF0: blend ignoring the colour key / alpha
void Blit_Add(Bmp_t* dst, const Bmp_t* src, int level);                   // 0x02: saturating add
void Blit_Sub(Bmp_t* dst, const Bmp_t* src, int level);                   // 0x03: saturating subtract
void Blit_Mul(Bmp_t* dst, const Bmp_t* src, int level);                   // 0x04: multiply
void Blit_Fx06(Bmp_t* dst, const Bmp_t* src, int level);                  // 0x06: screen
void Blit_Fx07(Bmp_t* dst, const Bmp_t* src, int level);                  // 0x07: alpha erase
void Blit_Fx08(Bmp_t* dst, const Bmp_t* src, int level);                  // 0x08: overlay
void Blit_Fx09(Bmp_t* dst, const Bmp_t* src, int level);                  // 0x09: hard light
void Blit_Punch(Bmp_t* dst, const Bmp_t* src, int level);                 // 0x40: clear dst where src is set
void Blit_Fx41(Bmp_t* dst, const Bmp_t* src);                             // 0x41: clear the common rectangle
void Blit_Dim(Bmp_t* dst, const Bmp_t* src, int level);                   // 0x05 / 0xC0: src darkened by level
void Blit_Tint(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level); // 0xC1 (colour 0xFFFFFF) and filter kind 0: src blended toward colour
void Blit_FxFF(Bmp_t* dst, const Bmp_t* src, int level);                  // 0xFF: one channel of src (level 0 B, 1 G, 2 R, 3 alpha as gray)

// ---- special blitters used by the objects ---------------------------------
/* sprite drawing mode 1 ("90 58"): dst (RGB32) = lerp(a, b, ratio / 256)
 * drawn over dst with the pictures' alpha, at transparency level.  0 ok,
 * 9 a or b not ARGB32, 0xa dst not RGB32. */
int Blit_Crossfade(Bmp_t* dst, const Bmp_t* a, const Bmp_t* b, int ratio, int level);
// tmp = lerp(a, b, ratio / 256) in their common mode; 0 ok, 9 modes differ, 0xa tmp not 32-bit
int Blit_CrossfadeTo(Bmp_t* tmp, const Bmp_t* a, const Bmp_t* b, int ratio);
// the gray-map filter of the Filter object: colour over dst (RGB32) weighted by the map (blit_filter.c)
void Blit_TintByGray(Bmp_t* dst, uint32_t colour, const Bmp_t* gray, int param, int level);
void Blit_FilterKind1(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level);   // filter kind 1: add colour * level (a black colour dims)
void Blit_FilterKind2(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level);   // filter kind 2, "91 1D" mode 2: luminance tint
void Blit_FilterKind3(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level);   // filter kind 3, "91 1D" mode 1: blend toward src XOR colour
void Blit_TintKeepAlpha(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level); // 1.494 on: "91 1D" mode 3, blend toward colour keeping alpha
void Blit_AddColour(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level);     // 1.494 on: "91 1D" mode 4, add colour * level keeping alpha
/* "90 19" and background type 4: src placed at (x, y) of dst and drawn
 * through the GRAY8 map `gray` of its size, revealed where the map is
 * below the threshold set by param / level.  0 ok, 1 modes differ, 3 level
 * above 0x100, 4 no overlap, 7 gray not GRAY8, 8 gray and src differ in
 * size. */
int Blit_ThroughGray(Bmp_t* dst, int x, int y, const Bmp_t* src, const Bmp_t* gray, int param, int level);
int Blit_ThroughGrayScreen(Bmp_t* dst, int x, int y, const Bmp_t* src, const Bmp_t* gray, int param, int level);
/* "90 1A", effector mode 0, background type 6: src read through one or two
 * PM_VECTOR maps (vec2 may be NULL) mixed by level; amount != 0 samples
 * bilinearly.  0 ok, 1 modes differ, 3 level above 0x100, 0xc / 0xd vec1 /
 * vec2 is not a vector map of src's size. */
int Blit_Displace(Bmp_t* dst, const Bmp_t* src, const Bmp_t* vec1, const Bmp_t* vec2, int level, int amount);
/* "90 1B", effector mode 1, background type 7: horizontal box blur of
 * width 2 * level + 1; type 0 pads with black, 1 repeats the edge.  0 ok,
 * 3 level above 0x100, 0xe unknown type, 0xf mode or size mismatch. */
int Blit_Gradient(Bmp_t* dst, const Bmp_t* src, int type, int level);
/* ripple effects (effector mode 2, background type 8, sprite mode 4):
 * srcCrop read through the PM_VECDIST map and the ring table of
 * BmpMgr_RippleFill; srcFull is the whole picture the samples may stray
 * into.  0 ok, 1 modes differ, 0xc bad map; the Level variant blends onto
 * RGB32 only and answers 0xf for another destination mode. */
int Blit_Ripple(Bmp_t* dst, const Bmp_t* srcCrop, const Bmp_t* srcFull, const Bmp_t* vecdist, const void* table);
int Blit_RippleLevel(Bmp_t* dst, const Bmp_t* srcCrop, const Bmp_t* srcFull, const Bmp_t* vecdist, const void* table, int level);
// background type 11: mosaic of (amount + 1)-pixel blocks mixed in at `extra`; `style` is ignored; 0 ok, 1 modes differ
int Blit_Flip(Bmp_t* dst, const Bmp_t* src, int amount, int style, int extra);
int Blit_ToGray(Bmp_t* dst, const Bmp_t* src);                               // "92 18": luminance (times alpha) into a GRAY8; 0 ok, 9 bad source mode, 0xa dst not GRAY8
int Blit_InvertGray(Bmp_t* b);                                               // "92 19": 255 - g in place; 1 if GRAY8, 0 (nothing done) otherwise
void Blit_ColourThroughGray(Bmp_t* dst, const Bmp_t* gray, uint32_t colour); // paint colour with the GRAY8 map as alpha (glyph drawing)
/* rotate by angle (16.16 degrees) and zoom (16.16) about the destination
 * centre, with the source point (cx, cy) (16.16) at that centre; smooth
 * selects bilinear sampling.  0 ok, 1 modes differ, 0x13 zoom <= 0. */
int Blit_Rotate(Bmp_t* dst, const Bmp_t* src, int32_t zoom, int32_t angle, int32_t cx, int32_t cy, int smooth);
int Blit_RotateCentred(Bmp_t* dst, const Bmp_t* src, int32_t zoom, int32_t angle, int smooth); // "90 1D", background type 10: about the source centre
/* scale by (sx, sy) (16.16) so that the source point (hx, hy) (16.16)
 * lands on the destination point (cx, cy) (16.16), offset by (ox, oy)
 * (16.16).  0 ok, 1 modes differ, 0x13 a factor <= 0, 0x14 fullW or fullH
 * below 2.0.  Blit_Scale2 ("90 1C", background type 9) takes the
 * destination centre and half of (fullW, fullH) as the two points. */
int Blit_ScaleTo(Bmp_t* dst, const Bmp_t* src, int32_t ox, int32_t oy, int32_t fullW, int32_t fullH,
	int32_t sx, int32_t sy, int32_t cx, int32_t cy, int32_t hx, int32_t hy, int smooth);
int Blit_Scale2(Bmp_t* dst, const Bmp_t* src, int32_t ox, int32_t oy, int32_t fullW, int32_t fullH,
	int32_t sx, int32_t sy, int smooth);
int Blit_ApplyMask(Bmp_t* dst, const Bmp_t* src, const Bmp_t* gray); // "91 1E" and the sprite mask of "90 55": the GRAY8 map (or the alpha of an ARGB32 one) becomes the alpha of dst (ARGB32); 0 ok, 7 neither kind
/* sprite mode 3 ("90 5A"): src (ARGB32) revealed over dst (RGB32) where
 * the GRAY8 pattern is below the threshold of `progress` (0 .. 0x100)
 * widened by `mode`; the To variant writes the revealed part into an
 * ARGB32 picture instead.  Always 0; nothing is drawn for other modes. */
int Blit_GrayWipe(Bmp_t* dst, const Bmp_t* src, const Bmp_t* gray, int mode, int progress, int level);
int Blit_GrayWipeTo(Bmp_t* tmp, const Bmp_t* src, const Bmp_t* gray, int mode, int progress);
/* the rain screen: src (ARGB32) blended onto dst (RGB32) only where the
 * GRAY8 mask is non-zero; the To variant keeps those pixels in an ARGB32
 * picture and zeroes the rest.  Always 0. */
int Blit_Masked(Bmp_t* dst, const Bmp_t* src, const Bmp_t* gray, int level);
int Blit_MaskedTo(Bmp_t* tmp, const Bmp_t* src, const Bmp_t* gray);
/* "91 18" / "91 19", sprite mode 2, effector mode 3, the background
 * layers: the source point (cx16, cy16) lands on the destination point
 * (x16, y16); the picture is rotated by angle (16.16 degrees, clockwise)
 * and scaled by sx / sy (16.16) about that point.  level is the
 * transparency (0 opaque), smooth selects bilinear sampling.  0 ok, 0x13
 * zero scale.  Blit_Xform blends onto an RGB32 destination, Blit_XformCopy
 * writes into a picture of the source's mode. */
int Blit_Xform(Bmp_t* dst, int32_t x16, int32_t y16, const Bmp_t* src, int32_t cx16, int32_t cy16,
	int32_t angle, int32_t sx, int32_t sy, int level, int smooth);
int Blit_XformCopy(Bmp_t* dst, int32_t x16, int32_t y16, const Bmp_t* src, int32_t cx16, int32_t cy16,
	int32_t angle, int32_t sx, int32_t sy, int level, int smooth);
// the vertical-only transform of the zoomed frame presentation (sy 16.16; angle 0, sx 1.0); 32-bit pictures, no checks
void Blit_XformVscale(Bmp_t* dst, int32_t x16, int32_t y16, const Bmp_t* src, int32_t cx16, int32_t cy16, int32_t sy);
// sprite mode 5: rows shifted by a sine wave of `period` rows, `phase` and `amount` (16.16 of the width); always 0
int Blit_Skew(Bmp_t* dst, const Bmp_t* src, int32_t period, int32_t phase, int32_t amount);
// "91 1C": src scaled by the 16.16 factors and centred in dst (32-bit pictures); filtered / nearest neighbour
void Blit_Scale(Bmp_t* dst, const Bmp_t* src, int32_t sx16, int32_t sy16);
void Blit_ScaleNearest(Bmp_t* dst, const Bmp_t* src, int32_t sx16, int32_t sy16);
/* 1.494 on ("91 1B"): the source's pixels drawn toward its
 * centre - pixel (i, j) lands at ((w - 1) cx / 2 + i (1 - cx), (h - 1) cy / 2
 * + j (1 - cy)) with cx, cy the 16.16 gathering rates - and added to the
 * destination with saturation, each scaled by (256 - level) / 256 and its
 * bilinear weight; RGB32 on both sides, else nothing is drawn */
void Blit_GatherAdd(Bmp_t* dst, const Bmp_t* src, uint32_t cx, uint32_t cy, uint32_t level);


// ---- tuning switches of the original -------------------------------------
// The C blitters take no notice of the CPU; the switches are kept so that
// the start-up sequence of the graphics manager is the same.
void Blit_InitTables(void);       // build the MMX multiply tables (a flag here)
void Blit_SetSse(int on);         // the CPU has SSE
void Blit_SetSse2(int on);        // the CPU has SSE2
void Blit_SetTuning(int v);       // 1 for every CPU but Intel family 0xf
void Gfx_SetScreenMode(int mode); // gScreenMode, set once at start-up
void Gfx_SetBmpOption(int v);     // gBmpOption ("90 0F")
int Gfx_GetBmpOption(void);

#endif // BGI_GFX_BITMAP_H_
