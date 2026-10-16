/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ops_gfx2.c - the "92 xx" graphics instruction family: ripple definitions,
 * the vector & distance map generators, grayscale derivation, plain font
 * output into bitmaps, the window frame / text layer blits and the styled
 * (shadowed) text output variants, plus the additions of the later builds
 * (bitmap base sizes and pixel reads, the link markup's font and colour,
 * private character pictures, the archive movie player and the video
 * textures, none of which play anything here).
 *
 * Same conventions as ops_gfx0.c: operands are popped in the original
 * order, "a, b → r" means b is on top of the stack, the Check* validators
 * raise and never return, result codes map to script errors.  Handlers
 * that install a wait object (92 90, 92 F1) return scheduler code 2 (end of
 * turn, the thread blocked until the object completes); everything else
 * returns 0.  The table is declared in bgi/vm.h; the window-layer handlers
 * are exported because the 1.58 .. 1.64 tables of ops_gfx1.c share them.
 */
#include "bgi/vm.h"
#include "bgi/wait.h"
#include "bgi/waitobj.h"
#include "bgi/error.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/gfx.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/text.h"
#include "bgi/file.h"
#include "bgi/codec.h"

VmHandler_t vm_optable_92[256]; // the "92 xx" dispatch table, filled by Vm_Optable92Init

// raise a script error with a formatted message; never returns
#define GFX_ERROR(t, ...)           \
	do                              \
	{                               \
		char msg_[0x104];           \
		sprintf(msg_, __VA_ARGS__); \
		ScriptError(msg_, (t));     \
	} while(0)

// -------------------------------------------------------------------------
// 92 00, 92 01 : ripple definitions
// -------------------------------------------------------------------------

/* Define ripple pattern `no` (0 .. 7, else a script error) for the ripple
 * effector ("91 67"): one sine cycle of `period` steps and `amplitude`
 * followed by `count` - 1 silent cycles, repeated for `rings` rings.  A
 * zero period, count or rings counts as 1. */
static int Opcode_Gfx2_DefineRipple(Thread_t* t) // 92 00: no, period, amplitude, count, rings →
{
	int rings = (int)Thread_Pop(t), count = (int)Thread_Pop(t);
	int amplitude = (int)Thread_Pop(t), period = (int)Thread_Pop(t), no = (int)Thread_Pop(t);
	if(GfxCall_DefineRipple(no, period, amplitude, count, rings) == 0x10)
		GFX_ERROR(t, MSG_BAD_RIPPLE_NO, no);
	return 0;
}

/* 92 01: no, period, amplitude, fadeIn, fadeOut, count, rings →
 * As "92 00" with the cycle grown over `fadeIn` cycles (amplitude / 2^k)
 * and decayed over `fadeOut` cycles; the silent cycles lead each ring. */
static int Opcode_Gfx2_DefineRippleEx(Thread_t* t)
{
	int rings = (int)Thread_Pop(t), count = (int)Thread_Pop(t);
	int fadeOut = (int)Thread_Pop(t), fadeIn = (int)Thread_Pop(t);
	int amplitude = (int)Thread_Pop(t), period = (int)Thread_Pop(t), no = (int)Thread_Pop(t);
	if(GfxCall_DefineRippleEx(no, period, amplitude, fadeIn, fadeOut, count, rings) == 0x10)
		GFX_ERROR(t, MSG_BAD_RIPPLE_NO, no);
	return 0;
}

// -------------------------------------------------------------------------
// 92 10, 92 11 : vector & distance maps
// -------------------------------------------------------------------------

/* Fill the vector-distance map in `slot` (pixel mode PM_VECDIST) with a
 * field radiating from (cx, cy); `type` 0 or 1 (1 swaps the direction
 * components), `range` the distance the field wraps at (0: none).  Script
 * errors: no bitmap in the slot, not a vector-distance map, a bad type. */
static int Opcode_Gfx2_VecDistRadial(Thread_t* t) // 92 10: slot, type, cx, cy, range →
{
	int range = (int)Thread_Pop(t), cy = (int)Thread_Pop(t), cx = (int)Thread_Pop(t);
	int type = (int)Thread_Pop(t), slot = (int)Thread_Pop(t);
	switch(BmpOp_VecDistRadial(slot, type, cx, cy, range))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BMP_DST_NOT_EXIST, slot); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_VDMAP_DST_NOT_VDMAP, slot); break;
		case(int)0x80000009: GFX_ERROR(t, MSG_BAD_RIPPLE_TYPE, type); break;
		default: break;
	}
	return 0;
}

/* The vector-distance field of the straight wipes: `type` 0 / 2 point
 * right, 1 / 3 down (above 3 is a script error, as are a missing bitmap
 * and a slot that is not a vector-distance map). */
