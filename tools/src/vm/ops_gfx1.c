/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ops_gfx1.c - the "91 xx" graphics instruction family: the vector-map
 * generators, bitmap transforms, the multi-layer background, the effector
 * objects, window font / style settings, rich text output, the ruby
 * dictionary and the sprite panel, plus the switches and font settings
 * that the later builds added at the low end of the family (91 00 .. 0F).
 *
 * Same conventions as ops_gfx0.c: operands are popped in the original
 * order, "a, b → r" means b is on top of the stack, the Check* validators
 * raise and never return, result codes map to script errors.  Handlers that
 * install a wait object (the text output, 91 90 / 91 92) return scheduler
 * code 2 (end of turn, the thread blocked until the object completes);
 * everything else returns 0.  An instruction that exists only from some
 * build on names that build in its comment ("1.529 on"); the generation
 * ranges of the table at the end of the file select the handlers for the
 * running profile.  The table is declared in bgi/vm.h.
 */
#include "bgi/vm.h"
#include "bgi/wait.h"
#include "bgi/waitobj.h"
#include "bgi/error.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/panel.h"
#include "bgi/gfx.h"
#include "bgi/gfx/dispobj.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/text.h"

VmHandler_t vm_optable_91[256]; // the "91 xx" dispatch table, filled by Vm_Optable91Init

// raise a script error with a formatted message; never returns
#define GFX_ERROR(t, ...)           \
	do                              \
	{                               \
		char msg_[0x104];           \
		sprintf(msg_, __VA_ARGS__); \
		ScriptError(msg_, (t));     \
	} while(0)

// -------------------------------------------------------------------------
// 91 00 .. 91 0D : switches of the later builds (stored, see docs/versions.md)
// -------------------------------------------------------------------------

/* 91 00 (1.653 on): on →
 * A flag of that build's frame presentation: when set, the frame is
 * presented even without a pending request.  Stored only; the display
 * here has no counterpart. */
static int gPresentAlways;
static int Opcode_Gfx1_SetPresentAlways(Thread_t* t)
{
	gPresentAlways = (int)Thread_Pop(t);
	return 0;
}

/* 91 06 (1.616 on): x, y →
 * An offset added to the draw position of every object whose followOffset
 * parameter is set (setParam 0xC4, the default); the display is redrawn in
 * full. */
static int Opcode_Gfx1_SetGlobalOffset(Thread_t* t)
{
	int32_t y = (int32_t)Thread_Pop(t), x = (int32_t)Thread_Pop(t);
	gDispGlobalOffset[0] = x;
	gDispGlobalOffset[1] = y;
	Gfx_InvalidateAll(gGfx);
	return 0;
}

/* 91 55 (1.529 on): h, h2 → code
 * Mask sprite h by the picture of sprite h2 at h2's position; h2 = 0
 * unlinks.  The code of Gfx_SpriteLinkMask is pushed as it is: 0 ok, 0xB
 * the same sprite or an unknown h2, 0xD already linked / nothing linked,
 * 0xE the mask sprite is in use, 0xF it had no user, 0xFF no sprite h. */
static int Opcode_Gfx1_SpriteLinkMask(Thread_t* t)
{
	uint32_t h2 = Thread_Pop(t), h = Thread_Pop(t);
	Thread_Push(t, (uint32_t)GfxCall_SpriteLinkMask(h, h2));
	return 0;
}

/* 91 09 (1.653 on): mode → ok
 * mode 0 or 1 selects which of two stored limits a size clamp of that
 * build uses; 1 is pushed when the mode was accepted, 0 otherwise.
 * Stored only. */
static int gSizeLimitSelect;
static int Opcode_Gfx1_SetSizeLimitSelect(Thread_t* t)
{
	uint32_t v = Thread_Pop(t);
	if(v <= 1)
		gSizeLimitSelect = (int)v;
	Thread_Push(t, v <= 1);
	return 0;
}

/* 91 0D (1.547 on): on →
 * When set, a variable-pitch face (one EnumFontFamiliesEx reports as
 * such) gets no half-width glyph advance adjustment in that build's glyph
 * renderer.  Stored only: the glyph renderer here takes its advances from
 * the font. */
static int gPropFontAdvance;
static int Opcode_Gfx1_SetPropFontAdvance(Thread_t* t)
{
	gPropFontAdvance = (int)Thread_Pop(t);
	return 0;
}

/* 91 0E (1.529 on): face, sx, sy, dx, dy →
 * A per-face glyph correction of that build's font cache: the scales are
 * 16.16 in 1.0 .. 2.0, the offsets at most (1 - 1/s) * 32 pixels.  An
 * out-of-range scale or offset is a script error.  Stored only: the glyph
 * renderer here takes the faces as FreeType gives them. */
typedef struct FaceAdjust
{
	char face[0x20];        // the face name
	int32_t sx, sy, dx, dy; // scales (16.16) and offsets (pixels)
	struct FaceAdjust* next;
} FaceAdjust_t;
static FaceAdjust_t* gFaceAdjust; // the list of corrections, one entry per face
static int Opcode_Gfx1_SetFaceAdjust(Thread_t* t)
{
	int32_t dy = (int32_t)Thread_Pop(t), dx = (int32_t)Thread_Pop(t), sy = (int32_t)Thread_Pop(t),
			sx = (int32_t)Thread_Pop(t);
	const char* face = (const char*)PopPtr(t);
	FaceAdjust_t* a;
	int32_t sxv;
	if(sx == 0 && sy == 0 && dx == 0 && dy == 0)
	{ // all zero clears the entry (1.588 on; 1.529 rejects it as a bad scale)
		for(a = gFaceAdjust; a; a = a->next)
			if(strcmp(a->face, face) == 0)
				a->sx = a->sy = a->dx = a->dy = 0;
		return 0;
	}
	// from 1.588 a zero x scale stands for the y scale
	if((sx != 0 && (sx < 0x10000 || sx > 0x20000)) || sy < 0x10000 || sy > 0x20000)
		GFX_ERROR(t, MSG_BAD_FACE_SCALE, (unsigned)sx, (unsigned)sy);
	sxv = sx ? sx : sy;
	if(dx > (int32_t)((1.0 - 65536.0 / sxv) * 32) || dy > (int32_t)((1.0 - 65536.0 / sy) * 32))
		GFX_ERROR(t, MSG_BAD_FACE_COORDS, (unsigned)dx, (unsigned)dy);
	for(a = gFaceAdjust; a; a = a->next)
		if(strcmp(a->face, face) == 0)
			break;
	if(!a)
	{
		a = (FaceAdjust_t*)BGI_Calloc(sizeof *a);
		snprintf(a->face, sizeof a->face, "%s", face);
		a->next = gFaceAdjust;
		gFaceAdjust = a;
	}
	a->sx = sx;
	a->sy = sy;
	a->dx = dx;
	a->dy = dy;
	return 0;
}

/* 91 0F (1.529 on): fontNo, size, width, bold, a, b →
 * Open the font in the cache ahead of use and store two values on it: the
 * extra width (and fixed-pitch advance) of its glyphs when drawn upright
 * (a) and in italics (b) by the inline text markup.  An unknown font
 * number, a bad size or a bad width is a script error. */
static int Opcode_Gfx1_FontPreopen(Thread_t* t)
{
	int handle;
	int width, size, fontNo, bold, extraUpright, extraItalic;
	const char* name;
	extraItalic = (int)Thread_Pop(t);
	extraUpright = (int)Thread_Pop(t);
	bold = (int)Thread_Pop(t);
	width = (int)Thread_Pop(t);
	size = (int)Thread_Pop(t);
	fontNo = (int)Thread_Pop(t);
	name = FontNameByNo(fontNo);
	if(!name)
		GFX_ERROR(t, MSG_BAD_FONT_NO, fontNo);
	switch((uint32_t)BmpMgr_FontOpenExtra(gBmpMgr, &handle, name, size, width, bold, extraUpright, extraItalic))
	{
		case 0x80000002u: GFX_ERROR(t, MSG_BAD_FONT_SIZE, size); break;
		case 0x80000003u: GFX_ERROR(t, MSG_BAD_FONT_WIDTH, width); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 91 10 .. 91 15 : vector (displacement) map generators
// -------------------------------------------------------------------------

/* The generators fill a vector-map bitmap slot (pixel mode PM_VECTOR) with
 * a displacement field; each maps the BmpOp_Vec* codes 0x80000001 (no
 * bitmap in the slot) and 0x80000003 (the slot is not a vector map) to
 * script errors, plus the generator's own range check where it has one. */

static int Opcode_Gfx1_VecStretchRange(Thread_t* t) // 91 10: slot, ox, oy, rw, rh →
{
	int rh = (int)Thread_Pop(t), rw = (int)Thread_Pop(t);
	int oy = (int)Thread_Pop(t), ox = (int)Thread_Pop(t);
	int slot = (int)Thread_Pop(t);
	switch(BmpOp_VecStretchRange(slot, ox, oy, rw, rh))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BMP_DST_NOT_EXIST, slot); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_VEC_DST_NOT_VECTOR, slot); break;
		case(int)0x80000005: GFX_ERROR(t, MSG_BAD_STRETCH_RANGE, rw, rh); break;
		default: break;
	}
	return 0;
}

