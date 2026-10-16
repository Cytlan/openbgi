/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ops_gfx0.c - the "90 xx" graphics instruction family
 *
 * The handlers of the first graphics family and the table that registers
 * them (Vm_Optable90Init, declared in bgi/vm.h): display control, the
 * bitmap operations, the object tweens, backgrounds, sprites, filters, maps,
 * windows, text output, menus and selections, panels, knobs, groups, movies
 * and hit targets.
 *
 * Every handler pops its operands in the order the original does (the first
 * pop is the value pushed last by the script), validates them with the
 * Check* routines of src/vm/checks.c (which raise a script error and never
 * return), calls the service and maps the service's result code to a script
 * error.  The stack comments use the notation "a, b → r": b is on top of
 * the stack; a handle `h` is a display object handle, `bmp` a bitmap slot
 * number, `level` an effect level 0 .. 0x100, `prio` a draw priority
 * 0 .. 0xFFF, `effect` a blit effect mode.
 *
 * Handlers that install a wait object return scheduler code 2 (end of turn,
 * thread blocked until the object completes); everything else returns 0.
 * GFX_ERROR formats a message into a local buffer and raises the script
 * error with it, as the original does.
 */
#include "bgi/vm.h"
#include "bgi/wait.h"
#include "bgi/waitobj.h"
#include "bgi/error.h"
#include "bgi/file.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/sysobj.h"
#include "bgi/panel.h"
#include "bgi/gfx.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/bmseq.h"
#include "bgi/gfx/text.h"
#include "bgi/gfx/font.h"

VmHandler_t vm_optable_90[256]; // the family's dispatch table, filled by Vm_Optable90Init

// raise a script error on thread t with a formatted MSG_* text; does not return
#define GFX_ERROR(t, ...)           \
	do                              \
	{                               \
		char msg_[0x104];           \
		sprintf(msg_, __VA_ARGS__); \
		ScriptError(msg_, (t));     \
	} while(0)

// -------------------------------------------------------------------------
// 90 00 .. 90 0F : display control
// -------------------------------------------------------------------------

// request a present at the end of the pass: the whole picture, or only the dirty rectangles
static int Opcode_Gfx0_Present(Thread_t* t) // 90 00: full →
{
	if(Thread_Pop(t))
		Present_RequestFull();
	else
		Present_RequestDirty();
	return 0;
}

static int Opcode_Gfx0_DisplayEnable(Thread_t* t) // 90 01: f →
{
	Display_Enable((int)Thread_Pop(t));
	return 0;
}

// the presentation rate: the frame interval becomes 1000 / fps ms
static int Opcode_Gfx0_SetFps(Thread_t* t) // 90 02: fps →
{
	uint32_t fps = Thread_Pop(t);
	// 1.58 accepted 0 .. 1000 as a signed value; from 1.64 the rate must be at least 1
	if(gEngine->gen == GEN_1_58 ? (int)fps < 0 || (int)fps > 1000 : fps < 1 || fps > 1000)
		GFX_ERROR(t, MSG_BAD_FRAME_RATE, (int)fps);
	Present_SetRate((int)fps);
	return 0;
}

// the capacity of the loader's file cache that the bitmap loads consult first (0 = none)
static int Opcode_Gfx0_SetCacheSize(Thread_t* t) // 90 03: bytes →
{
	uint32_t size = Thread_Pop(t);
	// 1.58 / 1.64 allowed up to 64 MB, 1.66 on 512 MB
	uint32_t limit = gEngine->gen <= GEN_1_64 ? 0x4000000 : 0x20000000;
	if(gEngine->gen == GEN_1_58 ? (int)size < 0 || size > limit : size > limit)
		GFX_ERROR(t, MSG_BAD_CACHE_SIZE, (int)size);
	Loader_SetCacheSize(size);
	return 0;
}

// create bitmap `bmp` at screen size and copy the back buffer into it
static int Opcode_Gfx0_Snapshot(Thread_t* t) // 90 04: bmp →
{
	int bmp = (int)Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	GfxCall_Snapshot(bmp);
	return 0;
}

// as "90 04", but the picture is re-composed from the objects up to priority `prio`
static int Opcode_Gfx0_SnapshotPrio(Thread_t* t) // 90 05: bmp, prio →
{
	uint32_t prio = Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckPriority(prio, t);
	CheckBitmapNo(bmp, t);
	GfxCall_SnapshotPrio(bmp, (int)prio);
	return 0;
}

/* the global effect value (gGlobalEffectValue) every object's draw
 * consults; the display is redrawn in full */
static int Opcode_Gfx0_SetGlobalEffect(Thread_t* t) // 90 08: v →
{
	GfxCall_SetGlobalEffect((int)Thread_Pop(t));
	return 0;
}

// the compositor's draw limit: objects below priority `prio` are not drawn
static int Opcode_Gfx0_SetDrawLimit(Thread_t* t) // 90 09: prio →
{
	uint32_t prio = Thread_Pop(t);
	CheckPriority(prio, t);
	GfxCall_SetDrawLimit((int)prio);
	return 0;
}

/* how a wait object presents what it drew: `onWait` whether it requests a
 * present at all when it is dirty, `full` whether that present is the whole
 * picture rather than the dirty rectangles */
static int Opcode_Gfx0_WaitPresentMode(Thread_t* t) // 90 0A: onWait, full →
{
	int full = (int)Thread_Pop(t);
	int onWait = (int)Thread_Pop(t);
	Wait_SetPresentMode(onWait, full);
	return 0;
}

// the bitmap manager's option: a re-created slot keeps its generation number when set
static int Opcode_Gfx0_BmpMgrOption(Thread_t* t) // 90 0B: v →
{
	BmpMgr_SetOption((int)Thread_Pop(t));
	return 0;
}

/* the visibility switch and the transparency added to all message windows;
 * every window is repainted */
static int Opcode_Gfx0_WindowGlobal(Thread_t* t) // 90 0C: shown, level →
{
	uint32_t level = Thread_Pop(t);
	int shown = (int)Thread_Pop(t);
	CheckAlpha(level, t);
	GfxCall_WindowGlobal(shown, (int)level);
	return 0;
}

// the font anti-aliasing level, 0 .. 3
static int Opcode_Gfx0_SetAntialias(Thread_t* t) // 90 0D: level →
{
	int level = (int)Thread_Pop(t);
	if(!Gfx_SetAntialias(level))
		GFX_ERROR(t, MSG_BAD_AA_LEVEL, level);
	return 0;
}

/* the font cache request of "90 0E": font number `fontNo` resolved to its
 * face name, then BmpMgr_FontRequest with its result (0 ok, 0x80000001
 * amount below 2, 0x80000002 bad size, 0x80000003 bad width percentage);
 * 0x80000004 for an unknown font number */
static int FontCacheRequest(int fontNo, int size, int widthPct, int bold, int amount)
{
	const char* name = FontNameByNo(fontNo);
	if(!name)
		return (int)0x80000004;
	return BmpMgr_FontRequest(gBmpMgr, name, size, widthPct, bold, amount);
}

/* Remember how many glyphs (`amount`) the glyph cache is to hold for the
 * font of number `fontNo` at `size` pixels, `widthPct` percent width and
 * weight `bold`.  The error messages are crossed in the original and kept
 * so: a bad cache amount (0x80000001) reports "invalid font number" with
 * the font number, and an unknown font number (0x80000004) reports
 * "invalid cache amount" with the amount. */
static int Opcode_Gfx0_FontCache(Thread_t* t) // 90 0E: fontNo, size, widthPct, bold, amount →
{
	int amount = (int)Thread_Pop(t);
	int bold = (int)Thread_Pop(t);
	int widthPct = (int)Thread_Pop(t);
	int size = (int)Thread_Pop(t);
	int fontNo = (int)Thread_Pop(t);
	switch(FontCacheRequest(fontNo, size, widthPct, bold, amount))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BAD_FONT_NO, fontNo); break;
		case(int)0x80000002: GFX_ERROR(t, MSG_BAD_FONT_SIZE, size); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_BAD_FONT_WIDTH, widthPct); break;
		case(int)0x80000004: GFX_ERROR(t, MSG_BAD_FONT_CACHE_AMOUNT, amount); break;
		default: break;
	}
	return 0;
}

/* the key colour (gBmpOption, 0 = none) that an ARGB32 picture put with
 * "90 14" was composited over; the manager undoes that composition to
 * recover the original colours */
static int Opcode_Gfx0_BmpOption(Thread_t* t) // 90 0F: v →
{
	Gfx_SetBmpOption((int)Thread_Pop(t));
	return 0;
}

// -------------------------------------------------------------------------
// 90 10 .. 90 1F : bitmaps
// -------------------------------------------------------------------------

/* The window of synchronous loading set by "90 07" (1.69 build 472 on).
 * While it is open every "90 10" load is done synchronously; it opens with
 * the first load after "90 07" and closes gSyncLoadMs later unless the skip
 * key keeps it open.  With no window set the skip key alone decides, as in
 * the builds before 1.69/472. */
static uint32_t gSyncLoadMs;    // the length of the window in ms; 0 = no window set
static uint32_t gSyncLoadUntil; // the tick the open window closes at; 0 = not open

// whether the "90 10" load about to start is done synchronously (1) or on the loader thread (0)
static int BmpLoad_Synchronous(void)
{
	int skip = Input_CheckSkip();
	if(gSyncLoadMs)
	{
		if(gSyncLoadUntil == 0)
		{
			gSyncLoadUntil = GetTicks() + gSyncLoadMs;
			return 1;
		}
		if(skip || GetTicks() < gSyncLoadUntil)
			return 1;
		gSyncLoadUntil = 0;
		return 0;
	}
	if(!skip)
		gSyncLoadUntil = 0;
	return skip;
}

// set the length of the synchronous-loading window (see BmpLoad_Synchronous) and close an open one
static int Opcode_Gfx0_SetSyncLoadWindow(Thread_t* t) // 90 07 (1.69/472 on): ms →
{
	gSyncLoadMs = Thread_Pop(t);
	gSyncLoadUntil = 0;
	return 0;
}

// the vanishing point of the projected sprites ("90 5C"); (-1, -1) restores the back-buffer centre
static int Opcode_Gfx0_SetProjCentre(Thread_t* t) // 90 06 (1.529 on): x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	Gfx_SetProjCentre(gGfx, x, y);
	return 0;
}

/* Convert the bitmap in slot `bmp` to pixel mode `mode`.  The result is
 * 0 ok, 1 no bitmap in the slot, 2 the conversion is not possible; for any
 * other manager code the pushed value is `mode` itself. */
static int Opcode_Gfx0_BmpSetMode(Thread_t* t) // 90 17 (1.494 on): bmp, mode → r
{
	int mode = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	uint32_t r = BmpMgr_SetMode(gBmpMgr, bmp, mode);
	Thread_Push(t, r == 0 ? 0 : r == 0xb ? 1
			: r == 0x15                  ? 2
										 : (uint32_t)mode);
	return 0;
}

/* Load the image file `name` of archive `arc` (a Windows BMP or the
 * engine's own compressed formats) into slot `bmp`.  Normally the load runs
 * on the loader thread behind a WaitBmpLoad object and the thread blocks;
 * while the player skips (or the "90 07" window is open) it is done at
 * once so the script keeps up, and only then are the decoder's errors
 * reported. */
static int Opcode_Gfx0_BmpLoad(Thread_t* t) // 90 10: bmp, arc, name →
{
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	int bmp = (int)Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	if(!BmpLoad_Synchronous())
	{
		Thread_SetWait(t, WaitBmpLoad_New(t, bmp, arc, name));
		return 2;
	}
	switch(BmpOp_LoadFile(bmp, arc, name))
	{
		case(int)0x80000002: GFX_ERROR(t, MSG_BMP_NOT_WINDOWS, arc, name); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_BMP_BAD_PLANES, arc, name); break;
		case(int)0x80000004: GFX_ERROR(t, MSG_BMP_BAD_BITCOUNT, arc, name); break;
		case(int)0x80000005: GFX_ERROR(t, MSG_BMP_COMPRESSED, arc, name); break;
		case(int)0x80000006: GFX_ERROR(t, MSG_BMP_BAD_SIZE, arc, name); break;
		case(int)0x80000008: GFX_ERROR(t, MSG_BMP_OUT_OF_MEMORY, arc, name); break;
		default: break; // 0, 0x80000001 (not a BMP) and 0x80000007 (the manager refused) pass silently
	}
	return 0;
}

// allocate an empty bitmap of w x h pixels in pixel mode `mode` (0 .. 6) in the slot
static int Opcode_Gfx0_BmpCreate(Thread_t* t) // 90 11: bmp, w, h, mode →
{
	int mode = (int)Thread_Pop(t);
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	if(!BmpOp_Create(bmp, w, h, mode))
		GFX_ERROR(t, MSG_BAD_PIXEL_MODE_NO, mode);
	return 0;
}

// release the slot; the result is 1 when it held a bitmap, 0 when it was already free
static int Opcode_Gfx0_BmpFree(Thread_t* t) // 90 12: bmp → wasInUse
{
	Thread_Push(t, (uint32_t)BmpOp_Free((int)Thread_Pop(t)));
	return 0;
}

// fill the bitmap with `colour` (0 clears it)
static int Opcode_Gfx0_BmpClear(Thread_t* t) // 90 13: bmp, colour →
{
	uint32_t colour = Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	if(!BmpOp_Clear(bmp, colour))
		ErrBitmapNotRegistered(bmp, t);
	return 0;
}

/* create the bitmap from packed pixels in script memory (24 bpp for the
 * RGB32 mode, the native layout otherwise); an ARGB32 picture is
 * un-composited against the "90 0F" key colour.  A failure is not reported. */
static int Opcode_Gfx0_BmpPut(Thread_t* t) // 90 14: bmp, w, h, mode, data →
{
	const void* data = PopPtr(t);
	int mode = (int)Thread_Pop(t);
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	BmpOp_Put(bmp, w, h, mode, data);
	return 0;
}

/* copy the bitmap's pixels, packed as "90 14" takes them, into the `size`
 * bytes at `dst`; the second operand is popped but never read.  Errors:
 * 9 no bitmap in the slot, 10 the buffer is too small. */
static int Opcode_Gfx0_BmpGet(Thread_t* t) // 90 15: dst, unused, size, bmp →
{
	int bmp = (int)Thread_Pop(t);
	int size = (int)Thread_Pop(t);
	void* unused = PopPtr(t);
	void* dst = PopPtr(t);
	CheckBitmapNo(bmp, t);
	switch(BmpOp_Get(dst, (int)(intptr_t)unused, size, bmp))
	{
		case 9: GFX_ERROR(t, MSG_BMP_NOT_EXIST, bmp); break;
		case 10: GFX_ERROR(t, MSG_BMP_GET_BAD_SIZE, size); break;
		default: break;
	}
	return 0;
}

/* Write the bitmap's descriptor to the 24 bytes at `out` in the script's
 * layout (a 32-bit pixel pointer, always written as 0, then pitch, w, h,
 * mode, bpp); the result is 1 when the slot holds a bitmap.  The host Bmp_t
 * has a wider pointer, so the fields are written one by one. */
static int Opcode_Gfx0_BmpInfo(Thread_t* t) // 90 16: out, bmp → ok
{
	int bmp = (int)Thread_Pop(t);
	int32_t* out = (int32_t*)PopPtr(t);
	Bmp_t b;
	int ok = BmpOp_GetInfo(&b, bmp);
	if(ok) // a failed lookup leaves the descriptor alone, except for the pointer
	{
		out[1] = b.pitch;
		out[2] = b.w;
		out[3] = b.h;
		out[4] = b.mode;
		out[5] = b.bpp;
	}
	out[0] = 0; // the pixel pointer
	Thread_Push(t, (uint32_t)ok);
	return 0;
}

// blit the whole of `src` onto `dst` at (x, y) with an effect mode and level
static int Opcode_Gfx0_BmpBlit(Thread_t* t) // 90 18: dst, x, y, src, effect, level →
{
	uint32_t level = Thread_Pop(t);
	uint32_t effect = Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	CheckBitmapNo(dst, t);
	CheckBitmapNo(src, t);
	CheckEffectMode(effect, t);
	CheckAlpha(level, t);
	switch(BmpOp_Blit(dst, x, y, src, (int)effect, (int)level))
	{
		case 1: GFX_ERROR(t, MSG_BMP_DST_NOT_EXIST, dst); break;
		case 2: GFX_ERROR(t, MSG_BMP_SRC_NOT_EXIST, src); break;
		case 3: GFX_ERROR(t, MSG_BMP_MODE_MISMATCH, dst, src); break;
		default: break;
	}
	return 0;
}

/* Blit `src` onto `dst` through the gray-scale bitmap `gray`: the gray
 * value and `level` decide per pixel how much of the source shows.  `param`
 * selects the transition curve: 0 .. 7 a ramp over the gray value scaled by
 * 1 << param, 8 and above a triangle wave with 2 * (param & 7) + 1
 * segments.  From 1.535 on the gray slot is not validated: when it holds no
 * bitmap the blit is a plain one with effect 0x80. */