static int Opcode_Gfx2_VecDistLinear(Thread_t* t) // 92 11: slot, type →
{
	int type = (int)Thread_Pop(t), slot = (int)Thread_Pop(t);
	switch(BmpOp_VecDistLinear(slot, type))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BMP_DST_NOT_EXIST, slot); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_VDMAP_DST_NOT_VDMAP, slot); break;
		case(int)0x8000000a: GFX_ERROR(t, MSG_BAD_SHAKE_TYPE, type); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 92 18 .. 92 1F : grayscale derivation, plain text, local files
// -------------------------------------------------------------------------

// make dst a new 8-bit gray bitmap of src's size holding src's luminance (times alpha for ARGB32)
static int Opcode_Gfx2_ToGray(Thread_t* t) // 92 18: dst, src →
{
	int src = (int)Thread_Pop(t), dst = (int)Thread_Pop(t);
	switch(BmpOp_ToGray(dst, src))
	{
		case 9: GFX_ERROR(t, MSG_GRAY_SRC_INVALID, src); break;
		case 10: GFX_ERROR(t, MSG_GRAY_DST_INVALID, dst); break;
		default: break;
	}
	return 0;
}

// invert an 8-bit gray bitmap in place; ok is 1, or 0 for a missing bitmap or one that is not gray
static int Opcode_Gfx2_InvertGray(Thread_t* t) // 92 19: slot → ok
{
	Thread_Push(t, BmpOp_InvertGray((int)Thread_Pop(t)) == 0);
	return 0;
}

/* The result map of the bitmap text calls: 0x80000001 bad font size,
 * 0x80000002 bad font width, 0x80000003 bad font number, 0x80000004 no
 * bitmap in the slot. */
static void Gfx2_TextResult(Thread_t* t, int r, int bmp, int fontNo, int size, int widthPct)
{
	switch(r)
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_FONT_SIZE_INVALID, size); break;
		case(int)0x80000002: GFX_ERROR(t, MSG_FONT_WIDTH_INVALID, widthPct); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_FONT_NO_INVALID, fontNo); break;
		case(int)0x80000004: GFX_ERROR(t, MSG_BMP_NOT_EXIST, bmp); break;
		default: break;
	}
}

/* 92 1C: bmp, x, y, str, fontNo, size, widthPct, bold, prop, colour → width
 * Plain (unwrapped, no markup) text into the bitmap at (x, y) with the
 * font given by number, size in pixels and width in percent; `prop` packs
 * the glyphs by their ink extent.  The widest line in pixels is pushed. */
static int Opcode_Gfx2_Text(Thread_t* t)
{
	int32_t measure;
	uint32_t colour = Thread_Pop(t);
	int prop = (int)Thread_Pop(t), bold = (int)Thread_Pop(t), widthPct = (int)Thread_Pop(t);
	int size = (int)Thread_Pop(t), fontNo = (int)Thread_Pop(t);
	const char* str = (const char*)PopPtr(t);
	int y = (int)Thread_Pop(t), x = (int)Thread_Pop(t), bmp = (int)Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	CheckFontNo(fontNo, t);
	Gfx2_TextResult(t, BmpOp_Text(bmp, x, y, str, fontNo, size, widthPct, bold, prop, colour, &measure), bmp, fontNo,
		size, widthPct);
	Thread_Push(t, (uint32_t)measure);
	return 0;
}

/* 92 1D: bmp, x, y, str, fontNo, size, widthPct, bold, prop, colour, spacingPct → lines
 * As "92 1C", but the text wraps at the bitmap's right edge with a line
 * spacing of `spacingPct` percent of the size, and the number of lines is
 * pushed instead of the width. */
static int Opcode_Gfx2_TextEx(Thread_t* t)
{
	int32_t measure;
	int spacingPct = (int)Thread_Pop(t);
	uint32_t colour = Thread_Pop(t);
	int prop = (int)Thread_Pop(t), bold = (int)Thread_Pop(t), widthPct = (int)Thread_Pop(t);
	int size = (int)Thread_Pop(t), fontNo = (int)Thread_Pop(t);
	const char* str = (const char*)PopPtr(t);
	int y = (int)Thread_Pop(t), x = (int)Thread_Pop(t), bmp = (int)Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	CheckFontNo(fontNo, t);
	Gfx2_TextResult(t, BmpOp_TextEx(bmp, x, y, str, fontNo, size, widthPct, bold, prop, colour, spacingPct, &measure),
		bmp, fontNo, size, widthPct);
	Thread_Push(t, (uint32_t)measure);
	return 0;
}

/* 92 1E: bmp, x, y, str, fontNo, size, bold, spacing, colour → width
 * Text through a temporary 1-bit (unantialiased) font with `spacing`
 * extra pixels per character; the width drawn is pushed. */