static int Opcode_Gfx1_VecRandom(Thread_t* t) // 91 11: slot, amplitude →
{
	int amplitude = (int)Thread_Pop(t);
	int slot = (int)Thread_Pop(t);
	switch(BmpOp_VecRandom(slot, amplitude))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BMP_DST_NOT_EXIST, slot); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_VEC_DST_NOT_VECTOR, slot); break;
		default: break;
	}
	return 0;
}

static int Opcode_Gfx1_VecRipple(Thread_t* t) // 91 12: slot, cx, cy, period, phase, amplitude →
{
	int amplitude = (int)Thread_Pop(t), phase = (int)Thread_Pop(t), period = (int)Thread_Pop(t);
	int cy = (int)Thread_Pop(t), cx = (int)Thread_Pop(t);
	int slot = (int)Thread_Pop(t);
	switch(BmpOp_VecRipple(slot, cx, cy, period, phase, amplitude))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BMP_DST_NOT_EXIST, slot); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_VEC_DST_NOT_VECTOR, slot); break;
		case(int)0x80000006: GFX_ERROR(t, MSG_BAD_RIPPLE_PERIOD, period); break;
		default: break;
	}
	return 0;
}

static int Opcode_Gfx1_VecPolar(Thread_t* t) // 91 13: slot, cx, cy, angle, k →
{
	int k = (int)Thread_Pop(t);
	int32_t angle = (int32_t)Thread_Pop(t);
	int cy = (int)Thread_Pop(t), cx = (int)Thread_Pop(t);
	int slot = (int)Thread_Pop(t);
	switch(BmpOp_VecPolar(slot, cx, cy, angle, k))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BMP_DST_NOT_EXIST, slot); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_VEC_DST_NOT_VECTOR, slot); break;
		default: break;
	}
	return 0;
}

static int Opcode_Gfx1_VecBendAngle(Thread_t* t) // 91 14: slot, cx, cy, angle, radius →
{
	int radius = (int)Thread_Pop(t);
	int32_t angle = (int32_t)Thread_Pop(t);
	int cy = (int)Thread_Pop(t), cx = (int)Thread_Pop(t);
	int slot = (int)Thread_Pop(t);
	switch(BmpOp_VecBendAngle(slot, cx, cy, angle, radius))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BMP_DST_NOT_EXIST, slot); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_VEC_DST_NOT_VECTOR, slot); break;
		case(int)0x80000008: GFX_ERROR(t, MSG_BAD_BEND_ANGLE, angle, radius); break;
		default: break;
	}
	return 0;
}

static int Opcode_Gfx1_VecBend(Thread_t* t) // 91 15: slot, cx, cy, radius, height →
{
	int height = (int)Thread_Pop(t), radius = (int)Thread_Pop(t);
	int cy = (int)Thread_Pop(t), cx = (int)Thread_Pop(t);
	int slot = (int)Thread_Pop(t);
	switch(BmpOp_VecBend(slot, cx, cy, radius, height))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BMP_DST_NOT_EXIST, slot); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_VEC_DST_NOT_VECTOR, slot); break;
		case(int)0x80000008: GFX_ERROR(t, MSG_BAD_BEND_DEGREE, radius, height); break;
		default: break;
	}
	return 0;
}

/* 91 16 (1.494 on): slot, periodX, phaseX, ampX, periodY, phaseY, ampY →
 * A sine-wave vector field with independent horizontal and vertical waves
 * (BmpOp_VecSine). */
static int Opcode_Gfx1_VecSine(Thread_t* t)
{
	int ampY = (int)Thread_Pop(t), phaseY = (int)Thread_Pop(t), periodY = (int)Thread_Pop(t);
	int ampX = (int)Thread_Pop(t), phaseX = (int)Thread_Pop(t), periodX = (int)Thread_Pop(t);
	int slot = (int)Thread_Pop(t);
	switch(BmpOp_VecSine(slot, periodX, phaseX, ampX, periodY, phaseY, ampY))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BMP_DST_NOT_EXIST, slot); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_VEC_DST_NOT_VECTOR, slot); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 91 18 .. 91 1F : bitmap transforms
// -------------------------------------------------------------------------

/* The shared body of "91 18" (blend) and "91 19" (copy): dst, x, y, src, cx,
 * cy, angle, sx, sy, level, smooth →.  Draws `src` into `dst` rotated by
 * `angle` about (cx, cy) and scaled by (sx, sy), placed at (x, y); the
 * positions and scales are 16.16, `level` the blend level (0 .. 0x100,
 * validated), `smooth` selects bilinear sampling.  The copy form replaces
 * the destination pixels instead of blending over them.  Result codes: 1
 * no destination bitmap, 2 no source bitmap, 3 a scale out of range. */
static int Gfx1_Xform(Thread_t* t, int copy)
{
	int smooth = (int)Thread_Pop(t), level = (int)Thread_Pop(t);
	int32_t sy = (int32_t)Thread_Pop(t), sx = (int32_t)Thread_Pop(t), angle = (int32_t)Thread_Pop(t);
	int32_t cy = (int32_t)Thread_Pop(t), cx = (int32_t)Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int32_t y = (int32_t)Thread_Pop(t), x = (int32_t)Thread_Pop(t);
	int dst = (int)Thread_Pop(t), r;
	CheckAlpha(level, t);
	r = copy ? BmpOp_XformCopy(dst, x, y, src, cx, cy, angle, sx, sy, level, smooth)
			 : BmpOp_Xform(dst, x, y, src, cx, cy, angle, sx, sy, level, smooth);
	switch(r)
	{
		case 1: GFX_ERROR(t, MSG_XFORM_OUT_INVALID, dst); break;
		case 2: GFX_ERROR(t, MSG_REF_BMP_INVALID, src); break;
		case 3: GFX_ERROR(t, MSG_BAD_SCALE2, sx, sy); break;
		default: break;
	}
	return 0;
}

static int Opcode_Gfx1_Xform(Thread_t* t) // 91 18: dst, x, y, src, cx, cy, angle, sx, sy, level, smooth →
{
	return Gfx1_Xform(t, 0);
}

static int Opcode_Gfx1_XformCopy(Thread_t* t) // 91 19: dst, x, y, src, cx, cy, angle, sx, sy, level, smooth →
{
	return Gfx1_Xform(t, 1);
}

// scale src by (sx, sy) (16.16) into dst; the source must be true colour
static int Opcode_Gfx1_Resample(Thread_t* t) // 91 1C: dst, src, sx, sy, smooth →
{
	int smooth = (int)Thread_Pop(t);
	int32_t sy = (int32_t)Thread_Pop(t), sx = (int32_t)Thread_Pop(t);
	int src = (int)Thread_Pop(t), dst = (int)Thread_Pop(t);
	switch(BmpOp_Resample(dst, src, sx, sy, smooth))
	{
		case 1: GFX_ERROR(t, MSG_RESAMPLE_DST_INVALID, dst); break;
		case 2: GFX_ERROR(t, MSG_RESAMPLE_SRC_INVALID, src); break;
		case 3: GFX_ERROR(t, MSG_RESAMPLE_SRC_NOT_TRUECOLOR, src); break;
		case 4: GFX_ERROR(t, MSG_BAD_STRETCH_RATE, sx, sy); break;
		default: break;
	}
	return 0;
}

/* 91 1B (1.494 on): dst, src, cx, cy, level →
 * The gathering add of the screen-group transition (BmpOp_GatherAdd): the
 * pixels of src are drawn toward its centre by the 16.16 gathering rates
 * cx, cy (at most 1.0) and added to dst with saturation, attenuated by
 * level (0 .. 0x100).  Both bitmaps must be 32-bit. */
static int Opcode_Gfx1_GatherAdd(Thread_t* t)
{
	uint32_t level = Thread_Pop(t);
	uint32_t cy = Thread_Pop(t), cx = Thread_Pop(t);
	int src = (int)Thread_Pop(t), dst = (int)Thread_Pop(t);
	switch(BmpOp_GatherAdd(dst, src, cx, cy, level))
	{
		case 1: GFX_ERROR(t, MSG_OUT_BMP_INVALID, dst); break;
		case 2: GFX_ERROR(t, MSG_RESAMPLE_SRC_INVALID, src); break;
		case 4: GFX_ERROR(t, MSG_BAD_GATHER_RATE, (int)cx, (int)cy); break;
		case 5: GFX_ERROR(t, MSG_BAD_ATTENUATION, (int)level); break;
		default: break;
	}
	return 0;
}