static int Opcode_Gfx0_BmpBlitMask(Thread_t* t) // 90 19: dst, x, y, src, gray, param, level →
{
	uint32_t level = Thread_Pop(t);
	int param = (int)Thread_Pop(t);
	int gray = (int)Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	CheckBitmapNo(dst, t);
	CheckBitmapNo(src, t);
	if(gEngine->gen >= GEN_1_535)
	{ // the gray is no longer checked; without one the blit is a plain one
		Bmp_t g;
		CheckAlpha(level, t);
		if(!BmpOp_GetInfo(&g, gray))
		{
			switch(BmpOp_Blit(dst, x, y, src, 0x80, (int)level))
			{
				case 1: GFX_ERROR(t, MSG_BMP_DST_NOT_EXIST, dst); break;
				case 2: GFX_ERROR(t, MSG_BMP_SRC_NOT_EXIST, src); break;
				default: break;
			}
			return 0;
		}
	}
	else
	{
		CheckBitmapNo(gray, t);
		CheckAlpha(level, t);
	}
	switch(BmpOp_BlitMask(dst, x, y, src, gray, param, (int)level))
	{
		case 1: GFX_ERROR(t, MSG_BMP_DST_NOT_EXIST, dst); break;
		case 2: GFX_ERROR(t, MSG_BMP_SRC_NOT_EXIST, src); break;
		case 3: GFX_ERROR(t, MSG_GRAY_NOT_EXIST, gray); break;
		case 4: GFX_ERROR(t, MSG_BMP_MODE_MISMATCH, dst, src); break;
		case 5: GFX_ERROR(t, MSG_BMP_NOT_GRAY, gray); break;
		case 6: GFX_ERROR(t, MSG_GRAY_BAD_SIZE, gray); break;
		default: break;
	}
	return 0;
}

/* draw `src` into `dst` displaced per pixel by the vector map `vec1`:
 * `level` scales the displacement, or, with a second map `vec2` (-1 =
 * none), blends between the two maps; a non-zero `amount` selects bilinear
 * sampling */
static int Opcode_Gfx0_BmpDisplace(Thread_t* t) // 90 1A: dst, src, vec1, vec2, level, amount →
{
	int amount = (int)Thread_Pop(t);
	int level = (int)Thread_Pop(t);
	int vec2 = (int)Thread_Pop(t);
	int vec1 = (int)Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	switch(BmpOp_Displace(dst, src, vec1, vec2, level, amount))
	{
		case 1: GFX_ERROR(t, MSG_STORE_BMP_INVALID, dst); break;
		case 2: GFX_ERROR(t, MSG_REF_BMP_INVALID, src); break;
		case 3: GFX_ERROR(t, MSG_STORE_REF_MODE_MISMATCH, dst, src); break;
		case 4: GFX_ERROR(t, MSG_VEC1_INVALID, vec1); break;
		case 5: GFX_ERROR(t, MSG_VEC1_UNSUITABLE, vec1, dst); break;
		case 6: GFX_ERROR(t, MSG_VEC2_INVALID, vec2); break;
		case 7: GFX_ERROR(t, MSG_VEC2_UNSUITABLE, vec2, dst); break;
		case 8: GFX_ERROR(t, MSG_BAD_EFFECT_LEVEL_SHORT, level); break;
		default: break;
	}
	return 0;
}

/* a horizontal box blur of `src` into `dst` (same size and mode) with a
 * window of 2 * level + 1 pixels; `type` 0 pads the edges with black, 1
 * repeats the edge pixel */
static int Opcode_Gfx0_BmpGradient(Thread_t* t) // 90 1B: dst, src, type, level →
{
	int level = (int)Thread_Pop(t);
	int type = (int)Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	switch(BmpOp_Gradient(dst, src, type, level))
	{
		case 1: GFX_ERROR(t, MSG_STORE_BMP_INVALID, dst); break;
		case 2: GFX_ERROR(t, MSG_REF_BMP_INVALID, src); break;
		case 3: GFX_ERROR(t, MSG_STORE_REF_SIZE_MODE_MISMATCH, dst, src); break;
		case 4: GFX_ERROR(t, MSG_BAD_GRADIENT_TYPE, type); break;
		case 5: GFX_ERROR(t, MSG_BAD_EFFECT_LEVEL_SHORT, level); break;
		default: break;
	}
	return 0;
}

/* draw the (sx, sy, sw, sh) part of `src` scaled into the (x, y, w, h)
 * rectangle of `dst`; both sizes must be at least 2 x 2 */
static int Opcode_Gfx0_BmpStretch(Thread_t* t) // 90 1C: dst, x, y, w, h, src, sx, sy, sw, sh →
{
	int sh = (int)Thread_Pop(t);
	int sw = (int)Thread_Pop(t);
	int sy = (int)Thread_Pop(t);
	int sx = (int)Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	switch(BmpOp_Stretch(dst, x, y, w, h, src, sx, sy, sw, sh))
	{
		case 1: GFX_ERROR(t, MSG_STORE_BMP_INVALID, dst); break;
		case 2: GFX_ERROR(t, MSG_REF_BMP_INVALID, src); break;
		case 5: GFX_ERROR(t, MSG_BAD_OUTPUT_RANGE, w, h); break;
		case 6: GFX_ERROR(t, MSG_BAD_REF_RANGE, sw, sh); break;
		case 8: GFX_ERROR(t, MSG_STORE_REF_MODE_MISMATCH, dst, src); break;
		default: break; // 3, 4, 7 pass silently
	}
	return 0;
}

/* draw `src` rotated by `angle` (16.16 degrees) and scaled by `zoom` (16.16,
 * above 0) with its centre at the centre of `dst`, nearest-neighbour sampled */
static int Opcode_Gfx0_BmpRotate(Thread_t* t) // 90 1D: dst, src, zoom, angle →
{
	int32_t angle = (int32_t)Thread_Pop(t);
	int32_t zoom = (int32_t)Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	switch(BmpOp_Rotate(dst, src, zoom, angle))
	{
		case 1: GFX_ERROR(t, MSG_STORE_BMP_INVALID, dst); break;
		case 2: GFX_ERROR(t, MSG_REF_BMP_INVALID, src); break;
		case 3: GFX_ERROR(t, MSG_BMP_INCOMPATIBLE, dst, src); break;
		case 4: GFX_ERROR(t, MSG_BAD_SCALE, zoom); break;
		default: break;
	}
	return 0;
}

/* plain copy of the (sx, sy, w, h) part of `src` to (dx, dy) of `dst`; an
 * empty size is not an error */
static int Opcode_Gfx0_BmpCopyRect(Thread_t* t) // 90 1E: dst, dx, dy, src, sx, sy, w, h →
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int sy = (int)Thread_Pop(t);
	int sx = (int)Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int dy = (int)Thread_Pop(t);
	int dx = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	switch(BmpOp_CopyRect(dst, dx, dy, src, sx, sy, w, h))
	{
		case 1: GFX_ERROR(t, MSG_XFER_DST_INVALID, dst); break;
		case 2: GFX_ERROR(t, MSG_XFER_SRC_INVALID, src); break;
		default: break;
	}
	return 0;
}

// slot `dst` becomes a new w x h bitmap holding the part of `src` at (x, y)
static int Opcode_Gfx0_BmpCloneRect(Thread_t* t) // 90 1F: dst, src, x, y, w, h →
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	switch(BmpOp_CloneRect(dst, src, x, y, w, h))
	{
		case 1: GFX_ERROR(t, MSG_OUT_BMP_INVALID, dst); break;
		case 2: GFX_ERROR(t, MSG_COPY_SRC_INVALID, src); break;
		case 3: GFX_ERROR(t, MSG_BAD_COPY_RANGE, w, h); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 90 20 .. 90 2C : object animation waits (src/wait/wait_tween.c)
// -------------------------------------------------------------------------

/* The instructions of this section animate a display object over `duration`
 * ms at `fps` updates per second and block the thread until the animation
 * ends.  `curve` is an Ease() curve number, `maxSkip` the number of frames
 * the clock may jump per update (0 = unlimited), `skip` whether a click or
 * key ends the wait early (through an input layer at priority `prio`). */

/* map the result of a tween starter to a script error: 0x80000001 a frame
 * rate below 1 (reported with `fps`), -1 no object with that handle */
static void TweenResult(Thread_t* t, int r, int fps)
{
	if(r == (int)0x80000001)
		GFX_ERROR(t, MSG_BAD_FRAME_RATE, fps);
	if(r == -1)
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
}

// fade the object's effect level to `level`
static int Opcode_Gfx0_ObjFade(Thread_t* t) // 90 20: h, level, duration, fps, skip, prio →
{
	uint32_t prio = Thread_Pop(t);
	int skip = (int)Thread_Pop(t);
	int fps = (int)Thread_Pop(t);
	int duration = (int)Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	TweenResult(t, StartObjTween(t, h, 0, 0, 0, 0, (int)level, duration, fps, 0, skip, prio), fps);
	return 2;
}

// "90 20" with a frame-skip limit
static int Opcode_Gfx0_ObjFadeEx(Thread_t* t) // 90 22: h, level, duration, fps, maxSkip, skip, prio →
{
	uint32_t prio = Thread_Pop(t);
	int skip = (int)Thread_Pop(t);
	int maxSkip = (int)Thread_Pop(t);
	int fps = (int)Thread_Pop(t);
	int duration = (int)Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	TweenResult(t, StartObjTween(t, h, 0, 0, 0, 0, (int)level, duration, fps, maxSkip, skip, prio), fps);
	return 2;
}

// move the object to (x, y) along `curve` while fading its level to `level`
static int Opcode_Gfx0_ObjMove(Thread_t* t) // 90 21: h, x, y, curve, level, duration, fps, skip, prio →
{
	uint32_t prio = Thread_Pop(t);
	int skip = (int)Thread_Pop(t);
	int fps = (int)Thread_Pop(t);
	int duration = (int)Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	int curve = (int)Thread_Pop(t);
	int32_t y = (int32_t)Thread_Pop(t);
	int32_t x = (int32_t)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	TweenResult(t, StartObjTween(t, h, x, y, curve, 1, (int)level, duration, fps, 0, skip, prio), fps);
	return 2;
}

// "90 21" with a frame-skip limit
static int Opcode_Gfx0_ObjMoveEx(Thread_t* t) // 90 23: h, x, y, curve, level, duration, fps, maxSkip, skip, prio →
{
	uint32_t prio = Thread_Pop(t);
	int skip = (int)Thread_Pop(t);
	int maxSkip = (int)Thread_Pop(t);
	int fps = (int)Thread_Pop(t);
	int duration = (int)Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	int curve = (int)Thread_Pop(t);
	int32_t y = (int32_t)Thread_Pop(t);
	int32_t x = (int32_t)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	TweenResult(t, StartObjTween(t, h, x, y, curve, 1, (int)level, duration, fps, maxSkip, skip, prio), fps);
	return 2;
}

/* move the object to (x, y) through the via point (viaX, viaY); the x
 * coordinates must advance in one direction (0x80000002 otherwise) */
static int Opcode_Gfx0_ObjMove2(Thread_t* t) // 90 24: h, viaX, viaY, x, y, curve, level, duration, fps, maxSkip, skip, prio →
{
	uint32_t prio = Thread_Pop(t);
	int skip = (int)Thread_Pop(t);
	int maxSkip = (int)Thread_Pop(t);
	int fps = (int)Thread_Pop(t);
	int duration = (int)Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	int curve = (int)Thread_Pop(t);
	int32_t y = (int32_t)Thread_Pop(t);
	int32_t x = (int32_t)Thread_Pop(t);
	int32_t viaY = (int32_t)Thread_Pop(t);
	int32_t viaX = (int32_t)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	int r;
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	r = StartObjMove2(t, h, viaX, viaY, x, y, curve, (int)level, duration, fps, maxSkip, skip, prio);
	if(r == (int)0x80000001)
		GFX_ERROR(t, MSG_BAD_FRAME_RATE, fps);
	if(r == (int)0x80000002)
		GFX_ERROR(t, MSG_MOVE2_BAD_X_VECTOR, viaX, x);
	if(r == -1)
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 2;
}

/* validate the progress argument of "90 28" / "90 29": 0 .. 0x100, checked
 * as a level; from 1.69 build 472 on -1 is allowed as well (it leaves the
 * object's progress alone) and the error is "invalid addition level"
 * (無効なアディションレベル) */
static void CheckProgressArg(uint32_t v, Thread_t* t)
{
	if(gEngine->gen >= GEN_1_69_472)
	{
		if((int32_t)v < -1 || (int32_t)v > 0x100)
			GFX_ERROR(t, MSG_BAD_ADDITION_LEVEL, (int)v);
	}
	else
		CheckAlpha(v, t);
}

/* move the object to (x, y) along `curve`, fade its level to `level` along
 * `levelCurve` and bring its progress value to `progress` */
static int Opcode_Gfx0_ObjTweenFull(Thread_t* t) // 90 28: h, x, y, curve, level, levelCurve, progress, duration, fps, maxSkip, skip, prio →
{
	uint32_t prio = Thread_Pop(t);
	int skip = (int)Thread_Pop(t);
	int maxSkip = (int)Thread_Pop(t);
	int fps = (int)Thread_Pop(t);
	int duration = (int)Thread_Pop(t);
	uint32_t progress = Thread_Pop(t);
	int levelCurve = (int)Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	int curve = (int)Thread_Pop(t);
	int32_t y = (int32_t)Thread_Pop(t);
	int32_t x = (int32_t)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	CheckProgressArg(progress, t);
	TweenResult(t,
		StartObjTweenFull(t, h, x, y, curve, (int)level, levelCurve, (int)progress, duration, fps, maxSkip, skip, prio),
		fps);
	return 2;
}

/* as "90 28", but the object moves along a path of `n` (at least 1) control
 * points in script memory, four 16.16 words each (x, y, z, padding) */
static int Opcode_Gfx0_ObjPath(Thread_t* t) // 90 29: h, n, points, curve, level, levelCurve, progress, duration, fps, maxSkip, skip, prio →
{
	uint32_t prio = Thread_Pop(t);
	int skip = (int)Thread_Pop(t);
	int maxSkip = (int)Thread_Pop(t);
	int fps = (int)Thread_Pop(t);
	int duration = (int)Thread_Pop(t);
	uint32_t progress = Thread_Pop(t);
	int levelCurve = (int)Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	int curve = (int)Thread_Pop(t);
	const int32_t* points = (const int32_t*)PopPtr(t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	int r;
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	CheckProgressArg(progress, t);
	r = StartObjPath(t, h, n, points, curve, (int)level, levelCurve, (int)progress, duration, fps, maxSkip, skip, prio);
	if(r == (int)0x80000001)
		GFX_ERROR(t, MSG_BAD_FRAME_RATE, fps);
	if(r == (int)0x80000003)
		GFX_ERROR(t, MSG_BAD_POINT_COUNT, n);
	if(r == -1)
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 2;
}

/* shake the object: `pattern` 0 .. 5 (0, 1, 4 vertical, 2, 3, 5
 * horizontal), `amplitude` in pixels, `frequency` cycles per second,
 * `repeat` cycles, the amplitude decaying by `decay` percent per cycle.
 * An unknown handle is not reported: the turn ends without a wait object. */
static int Opcode_Gfx0_ObjQuake(Thread_t* t) // 90 2C: h, pattern, amplitude, frequency, repeat, decay, fps, skip, prio →
{
	uint32_t prio = Thread_Pop(t);
	int skip = (int)Thread_Pop(t);
	int fps = (int)Thread_Pop(t);
	int decay = (int)Thread_Pop(t);
	int repeat = (int)Thread_Pop(t);
	int frequency = (int)Thread_Pop(t);
	int amplitude = (int)Thread_Pop(t);
	int pattern = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(StartObjQuake(t, h, pattern, amplitude, frequency, repeat, decay, fps, skip, prio))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BAD_QUAKE_PATTERN, pattern); break;
		case(int)0x80000002: GFX_ERROR(t, MSG_BAD_FREQUENCY, frequency); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_BAD_REPEAT_COUNT, repeat); break;
		case(int)0x80000004:
			// a rate of at least 1 that is still below the frequency gets the longer explanation
			if((uint32_t)fps >= 1)
				GFX_ERROR(t, MSG_BAD_FRAME_RATE_FREQ, fps);
			GFX_ERROR(t, MSG_BAD_FRAME_RATE, fps);
			break;
		default: break; // -1 (no such object) is not reported here
	}
	return 2;
}

// -------------------------------------------------------------------------
// 90 30 .. 90 3F : display objects by handle
// -------------------------------------------------------------------------

/* The instructions of this section take the handle of any display object
 * (sprite, window, filter, map, group, knob, ...; 0 is the background) and
 * raise "invalid object handle" when none has it. */

// show (f != 0) or hide the object
static int Opcode_Gfx0_ObjShow(Thread_t* t) // 90 30: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_ObjShow(h, f))
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

// the object's enabled flag (default 1), a second condition of its visibility besides "90 30"
static int Opcode_Gfx0_ObjSetEnabled(Thread_t* t) // 90 31: h, v →
{
	int v = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_ObjSetEnabled(h, v))
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