static int Opcode_Gfx2_TextMono(Thread_t* t)
{
	int32_t width;
	uint32_t colour = Thread_Pop(t);
	int spacing = (int)Thread_Pop(t), bold = (int)Thread_Pop(t), size = (int)Thread_Pop(t);
	int fontNo = (int)Thread_Pop(t);
	const char* str = (const char*)PopPtr(t);
	int y = (int)Thread_Pop(t), x = (int)Thread_Pop(t), bmp = (int)Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	CheckFontNo(fontNo, t);
	switch(BmpOp_TextMono(bmp, x, y, str, fontNo, size, bold, spacing, colour, &width))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_FONT_SIZE_INVALID, size); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_FONT_NO_INVALID, fontNo); break;
		case(int)0x80000004: GFX_ERROR(t, MSG_BMP_NOT_EXIST, bmp); break;
		default: break;
	}
	Thread_Push(t, (uint32_t)width);
	return 0;
}

/* Load a Windows BMP file from the file system (not an archive) into the
 * slot.  r is the decoder's result: 0 ok, 0x80000001 not a BMP, ..2 bad
 * header, ..3 planes, ..4 depth, ..5 compressed, ..6 empty, ..7 the slot
 * refused; -1 when the file cannot be read. */
static int Opcode_Gfx2_LoadLocal(Thread_t* t) // 92 1F: slot, path → r
{
	const char* path = (const char*)PopPtr(t);
	int slot = (int)Thread_Pop(t);
	Thread_Push(t, (uint32_t)BmpOp_LoadLocal(slot, path));
	return 0;
}

// -------------------------------------------------------------------------
// 92 88 .. 92 8E : window layers
// -------------------------------------------------------------------------

// show or hide the window's frame layer ("90 8C" of 1.58 .. 1.64)
int Opcode_Gfx2_WindowShowFrame(Thread_t* t) // 92 88: h, on →
{
	int on = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_WindowShowFrame(h, on))
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

/* The shared body of "92 89" (into the frame layer) and "92 8D" (into the
 * text layer): h, x, y, bmp, effect, level →.  Blit bitmap `bmp` at (x, y)
 * of the layer with the blend `effect` and `level` (both validated).
 * Script errors: no bitmap in the slot, a bitmap whose pixel mode does not
 * match the screen's, an unknown window; a window without that layer is
 * silently ignored. */