/* 91 1D (1.494 on): dst, src, mode, colour, level →
 * The colour modes of the bust-shot control (BmpOp_ColourMode): src into
 * dst through mode 0 copy, 1 blend toward src XOR colour, 2 luminance
 * tint, 3 blend toward the colour, 4 add the colour, at `level` (0 ..
 * 0x100).  An unknown mode is a script error. */
static int Opcode_Gfx1_ColourMode(Thread_t* t)
{
	int level = (int)Thread_Pop(t);
	uint32_t colour = Thread_Pop(t);
	int mode = (int)Thread_Pop(t);
	int src = (int)Thread_Pop(t), dst = (int)Thread_Pop(t);
	CheckAlpha((uint32_t)level, t);
	switch(BmpOp_ColourMode(dst, src, mode, colour, level))
	{
		case 1: GFX_ERROR(t, MSG_OUT_BMP_INVALID, dst); break;
		case 2: GFX_ERROR(t, MSG_RESAMPLE_SRC_INVALID, src); break;
		case 6: GFX_ERROR(t, MSG_BAD_PROCESS_TYPE, mode); break;
		default: break;
	}
	return 0;
}

// copy the 8-bit gray bitmap into the alpha channel of dst; dst pixel (px, py) takes gray pixel (px + x, py + y)
static int Opcode_Gfx1_ApplyGray(Thread_t* t) // 91 1E: dst, gray, x, y →
{
	int y = (int)Thread_Pop(t), x = (int)Thread_Pop(t);
	int gray = (int)Thread_Pop(t), dst = (int)Thread_Pop(t);
	switch(BmpOp_ApplyGray(dst, gray, x, y))
	{
		case 1: GFX_ERROR(t, MSG_TARGET_BMP_INVALID, dst); break;
		case 2: GFX_ERROR(t, MSG_RESAMPLE_SRC_INVALID, gray); break;
		case 3: GFX_ERROR(t, MSG_SRC_NOT_GRAY, gray); break;
		default: break;
	}
	return 0;
}