static int Opcode_Gfx0_ObjSetLevel(Thread_t* t) // 90 32: h, level →
{
	uint32_t level = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckAlpha(level, t);
	if(!GfxCall_ObjSetLevel(h, (int)level))
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

static int Opcode_Gfx0_ObjSetPos(Thread_t* t) // 90 33: h, x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_ObjSetPos(h, x, y))
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

/* the object's opacity, a second multiplier on the effect level (0 .. 0x100,
 * the default) that the children inherit; validated with the level
 * message */
static int Opcode_Gfx0_ObjSetOpacity(Thread_t* t) // 90 39 (1.599 on): h, v →
{
	uint32_t v = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckAlpha(v, t); // the level / alpha message
	if(!GfxCall_ObjSetOpacity(h, (int)v))
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

/* the object's fade value (0 .. 0x100), a multiplier on the effect level
 * that the children inherit; the object tweens animate it */
static int Opcode_Gfx0_ObjSetFade(Thread_t* t) // 90 34: h, v →
{
	uint32_t v = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckAlpha(v, t);
	if(!GfxCall_ObjSetFade(h, (int)v))
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

/* the object's progress value (0 .. 0x100, an integer step), the parameter
 * of the sprite modes that reveal or transform over a progress (wipe,
 * ripple, ...); the children inherit it */
static int Opcode_Gfx0_ObjSetProgress(Thread_t* t) // 90 35: h, v →
{
	uint32_t v = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckAlpha(v, t);
	if(!GfxCall_ObjSetProgress(h, (int)v))
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

// a second draw offset in pixels, added to the "90 37" one and handed to the children as well
static int Opcode_Gfx0_ObjSetOffset2(Thread_t* t) // 90 36 (1.494 on): h, dx, dy →
{
	int dy = (int)Thread_Pop(t);
	int dx = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_ObjSetOffset2(h, dx, dy))
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

// the object's draw offset in pixels, added to its position
static int Opcode_Gfx0_ObjSetOffset(Thread_t* t) // 90 37: h, dx, dy →
{
	int dy = (int)Thread_Pop(t);
	int dx = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_ObjSetOffset(h, dx, dy))
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

// the draw priority of any object (the per-class Set instructions take it as well)
static int Opcode_Gfx0_ObjSetPriority(Thread_t* t) // 90 3A (1.69/472 on): h, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	if(GfxCall_ObjSetPriority(h, (int)prio) == 0xff)
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

/* the object's generic parameter `no` with the values (a, b): the object
 * class decides which numbers it supports and what they mean (the
 * setParam methods of src/gfx).  Errors: 5 the parameter is not supported
 * by that class, 0xfe the values are out of range. */
static int Opcode_Gfx0_ObjSetParam(Thread_t* t) // 90 38: h, no, a, b →
{
	int b = (int)Thread_Pop(t);
	int a = (int)Thread_Pop(t);
	int no = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_ObjSetParam(h, no, a, b))
	{
		case 5: GFX_ERROR(t, MSG_BAD_PARAM_NO, no); break;
		case 0xfe: GFX_ERROR(t, MSG_BAD_PARAM_ARGS, no, a, a, b, b); break;
		case 0xff: ScriptError(MSG_BAD_OBJ_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* the object's hit-test mask: built from the alpha of bitmap `bmp`; -1
 * removes it (the whole rectangle hits), -2 installs a mask that never
 * hits.  Errors: 1 the object has no mask (a virtual object, group or
 * knob), 2 no such bitmap. */
static int Opcode_Gfx0_ObjSetHitMask(Thread_t* t) // 90 3C: h, bmp →
{
	int bmp = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_ObjSetHitMask(h, bmp))
	{
		case 1: ScriptError(MSG_OBJ_IS_VIRTUAL, t); break;
		case 2: GFX_ERROR(t, MSG_BMP_INVALID, bmp); break;
		case 0xff: ScriptError(MSG_BAD_OBJ_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// whether the mouse position is over the object (through its hit mask)
static int Opcode_Gfx0_ObjHitTest(Thread_t* t) // 90 3D: h → hit
{
	uint32_t h = Thread_Pop(t);
	int hit;
	if(!GfxCall_ObjHitTest(&hit, h))
		ScriptError(MSG_BAD_OBJ_HANDLE, t);
	Thread_Push(t, (uint32_t)hit);
	return 0;
}

/* ask the object to build its picture cache (the buildCache method of its
 * class); 3 when the class does not support it (the default method, which
 * no class of this build overrides), 4 an unusable picture size */
static int Opcode_Gfx0_ObjBuildCache(Thread_t* t) // 90 3F: h →
{
	switch(GfxCall_ObjBuildCache(Thread_Pop(t)))
	{
		case 3: ScriptError(MSG_OBJ_NO_CACHE, t); break;
		case 4: ScriptError(MSG_OBJ_BAD_IMAGE_SIZE, t); break;
		case 0xff: ScriptError(MSG_BAD_OBJ_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 90 40 .. 90 4D : the background
// -------------------------------------------------------------------------

/* The background is the screen-sized object at the bottom of the picture
 * (handle 0 of the object instructions).  Each of "90 40" .. "90 4A"
 * replaces it by one of the background types of inc/bgi/gfx/background.h
 * (1 .. 11, in that order) and sets that type up from the bitmaps given;
 * a background bitmap has to be screen sized unless the type says
 * otherwise.  A bitmap number may be the sentinel 0x7000 (black) or 0x7001
 * (white) where noted. */

// type 1: one plain bitmap
static int Opcode_Gfx0_BgBitmap(Thread_t* t) // 90 40: bmp →
{
	int bmp = (int)Thread_Pop(t);
	if(!GfxCall_BgSetBitmap(bmp))
		GFX_ERROR(t, MSG_BG_BMP_UNUSABLE, bmp);
	return 0;
}

// type 2: a cross fade from `bmp` to `second` (a bitmap, black or white) at `level`
static int Opcode_Gfx0_BgFade(Thread_t* t) // 90 41: bmp, second, level →
{
	uint32_t level = Thread_Pop(t);
	int second = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckAlpha(level, t);
	if(!GfxCall_BgSetFade(bmp, second, (int)level))
		GFX_ERROR(t, MSG_BG_BMP2_UNUSABLE, bmp, second);
	return 0;
}

/* type 3: a 2 x 2 tiling of four bitmaps viewed from the origin (x, y),
 * which may run from (0, 0) to (W, H) inclusive */
static int Opcode_Gfx0_BgScroll(Thread_t* t) // 90 42: b0, b1, b2, b3, x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int b3 = (int)Thread_Pop(t);
	int b2 = (int)Thread_Pop(t);
	int b1 = (int)Thread_Pop(t);
	int b0 = (int)Thread_Pop(t);
	switch(GfxCall_BgSetScroll(b0, b1, b2, b3, x, y))
	{
		case 1: GFX_ERROR(t, MSG_BG_SCROLL_BAD_POS, x, y); break;
		case 2: GFX_ERROR(t, MSG_BG_BMP4_UNUSABLE, b0, b1, b2, b3); break;
		default: break;
	}
	return 0;
}

/* type 4: two positioned bitmaps (`bmp1` of any size, `bmp2` a bitmap or a
 * sentinel) with a wipe between them through the screen-sized gray map
 * `gray` (-1 = a plain alpha blend) at `level`; `param` is the transition
 * curve of "90 19" */
static int Opcode_Gfx0_BgBlend(Thread_t* t) // 90 43: x1, y1, bmp1, x2, y2, bmp2, gray, param, level →
{
	uint32_t level = Thread_Pop(t);
	int param = (int)Thread_Pop(t);
	int gray = (int)Thread_Pop(t);
	int bmp2 = (int)Thread_Pop(t);
	int y2 = (int)Thread_Pop(t);
	int x2 = (int)Thread_Pop(t);
	int bmp1 = (int)Thread_Pop(t);
	int y1 = (int)Thread_Pop(t);
	int x1 = (int)Thread_Pop(t);
	CheckAlpha(level, t);
	switch(GfxCall_BgSetBlend(x1, y1, bmp1, x2, y2, bmp2, gray, param, (int)level))
	{
		case 1: GFX_ERROR(t, MSG_BMP_NOT_EXIST, bmp1); break;
		case 2: GFX_ERROR(t, MSG_BMP_NOT_EXIST, bmp2); break;
		case 3: GFX_ERROR(t, MSG_BMP_NOT_EXIST, gray); break;
		case 4: GFX_ERROR(t, MSG_BMP_NOT_GRAY, gray); break;
		case 5: GFX_ERROR(t, MSG_GRAY_UNSUITABLE, gray); break;
		default: break;
	}
	return 0;
}

/* type 5: a flip book of `n` (2 .. 32) bitmaps; `level` is the index of
 * the frame shown.  Up to 32 bitmap numbers are copied out of script
 * memory; the error for an unusable one lists every number the script
 * passed (also beyond 32). */
static int Opcode_Gfx0_BgFrames(Thread_t* t) // 90 44: n, list, level →
{
	int level = (int)Thread_Pop(t);
	const int32_t* list = (const int32_t*)PopPtr(t);
	int n = (int)Thread_Pop(t);
	int32_t local[32];
	int i;
	for(i = 0; i < n && i < 32; i++)
		local[i] = list[i];
	switch(GfxCall_BgSetFrames(n, local, level))
	{
		case 1: GFX_ERROR(t, MSG_BAD_BMP_COUNT, n); break;
		case 2:
		{
			char msg[0x400], tmp[0x400];
			sprintf(msg, MSG_BG_LIST_HEAD);
			for(i = 0; i < n; i++)
			{
				sprintf(tmp, i < n - 1 ? "%s%d , " : MSG_BG_LIST_TAIL, msg, list[i]);
				strcpy(msg, tmp);
			}
			ScriptError(msg, t);
			break;
		}
		default: break;
	}
	return 0;
}

// type 6: `bmp` displaced by the screen-sized vector maps `vec1` and `vec2` (-1 = none), as "90 1A"
static int Opcode_Gfx0_BgDisplace(Thread_t* t) // 90 45: bmp, vec1, vec2, level, amount →
{
	int amount = (int)Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	int vec2 = (int)Thread_Pop(t);
	int vec1 = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckAlpha(level, t);
	switch(GfxCall_BgSetDisplace(bmp, vec1, vec2, (int)level, amount))
	{
		case 1: GFX_ERROR(t, MSG_BMP_NOT_EXIST, bmp); break;
		case 2: GFX_ERROR(t, MSG_BMP_NOT_SCREEN_SIZE, bmp); break;
		case 3: GFX_ERROR(t, MSG_BMP_NOT_EXIST, vec1); break;
		case 4: GFX_ERROR(t, MSG_VECMAP_NOT_SCREEN, vec1); break;
		case 5: GFX_ERROR(t, MSG_BMP_NOT_EXIST, vec2); break;
		case 6: GFX_ERROR(t, MSG_VECMAP_NOT_SCREEN, vec2); break;
		default: break;
	}
	return 0;
}

// type 7: `bmp` through the box blur of "90 1B" (`type` the edge handling, `level` the window)
static int Opcode_Gfx0_BgGradient(Thread_t* t) // 90 46: bmp, type, level →
{
	uint32_t level = Thread_Pop(t);
	int type = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckAlpha(level, t);
	switch(GfxCall_BgSetGradient(bmp, type, (int)level))
	{
		case 1: GFX_ERROR(t, MSG_BMP_NOT_EXIST, bmp); break;
		case 2: GFX_ERROR(t, MSG_BMP_NOT_SCREEN_SIZE, bmp); break;
		case 3: GFX_ERROR(t, MSG_BAD_GRADIENT_TYPE, type); break;
		default: break;
	}
	return 0;
}

/* type 8: `bmp` under the ripples of definition `ripple` ("92 00") read
 * through the screen-sized vector + distance map `map` with `type` rings
 * (at least 1; a 0 is reported with the gradient-type message); `level`
 * drives the ripple */
static int Opcode_Gfx0_BgRipple(Thread_t* t) // 90 47: bmp, map, type, ripple, level →
{
	uint32_t level = Thread_Pop(t);
	int ripple = (int)Thread_Pop(t);
	int type = (int)Thread_Pop(t);
	int map = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckAlpha(level, t);
	switch(GfxCall_BgSetRipple(bmp, map, type, ripple, (int)level))
	{
		case 1: GFX_ERROR(t, MSG_BMP_NOT_EXIST, bmp); break;
		case 2: GFX_ERROR(t, MSG_BMP_NOT_SCREEN_SIZE, bmp); break;
		case 3: GFX_ERROR(t, MSG_VDMAP_NOT_EXIST, map); break;
		case 4: GFX_ERROR(t, MSG_VDMAP_UNSUITABLE, map); break;
		case 5: GFX_ERROR(t, MSG_BAD_GRADIENT_TYPE, type); break;
		case 6: GFX_ERROR(t, MSG_RIPPLE_NOT_REGISTERED, ripple); break;
		case 7: GFX_ERROR(t, MSG_RIPPLE_MISMATCH, ripple, map); break;
		default: break;
	}
	return 0;
}

/* type 9: the w x h view of `bmp` (any size; w, h at least 2) at (x, y)
 * stretched over the screen; the level moves the view towards a target
 * rectangle set with parameter 0x102 */
static int Opcode_Gfx0_BgView(Thread_t* t) // 90 48: bmp, x, y, w, h →
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	switch(GfxCall_BgSetView(bmp, x, y, w, h))
	{
		case 1: GFX_ERROR(t, MSG_BMP_NOT_EXIST, bmp); break;
		case 2: GFX_ERROR(t, MSG_BAD_REF_RANGE2, w, h); break;
		default: break;
	}
	return 0;
}

/* type 10: `bmp` (any size) zoomed by `scale` (16.16, not 0) about the
 * screen centre; `option` is the rotation angle (16.16 degrees) */
static int Opcode_Gfx0_BgZoom(Thread_t* t) // 90 49: bmp, scale, option →
{
	int option = (int)Thread_Pop(t);
	int scale = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	switch(GfxCall_BgSetZoom(bmp, scale, option))
	{
		case 1: GFX_ERROR(t, MSG_BMP_NOT_EXIST, bmp); break;
		case 2: GFX_ERROR(t, MSG_BAD_SCALE, scale); break;
		default: break;
	}
	return 0;
}

/* type 11: the mosaic transition from `front` to `back` (a bitmap, black or
 * white) at `level`; `style` must be 0, `link` 0 or 1 (1 also mosaics the
 * back bitmap, by 0x100 - level).  The link is on top of the stack, the
 * level below it (the manager's 0x428bb0 takes the fifth argument as the
 * link and the fourth as the level). */
static int Opcode_Gfx0_BgFlip(Thread_t* t) // 90 4A: front, back, style, level, link →
{
	int link = (int)Thread_Pop(t);
	int level = (int)Thread_Pop(t);
	int style = (int)Thread_Pop(t);
	int back = (int)Thread_Pop(t);
	int front = (int)Thread_Pop(t);
	switch(GfxCall_BgSetFlip(front, back, style, link, level))
	{
		case 1: GFX_ERROR(t, MSG_BG_FRONT_UNUSABLE, front); break;
		case 2: GFX_ERROR(t, MSG_BG_BACK_UNUSABLE, back); break;
		case 3: GFX_ERROR(t, MSG_BAD_STYLE, style); break;
		case 4: GFX_ERROR(t, MSG_BAD_LINK_STATE, link); break;
		default: break;
	}
	return 0;
}

// the background's visibility and draw mode (0 paints black, anything else the type's picture)
static int Opcode_Gfx0_BgShow(Thread_t* t) // 90 4C: visible, mode →
{
	int mode = (int)Thread_Pop(t);
	int visible = (int)Thread_Pop(t);
	GfxCall_BgShow(visible, mode);
	return 0;
}

// the type number (1 .. 12) of the current background
static int Opcode_Gfx0_BgTypeId(Thread_t* t) // 90 4D: → type
{
	Thread_Push(t, (uint32_t)GfxCall_BgTypeId());
	return 0;
}

// -------------------------------------------------------------------------
// 90 50 .. 90 5C : sprites
// -------------------------------------------------------------------------

static int Opcode_Gfx0_SpriteCreate(Thread_t* t) // 90 50: → h
{
	uint32_t h = GfxCall_SpriteCreate();
	if(!h)
		ScriptError(MSG_NO_MORE_SPRITES, t);
	Thread_Push(t, h);
	return 0;
}

/* delete the sprite, after removing it from the click targets; refused
 * while a panel procedure is attached to it or it has an owner (a group it
 * was added to with "90 E8", a parent it was attached to with "91 3E", a
 * knob it is the target of) */
static int Opcode_Gfx0_SpriteDelete(Thread_t* t) // 90 51: h →
{
	uint32_t h = Thread_Pop(t);
	Target_Remove(h);
	if(Gfx_ObjHasProc(h))
		ScriptError(MSG_SPRITE_HAS_PROC, t);
	if(GfxCall_ObjHasOwner(h))
		ScriptError(MSG_SPRITE_HAS_OWNER, t);
	if(!GfxCall_SpriteDelete(h))
		ScriptError(MSG_BAD_SPRITE_HANDLE, t);
	return 0;
}

/* mark the (x, y, w, hgt) rectangle of the sprite (in sprite coordinates)
 * for redraw, after the script changed the bitmap's pixels; 0xa when the
 * sprite's mode does not support it */
static int Opcode_Gfx0_SpriteUpdateRect(Thread_t* t) // 90 53: h, x, y, w, hgt →
{
	int hgt = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_SpriteUpdateRect(h, x, y, w, hgt))
	{
		case 0xa: ScriptError(MSG_BAD_UPDATE_RECT, t); break;
		case 0xff: ScriptError(MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

static int Opcode_Gfx0_SpriteShow(Thread_t* t) // 90 54: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_SpriteShow(h, f))
		ScriptError(MSG_BAD_SPRITE_HANDLE, t);
	return 0;
}

/* a screen-sized gray bitmap that masks the sprite in screen space (-1 =
 * none); 2 when it is not one */
static int Opcode_Gfx0_SpriteSetMask(Thread_t* t) // 90 55: h, bmp →
{
	int bmp = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_SpriteSetMask(h, bmp))
	{
		case 1: GFX_ERROR(t, MSG_BAD_BITMAP, bmp); break;
		case 2: GFX_ERROR(t, MSG_NOT_SCREEN_GRAY, bmp); break;
		case 0xff: ScriptError(MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* The "set" instructions below select the sprite's mode (inc/bgi/gfx/sprite.h)
 * and its sources together with its position, effect, level and priority;
 * a bitmap the sprite refuses is error 1. */

// mode 0: one plain bitmap
static int Opcode_Gfx0_SpriteSet(Thread_t* t) // 90 56: h, x, y, bmp, effect, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t effect = Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	CheckEffectMode(effect, t);
	CheckBitmapNo(bmp, t);
	switch(GfxCall_SpriteSet(h, x, y, bmp, (int)effect, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(t, MSG_BMP_INVALID, bmp); break;
		case 0xff: ScriptError(MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// swap the bitmap of a plain, transform or projected sprite, keeping everything else
static int Opcode_Gfx0_SpriteChangeBitmap(Thread_t* t) // 90 57: h, bmp →
{
	int bmp = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	switch(GfxCall_SpriteChangeBitmap(h, bmp))
	{
		case 1: GFX_ERROR(t, MSG_BMP_INVALID, bmp); break;
		case 0xff: ScriptError(MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* mode 1: two bitmaps of equal size cross-faded by `mixRatio` (0 .. 0x100);
 * `levelRoute` says what a later level change drives (-1 / 0 the level, 1
 * the mix ratio, 2 both).  Case 2 reports differing sizes; Gfx_SpriteSetBlend
 * of this build returns 9 for that, which passes silently. */
static int Opcode_Gfx0_SpriteSetBlend(Thread_t* t) // 90 58: h, x, y, bmp1, bmp2, mixRatio, level, prio, levelRoute →
{
	int levelRoute = (int)Thread_Pop(t);
	uint32_t prio = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t mixRatio = Thread_Pop(t);
	int bmp2 = (int)Thread_Pop(t);
	int bmp1 = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckBitmapNo(bmp1, t);
	CheckBitmapNo(bmp2, t);
	CheckMixRatio(mixRatio, t);
	CheckTransparency(level, t);
	CheckPriority(prio, t);
	switch(GfxCall_SpriteSetBlend(h, x, y, bmp1, bmp2, (int)mixRatio, (int)level, (int)prio, levelRoute))
	{
		case 1: GFX_ERROR(t, MSG_BMP2_INVALID, bmp1, bmp2); break;
		case 2: GFX_ERROR(t, MSG_BMP2_SIZE_MISMATCH, bmp1, bmp2); break;
		case 0xff: ScriptError(MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* mode 2: one bitmap rotated by `angle` (16.16 degrees) and scaled by
 * (sx, sy) (16.16) about the origin (ox, oy) inside the bitmap, `smooth`
 * selecting bilinear sampling; 8 when the transformed picture is smaller
 * than 2 pixels on a side */
static int Opcode_Gfx0_SpriteSetTransform(Thread_t* t) // 90 59: h, x, y, bmp, ox, oy, angle, sx, sy, smooth, effect, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t effect = Thread_Pop(t);
	int smooth = (int)Thread_Pop(t);
	int32_t sy = (int32_t)Thread_Pop(t);
	int32_t sx = (int32_t)Thread_Pop(t);
	int32_t angle = (int32_t)Thread_Pop(t);
	int oy = (int)Thread_Pop(t);
	int ox = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	CheckEffectMode(effect, t);
	switch(GfxCall_SpriteSetTransform(h, x, y, bmp, ox, oy, angle, sx, sy, smooth, (int)effect, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(t, MSG_BAD_BITMAP, bmp); break;
		case 8: GFX_ERROR(t, MSG_BAD_SCALE2, sx, sy); break;
		case 0xff: ScriptError(MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* The scale-only ancestor of "90 59" in 1.58.  That build's sprite had no
 * origin or rotation; `clamp` (not a smoothing flag) kept the scaled size
 * from exceeding the bitmap's, which is the same as capping the factors at
 * 1.0.  Result codes as "90 59"; the scale message of that build read
 * "無効な伸縮倍率 [ %d , %d ]" (invalid scale factors) like the later one. */
static int Opcode_Gfx0_SpriteSetScale_158(Thread_t* t) // 90 5A (1.58): h, x, y, bmp, sx, sy, clamp, effect, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t effect = Thread_Pop(t);
	int clamp = (int)Thread_Pop(t);
	int32_t sy = (int32_t)Thread_Pop(t);
	int32_t sx = (int32_t)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	CheckEffectMode(effect, t);
	if(clamp)
	{
		sx = sx > 0x10000 ? 0x10000 : sx;
		sy = sy > 0x10000 ? 0x10000 : sy;
	}
	switch(GfxCall_SpriteSetTransform(h, x, y, bmp, 0, 0, 0, sx, sy, 0, (int)effect, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(t, MSG_BAD_BITMAP, bmp); break;
		case 8: GFX_ERROR(t, MSG_BAD_SCALE2, sx, sy); break;
		case 0xff: ScriptError(MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* re-scale the sprite of 1.58 in place: the bitmap and the factors change,
 * position, effect, level and priority stay (`clamp` as in "90 5A" of that
 * build) */
static int Opcode_Gfx0_SpriteRescale_158(Thread_t* t) // 90 5B (1.58): h, bmp, sx, sy, clamp →
{
	int clamp = (int)Thread_Pop(t);
	int32_t sy = (int32_t)Thread_Pop(t);
	int32_t sx = (int32_t)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(clamp)
	{
		sx = sx > 0x10000 ? 0x10000 : sx;
		sy = sy > 0x10000 ? 0x10000 : sy;
	}
	switch(Gfx_SpriteRescale(gGfx, h, bmp, sx, sy, 0))
	{
		case 1: GFX_ERROR(t, MSG_BAD_BITMAP, bmp); break;
		case 8: GFX_ERROR(t, MSG_BAD_SCALE2, sx, sy); break;
		case 0xff: ScriptError(MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* mode 3: `bmp` revealed through the grayscale pattern `gray` (copied into
 * the sprite) by `progress` (0 .. 0x100, later driven by "90 35" and the
 * tweens); `wipeMode` scales the gray value by 1 << wipeMode, the width of
 * the transition.  2: the pattern is not grayscale. */
static int Opcode_Gfx0_SpriteSetWipe(Thread_t* t) // 90 5A: h, x, y, bmp, gray, wipeMode, progress, effect, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t effect = Thread_Pop(t);
	uint32_t progress = Thread_Pop(t);
	int wipeMode = (int)Thread_Pop(t);
	int gray = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	CheckEffectMode(effect, t);
	CheckAlpha(progress, t);
	switch(GfxCall_SpriteSetWipe(h, x, y, bmp, gray, wipeMode, (int)progress, (int)effect, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(t, MSG_BMP2_INVALID, bmp, gray); break;
		case 2: GFX_ERROR(t, MSG_FILTER_BMP_UNSUITABLE, gray); break;
		case 0xff: ScriptError(MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* mode 4: `bmp` distorted through the vector + distance map `map` (of the
 * bitmap's size) by the ripple definition `rippleNo` ("92 00") with `rings`
 * rings.  `level` is the ripple amplitude; `fade` (0 .. 0x100) is what the
 * sprite blits with as its level.  Errors: 2 not a sprite bitmap, 3 / 4 the
 * map, 5 the ring count, 6 / 7 the ripple definition. */
static int Opcode_Gfx0_SpriteSetRipple(Thread_t* t) // 90 5B: h, x, y, bmp, map, rings, rippleNo, level, fade, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t fade = Thread_Pop(t);
	int level = (int)Thread_Pop(t);
	int rippleNo = (int)Thread_Pop(t);
	int rings = (int)Thread_Pop(t);
	int map = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	CheckBitmapNo(map, t);
	CheckTransparency(fade, t);
	CheckPriority(prio, t);
	switch(GfxCall_SpriteSetRipple(h, x, y, bmp, map, rings, rippleNo, level, (int)fade, (int)prio))
	{
		case 1: GFX_ERROR(t, MSG_BAD_BITMAP, bmp); break;
		case 2: GFX_ERROR(t, MSG_NOT_SPRITE_BMP, bmp); break;
		case 3: GFX_ERROR(t, MSG_VDMAP_INVALID, map); break;
		case 4: GFX_ERROR(t, MSG_VDMAP_SIZE_MISMATCH, map, bmp); break;
		case 5: GFX_ERROR(t, MSG_BAD_MAX_DISTANCE, rings); break;
		case 6: GFX_ERROR(t, MSG_RIPPLE_NOT_REGISTERED, rippleNo); break;
		case 7: GFX_ERROR(t, MSG_RIPPLE_MISMATCH, rippleNo, map); break;
		case 0xff: ScriptError(MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* mode 5: a transform sprite placed in 3-D at the 16.16 position (fx, fy,
 * fz): depth sorted, perspective scaled by `projDist` (the projection
 * distance) and, when `projPos` is set, with its position scaled by the
 * perspective as well; the vanishing point is that of "90 06".  `bmp2`
 * (-1 = none) is cross-faded in by `mixRatio`, `levelRoute` as in "90 58",
 * origin, angle and `smooth` as in "90 59".  Case 2 reports a second
 * bitmap of another size or mode; Gfx_SpriteSetProjected of this build
 * returns 9 for that, which passes silently.  8: the projected picture is
 * smaller than 2 pixels on a side (reported with the projection distance). */
static int Opcode_Gfx0_SpriteSetProjected(Thread_t* t) // 90 5C: h, fx, fy, fz, bmp1, bmp2, mixRatio, levelRoute, ox, oy, angle, projDist, projPos, smooth, effect, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t effect = Thread_Pop(t);
	int smooth = (int)Thread_Pop(t);
	int projPos = (int)Thread_Pop(t);
	int projDist = (int)Thread_Pop(t);
	int32_t angle = (int32_t)Thread_Pop(t);
	int oy = (int)Thread_Pop(t);
	int ox = (int)Thread_Pop(t);
	int levelRoute = (int)Thread_Pop(t);
	uint32_t mixRatio = Thread_Pop(t);
	int bmp2 = (int)Thread_Pop(t);
	int bmp1 = (int)Thread_Pop(t);
	int32_t fz = (int32_t)Thread_Pop(t);
	int32_t fy = (int32_t)Thread_Pop(t);
	int32_t fx = (int32_t)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckBitmapNo(bmp1, t);
	CheckMixRatio(mixRatio, t);
	CheckEffectMode(effect, t);
	CheckAlpha(level, t);
	CheckPriority(prio, t);
	switch(GfxCall_SpriteSetProjected(h, fx, fy, fz, bmp1, bmp2, (int)mixRatio, levelRoute, ox, oy, angle, projDist,
		projPos, smooth, (int)effect, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(t, MSG_BMP2_INVALID, bmp1, bmp2); break;
		case 2: GFX_ERROR(t, MSG_BMP2_SIZE_MODE_MISMATCH, bmp1, bmp2); break;
		case 8: GFX_ERROR(t, MSG_BAD_PROJ_SCALE, projDist); break;
		case 0xff: ScriptError(MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* 90 5D (1.588 on): "90 5C" with three more 16.16 angles between the
 * rotation and the projection distance - the rotations about the X, Y and
 * Z axes of the 3-D sprite class of those builds (tweened like the angle;
 * the Z value goes into the perspective with projDist).  The scene-select
 * screens of 1.653 on (scrslct._bp) call it with all three at 0, which is
 * the flat picture of "90 5C"; non-zero rotations are not implemented and
 * are ignored, so such a sprite shows unrotated (docs/versions.md). */
static int Opcode_Gfx0_SpriteSetProjected3D(Thread_t* t) // 90 5D: h, fx, fy, fz, bmp1, bmp2, mixRatio, levelRoute, ox, oy, angle, rotX, rotY, rotZ, projDist, projPos, smooth, effect, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t effect = Thread_Pop(t);
	int smooth = (int)Thread_Pop(t);
	int projPos = (int)Thread_Pop(t);
	int projDist = (int)Thread_Pop(t);
	int32_t rotZ = (int32_t)Thread_Pop(t);
	int32_t rotY = (int32_t)Thread_Pop(t);
	int32_t rotX = (int32_t)Thread_Pop(t);
	int32_t angle = (int32_t)Thread_Pop(t);
	int oy = (int)Thread_Pop(t);
	int ox = (int)Thread_Pop(t);
	int levelRoute = (int)Thread_Pop(t);
	uint32_t mixRatio = Thread_Pop(t);
	int bmp2 = (int)Thread_Pop(t);
	int bmp1 = (int)Thread_Pop(t);
	int32_t fz = (int32_t)Thread_Pop(t);
	int32_t fy = (int32_t)Thread_Pop(t);
	int32_t fx = (int32_t)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	(void)rotX;
	(void)rotY;
	(void)rotZ;
	CheckBitmapNo(bmp1, t);
	CheckMixRatio(mixRatio, t);
	CheckEffectMode(effect, t);
	CheckAlpha(level, t);
	CheckPriority(prio, t);
	switch(GfxCall_SpriteSetProjected(h, fx, fy, fz, bmp1, bmp2, (int)mixRatio, levelRoute, ox, oy, angle, projDist,
		projPos, smooth, (int)effect, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(t, MSG_BMP2_INVALID, bmp1, bmp2); break;
		case 2: GFX_ERROR(t, MSG_BMP2_SIZE_MODE_MISMATCH, bmp1, bmp2); break;
		case 8: GFX_ERROR(t, MSG_BAD_PROJ_SCALE, projDist); break;
		case 0xff: ScriptError(MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 90 60 .. 90 66 : filters
// -------------------------------------------------------------------------

static int Opcode_Gfx0_FilterCreate(Thread_t* t) // 90 60: → h
{
	uint32_t h = GfxCall_FilterCreate();
	if(!h)
		ScriptError(MSG_NO_MORE_FILTERS, t);
	Thread_Push(t, h);
	return 0;
}

static int Opcode_Gfx0_FilterDelete(Thread_t* t) // 90 61: h →
{
	if(!GfxCall_FilterDelete(Thread_Pop(t)))
		ScriptError(MSG_BAD_FILTER_HANDLE, t);
	return 0;
}

static int Opcode_Gfx0_FilterShow(Thread_t* t) // 90 64: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_FilterShow(h, f))
		ScriptError(MSG_BAD_FILTER_HANDLE, t);
	return 0;
}

/* A filter has no picture of its own: it applies a colour operation to
 * everything drawn below its priority.  Kind 0 blends the picture towards
 * `colour` by `level` (a black colour dims it); kinds 1 .. 3 are the
 * variants of src/gfx/blit_filter.c (add the colour, monochrome tint, blend
 * towards the picture XOR the colour). */

// a uniform kind-0 filter of `colour` at `level`, no gray bitmap
static int Opcode_Gfx0_FilterSet(Thread_t* t) // 90 65: h, colour, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t colour = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	if(GfxCall_FilterSet(h, 0, colour, -1, 0, (int)level, (int)prio) != 0)
		ScriptError(MSG_BAD_FILTER_HANDLE, t);
	return 0;
}

/* a filter of `kind` 0 .. 3, modulated per pixel by the screen-sized gray
 * bitmap `bmp` (-1 = uniform; only kind 0 uses it, with `param` the
 * transition curve of "90 19").  Errors: 1 no such bitmap, 2 not
 * grayscale, 3 not screen sized. */
static int Opcode_Gfx0_FilterSetBitmap(Thread_t* t) // 90 66: h, kind, colour, bmp, param, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	int param = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	uint32_t colour = Thread_Pop(t);
	int kind = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	switch(GfxCall_FilterSet(h, kind, colour, bmp, param, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(t, MSG_BAD_BITMAP_NO, bmp); break;
		case 2: GFX_ERROR(t, MSG_BMP_NOT_GRAY, bmp); break;
		case 3: GFX_ERROR(t, MSG_BMP_NOT_SCREEN_SIZED, bmp); break;
		case 0xff: ScriptError(MSG_BAD_FILTER_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 90 70 .. 90 7A : tile maps
// -------------------------------------------------------------------------

static int Opcode_Gfx0_MapCreate(Thread_t* t) // 90 70: → h
{
	uint32_t h = GfxCall_MapCreate();
	if(!h)
		ScriptError(MSG_NO_MORE_MAPS, t);
	Thread_Push(t, h);
	return 0;
}

static int Opcode_Gfx0_MapDelete(Thread_t* t) // 90 71: h →
{
	if(!GfxCall_MapDelete(Thread_Pop(t)))
		ScriptError(MSG_BAD_MAP_HANDLE, t);
	return 0;
}

static int Opcode_Gfx0_MapShow(Thread_t* t) // 90 74: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_MapShow(h, f))
		ScriptError(MSG_BAD_MAP_HANDLE, t);
	return 0;
}

/* A map is a scrolling tile map: a chipset bitmap sliced into equal tiles
 * ("90 76" sets the sizes, "90 75" the chipset), a terrain of 16-bit tile
 * numbers (0xFFFF = empty, "90 78") and a view position in it ("90 79"). */

/* place the map at (x, y) with `bmp` as its chipset; 1 when the bitmap is
 * unusable (missing, too small for one tile, or "90 76" not done yet) */
static int Opcode_Gfx0_MapSet(Thread_t* t) // 90 75: h, x, y, bmp, effect, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t effect = Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level, t);
	CheckEffectMode(effect, t);
	CheckBitmapNo(bmp, t);
	switch(GfxCall_MapSet(h, x, y, bmp, (int)effect, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(t, MSG_MAP_BMP_MISMATCH, bmp); break;
		case 0xff: ScriptError(MSG_BAD_MAP_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* the view size in tiles and the tile size in pixels; a tile is at most
 * 0x100 x 0x100, the view at most 0x400 x 0x300 pixels (2: the view, 3:
 * the tile size) */
static int Opcode_Gfx0_MapSetSize(Thread_t* t) // 90 76: h, cols, rows, chipW, chipH →
{
	int chipH = (int)Thread_Pop(t);
	int chipW = (int)Thread_Pop(t);
	int rows = (int)Thread_Pop(t);
	int cols = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_MapSetSize(h, cols, rows, chipW, chipH))
	{
		case 2: GFX_ERROR(t, MSG_BAD_VIEW_SIZE, cols, rows); break;
		case 3: GFX_ERROR(t, MSG_BAD_CHIP_SIZE, chipW, chipH); break;
		case 0xff: ScriptError(MSG_BAD_MAP_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// copy a w x hgt terrain of 16-bit tile numbers out of script memory (4: an empty size)
static int Opcode_Gfx0_MapSetTerrain(Thread_t* t) // 90 78: h, w, hgt, data →
{
	const uint16_t* data = (const uint16_t*)PopPtr(t);
	int hgt = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_MapSetTerrain(h, w, hgt, data))
	{
		case 4: GFX_ERROR(t, MSG_BAD_TERRAIN_SIZE, w, hgt); break;
		case 0xff: ScriptError(MSG_BAD_MAP_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* scroll the view: terrain cell (col, row) at the top-left with the pixel
 * offset (offX, offY) inside it; with `wrap` the terrain repeats past its
 * edges, otherwise the cells beyond stay empty.  5: the position is out of
 * range, or the map has no size or terrain yet. */
static int Opcode_Gfx0_MapSetView(Thread_t* t) // 90 79: h, col, row, offX, offY, wrap →
{
	int wrap = (int)Thread_Pop(t);
	int offY = (int)Thread_Pop(t);
	int offX = (int)Thread_Pop(t);
	int row = (int)Thread_Pop(t);
	int col = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_MapSetView(h, col, row, offX, offY, wrap))
	{
		case 5: GFX_ERROR(t, MSG_BAD_MAP_VIEW_POS, col, row, offX, offY); break;
		case 0xff: ScriptError(MSG_BAD_MAP_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* redraw every visible cell that shows tile number `tile`, after the script
 * repainted that tile inside the chipset; 6 when the map has no size yet */
static int Opcode_Gfx0_MapInvalidateChip(Thread_t* t) // 90 7A: h, tile →
{
	int tile = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_MapInvalidateChip(h, tile))
	{
		case 6: ScriptError(MSG_OBJ_NOT_INITIALISED, t); break;
		case 0xff: ScriptError(MSG_BAD_MAP_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 90 80 .. 90 89 : message windows
// -------------------------------------------------------------------------

// create a message window of w x h pixels (16 slots); 2 when the size is rejected
static int Opcode_Gfx0_WindowCreate(Thread_t* t) // 90 80: w, h → handle
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	uint32_t handle;
	switch(GfxCall_WindowCreate(&handle, w, h))
	{
		case 1: ScriptError(MSG_NO_MORE_WINDOWS, t); break;
		case 2: GFX_ERROR(t, MSG_BAD_WINDOW_SIZE, w, h); break;
		default: break;
	}
	Thread_Push(t, handle);
	return 0;
}

/* delete the window; refused while a panel procedure is attached to it or
 * it has an owner (as "90 51") */
static int Opcode_Gfx0_WindowDelete(Thread_t* t) // 90 81: h →
{
	uint32_t h = Thread_Pop(t);
	if(Gfx_ObjHasProc(h))
		ScriptError(MSG_WINDOW_HAS_PROC, t);
	if(GfxCall_ObjHasOwner(h))
		ScriptError(MSG_WINDOW_HAS_OWNER, t);
	if(!GfxCall_WindowDelete(h))
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

/* create bitmap `bmp` of the window's size and copy the window's composite
 * picture into it; a bitmap the manager refuses is not reported */
static int Opcode_Gfx0_WindowCapture(Thread_t* t) // 90 83: bmp, h →
{
	uint32_t h = Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckBitmapNo(bmp, t);
	if(GfxCall_WindowCopyToBitmap(bmp, h) == -1)
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

static int Opcode_Gfx0_WindowShow(Thread_t* t) // 90 84: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_WindowShow(h, f))
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

// position, effect, level and priority of the window; `level2` is validated but not used
static int Opcode_Gfx0_WindowSet(Thread_t* t) // 90 85: h, x, y, effect, level, level2, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level2 = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t effect = Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(prio, t);
	CheckAlpha(level2, t);
	CheckAlpha(level, t);
	CheckEffectMode(effect, t);
	if(!GfxCall_WindowSet(h, x, y, (int)effect, (int)level, (int)level2, (int)prio))
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

/* the window's frame layer: bitmap `back` is copied into it (-1 clears and
 * hides the frame); `decoration` and `frame` are popped but not used by
 * this build's window.  Errors: 1 the window has no surfaces, 2 no such
 * bitmap (reported with all three numbers), 3 not a frame bitmap. */
static int Opcode_Gfx0_WindowSetFrame(Thread_t* t) // 90 86: h, decoration, frame, back →
{
	int back = (int)Thread_Pop(t);
	int frame = (int)Thread_Pop(t);
	int decoration = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_WindowSetFrame(h, decoration, frame, back))
	{
		case 1: ScriptError(MSG_WINDOW_NO_RESOURCES, t); break;
		case 2: GFX_ERROR(t, MSG_WINDOW_BMPS_UNREGISTERED, decoration, frame, back); break;
		case 3: GFX_ERROR(t, MSG_NOT_FRAME_BMP, frame); break;
		case -1: ScriptError(MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* whether the item layers (icons, panel buttons) are "punched" out of the
 * text layer when the window is composed.  Shared with the 1.58 / 1.64
 * tables, where the instruction was "90 8B" (declared in bgi/vm.h). */
int Opcode_Gfx0_WindowSetPunch(Thread_t* t) // 90 87: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_WindowSetPunch(h, f))
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

/* the order the window's frame, text and sub-sprite layers are composed
 * in: 0 frame, text, subs; 1 frame, subs, text; 2 text, frame, subs; 3
 * text, subs, frame; 4 subs, frame, text; 5 subs, text, frame */
static int Opcode_Gfx0_WindowSetLayerOrder(Thread_t* t) // 90 82 (1.494 on): h, order →
{
	int order = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_WindowSetLayerOrder(h, order))
	{
		case 0xff: ScriptError(MSG_BAD_WINDOW_HANDLE, t); // does not return, so no break is needed
		case 0x14: GFX_ERROR(t, MSG_BAD_DRAW_ORDER, order);
		default: return 0;
	}
}

// the rectangle of the window that text may be drawn in, in window coordinates
static int Opcode_Gfx0_WindowSetTextArea(Thread_t* t) // 90 88: h, x, y, w, hgt →
{
	int hgt = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_WindowSetTextArea(h, x, y, w, hgt))
	{
		case 1: GFX_ERROR(t, MSG_BAD_WINDOW_AREA, x, y, w, hgt); break;
		case -1: ScriptError(MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// write the text area as a rectangle (l, t, r, b inclusive) to `out`; the result is always 1
static int Opcode_Gfx0_WindowGetTextArea(Thread_t* t) // 90 89: out, h → ok
{
	uint32_t h = Thread_Pop(t);
	Rect_t* out = (Rect_t*)PopPtr(t);
	int r = GfxCall_WindowGetTextArea(out, h);
	if(r == -1)
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	Thread_Push(t, r == 0);
	return 0;
}

// -------------------------------------------------------------------------
// 90 90 .. 90 9F : text output and its parameters
// -------------------------------------------------------------------------

/* Write `str` into the window's text layer one character per step (the
 * plain WaitTextOut of src/wait/wait_text.c; "91 90" / "92 90" are the
 * rich variants) and block until it is done.  `param` is the text colour,
 * `instant` skips the pacing, `waitAtEnd` waits for input after the last
 * character, `allowSkip` lets the skip key hurry the output; the keyboard
 * always does.  The wait pushes 1 when input hurried the output, else 0. */
static int Opcode_Gfx0_TextOut(Thread_t* t) // 90 90: h, str, param, instant, waitAtEnd, allowSkip →
{
	int allowSkip = (int)Thread_Pop(t);
	int waitAtEnd = (int)Thread_Pop(t);
	int instant = (int)Thread_Pop(t);
	uint32_t param = Thread_Pop(t);
	const char* str = (const char*)PopPtr(t);
	uint32_t h = Thread_Pop(t);
	int r = StartTextOutW(t, h, str, param, 0, instant, waitAtEnd, allowSkip, 1, 0, 0, -1);
	if(r == (int)0x80000001)
		ScriptError(MSG_WINDOW_NO_FONT, t);
	if(r == -1)
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	return 2;
}

/* The remaining instructions of this section set the global pacing and
 * style parameters of the text output (the gText* / gCursor* globals of
 * inc/bgi/gfx/text.h); they take effect with the next output. */

/* which input layer the text output listens on: `mode` 0 the standard
 * layer 2, 1 the fixed layer `layerNo` (below 0x1000), 2 the window's
 * priority */
static int Opcode_Gfx0_TextSetMode(Thread_t* t) // 90 91: mode, layerNo →
{
	int layerNo = (int)Thread_Pop(t);
	int mode = (int)Thread_Pop(t);
	switch(Text_SetLayerMode(mode, layerNo))
	{
		case(int)0x80000004: GFX_ERROR(t, MSG_BAD_MODE, mode); break;
		case(int)0x80000005: GFX_ERROR(t, MSG_BAD_PRIORITY, layerNo); break;
		default: break;
	}
	return 0;
}

// the delay between two characters of the paced output
static int Opcode_Gfx0_TextSetCharInterval(Thread_t* t) // 90 94: ms →
{
	Text_SetCharInterval((int)Thread_Pop(t));
	return 0;
}

// a line scroll of the text layer takes `steps` steps (1 .. 256) of `ms` each
static int Opcode_Gfx0_TextSetScroll(Thread_t* t) // 90 95: steps, ms →
{
	int ms = (int)Thread_Pop(t);
	int steps = (int)Thread_Pop(t);
	CheckDivCount(steps, t);
	Text_SetScroll(steps, ms);
	return 0;
}

// a fade of the text layer takes `steps` steps (1 .. 256) of `ms` each
static int Opcode_Gfx0_TextSetFade(Thread_t* t) // 90 96: steps, ms →
{
	int ms = (int)Thread_Pop(t);
	int steps = (int)Thread_Pop(t);
	CheckDivCount(steps, t);
	Text_SetFade(steps, ms);
	return 0;
}

// auto-advance: with `on`, a text output that waits at its end finishes by itself after `ms`
static int Opcode_Gfx0_TextSetAutoAdvance(Thread_t* t) // 90 97: on, ms →
{
	int ms = (int)Thread_Pop(t);
	int on = (int)Thread_Pop(t);
	Text_SetAutoAdvance(on, ms);
	return 0;
}

/* the frames of the "waiting" cursor animation shown at the end of a text:
 * `n` bitmap numbers in script memory (-1 = an empty frame), copied into
 * private screen-format bitmaps; a single entry means no cursor.  The error
 * names the first bitmap that cannot be used. */
static int Opcode_Gfx0_TextSetBitmaps(Thread_t* t) // 90 98: n, list →
{
	const int* list = (const int*)PopPtr(t);
	int n = (int)Thread_Pop(t);
	int bad;
	if(!Text_SetItemBitmaps(n, list, &bad))
		GFX_ERROR(t, MSG_BMP_SCREEN_INCOMPATIBLE, bad);
	return 0;
}

// the delay between two frames of the cursor animation
static int Opcode_Gfx0_TextSetCursorInterval(Thread_t* t) // 90 99: ms →
{
	Text_SetCursorInterval((int)Thread_Pop(t));
	return 0;
}

// where the cursor animation is drawn: `mode` 1 at the fixed position (x, y), else after the text
static int Opcode_Gfx0_TextSetCursorPos(Thread_t* t) // 90 9A: mode, x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int mode = (int)Thread_Pop(t);
	Text_SetCursorPos(mode, x, y);
	return 0;
}

/* with `on`, a text output that waits at its end does not start that wait
 * (cursor animation and input) until `ms` have passed since the output
 * began, unless input already hurried it */
static int Opcode_Gfx0_TextSetStartDelay(Thread_t* t) // 90 9B: on, ms →
{
	int ms = (int)Thread_Pop(t);
	int on = (int)Thread_Pop(t);
	Text_SetStartDelay(on, ms);
	return 0;
}

// switch the character shadow of the text output on or off
static int Opcode_Gfx0_TextSetStyleOn(Thread_t* t) // 90 9C: on →
{
	Text_SetStyleOn((int)Thread_Pop(t));
	return 0;
}

/* the shadow's offset in percent of the font size (0 .. 100 each; the
 * handler only rejects negative values, Text_SetStyleParams ignores a
 * value above 100) and its density `level` (0 .. 0x100) */
static int Opcode_Gfx0_TextSetStyleParams(Thread_t* t) // 90 9D: dxPct, dyPct, level →
{
	uint32_t level = Thread_Pop(t);
	int dyPct = (int)Thread_Pop(t);
	int dxPct = (int)Thread_Pop(t);
	if(dxPct < 0 || dyPct < 0)
		GFX_ERROR(t, MSG_BAD_COORDS, dxPct, dyPct);
	if(level > 0x100)
		GFX_ERROR(t, MSG_BAD_DENSITY, (int)level);
	Text_SetStyleParams(dxPct, dyPct, (int)level);
	return 0;
}

/* the custom glyph sheet: bitmap `bmp` is a strip of `count` (0 .. 0xFF)
 * equal cells drawn for the character codes 0xFF01 .. 0xFF00 + count; a
 * count of 0 releases the sheet.  3: the width is not a multiple of the
 * count. */
static int Opcode_Gfx0_TextSetGlyphSheet(Thread_t* t) // 90 9E: count, bmp →
{
	int bmp = (int)Thread_Pop(t);
	int count = (int)Thread_Pop(t);
	switch(Text_SetGlyphSheet(count, bmp))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BAD_CHAR_COUNT, count); break;
		case(int)0x80000002: GFX_ERROR(t, MSG_BAD_BITMAP, bmp); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_BMP_UNSUITABLE, bmp); break;
		default: break;
	}
	return 0;
}

// with `f`, input during a text output finishes it at once instead of only switching to fast mode
static int Opcode_Gfx0_TextSetInputFinishes(Thread_t* t) // 90 9F: f →
{
	Text_SetInputFinishes((int)Thread_Pop(t));
	return 0;
}

// -------------------------------------------------------------------------
// 90 A0 .. 90 AF : text menus
// -------------------------------------------------------------------------

/* A text menu lays `n` (1 .. 16) strings (tagged pointers in script memory
 * at `items`) out in `cols` columns of the window's text layer, optionally
 * centred, in `colour` unless the window has a colour table ("90 A7"), and
 * lets the player pick one with the mouse or the keys (src/wait/wait_menu.c).
 * The wait pushes the selected index twice (-1 for cancel). */

// run a menu with the cursor on item `init`; `allowCancel` lets the cancel key end it
static int Opcode_Gfx0_MenuSelect(Thread_t* t) // 90 A0: h, n, items, cols, centre, colour, init, allowCancel →
{
	int allowCancel = (int)Thread_Pop(t);
	int init = (int)Thread_Pop(t);
	uint32_t colour = Thread_Pop(t);
	int centre = (int)Thread_Pop(t);
	int cols = (int)Thread_Pop(t);
	const void* items = PopPtr(t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(StartMenuSelect(t, h, n, items, cols, centre, colour, init, allowCancel))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BAD_ITEM_COUNT, n); break;
		case(int)0x80000002: GFX_ERROR(t, MSG_BAD_INITIAL_ITEM, init); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_BAD_COLUMN_COUNT, cols); break;
		case(int)0x80000004: ScriptError(MSG_WINDOW_NO_FONT, t); break;
		case -1: ScriptError(MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 2;
}

// lay the entries out as "90 A0" does, without a cursor and without waiting
static int Opcode_Gfx0_MenuDraw(Thread_t* t) // 90 A1: h, n, items, cols, centre, colour →
{
	uint32_t colour = Thread_Pop(t);
	int centre = (int)Thread_Pop(t);
	int cols = (int)Thread_Pop(t);
	const void* items = PopPtr(t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(Menu_Draw(h, n, items, t, cols, centre, colour))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BAD_ITEM_COUNT, n); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_BAD_COLUMN_COUNT, cols); break;
		case -1: ScriptError(MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* The body shared by "90 A2" (mode 1) and "90 A3" (mode 0): a menu as
 * "90 A0" with two decoration bitmaps (-1 = none) placed at (dx1, dy1) /
 * (dx2, dy2) from the selected entry's top-left corner, the second also
 * past its right edge.  Mode 1 is driven by the mouse alone; mode 0 takes
 * no input, flashes entry `init` for a second and finishes.  Stack:
 * h, n, items, cols, centre, colour, init, allowCancel, bmp1, dx1, dy1,
 * bmp2, dx2, dy2 →.  Errors 0x80000005 .. 8: the two bitmaps are unknown
 * or not usable in the window. */
static int MenuSelectBmp(Thread_t* t, int mode)
{
	int dy2 = (int)Thread_Pop(t);
	int dx2 = (int)Thread_Pop(t);
	int bmp2 = (int)Thread_Pop(t);
	int dy1 = (int)Thread_Pop(t);
	int dx1 = (int)Thread_Pop(t);
	int bmp1 = (int)Thread_Pop(t);
	int allowCancel = (int)Thread_Pop(t);
	int init = (int)Thread_Pop(t);
	uint32_t colour = Thread_Pop(t);
	int centre = (int)Thread_Pop(t);
	int cols = (int)Thread_Pop(t);
	const void* items = PopPtr(t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(StartMenuSelectBmp(t, h, n, items, cols, centre, colour, init, allowCancel, bmp1, dx1, dy1, bmp2, dx2, dy2,
		mode))
	{
		case -1: ScriptError(MSG_BAD_WINDOW_HANDLE, t); break;
		case(int)0x80000001: GFX_ERROR(t, MSG_BAD_ITEM_COUNT, n); break;
		case(int)0x80000002: GFX_ERROR(t, MSG_BAD_INITIAL_ITEM, init); break;
		case(int)0x80000003: GFX_ERROR(t, MSG_BAD_COLUMN_COUNT, cols); break;
		case(int)0x80000004: ScriptError(MSG_WINDOW_NO_FONT, t); break;
		case(int)0x80000005: GFX_ERROR(t, MSG_BMP_INVALID, bmp1); break;
		case(int)0x80000006: GFX_ERROR(t, MSG_BMP_SCREEN_INCOMPAT, bmp1); break;
		case(int)0x80000007: GFX_ERROR(t, MSG_BMP_INVALID, bmp2); break;
		case(int)0x80000008: GFX_ERROR(t, MSG_BMP_SCREEN_INCOMPAT, bmp2); break;
		default: break;
	}
	return 2;
}

// the mouse-driven menu with decoration bitmaps (MenuSelectBmp mode 1)
static int Opcode_Gfx0_MenuSelectBmp(Thread_t* t) // 90 A2: h, n, items, cols, centre, colour, init, allowCancel, bmp1, dx1, dy1, bmp2, dx2, dy2 →
{
	return MenuSelectBmp(t, 1);
}

// the input-less flash of one entry (MenuSelectBmp mode 0)
static int Opcode_Gfx0_MenuSelectBmp0(Thread_t* t) // 90 A3: h, n, items, cols, centre, colour, init, allowCancel, bmp1, dx1, dy1, bmp2, dx2, dy2 →
{
	return MenuSelectBmp(t, 0);
}

// the colour of the menu cursor in each of its two blink phases (0 hides it in that phase)
static int Opcode_Gfx0_MenuSetCursorColours(Thread_t* t) // 90 A4: c0, c1 →
{
	uint32_t c1 = Thread_Pop(t);
	uint32_t c0 = Thread_Pop(t);
	Menu_SetCursorColours(c0, c1);
	return 0;
}

// the blink interval of the menu cursor (600 ms by default)
static int Opcode_Gfx0_MenuSetBlink(Thread_t* t) // 90 A5: ms →
{
	Menu_SetBlinkInterval(Thread_Pop(t));
	return 0;
}

/* with `on`, the mouse wheel steps through a menu by gliding the pointer to
 * the next entry (an Ease() `curve` over `duration` ms at `fps` updates per
 * second); off, the wheel is ignored */
static int Opcode_Gfx0_MenuSetWheel(Thread_t* t) // 90 A6: on, curve, duration, fps →
{
	int fps = (int)Thread_Pop(t);
	int duration = (int)Thread_Pop(t);
	int curve = (int)Thread_Pop(t);
	int on = (int)Thread_Pop(t);
	Menu_SetWheelMove(on, curve, duration, fps);
	return 0;
}

/* the window's colour table: 16 words in script memory, one colour per
 * menu entry (used instead of the menu colour while set); a null pointer
 * removes it */
static int Opcode_Gfx0_WindowSetTable(Thread_t* t) // 90 A7: h, table →
{
	const int32_t* table = (const int32_t*)PopPtr(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_WindowSetTable(h, table))
		ScriptError(MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

// whether the menu, icon and panel selections take input only while the window is the active one
static int Opcode_Gfx0_SelectRequireActive(Thread_t* t) // 90 AF: f →
{
	Select_RequireActive((int)Thread_Pop(t));
	return 0;
}

// -------------------------------------------------------------------------
// 90 B0 .. 90 BF : icon selections and panels
// -------------------------------------------------------------------------

/* An icon selection draws `n` (1 .. 64) icon records from script memory
 * into the window's text layer, each with a virtual hit object, and waits
 * for a click on one, a hot key (`hotkeys`: one key per icon, or a null
 * pointer), a cancel key (when `allowCancel`) or a window message.  `mode`
 * 0 .. 3 selects the input layers, `useHitMask` makes the icons hit by
 * their bitmap's alpha rather than their rectangle.  The wait pushes the
 * click position inside the icon and the icon index (-1 for none / cancel).
 * See src/wait/wait_icon.c. */

/* map the result of an icon selection starter to a script error (bad icon
 * count, bad focus mode, an icon bitmap unusable, no such window) and end
 * the turn */
static int IconSelectResult(Thread_t* t, int r, int n, int mode)
{
	switch(r)
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BAD_ICON_COUNT, n); break;
		case(int)0x80000002: GFX_ERROR(t, MSG_BAD_FOCUS_MODE, mode); break;
		case(int)0x80000003: ScriptError(MSG_ICON_BMPS_INVALID, t); break;
		case -1: ScriptError(MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 2;
}

/* icon selection with 16-byte records (x, y, bitmap, focus bitmap); the
 * focus bitmap (-1 = none) is drawn over the icon under the mouse */
static int Opcode_Gfx0_IconSelect(Thread_t* t) // 90 B0: h, n, icons, hotkeys, mode, allowCancel, useHitMask →
{
	int useHitMask = (int)Thread_Pop(t);
	int allowCancel = (int)Thread_Pop(t);
	int mode = (int)Thread_Pop(t);
	const int32_t* hotkeys = (const int32_t*)PopPtr(t);
	const void* icons = PopPtr(t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	return IconSelectResult(t, StartIconSelect(t, h, n, icons, hotkeys, mode, allowCancel, useHitMask), n, mode);
}

/* icon selection with 0x40-byte records (x, y, bitmap, four (x, y, bitmap)
 * overlay triples for the window's item layers, a hit-mask bitmap or -1
 * for the icon's own) */
static int Opcode_Gfx0_IconSelect2(Thread_t* t) // 90 B1: h, n, icons, hotkeys, mode, allowCancel, useHitMask →
{
	int useHitMask = (int)Thread_Pop(t);
	int allowCancel = (int)Thread_Pop(t);
	int mode = (int)Thread_Pop(t);
	const int32_t* hotkeys = (const int32_t*)PopPtr(t);
	const void* icons = PopPtr(t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	return IconSelectResult(t, StartIconSelect2(t, h, n, icons, hotkeys, mode, allowCancel, useHitMask), n, mode);
}

// draw the icons of 16-byte records ("90 B0" layout) into the window without waiting
static int Opcode_Gfx0_IconDraw(Thread_t* t) // 90 B4: h, n, icons →
{
	const void* icons = PopPtr(t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(Icon_Draw(h, n, icons))
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BAD_ICON_COUNT, n); break;
		case -1: ScriptError(MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* "90 B4" for the 0x40-byte records of "90 B1": the first 12 bytes of each
 * (x, y, bitmap) become a 16-byte Icon_Draw record with no focus bitmap.
 * 1 .. 64 records. */
static int Opcode_Gfx0_IconDraw64(Thread_t* t) // 90 B5: h, n, icons64 →
{
	const uint8_t* icons = (const uint8_t*)PopPtr(t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	int32_t* rec;
	int i, r;
	if(n < 1 || n > 0x40)
		GFX_ERROR(t, MSG_BAD_ICON_COUNT, n);
	rec = (int32_t*)BGI_Alloc((size_t)n << 4);
	for(i = 0; i < n; i++)
	{
		memcpy(&rec[i * 4], icons + i * 0x40, 12); // x, y, bitmap
		rec[i * 4 + 3] = -1;
	}
	r = Icon_Draw(h, n, rec);
	BGI_Free(rec);
	switch(r)
	{
		case(int)0x80000001: GFX_ERROR(t, MSG_BAD_ICON_COUNT, n); break;
		case -1: ScriptError(MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* A button panel is a table of button groups in script memory (the layout
 * described in inc/bgi/panel.h) that is either painted
 * once into a window ("90 B6") or run as a "sub-object": an interactive
 * procedure created with "90 B8", started from a table with "90 BA", polled
 * every pass by the scheduler, queried with "90 BC" .. "90 BF" and deleted
 * with "90 B9".  Unlike the selections above none of these instructions
 * blocks the thread; the results are pushed, not raised. */

/* paint the buttons of the table at `desc` into the text layer of window
 * `h`; the result is 0 ok, 1 no such window, 2 the group table is unusable
 * (null or not 1 .. 0x100 groups), 3 a button table is */
static int Opcode_Gfx0_PanelDraw(Thread_t* t) // 90 B6: h, desc → r
{
	const void* desc = PopPtr(t);
	uint32_t h = Thread_Pop(t);
	Thread_Push(t, (uint32_t)Panel_Draw(h, desc, t));
	return 0;
}

// "90 B6" for a sprite-panel table: one sub-sprite per button; result codes as "90 B6"
static int Opcode_Gfx0_SprPanelDraw(Thread_t* t) // 90 B7: h, desc → r
{
	const void* desc = PopPtr(t);
	uint32_t h = Thread_Pop(t);
	Thread_Push(t, (uint32_t)SprPanel_Draw(h, desc, t));
	return 0;
}

/* create a button panel procedure on window `h` and push its id (an unknown
 * handle still creates one, on no window) */
static int Opcode_Gfx0_SubObjCreate(Thread_t* t) // 90 B8: h → id
{
	Thread_Push(t, SubObj_Create(Thread_Pop(t), 0));
	return 0;
}

// destroy the procedure with that id; 1 when there was one, 0 otherwise
static int Opcode_Gfx0_SubObjDelete(Thread_t* t) // 90 B9: id → r
{
	Thread_Push(t, (uint32_t)SubObj_Delete(Thread_Pop(t)));
	return 0;
}

/* start (or restart) panel `id` from the table at `desc`: the buttons are
 * drawn and from now on the panel tracks the mouse and keys each pass.
 * Result: 0 ok, 1 no panel with that id, 2 / 3 the group / button table is
 * unusable, 4 the id is a sprite panel ("91 BA" starts those). */
static int Opcode_Gfx0_PanelStart(Thread_t* t) // 90 BA: id, desc → r
{
	const void* desc = PopPtr(t);
	uint32_t id = Thread_Pop(t);
	Thread_Push(t, (uint32_t)Panel_Start(id, desc, t));
	return 0;
}

/* the panel's state into six words at `out`: started, the decided group,
 * index and flag, and the click position; the result is 1 when `id` is a
 * panel (the queries below push the same) */
static int Opcode_Gfx0_SubObjGetCursor(Thread_t* t) // 90 BC: out, id → f
{
	uint32_t id = Thread_Pop(t);
	int32_t* out = (int32_t*)PopPtr(t);
	Thread_Push(t, (uint32_t)SubObj_GetCursor(out, id));
	return 0;
}

// the index of the panel's current group into *out (-1 for none)
static int Opcode_Gfx0_SubObjGetGroup(Thread_t* t) // 90 BD: out, id → f
{
	uint32_t id = Thread_Pop(t);
	int32_t* out = (int32_t*)PopPtr(t);
	Thread_Push(t, (uint32_t)SubObj_GetGroup(out, id));
	return 0;
}

// the selected button index of every group into `out` (one word per group)
static int Opcode_Gfx0_SubObjGetSelections(Thread_t* t) // 90 BE: out, id → f
{
	uint32_t id = Thread_Pop(t);
	int32_t* out = (int32_t*)PopPtr(t);
	Thread_Push(t, (uint32_t)SubObj_GetSelections(out, id));
	return 0;
}

/* take the oldest event of the panel's queue into three words at `out`
 * (message, a, b; the 0x1000000n messages of src/sys/panel.c), zeros when
 * the queue is empty */
static int Opcode_Gfx0_SubObjPopEvent(Thread_t* t) // 90 BF: out, id → f
{
	uint32_t id = Thread_Pop(t);
	uint32_t* out = (uint32_t*)PopPtr(t);
	Thread_Push(t, (uint32_t)SubObj_PopEvent(out, id));
	return 0;
}

// -------------------------------------------------------------------------
// 90 D0 .. 90 DF : knobs (sliders)
// -------------------------------------------------------------------------

/* A knob turns a display object (its target, the thumb) into a slider: it
 * becomes the target's owner, keeps the thumb inside a travel rectangle
 * relative to the knob's own position and converts between the thumb's
 * pixel displacement and a logical (x, y) value in steps.  The main
 * window's registry (src/sys/knobreg.c) lets the mouse drag it and flags
 * right clicks; "captured" knobs also take the mouse wheel. */

/* create a knob driving `target`, which must not have an owner yet and not
 * be a virtual object, group or knob itself; the knob is registered with
 * the main window so the mouse can drag it */
static int Opcode_Gfx0_KnobCreate(Thread_t* t) // 90 D0: target → h
{
	uint32_t target = Thread_Pop(t);
	uint32_t h;
	if(GfxCall_ObjHasOwner(target))
		ScriptError(MSG_OBJ_HAS_OWNER, t);
	switch(GfxCall_KnobCreate(&h, target))
	{
		case 1: ScriptError(MSG_NO_MORE_KNOBS, t); break;
		case 2: ScriptError(MSG_BAD_TARGET_HANDLE, t); break;
		case 3: ScriptError(MSG_VIRTUAL_TARGET, t); break;
		default: break;
	}
	Knob_Register(h);
	Thread_Push(t, h);
	return 0;
}

// release, unregister and delete the knob; the target object stays and loses its owner
static int Opcode_Gfx0_KnobDelete(Thread_t* t) // 90 D1: h →
{
	uint32_t h = Thread_Pop(t);
	Knob_Release(h);
	Knob_Unregister(h);
	if(!GfxCall_KnobDelete(h))
		ScriptError(MSG_BAD_KNOB_HANDLE, t);
	return 0;
}

static int Opcode_Gfx0_KnobShow(Thread_t* t) // 90 D4: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_KnobShow(h, f))
		ScriptError(MSG_BAD_KNOB_HANDLE, t);
	return 0;
}

// the track origin: the travel rectangle is relative to it
static int Opcode_Gfx0_KnobSetPos(Thread_t* t) // 90 D5: h, x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_KnobSetPos(h, x, y))
		ScriptError(MSG_BAD_KNOB_HANDLE, t);
	return 0;
}

/* the knob's value (in steps, or pixels for a continuous axis) and with it
 * the thumb's position; a value outside the range is dropped silently */
static int Opcode_Gfx0_KnobSetValue(Thread_t* t) // 90 D6: h, x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_KnobSetValue(h, x, y))
		ScriptError(MSG_BAD_KNOB_HANDLE, t);
	return 0;
}

// the knob's current value; x is pushed first, y is on top
static int Opcode_Gfx0_KnobGetValue(Thread_t* t) // 90 D7: h → x, y
{
	int32_t out[2];
	if(!GfxCall_KnobGetValue(Thread_Pop(t), out))
		ScriptError(MSG_BAD_KNOB_HANDLE, t);
	Thread_Push(t, (uint32_t)out[0]);
	Thread_Push(t, (uint32_t)out[1]);
	return 0;
}

// the number of positions per axis (0 = continuous, in pixels); 4 for a negative count
static int Opcode_Gfx0_KnobSetSteps(Thread_t* t) // 90 D8: h, x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_KnobSetSteps(h, x, y))
	{
		case 4: GFX_ERROR(t, MSG_BAD_KNOB_STEP, x, y); break;
		case -1: ScriptError(MSG_BAD_KNOB_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// the size of the travel rectangle in pixels; 5 when it would not hold the thumb
static int Opcode_Gfx0_KnobSetRange(Thread_t* t) // 90 D9: h, w, hgt →
{
	int hgt = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_KnobSetRange(h, w, hgt))
	{
		case 5: GFX_ERROR(t, MSG_BAD_KNOB_RANGE, w, hgt); break;
		case -1: ScriptError(MSG_BAD_KNOB_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* report and clear the last keyboard / wheel nudge of the knob: the
 * vertical step it asked for when the range rejected it (so the script can
 * act on a knob pushed past its end), 0 otherwise */
static int Opcode_Gfx0_KnobTakeNudge(Thread_t* t) // 90 DA: h → v
{
	int32_t out;
	if(!GfxCall_KnobTakeNudge(Thread_Pop(t), &out))
		ScriptError(MSG_BAD_KNOB_HANDLE, t);
	Thread_Push(t, (uint32_t)out);
	return 0;
}

// the handle of the last knob flagged by a right click (0 when none); every flag is cleared
static int Opcode_Gfx0_KnobPollChanged(Thread_t* t) // 90 DB: → h
{
	Thread_Push(t, Knob_PollChanged());
	return 0;
}

// whether the mouse may drag the knob's thumb (the default)
static int Opcode_Gfx0_KnobSetDraggable(Thread_t* t) // 90 DC: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_KnobSetDraggable(h, f))
		ScriptError(MSG_BAD_KNOB_HANDLE, t);
	return 0;
}

/* set the global knob-wheel switch and push its previous value: while it
 * is on, the mouse wheel nudges the most recently captured knob instead of
 * being reported as the wheel keys */
static int Opcode_Gfx0_KnobSwapGlobal(Thread_t* t) // 90 DD: v → old
{
	int v = (int)Thread_Pop(t);
	int old = Knob_GetGlobal();
	Knob_SetGlobal(v);
	Thread_Push(t, (uint32_t)old);
	return 0;
}

// put the knob on top of the captured list (the one the wheel nudges, see "90 DD")
static int Opcode_Gfx0_KnobCapture(Thread_t* t) // 90 DE: h →
{
	if(!Knob_Capture(Thread_Pop(t)))
		ScriptError(MSG_BAD_KNOB_HANDLE, t);
	return 0;
}

/* take the knob off the captured list; the handle error is raised also for
 * a knob that was not captured */
static int Opcode_Gfx0_KnobRelease(Thread_t* t) // 90 DF: h →
{
	if(!Knob_Release(Thread_Pop(t)))
		ScriptError(MSG_BAD_KNOB_HANDLE, t);
	return 0;
}

// -------------------------------------------------------------------------
// 90 CC, 90 CD : tone curves (1.616 on; src/gfx/tonecurve.c)
// -------------------------------------------------------------------------

/* register tone curve `id` from the six via-point words at `points` (x, y
 * of the R, G and B curves), or remove it when `points` is 0.  Script
 * error: no curve `id` to remove.  Out-of-range points register nothing
 * and pass silently, as in the original (its handler compares the
 * registration's code against the wrong value). */
static int Opcode_Gfx0_ToneCurveRegister(Thread_t* t) // 90 CC (1.616 on): id, points →
{
	const int32_t* points = (const int32_t*)PopPtr(t);
	int32_t id = (int32_t)Thread_Pop(t);
	if(ToneCurve_Register(id, points) == 0x8000000fu)
		GFX_ERROR(t, MSG_TONE_BAD_ID, id);
	return 0;
}

/* dst becomes src (32-bit) through curve `curveId` with the monochrome
 * mix (colour, level 0 .. 256) and the colour film (effect 8 / 0x26,
 * colour, level 0 .. 256); see BmpOp_ToneCurve for the errors */
static int Opcode_Gfx0_ToneCurveApply(Thread_t* t) // 90 CD (1.616 on): dst, src, monoColour, monoLevel, curveId, filmColour, effect, filmLevel →
{
	int filmLevel = (int)Thread_Pop(t);
	int effect = (int)Thread_Pop(t);
	uint32_t filmColour = Thread_Pop(t);
	int32_t curveId = (int32_t)Thread_Pop(t);
	int monoLevel = (int)Thread_Pop(t);
	uint32_t monoColour = Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	switch(BmpOp_ToneCurve(dst, src, curveId, monoColour, monoLevel, filmColour, effect, filmLevel))
	{
		case 0x80000009u: GFX_ERROR(t, MSG_TONE_DST_INVALID, dst); break;
		case 0x8000000au: GFX_ERROR(t, MSG_GEN_SRC_INVALID, src); break;
		case 0x8000000fu: GFX_ERROR(t, MSG_TONE_NO_CURVE, curveId); break;
		case 0x80000011u: GFX_ERROR(t, MSG_TONE_MODE_UNSUPPORTED, src); break;
		case 0x80000013u: GFX_ERROR(t, MSG_TONE_EFFECT_UNSUPPORTED, effect); break;
		case 0x80000014u: GFX_ERROR(t, MSG_TONE_BAD_FILM_LEVEL, filmLevel); break;
		case 0x80000015u: GFX_ERROR(t, MSG_TONE_BAD_MONO_LEVEL, monoLevel); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 90 E0 .. 90 E9 : groups
// -------------------------------------------------------------------------

static int Opcode_Gfx0_GroupCreate(Thread_t* t) // 90 E0: → h
{
	uint32_t h = GfxCall_GroupCreate();
	if(!h)
		ScriptError(MSG_NO_MORE_GROUPS, t);
	Thread_Push(t, h);
	return 0;
}

static int Opcode_Gfx0_GroupDelete(Thread_t* t) // 90 E1: h →
{
	if(!GfxCall_GroupDelete(Thread_Pop(t)))
		ScriptError(MSG_BAD_GROUP_HANDLE, t);
	return 0;
}

static int Opcode_Gfx0_GroupShow(Thread_t* t) // 90 E4: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_GroupShow(h, f))
		ScriptError(MSG_BAD_GROUP_HANDLE, t);
	return 0;
}

/* A group is an object without a picture that carries children: a child
 * added to it follows the group's position (at its offset), inherits its
 * level, fade and visibility, and has the group as its owner until it is
 * removed again. */

// the group's position and level; both are propagated to the children
static int Opcode_Gfx0_GroupSet(Thread_t* t) // 90 E5: h, x, y, level →
{
	uint32_t level = Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckAlpha(level, t);
	if(!GfxCall_GroupSet(h, x, y, (int)level))
		ScriptError(MSG_BAD_GROUP_HANDLE, t);
	return 0;
}

/* add object `hObj` to the group at offset (dx, dy) from the group's
 * position; refused when the object is the group itself or already has an
 * owner */
static int Opcode_Gfx0_GroupAdd(Thread_t* t) // 90 E8: hGroup, hObj, dx, dy →
{
	int dy = (int)Thread_Pop(t);
	int dx = (int)Thread_Pop(t);
	uint32_t hObj = Thread_Pop(t);
	uint32_t hGroup = Thread_Pop(t);
	switch(GfxCall_GroupAdd(hGroup, hObj, dx, dy))
	{
		case 1: ScriptError(MSG_BAD_OBJ_HANDLE, t); break;
		case 3: ScriptError(MSG_GROUP_SELF, t); break;
		case 4: ScriptError(MSG_OBJ_HAS_OWNER, t); break;
		case -1: ScriptError(MSG_BAD_GROUP_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// take the object out of the group again; 2 when it is not a member
static int Opcode_Gfx0_GroupRemove(Thread_t* t) // 90 E9: hGroup, hObj →
{
	uint32_t hObj = Thread_Pop(t);
	uint32_t hGroup = Thread_Pop(t);
	switch(GfxCall_GroupRemove(hGroup, hObj))
	{
		case 1: ScriptError(MSG_BAD_OBJ_HANDLE, t); break;
		case 2: ScriptError(MSG_NOT_IN_GROUP, t); break;
		case -1: ScriptError(MSG_BAD_GROUP_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 90 F0 .. 90 F3 : movies
// -------------------------------------------------------------------------

/* Start playing the loose movie file `file` (looked up in the game
 * directory, then the alternative one) in the (x, y, w, h) rectangle of the
 * window; the script goes on while it plays.  A missing file brings up the
 * retry prompt until it exists.  The result is the movie's length in ms as
 * reported by the player, 0 when playback could not start. */
static int Opcode_Gfx0_MoviePlay(Thread_t* t) // 90 F0: file, x, y, w, h → length
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	const char* file = (const char*)PopPtr(t);
	char path[0x104];
	int32_t length = 0;
	if(w <= 0 || h <= 0)
		GFX_ERROR(t, MSG_BAD_MOVIE_SIZE, w, h);
	while(!FileExists(NULL, file))
	{
		char msg[0x100];
		sprintf(msg, MSG_MOVIE_NOT_FOUND, file);
		RetryPrompt(msg);
	}
	FindMoviePath(path, file);
	Thread_Push(t, Movie_Play(&length, path, x, y, w, h) ? (uint32_t)length : 0u);
	return 0;
}

// stop the movie and repaint the window from the back buffer
static int Opcode_Gfx0_MovieStop(Thread_t* t) // 90 F1: →
{
	Movie_Stop();
	Window_Repaint();
	return 0;
}

// whether a movie is still playing
static int Opcode_Gfx0_MoviePlaying(Thread_t* t) // 90 F2: → f
{
	Thread_Push(t, (uint32_t)Movie_IsPlaying());
	return 0;
}

// the movie volume, 0 .. 0x80 (anything above is treated as 0)
static int Opcode_Gfx0_MovieVolume(Thread_t* t) // 90 F3: v →
{
	Movie_SetVolume(Thread_Pop(t));
	return 0;
}

// -------------------------------------------------------------------------
// 90 F4 .. 90 F7 : BF_Movie sequences (1.69/451 on; 90 F7 from 472)
// -------------------------------------------------------------------------

/* A BF_Movie sequence is a frame-by-frame animation file kept in memory by
 * the registry of src/gfx/bmseq.c; the script decodes single frames into a
 * bitmap slot of the movie's size and pixel mode.  These instructions push
 * their result code instead of raising an error: the 0x8000000n codes of
 * the registry are mapped to the small numbers given below. */

/* load the sequence file `name` of archive `arc` on the loader thread.  The
 * wait writes the new sequence number to *no and width, height, pixel mode,
 * frame rate and frame count to the five words at `info`, then pushes
 * 0 ok / 2 not a movie file (1 when the wait ended before the load did). */
static int Opcode_Gfx0_SeqLoad(Thread_t* t) // 90 F4 (1.69/472 on): &no, &info, arc, name →
{
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	int32_t* info = (int32_t*)PopPtr(t);
	int32_t* outNo = (int32_t*)PopPtr(t);
	Thread_SetWait(t, WaitSeqLoad_New(t, arc, name, outNo, info));
	return 2;
}

/* The first version of the instruction (1.69 build 451, the retail
 * Tayutama) reads the file on the calling thread: it pushes 0 when the
 * file is missing (nothing written), 1 when it could not be read, 2 when
 * it is not a movie file, and 0 once the sequence is registered. */
static int Opcode_Gfx0_SeqLoadSync(Thread_t* t) // 90 F4 (1.69/451): &no, &info, arc, name → r
{
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	int32_t* info = (int32_t*)PopPtr(t);
	int32_t* outNo = (int32_t*)PopPtr(t);
	uint32_t size = GetFileSize_(arc, name), r = 0;
	if(size)
	{
		uint8_t* buf = (uint8_t*)BGI_Alloc(size);
		if(LoadFile(buf, arc, name) != size)
			r = 1;
		else
			switch(BmSeq_Register(buf, size, outNo, info))
			{
				case 0: r = 0; break;
				case 0x80000001u: r = 1; break;
				case 0x80000002u: r = 2; break;
				default: r = 0x80000002u; break;
			}
		BGI_Free(buf);
	}
	Thread_Push(t, r);
	return 0;
}

// release sequence `no` (its data goes with the last of it and its clone); 0 ok, 3 no such sequence
static int Opcode_Gfx0_SeqFree(Thread_t* t) // 90 F5: no → r
{
	uint32_t r = BmSeq_Free((int32_t)Thread_Pop(t));
	Thread_Push(t, r == 0x80000003u ? 3 : r);
	return 0;
}

/* Decode frame `frame` of sequence `no` into bitmap `bmp`, which must
 * already have the movie's size and pixel mode.  Result: 0 ok, 3 no such
 * sequence, 4 no such frame, 5 the slot does not match, 6 (the registry's
 * 0x80000006, which this build's decoder does not produce); other codes
 * (a type this build cannot decode, a failed decode) are pushed as they
 * are.  After "80 53" the frame is decoded inside a wait object instead,
 * which pushes 0 / 3 / 5 / 8 (failed) when it ends. */
static int Opcode_Gfx0_SeqDecode(Thread_t* t) // 90 F6: bmp, no, frame → r
{
	uint32_t frame = Thread_Pop(t);
	int no = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	uint32_t r;
	if(BmSeq_TakeDeferFlag())
	{
		Thread_SetWait(t, WaitSeqDecode_New(t, bmp, no, frame));
		return 2;
	}
	r = BmSeq_Decode(bmp, no, frame);
	switch(r)
	{
		case 0x80000003u: r = 3; break;
		case 0x80000004u: r = 4; break;
		case 0x80000005u: r = 5; break;
		case 0x80000006u: r = 6; break;
		default: break;
	}
	Thread_Push(t, r);
	return 0;
}

/* give sequence `no` a second number (written to *out) that shares its
 * data; 0 ok, 3 no such sequence, 7 it already has a clone */
static int Opcode_Gfx0_SeqClone(Thread_t* t) // 90 F7: no, &out → r
{
	int32_t* out = (int32_t*)PopPtr(t);
	int no = (int)Thread_Pop(t);
	uint32_t r = BmSeq_Clone(no, out);
	Thread_Push(t, r == 0x80000003u ? 3 : r == 0x80000007u ? 7
														   : r);
	return 0;
}

// -------------------------------------------------------------------------
// 90 F8 .. 90 FD : click targets
// -------------------------------------------------------------------------

/* The click targets are a list of sprites (src/sysobj/targets.c), separate
 * from the knobs and the selections' virtual objects, that the script polls
 * for the mouse: each registered sprite gets the running number of its
 * registration as its index. */

// empty the target list
static int Opcode_Gfx0_TargetClear(Thread_t* t) // 90 F8: →
{
	Target_Clear();
	return 0;
}

// register sprite `h` as a click target; an error when the handle is not a sprite
static int Opcode_Gfx0_TargetAdd(Thread_t* t) // 90 FA: h →
{
	if(!Target_Add(Thread_Pop(t)))
		ScriptError(MSG_TARGET_NOT_SPRITE, t);
	return 0;
}

// unregister the sprite; an error when it was not registered
static int Opcode_Gfx0_TargetRemove(Thread_t* t) // 90 FB: h →
{
	if(!Target_Remove(Thread_Pop(t)))
		ScriptError(MSG_TARGET_NOT_REGISTERED, t);
	return 0;
}

// the index of the first target under the mouse (by its hit test), 0xFFFFFFFF for none
static int Opcode_Gfx0_TargetHit(Thread_t* t) // 90 FC: → index
{
	Thread_Push(t, Target_Hit());
	return 0;
}

// the last sampled press state (1 pressed) of the target with that index
static int Opcode_Gfx0_TargetGet(Thread_t* t) // 90 FD: index → pressed
{
	uint32_t out;
	if(!Target_Get(&out, Thread_Pop(t)))
		ScriptError(MSG_BAD_TARGET_NO, t);
	Thread_Push(t, out);
	return 0;
}

// -------------------------------------------------------------------------
// the table
// -------------------------------------------------------------------------

/* Fill vm_optable_90 for the selected engine profile from the registrations
 * below (called once at start-up by Vm_TablesInit).  Each entry names the
 * opcode, its handler and the generations the handler is valid for; an
 * opcode whose meaning changed between builds has several entries, and
 * Vm_FillTable takes the one with the narrowest range that covers the
 * profile.  An opcode the profile defines without a matching entry gets
 * the not-implemented stub, one it does not define stays NULL. */
void Vm_Optable90Init(void)
{
	static const OpEntry_t ops[] = {
		{0x00, Opcode_Gfx0_Present, GEN_FIRST, GEN_LAST},
		{0x01, Opcode_Gfx0_DisplayEnable, GEN_FIRST, GEN_LAST},
		{0x02, Opcode_Gfx0_SetFps, GEN_FIRST, GEN_LAST},
		{0x03, Opcode_Gfx0_SetCacheSize, GEN_FIRST, GEN_LAST},
		{0x04, Opcode_Gfx0_Snapshot, GEN_FIRST, GEN_LAST},
		{0x05, Opcode_Gfx0_SnapshotPrio, GEN_FIRST, GEN_LAST},
		{0x06, Opcode_Gfx0_SetProjCentre, GEN_1_529, GEN_LAST},
		{0x17, Opcode_Gfx0_BmpSetMode, GEN_1_494, GEN_LAST},
		{0x07, Opcode_Gfx0_SetSyncLoadWindow, GEN_1_69_472, GEN_LAST},
		{0x08, Opcode_Gfx0_SetGlobalEffect, GEN_FIRST, GEN_LAST},
		{0x09, Opcode_Gfx0_SetDrawLimit, GEN_FIRST, GEN_LAST},
		{0x0A, Opcode_Gfx0_WaitPresentMode, GEN_FIRST, GEN_LAST},
		{0x0B, Opcode_Gfx0_BmpMgrOption, GEN_FIRST, GEN_LAST},
		{0x0C, Opcode_Gfx0_WindowGlobal, GEN_FIRST, GEN_LAST},
		{0x0D, Opcode_Gfx0_SetAntialias, GEN_FIRST, GEN_LAST},
		{0x0E, Opcode_Gfx0_FontCache, GEN_FIRST, GEN_LAST},
		{0x0F, Opcode_Gfx0_BmpOption, GEN_FIRST, GEN_LAST},
		{0x10, Opcode_Gfx0_BmpLoad, GEN_FIRST, GEN_LAST},
		{0x11, Opcode_Gfx0_BmpCreate, GEN_FIRST, GEN_LAST},
		{0x12, Opcode_Gfx0_BmpFree, GEN_FIRST, GEN_LAST},
		{0x13, Opcode_Gfx0_BmpClear, GEN_FIRST, GEN_LAST},
		{0x14, Opcode_Gfx0_BmpPut, GEN_FIRST, GEN_LAST},
		{0x15, Opcode_Gfx0_BmpGet, GEN_FIRST, GEN_LAST},
		{0x16, Opcode_Gfx0_BmpInfo, GEN_FIRST, GEN_LAST},
		{0x18, Opcode_Gfx0_BmpBlit, GEN_FIRST, GEN_LAST},
		{0x19, Opcode_Gfx0_BmpBlitMask, GEN_FIRST, GEN_LAST},
		{0x1A, Opcode_Gfx0_BmpDisplace, GEN_FIRST, GEN_LAST},
		{0x1B, Opcode_Gfx0_BmpGradient, GEN_FIRST, GEN_LAST},
		{0x1C, Opcode_Gfx0_BmpStretch, GEN_FIRST, GEN_LAST},
		{0x1D, Opcode_Gfx0_BmpRotate, GEN_FIRST, GEN_LAST},
		{0x1E, Opcode_Gfx0_BmpCopyRect, GEN_FIRST, GEN_LAST},
		{0x1F, Opcode_Gfx0_BmpCloneRect, GEN_FIRST, GEN_LAST},
		{0x20, Opcode_Gfx0_ObjFade, GEN_FIRST, GEN_LAST},
		{0x21, Opcode_Gfx0_ObjMove, GEN_FIRST, GEN_LAST},
		{0x22, Opcode_Gfx0_ObjFadeEx, GEN_FIRST, GEN_LAST},
		{0x23, Opcode_Gfx0_ObjMoveEx, GEN_FIRST, GEN_LAST},
		{0x24, Opcode_Gfx0_ObjMove2, GEN_FIRST, GEN_LAST},
		{0x28, Opcode_Gfx0_ObjTweenFull, GEN_FIRST, GEN_LAST},
		{0x29, Opcode_Gfx0_ObjPath, GEN_FIRST, GEN_LAST},
		{0x2C, Opcode_Gfx0_ObjQuake, GEN_FIRST, GEN_LAST},
		{0x30, Opcode_Gfx0_ObjShow, GEN_FIRST, GEN_LAST},
		{0x31, Opcode_Gfx0_ObjSetEnabled, GEN_FIRST, GEN_LAST},
		{0x32, Opcode_Gfx0_ObjSetLevel, GEN_FIRST, GEN_LAST},
		{0x33, Opcode_Gfx0_ObjSetPos, GEN_FIRST, GEN_LAST},
		{0x34, Opcode_Gfx0_ObjSetFade, GEN_FIRST, GEN_LAST},
		{0x39, Opcode_Gfx0_ObjSetOpacity, GEN_1_599, GEN_LAST},
		{0x35, Opcode_Gfx0_ObjSetProgress, GEN_FIRST, GEN_LAST},
		{0x36, Opcode_Gfx0_ObjSetOffset2, GEN_1_494, GEN_LAST},
		{0x37, Opcode_Gfx0_ObjSetOffset, GEN_FIRST, GEN_LAST},
		{0x38, Opcode_Gfx0_ObjSetParam, GEN_FIRST, GEN_LAST},
		{0x3A, Opcode_Gfx0_ObjSetPriority, GEN_1_69_472, GEN_LAST},
		{0x3C, Opcode_Gfx0_ObjSetHitMask, GEN_FIRST, GEN_LAST},
		{0x3D, Opcode_Gfx0_ObjHitTest, GEN_FIRST, GEN_LAST},
		{0x3F, Opcode_Gfx0_ObjBuildCache, GEN_FIRST, GEN_LAST},
		{0x40, Opcode_Gfx0_BgBitmap, GEN_FIRST, GEN_LAST},
		{0x41, Opcode_Gfx0_BgFade, GEN_FIRST, GEN_LAST},
		{0x42, Opcode_Gfx0_BgScroll, GEN_FIRST, GEN_LAST},
		{0x43, Opcode_Gfx0_BgBlend, GEN_FIRST, GEN_LAST},
		{0x44, Opcode_Gfx0_BgFrames, GEN_FIRST, GEN_LAST},
		{0x45, Opcode_Gfx0_BgDisplace, GEN_FIRST, GEN_LAST},
		{0x46, Opcode_Gfx0_BgGradient, GEN_FIRST, GEN_LAST},
		{0x47, Opcode_Gfx0_BgRipple, GEN_FIRST, GEN_LAST},
		{0x48, Opcode_Gfx0_BgView, GEN_FIRST, GEN_LAST},
		{0x49, Opcode_Gfx0_BgZoom, GEN_FIRST, GEN_LAST},
		{0x4A, Opcode_Gfx0_BgFlip, GEN_FIRST, GEN_LAST},
		{0x4C, Opcode_Gfx0_BgShow, GEN_FIRST, GEN_LAST},
		{0x4D, Opcode_Gfx0_BgTypeId, GEN_FIRST, GEN_LAST},
		{0x50, Opcode_Gfx0_SpriteCreate, GEN_FIRST, GEN_LAST},
		{0x51, Opcode_Gfx0_SpriteDelete, GEN_FIRST, GEN_LAST},
		{0x53, Opcode_Gfx0_SpriteUpdateRect, GEN_FIRST, GEN_LAST},
		{0x54, Opcode_Gfx0_SpriteShow, GEN_FIRST, GEN_LAST},
		{0x55, Opcode_Gfx0_SpriteSetMask, GEN_FIRST, GEN_LAST},
		{0x56, Opcode_Gfx0_SpriteSet, GEN_FIRST, GEN_LAST},
		{0x57, Opcode_Gfx0_SpriteChangeBitmap, GEN_FIRST, GEN_LAST},
		{0x58, Opcode_Gfx0_SpriteSetBlend, GEN_FIRST, GEN_LAST},
		/* the sprite mode instructions were renumbered twice: 1.58 had "90 59"
		 * ripple / "90 5A" scale / "90 5B" rescale, 1.64 "90 59" ripple /
		 * "90 5A" transform / "90 5B" transform (the second with the animated
		 * offsets of its sprite; both share one handler here), and 1.66
		 * settled on "90 59" transform / "90 5A" wipe / "90 5B" ripple */
		{0x59, Opcode_Gfx0_SpriteSetRipple, GEN_1_58, GEN_1_64},
		{0x59, Opcode_Gfx0_SpriteSetTransform, GEN_1_66, GEN_LAST},
		{0x5A, Opcode_Gfx0_SpriteSetScale_158, GEN_1_58, GEN_1_58},
		{0x5A, Opcode_Gfx0_SpriteSetTransform, GEN_1_64, GEN_1_64},
		{0x5A, Opcode_Gfx0_SpriteSetWipe, GEN_1_66, GEN_LAST},
		{0x5B, Opcode_Gfx0_SpriteRescale_158, GEN_1_58, GEN_1_58},
		{0x5B, Opcode_Gfx0_SpriteSetTransform, GEN_1_64, GEN_1_64},
		{0x5B, Opcode_Gfx0_SpriteSetRipple, GEN_1_66, GEN_LAST},
		{0x5C, Opcode_Gfx0_SpriteSetProjected, GEN_FIRST, GEN_LAST},
		{0x5D, Opcode_Gfx0_SpriteSetProjected3D, GEN_1_588, GEN_LAST},
		{0x60, Opcode_Gfx0_FilterCreate, GEN_FIRST, GEN_LAST},
		{0x61, Opcode_Gfx0_FilterDelete, GEN_FIRST, GEN_LAST},
		{0x64, Opcode_Gfx0_FilterShow, GEN_FIRST, GEN_LAST},
		{0x65, Opcode_Gfx0_FilterSet, GEN_FIRST, GEN_LAST},
		{0x66, Opcode_Gfx0_FilterSetBitmap, GEN_FIRST, GEN_LAST},
		{0x70, Opcode_Gfx0_MapCreate, GEN_FIRST, GEN_LAST},
		{0x71, Opcode_Gfx0_MapDelete, GEN_FIRST, GEN_LAST},
		{0x74, Opcode_Gfx0_MapShow, GEN_FIRST, GEN_LAST},
		{0x75, Opcode_Gfx0_MapSet, GEN_FIRST, GEN_LAST},
		{0x76, Opcode_Gfx0_MapSetSize, GEN_FIRST, GEN_LAST},
		{0x78, Opcode_Gfx0_MapSetTerrain, GEN_FIRST, GEN_LAST},
		{0x79, Opcode_Gfx0_MapSetView, GEN_FIRST, GEN_LAST},
		{0x7A, Opcode_Gfx0_MapInvalidateChip, GEN_FIRST, GEN_LAST},
		{0x80, Opcode_Gfx0_WindowCreate, GEN_FIRST, GEN_LAST},
		{0x81, Opcode_Gfx0_WindowDelete, GEN_FIRST, GEN_LAST},
		{0x83, Opcode_Gfx0_WindowCapture, GEN_FIRST, GEN_LAST},
		{0x84, Opcode_Gfx0_WindowShow, GEN_FIRST, GEN_LAST},
		{0x85, Opcode_Gfx0_WindowSet, GEN_FIRST, GEN_LAST},
		{0x86, Opcode_Gfx0_WindowSetFrame, GEN_FIRST, GEN_LAST},
		/* until 1.64 the font, spacing, punch and frame operations of the
		 * window sat at "90 87" .. "90 8D"; 1.66 moved them to "91 88" /
		 * "91 89" / "90 87" / "92 88" / "92 89" (the handlers live in the
		 * files of their later family and are shared through bgi/vm.h) */
		{0x87, Opcode_Gfx1_WindowSetFont, GEN_1_58, GEN_1_64},
		{0x87, Opcode_Gfx0_WindowSetPunch, GEN_1_66, GEN_LAST},
		{0x82, Opcode_Gfx0_WindowSetLayerOrder, GEN_1_494, GEN_LAST},
		{0x88, Opcode_Gfx0_WindowSetTextArea, GEN_FIRST, GEN_LAST},
		{0x89, Opcode_Gfx0_WindowGetTextArea, GEN_FIRST, GEN_LAST},
		{0x8A, Opcode_Gfx1_WindowSetSpacing, GEN_1_58, GEN_1_64},
		{0x8B, Opcode_Gfx0_WindowSetPunch, GEN_1_58, GEN_1_64},
		{0x8C, Opcode_Gfx2_WindowShowFrame, GEN_1_58, GEN_1_64},
		{0x8D, Opcode_Gfx2_WindowDrawToFrame, GEN_1_58, GEN_1_64},
		{0x90, Opcode_Gfx0_TextOut, GEN_FIRST, GEN_LAST},
		{0x91, Opcode_Gfx0_TextSetMode, GEN_FIRST, GEN_LAST},
		{0x94, Opcode_Gfx0_TextSetCharInterval, GEN_FIRST, GEN_LAST},
		{0x95, Opcode_Gfx0_TextSetScroll, GEN_FIRST, GEN_LAST},
		{0x96, Opcode_Gfx0_TextSetFade, GEN_FIRST, GEN_LAST},
		{0x97, Opcode_Gfx0_TextSetAutoAdvance, GEN_FIRST, GEN_LAST},
		{0x98, Opcode_Gfx0_TextSetBitmaps, GEN_FIRST, GEN_LAST},
		{0x99, Opcode_Gfx0_TextSetCursorInterval, GEN_FIRST, GEN_LAST},
		{0x9A, Opcode_Gfx0_TextSetCursorPos, GEN_FIRST, GEN_LAST},
		{0x9B, Opcode_Gfx0_TextSetStartDelay, GEN_FIRST, GEN_LAST},
		{0x9C, Opcode_Gfx0_TextSetStyleOn, GEN_FIRST, GEN_LAST},
		{0x9D, Opcode_Gfx0_TextSetStyleParams, GEN_FIRST, GEN_LAST},
		{0x9E, Opcode_Gfx0_TextSetGlyphSheet, GEN_FIRST, GEN_LAST},
		{0x9F, Opcode_Gfx0_TextSetInputFinishes, GEN_FIRST, GEN_LAST},
		{0xA0, Opcode_Gfx0_MenuSelect, GEN_FIRST, GEN_LAST},
		{0xA1, Opcode_Gfx0_MenuDraw, GEN_FIRST, GEN_LAST},
		{0xA2, Opcode_Gfx0_MenuSelectBmp, GEN_FIRST, GEN_LAST},
		{0xA3, Opcode_Gfx0_MenuSelectBmp0, GEN_FIRST, GEN_LAST},
		{0xA4, Opcode_Gfx0_MenuSetCursorColours, GEN_FIRST, GEN_LAST},
		{0xA5, Opcode_Gfx0_MenuSetBlink, GEN_FIRST, GEN_LAST},
		{0xA6, Opcode_Gfx0_MenuSetWheel, GEN_FIRST, GEN_LAST},
		{0xA7, Opcode_Gfx0_WindowSetTable, GEN_FIRST, GEN_LAST},
		{0xAF, Opcode_Gfx0_SelectRequireActive, GEN_FIRST, GEN_LAST},
		{0xB0, Opcode_Gfx0_IconSelect, GEN_FIRST, GEN_LAST},
		{0xB1, Opcode_Gfx0_IconSelect2, GEN_FIRST, GEN_LAST},
		{0xB4, Opcode_Gfx0_IconDraw, GEN_FIRST, GEN_LAST},
		{0xB5, Opcode_Gfx0_IconDraw64, GEN_FIRST, GEN_LAST},
		{0xB6, Opcode_Gfx0_PanelDraw, GEN_FIRST, GEN_LAST},
		{0xB7, Opcode_Gfx0_SprPanelDraw, GEN_FIRST, GEN_LAST},
		{0xB8, Opcode_Gfx0_SubObjCreate, GEN_FIRST, GEN_LAST},
		{0xB9, Opcode_Gfx0_SubObjDelete, GEN_FIRST, GEN_LAST},
		{0xBA, Opcode_Gfx0_PanelStart, GEN_FIRST, GEN_LAST},
		{0xBC, Opcode_Gfx0_SubObjGetCursor, GEN_FIRST, GEN_LAST},
		{0xBD, Opcode_Gfx0_SubObjGetGroup, GEN_FIRST, GEN_LAST},
		{0xBE, Opcode_Gfx0_SubObjGetSelections, GEN_FIRST, GEN_LAST},
		{0xBF, Opcode_Gfx0_SubObjPopEvent, GEN_FIRST, GEN_LAST},
		{0xD0, Opcode_Gfx0_KnobCreate, GEN_FIRST, GEN_LAST},
		{0xD1, Opcode_Gfx0_KnobDelete, GEN_FIRST, GEN_LAST},
		{0xD4, Opcode_Gfx0_KnobShow, GEN_FIRST, GEN_LAST},
		{0xD5, Opcode_Gfx0_KnobSetPos, GEN_FIRST, GEN_LAST},
		{0xD6, Opcode_Gfx0_KnobSetValue, GEN_FIRST, GEN_LAST},
		{0xD7, Opcode_Gfx0_KnobGetValue, GEN_FIRST, GEN_LAST},
		{0xD8, Opcode_Gfx0_KnobSetSteps, GEN_FIRST, GEN_LAST},
		{0xD9, Opcode_Gfx0_KnobSetRange, GEN_FIRST, GEN_LAST},
		{0xDA, Opcode_Gfx0_KnobTakeNudge, GEN_FIRST, GEN_LAST},
		{0xDB, Opcode_Gfx0_KnobPollChanged, GEN_FIRST, GEN_LAST},
		{0xDC, Opcode_Gfx0_KnobSetDraggable, GEN_FIRST, GEN_LAST},
		{0xDD, Opcode_Gfx0_KnobSwapGlobal, GEN_FIRST, GEN_LAST},
		{0xDE, Opcode_Gfx0_KnobCapture, GEN_FIRST, GEN_LAST},
		{0xDF, Opcode_Gfx0_KnobRelease, GEN_FIRST, GEN_LAST},
		{0xCC, Opcode_Gfx0_ToneCurveRegister, GEN_1_616, GEN_LAST},
		{0xCD, Opcode_Gfx0_ToneCurveApply, GEN_1_616, GEN_LAST},
		{0xE0, Opcode_Gfx0_GroupCreate, GEN_FIRST, GEN_LAST},
		{0xE1, Opcode_Gfx0_GroupDelete, GEN_FIRST, GEN_LAST},
		{0xE4, Opcode_Gfx0_GroupShow, GEN_FIRST, GEN_LAST},
		{0xE5, Opcode_Gfx0_GroupSet, GEN_FIRST, GEN_LAST},
		{0xE8, Opcode_Gfx0_GroupAdd, GEN_FIRST, GEN_LAST},
		{0xE9, Opcode_Gfx0_GroupRemove, GEN_FIRST, GEN_LAST},
		{0xF0, Opcode_Gfx0_MoviePlay, GEN_FIRST, GEN_LAST},
		{0xF1, Opcode_Gfx0_MovieStop, GEN_FIRST, GEN_LAST},
		{0xF2, Opcode_Gfx0_MoviePlaying, GEN_FIRST, GEN_LAST},
		{0xF3, Opcode_Gfx0_MovieVolume, GEN_FIRST, GEN_LAST},
		{0xF4, Opcode_Gfx0_SeqLoadSync, GEN_1_69_451, GEN_1_69_451},
		{0xF4, Opcode_Gfx0_SeqLoad, GEN_1_69_472, GEN_LAST},
		{0xF5, Opcode_Gfx0_SeqFree, GEN_1_69_451, GEN_LAST},
		{0xF6, Opcode_Gfx0_SeqDecode, GEN_1_69_451, GEN_LAST},
		{0xF7, Opcode_Gfx0_SeqClone, GEN_1_69_472, GEN_LAST},
		{0xF8, Opcode_Gfx0_TargetClear, GEN_FIRST, GEN_LAST},
		{0xFA, Opcode_Gfx0_TargetAdd, GEN_FIRST, GEN_LAST},
		{0xFB, Opcode_Gfx0_TargetRemove, GEN_FIRST, GEN_LAST},
		{0xFC, Opcode_Gfx0_TargetHit, GEN_FIRST, GEN_LAST},
		{0xFD, Opcode_Gfx0_TargetGet, GEN_FIRST, GEN_LAST},
	};
	Vm_FillTable(vm_optable_90, OPFAM_90, ops, BGI_COUNTOF(ops));
}