static int Gfx2_WindowDraw(Thread_t* t, int toText)
{
	int level = (int)Thread_Pop(t), effect = (int)Thread_Pop(t), bmp = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t), x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	CheckEffectMode(effect, t);
	CheckAlpha(level, t);
	switch(toText ? GfxCall_WindowDrawToText(h, x, y, bmp, effect, level)
				  : GfxCall_WindowDrawToFrame(h, x, y, bmp, effect, level))
	{
		case 1: GFX_ERROR(t, MSG_BMP_NOT_EXIST, bmp); break;
		case 2: GFX_ERROR(t, MSG_BMP_SCREEN_MODE_MISMATCH, bmp); break;
		case -1: ScriptError(MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 0;
}

int Opcode_Gfx2_WindowDrawToFrame(Thread_t* t) // 92 89: h, x, y, bmp, effect, level → ("90 8D" of 1.58 .. 1.64)
{
	return Gfx2_WindowDraw(t, 0);
}

int Opcode_Gfx2_WindowDrawToText(Thread_t* t) // 92 8D: h, x, y, bmp, effect, level → ("91 8D" of 1.58 .. 1.64)
{
	return Gfx2_WindowDraw(t, 1);
}

static int Opcode_Gfx2_WindowFillFrame(Thread_t* t) // 92 8A: h, colour →
{
	uint32_t colour = Thread_Pop(t), h = Thread_Pop(t);
	if(GfxCall_WindowFillFrame(h, colour) == -1)
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

// show or hide the window's text layer ("91 8C" of 1.58 .. 1.64)
int Opcode_Gfx2_WindowShowText(Thread_t* t) // 92 8C: h, on →
{
	int on = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_WindowShowText(h, on))
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

// clear the window's text: cursor home, text layer cleared, items off, sub-sprites gone ("91 8E" of 1.58 .. 1.64)
int Opcode_Gfx2_WindowClear(Thread_t* t) // 92 8E: h →
{
	if(!GfxCall_WindowClear(Thread_Pop(t)))
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

// -------------------------------------------------------------------------
// 92 90 .. 92 9C : text output with an explicit shadow style
// -------------------------------------------------------------------------

/* 92 90: h, str, colour, rubyOn, rubyColour, hang, shadowOn,
 * shadowDx, shadowDy, shadowColour, shadowLevel, instant, waitAtEnd,
 * allowSkip, allowKeys →
 * The rich text output of "91 90" (StartTextOut mode 1: the rich class
 * or its vertical variant by the window's draw style) with its own ruby
 * colour and shadow (on / off, offset in pixels, colour, level) instead of
 * the current style; the thread blocks until the output completes. */
static int Opcode_Gfx2_TextOutStyled(Thread_t* t)
{
	int32_t style[5];
	int allowKeys = (int)Thread_Pop(t), allowSkip = (int)Thread_Pop(t), waitAtEnd = (int)Thread_Pop(t);
	int instant = (int)Thread_Pop(t), shadowLevel = (int)Thread_Pop(t);
	uint32_t shadowColour = Thread_Pop(t);
	int shadowDy = (int)Thread_Pop(t), shadowDx = (int)Thread_Pop(t), shadowOn = (int)Thread_Pop(t);
	int hang = (int)Thread_Pop(t);
	uint32_t rubyColour = Thread_Pop(t);
	int rubyOn = (int)Thread_Pop(t);
	uint32_t colour = Thread_Pop(t);
	const char* str = (const char*)PopPtr(t);
	uint32_t h = Thread_Pop(t);
	int r;
	Text_MakeStyle(style, shadowOn, shadowDx, shadowDy, shadowColour, shadowLevel);
	r = StartTextOut(t, h, str, colour, rubyOn, rubyColour, hang, 0, style, instant, waitAtEnd, allowSkip, allowKeys, 1);
	if(r == (int)0x80000001)
		ScriptError(MSG_WINDOW_NO_FONT, t);
	else if(r == -1)
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	return 2;
}

/* 92 91: h, str, colour, parseTags, rubyColour, hang, shadowOn,
 * shadowDx, shadowDy, shadowColour, shadowLevel →
 * The immediate text draw of "91 91" with its own ruby colour and
 * shadow.  Script errors: the window has no font, an unknown window. */
static int Opcode_Gfx2_DrawTextStyled(Thread_t* t)
{
	int32_t style[5];
	int shadowLevel = (int)Thread_Pop(t);
	uint32_t shadowColour = Thread_Pop(t);
	int shadowDy = (int)Thread_Pop(t), shadowDx = (int)Thread_Pop(t), shadowOn = (int)Thread_Pop(t);
	int hang = (int)Thread_Pop(t);
	uint32_t rubyColour = Thread_Pop(t);
	int parseTags = (int)Thread_Pop(t);
	uint32_t colour = Thread_Pop(t);
	const char* str = (const char*)PopPtr(t);
	uint32_t h = Thread_Pop(t);
	int r;
	Text_MakeStyle(style, shadowOn, shadowDx, shadowDy, shadowColour, shadowLevel);
	r = GfxCall_WindowDrawText(h, str, parseTags, hang, colour, rubyColour, style);
	if(r == 1)
		ScriptError(MSG_WINDOW_NO_FONT, t);
	else if(r == -1)
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

/* 92 9C: bmp, x, y, str, colour, parseTags, tags, rubyColour,
 * fontNo, size, widthPct, bold, prop, hang, spacing, shadowOn, shadowDx,
 * shadowDy, shadowColour, shadowLevel, unused → lines
 * The rich text layout into a bitmap of "91 9C" with its own ruby colour
 * and shadow; the last operand is popped and ignored.  The number of
 * lines is pushed. */
static int Opcode_Gfx2_BmpDrawTextStyled(Thread_t* t)
{
	int32_t style[5], lines;
	int shadowLevel, shadowDy, shadowDx, shadowOn, spacing, hang, prop, bold, widthPct, size, fontNo, parseTags;
	uint32_t shadowColour, rubyColour, colour;
	const char *tags, *str;
	int y, x, bmp, r;
	Thread_Pop(t);
	shadowLevel = (int)Thread_Pop(t);
	shadowColour = Thread_Pop(t);
	shadowDy = (int)Thread_Pop(t);
	shadowDx = (int)Thread_Pop(t);
	shadowOn = (int)Thread_Pop(t);
	spacing = (int)Thread_Pop(t);
	hang = (int)Thread_Pop(t);
	prop = (int)Thread_Pop(t);
	bold = (int)Thread_Pop(t);
	widthPct = (int)Thread_Pop(t);
	size = (int)Thread_Pop(t);
	fontNo = (int)Thread_Pop(t);
	rubyColour = Thread_Pop(t);
	tags = (const char*)PopPtr(t);
	parseTags = (int)Thread_Pop(t);
	colour = Thread_Pop(t);
	str = (const char*)PopPtr(t);
	y = (int)Thread_Pop(t);
	x = (int)Thread_Pop(t);
	bmp = (int)Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	CheckFontNo(fontNo, t);
	Text_MakeStyle(style, shadowOn, shadowDx, shadowDy, shadowColour, shadowLevel);
	r = BmpOp_DrawText(bmp, &lines, x, y, str, parseTags, tags, fontNo, size, widthPct, bold, prop, hang, spacing, colour,
		rubyColour, style);
	Gfx2_TextResult(t, r, bmp, fontNo, size, widthPct);
	Thread_Push(t, (uint32_t)lines);
	return 0;
}

// -------------------------------------------------------------------------

// 92 37 (1.69/472 on): h, x, y, z →; the second 16.16 offset added to the fixed position ("91 36" elsewhere)
static int Opcode_Gfx2_ObjSetFixedOffset2(Thread_t* t)
{
	int32_t z = (int32_t)Thread_Pop(t), y = (int32_t)Thread_Pop(t), x = (int32_t)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_ObjSetFixedOffset(h, 2, x, y, z))
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

/* 92 14 (1.494 on): name, arc →
 * Preload an image file into that build's decoded-image cache, which
 * "90 10" consults first; "92 15" empties the cache.  The cache only saves
 * time: here the file is read and checked (a script error names a file
 * that is not an image) and nothing is kept. */
static int Opcode_Gfx2_PreloadImage(Thread_t* t)
{
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	uint8_t* buf = (uint8_t*)BGI_Alloc(0x2000000);
	uint32_t n = LoadFile(buf, arc, name);
	int ok = n >= 0x10 && (BGI_IsCbg(buf) || ((const RawImageHeader_t*)buf)->bpp % 8 == 0);
	BGI_Free(buf);
	if(!ok)
		GFX_ERROR(t, MSG_NOT_BG_FILE, arc, name);
	return 0;
}

/* 92 12 (1.535 on): bmp, w, h → valid
 * Set the base size of a bitmap slot (the design size an image file
 * declares, what "92 16" reads); 1 for a valid slot number, 0 otherwise. */
static int Opcode_Gfx2_BmpSetBaseSize(Thread_t* t)
{
	int h = (int)Thread_Pop(t), w = (int)Thread_Pop(t), bmp = (int)Thread_Pop(t);
	Thread_Push(t, (uint32_t)BmpMgr_SetBaseSize(gBmpMgr, bmp, w, h));
	return 0;
}

/* 92 16 (1.529 on): out, bmp → valid
 * The base size of the bitmap slot into out[0], out[1] (0, 0 until an
 * image sets it, -1, -1 after the slot is freed); 1 for a valid slot
 * number, 0 otherwise.  The slot need not hold a bitmap. */
static int Opcode_Gfx2_BmpBaseSize(Thread_t* t)
{
	int bmp = (int)Thread_Pop(t);
	int32_t* out = (int32_t*)PopPtr(t);
	Thread_Push(t, (uint32_t)BmpMgr_GetBaseSize(gBmpMgr, out, bmp));
	return 0;
}

/* 92 17 (1.529 on): out, bmp, x, y → code
 * The pixel at (x, y) of the bitmap into the 4 bytes at out (zeroed
 * first; as many bytes as the pixel has).  code: 0 ok, 1 no bitmap, 2 more
 * than 4 bytes per pixel, 3 (x, y) outside the bitmap. */
static int Opcode_Gfx2_BmpGetPixel(Thread_t* t)
{
	int y = (int)Thread_Pop(t), x = (int)Thread_Pop(t), bmp = (int)Thread_Pop(t);
	void* out = PopPtr(t);
	Thread_Push(t, (uint32_t)BmpOp_GetPixel(out, bmp, x, y));
	return 0;
}

static int Opcode_Gfx2_PreloadClear(Thread_t* t) // 92 15 (1.494 on): → ; empty the image cache (nothing to do here)
{
	BGI_UNUSED(t);
	return 0;
}

/* 92 97 (1.616 on): fontNo, size, widthPct, dx, dy, bold, shadow →
 * The ruby font of "91 97" with two more overrides: the bold flag and
 * the shadow colour of the ruby glyphs (-1 each: the base font's / the
 * style's). */
static int Opcode_Gfx2_SetRubyFontEx(Thread_t* t)
{
	int32_t shadow = (int32_t)Thread_Pop(t), bold = (int32_t)Thread_Pop(t);
	int dy = (int)Thread_Pop(t), dx = (int)Thread_Pop(t), width = (int)Thread_Pop(t), size = (int)Thread_Pop(t);
	int fontNo = (int)Thread_Pop(t);
	const char* name = FontNameByNo(fontNo);
	if(name && strlen(name) >= 0x100)
		GFX_ERROR(t, MSG_BAD_FONT_NO, fontNo);
	Text_SetRubyFont(name, size, width, dx, dy);
	gRubyBold = bold;
	gRubyShadow = shadow;
	return 0;
}

/* 92 9D (1.529 on): face, size, width, bold, italic → code
 * The font the "<l>" link markup switches to (an empty face keeps the
 * text's own).  code: 0 ok, 1 bad size (not 4 .. 200), 2 bad width (not
 * 25 .. 200), 3 the face cannot be opened, -1 anything else. */
static int Opcode_Gfx2_SetLinkFont(Thread_t* t)
{
	int italic = (int)Thread_Pop(t), bold = (int)Thread_Pop(t), width = (int)Thread_Pop(t), size = (int)Thread_Pop(t);
	const char* face = (const char*)PopPtr(t);
	uint32_t code;
	switch(Text_SetLinkFont(face, size, width, bold, italic))
	{
		case 0: code = 0; break;
		case(int)0x80000004: code = 1; break;
		case(int)0x80000005: code = 2; break;
		case(int)0x80000006: code = 3; break;
		default: code = 0xffffffffu; break;
	}
	Thread_Push(t, code);
	return 0;
}

/* 92 9E (1.529 on): out → n
 * The links the last horizontal layout registered (TextLink_t each: the
 * text and the position of its first glyph) into `out`, and forgotten;
 * their count is pushed. */
static int Opcode_Gfx2_TakeLinks(Thread_t* t)
{
	TextLink_t* out = (TextLink_t*)PopPtr(t);
	Thread_Push(t, (uint32_t)TextLinks_Take(out));
	return 0;
}

// 92 9F (1.529 on): colour →; the colour of link text (-1: the text's own)
static int Opcode_Gfx2_SetLinkColour(Thread_t* t)
{
	Text_SetLinkColour((int32_t)Thread_Pop(t));
	return 0;
}

/* 92 99 (1.653 on): out, count, str, fontNo, size, widthPct, bold → r
 * The ABC widths of the characters of `str` in the numbered font (three
 * words each: blank columns before the ink, the ink width, blank columns
 * after it) into `out` (0 counts only) and their number into *count;
 * cnfgsmpltxt2._bp lays its font sample out by them.  r is 0, or the
 * font's error (0x80000001 bad size, ..2 bad width, ..3 bad number) -
 * pushed, not raised. */
static int Opcode_Gfx2_TextAbcWidths(Thread_t* t)
{
	int bold = (int)Thread_Pop(t), widthPct = (int)Thread_Pop(t), size = (int)Thread_Pop(t);
	int fontNo = (int)Thread_Pop(t);
	const char* str = (const char*)PopPtr(t);
	int32_t* count = (int32_t*)PopPtr(t);
	int32_t* out = (int32_t*)PopPtr(t);
	int handle, r = BmpOp_FontOpen(&handle, fontNo, size, widthPct, bold);
	if(r == 0)
		Text_AbcWidths(out, count, str, handle);
	Thread_Push(t, (uint32_t)r);
	return 0;
}

/* 92 9B (1.640 on): no, out →
 * A parameter of the last rich text layout into a bitmap ("91 9C" / "91
 * 9D" / "92 9C") into *out: 0x100 the pen position after its last
 * character (two words), 0x101 the text colour in effect there (the colour
 * tags applied).  snsctrl._bp continues a message where the last one
 * ended.  Any other number is a script error. */
static int Opcode_Gfx2_TextLayoutParam(Thread_t* t)
{
	int32_t* out = (int32_t*)PopPtr(t);
	uint32_t no = Thread_Pop(t);
	switch(no)
	{
		case 0x100:
			out[0] = gTextLastPos[0];
			out[1] = gTextLastPos[1];
			break;
		case 0x101: out[0] = (int32_t)gTextLastColour; break;
		default: GFX_ERROR(t, MSG_BAD_TEXT_PARAM_NO, no);
	}
	return 0;
}

/* 92 98 (1.588 on): code, bmp, x, y, w, h →
 * Give the private text code 0xFF01 .. 0xFFFF its own picture: the w x h
 * region of bitmap `bmp` at (x, y) (bmp = -1 drops the picture).  The
 * picture takes precedence over the glyph sheet's cell when the text is
 * drawn.  Script errors: a bad bitmap, a code outside the range, a bad
 * size. */
static int Opcode_Gfx2_TextSetCharImage(Thread_t* t)
{
	int h = (int)Thread_Pop(t), w = (int)Thread_Pop(t), y = (int)Thread_Pop(t), x = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	uint32_t code = Thread_Pop(t);
	switch(Text_SetCharImage(code, bmp, x, y, w, h))
	{
		case(int)0x80000002: GFX_ERROR(t, MSG_BAD_BITMAP, bmp);
		case(int)0x80000006: GFX_ERROR(t, MSG_BAD_CHAR_CODE, code);
		case(int)0x80000007: GFX_ERROR(t, MSG_BAD_IMAGE_SIZE, w, h);
		default: return 0;
	}
}

/* 92 1A (1.494 on): dst, src, src2, x, y, ratio →
 * dst becomes a gray bitmap of the back buffer's size holding, at (x, y),
 * the alpha of src (a 32-bit bitmap), blended with the alpha of src2 by
 * ratio / 256 when src2 is given (-1: none), and 0 everywhere else
 * (BmpOp_AlphaToScreenGray).  Script errors: dst cannot be made, a
 * missing source, sources of different pixel modes. */
static int Opcode_Gfx2_AlphaToScreenGray(Thread_t* t)
{
	int ratio = (int)Thread_Pop(t), y = (int)Thread_Pop(t), x = (int)Thread_Pop(t);
	int src2 = (int)Thread_Pop(t), src = (int)Thread_Pop(t), dst = (int)Thread_Pop(t);
	switch(BmpOp_AlphaToScreenGray(dst, src, src2, x, y, ratio))
	{
		case 0x80000009u: GFX_ERROR(t, MSG_GEN_DST_INVALID, dst); break;
		case 0x8000000au: GFX_ERROR(t, MSG_GEN_SRC_INVALID, src); break;
		case 0x8000000bu: GFX_ERROR(t, MSG_GEN_SRC_INVALID, src2); break;
		case 0x8000000cu: GFX_ERROR(t, MSG_GEN_SRC_MODE_MISMATCH, src, src2); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 92 F2, 92 F4, 92 F5 : video textures (1.494 on)
// -------------------------------------------------------------------------

/* 92 F2 (1.494 on): name, arc, a, b, bmp → code
 * Attach a movie (DirectShow in the original) to a bitmap slot as a video
 * texture (see "91 F0 .. F7"): 0 ok, 1 the player could not be created,
 * 2 / 3 / 4 its other failures.  No video playback here: every attach
 * fails with 1, and the scripts (scrgrp3._bp of Tayutama2AS checks the
 * code) go without. */
static int Opcode_Gfx2_VideoTextureAttach(Thread_t* t)
{
	int i;
	for(i = 0; i < 5; i++)
		Thread_Pop(t);
	Thread_Push(t, 1);
	return 0;
}

/* 92 F0 (1.494 on): arc, name, x, y, w, h → length
 * A movie played out of an archive (the original feeds DirectShow from
 * the entry through its own source filter) at (x, y) in a w x h area.
 * Without video playback the player answers 0 - as "90 F0" does here -
 * and the scripts move on; a non-positive size is still a script error. */
static int Opcode_Gfx2_MoviePlayArc(Thread_t* t)
{
	int h = (int)Thread_Pop(t), w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t), x = (int)Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	int32_t length = 0;
	(void)x;
	(void)y;
	(void)name;
	(void)arc;
	if(w <= 0 || h <= 0)
		GFX_ERROR(t, MSG_BAD_MOVIE_SIZE, w, h);
	Thread_Push(t, (uint32_t)length);
	return 0;
}

/* 92 F1 (1.494 on): &no, &info, arc, name, streamed →
 * The BF_Movie sequence load of "90 F4" (the wait pushes 0 ok / 1 / 2 not
 * a movie when it ends; the thread blocks on it).  `streamed` asks for
 * the form that keeps only the header and index in memory and reads the
 * frames from the archive, whose files are of the BF_Movie types 0x10000
 * / 0x10001 that this build does not decode - the whole file is loaded
 * instead, which serves a type-0 file. */
static int Opcode_Gfx2_SeqLoad(Thread_t* t)
{
	int streamed = (int)Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	int32_t* info = (int32_t*)PopPtr(t);
	int32_t* outNo = (int32_t*)PopPtr(t);
	(void)streamed;
	Thread_SetWait(t, WaitSeqLoad_New(t, arc, name, outNo, info));
	return 2;
}

// 92 F4 (1.494 on): bmp, v → code; control the slot's video texture (always 1: the slot has none)
static int Opcode_Gfx2_VideoTextureControl(Thread_t* t)
{
	Thread_Pop(t);
	Thread_Pop(t);
	Thread_Push(t, 1);
	return 0;
}

/* 92 F5 (1.573 on): bmp, out → code
 * The playing position of the slot's video texture into *out: 0 ok, 1 the
 * slot has no video texture, 4 no such slot (or the texture has no
 * player).  No slot has one here, so 1 for a valid slot and *out is left
 * alone (moviemngr._bp polls it only after a successful "92 F2"). */
static int Opcode_Gfx2_VideoTexturePosition(Thread_t* t)
{
	Bmp_t b;
	int bmp;
	PopPtr(t);
	bmp = (int)Thread_Pop(t);
	Thread_Push(t, BmpMgr_GetInfo(gBmpMgr, &b, bmp) ? 1u : 4u);
	return 0;
}

/* Fill vm_optable_92 for the selected engine profile.  Each entry names the
 * opcode, its handler and the generations that have it (GEN_FIRST ..
 * GEN_LAST for one that never changed), see Vm_FillTable. */
void Vm_Optable92Init(void)
{
	static const OpEntry_t ops[] = {
		{0x14, Opcode_Gfx2_PreloadImage, GEN_1_494, GEN_LAST},
		{0x15, Opcode_Gfx2_PreloadClear, GEN_1_494, GEN_LAST},
		{0x12, Opcode_Gfx2_BmpSetBaseSize, GEN_1_535, GEN_LAST},
		{0x16, Opcode_Gfx2_BmpBaseSize, GEN_1_529, GEN_LAST},
		{0x17, Opcode_Gfx2_BmpGetPixel, GEN_1_529, GEN_LAST},
		{0x97, Opcode_Gfx2_SetRubyFontEx, GEN_1_616, GEN_LAST},
		{0x98, Opcode_Gfx2_TextSetCharImage, GEN_1_588, GEN_LAST},
		{0x9D, Opcode_Gfx2_SetLinkFont, GEN_1_529, GEN_LAST},
		{0x9E, Opcode_Gfx2_TakeLinks, GEN_1_529, GEN_LAST},
		{0x99, Opcode_Gfx2_TextAbcWidths, GEN_1_653, GEN_LAST},
		{0x9B, Opcode_Gfx2_TextLayoutParam, GEN_1_640, GEN_LAST},
		{0x9F, Opcode_Gfx2_SetLinkColour, GEN_1_529, GEN_LAST},
		{0xF0, Opcode_Gfx2_MoviePlayArc, GEN_1_494, GEN_LAST},
		{0xF1, Opcode_Gfx2_SeqLoad, GEN_1_547, GEN_LAST},
		{0xF2, Opcode_Gfx2_VideoTextureAttach, GEN_1_494, GEN_LAST},
		{0xF4, Opcode_Gfx2_VideoTextureControl, GEN_1_547, GEN_LAST},
		{0xF5, Opcode_Gfx2_VideoTexturePosition, GEN_1_573, GEN_LAST},
		{0x1A, Opcode_Gfx2_AlphaToScreenGray, GEN_1_494, GEN_LAST},
		{0x37, Opcode_Gfx2_ObjSetFixedOffset2, GEN_1_69_472, GEN_1_69_472}, // the other builds have it at "91 36" from 1.494
		{0x00, Opcode_Gfx2_DefineRipple, GEN_FIRST, GEN_LAST},
		{0x01, Opcode_Gfx2_DefineRippleEx, GEN_FIRST, GEN_LAST},
		{0x10, Opcode_Gfx2_VecDistRadial, GEN_FIRST, GEN_LAST},
		{0x11, Opcode_Gfx2_VecDistLinear, GEN_FIRST, GEN_LAST},
		{0x18, Opcode_Gfx2_ToGray, GEN_FIRST, GEN_LAST},
		{0x19, Opcode_Gfx2_InvertGray, GEN_FIRST, GEN_LAST},
		{0x1C, Opcode_Gfx2_Text, GEN_FIRST, GEN_LAST},
		{0x1D, Opcode_Gfx2_TextEx, GEN_FIRST, GEN_LAST},
		{0x1E, Opcode_Gfx2_TextMono, GEN_FIRST, GEN_LAST},
		{0x1F, Opcode_Gfx2_LoadLocal, GEN_FIRST, GEN_LAST},
		{0x88, Opcode_Gfx2_WindowShowFrame, GEN_FIRST, GEN_LAST},
		{0x89, Opcode_Gfx2_WindowDrawToFrame, GEN_FIRST, GEN_LAST},
		{0x8A, Opcode_Gfx2_WindowFillFrame, GEN_FIRST, GEN_LAST},
		{0x8C, Opcode_Gfx2_WindowShowText, GEN_FIRST, GEN_LAST},
		{0x8D, Opcode_Gfx2_WindowDrawToText, GEN_FIRST, GEN_LAST},
		{0x8E, Opcode_Gfx2_WindowClear, GEN_FIRST, GEN_LAST},
		{0x90, Opcode_Gfx2_TextOutStyled, GEN_FIRST, GEN_LAST},
		{0x91, Opcode_Gfx2_DrawTextStyled, GEN_FIRST, GEN_LAST},
		{0x9C, Opcode_Gfx2_BmpDrawTextStyled, GEN_FIRST, GEN_LAST},
	};
	Vm_FillTable(vm_optable_92, OPFAM_92, ops, BGI_COUNTOF(ops));
}