static int Opcode_Gfx1_Duplicate(Thread_t* t) // 91 1F: dst, src →
{
	int src = (int)Thread_Pop(t), dst = (int)Thread_Pop(t);
	switch(BmpOp_Duplicate(dst, src))
	{
		case 1: GFX_ERROR(t, MSG_OUT_BMP_INVALID, dst); break;
		case 2: GFX_ERROR(t, MSG_COPY_SRC_INVALID, src); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 91 33 : objects
// -------------------------------------------------------------------------

// the fixed position of any object: 16.16 coordinates and depth
static int Opcode_Gfx1_ObjSetFixedPos(Thread_t* t) // 91 33: h, fx, fy, fz →
{
	int32_t fz = (int32_t)Thread_Pop(t), fy = (int32_t)Thread_Pop(t), fx = (int32_t)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_ObjSetFixedPos(h, fx, fy, fz))
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

/* 91 1A (1.69/472 on): dst, src, colour →
 * Every pixel of dst becomes `colour` with the alpha of the source pixel;
 * 32-bit bitmaps only (anything else is silently left alone), over the
 * overlap of the two.  A missing bitmap is a script error. */
static int Opcode_Gfx1_BmpColourWithAlpha(Thread_t* t)
{
	uint32_t colour = Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	Bmp_t d, s;
	int w, h, x, y;
	if(!BmpOp_GetInfo(&d, dst))
		GFX_ERROR(t, MSG_OUT_BMP_INVALID, dst);
	if(!BmpOp_GetInfo(&s, src))
		GFX_ERROR(t, MSG_RESAMPLE_SRC_INVALID, src);
	if(d.mode != 2 || s.mode != 2)
		return 0;
	w = BGI_MIN(d.w, s.w);
	h = BGI_MIN(d.h, s.h);
	for(y = 0; y < h; y++)
	{
		const uint32_t* sp = (const uint32_t*)(s.pixels + (size_t)y * (size_t)s.pitch);
		uint32_t* dp = (uint32_t*)(d.pixels + (size_t)y * (size_t)d.pitch);
		for(x = 0; x < w; x++)
			dp[x] = (sp[x] & 0xff000000u) | (colour & 0xffffffu);
	}
	return 0;
}

// 91 37 (1.69/472 on): h, x, y, z →; the first of the two 16.16 offsets added to the fixed position
static int Opcode_Gfx1_ObjSetFixedOffset(Thread_t* t)
{
	int32_t z = (int32_t)Thread_Pop(t), y = (int32_t)Thread_Pop(t), x = (int32_t)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_ObjSetFixedOffset(h, 1, x, y, z))
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

/* 91 38 (1.69/472 on): out, h, no →
 * Read parameter `no` of object h into *out (the pointer is pushed first,
 * the number is on top): 0 the position (two values), 1 the effect, 2 the
 * level, 3 the priority (1.494 on), 0x20 the fixed position (three
 * values), -1 the filter's stored value (1.535 on), -2 the class order
 * (1.616 on), 0x7FFFFFFF a user value (1.494 on; *out names the slot, 0 ..
 * 15, and receives the value).  An unsupported number is a script error;
 * from 1.494 so is a parameter that rejects the value *out carries in. */
static int Opcode_Gfx1_ObjGetParam(Thread_t* t)
{
	int no = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	int32_t* out = (int32_t*)PopPtr(t);
	switch(GfxCall_ObjGetParam(h, no, out))
	{
		case 5: GFX_ERROR(t, MSG_BAD_PARAM_NO, (unsigned)no); break;
		case 0xfe:
			if(gEngine->gen >= GEN_1_494)
				GFX_ERROR(t, MSG_BAD_GETPARAM_ARG, (unsigned)no, (int)out[0], (unsigned)out[0]);
			break;
		case 0xff: ScriptError(MSG_BAD_OBJ_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* 91 97 (1.529 on): fontNo, size, width, dx, dy →
 * The ruby font: a registered face number (-1 or an unknown number keeps
 * the base face), a fixed size (0: the ruby percentage of the base size),
 * a width percentage (0: the base font's) and an offset in pixels.  A face
 * name of 0x100 bytes or more is a script error. */
static int Opcode_Gfx1_SetRubyFont(Thread_t* t)
{
	int dy = (int)Thread_Pop(t), dx = (int)Thread_Pop(t), width = (int)Thread_Pop(t), size = (int)Thread_Pop(t);
	int fontNo = (int)Thread_Pop(t);
	const char* name = FontNameByNo(fontNo);
	if(name && strlen(name) >= 0x100)
		GFX_ERROR(t, MSG_BAD_FONT_NO, fontNo);
	Text_SetRubyFont(name, size, width, dx, dy);
	return 0;
}

/* 91 9B (1.494 on): out, str, fontNo, size, widthPct, bold, prop → r
 * The width of `str` in that font into *out: the third measure of
 * Text_Measure, the advance less half the trailing gap.  r is
 * BmpOp_FontOpen's result (0 ok, 0x80000001 bad size, 0x80000002 bad
 * width, 0x80000003 bad font number); nothing is written when the font
 * cannot be opened. */
static int Opcode_Gfx1_MeasureText(Thread_t* t)
{
	int prop = (int)Thread_Pop(t), bold = (int)Thread_Pop(t), widthPct = (int)Thread_Pop(t);
	int size = (int)Thread_Pop(t), fontNo = (int)Thread_Pop(t);
	const char* str = (const char*)PopPtr(t);
	int32_t* out = (int32_t*)PopPtr(t);
	int font = 0;
	int r = BmpOp_FontOpen(&font, fontNo, size, widthPct, bold);
	if(r == 0)
	{
		int32_t m[3] = {0, 0, 0};
		Text_Measure(m, str, font, prop);
		*out = m[2];
	}
	Thread_Push(t, (uint32_t)r);
	return 0;
}

/* 91 9E (1.529 on): out, str → n
 * The "<l>..</l>" link sections of `str`: their texts into `out` (0x80
 * bytes each, see TextLink_t), the count pushed. */
static int Opcode_Gfx1_TextCountLinks(Thread_t* t)
{
	const char* str = (const char*)PopPtr(t);
	TextLink_t* out = (TextLink_t*)PopPtr(t);
	Thread_Push(t, (uint32_t)Text_CountLinks(out, str));
	return 0;
}

// 91 9F (1.529 on): out, str →; `str` without its markup into `out`
static int Opcode_Gfx1_TextStripTags(Thread_t* t)
{
	const char* str = (const char*)PopPtr(t);
	char* out = (char*)PopPtr(t);
	Text_StripTags(out, str);
	return 0;
}

/* 91 F0 .. 91 F7 (1.494 on): the bitmap manager's video textures.  In the
 * original a bitmap slot can carry a "video texture", a movie decoded into
 * its pixels by a player object that the attach ("92 F2") creates.  There
 * is no video playback here, so no slot ever has a texture: each of these
 * instructions answers 4 (the original's 0x80000004) for a slot without a
 * bitmap and 1 (0x80000001) for a slot without a texture, which every slot
 * with a bitmap is.  91 F0 is the attach by name (the original answers 0
 * .. 4; always 1 here); 91 F1 reads the player state; 91 F2 releases the
 * texture; 91 F3 is a control with one value and 91 F4 one with a pointer
 * and three values; 91 F5 advances the frame; 91 F6 releases the slot;
 * 91 F7 writes the slot's last frame number (0 without a texture) and
 * pushes 1, or pushes 0 for a slot without a bitmap. */

// the shared answer: 1 for a slot with a bitmap (and so without a texture), 4 for one without
static uint32_t VideoTextureSlotCode(int bmp)
{
	Bmp_t b;
	return BmpMgr_GetInfo(gBmpMgr, &b, bmp) ? 1u : 4u;
}

static int Opcode_Gfx1_VideoTextureOpen(Thread_t* t) // 91 F0: v1, v2, name, v3 → 1 (the operands are ignored)
{
	Thread_Pop(t);
	PopPtr(t);
	Thread_Pop(t);
	Thread_Pop(t);
	Thread_Push(t, 1);
	return 0;
}

static int Opcode_Gfx1_VideoTextureState(Thread_t* t) // 91 F1: bmp, &out → code
{
	int32_t* out = (int32_t*)PopPtr(t);
	int bmp = (int)Thread_Pop(t);
	(void)out;
	Thread_Push(t, VideoTextureSlotCode(bmp));
	return 0;
}

static int Opcode_Gfx1_VideoTextureSlotOp(Thread_t* t) // 91 F2 / F5 / F6: bmp → code
{
	Thread_Push(t, VideoTextureSlotCode((int)Thread_Pop(t)));
	return 0;
}

static int Opcode_Gfx1_VideoTextureControl1(Thread_t* t) // 91 F3: bmp, v → code
{
	Thread_Pop(t);
	Thread_Push(t, VideoTextureSlotCode((int)Thread_Pop(t)));
	return 0;
}

static int Opcode_Gfx1_VideoTextureControl4(Thread_t* t) // 91 F4: bmp, &p, a, b, c → code
{
	Thread_Pop(t);
	Thread_Pop(t);
	Thread_Pop(t);
	PopPtr(t);
	Thread_Push(t, VideoTextureSlotCode((int)Thread_Pop(t)));
	return 0;
}

static int Opcode_Gfx1_VideoTextureFrame(Thread_t* t) // 91 F7: bmp, &out → 1 / 0
{
	int32_t* out = (int32_t*)PopPtr(t);
	int bmp = (int)Thread_Pop(t);
	Bmp_t b;
	if(!BmpMgr_GetInfo(gBmpMgr, &b, bmp))
	{
		Thread_Push(t, 0);
		return 0;
	}
	*out = 0;
	Thread_Push(t, 1);
	return 0;
}

// 91 99 (1.529 on): v → ok; the text engine's scale divisor (Text_SetScaleDivisor: 1 accepted, 0 rejected)
static int Opcode_Gfx1_SetTextScaleDivisor(Thread_t* t)
{
	Thread_Push(t, (uint32_t)Text_SetScaleDivisor((int32_t)Thread_Pop(t)));
	return 0;
}

/* 91 9A (1.529 on): func, param →
 * Set a text function (Text_SetFunction; the numbers are 0 and 0x80000000
 * .. 0x8000000A, see bgi/gfx/text.h).  An unknown number or a bad
 * parameter is a script error. */
static int Opcode_Gfx1_SetTextFunction(Thread_t* t)
{
	int32_t param = (int32_t)Thread_Pop(t);
	uint32_t func = Thread_Pop(t);
	switch(Text_SetFunction(func, param))
	{
		case 0x80000007: GFX_ERROR(t, MSG_BAD_TEXT_FUNC, func);
		case 0x80000008: GFX_ERROR(t, MSG_BAD_TEXT_FUNC_PARAM, param);
		default: return 0;
	}
}

// 91 36 (1.494 on): h, x, y, z →; the second fixed-position offset (build 472 has it at "92 37" as well)
static int Opcode_Gfx1_ObjSetFixedOffset2(Thread_t* t)
{
	int32_t z = (int32_t)Thread_Pop(t), y = (int32_t)Thread_Pop(t), x = (int32_t)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_ObjSetFixedOffset(h, 2, x, y, z))
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

/* 91 3E (1.494 on): master, slave, dx, dy →
 * Attach any object to another: the slave follows the master's position
 * at the offset (dx, dy).  Script errors: an unknown master or slave, the
 * slave being the master, or a slave that already has an owner. */
static int Opcode_Gfx1_ObjAttach(Thread_t* t)
{
	int dy = (int)Thread_Pop(t), dx = (int)Thread_Pop(t);
	uint32_t slave = Thread_Pop(t), master = Thread_Pop(t);
	switch(GfxCall_ObjAttach(master, slave, dx, dy))
	{
		case 0xff: ScriptError(MSG_BAD_MASTER_HANDLE, t); break;
		case 6: ScriptError(MSG_BAD_SLAVE_HANDLE, t); break;
		case 7: ScriptError(MSG_SLAVE_IS_MASTER, t); break;
		case 8: ScriptError(MSG_SLAVE_HAS_OWNER, t); break;
		default: break;
	}
	return 0;
}

// 91 3F (1.494 on): master, slave →; detach; an unknown handle or a slave not attached to that master is a script error
static int Opcode_Gfx1_ObjDetach(Thread_t* t)
{
	uint32_t slave = Thread_Pop(t), master = Thread_Pop(t);
	switch(GfxCall_ObjDetach(master, slave))
	{
		case 0xff: ScriptError(MSG_BAD_MASTER_HANDLE, t); break;
		case 6: ScriptError(MSG_BAD_SLAVE_HANDLE, t); break;
		case 9: ScriptError(MSG_SLAVE_NOT_ATTACHED, t); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 91 40 .. 91 4A : the multi-layer background
// -------------------------------------------------------------------------

/* Switch the background to the multi-layer class (type 12, eight
 * transformed layers) with layer 0 set up in one go: bitmap `bmp` with its
 * centre (cx, cy) at (x, y), rotated by `angle` and scaled by (sx, sy)
 * (all 16.16); `smooth` selects bilinear sampling. */
static int Opcode_Gfx1_BgSetLayers(Thread_t* t) // 91 40: x, y, bmp, cx, cy, angle, sx, sy, smooth →
{
	int smooth = (int)Thread_Pop(t);
	int32_t sy = (int32_t)Thread_Pop(t), sx = (int32_t)Thread_Pop(t), angle = (int32_t)Thread_Pop(t);
	int32_t cy = (int32_t)Thread_Pop(t), cx = (int32_t)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t), x = (int)Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	switch(GfxCall_BgSetLayers(x, y, bmp, cx, cy, angle, sx, sy, smooth))
	{
		case 3: GFX_ERROR(t, MSG_BMP_INVALID, bmp); break;
		case 4: GFX_ERROR(t, MSG_BAD_SCALE2, sx, sy); break;
		default: break;
	}
	return 0;
}

// the result map shared by the layer calls: 1 not in multi-layer mode, 2 bad layer number
static void Gfx1_LayerResult(Thread_t* t, int r, int layer)
{
	switch(r)
	{
		case 1: ScriptError(MSG_BG_NOT_MULTILAYER, t); break;
		case 2: GFX_ERROR(t, MSG_BAD_LAYER, layer); break;
		default: break;
	}
}

// make `layer` (0 .. 7) the one the generic object operations on the background (position, level) address
static int Opcode_Gfx1_BglSelect(Thread_t* t) // 91 41: layer →
{
	int layer = (int)Thread_Pop(t);
	Gfx1_LayerResult(t, GfxCall_BglSelect(layer), layer);
	return 0;
}

static int Opcode_Gfx1_BglShow(Thread_t* t) // 91 42: layer, on →
{
	int on = (int)Thread_Pop(t), layer = (int)Thread_Pop(t);
	Gfx1_LayerResult(t, GfxCall_BglShow(layer, on), layer);
	return 0;
}

static int Opcode_Gfx1_BglSetPos(Thread_t* t) // 91 43: layer, x, y →
{
	int32_t y = (int32_t)Thread_Pop(t), x = (int32_t)Thread_Pop(t);
	int layer = (int)Thread_Pop(t);
	Gfx1_LayerResult(t, GfxCall_BglSetPos(layer, x, y), layer);
	return 0;
}

static int Opcode_Gfx1_BglSetEffect(Thread_t* t) // 91 44: layer, effect →
{
	int effect = (int)Thread_Pop(t), layer = (int)Thread_Pop(t);
	CheckEffectMode(effect, t);
	Gfx1_LayerResult(t, GfxCall_BglSetEffect(layer, effect), layer);
	return 0;
}

static int Opcode_Gfx1_BglSetLevel(Thread_t* t) // 91 45: layer, level →
{
	int level = (int)Thread_Pop(t), layer = (int)Thread_Pop(t);
	CheckAlpha(level, t);
	Gfx1_LayerResult(t, GfxCall_BglSetLevel(layer, level), layer);
	return 0;
}

// the layer's bitmap and its rotation centre (16.16)
static int Opcode_Gfx1_BglSetBitmap(Thread_t* t) // 91 46: layer, bmp, cx, cy →
{
	int32_t cy = (int32_t)Thread_Pop(t), cx = (int32_t)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t), layer = (int)Thread_Pop(t), r;
	CheckBitmapNo(bmp, t);
	r = GfxCall_BglSetBitmap(layer, bmp, cx, cy);
	if(r == 3)
		GFX_ERROR(t, MSG_BMP_INVALID, bmp);
	else
		Gfx1_LayerResult(t, r, layer);
	return 0;
}

// the layer's rotation angle and scales (16.16); a scale out of range is a script error
static int Opcode_Gfx1_BglSetTransform(Thread_t* t) // 91 47: layer, angle, sx, sy, smooth →
{
	int smooth = (int)Thread_Pop(t);
	int32_t sy = (int32_t)Thread_Pop(t), sx = (int32_t)Thread_Pop(t), angle = (int32_t)Thread_Pop(t);
	int layer = (int)Thread_Pop(t), r;
	r = GfxCall_BglSetTransform(layer, angle, sx, sy, smooth);
	if(r == 4)
		GFX_ERROR(t, MSG_BAD_SCALE2, sx, sy);
	else
		Gfx1_LayerResult(t, r, layer);
	return 0;
}

// the ease curves of the layer's angle and scale deltas
static int Opcode_Gfx1_BglSetCurves(Thread_t* t) // 91 48: layer, angleCurve, scaleCurve →
{
	int scaleCurve = (int)Thread_Pop(t), angleCurve = (int)Thread_Pop(t);
	int layer = (int)Thread_Pop(t);
	Gfx1_LayerResult(t, GfxCall_BglSetCurves(layer, angleCurve, scaleCurve), layer);
	return 0;
}

// the change of the layer's centre over the tween's progress (16.16)
static int Opcode_Gfx1_BglSetCentreDelta(Thread_t* t) // 91 49: layer, dcx, dcy →
{
	int32_t dcy = (int32_t)Thread_Pop(t), dcx = (int32_t)Thread_Pop(t);
	int layer = (int)Thread_Pop(t);
	Gfx1_LayerResult(t, GfxCall_BglSetCentreDelta(layer, dcx, dcy), layer);
	return 0;
}

// the change of the layer's angle and scales over the tween's progress (16.16)
static int Opcode_Gfx1_BglSetXformDelta(Thread_t* t) // 91 4A: layer, dAngle, dsx, dsy →
{
	int32_t dsy = (int32_t)Thread_Pop(t), dsx = (int32_t)Thread_Pop(t), dAngle = (int32_t)Thread_Pop(t);
	int layer = (int)Thread_Pop(t);
	Gfx1_LayerResult(t, GfxCall_BglSetXformDelta(layer, dAngle, dsx, dsy), layer);
	return 0;
}

// -------------------------------------------------------------------------
// 91 60 .. 91 68 : effector objects
// -------------------------------------------------------------------------

// a new effector; its handle, or a script error when the table is full
static int Opcode_Gfx1_EffectorCreate(Thread_t* t) // 91 60: → h
{
	uint32_t h = GfxCall_EffectorCreate();
	if(!h)
		ScriptError(MSG_NO_MORE_EFFECTORS, t);
	Thread_Push(t, h);
	return 0;
}

static int Opcode_Gfx1_EffectorDelete(Thread_t* t) // 91 61: h →
{
	if(!GfxCall_EffectorDelete(Thread_Pop(t)))
		ScriptError(MSG_BAD_EFFECTOR_HANDLE, t);
	return 0;
}

static int Opcode_Gfx1_EffectorShow(Thread_t* t) // 91 64: h, on →
{
	int on = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_EffectorShow(h, on))
		ScriptError(MSG_BAD_EFFECTOR_HANDLE, t);
	return 0;
}

/* Make the effector a displacement of the screen below it through the
 * screen-sized vector map vec1 (and vec2; -1 for none); a non-zero
 * `amount` selects bilinear sampling, `level` (0 .. 0x100) is the blend
 * level and `prio` (0 .. 4095) the draw priority.  Script errors: an
 * unknown handle, a map that does not exist or is not a screen-sized
 * vector map. */
static int Opcode_Gfx1_EffectorSetVector(Thread_t* t) // 91 65: h, vec1, vec2, level, amount, prio →
{
	uint32_t prio = Thread_Pop(t);
	int amount = (int)Thread_Pop(t), level = (int)Thread_Pop(t);
	int vec2 = (int)Thread_Pop(t), vec1 = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	switch(GfxCall_EffectorSetVector(h, vec1, vec2, level, amount, prio))
	{
		case 0xff: ScriptError(MSG_BAD_EFFECTOR_HANDLE, t); break;
		case 1: GFX_ERROR(t, MSG_VEC1_INVALID, vec1); break;
		case 3: GFX_ERROR(t, MSG_VEC1_NOT_SCREEN, vec1); break;
		case 2: GFX_ERROR(t, MSG_VEC2_INVALID, vec2); break;
		case 4: GFX_ERROR(t, MSG_VEC2_NOT_SCREEN, vec2); break;
		default: break;
	}
	return 0;
}

// make the effector a gradient of `type` over the screen below it; an unknown type is a script error
static int Opcode_Gfx1_EffectorSetGradient(Thread_t* t) // 91 66: h, type, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	int level = (int)Thread_Pop(t), type = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	switch(GfxCall_EffectorSetGradient(h, type, level, prio))
	{
		case 0xff: ScriptError(MSG_BAD_EFFECTOR_HANDLE, t); break;
		case 5: GFX_ERROR(t, MSG_BAD_GRADIENT_TYPE2, type); break;
		default: break;
	}
	return 0;
}

/* Make the effector a ripple: `map` is a screen-sized vector-distance map,
 * `rings` the number of rings (the maximum distance, non-zero), `ripple` a
 * registered ripple pattern that must match the map.  Script errors for
 * each of those being missing or mismatched, and for an unknown handle. */
static int Opcode_Gfx1_EffectorSetRipple(Thread_t* t) // 91 67: h, map, rings, ripple, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	int level = (int)Thread_Pop(t), ripple = (int)Thread_Pop(t), rings = (int)Thread_Pop(t);
	int map = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	switch(GfxCall_EffectorSetRipple(h, map, rings, ripple, level, prio))
	{
		case 0xff: ScriptError(MSG_BAD_EFFECTOR_HANDLE, t); break;
		case 1: GFX_ERROR(t, MSG_VDMAP_INVALID, map); break;
		case 3: GFX_ERROR(t, MSG_VDMAP_NOT_SCREEN2, map); break;
		case 6: GFX_ERROR(t, MSG_BAD_MAX_DISTANCE, rings); break;
		case 7: GFX_ERROR(t, MSG_RIPPLE_NOT_REGISTERED, ripple); break;
		case 8: GFX_ERROR(t, MSG_RIPPLE_MISMATCH, ripple, map); break;
		default: break;
	}
	return 0;
}

/* Make the effector a rotation / zoom of the screen below it about
 * (cx, cy): `angle` and the scales are 16.16, `smooth` selects bilinear
 * sampling.  Only a zero scale is a script error (code 9); an unknown
 * handle is ignored, as in the original. */
static int Opcode_Gfx1_EffectorSetZoom(Thread_t* t) // 91 68: h, cx, cy, angle, sx, sy, smooth, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	int level = (int)Thread_Pop(t), smooth = (int)Thread_Pop(t);
	int32_t sy = (int32_t)Thread_Pop(t), sx = (int32_t)Thread_Pop(t), angle = (int32_t)Thread_Pop(t);
	int32_t cy = (int32_t)Thread_Pop(t), cx = (int32_t)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	if(GfxCall_EffectorSetZoom(h, cx, cy, angle, sx, sy, smooth, level, prio) == 9)
		GFX_ERROR(t, MSG_BAD_SCALE2, sx, sy);
	return 0;
}

// -------------------------------------------------------------------------
// 91 88 .. 91 8E : window font and style
// -------------------------------------------------------------------------

/* The window's text font by number: size in pixels, width in percent,
 * bold, proportional glyph placement, and whether one font size is kept
 * free on the right for readings.  Shared with the 1.58 .. 1.64 tables,
 * which had it at "90 87". */
int Opcode_Gfx1_WindowSetFont(Thread_t* t) // 91 88: h, fontNo, size, widthPct, bold, prop, rubyReserve →
{
	int rubyReserve = (int)Thread_Pop(t), prop = (int)Thread_Pop(t), bold = (int)Thread_Pop(t);
	int widthPct = (int)Thread_Pop(t), size = (int)Thread_Pop(t), fontNo = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckFontNo(fontNo, t);
	switch(GfxCall_WindowSetFont(h, fontNo, size, widthPct, bold, prop, rubyReserve))
	{
		case 1: GFX_ERROR(t, MSG_FONT_SIZE_INVALID, size); break;
		case 2: GFX_ERROR(t, MSG_FONT_WIDTH_INVALID, widthPct); break;
		case 3: GFX_ERROR(t, MSG_FONT_NO_INVALID, fontNo); break;
		case -1: ScriptError(MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// the line gap in percent of the font size, 0 .. 800 ("90 8A" of 1.58 .. 1.64)
int Opcode_Gfx1_WindowSetSpacing(Thread_t* t) // 91 89: h, pct →
{
	int pct = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_WindowSetSpacing(h, pct))
	{
		case 1: GFX_ERROR(t, MSG_BAD_SPACING_FACTOR, pct); break;
		case -1: ScriptError(MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// the text direction: 0 horizontal, 1 vertical
static int Opcode_Gfx1_WindowSetDrawStyle(Thread_t* t) // 91 8A: h, style →
{
	int style = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_WindowSetDrawStyle(h, style))
	{
		case 1: GFX_ERROR(t, MSG_BAD_DRAW_STYLE, style); break;
		case -1: ScriptError(MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// the line alignment ("swing"): 0 none, 1 centre every line, 2 right-align it
static int Opcode_Gfx1_WindowSetSwingStyle(Thread_t* t) // 91 8B: h, style →
{
	int style = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_WindowSetSwingStyle(h, style))
	{
		case 1: GFX_ERROR(t, MSG_BAD_SWING_STYLE, style); break;
		case -1: ScriptError(MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// the text cursor, in text-area pixels
static int Opcode_Gfx1_WindowSetCursor(Thread_t* t) // 91 8C: h, x, y →
{
	int y = (int)Thread_Pop(t), x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_WindowSetCursor(h, x, y))
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

// the text cursor: r is 1 (an unknown handle is a script error), then its x and y ("91 8F" of 1.58 .. 1.64)
int Opcode_Gfx1_WindowGetCursor(Thread_t* t) // 91 8D: h → r, x, y
{
	int32_t pos[2];
	uint32_t h = Thread_Pop(t);
	int r = GfxCall_WindowGetCursor(pos, h);
	if(!r)
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	Thread_Push(t, (uint32_t)r);
	Thread_Push(t, (uint32_t)pos[0]);
	Thread_Push(t, (uint32_t)pos[1]);
	return 0;
}

// status is 1 when the text cursor sits at the start of a line (the area's leading edge plus the indent), else 0
static int Opcode_Gfx1_WindowCursorStatus(Thread_t* t) // 91 8E: h → status
{
	int status;
	uint32_t h = Thread_Pop(t);
	if(GfxCall_WindowCursorStatus(&status, h) == -1)
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	Thread_Push(t, (uint32_t)status);
	return 0;
}

// -------------------------------------------------------------------------
// 91 90 .. 91 98 : rich text output and the ruby dictionary
// -------------------------------------------------------------------------

// the result map of StartTextOutW: 0x80000001 the window has no font, -1 no such window
static void Gfx1_TextOutResult(Thread_t* t, int r)
{
	if(r == (int)0x80000001)
		ScriptError(MSG_WINDOW_NO_FONT, t);
	else if(r == -1)
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
}

/* 91 90: h, str, colour, instant, waitAtEnd, allowSkip, allowKeys,
 * rubyOn, hang, unused → (the last operand is popped and ignored)
 * Start the rich text output of `str` in window h and block the thread on
 * it: `instant` shows the whole text at once, `waitAtEnd` waits for a key
 * when it is complete, `allowSkip` / `allowKeys` let the user hurry it,
 * `rubyOn` draws the ruby readings, `hang` lets a leading bracket hang into
 * the margin.  The colour doubles as the ruby colour; the current shadow
 * style applies. */
static int Opcode_Gfx1_TextOut(Thread_t* t)
{
	int hang, rubyOn, allowKeys, allowSkip, waitAtEnd, instant;
	uint32_t colour, h;
	const char* str;
	Thread_Pop(t);
	hang = (int)Thread_Pop(t);
	rubyOn = (int)Thread_Pop(t);
	allowKeys = (int)Thread_Pop(t);
	allowSkip = (int)Thread_Pop(t);
	waitAtEnd = (int)Thread_Pop(t);
	instant = (int)Thread_Pop(t);
	colour = Thread_Pop(t);
	str = (const char*)PopPtr(t);
	h = Thread_Pop(t);
	Gfx1_TextOutResult(t, StartTextOutW(t, h, str, colour, 0, instant, waitAtEnd, allowSkip, allowKeys, rubyOn, hang, 0));
	return 2;
}

/* 91 92: h, str, colour, plainStyle, instant, waitAtEnd,
 * allowSkip, allowKeys, rubyOn, hang, unused →
 * As "91 90" with `plainStyle`, which drops the shadow. */
static int Opcode_Gfx1_TextOutStyled(Thread_t* t)
{
	int hang, rubyOn, allowKeys, allowSkip, waitAtEnd, instant, plainStyle;
	uint32_t colour, h;
	const char* str;
	Thread_Pop(t);
	hang = (int)Thread_Pop(t);
	rubyOn = (int)Thread_Pop(t);
	allowKeys = (int)Thread_Pop(t);
	allowSkip = (int)Thread_Pop(t);
	waitAtEnd = (int)Thread_Pop(t);
	instant = (int)Thread_Pop(t);
	plainStyle = (int)Thread_Pop(t);
	colour = Thread_Pop(t);
	str = (const char*)PopPtr(t);
	h = Thread_Pop(t);
	Gfx1_TextOutResult(
		t, StartTextOutW(t, h, str, colour, plainStyle, instant, waitAtEnd, allowSkip, allowKeys, rubyOn, hang, 0));
	return 2;
}

// the result map of GfxCall_WindowDrawText: 1 the window has no font, -1 no such window
static void Gfx1_DrawTextResult(Thread_t* t, int r)
{
	if(r == 1)
		ScriptError(MSG_WINDOW_NO_FONT, t);
	else if(r == -1)
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
}

/* 91 91: h, str, colour, parseTags, hang →
 * Draw `str` into the window's text layer at the cursor without waiting;
 * `parseTags` interprets the ruby markup.  The colour doubles as the ruby
 * colour; the current shadow style applies. */
static int Opcode_Gfx1_DrawText(Thread_t* t)
{
	int32_t style[5];
	int hang = (int)Thread_Pop(t), parseTags = (int)Thread_Pop(t);
	uint32_t colour = Thread_Pop(t);
	const char* str = (const char*)PopPtr(t);
	uint32_t h = Thread_Pop(t);
	Text_GetStyle(style);
	Gfx1_DrawTextResult(t, GfxCall_WindowDrawText(h, str, parseTags, hang, colour, colour, style));
	return 0;
}

// 91 93: h, str, colour, plainStyle, parseTags, hang →; as "91 91", plainStyle drops the shadow
static int Opcode_Gfx1_DrawTextStyled(Thread_t* t)
{
	int32_t style[5];
	int hang = (int)Thread_Pop(t), parseTags = (int)Thread_Pop(t), plainStyle = (int)Thread_Pop(t);
	uint32_t colour = Thread_Pop(t);
	const char* str = (const char*)PopPtr(t);
	uint32_t h = Thread_Pop(t);
	if(plainStyle)
		Text_MakeStyle(style, 0, 0, 0, 0, 0);
	else
		Text_GetStyle(style);
	Gfx1_DrawTextResult(t, GfxCall_WindowDrawText(h, str, parseTags, hang, colour, colour, style));
	return 0;
}

// the ruby dictionary: a null word clears it, a null reading removes the word, otherwise the pair is added
static int Opcode_Gfx1_RubySet(Thread_t* t) // 91 94: word, reading →
{
	const char* reading = (const char*)PopPtr(t);
	const char* word = (const char*)PopPtr(t);
	GfxCall_RubySet(word, reading);
	return 0;
}

// the ruby words of `str` as "word\reading\n" lines into `tags` (0x400 bytes); their count
static int Opcode_Gfx1_RubyParse(Thread_t* t) // 91 95: str, tags → count
{
	char* tags = (char*)PopPtr(t);
	const char* str = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)Text_ParseTags(tags, str));
	return 0;
}

// add the "word\reading\n" lines of `list` to the ruby dictionary; r is 1 when the whole list was consumed
static int Opcode_Gfx1_RubyAddList(Thread_t* t) // 91 96: list → r
{
	Thread_Push(t, (uint32_t)Ruby_AddList((const char*)PopPtr(t)));
	return 0;
}

/* 91 98: charDelay, fadeSteps, pitch, rubySizePct, rubyMargin, hangBrackets →
 * The rich text parameters: the delay added per character and the steps
 * of its fade-in, the extra pixels per non-proportional character, the
 * ruby size in percent of the font (25 .. 100), the margin left for the
 * readings (the layout's indent, non-negative) and whether a leading
 * bracket hangs into the margin.  A ruby size or margin out of range is a
 * script error. */
static int Opcode_Gfx1_SetRichParams(Thread_t* t)
{
	int hangBrackets = (int)Thread_Pop(t), rubyMargin = (int)Thread_Pop(t), rubySizePct = (int)Thread_Pop(t);
	int pitch = (int)Thread_Pop(t), fadeSteps = (int)Thread_Pop(t), charDelay = (int)Thread_Pop(t);
	switch(Text_SetRichParams(charDelay, fadeSteps, pitch, rubySizePct, rubyMargin, hangBrackets))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BAD_RUBY_SIZE_RATE, rubySizePct); break;
		case(int)0x80000002: GFX_ERROR(t, MSG_BAD_RUBY_MARGIN, rubyMargin); break;
		default: break;
	}
	return 0;
}

/* 91 98 of 1.58: charDelay, fadeSteps, pitch, rubySizePct, hangBrackets →
 * No ruby margin yet, and the delay came in tenths of a step (the build
 * divided it by 10 before counting it down per character).  Only the size
 * is range-checked. */
static int Opcode_Gfx1_SetRichParams_158(Thread_t* t)
{
	int hangBrackets = (int)Thread_Pop(t), rubySizePct = (int)Thread_Pop(t), pitch = (int)Thread_Pop(t);
	int fadeSteps = (int)Thread_Pop(t), charDelay = (int)Thread_Pop(t);
	if(Text_SetRichParams((int)((uint32_t)charDelay / 10), fadeSteps, pitch, rubySizePct, 0, hangBrackets) ==
		(int)0x80000001)
		GFX_ERROR(t, MSG_BAD_RUBY_SIZE_RATE, rubySizePct);
	return 0;
}

// -------------------------------------------------------------------------
// 91 9C, 91 9D : rich text into a bitmap
// -------------------------------------------------------------------------

/* The result map of BmpOp_DrawText: 0x80000001 bad font size, 0x80000002
 * bad font width, 0x80000003 bad font number, 0x80000004 no bitmap in the
 * slot. */
static void Gfx1_BmpTextResult(Thread_t* t, int r, int bmp, int fontNo, int size, int widthPct)
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

/* 91 9C: bmp, x, y, str, parseTags, tags, fontNo, size, widthPct,
 * bold, prop, hang, spacing, colour → lines
 * Lay `str` out as rich text into bitmap `bmp` at (x, y) with the given
 * font and push the number of lines it took; `parseTags` interprets the
 * ruby markup, `tags` is the ruby list "91 95" produced.  The colour
 * doubles as the ruby colour, the current shadow style applies. */
static int Opcode_Gfx1_BmpDrawText(Thread_t* t)
{
	int32_t style[5], lines;
	uint32_t colour = Thread_Pop(t);
	int spacing = (int)Thread_Pop(t), hang = (int)Thread_Pop(t), prop = (int)Thread_Pop(t), bold = (int)Thread_Pop(t);
	int widthPct = (int)Thread_Pop(t), size = (int)Thread_Pop(t), fontNo = (int)Thread_Pop(t);
	const char* tags = (const char*)PopPtr(t);
	int parseTags = (int)Thread_Pop(t);
	const char* str = (const char*)PopPtr(t);
	int y = (int)Thread_Pop(t), x = (int)Thread_Pop(t), bmp = (int)Thread_Pop(t), r;
	CheckBitmapNo(bmp, t);
	CheckFontNo(fontNo, t);
	Text_GetStyle(style);
	r = BmpOp_DrawText(bmp, &lines, x, y, str, parseTags, tags, fontNo, size, widthPct, bold, prop, hang, spacing, colour,
		colour, style);
	Gfx1_BmpTextResult(t, r, bmp, fontNo, size, widthPct);
	Thread_Push(t, (uint32_t)lines);
	return 0;
}

/* 91 9D: bmp, x, y, str, parseTags, tags, fontNo, size, widthPct,
 * bold, prop, hang, spacing, colour, plainStyle → lines
 * As "91 9C" with `plainStyle`, which drops the shadow. */
static int Opcode_Gfx1_BmpDrawTextStyled(Thread_t* t)
{
	int32_t style[5], lines;
	int plainStyle = (int)Thread_Pop(t);
	uint32_t colour = Thread_Pop(t);
	int spacing = (int)Thread_Pop(t), hang = (int)Thread_Pop(t), prop = (int)Thread_Pop(t), bold = (int)Thread_Pop(t);
	int widthPct = (int)Thread_Pop(t), size = (int)Thread_Pop(t), fontNo = (int)Thread_Pop(t);
	const char* tags = (const char*)PopPtr(t);
	int parseTags = (int)Thread_Pop(t);
	const char* str = (const char*)PopPtr(t);
	int y = (int)Thread_Pop(t), x = (int)Thread_Pop(t), bmp = (int)Thread_Pop(t), r;
	CheckBitmapNo(bmp, t);
	CheckFontNo(fontNo, t);
	if(plainStyle)
		Text_MakeStyle(style, 0, 0, 0, 0, 0);
	else
		Text_GetStyle(style);
	r = BmpOp_DrawText(bmp, &lines, x, y, str, parseTags, tags, fontNo, size, widthPct, bold, prop, hang, spacing, colour,
		colour, style);
	Gfx1_BmpTextResult(t, r, bmp, fontNo, size, widthPct);
	Thread_Push(t, (uint32_t)lines);
	return 0;
}

// -------------------------------------------------------------------------
// 91 DB : knobs
// -------------------------------------------------------------------------

// the handle of the knob the mouse is dragging ("90 D0" registry), 0 when none
static int Opcode_Gfx1_KnobDragHandle(Thread_t* t) // 91 DB (1.588 on): → h
{
	Thread_Push(t, Knob_DragHandle());
	return 0;
}

// -------------------------------------------------------------------------
// 91 B8 .. 91 BF : the sprite panel
// -------------------------------------------------------------------------

// a sprite panel proc on window h (SubObj_Create kind 1; an unknown handle still creates one, on no window); its id
static int Opcode_Gfx1_SprPanelCreate(Thread_t* t) // 91 B8: h → id
{
	Thread_Push(t, SubObj_Create(Thread_Pop(t), 1));
	return 0;
}

/* Start the sprite panel `id` from the script's descriptor record.  r: 0
 * ok, 1 no such proc, 2 the descriptor or its group table is unusable,
 * 3 a button table is unusable, 4 the proc is not a sprite panel. */
static int Opcode_Gfx1_SprPanelStart(Thread_t* t) // 91 BA: id, desc → r
{
	const void* desc = PopPtr(t);
	uint32_t id = Thread_Pop(t);
	Thread_Push(t, (uint32_t)SprPanel_Start(id, desc, t));
	return 0;
}

// define user key map `no` (4 .. 7) of the panels: 24 actions, one per standard-key bit; a bad number is a script error
static int Opcode_Gfx1_SetKeyMap(Thread_t* t) // 91 BF: no, actions →
{
	const uint32_t* actions = (const uint32_t*)PopPtr(t);
	int no = (int)Thread_Pop(t);
	if(!Panel_SetKeyMap(no, actions))
		GFX_ERROR(t, MSG_BAD_KEY_ASSIGNMENT, no);
	return 0;
}

// -------------------------------------------------------------------------

/* Fill vm_optable_91 for the selected engine profile.  Each entry names the
 * opcode, its handler and the generations that have it (GEN_FIRST ..
 * GEN_LAST for one that never changed); where an opcode changed meaning
 * between builds (91 8C .. 8F, 91 98) two entries with disjoint ranges
 * cover it, see Vm_FillTable. */
void Vm_Optable91Init(void)
{
	static const OpEntry_t ops[] = {
		{0x00, Opcode_Gfx1_SetPresentAlways, GEN_1_653, GEN_LAST},
		{0x06, Opcode_Gfx1_SetGlobalOffset, GEN_1_616, GEN_LAST},
		{0x55, Opcode_Gfx1_SpriteLinkMask, GEN_1_529, GEN_LAST},
		{0x09, Opcode_Gfx1_SetSizeLimitSelect, GEN_1_653, GEN_LAST},
		{0x0D, Opcode_Gfx1_SetPropFontAdvance, GEN_1_547, GEN_LAST},
		{0x0E, Opcode_Gfx1_SetFaceAdjust, GEN_1_529, GEN_LAST},
		{0x0F, Opcode_Gfx1_FontPreopen, GEN_1_529, GEN_LAST},
		{0x10, Opcode_Gfx1_VecStretchRange, GEN_FIRST, GEN_LAST},
		{0x11, Opcode_Gfx1_VecRandom, GEN_FIRST, GEN_LAST},
		{0x12, Opcode_Gfx1_VecRipple, GEN_FIRST, GEN_LAST},
		{0x13, Opcode_Gfx1_VecPolar, GEN_FIRST, GEN_LAST},
		{0x14, Opcode_Gfx1_VecBendAngle, GEN_FIRST, GEN_LAST},
		{0x15, Opcode_Gfx1_VecBend, GEN_FIRST, GEN_LAST},
		{0x16, Opcode_Gfx1_VecSine, GEN_1_494, GEN_LAST},
		{0x18, Opcode_Gfx1_Xform, GEN_FIRST, GEN_LAST},
		{0x19, Opcode_Gfx1_XformCopy, GEN_FIRST, GEN_LAST},
		{0x1B, Opcode_Gfx1_GatherAdd, GEN_1_494, GEN_LAST},
		{0x1C, Opcode_Gfx1_Resample, GEN_FIRST, GEN_LAST},
		{0x1D, Opcode_Gfx1_ColourMode, GEN_1_529, GEN_LAST},
		{0x1E, Opcode_Gfx1_ApplyGray, GEN_FIRST, GEN_LAST},
		{0x1F, Opcode_Gfx1_Duplicate, GEN_FIRST, GEN_LAST},
		{0x1A, Opcode_Gfx1_BmpColourWithAlpha, GEN_1_69_472, GEN_LAST},
		{0x33, Opcode_Gfx1_ObjSetFixedPos, GEN_FIRST, GEN_LAST},
		{0x36, Opcode_Gfx1_ObjSetFixedOffset2, GEN_1_494, GEN_LAST},
		{0x37, Opcode_Gfx1_ObjSetFixedOffset, GEN_1_69_472, GEN_LAST},
		{0x38, Opcode_Gfx1_ObjGetParam, GEN_1_69_472, GEN_LAST},
		{0x3E, Opcode_Gfx1_ObjAttach, GEN_1_494, GEN_LAST},
		{0x3F, Opcode_Gfx1_ObjDetach, GEN_1_494, GEN_LAST},
		{0x97, Opcode_Gfx1_SetRubyFont, GEN_1_529, GEN_LAST},
		{0x99, Opcode_Gfx1_SetTextScaleDivisor, GEN_1_529, GEN_LAST},
		{0x9B, Opcode_Gfx1_MeasureText, GEN_1_494, GEN_LAST},
		{0x9A, Opcode_Gfx1_SetTextFunction, GEN_1_529, GEN_LAST},
		{0x9E, Opcode_Gfx1_TextCountLinks, GEN_1_529, GEN_LAST},
		{0x9F, Opcode_Gfx1_TextStripTags, GEN_1_529, GEN_LAST},
		{0xF0, Opcode_Gfx1_VideoTextureOpen, GEN_1_69_451, GEN_LAST},
		{0xF1, Opcode_Gfx1_VideoTextureState, GEN_1_69_451, GEN_LAST},
		{0xF2, Opcode_Gfx1_VideoTextureSlotOp, GEN_1_69_451, GEN_LAST},
		{0xF3, Opcode_Gfx1_VideoTextureControl1, GEN_1_69_472, GEN_LAST},
		{0xF4, Opcode_Gfx1_VideoTextureControl4, GEN_1_69_472, GEN_LAST},
		{0xF5, Opcode_Gfx1_VideoTextureSlotOp, GEN_1_69_472, GEN_LAST},
		{0xF6, Opcode_Gfx1_VideoTextureSlotOp, GEN_1_69_472, GEN_LAST},
		{0xF7, Opcode_Gfx1_VideoTextureFrame, GEN_1_494, GEN_LAST},
		{0x40, Opcode_Gfx1_BgSetLayers, GEN_FIRST, GEN_LAST},
		{0x41, Opcode_Gfx1_BglSelect, GEN_FIRST, GEN_LAST},
		{0x42, Opcode_Gfx1_BglShow, GEN_FIRST, GEN_LAST},
		{0x43, Opcode_Gfx1_BglSetPos, GEN_FIRST, GEN_LAST},
		{0x44, Opcode_Gfx1_BglSetEffect, GEN_FIRST, GEN_LAST},
		{0x45, Opcode_Gfx1_BglSetLevel, GEN_FIRST, GEN_LAST},
		{0x46, Opcode_Gfx1_BglSetBitmap, GEN_FIRST, GEN_LAST},
		{0x47, Opcode_Gfx1_BglSetTransform, GEN_FIRST, GEN_LAST},
		{0x48, Opcode_Gfx1_BglSetCurves, GEN_FIRST, GEN_LAST},
		{0x49, Opcode_Gfx1_BglSetCentreDelta, GEN_FIRST, GEN_LAST},
		{0x4A, Opcode_Gfx1_BglSetXformDelta, GEN_FIRST, GEN_LAST},
		{0x60, Opcode_Gfx1_EffectorCreate, GEN_FIRST, GEN_LAST},
		{0x61, Opcode_Gfx1_EffectorDelete, GEN_FIRST, GEN_LAST},
		{0x64, Opcode_Gfx1_EffectorShow, GEN_FIRST, GEN_LAST},
		{0x65, Opcode_Gfx1_EffectorSetVector, GEN_FIRST, GEN_LAST},
		{0x66, Opcode_Gfx1_EffectorSetGradient, GEN_FIRST, GEN_LAST},
		{0x67, Opcode_Gfx1_EffectorSetRipple, GEN_FIRST, GEN_LAST},
		{0x68, Opcode_Gfx1_EffectorSetZoom, GEN_FIRST, GEN_LAST},
		{0x88, Opcode_Gfx1_WindowSetFont, GEN_FIRST, GEN_LAST},
		{0x89, Opcode_Gfx1_WindowSetSpacing, GEN_FIRST, GEN_LAST},
		{0x8A, Opcode_Gfx1_WindowSetDrawStyle, GEN_FIRST, GEN_LAST},
		{0x8B, Opcode_Gfx1_WindowSetSwingStyle, GEN_FIRST, GEN_LAST},
		/* until 1.64 the text-layer operations of the window sat here (91 8C
		 * .. 8F); 1.66 moved them to 92 8C / 92 8D / 92 8E / 91 8D and put the
		 * cursor operations at 91 8C .. 8E */
		{0x8C, Opcode_Gfx2_WindowShowText, GEN_1_58, GEN_1_64},
		{0x8C, Opcode_Gfx1_WindowSetCursor, GEN_1_66, GEN_LAST},
		{0x8D, Opcode_Gfx2_WindowDrawToText, GEN_1_58, GEN_1_64},
		{0x8D, Opcode_Gfx1_WindowGetCursor, GEN_1_66, GEN_LAST},
		{0x8E, Opcode_Gfx2_WindowClear, GEN_1_58, GEN_1_64},
		{0x8E, Opcode_Gfx1_WindowCursorStatus, GEN_1_66, GEN_LAST},
		{0x8F, Opcode_Gfx1_WindowGetCursor, GEN_1_58, GEN_1_64},
		{0x90, Opcode_Gfx1_TextOut, GEN_FIRST, GEN_LAST},
		{0x91, Opcode_Gfx1_DrawText, GEN_FIRST, GEN_LAST},
		{0x92, Opcode_Gfx1_TextOutStyled, GEN_FIRST, GEN_LAST},
		{0x93, Opcode_Gfx1_DrawTextStyled, GEN_FIRST, GEN_LAST},
		{0x94, Opcode_Gfx1_RubySet, GEN_FIRST, GEN_LAST},
		{0x95, Opcode_Gfx1_RubyParse, GEN_FIRST, GEN_LAST},
		{0x96, Opcode_Gfx1_RubyAddList, GEN_FIRST, GEN_LAST},
		{0x98, Opcode_Gfx1_SetRichParams_158, GEN_1_58, GEN_1_58},
		{0x98, Opcode_Gfx1_SetRichParams, GEN_1_64, GEN_LAST},
		{0x9C, Opcode_Gfx1_BmpDrawText, GEN_FIRST, GEN_LAST},
		{0x9D, Opcode_Gfx1_BmpDrawTextStyled, GEN_FIRST, GEN_LAST},
		{0xB8, Opcode_Gfx1_SprPanelCreate, GEN_FIRST, GEN_LAST},
		{0xBA, Opcode_Gfx1_SprPanelStart, GEN_FIRST, GEN_LAST},
		{0xBF, Opcode_Gfx1_SetKeyMap, GEN_FIRST, GEN_LAST},
		{0xDB, Opcode_Gfx1_KnobDragHandle, GEN_1_588, GEN_LAST},
	};
	Vm_FillTable(vm_optable_91, OPFAM_91, ops, BGI_COUNTOF(ops));
}
