/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bmpops.h - the bitmap instructions' implementation layer: routines that
 *            take bitmap slot numbers, look the bitmaps up in the global
 *            manager (gBmpMgr, bmpmgr.h) and call the blitters of
 *            bitmap.h.  Every "90 1x" / "91 1x" / "92 1x" bitmap
 *            instruction handler is a thin argument-popping shell around
 *            one of these.  Implemented in src/gfx/bmpops.c (slots and
 *            blits), bmpload.c (image files), bmptext.c (fonts and text),
 *            vecmaps.c (vector maps and bwef files) and tonecurve.c (the
 *            tone curves of 1.616 on).
 *
 * Result conventions: 0 success; small integers are per-routine error codes
 * (1 = no destination bitmap, 2 = no source bitmap in most of them, higher
 * numbers map the blitter's result); the loaders return 0x8000000n codes.
 * Several routines return a dead argument on an unexpected blitter result,
 * as the original does; those cases are marked in the sources.
 */
#ifndef BGI_GFX_BMPOPS_H_
#define BGI_GFX_BMPOPS_H_

#include "bgi/gfx/bitmap.h"

struct Thread;

// ---- loading -----------------------------------------------------------------------
/* "90 10": load an image file of an archive into a slot: a Windows BMP
 * (BmpOp_DecodeBmp) or the engine's raw image (BmpOp_PutRaw).  0 ok; the
 * decoder's 0x8000000n codes, with a raw image's "unsupported" as
 * 0x80000004 and "refused" as 0x80000008 */
int BmpOp_LoadFile(int slot, const char* arc, const char* name);
int BmpOp_LoadLocal(int slot, const char* path); // "92 1F": a BMP file of the file system; the decoder's result, -1 unreadable
/* a Windows BMP (uncompressed, 8/16/24/32 bpp) into a slot; 0 or
 * 0x80000001 not BMP, ..2 header, ..3 planes, ..4 depth, ..5 compressed,
 * ..6 empty, ..7 the manager refused */
int BmpOp_DecodeBmp(int slot, const void* data);
// the engine's own raw image (RawImageHeader) into a slot; 0 ok, 1 unsupported depth, 2 the manager refused
int BmpOp_PutRaw(int slot, const void* data);
int RawImage_Unpack(void* dst, const void* src);    // unpack a raw image into dst (format 0 or 1); always 1
void RawImage_Unfilter(void* dst, const void* src); // the delta-coded format 1 -> plain format 0

// ---- the manager forwarders -------------------------------------------------------------
int BmpOp_Put(int slot, int w, int h, int mode, const void* data); // "90 14": BmpMgr_PutPixels
int BmpOp_Get(void* dst, int unused, int size, int slot);          // "90 15": BmpMgr_GetPixels
int BmpOp_Create(int slot, int w, int h, int mode);                // "90 11": BmpMgr_Create
int BmpOp_Clear(int slot, uint32_t colour);                        // "90 13": BmpMgr_Clear
int BmpOp_Free(int slot);                                          // "90 12": BmpMgr_FreeSlot
int BmpOp_GetInfo(Bmp_t* out, int slot);                           // "90 16": BmpMgr_GetInfo
int BmpOp_ToGray(int dst, int src);                                // "92 18": BmpMgr_ToGray
int BmpOp_InvertGray(int slot);                                    // "92 19": BmpMgr_InvertGray

// ---- blits ------------------------------------------------------------------------------
/* "90 18": Bmp_Blit of slot src at (x, y) of slot dst; 0 ok, 1 / 2 no
 * destination / source, 3 incompatible modes, 4 unknown effect, 5 bad
 * level, 6 no overlap */
int BmpOp_Blit(int dst, int x, int y, int src, int effect, int level);
/* "90 19": Blit_ThroughGray of src at (x, y) of dst through the gray slot;
 * 0 ok, 1 / 2 / 3 no destination / source / gray, 4 modes differ, 5 gray
 * not GRAY8, 6 sizes differ, 7 no overlap, 8 bad level */
int BmpOp_BlitMask(int dst, int x, int y, int src, int gray, int param, int level);
uint32_t BmpOp_AlphaToScreenGray(int dst, int src, int src2, int x, int y, int ratio); // 1.494 on, "92 1A": the alpha of src as a gray bitmap
/* 1.616 on, the tone curves (src/gfx/tonecurve.c).  "90 CC": register
 * curve `id` from six words (the via point x, y of the R, G and B
 * curves), or remove it with a NULL `points`: 0 ok, 0x16 a point out of
 * range (nothing registered), 0x8000000f no curve to remove.  "90 CD":
 * dst becomes src through curve `curveId`, its monochrome mix (colour and
 * level 0 .. 256) and its colour film (effect 8 / 0x26, colour, level):
 * 0 ok, 0x80000009 dst cannot be made, 0x8000000a no src, 0x8000000f no
 * such curve, 0x80000011 src not 32-bit, 0x80000013 bad effect,
 * 0x80000014 bad film level, 0x80000015 bad monochrome level. */
uint32_t ToneCurve_Register(int32_t id, const int32_t* points);
void ToneCurve_FreeAll(void);
uint32_t BmpOp_ToneCurve(int dst, int src, int32_t curveId, uint32_t monoColour, int monoLevel, uint32_t filmColour,
	int effect, int filmLevel);
/* "90 1A": Blit_Displace of src through the vector slots (vec2 -1: none)
 * into dst; 0 ok, 1 / 2 / 4 / 6 no destination / source / vec1 / vec2,
 * 3 modes differ, 5 / 7 vec1 / vec2 is not a vector map, 8 bad level */
int BmpOp_Displace(int dst, int src, int vec1, int vec2, int level, int amount);
// "90 1B": Blit_Gradient of src into dst; 0 ok, 1 / 2 no bitmap, 3 mode / size mismatch, 4 unknown type, 5 bad level
int BmpOp_Gradient(int dst, int src, int type, int level);
int BmpOp_VecSine(int slot, int periodX, int phaseX, int ampX, int periodY, int phaseY, int ampY); // 1.494 on: "91 16"
/* "91 1C": create dst as src scaled by the 16.16 factors (Blit_Scale /
 * Blit_ScaleNearest); 0 ok, 1 dst could not be created, 2 no source, 3
 * source not 32-bit, 4 zero result size */
int BmpOp_Resample(int dst, int src, int32_t sx16, int32_t sy16, int smooth);
/* 1.494 on ("91 1D"): `src` into `dst` through colour mode
 * 0 copy, 1 blend toward src XOR colour, 2 luminance tint, 3 blend toward
 * the colour, 4 add the colour (`level` 0 .. 0x100); 0 ok, 1 no destination,
 * 2 no source, 6 bad mode */
int BmpOp_ColourMode(int dst, int src, int mode, uint32_t colour, int level);
/* 1.494 on ("91 1B"): Blit_GatherAdd of `src` into `dst`;
 * 0 ok, 1 no destination, 2 no source, 4 a rate above 1.0, 5 a level above 0x100 */
int BmpOp_GatherAdd(int dst, int src, uint32_t cx, uint32_t cy, uint32_t level);
/* "90 1C": the (sx, sy, sw, sh) part of src scaled into the (x, y, w, h)
 * rectangle of dst (Blit_Scale2, smooth); 0 ok, 1 / 2 no bitmap, 5 / 6 a
 * size below 2, 7 zero scale, 8 modes differ */
int BmpOp_Stretch(int dst, int x, int y, int w, int h, int src, int sx, int sy, int sw, int sh);
// "90 1D": Blit_RotateCentred (nearest sampling) of src into dst; 0 ok, 1 / 2 no bitmap, 3 modes differ, 4 zoom <= 0
int BmpOp_Rotate(int dst, int src, int32_t zoom, int32_t angle);
// "91 18" / "91 19": Blit_Xform / Blit_XformCopy of src into dst; 0 ok, 1 / 2 no bitmap, 3 zero scale
int BmpOp_Xform(int dst, int32_t x16, int32_t y16, int src, int32_t cx16, int32_t cy16, int32_t angle,
	int32_t sx, int32_t sy, int level, int smooth);
int BmpOp_XformCopy(int dst, int32_t x16, int32_t y16, int src, int32_t cx16, int32_t cy16, int32_t angle,
	int32_t sx, int32_t sy, int level, int smooth);
int BmpOp_ApplyGray(int dst, int gray, int x, int y);                               // "91 1E": the gray slot becomes the alpha of dst; 0 ok, 1 / 2 no bitmap, 3 not GRAY8
int BmpOp_CopyRect(int dst, int dx, int dy, int src, int sx, int sy, int w, int h); // "90 1E": raw copy of a part of src to (dx, dy); 0 ok, 1 / 2 no bitmap, 3 empty size
int BmpOp_CloneRect(int dst, int src, int x, int y, int w, int h);                  // "90 1F": dst created as the w x h part of src at (x, y); 0 ok, 1 refused, 2 no source, 3 empty size
int BmpOp_Duplicate(int dst, int src);                                              // "91 1F": dst created as a copy of src (base size included); 0 ok, 1 refused, 2 no source
int BmpOp_GetPixel(void* out, int slot, int x, int y);                              // 1.529 on, "92 17"

// ---- text -------------------------------------------------------------------------------
const char* FontNameByNo(int no);                                   // 0 MS Gothic, 1 MS Mincho, the registered faces, else NULL
int FontNames_Register(const char* name, int kind);                 // 1.69/472 on, "B0 C0" / "B0 C1": the face's number
int FontNames_AddFile(const char* file);                            // "B0 C2": a font file of the game directory
int FontNames_AddFileFromArc(const char* arc, const char* file);    // "B0 C3" (1.494 on): one of an archive
void FontNames_SetFallback(const char* face, const char* fallback); // "B0 C7" (1.535 on)
const char* FontNames_Resolve(const char* face);                    // the face, or its fallback when not installed
/* open font number `fontNo` in the manager's cache; 0 ok, 0x80000001 bad
 * size, 0x80000002 bad width, 0x80000003 unknown font number (the
 * original returns `bold` on any other cache result) */
int BmpOp_FontOpen(int* outHandle, int fontNo, int size, int widthPct, int bold);
/* "92 1C" / "92 1D": plain text into a bitmap, unwrapped (*measure = the
 * widest line) or wrapped at the right edge with a line spacing in percent
 * (*measure = the line count); 0 ok, the BmpOp_FontOpen codes, 0x80000004
 * no bitmap */
int BmpOp_Text(int bmp, int x, int y, const char* str, int fontNo, int size, int widthPct, int bold, int prop,
	uint32_t colour, int32_t* measure);
int BmpOp_TextEx(int bmp, int x, int y, const char* str, int fontNo, int size, int widthPct, int bold, int prop,
	uint32_t colour, int spacingPct, int32_t* measure);
// "92 1E": text through a temporary 1-bit font; 0 ok, 0x80000001 bad size, 0x80000003 unknown font, 0x80000004 no bitmap
int BmpOp_TextMono(int bmp, int x, int y, const char* str, int fontNo, int size, int bold, int spacing,
	uint32_t colour, int32_t* outWidth);
// "91 9C" / "91 9D" / "92 9C": the rich text layout into a bitmap; the BmpOp_Text codes
int BmpOp_DrawText(int bmp, int32_t* outLines, int x, int y, const char* str, int parseTags, const char* tags,
	int fontNo, int size, int widthPct, int bold, int prop, int hang, int spacing, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5]);
// "7D": hex dump of `len` bytes at `data` into a bitmap
int Vm_DebugText(uint32_t bmp, int x, int y, const void* data, uint32_t len, int fontSize, uint32_t colour,
	struct Thread* t);

// ---- vector (displacement) map generators ----------------------------------------------------
/* The Vec_* / VecDist_* routines fill a PM_VECTOR / PM_VECDIST bitmap
 * (0x80000003 when it is not one); the BmpOp_* routines take a slot
 * instead ("91 10" .. "91 15", "92 10" / "92 11"; 0x80000001 for an empty
 * slot).  The fields are described in vecmaps.c. */
int Vec_StretchRange(Bmp_t* b, int ox, int oy, int rw, int rh);
int BmpOp_VecStretchRange(int slot, int ox, int oy, int rw, int rh);
int Vec_Random(Bmp_t* b, int amplitude);
int BmpOp_VecRandom(int slot, int amplitude);
int Vec_FromHeight(Bmp_t* vec, const Bmp_t* height, int32_t scale);
int Vec_Ripple(Bmp_t* b, int cx, int cy, int period, int phase, int amplitude);
int BmpOp_VecRipple(int slot, int cx, int cy, int period, int phase, int amplitude);
int Vec_Polar(Bmp_t* b, int cx, int cy, int32_t angle16, int k);
int BmpOp_VecPolar(int slot, int cx, int cy, int32_t angle16, int k);
int Vec_BendAngle(Bmp_t* b, int cx, int cy, int32_t angle16, int radius);
int BmpOp_VecBendAngle(int slot, int cx, int cy, int32_t angle16, int radius);
int Vec_Bend(Bmp_t* b, int cx, int cy, int radius, int height);
int BmpOp_VecBend(int slot, int cx, int cy, int radius, int height);
int VecDist_Radial(Bmp_t* b, int type, int cx, int cy, int range);
int BmpOp_VecDistRadial(int slot, int type, int cx, int cy, int range);
int VecDist_Linear(Bmp_t* b, int type);
int BmpOp_VecDistLinear(int slot, int type);

// ---- "bwef" files (particle wind data, "C0 F0") ---------------------------------------------
typedef struct BwefEntry // what ParseBwef writes, 8 bytes per entry
{
	uint32_t offset; // file dword + base
	uint32_t common; // the header's dword at 0x18
} BwefEntry_t;
/* parse a bwef file of `size` bytes into out[] (the caller's array);
 * 0 ok, 0x80000002 bad magic, 0x80000003 size mismatch */
int ParseBwef(BwefEntry_t* out, int32_t* outCount, const void* data, uint32_t size, uint32_t base);
// load and parse a bwef file of an archive; the ParseBwef codes, 0x80000001 for a missing file
int LoadBwef(const char* arc, const char* name, BwefEntry_t* out, int32_t* outCount, uint32_t base);

#endif // BGI_GFX_BMPOPS_H_
