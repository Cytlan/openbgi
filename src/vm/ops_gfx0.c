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
#include "bgi/core.h"

// raise a script error on thread t with a formatted MSG_* text; does not return
#define GFX_ERROR(o, t, ...)        \
	do                              \
	{                               \
		char msg_[0x104];           \
		sprintf(msg_, __VA_ARGS__); \
		ScriptError(o, msg_, (t));  \
	} while(0)

// -------------------------------------------------------------------------
// 90 00 .. 90 0F : display control
// -------------------------------------------------------------------------

// request a present at the end of the pass: the whole picture, or only the dirty rectangles
static int Opcode_Gfx0_Present(OpenBGI_t* o, Thread_t* t) // 90 00: full →
{
	if(Thread_Pop(t))
		Present_RequestFull(o);
	else
		Present_RequestDirty(o);
	return 0;
}

static int Opcode_Gfx0_DisplayEnable(OpenBGI_t* o, Thread_t* t) // 90 01: f →
{
	Display_Enable(o, (int)Thread_Pop(t));
	return 0;
}

// the presentation rate: the frame interval becomes 1000 / fps ms
static int Opcode_Gfx0_SetFps(OpenBGI_t* o, Thread_t* t) // 90 02: fps →
{
	uint32_t fps = Thread_Pop(t);
	// 1.58 accepted 0 .. 1000 as a signed value; from 1.64 the rate must be at least 1
	if(o->engine.profile->gen == GEN_1_58 ? (int)fps < 0 || (int)fps > 1000 : fps < 1 || fps > 1000)
		GFX_ERROR(o, t, MSG_BAD_FRAME_RATE, (int)fps);
	Present_SetRate(o, (int)fps);
	return 0;
}

// the capacity of the loader's file cache that the bitmap loads consult first (0 = none)
static int Opcode_Gfx0_SetCacheSize(OpenBGI_t* o, Thread_t* t) // 90 03: bytes →
{
	uint32_t size = Thread_Pop(t);
	// 1.58 / 1.64 allowed up to 64 MB, 1.66 on 512 MB
	uint32_t limit = o->engine.profile->gen <= GEN_1_64 ? 0x4000000 : 0x20000000;
	if(o->engine.profile->gen == GEN_1_58 ? (int)size < 0 || size > limit : size > limit)
		GFX_ERROR(o, t, MSG_BAD_CACHE_SIZE, (int)size);
	Loader_SetCacheSize(o, size);
	return 0;
}

// create bitmap `bmp` at screen size and copy the back buffer into it
static int Opcode_Gfx0_Snapshot(OpenBGI_t* o, Thread_t* t) // 90 04: bmp →
{
	int bmp = (int)Thread_Pop(t);
	CheckBitmapNo(o, bmp, t);
	GfxCall_Snapshot(o, bmp);
	return 0;
}

// as "90 04", but the picture is re-composed from the objects up to priority `prio`
static int Opcode_Gfx0_SnapshotPrio(OpenBGI_t* o, Thread_t* t) // 90 05: bmp, prio →
{
	uint32_t prio = Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckPriority(o, prio, t);
	CheckBitmapNo(o, bmp, t);
	GfxCall_SnapshotPrio(o, bmp, (int)prio);
	return 0;
}

/* the global effect value (o->gfx.globalEffectValue) every object's
 * draw consults; the display is redrawn in full */
static int Opcode_Gfx0_SetGlobalEffect(OpenBGI_t* o, Thread_t* t) // 90 08: v →
{
	GfxCall_SetGlobalEffect(o, (int)Thread_Pop(t));
	return 0;
}

// the compositor's draw limit: objects below priority `prio` are not drawn
static int Opcode_Gfx0_SetDrawLimit(OpenBGI_t* o, Thread_t* t) // 90 09: prio →
{
	uint32_t prio = Thread_Pop(t);
	CheckPriority(o, prio, t);
	GfxCall_SetDrawLimit(o, (int)prio);
	return 0;
}

/* how a wait object presents what it drew: `onWait` whether it requests a
 * present at all when it is dirty, `full` whether that present is the whole
 * picture rather than the dirty rectangles */
static int Opcode_Gfx0_WaitPresentMode(OpenBGI_t* o, Thread_t* t) // 90 0A: onWait, full →
{
	int full = (int)Thread_Pop(t);
	int onWait = (int)Thread_Pop(t);
	Wait_SetPresentMode(o, onWait, full);
	return 0;
}

// the bitmap manager's option: a re-created slot keeps its generation number when set
static int Opcode_Gfx0_BmpMgrOption(OpenBGI_t* o, Thread_t* t) // 90 0B: v →
{
	BmpMgr_SetOption(o, (int)Thread_Pop(t));
	return 0;
}

/* the visibility switch and the transparency added to all message windows;
 * every window is repainted */
static int Opcode_Gfx0_WindowGlobal(OpenBGI_t* o, Thread_t* t) // 90 0C: shown, level →
{
	uint32_t level = Thread_Pop(t);
	int shown = (int)Thread_Pop(t);
	CheckAlpha(o, level, t);
	GfxCall_WindowGlobal(o, shown, (int)level);
	return 0;
}

// the font anti-aliasing level, 0 .. 3
static int Opcode_Gfx0_SetAntialias(OpenBGI_t* o, Thread_t* t) // 90 0D: level →
{
	int level = (int)Thread_Pop(t);
	if(!Gfx_SetAntialias(o, level))
		GFX_ERROR(o, t, MSG_BAD_AA_LEVEL, level);
	return 0;
}

/* the font cache request of "90 0E": font number `fontNo` resolved to its
 * face name, then BmpMgr_FontRequest with its result (0 ok, 0x80000001
 * amount below 2, 0x80000002 bad size, 0x80000003 bad width percentage);
 * 0x80000004 for an unknown font number */
static int FontCacheRequest(OpenBGI_t* o, int fontNo, int size, int widthPct, int bold, int amount)
{
	const char* name = FontNameByNo(o, fontNo);
	if(!name)
		return (int)0x80000004;
	return BmpMgr_FontRequest(o->gfx.bmpMgr, name, size, widthPct, bold, amount);
}

/* Remember how many glyphs (`amount`) the glyph cache is to hold for the
 * font of number `fontNo` at `size` pixels, `widthPct` percent width and
 * weight `bold`.  The error messages are crossed in the original and kept
 * so: a bad cache amount (0x80000001) reports "invalid font number" with
 * the font number, and an unknown font number (0x80000004) reports
 * "invalid cache amount" with the amount. */
static int Opcode_Gfx0_FontCache(OpenBGI_t* o, Thread_t* t) // 90 0E: fontNo, size, widthPct, bold, amount →
{
	int amount = (int)Thread_Pop(t);
	int bold = (int)Thread_Pop(t);
	int widthPct = (int)Thread_Pop(t);
	int size = (int)Thread_Pop(t);
	int fontNo = (int)Thread_Pop(t);
	switch(FontCacheRequest(o, fontNo, size, widthPct, bold, amount))
	{
		case(int)0x80000001: GFX_ERROR(o, t, MSG_BAD_FONT_NO, fontNo); break;
		case(int)0x80000002: GFX_ERROR(o, t, MSG_BAD_FONT_SIZE, size); break;
		case(int)0x80000003: GFX_ERROR(o, t, MSG_BAD_FONT_WIDTH, widthPct); break;
		case(int)0x80000004: GFX_ERROR(o, t, MSG_BAD_FONT_CACHE_AMOUNT, amount); break;
		default: break;
	}
	return 0;
}

/* the key colour (o->gfx.bmpOption, 0 = none) that an ARGB32 picture
 * put with "90 14" was composited over; the manager undoes that
 * composition to recover the original colours */
static int Opcode_Gfx0_BmpOption(OpenBGI_t* o, Thread_t* t) // 90 0F: v →
{
	Gfx_SetBmpOption(o, (int)Thread_Pop(t));
	return 0;
}

// -------------------------------------------------------------------------
// 90 10 .. 90 1F : bitmaps
// -------------------------------------------------------------------------

/* The window of synchronous loading set by "90 07" (1.69 build 472 on).
 * While it is open every "90 10" load is done synchronously; it opens with
 * the first load after "90 07" and closes o->ops.syncLoadMs later unless
 * the skip key keeps it open.  With no window set the skip key alone
 * decides, as in the builds before 1.69/472. */
// whether the "90 10" load about to start is done synchronously (1) or on the loader thread (0)
static int BmpLoad_Synchronous(OpenBGI_t* o)
{
	int skip = Input_CheckSkip(o);
	if(o->ops.syncLoadMs)
	{
		if(o->ops.syncLoadUntil == 0)
		{
			o->ops.syncLoadUntil = GetTicks(o) + o->ops.syncLoadMs;
			return 1;
		}
		if(skip || GetTicks(o) < o->ops.syncLoadUntil)
			return 1;
		o->ops.syncLoadUntil = 0;
		return 0;
	}
	if(!skip)
		o->ops.syncLoadUntil = 0;
	return skip;
}

// set the length of the synchronous-loading window (see BmpLoad_Synchronous) and close an open one
static int Opcode_Gfx0_SetSyncLoadWindow(OpenBGI_t* o, Thread_t* t) // 90 07 (1.69/472 on): ms →
{
	o->ops.syncLoadMs = Thread_Pop(t);
	o->ops.syncLoadUntil = 0;
	return 0;
}

// the vanishing point of the projected sprites ("90 5C"); (-1, -1) restores the back-buffer centre
static int Opcode_Gfx0_SetProjCentre(OpenBGI_t* o, Thread_t* t) // 90 06 (1.529 on): x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	Gfx_SetProjCentre(o->gfx.gfx, x, y);
	return 0;
}

/* Convert the bitmap in slot `bmp` to pixel mode `mode`.  The result is
 * 0 ok, 1 no bitmap in the slot, 2 the conversion is not possible; for any
 * other manager code the pushed value is `mode` itself. */
static int Opcode_Gfx0_BmpSetMode(OpenBGI_t* o, Thread_t* t) // 90 17 (1.492 on): bmp, mode → r
{
	int mode = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	uint32_t r = BmpMgr_SetMode(o->gfx.bmpMgr, bmp, mode);
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
static int Opcode_Gfx0_BmpLoad(OpenBGI_t* o, Thread_t* t) // 90 10: bmp, arc, name →
{
	const char* name = (const char*)PopPtr(o, t);
	const char* arc = (const char*)PopPtr(o, t);
	int bmp = (int)Thread_Pop(t);
	CheckBitmapNo(o, bmp, t);
	if(!BmpLoad_Synchronous(o))
	{
		Thread_SetWait(o, t, WaitBmpLoad_New(o, t, bmp, arc, name));
		return 2;
	}
	switch(BmpOp_LoadFile(o, bmp, arc, name))
	{
		case(int)0x80000002: GFX_ERROR(o, t, MSG_BMP_NOT_WINDOWS, arc, name); break;
		case(int)0x80000003: GFX_ERROR(o, t, MSG_BMP_BAD_PLANES, arc, name); break;
		case(int)0x80000004: GFX_ERROR(o, t, MSG_BMP_BAD_BITCOUNT, arc, name); break;
		case(int)0x80000005: GFX_ERROR(o, t, MSG_BMP_COMPRESSED, arc, name); break;
		case(int)0x80000006: GFX_ERROR(o, t, MSG_BMP_BAD_SIZE, arc, name); break;
		case(int)0x80000008: GFX_ERROR(o, t, MSG_BMP_OUT_OF_MEMORY, arc, name); break;
		default: break; // 0, 0x80000001 (not a BMP) and 0x80000007 (the manager refused) pass silently
	}
	return 0;
}

/* the slot made a zero-filled bitmap shaped like the picture file arc:name
 * - its size, pixel mode and base size, read from the header as stored
 * (BmpOp_CreateLikeFile); a missing file, or one that is not a
 * CompressedBG or a raw image, is a script error; a slot that cannot be
 * made passes silently */
static int Opcode_Gfx0_BmpCreateLikeFile(OpenBGI_t* o, Thread_t* t) // 90 C0 (1.547 on): bmp, arc, name →
{
	const char* name = (const char*)PopPtr(o, t);
	const char* arc = (const char*)PopPtr(o, t);
	int bmp = (int)Thread_Pop(t);
	CheckBitmapNo(o, bmp, t);
	switch(BmpOp_CreateLikeFile(o, bmp, arc, name))
	{
		case -1: GFX_ERROR(o, t, MSG_ARC_FILE_NOT_FOUND, arc, name); break;
		case(int)BMP_ERR_NOT_BG: GFX_ERROR(o, t, MSG_ARC_FILE_NOT_BG, arc, name); break;
		default: break;
	}
	return 0;
}

// allocate an empty bitmap of w x h pixels in pixel mode `mode` (0 .. 6) in the slot
static int Opcode_Gfx0_BmpCreate(OpenBGI_t* o, Thread_t* t) // 90 11: bmp, w, h, mode →
{
	int mode = (int)Thread_Pop(t);
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckBitmapNo(o, bmp, t);
	if(!BmpOp_Create(o, bmp, w, h, mode))
		GFX_ERROR(o, t, MSG_BAD_PIXEL_MODE_NO, mode);
	return 0;
}

// release the slot; the result is 1 when it held a bitmap, 0 when it was already free
static int Opcode_Gfx0_BmpFree(OpenBGI_t* o, Thread_t* t) // 90 12: bmp → wasInUse
{
	Thread_Push(t, (uint32_t)BmpOp_Free(o, (int)Thread_Pop(t)));
	return 0;
}

// fill the bitmap with `colour` (0 clears it)
static int Opcode_Gfx0_BmpClear(OpenBGI_t* o, Thread_t* t) // 90 13: bmp, colour →
{
	uint32_t colour = Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckBitmapNo(o, bmp, t);
	if(!BmpOp_Clear(o, bmp, colour))
		ErrBitmapNotRegistered(o, bmp, t);
	return 0;
}

/* create the bitmap from packed pixels in script memory (24 bpp for the
 * RGB32 mode, the native layout otherwise); an ARGB32 picture is
 * un-composited against the "90 0F" key colour.  A failure is not reported. */
static int Opcode_Gfx0_BmpPut(OpenBGI_t* o, Thread_t* t) // 90 14: bmp, w, h, mode, data →
{
	const void* data = PopPtr(o, t);
	int mode = (int)Thread_Pop(t);
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckBitmapNo(o, bmp, t);
	BmpOp_Put(o, bmp, w, h, mode, data);
	return 0;
}

/* copy the bitmap's pixels, packed as "90 14" takes them, into the `size`
 * bytes at `dst`; the second operand is popped but never read.  Errors:
 * 9 no bitmap in the slot, 10 the buffer is too small. */
static int Opcode_Gfx0_BmpGet(OpenBGI_t* o, Thread_t* t) // 90 15: dst, unused, size, bmp →
{
	int bmp = (int)Thread_Pop(t);
	int size = (int)Thread_Pop(t);
	void* unused = PopPtr(o, t);
	void* dst = PopPtr(o, t);
	CheckBitmapNo(o, bmp, t);
	switch(BmpOp_Get(o, dst, (int)(intptr_t)unused, size, bmp))
	{
		case 9: GFX_ERROR(o, t, MSG_BMP_NOT_EXIST, bmp); break;
		case 10: GFX_ERROR(o, t, MSG_BMP_GET_BAD_SIZE, size); break;
		default: break;
	}
	return 0;
}

/* Write the bitmap's descriptor to the 24 bytes at `out` in the script's
 * layout (a 32-bit pixel pointer, always written as 0, then pitch, w, h,
 * mode, bpp); the result is 1 when the slot holds a bitmap.  The host Bmp_t
 * has a wider pointer, so the fields are written one by one. */
static int Opcode_Gfx0_BmpInfo(OpenBGI_t* o, Thread_t* t) // 90 16: out, bmp → ok
{
	int bmp = (int)Thread_Pop(t);
	int32_t* out = (int32_t*)PopPtr(o, t);
	Bmp_t b;
	int ok = BmpOp_GetInfo(o, &b, bmp);
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
static int Opcode_Gfx0_BmpBlit(OpenBGI_t* o, Thread_t* t) // 90 18: dst, x, y, src, effect, level →
{
	uint32_t level = Thread_Pop(t);
	uint32_t effect = Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	CheckBitmapNo(o, dst, t);
	CheckBitmapNo(o, src, t);
	CheckEffectMode(o, effect, t);
	CheckAlpha(o, level, t);
	switch(BmpOp_Blit(o, dst, x, y, src, (int)effect, (int)level))
	{
		case 1: GFX_ERROR(o, t, MSG_BMP_DST_NOT_EXIST, dst); break;
		case 2: GFX_ERROR(o, t, MSG_BMP_SRC_NOT_EXIST, src); break;
		case 3: GFX_ERROR(o, t, MSG_BMP_MODE_MISMATCH, dst, src); break;
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
static int Opcode_Gfx0_BmpBlitMask(OpenBGI_t* o, Thread_t* t) // 90 19: dst, x, y, src, gray, param, level →
{
	uint32_t level = Thread_Pop(t);
	int param = (int)Thread_Pop(t);
	int gray = (int)Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	CheckBitmapNo(o, dst, t);
	CheckBitmapNo(o, src, t);
	if(o->engine.profile->gen >= GEN_1_535)
	{ // the gray is no longer checked; without one the blit is a plain one
		Bmp_t g;
		CheckAlpha(o, level, t);
		if(!BmpOp_GetInfo(o, &g, gray))
		{
			switch(BmpOp_Blit(o, dst, x, y, src, 0x80, (int)level))
			{
				case 1: GFX_ERROR(o, t, MSG_BMP_DST_NOT_EXIST, dst); break;
				case 2: GFX_ERROR(o, t, MSG_BMP_SRC_NOT_EXIST, src); break;
				default: break;
			}
			return 0;
		}
	}
	else
	{
		CheckBitmapNo(o, gray, t);
		CheckAlpha(o, level, t);
	}
	switch(BmpOp_BlitMask(o, dst, x, y, src, gray, param, (int)level))
	{
		case 1: GFX_ERROR(o, t, MSG_BMP_DST_NOT_EXIST, dst); break;
		case 2: GFX_ERROR(o, t, MSG_BMP_SRC_NOT_EXIST, src); break;
		case 3: GFX_ERROR(o, t, MSG_GRAY_NOT_EXIST, gray); break;
		case 4: GFX_ERROR(o, t, MSG_BMP_MODE_MISMATCH, dst, src); break;
		case 5: GFX_ERROR(o, t, MSG_BMP_NOT_GRAY, gray); break;
		case 6: GFX_ERROR(o, t, MSG_GRAY_BAD_SIZE, gray); break;
		default: break;
	}
	return 0;
}

/* draw `src` into `dst` displaced per pixel by the vector map `vec1`:
 * `level` scales the displacement, or, with a second map `vec2` (-1 =
 * none), blends between the two maps; a non-zero `amount` selects bilinear
 * sampling */
static int Opcode_Gfx0_BmpDisplace(OpenBGI_t* o, Thread_t* t) // 90 1A: dst, src, vec1, vec2, level, amount →
{
	int amount = (int)Thread_Pop(t);
	int level = (int)Thread_Pop(t);
	int vec2 = (int)Thread_Pop(t);
	int vec1 = (int)Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	switch(BmpOp_Displace(o, dst, src, vec1, vec2, level, amount))
	{
		case 1: GFX_ERROR(o, t, MSG_STORE_BMP_INVALID, dst); break;
		case 2: GFX_ERROR(o, t, MSG_REF_BMP_INVALID, src); break;
		case 3: GFX_ERROR(o, t, MSG_STORE_REF_MODE_MISMATCH, dst, src); break;
		case 4: GFX_ERROR(o, t, MSG_VEC1_INVALID, vec1); break;
		case 5: GFX_ERROR(o, t, MSG_VEC1_UNSUITABLE, vec1, dst); break;
		case 6: GFX_ERROR(o, t, MSG_VEC2_INVALID, vec2); break;
		case 7: GFX_ERROR(o, t, MSG_VEC2_UNSUITABLE, vec2, dst); break;
		case 8: GFX_ERROR(o, t, MSG_BAD_EFFECT_LEVEL_SHORT, level); break;
		default: break;
	}
	return 0;
}

/* a horizontal box blur of `src` into `dst` (same size and mode) with a
 * window of 2 * level + 1 pixels; `type` 0 pads the edges with black, 1
 * repeats the edge pixel */
static int Opcode_Gfx0_BmpGradient(OpenBGI_t* o, Thread_t* t) // 90 1B: dst, src, type, level →
{
	int level = (int)Thread_Pop(t);
	int type = (int)Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	switch(BmpOp_Gradient(o, dst, src, type, level))
	{
		case 1: GFX_ERROR(o, t, MSG_STORE_BMP_INVALID, dst); break;
		case 2: GFX_ERROR(o, t, MSG_REF_BMP_INVALID, src); break;
		case 3: GFX_ERROR(o, t, MSG_STORE_REF_SIZE_MODE_MISMATCH, dst, src); break;
		case 4: GFX_ERROR(o, t, MSG_BAD_GRADIENT_TYPE, type); break;
		case 5: GFX_ERROR(o, t, MSG_BAD_EFFECT_LEVEL_SHORT, level); break;
		default: break;
	}
	return 0;
}

/* draw the (sx, sy, sw, sh) part of `src` scaled into the (x, y, w, h)
 * rectangle of `dst`; both sizes must be at least 2 x 2 */
static int Opcode_Gfx0_BmpStretch(OpenBGI_t* o, Thread_t* t) // 90 1C: dst, x, y, w, h, src, sx, sy, sw, sh →
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
	switch(BmpOp_Stretch(o, dst, x, y, w, h, src, sx, sy, sw, sh))
	{
		case 1: GFX_ERROR(o, t, MSG_STORE_BMP_INVALID, dst); break;
		case 2: GFX_ERROR(o, t, MSG_REF_BMP_INVALID, src); break;
		case 5: GFX_ERROR(o, t, MSG_BAD_OUTPUT_RANGE, w, h); break;
		case 6: GFX_ERROR(o, t, MSG_BAD_REF_RANGE, sw, sh); break;
		case 8: GFX_ERROR(o, t, MSG_STORE_REF_MODE_MISMATCH, dst, src); break;
		default: break; // 3, 4, 7 pass silently
	}
	return 0;
}

/* draw `src` rotated by `angle` (16.16 degrees) and scaled by `zoom` (16.16,
 * above 0) with its centre at the centre of `dst`, nearest-neighbour sampled */
static int Opcode_Gfx0_BmpRotate(OpenBGI_t* o, Thread_t* t) // 90 1D: dst, src, zoom, angle →
{
	int32_t angle = (int32_t)Thread_Pop(t);
	int32_t zoom = (int32_t)Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	switch(BmpOp_Rotate(o, dst, src, zoom, angle))
	{
		case 1: GFX_ERROR(o, t, MSG_STORE_BMP_INVALID, dst); break;
		case 2: GFX_ERROR(o, t, MSG_REF_BMP_INVALID, src); break;
		case 3: GFX_ERROR(o, t, MSG_BMP_INCOMPATIBLE, dst, src); break;
		case 4: GFX_ERROR(o, t, MSG_BAD_SCALE, zoom); break;
		default: break;
	}
	return 0;
}

/* plain copy of the (sx, sy, w, h) part of `src` to (dx, dy) of `dst`; an
 * empty size is not an error */
static int Opcode_Gfx0_BmpCopyRect(OpenBGI_t* o, Thread_t* t) // 90 1E: dst, dx, dy, src, sx, sy, w, h →
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int sy = (int)Thread_Pop(t);
	int sx = (int)Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int dy = (int)Thread_Pop(t);
	int dx = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	switch(BmpOp_CopyRect(o, dst, dx, dy, src, sx, sy, w, h))
	{
		case 1: GFX_ERROR(o, t, MSG_XFER_DST_INVALID, dst); break;
		case 2: GFX_ERROR(o, t, MSG_XFER_SRC_INVALID, src); break;
		default: break;
	}
	return 0;
}

// slot `dst` becomes a new w x h bitmap holding the part of `src` at (x, y)
static int Opcode_Gfx0_BmpCloneRect(OpenBGI_t* o, Thread_t* t) // 90 1F: dst, src, x, y, w, h →
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	switch(BmpOp_CloneRect(o, dst, src, x, y, w, h))
	{
		case 1: GFX_ERROR(o, t, MSG_OUT_BMP_INVALID, dst); break;
		case 2: GFX_ERROR(o, t, MSG_COPY_SRC_INVALID, src); break;
		case 3: GFX_ERROR(o, t, MSG_BAD_COPY_RANGE, w, h); break;
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
static void TweenResult(OpenBGI_t* o, Thread_t* t, int r, int fps)
{
	if(r == (int)0x80000001)
		GFX_ERROR(o, t, MSG_BAD_FRAME_RATE, fps);
	if(r == -1)
		ScriptError(o, MSG_BAD_OBJ_HANDLE, t);
}

// fade the object's effect level to `level`
static int Opcode_Gfx0_ObjFade(OpenBGI_t* o, Thread_t* t) // 90 20: h, level, duration, fps, skip, prio →
{
	uint32_t prio = Thread_Pop(t);
	int skip = (int)Thread_Pop(t);
	int fps = (int)Thread_Pop(t);
	int duration = (int)Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(o, prio, t);
	CheckAlpha(o, level, t);
	TweenResult(o, t, StartObjTween(o, t, h, 0, 0, 0, 0, (int)level, duration, fps, 0, skip, prio), fps);
	return 2;
}

// "90 20" with a frame-skip limit
static int Opcode_Gfx0_ObjFadeEx(OpenBGI_t* o, Thread_t* t) // 90 22: h, level, duration, fps, maxSkip, skip, prio →
{
	uint32_t prio = Thread_Pop(t);
	int skip = (int)Thread_Pop(t);
	int maxSkip = (int)Thread_Pop(t);
	int fps = (int)Thread_Pop(t);
	int duration = (int)Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(o, prio, t);
	CheckAlpha(o, level, t);
	TweenResult(o, t, StartObjTween(o, t, h, 0, 0, 0, 0, (int)level, duration, fps, maxSkip, skip, prio), fps);
	return 2;
}

// move the object to (x, y) along `curve` while fading its level to `level`
static int Opcode_Gfx0_ObjMove(OpenBGI_t* o, Thread_t* t) // 90 21: h, x, y, curve, level, duration, fps, skip, prio →
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
	CheckPriority(o, prio, t);
	CheckAlpha(o, level, t);
	TweenResult(o, t, StartObjTween(o, t, h, x, y, curve, 1, (int)level, duration, fps, 0, skip, prio), fps);
	return 2;
}

// "90 21" with a frame-skip limit
static int Opcode_Gfx0_ObjMoveEx(OpenBGI_t* o, Thread_t* t) // 90 23: h, x, y, curve, level, duration, fps, maxSkip, skip, prio →
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
	CheckPriority(o, prio, t);
	CheckAlpha(o, level, t);
	TweenResult(o, t, StartObjTween(o, t, h, x, y, curve, 1, (int)level, duration, fps, maxSkip, skip, prio), fps);
	return 2;
}

/* move the object to (x, y) through the via point (viaX, viaY); the x
 * coordinates must advance in one direction (0x80000002 otherwise) */
static int Opcode_Gfx0_ObjMove2(OpenBGI_t* o, Thread_t* t) // 90 24: h, viaX, viaY, x, y, curve, level, duration, fps, maxSkip, skip, prio →
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
	CheckPriority(o, prio, t);
	CheckAlpha(o, level, t);
	r = StartObjMove2(o, t, h, viaX, viaY, x, y, curve, (int)level, duration, fps, maxSkip, skip, prio);
	if(r == (int)0x80000001)
		GFX_ERROR(o, t, MSG_BAD_FRAME_RATE, fps);
	if(r == (int)0x80000002)
		GFX_ERROR(o, t, MSG_MOVE2_BAD_X_VECTOR, viaX, x);
	if(r == -1)
		ScriptError(o, MSG_BAD_OBJ_HANDLE, t);
	return 2;
}

/* validate the progress argument of "90 28" / "90 29": 0 .. 0x100, checked
 * as a level; from 1.69 build 472 on -1 is allowed as well (it leaves the
 * object's progress alone) and the error is "invalid addition level"
 * (無効なアディションレベル) */
static void CheckProgressArg(OpenBGI_t* o, uint32_t v, Thread_t* t)
{
	if(o->engine.profile->gen >= GEN_1_69_472)
	{
		if((int32_t)v < -1 || (int32_t)v > 0x100)
			GFX_ERROR(o, t, MSG_BAD_ADDITION_LEVEL, (int)v);
	}
	else
		CheckAlpha(o, v, t);
}

/* move the object to (x, y) along `curve`, fade its level to `level` along
 * `levelCurve` and bring its progress value to `progress` */
static int Opcode_Gfx0_ObjTweenFull(OpenBGI_t* o, Thread_t* t) // 90 28: h, x, y, curve, level, levelCurve, progress, duration, fps, maxSkip, skip, prio →
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
	CheckPriority(o, prio, t);
	CheckAlpha(o, level, t);
	CheckProgressArg(o, progress, t);
	TweenResult(o, t,
		StartObjTweenFull(o, t, h, x, y, curve, (int)level, levelCurve, (int)progress, duration, fps, maxSkip, skip, prio),
		fps);
	return 2;
}

/* as "90 28", but the object moves along a path of `n` (at least 1) control
 * points in script memory, four 16.16 words each (x, y, z, padding) */
static int Opcode_Gfx0_ObjPath(OpenBGI_t* o, Thread_t* t) // 90 29: h, n, points, curve, level, levelCurve, progress, duration, fps, maxSkip, skip, prio →
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
	const int32_t* points = (const int32_t*)PopPtr(o, t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	int r;
	CheckPriority(o, prio, t);
	CheckAlpha(o, level, t);
	CheckProgressArg(o, progress, t);
	r = StartObjPath(o, t, h, n, points, curve, (int)level, levelCurve, (int)progress, duration, fps, maxSkip, skip, prio);
	if(r == (int)0x80000001)
		GFX_ERROR(o, t, MSG_BAD_FRAME_RATE, fps);
	if(r == (int)0x80000003)
		GFX_ERROR(o, t, MSG_BAD_POINT_COUNT, n);
	if(r == -1)
		ScriptError(o, MSG_BAD_OBJ_HANDLE, t);
	return 2;
}

/* shake the object: `pattern` 0 .. 5 (0, 1, 4 vertical, 2, 3, 5
 * horizontal), `amplitude` in pixels, `frequency` cycles per second,
 * `repeat` cycles, the amplitude decaying by `decay` percent per cycle.
 * An unknown handle is not reported: the turn ends without a wait object. */
static int Opcode_Gfx0_ObjQuake(OpenBGI_t* o, Thread_t* t) // 90 2C: h, pattern, amplitude, frequency, repeat, decay, fps, skip, prio →
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
	switch(StartObjQuake(o, t, h, pattern, amplitude, frequency, repeat, decay, fps, skip, prio))
	{
		case(int)0x80000001: GFX_ERROR(o, t, MSG_BAD_QUAKE_PATTERN, pattern); break;
		case(int)0x80000002: GFX_ERROR(o, t, MSG_BAD_FREQUENCY, frequency); break;
		case(int)0x80000003: GFX_ERROR(o, t, MSG_BAD_REPEAT_COUNT, repeat); break;
		case(int)0x80000004:
			// a rate of at least 1 that is still below the frequency gets the longer explanation
			if((uint32_t)fps >= 1)
				GFX_ERROR(o, t, MSG_BAD_FRAME_RATE_FREQ, fps);
			GFX_ERROR(o, t, MSG_BAD_FRAME_RATE, fps);
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
static int Opcode_Gfx0_ObjShow(OpenBGI_t* o, Thread_t* t) // 90 30: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_ObjShow(o, h, f))
		ScriptError(o, MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

// the object's enabled flag (default 1), a second condition of its visibility besides "90 30"
static int Opcode_Gfx0_ObjSetEnabled(OpenBGI_t* o, Thread_t* t) // 90 31: h, v →
{
	int v = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_ObjSetEnabled(o, h, v))
		ScriptError(o, MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

static int Opcode_Gfx0_ObjSetLevel(OpenBGI_t* o, Thread_t* t) // 90 32: h, level →
{
	uint32_t level = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckAlpha(o, level, t);
	if(!GfxCall_ObjSetLevel(o, h, (int)level))
		ScriptError(o, MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

static int Opcode_Gfx0_ObjSetPos(OpenBGI_t* o, Thread_t* t) // 90 33: h, x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_ObjSetPos(o, h, x, y))
		ScriptError(o, MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

/* the object's opacity, a second multiplier on the effect level (0 .. 0x100,
 * the default) that the children inherit; validated with the level
 * message */
static int Opcode_Gfx0_ObjSetOpacity(OpenBGI_t* o, Thread_t* t) // 90 39 (1.599 on): h, v →
{
	uint32_t v = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckAlpha(o, v, t); // the level / alpha message
	if(!GfxCall_ObjSetOpacity(o, h, (int)v))
		ScriptError(o, MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

/* the object's fade value (0 .. 0x100), a multiplier on the effect level
 * that the children inherit; the object tweens animate it */
static int Opcode_Gfx0_ObjSetFade(OpenBGI_t* o, Thread_t* t) // 90 34: h, v →
{
	uint32_t v = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckAlpha(o, v, t);
	if(!GfxCall_ObjSetFade(o, h, (int)v))
		ScriptError(o, MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

/* the object's progress value (0 .. 0x100, an integer step), the parameter
 * of the sprite modes that reveal or transform over a progress (wipe,
 * ripple, ...); the children inherit it */
static int Opcode_Gfx0_ObjSetProgress(OpenBGI_t* o, Thread_t* t) // 90 35: h, v →
{
	uint32_t v = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckAlpha(o, v, t);
	if(!GfxCall_ObjSetProgress(o, h, (int)v))
		ScriptError(o, MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

// a second draw offset in pixels, added to the "90 37" one and handed to the children as well
static int Opcode_Gfx0_ObjSetOffset2(OpenBGI_t* o, Thread_t* t) // 90 36 (1.492 on): h, dx, dy →
{
	int dy = (int)Thread_Pop(t);
	int dx = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_ObjSetOffset2(o, h, dx, dy))
		ScriptError(o, MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

// the object's draw offset in pixels, added to its position
static int Opcode_Gfx0_ObjSetOffset(OpenBGI_t* o, Thread_t* t) // 90 37: h, dx, dy →
{
	int dy = (int)Thread_Pop(t);
	int dx = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_ObjSetOffset(o, h, dx, dy))
		ScriptError(o, MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

// the draw priority of any object (the per-class Set instructions take it as well)
static int Opcode_Gfx0_ObjSetPriority(OpenBGI_t* o, Thread_t* t) // 90 3A (1.69/472 on): h, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(o, prio, t);
	if(GfxCall_ObjSetPriority(o, h, (int)prio) == 0xff)
		ScriptError(o, MSG_BAD_OBJ_HANDLE, t);
	return 0;
}

/* the object's generic parameter `no` with the values (a, b): the object
 * class decides which numbers it supports and what they mean (the
 * setParam methods of src/gfx).  Errors: 5 the parameter is not supported
 * by that class, 0xfe the values are out of range. */
static int Opcode_Gfx0_ObjSetParam(OpenBGI_t* o, Thread_t* t) // 90 38: h, no, a, b →
{
	int b = (int)Thread_Pop(t);
	int a = (int)Thread_Pop(t);
	int no = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_ObjSetParam(o, h, no, a, b))
	{
		case 5: GFX_ERROR(o, t, MSG_BAD_PARAM_NO, no); break;
		case 0xfe: GFX_ERROR(o, t, MSG_BAD_PARAM_ARGS, no, a, a, b, b); break;
		case 0xff: ScriptError(o, MSG_BAD_OBJ_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* the object's hit-test mask: built from the alpha of bitmap `bmp`; -1
 * removes it (the whole rectangle hits), -2 installs a mask that never
 * hits.  Errors: 1 the object has no mask (a virtual object, group or
 * knob), 2 no such bitmap. */
static int Opcode_Gfx0_ObjSetHitMask(OpenBGI_t* o, Thread_t* t) // 90 3C: h, bmp →
{
	int bmp = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_ObjSetHitMask(o, h, bmp))
	{
		case 1: ScriptError(o, MSG_OBJ_IS_VIRTUAL, t); break;
		case 2: GFX_ERROR(o, t, MSG_BMP_INVALID, bmp); break;
		case 0xff: ScriptError(o, MSG_BAD_OBJ_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// whether the mouse position is over the object (through its hit mask)
static int Opcode_Gfx0_ObjHitTest(OpenBGI_t* o, Thread_t* t) // 90 3D: h → hit
{
	uint32_t h = Thread_Pop(t);
	int hit;
	if(!GfxCall_ObjHitTest(o, &hit, h))
		ScriptError(o, MSG_BAD_OBJ_HANDLE, t);
	Input_NoteProbe(o, PROBE_OBJECT, h); // the script decides what a click on it does (OpenBGI_Targets)
	Thread_Push(t, (uint32_t)hit);
	return 0;
}

/* ask the object to build its picture cache (the buildCache method of its
 * class); 3 when the class does not support it (the default method, which
 * no class of this build overrides), 4 an unusable picture size */
static int Opcode_Gfx0_ObjBuildCache(OpenBGI_t* o, Thread_t* t) // 90 3F: h →
{
	switch(GfxCall_ObjBuildCache(o, Thread_Pop(t)))
	{
		case 3: ScriptError(o, MSG_OBJ_NO_CACHE, t); break;
		case 4: ScriptError(o, MSG_OBJ_BAD_IMAGE_SIZE, t); break;
		case 0xff: ScriptError(o, MSG_BAD_OBJ_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 90 40 .. 90 4D : the background
// -------------------------------------------------------------------------

/* The background is the screen-sized object at the bottom of the picture
 * (handle 0 of the object instructions).  Each of "90 40" .. "90 4A"
 * replaces it by one of the background types of src/include/bgi/gfx/background.h
 * (1 .. 11, in that order) and sets that type up from the bitmaps given;
 * a background bitmap has to be screen sized unless the type says
 * otherwise.  A bitmap number may be the sentinel 0x7000 (black) or 0x7001
 * (white) where noted. */

// type 1: one plain bitmap
static int Opcode_Gfx0_BgBitmap(OpenBGI_t* o, Thread_t* t) // 90 40: bmp →
{
	int bmp = (int)Thread_Pop(t);
	if(!GfxCall_BgSetBitmap(o, bmp))
		GFX_ERROR(o, t, MSG_BG_BMP_UNUSABLE, bmp);
	return 0;
}

// type 2: a cross fade from `bmp` to `second` (a bitmap, black or white) at `level`
static int Opcode_Gfx0_BgFade(OpenBGI_t* o, Thread_t* t) // 90 41: bmp, second, level →
{
	uint32_t level = Thread_Pop(t);
	int second = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckAlpha(o, level, t);
	if(!GfxCall_BgSetFade(o, bmp, second, (int)level))
		GFX_ERROR(o, t, MSG_BG_BMP2_UNUSABLE, bmp, second);
	return 0;
}

/* type 3: a 2 x 2 tiling of four bitmaps viewed from the origin (x, y),
 * which may run from (0, 0) to (W, H) inclusive */
static int Opcode_Gfx0_BgScroll(OpenBGI_t* o, Thread_t* t) // 90 42: b0, b1, b2, b3, x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int b3 = (int)Thread_Pop(t);
	int b2 = (int)Thread_Pop(t);
	int b1 = (int)Thread_Pop(t);
	int b0 = (int)Thread_Pop(t);
	switch(GfxCall_BgSetScroll(o, b0, b1, b2, b3, x, y))
	{
		case 1: GFX_ERROR(o, t, MSG_BG_SCROLL_BAD_POS, x, y); break;
		case 2: GFX_ERROR(o, t, MSG_BG_BMP4_UNUSABLE, b0, b1, b2, b3); break;
		default: break;
	}
	return 0;
}

/* type 4: two positioned bitmaps (`bmp1` of any size, `bmp2` a bitmap or a
 * sentinel) with a wipe between them through the screen-sized gray map
 * `gray` (-1 = a plain alpha blend) at `level`; `param` is the transition
 * curve of "90 19" */
static int Opcode_Gfx0_BgBlend(OpenBGI_t* o, Thread_t* t) // 90 43: x1, y1, bmp1, x2, y2, bmp2, gray, param, level →
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
	CheckAlpha(o, level, t);
	switch(GfxCall_BgSetBlend(o, x1, y1, bmp1, x2, y2, bmp2, gray, param, (int)level))
	{
		case 1: GFX_ERROR(o, t, MSG_BMP_NOT_EXIST, bmp1); break;
		case 2: GFX_ERROR(o, t, MSG_BMP_NOT_EXIST, bmp2); break;
		case 3: GFX_ERROR(o, t, MSG_BMP_NOT_EXIST, gray); break;
		case 4: GFX_ERROR(o, t, MSG_BMP_NOT_GRAY, gray); break;
		case 5: GFX_ERROR(o, t, MSG_GRAY_UNSUITABLE, gray); break;
		default: break;
	}
	return 0;
}

/* type 5: a flip book of `n` (2 .. 32) bitmaps; `level` is the index of
 * the frame shown.  Up to 32 bitmap numbers are copied out of script
 * memory; the error for an unusable one lists every number the script
 * passed (also beyond 32). */
static int Opcode_Gfx0_BgFrames(OpenBGI_t* o, Thread_t* t) // 90 44: n, list, level →
{
	int level = (int)Thread_Pop(t);
	const int32_t* list = (const int32_t*)PopPtr(o, t);
	int n = (int)Thread_Pop(t);
	int32_t local[32];
	int i;
	for(i = 0; i < n && i < 32; i++)
		local[i] = list[i];
	switch(GfxCall_BgSetFrames(o, n, local, level))
	{
		case 1: GFX_ERROR(o, t, MSG_BAD_BMP_COUNT, n); break;
		case 2:
		{
			char msg[0x400], tmp[0x400];
			sprintf(msg, MSG_BG_LIST_HEAD);
			for(i = 0; i < n; i++)
			{
				sprintf(tmp, i < n - 1 ? "%s%d , " : MSG_BG_LIST_TAIL, msg, list[i]);
				strcpy(msg, tmp);
			}
			ScriptError(o, msg, t);
			break;
		}
		default: break;
	}
	return 0;
}

// type 6: `bmp` displaced by the screen-sized vector maps `vec1` and `vec2` (-1 = none), as "90 1A"
static int Opcode_Gfx0_BgDisplace(OpenBGI_t* o, Thread_t* t) // 90 45: bmp, vec1, vec2, level, amount →
{
	int amount = (int)Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	int vec2 = (int)Thread_Pop(t);
	int vec1 = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckAlpha(o, level, t);
	switch(GfxCall_BgSetDisplace(o, bmp, vec1, vec2, (int)level, amount))
	{
		case 1: GFX_ERROR(o, t, MSG_BMP_NOT_EXIST, bmp); break;
		case 2: GFX_ERROR(o, t, MSG_BMP_NOT_SCREEN_SIZE, bmp); break;
		case 3: GFX_ERROR(o, t, MSG_BMP_NOT_EXIST, vec1); break;
		case 4: GFX_ERROR(o, t, MSG_VECMAP_NOT_SCREEN, vec1); break;
		case 5: GFX_ERROR(o, t, MSG_BMP_NOT_EXIST, vec2); break;
		case 6: GFX_ERROR(o, t, MSG_VECMAP_NOT_SCREEN, vec2); break;
		default: break;
	}
	return 0;
}

// type 7: `bmp` through the box blur of "90 1B" (`type` the edge handling, `level` the window)
static int Opcode_Gfx0_BgGradient(OpenBGI_t* o, Thread_t* t) // 90 46: bmp, type, level →
{
	uint32_t level = Thread_Pop(t);
	int type = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckAlpha(o, level, t);
	switch(GfxCall_BgSetGradient(o, bmp, type, (int)level))
	{
		case 1: GFX_ERROR(o, t, MSG_BMP_NOT_EXIST, bmp); break;
		case 2: GFX_ERROR(o, t, MSG_BMP_NOT_SCREEN_SIZE, bmp); break;
		case 3: GFX_ERROR(o, t, MSG_BAD_GRADIENT_TYPE, type); break;
		default: break;
	}
	return 0;
}

/* type 8: `bmp` under the ripples of definition `ripple` ("92 00") read
 * through the screen-sized vector + distance map `map` with `type` rings
 * (at least 1; a 0 is reported with the gradient-type message); `level`
 * drives the ripple */
static int Opcode_Gfx0_BgRipple(OpenBGI_t* o, Thread_t* t) // 90 47: bmp, map, type, ripple, level →
{
	uint32_t level = Thread_Pop(t);
	int ripple = (int)Thread_Pop(t);
	int type = (int)Thread_Pop(t);
	int map = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckAlpha(o, level, t);
	switch(GfxCall_BgSetRipple(o, bmp, map, type, ripple, (int)level))
	{
		case 1: GFX_ERROR(o, t, MSG_BMP_NOT_EXIST, bmp); break;
		case 2: GFX_ERROR(o, t, MSG_BMP_NOT_SCREEN_SIZE, bmp); break;
		case 3: GFX_ERROR(o, t, MSG_VDMAP_NOT_EXIST, map); break;
		case 4: GFX_ERROR(o, t, MSG_VDMAP_UNSUITABLE, map); break;
		case 5: GFX_ERROR(o, t, MSG_BAD_GRADIENT_TYPE, type); break;
		case 6: GFX_ERROR(o, t, MSG_RIPPLE_NOT_REGISTERED, ripple); break;
		case 7: GFX_ERROR(o, t, MSG_RIPPLE_MISMATCH, ripple, map); break;
		default: break;
	}
	return 0;
}

/* type 9: the w x h view of `bmp` (any size; w, h at least 2) at (x, y)
 * stretched over the screen; the level moves the view towards a target
 * rectangle set with parameter 0x102 */
static int Opcode_Gfx0_BgView(OpenBGI_t* o, Thread_t* t) // 90 48: bmp, x, y, w, h →
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	switch(GfxCall_BgSetView(o, bmp, x, y, w, h))
	{
		case 1: GFX_ERROR(o, t, MSG_BMP_NOT_EXIST, bmp); break;
		case 2: GFX_ERROR(o, t, MSG_BAD_REF_RANGE2, w, h); break;
		default: break;
	}
	return 0;
}

/* type 10: `bmp` (any size) zoomed by `scale` (16.16, not 0) about the
 * screen centre; `option` is the rotation angle (16.16 degrees) */
static int Opcode_Gfx0_BgZoom(OpenBGI_t* o, Thread_t* t) // 90 49: bmp, scale, option →
{
	int option = (int)Thread_Pop(t);
	int scale = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	switch(GfxCall_BgSetZoom(o, bmp, scale, option))
	{
		case 1: GFX_ERROR(o, t, MSG_BMP_NOT_EXIST, bmp); break;
		case 2: GFX_ERROR(o, t, MSG_BAD_SCALE, scale); break;
		default: break;
	}
	return 0;
}

/* type 11: the mosaic transition from `front` to `back` (a bitmap, black or
 * white) at `level`; `style` must be 0, `link` 0 or 1 (1 also mosaics the
 * back bitmap, by 0x100 - level).  The link is on top of the stack, the
 * level below it (the manager's 0x428bb0 takes the fifth argument as the
 * link and the fourth as the level). */
static int Opcode_Gfx0_BgFlip(OpenBGI_t* o, Thread_t* t) // 90 4A: front, back, style, level, link →
{
	int link = (int)Thread_Pop(t);
	int level = (int)Thread_Pop(t);
	int style = (int)Thread_Pop(t);
	int back = (int)Thread_Pop(t);
	int front = (int)Thread_Pop(t);
	switch(GfxCall_BgSetFlip(o, front, back, style, link, level))
	{
		case 1: GFX_ERROR(o, t, MSG_BG_FRONT_UNUSABLE, front); break;
		case 2: GFX_ERROR(o, t, MSG_BG_BACK_UNUSABLE, back); break;
		case 3: GFX_ERROR(o, t, MSG_BAD_STYLE, style); break;
		case 4: GFX_ERROR(o, t, MSG_BAD_LINK_STATE, link); break;
		default: break;
	}
	return 0;
}

// the background's visibility and draw mode (0 paints black, anything else the type's picture)
static int Opcode_Gfx0_BgShow(OpenBGI_t* o, Thread_t* t) // 90 4C: visible, mode →
{
	int mode = (int)Thread_Pop(t);
	int visible = (int)Thread_Pop(t);
	GfxCall_BgShow(o, visible, mode);
	return 0;
}

// the type number (1 .. 12) of the current background
static int Opcode_Gfx0_BgTypeId(OpenBGI_t* o, Thread_t* t) // 90 4D: → type
{
	Thread_Push(t, (uint32_t)GfxCall_BgTypeId(o));
	return 0;
}

// -------------------------------------------------------------------------
// 90 50 .. 90 5C : sprites
// -------------------------------------------------------------------------

static int Opcode_Gfx0_SpriteCreate(OpenBGI_t* o, Thread_t* t) // 90 50: → h
{
	uint32_t h = GfxCall_SpriteCreate(o);
	if(!h)
		ScriptError(o, MSG_NO_MORE_SPRITES, t);
	Thread_Push(t, h);
	return 0;
}

/* delete the sprite, after removing it from the click targets; refused
 * while a panel procedure is attached to it or it has an owner (a group it
 * was added to with "90 E8", a parent it was attached to with "91 3E", a
 * knob it is the target of) */
static int Opcode_Gfx0_SpriteDelete(OpenBGI_t* o, Thread_t* t) // 90 51: h →
{
	uint32_t h = Thread_Pop(t);
	Target_Remove(o, h);
	if(Gfx_ObjHasProc(o, h))
		ScriptError(o, MSG_SPRITE_HAS_PROC, t);
	if(GfxCall_ObjHasOwner(o, h))
		ScriptError(o, MSG_SPRITE_HAS_OWNER, t);
	if(!GfxCall_SpriteDelete(o, h))
		ScriptError(o, MSG_BAD_SPRITE_HANDLE, t);
	return 0;
}

/* mark the (x, y, w, hgt) rectangle of the sprite (in sprite coordinates)
 * for redraw, after the script changed the bitmap's pixels; 0xa when the
 * sprite's mode does not support it */
static int Opcode_Gfx0_SpriteUpdateRect(OpenBGI_t* o, Thread_t* t) // 90 53: h, x, y, w, hgt →
{
	int hgt = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_SpriteUpdateRect(o, h, x, y, w, hgt))
	{
		case 0xa: ScriptError(o, MSG_BAD_UPDATE_RECT, t); break;
		case 0xff: ScriptError(o, MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

static int Opcode_Gfx0_SpriteShow(OpenBGI_t* o, Thread_t* t) // 90 54: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_SpriteShow(o, h, f))
		ScriptError(o, MSG_BAD_SPRITE_HANDLE, t);
	return 0;
}

/* a screen-sized gray bitmap that masks the sprite in screen space (-1 =
 * none); 2 when it is not one */
static int Opcode_Gfx0_SpriteSetMask(OpenBGI_t* o, Thread_t* t) // 90 55: h, bmp →
{
	int bmp = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_SpriteSetMask(o, h, bmp))
	{
		case 1: GFX_ERROR(o, t, MSG_BAD_BITMAP, bmp); break;
		case 2: GFX_ERROR(o, t, MSG_NOT_SCREEN_GRAY, bmp); break;
		case 0xff: ScriptError(o, MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* The "set" instructions below select the sprite's mode (src/include/bgi/gfx/sprite.h)
 * and its sources together with its position, effect, level and priority;
 * a bitmap the sprite refuses is error 1. */

// mode 0: one plain bitmap
static int Opcode_Gfx0_SpriteSet(OpenBGI_t* o, Thread_t* t) // 90 56: h, x, y, bmp, effect, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t effect = Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(o, prio, t);
	CheckAlpha(o, level, t);
	CheckEffectMode(o, effect, t);
	CheckBitmapNo(o, bmp, t);
	switch(GfxCall_SpriteSet(o, h, x, y, bmp, (int)effect, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(o, t, MSG_BMP_INVALID, bmp); break;
		case 0xff: ScriptError(o, MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// swap the bitmap of a plain, transform or projected sprite, keeping everything else
static int Opcode_Gfx0_SpriteChangeBitmap(OpenBGI_t* o, Thread_t* t) // 90 57: h, bmp →
{
	int bmp = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckBitmapNo(o, bmp, t);
	switch(GfxCall_SpriteChangeBitmap(o, h, bmp))
	{
		case 1: GFX_ERROR(o, t, MSG_BMP_INVALID, bmp); break;
		case 0xff: ScriptError(o, MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* mode 1: two bitmaps of equal size cross-faded by `mixRatio` (0 .. 0x100);
 * `levelRoute` says what a later level change drives (-1 / 0 the level, 1
 * the mix ratio, 2 both).  Case 2 reports differing sizes; Gfx_SpriteSetBlend
 * of this build returns 9 for that, which passes silently. */
static int Opcode_Gfx0_SpriteSetBlend(OpenBGI_t* o, Thread_t* t) // 90 58: h, x, y, bmp1, bmp2, mixRatio, level, prio, levelRoute →
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
	CheckBitmapNo(o, bmp1, t);
	CheckBitmapNo(o, bmp2, t);
	CheckMixRatio(o, mixRatio, t);
	CheckTransparency(o, level, t);
	CheckPriority(o, prio, t);
	switch(GfxCall_SpriteSetBlend(o, h, x, y, bmp1, bmp2, (int)mixRatio, (int)level, (int)prio, levelRoute))
	{
		case 1: GFX_ERROR(o, t, MSG_BMP2_INVALID, bmp1, bmp2); break;
		case 2: GFX_ERROR(o, t, MSG_BMP2_SIZE_MISMATCH, bmp1, bmp2); break;
		case 0xff: ScriptError(o, MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* mode 2: one bitmap rotated by `angle` (16.16 degrees) and scaled by
 * (sx, sy) (16.16) about the origin (ox, oy) inside the bitmap, `smooth`
 * selecting bilinear sampling; 8 when the transformed picture is smaller
 * than 2 pixels on a side */
static int Opcode_Gfx0_SpriteSetTransform(OpenBGI_t* o, Thread_t* t) // 90 59: h, x, y, bmp, ox, oy, angle, sx, sy, smooth, effect, level, prio →
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
	CheckPriority(o, prio, t);
	CheckAlpha(o, level, t);
	CheckEffectMode(o, effect, t);
	switch(GfxCall_SpriteSetTransform(o, h, x, y, bmp, ox, oy, angle, sx, sy, smooth, (int)effect, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(o, t, MSG_BAD_BITMAP, bmp); break;
		case 8: GFX_ERROR(o, t, MSG_BAD_SCALE2, sx, sy); break;
		case 0xff: ScriptError(o, MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* The scale-only ancestor of "90 59" in 1.58.  That build's sprite had no
 * origin or rotation; `clamp` (not a smoothing flag) kept the scaled size
 * from exceeding the bitmap's, which is the same as capping the factors at
 * 1.0.  Result codes as "90 59"; the scale message of that build read
 * "無効な伸縮倍率 [ %d , %d ]" (invalid scale factors) like the later one. */
static int Opcode_Gfx0_SpriteSetScale_158(OpenBGI_t* o, Thread_t* t) // 90 5A (1.58): h, x, y, bmp, sx, sy, clamp, effect, level, prio →
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
	CheckPriority(o, prio, t);
	CheckAlpha(o, level, t);
	CheckEffectMode(o, effect, t);
	if(clamp)
	{
		sx = sx > 0x10000 ? 0x10000 : sx;
		sy = sy > 0x10000 ? 0x10000 : sy;
	}
	switch(GfxCall_SpriteSetTransform(o, h, x, y, bmp, 0, 0, 0, sx, sy, 0, (int)effect, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(o, t, MSG_BAD_BITMAP, bmp); break;
		case 8: GFX_ERROR(o, t, MSG_BAD_SCALE2, sx, sy); break;
		case 0xff: ScriptError(o, MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* re-scale the sprite of 1.58 in place: the bitmap and the factors change,
 * position, effect, level and priority stay (`clamp` as in "90 5A" of that
 * build) */
static int Opcode_Gfx0_SpriteRescale_158(OpenBGI_t* o, Thread_t* t) // 90 5B (1.58): h, bmp, sx, sy, clamp →
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
	switch(Gfx_SpriteRescale(o, o->gfx.gfx, h, bmp, sx, sy, 0))
	{
		case 1: GFX_ERROR(o, t, MSG_BAD_BITMAP, bmp); break;
		case 8: GFX_ERROR(o, t, MSG_BAD_SCALE2, sx, sy); break;
		case 0xff: ScriptError(o, MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* mode 3: `bmp` revealed through the grayscale pattern `gray` (copied into
 * the sprite) by `progress` (0 .. 0x100, later driven by "90 35" and the
 * tweens); `wipeMode` scales the gray value by 1 << wipeMode, the width of
 * the transition.  2: the pattern is not grayscale. */
static int Opcode_Gfx0_SpriteSetWipe(OpenBGI_t* o, Thread_t* t) // 90 5A: h, x, y, bmp, gray, wipeMode, progress, effect, level, prio →
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
	CheckPriority(o, prio, t);
	CheckAlpha(o, level, t);
	CheckEffectMode(o, effect, t);
	CheckAlpha(o, progress, t);
	switch(GfxCall_SpriteSetWipe(o, h, x, y, bmp, gray, wipeMode, (int)progress, (int)effect, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(o, t, MSG_BMP2_INVALID, bmp, gray); break;
		case 2: GFX_ERROR(o, t, MSG_FILTER_BMP_UNSUITABLE, gray); break;
		case 0xff: ScriptError(o, MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* mode 4: `bmp` distorted through the vector + distance map `map` (of the
 * bitmap's size) by the ripple definition `rippleNo` ("92 00") with `rings`
 * rings.  `level` is the ripple amplitude; `fade` (0 .. 0x100) is what the
 * sprite blits with as its level.  Errors: 2 not a sprite bitmap, 3 / 4 the
 * map, 5 the ring count, 6 / 7 the ripple definition. */
static int Opcode_Gfx0_SpriteSetRipple(OpenBGI_t* o, Thread_t* t) // 90 5B: h, x, y, bmp, map, rings, rippleNo, level, fade, prio →
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
	CheckBitmapNo(o, bmp, t);
	CheckBitmapNo(o, map, t);
	CheckTransparency(o, fade, t);
	CheckPriority(o, prio, t);
	switch(GfxCall_SpriteSetRipple(o, h, x, y, bmp, map, rings, rippleNo, level, (int)fade, (int)prio))
	{
		case 1: GFX_ERROR(o, t, MSG_BAD_BITMAP, bmp); break;
		case 2: GFX_ERROR(o, t, MSG_NOT_SPRITE_BMP, bmp); break;
		case 3: GFX_ERROR(o, t, MSG_VDMAP_INVALID, map); break;
		case 4: GFX_ERROR(o, t, MSG_VDMAP_SIZE_MISMATCH, map, bmp); break;
		case 5: GFX_ERROR(o, t, MSG_BAD_MAX_DISTANCE, rings); break;
		case 6: GFX_ERROR(o, t, MSG_RIPPLE_NOT_REGISTERED, rippleNo); break;
		case 7: GFX_ERROR(o, t, MSG_RIPPLE_MISMATCH, rippleNo, map); break;
		case 0xff: ScriptError(o, MSG_BAD_SPRITE_HANDLE, t); break;
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
static int Opcode_Gfx0_SpriteSetProjected(OpenBGI_t* o, Thread_t* t) // 90 5C: h, fx, fy, fz, bmp1, bmp2, mixRatio, levelRoute, ox, oy, angle, projDist, projPos, smooth, effect, level, prio →
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
	CheckBitmapNo(o, bmp1, t);
	CheckMixRatio(o, mixRatio, t);
	CheckEffectMode(o, effect, t);
	CheckAlpha(o, level, t);
	CheckPriority(o, prio, t);
	switch(GfxCall_SpriteSetProjected(o, h, fx, fy, fz, bmp1, bmp2, (int)mixRatio, levelRoute, ox, oy, angle, projDist,
		projPos, smooth, (int)effect, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(o, t, MSG_BMP2_INVALID, bmp1, bmp2); break;
		case 2: GFX_ERROR(o, t, MSG_BMP2_SIZE_MODE_MISMATCH, bmp1, bmp2); break;
		case 8: GFX_ERROR(o, t, MSG_BAD_PROJ_SCALE, projDist); break;
		case 0xff: ScriptError(o, MSG_BAD_SPRITE_HANDLE, t); break;
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
 * are ignored, so such a sprite shows unrotated (docs/dev/versions.md). */
static int Opcode_Gfx0_SpriteSetProjected3D(OpenBGI_t* o, Thread_t* t) // 90 5D: h, fx, fy, fz, bmp1, bmp2, mixRatio, levelRoute, ox, oy, angle, rotX, rotY, rotZ, projDist, projPos, smooth, effect, level, prio →
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
	CheckBitmapNo(o, bmp1, t);
	CheckMixRatio(o, mixRatio, t);
	CheckEffectMode(o, effect, t);
	CheckAlpha(o, level, t);
	CheckPriority(o, prio, t);
	switch(GfxCall_SpriteSetProjected(o, h, fx, fy, fz, bmp1, bmp2, (int)mixRatio, levelRoute, ox, oy, angle, projDist,
		projPos, smooth, (int)effect, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(o, t, MSG_BMP2_INVALID, bmp1, bmp2); break;
		case 2: GFX_ERROR(o, t, MSG_BMP2_SIZE_MODE_MISMATCH, bmp1, bmp2); break;
		case 8: GFX_ERROR(o, t, MSG_BAD_PROJ_SCALE, projDist); break;
		case 0xff: ScriptError(o, MSG_BAD_SPRITE_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 90 60 .. 90 66 : filters
// -------------------------------------------------------------------------

static int Opcode_Gfx0_FilterCreate(OpenBGI_t* o, Thread_t* t) // 90 60: → h
{
	uint32_t h = GfxCall_FilterCreate(o);
	if(!h)
		ScriptError(o, MSG_NO_MORE_FILTERS, t);
	Thread_Push(t, h);
	return 0;
}

static int Opcode_Gfx0_FilterDelete(OpenBGI_t* o, Thread_t* t) // 90 61: h →
{
	if(!GfxCall_FilterDelete(o, Thread_Pop(t)))
		ScriptError(o, MSG_BAD_FILTER_HANDLE, t);
	return 0;
}

static int Opcode_Gfx0_FilterShow(OpenBGI_t* o, Thread_t* t) // 90 64: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_FilterShow(o, h, f))
		ScriptError(o, MSG_BAD_FILTER_HANDLE, t);
	return 0;
}

/* A filter has no picture of its own: it applies a colour operation to
 * everything drawn below its priority.  Kind 0 blends the picture towards
 * `colour` by `level` (a black colour dims it); kinds 1 .. 3 are the
 * variants of src/gfx/blit_filter.c (add the colour, monochrome tint, blend
 * towards the picture XOR the colour). */

// a uniform kind-0 filter of `colour` at `level`, no gray bitmap
static int Opcode_Gfx0_FilterSet(OpenBGI_t* o, Thread_t* t) // 90 65: h, colour, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t colour = Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(o, prio, t);
	CheckAlpha(o, level, t);
	if(GfxCall_FilterSet(o, h, 0, colour, -1, 0, (int)level, (int)prio) != 0)
		ScriptError(o, MSG_BAD_FILTER_HANDLE, t);
	return 0;
}

/* a filter of `kind` 0 .. 3, modulated per pixel by the screen-sized gray
 * bitmap `bmp` (-1 = uniform; only kind 0 uses it, with `param` the
 * transition curve of "90 19").  Errors: 1 no such bitmap, 2 not
 * grayscale, 3 not screen sized. */
static int Opcode_Gfx0_FilterSetBitmap(OpenBGI_t* o, Thread_t* t) // 90 66: h, kind, colour, bmp, param, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	int param = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	uint32_t colour = Thread_Pop(t);
	int kind = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(o, prio, t);
	CheckAlpha(o, level, t);
	switch(GfxCall_FilterSet(o, h, kind, colour, bmp, param, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(o, t, MSG_BAD_BITMAP_NO, bmp); break;
		case 2: GFX_ERROR(o, t, MSG_BMP_NOT_GRAY, bmp); break;
		case 3: GFX_ERROR(o, t, MSG_BMP_NOT_SCREEN_SIZED, bmp); break;
		case 0xff: ScriptError(o, MSG_BAD_FILTER_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 90 70 .. 90 7A : tile maps
// -------------------------------------------------------------------------

static int Opcode_Gfx0_MapCreate(OpenBGI_t* o, Thread_t* t) // 90 70: → h
{
	uint32_t h = GfxCall_MapCreate(o);
	if(!h)
		ScriptError(o, MSG_NO_MORE_MAPS, t);
	Thread_Push(t, h);
	return 0;
}

static int Opcode_Gfx0_MapDelete(OpenBGI_t* o, Thread_t* t) // 90 71: h →
{
	if(!GfxCall_MapDelete(o, Thread_Pop(t)))
		ScriptError(o, MSG_BAD_MAP_HANDLE, t);
	return 0;
}

static int Opcode_Gfx0_MapShow(OpenBGI_t* o, Thread_t* t) // 90 74: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_MapShow(o, h, f))
		ScriptError(o, MSG_BAD_MAP_HANDLE, t);
	return 0;
}

/* A map is a scrolling tile map: a chipset bitmap sliced into equal tiles
 * ("90 76" sets the sizes, "90 75" the chipset), a terrain of 16-bit tile
 * numbers (0xFFFF = empty, "90 78") and a view position in it ("90 79"). */

/* place the map at (x, y) with `bmp` as its chipset; 1 when the bitmap is
 * unusable (missing, too small for one tile, or "90 76" not done yet) */
static int Opcode_Gfx0_MapSet(OpenBGI_t* o, Thread_t* t) // 90 75: h, x, y, bmp, effect, level, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t effect = Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(o, prio, t);
	CheckAlpha(o, level, t);
	CheckEffectMode(o, effect, t);
	CheckBitmapNo(o, bmp, t);
	switch(GfxCall_MapSet(o, h, x, y, bmp, (int)effect, (int)level, (int)prio))
	{
		case 1: GFX_ERROR(o, t, MSG_MAP_BMP_MISMATCH, bmp); break;
		case 0xff: ScriptError(o, MSG_BAD_MAP_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* the view size in tiles and the tile size in pixels; a tile is at most
 * 0x100 x 0x100, the view at most 0x400 x 0x300 pixels (2: the view, 3:
 * the tile size) */
static int Opcode_Gfx0_MapSetSize(OpenBGI_t* o, Thread_t* t) // 90 76: h, cols, rows, chipW, chipH →
{
	int chipH = (int)Thread_Pop(t);
	int chipW = (int)Thread_Pop(t);
	int rows = (int)Thread_Pop(t);
	int cols = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_MapSetSize(o, h, cols, rows, chipW, chipH))
	{
		case 2: GFX_ERROR(o, t, MSG_BAD_VIEW_SIZE, cols, rows); break;
		case 3: GFX_ERROR(o, t, MSG_BAD_CHIP_SIZE, chipW, chipH); break;
		case 0xff: ScriptError(o, MSG_BAD_MAP_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// copy a w x hgt terrain of 16-bit tile numbers out of script memory (4: an empty size)
static int Opcode_Gfx0_MapSetTerrain(OpenBGI_t* o, Thread_t* t) // 90 78: h, w, hgt, data →
{
	const uint16_t* data = (const uint16_t*)PopPtr(o, t);
	int hgt = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_MapSetTerrain(o, h, w, hgt, data))
	{
		case 4: GFX_ERROR(o, t, MSG_BAD_TERRAIN_SIZE, w, hgt); break;
		case 0xff: ScriptError(o, MSG_BAD_MAP_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* scroll the view: terrain cell (col, row) at the top-left with the pixel
 * offset (offX, offY) inside it; with `wrap` the terrain repeats past its
 * edges, otherwise the cells beyond stay empty.  5: the position is out of
 * range, or the map has no size or terrain yet. */
static int Opcode_Gfx0_MapSetView(OpenBGI_t* o, Thread_t* t) // 90 79: h, col, row, offX, offY, wrap →
{
	int wrap = (int)Thread_Pop(t);
	int offY = (int)Thread_Pop(t);
	int offX = (int)Thread_Pop(t);
	int row = (int)Thread_Pop(t);
	int col = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_MapSetView(o, h, col, row, offX, offY, wrap))
	{
		case 5: GFX_ERROR(o, t, MSG_BAD_MAP_VIEW_POS, col, row, offX, offY); break;
		case 0xff: ScriptError(o, MSG_BAD_MAP_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* redraw every visible cell that shows tile number `tile`, after the script
 * repainted that tile inside the chipset; 6 when the map has no size yet */
static int Opcode_Gfx0_MapInvalidateChip(OpenBGI_t* o, Thread_t* t) // 90 7A: h, tile →
{
	int tile = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_MapInvalidateChip(o, h, tile))
	{
		case 6: ScriptError(o, MSG_OBJ_NOT_INITIALISED, t); break;
		case 0xff: ScriptError(o, MSG_BAD_MAP_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 90 80 .. 90 89 : message windows
// -------------------------------------------------------------------------

// create a message window of w x h pixels (16 slots); 2 when the size is rejected
static int Opcode_Gfx0_WindowCreate(OpenBGI_t* o, Thread_t* t) // 90 80: w, h → handle
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	uint32_t handle;
	switch(GfxCall_WindowCreate(o, &handle, w, h))
	{
		case 1: ScriptError(o, MSG_NO_MORE_WINDOWS, t); break;
		case 2: GFX_ERROR(o, t, MSG_BAD_WINDOW_SIZE, w, h); break;
		default: break;
	}
	Thread_Push(t, handle);
	return 0;
}

/* delete the window; refused while a panel procedure is attached to it or
 * it has an owner (as "90 51") */
static int Opcode_Gfx0_WindowDelete(OpenBGI_t* o, Thread_t* t) // 90 81: h →
{
	uint32_t h = Thread_Pop(t);
	if(Gfx_ObjHasProc(o, h))
		ScriptError(o, MSG_WINDOW_HAS_PROC, t);
	if(GfxCall_ObjHasOwner(o, h))
		ScriptError(o, MSG_WINDOW_HAS_OWNER, t);
	if(!GfxCall_WindowDelete(o, h))
		ScriptError(o, MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

/* create bitmap `bmp` of the window's size and copy the window's composite
 * picture into it; a bitmap the manager refuses is not reported */
static int Opcode_Gfx0_WindowCapture(OpenBGI_t* o, Thread_t* t) // 90 83: bmp, h →
{
	uint32_t h = Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	CheckBitmapNo(o, bmp, t);
	if(GfxCall_WindowCopyToBitmap(o, bmp, h) == -1)
		ScriptError(o, MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

static int Opcode_Gfx0_WindowShow(OpenBGI_t* o, Thread_t* t) // 90 84: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_WindowShow(o, h, f))
		ScriptError(o, MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

// position, effect, level and priority of the window; `level2` is validated but not used
static int Opcode_Gfx0_WindowSet(OpenBGI_t* o, Thread_t* t) // 90 85: h, x, y, effect, level, level2, prio →
{
	uint32_t prio = Thread_Pop(t);
	uint32_t level2 = Thread_Pop(t);
	uint32_t level = Thread_Pop(t);
	uint32_t effect = Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckPriority(o, prio, t);
	CheckAlpha(o, level2, t);
	CheckAlpha(o, level, t);
	CheckEffectMode(o, effect, t);
	if(!GfxCall_WindowSet(o, h, x, y, (int)effect, (int)level, (int)level2, (int)prio))
		ScriptError(o, MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

/* the window's frame layer: bitmap `back` is copied into it (-1 clears and
 * hides the frame); `decoration` and `frame` are popped but not used by
 * this build's window.  Errors: 1 the window has no surfaces, 2 no such
 * bitmap (reported with all three numbers), 3 not a frame bitmap. */
static int Opcode_Gfx0_WindowSetFrame(OpenBGI_t* o, Thread_t* t) // 90 86: h, decoration, frame, back →
{
	int back = (int)Thread_Pop(t);
	int frame = (int)Thread_Pop(t);
	int decoration = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_WindowSetFrame(o, h, decoration, frame, back))
	{
		case 1: ScriptError(o, MSG_WINDOW_NO_RESOURCES, t); break;
		case 2: GFX_ERROR(o, t, MSG_WINDOW_BMPS_UNREGISTERED, decoration, frame, back); break;
		case 3: GFX_ERROR(o, t, MSG_NOT_FRAME_BMP, frame); break;
		case -1: ScriptError(o, MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* whether the item layers (icons, panel buttons) are "punched" out of the
 * text layer when the window is composed.  Shared with the 1.58 / 1.64
 * tables, where the instruction was "90 8B" (declared in bgi/vm.h). */
int Opcode_Gfx0_WindowSetPunch(OpenBGI_t* o, Thread_t* t) // 90 87: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_WindowSetPunch(o, h, f))
		ScriptError(o, MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

/* the order the window's frame, text and sub-sprite layers are composed
 * in: 0 frame, text, subs; 1 frame, subs, text; 2 text, frame, subs; 3
 * text, subs, frame; 4 subs, frame, text; 5 subs, text, frame */
static int Opcode_Gfx0_WindowSetLayerOrder(OpenBGI_t* o, Thread_t* t) // 90 82 (1.492 on): h, order →
{
	int order = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_WindowSetLayerOrder(o, h, order))
	{
		case 0xff: ScriptError(o, MSG_BAD_WINDOW_HANDLE, t); // does not return, so no break is needed
		case 0x14: GFX_ERROR(o, t, MSG_BAD_DRAW_ORDER, order);
		default: return 0;
	}
}

// the rectangle of the window that text may be drawn in, in window coordinates
static int Opcode_Gfx0_WindowSetTextArea(OpenBGI_t* o, Thread_t* t) // 90 88: h, x, y, w, hgt →
{
	int hgt = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_WindowSetTextArea(o, h, x, y, w, hgt))
	{
		case 1: GFX_ERROR(o, t, MSG_BAD_WINDOW_AREA, x, y, w, hgt); break;
		case -1: ScriptError(o, MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// write the text area as a rectangle (l, t, r, b inclusive) to `out`; the result is always 1
static int Opcode_Gfx0_WindowGetTextArea(OpenBGI_t* o, Thread_t* t) // 90 89: out, h → ok
{
	uint32_t h = Thread_Pop(t);
	Rect_t* out = (Rect_t*)PopPtr(o, t);
	int r = GfxCall_WindowGetTextArea(o, out, h);
	if(r == -1)
		ScriptError(o, MSG_BAD_WINDOW_HANDLE, t);
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
static int Opcode_Gfx0_TextOut(OpenBGI_t* o, Thread_t* t) // 90 90: h, str, param, instant, waitAtEnd, allowSkip →
{
	int allowSkip = (int)Thread_Pop(t);
	int waitAtEnd = (int)Thread_Pop(t);
	int instant = (int)Thread_Pop(t);
	uint32_t param = Thread_Pop(t);
	const char* str = (const char*)PopPtr(o, t);
	uint32_t h = Thread_Pop(t);
	int r = StartTextOutW(o, t, h, str, param, 0, instant, waitAtEnd, allowSkip, 1, 0, 0, -1);
	if(r == (int)0x80000001)
		ScriptError(o, MSG_WINDOW_NO_FONT, t);
	if(r == -1)
		ScriptError(o, MSG_BAD_WINDOW_HANDLE, t);
	return 2;
}

/* The remaining instructions of this section set the engine-wide pacing
 * and style parameters of the text output (o->text, through the Text_Set*
 * functions of src/include/bgi/gfx/text.h); they take effect with the next
 * output. */

/* which input layer the text output listens on: `mode` 0 the standard
 * layer 2, 1 the fixed layer `layerNo` (a priority), 2 the window's
 * priority */
static int Opcode_Gfx0_TextSetMode(OpenBGI_t* o, Thread_t* t) // 90 91: mode, layerNo →
{
	int layerNo = (int)Thread_Pop(t);
	int mode = (int)Thread_Pop(t);
	switch(Text_SetLayerMode(o, mode, layerNo))
	{
		case(int)0x80000004: GFX_ERROR(o, t, MSG_BAD_MODE, mode); break;
		case(int)0x80000005: GFX_ERROR(o, t, MSG_BAD_PRIORITY, layerNo); break;
		default: break;
	}
	return 0;
}

// the delay between two characters of the paced output
static int Opcode_Gfx0_TextSetCharInterval(OpenBGI_t* o, Thread_t* t) // 90 94: ms →
{
	Text_SetCharInterval(o, (int)Thread_Pop(t));
	return 0;
}

// a line scroll of the text layer takes `steps` steps (1 .. 256) of `ms` each
static int Opcode_Gfx0_TextSetScroll(OpenBGI_t* o, Thread_t* t) // 90 95: steps, ms →
{
	int ms = (int)Thread_Pop(t);
	int steps = (int)Thread_Pop(t);
	CheckDivCount(o, steps, t);
	Text_SetScroll(o, steps, ms);
	return 0;
}

// a fade of the text layer takes `steps` steps (1 .. 256) of `ms` each
static int Opcode_Gfx0_TextSetFade(OpenBGI_t* o, Thread_t* t) // 90 96: steps, ms →
{
	int ms = (int)Thread_Pop(t);
	int steps = (int)Thread_Pop(t);
	CheckDivCount(o, steps, t);
	Text_SetFade(o, steps, ms);
	return 0;
}

// auto-advance: with `on`, a text output that waits at its end finishes by itself after `ms`
static int Opcode_Gfx0_TextSetAutoAdvance(OpenBGI_t* o, Thread_t* t) // 90 97: on, ms →
{
	int ms = (int)Thread_Pop(t);
	int on = (int)Thread_Pop(t);
	Text_SetAutoAdvance(o, on, ms);
	return 0;
}

/* the frames of the "waiting" cursor animation shown at the end of a text:
 * `n` bitmap numbers in script memory (-1 = an empty frame), copied into
 * private screen-format bitmaps; a single entry means no cursor.  The error
 * names the first bitmap that cannot be used. */
static int Opcode_Gfx0_TextSetBitmaps(OpenBGI_t* o, Thread_t* t) // 90 98: n, list →
{
	const int* list = (const int*)PopPtr(o, t);
	int n = (int)Thread_Pop(t);
	int bad;
	if(!Text_SetItemBitmaps(o, n, list, &bad))
		GFX_ERROR(o, t, MSG_BMP_SCREEN_INCOMPATIBLE, bad);
	return 0;
}

// the delay between two frames of the cursor animation
static int Opcode_Gfx0_TextSetCursorInterval(OpenBGI_t* o, Thread_t* t) // 90 99: ms →
{
	Text_SetCursorInterval(o, (int)Thread_Pop(t));
	return 0;
}

// where the cursor animation is drawn: `mode` 1 at the fixed position (x, y), else after the text
static int Opcode_Gfx0_TextSetCursorPos(OpenBGI_t* o, Thread_t* t) // 90 9A: mode, x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	int mode = (int)Thread_Pop(t);
	Text_SetCursorPos(o, mode, x, y);
	return 0;
}

/* with `on`, a text output that waits at its end does not start that wait
 * (cursor animation and input) until `ms` have passed since the output
 * began, unless input already hurried it */
static int Opcode_Gfx0_TextSetStartDelay(OpenBGI_t* o, Thread_t* t) // 90 9B: on, ms →
{
	int ms = (int)Thread_Pop(t);
	int on = (int)Thread_Pop(t);
	Text_SetStartDelay(o, on, ms);
	return 0;
}

// switch the character shadow of the text output on or off
static int Opcode_Gfx0_TextSetStyleOn(OpenBGI_t* o, Thread_t* t) // 90 9C: on →
{
	Text_SetStyleOn(o, (int)Thread_Pop(t));
	return 0;
}

/* the shadow's offset in percent of the font size (0 .. 100 each; the
 * handler only rejects negative values, Text_SetStyleParams ignores a
 * value above 100) and its density `level` (0 .. 0x100) */
static int Opcode_Gfx0_TextSetStyleParams(OpenBGI_t* o, Thread_t* t) // 90 9D: dxPct, dyPct, level →
{
	uint32_t level = Thread_Pop(t);
	int dyPct = (int)Thread_Pop(t);
	int dxPct = (int)Thread_Pop(t);
	if(dxPct < 0 || dyPct < 0)
		GFX_ERROR(o, t, MSG_BAD_COORDS, dxPct, dyPct);
	if(level > 0x100)
		GFX_ERROR(o, t, MSG_BAD_DENSITY, (int)level);
	Text_SetStyleParams(o, dxPct, dyPct, (int)level);
	return 0;
}

/* the custom glyph sheet: bitmap `bmp` is a strip of `count` (0 .. 0xFF)
 * equal cells drawn for the character codes 0xFF01 .. 0xFF00 + count; a
 * count of 0 releases the sheet.  3: the width is not a multiple of the
 * count. */
static int Opcode_Gfx0_TextSetGlyphSheet(OpenBGI_t* o, Thread_t* t) // 90 9E: count, bmp →
{
	int bmp = (int)Thread_Pop(t);
	int count = (int)Thread_Pop(t);
	switch(Text_SetGlyphSheet(o, count, bmp))
	{
		case(int)0x80000001: GFX_ERROR(o, t, MSG_BAD_CHAR_COUNT, count); break;
		case(int)0x80000002: GFX_ERROR(o, t, MSG_BAD_BITMAP, bmp); break;
		case(int)0x80000003: GFX_ERROR(o, t, MSG_BMP_UNSUITABLE, bmp); break;
		default: break;
	}
	return 0;
}

// with `f`, input during a text output finishes it at once instead of only switching to fast mode
static int Opcode_Gfx0_TextSetInputFinishes(OpenBGI_t* o, Thread_t* t) // 90 9F: f →
{
	Text_SetInputFinishes(o, (int)Thread_Pop(t));
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
static int Opcode_Gfx0_MenuSelect(OpenBGI_t* o, Thread_t* t) // 90 A0: h, n, items, cols, centre, colour, init, allowCancel →
{
	int allowCancel = (int)Thread_Pop(t);
	int init = (int)Thread_Pop(t);
	uint32_t colour = Thread_Pop(t);
	int centre = (int)Thread_Pop(t);
	int cols = (int)Thread_Pop(t);
	const void* items = PopPtr(o, t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(StartMenuSelect(o, t, h, n, items, cols, centre, colour, init, allowCancel))
	{
		case(int)0x80000001: GFX_ERROR(o, t, MSG_BAD_ITEM_COUNT, n); break;
		case(int)0x80000002: GFX_ERROR(o, t, MSG_BAD_INITIAL_ITEM, init); break;
		case(int)0x80000003: GFX_ERROR(o, t, MSG_BAD_COLUMN_COUNT, cols); break;
		case(int)0x80000004: ScriptError(o, MSG_WINDOW_NO_FONT, t); break;
		case -1: ScriptError(o, MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 2;
}

// lay the entries out as "90 A0" does, without a cursor and without waiting
static int Opcode_Gfx0_MenuDraw(OpenBGI_t* o, Thread_t* t) // 90 A1: h, n, items, cols, centre, colour →
{
	uint32_t colour = Thread_Pop(t);
	int centre = (int)Thread_Pop(t);
	int cols = (int)Thread_Pop(t);
	const void* items = PopPtr(o, t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(Menu_Draw(o, h, n, items, t, cols, centre, colour))
	{
		case(int)0x80000001: GFX_ERROR(o, t, MSG_BAD_ITEM_COUNT, n); break;
		case(int)0x80000003: GFX_ERROR(o, t, MSG_BAD_COLUMN_COUNT, cols); break;
		case -1: ScriptError(o, MSG_BAD_WINDOW_HANDLE, t); break;
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
static int MenuSelectBmp(OpenBGI_t* o, Thread_t* t, int mode)
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
	const void* items = PopPtr(o, t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(StartMenuSelectBmp(o, t, h, n, items, cols, centre, colour, init, allowCancel, bmp1, dx1, dy1, bmp2, dx2, dy2,
		mode))
	{
		case -1: ScriptError(o, MSG_BAD_WINDOW_HANDLE, t); break;
		case(int)0x80000001: GFX_ERROR(o, t, MSG_BAD_ITEM_COUNT, n); break;
		case(int)0x80000002: GFX_ERROR(o, t, MSG_BAD_INITIAL_ITEM, init); break;
		case(int)0x80000003: GFX_ERROR(o, t, MSG_BAD_COLUMN_COUNT, cols); break;
		case(int)0x80000004: ScriptError(o, MSG_WINDOW_NO_FONT, t); break;
		case(int)0x80000005: GFX_ERROR(o, t, MSG_BMP_INVALID, bmp1); break;
		case(int)0x80000006: GFX_ERROR(o, t, MSG_BMP_SCREEN_INCOMPAT, bmp1); break;
		case(int)0x80000007: GFX_ERROR(o, t, MSG_BMP_INVALID, bmp2); break;
		case(int)0x80000008: GFX_ERROR(o, t, MSG_BMP_SCREEN_INCOMPAT, bmp2); break;
		default: break;
	}
	return 2;
}

// the mouse-driven menu with decoration bitmaps (MenuSelectBmp mode 1)
static int Opcode_Gfx0_MenuSelectBmp(OpenBGI_t* o, Thread_t* t) // 90 A2: h, n, items, cols, centre, colour, init, allowCancel, bmp1, dx1, dy1, bmp2, dx2, dy2 →
{
	return MenuSelectBmp(o, t, 1);
}

// the input-less flash of one entry (MenuSelectBmp mode 0)
static int Opcode_Gfx0_MenuSelectBmp0(OpenBGI_t* o, Thread_t* t) // 90 A3: h, n, items, cols, centre, colour, init, allowCancel, bmp1, dx1, dy1, bmp2, dx2, dy2 →
{
	return MenuSelectBmp(o, t, 0);
}

// the colour of the menu cursor in each of its two blink phases (0 hides it in that phase)
static int Opcode_Gfx0_MenuSetCursorColours(OpenBGI_t* o, Thread_t* t) // 90 A4: c0, c1 →
{
	uint32_t c1 = Thread_Pop(t);
	uint32_t c0 = Thread_Pop(t);
	Menu_SetCursorColours(o, c0, c1);
	return 0;
}

// the blink interval of the menu cursor (600 ms by default)
static int Opcode_Gfx0_MenuSetBlink(OpenBGI_t* o, Thread_t* t) // 90 A5: ms →
{
	Menu_SetBlinkInterval(o, Thread_Pop(t));
	return 0;
}

/* with `on`, the mouse wheel steps through a menu by gliding the pointer to
 * the next entry (an Ease() `curve` over `duration` ms at `fps` updates per
 * second); off, the wheel is ignored */
static int Opcode_Gfx0_MenuSetWheel(OpenBGI_t* o, Thread_t* t) // 90 A6: on, curve, duration, fps →
{
	int fps = (int)Thread_Pop(t);
	int duration = (int)Thread_Pop(t);
	int curve = (int)Thread_Pop(t);
	int on = (int)Thread_Pop(t);
	Menu_SetWheelMove(o, on, curve, duration, fps);
	return 0;
}

/* the window's colour table: 16 words in script memory, one colour per
 * menu entry (used instead of the menu colour while set); a null pointer
 * removes it */
static int Opcode_Gfx0_WindowSetTable(OpenBGI_t* o, Thread_t* t) // 90 A7: h, table →
{
	const int32_t* table = (const int32_t*)PopPtr(o, t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_WindowSetTable(o, h, table))
		ScriptError(o, MSG_BAD_WINDOW_HANDLE, t);
	return 0;
}

// whether the menu, icon and panel selections take input only while the window is the active one
static int Opcode_Gfx0_SelectRequireActive(OpenBGI_t* o, Thread_t* t) // 90 AF: f →
{
	Select_RequireActive(o, (int)Thread_Pop(t));
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
static int IconSelectResult(OpenBGI_t* o, Thread_t* t, int r, int n, int mode)
{
	switch(r)
	{
		case(int)0x80000001: GFX_ERROR(o, t, MSG_BAD_ICON_COUNT, n); break;
		case(int)0x80000002: GFX_ERROR(o, t, MSG_BAD_FOCUS_MODE, mode); break;
		case(int)0x80000003: ScriptError(o, MSG_ICON_BMPS_INVALID, t); break;
		case -1: ScriptError(o, MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 2;
}

/* icon selection with 16-byte records (x, y, bitmap, focus bitmap); the
 * focus bitmap (-1 = none) is drawn over the icon under the mouse */
static int Opcode_Gfx0_IconSelect(OpenBGI_t* o, Thread_t* t) // 90 B0: h, n, icons, hotkeys, mode, allowCancel, useHitMask →
{
	int useHitMask = (int)Thread_Pop(t);
	int allowCancel = (int)Thread_Pop(t);
	int mode = (int)Thread_Pop(t);
	const int32_t* hotkeys = (const int32_t*)PopPtr(o, t);
	const void* icons = PopPtr(o, t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	return IconSelectResult(o, t, StartIconSelect(o, t, h, n, icons, hotkeys, mode, allowCancel, useHitMask), n, mode);
}

/* icon selection with 0x40-byte records (x, y, bitmap, four (x, y, bitmap)
 * overlay triples for the window's item layers, a hit-mask bitmap or -1
 * for the icon's own) */
static int Opcode_Gfx0_IconSelect2(OpenBGI_t* o, Thread_t* t) // 90 B1: h, n, icons, hotkeys, mode, allowCancel, useHitMask →
{
	int useHitMask = (int)Thread_Pop(t);
	int allowCancel = (int)Thread_Pop(t);
	int mode = (int)Thread_Pop(t);
	const int32_t* hotkeys = (const int32_t*)PopPtr(o, t);
	const void* icons = PopPtr(o, t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	return IconSelectResult(o, t, StartIconSelect2(o, t, h, n, icons, hotkeys, mode, allowCancel, useHitMask), n, mode);
}

// draw the icons of 16-byte records ("90 B0" layout) into the window without waiting
static int Opcode_Gfx0_IconDraw(OpenBGI_t* o, Thread_t* t) // 90 B4: h, n, icons →
{
	const void* icons = PopPtr(o, t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(Icon_Draw(o, h, n, icons))
	{
		case(int)0x80000001: GFX_ERROR(o, t, MSG_BAD_ICON_COUNT, n); break;
		case -1: ScriptError(o, MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* "90 B4" for the 0x40-byte records of "90 B1": the first 12 bytes of each
 * (x, y, bitmap) become a 16-byte Icon_Draw record with no focus bitmap.
 * 1 .. 64 records. */
static int Opcode_Gfx0_IconDraw64(OpenBGI_t* o, Thread_t* t) // 90 B5: h, n, icons64 →
{
	const uint8_t* icons = (const uint8_t*)PopPtr(o, t);
	int n = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	int32_t* rec;
	int i, r;
	if(n < 1 || n > 0x40)
		GFX_ERROR(o, t, MSG_BAD_ICON_COUNT, n);
	rec = (int32_t*)BGI_Alloc((size_t)n << 4);
	for(i = 0; i < n; i++)
	{
		memcpy(&rec[i * 4], icons + i * 0x40, 12); // x, y, bitmap
		rec[i * 4 + 3] = -1;
	}
	r = Icon_Draw(o, h, n, rec);
	BGI_Free(rec);
	switch(r)
	{
		case(int)0x80000001: GFX_ERROR(o, t, MSG_BAD_ICON_COUNT, n); break;
		case -1: ScriptError(o, MSG_BAD_WINDOW_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* A button panel is a table of button groups in script memory (the layout
 * described in src/include/bgi/panel.h) that is either painted
 * once into a window ("90 B6") or run as a "sub-object": an interactive
 * procedure created with "90 B8", started from a table with "90 BA", polled
 * every pass by the scheduler, queried with "90 BC" .. "90 BF" and deleted
 * with "90 B9".  Unlike the selections above none of these instructions
 * blocks the thread; the results are pushed, not raised. */

/* paint the buttons of the table at `desc` into the text layer of window
 * `h`; the result is 0 ok, 1 no such window, 2 the group table is unusable
 * (null or not 1 .. 0x100 groups), 3 a button table is */
static int Opcode_Gfx0_PanelDraw(OpenBGI_t* o, Thread_t* t) // 90 B6: h, desc → r
{
	const void* desc = PopPtr(o, t);
	uint32_t h = Thread_Pop(t);
	Thread_Push(t, (uint32_t)Panel_Draw(o, h, desc, t));
	return 0;
}

// "90 B6" for a sprite-panel table: one sub-sprite per button; result codes as "90 B6"
static int Opcode_Gfx0_SprPanelDraw(OpenBGI_t* o, Thread_t* t) // 90 B7: h, desc → r
{
	const void* desc = PopPtr(o, t);
	uint32_t h = Thread_Pop(t);
	Thread_Push(t, (uint32_t)SprPanel_Draw(o, h, desc, t));
	return 0;
}

/* create a button panel procedure on window `h` and push its id (an unknown
 * handle still creates one, on no window) */
static int Opcode_Gfx0_SubObjCreate(OpenBGI_t* o, Thread_t* t) // 90 B8: h → id
{
	Thread_Push(t, SubObj_Create(o, Thread_Pop(t), 0));
	return 0;
}

// destroy the procedure with that id; 1 when there was one, 0 otherwise
static int Opcode_Gfx0_SubObjDelete(OpenBGI_t* o, Thread_t* t) // 90 B9: id → r
{
	Thread_Push(t, (uint32_t)SubObj_Delete(o, Thread_Pop(t)));
	return 0;
}

/* start (or restart) panel `id` from the table at `desc`: the buttons are
 * drawn and from now on the panel tracks the mouse and keys each pass.
 * Result: 0 ok, 1 no panel with that id, 2 / 3 the group / button table is
 * unusable, 4 the id is a sprite panel ("91 BA" starts those). */
static int Opcode_Gfx0_PanelStart(OpenBGI_t* o, Thread_t* t) // 90 BA: id, desc → r
{
	const void* desc = PopPtr(o, t);
	uint32_t id = Thread_Pop(t);
	Thread_Push(t, (uint32_t)Panel_Start(o, id, desc, t));
	return 0;
}

/* the panel's state into six words at `out`: started, the decided group,
 * index and flag, and the click position; the result is 1 when `id` is a
 * panel (the queries below push the same) */
static int Opcode_Gfx0_SubObjGetCursor(OpenBGI_t* o, Thread_t* t) // 90 BC: out, id → f
{
	uint32_t id = Thread_Pop(t);
	int32_t* out = (int32_t*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)SubObj_GetCursor(o, out, id));
	return 0;
}

// the index of the panel's current group into *out (-1 for none)
static int Opcode_Gfx0_SubObjGetGroup(OpenBGI_t* o, Thread_t* t) // 90 BD: out, id → f
{
	uint32_t id = Thread_Pop(t);
	int32_t* out = (int32_t*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)SubObj_GetGroup(o, out, id));
	return 0;
}

// the selected button index of every group into `out` (one word per group)
static int Opcode_Gfx0_SubObjGetSelections(OpenBGI_t* o, Thread_t* t) // 90 BE: out, id → f
{
	uint32_t id = Thread_Pop(t);
	int32_t* out = (int32_t*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)SubObj_GetSelections(o, out, id));
	return 0;
}

/* take the oldest event of the panel's queue into three words at `out`
 * (message, a, b; the 0x1000000n messages of src/sys/panel.c), zeros when
 * the queue is empty */
static int Opcode_Gfx0_SubObjPopEvent(OpenBGI_t* o, Thread_t* t) // 90 BF: out, id → f
{
	uint32_t id = Thread_Pop(t);
	uint32_t* out = (uint32_t*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)SubObj_PopEvent(o, out, id));
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
static int Opcode_Gfx0_KnobCreate(OpenBGI_t* o, Thread_t* t) // 90 D0: target → h
{
	uint32_t target = Thread_Pop(t);
	uint32_t h;
	if(GfxCall_ObjHasOwner(o, target))
		ScriptError(o, MSG_OBJ_HAS_OWNER, t);
	switch(GfxCall_KnobCreate(o, &h, target))
	{
		case 1: ScriptError(o, MSG_NO_MORE_KNOBS, t); break;
		case 2: ScriptError(o, MSG_BAD_TARGET_HANDLE, t); break;
		case 3: ScriptError(o, MSG_VIRTUAL_TARGET, t); break;
		default: break;
	}
	Knob_Register(o, h);
	Thread_Push(t, h);
	return 0;
}

// release, unregister and delete the knob; the target object stays and loses its owner
static int Opcode_Gfx0_KnobDelete(OpenBGI_t* o, Thread_t* t) // 90 D1: h →
{
	uint32_t h = Thread_Pop(t);
	Knob_Release(o, h);
	Knob_Unregister(o, h);
	if(!GfxCall_KnobDelete(o, h))
		ScriptError(o, MSG_BAD_KNOB_HANDLE, t);
	return 0;
}

static int Opcode_Gfx0_KnobShow(OpenBGI_t* o, Thread_t* t) // 90 D4: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_KnobShow(o, h, f))
		ScriptError(o, MSG_BAD_KNOB_HANDLE, t);
	return 0;
}

// the track origin: the travel rectangle is relative to it
static int Opcode_Gfx0_KnobSetPos(OpenBGI_t* o, Thread_t* t) // 90 D5: h, x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_KnobSetPos(o, h, x, y))
		ScriptError(o, MSG_BAD_KNOB_HANDLE, t);
	return 0;
}

/* the knob's value (in steps, or pixels for a continuous axis) and with it
 * the thumb's position; a value outside the range is dropped silently */
static int Opcode_Gfx0_KnobSetValue(OpenBGI_t* o, Thread_t* t) // 90 D6: h, x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_KnobSetValue(o, h, x, y))
		ScriptError(o, MSG_BAD_KNOB_HANDLE, t);
	return 0;
}

// the knob's current value; x is pushed first, y is on top
static int Opcode_Gfx0_KnobGetValue(OpenBGI_t* o, Thread_t* t) // 90 D7: h → x, y
{
	int32_t out[2];
	if(!GfxCall_KnobGetValue(o, Thread_Pop(t), out))
		ScriptError(o, MSG_BAD_KNOB_HANDLE, t);
	Thread_Push(t, (uint32_t)out[0]);
	Thread_Push(t, (uint32_t)out[1]);
	return 0;
}

// the number of positions per axis (0 = continuous, in pixels); 4 for a negative count
static int Opcode_Gfx0_KnobSetSteps(OpenBGI_t* o, Thread_t* t) // 90 D8: h, x, y →
{
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_KnobSetSteps(o, h, x, y))
	{
		case 4: GFX_ERROR(o, t, MSG_BAD_KNOB_STEP, x, y); break;
		case -1: ScriptError(o, MSG_BAD_KNOB_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// the size of the travel rectangle in pixels; 5 when it would not hold the thumb
static int Opcode_Gfx0_KnobSetRange(OpenBGI_t* o, Thread_t* t) // 90 D9: h, w, hgt →
{
	int hgt = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	switch(GfxCall_KnobSetRange(o, h, w, hgt))
	{
		case 5: GFX_ERROR(o, t, MSG_BAD_KNOB_RANGE, w, hgt); break;
		case -1: ScriptError(o, MSG_BAD_KNOB_HANDLE, t); break;
		default: break;
	}
	return 0;
}

/* report and clear the last keyboard / wheel nudge of the knob: the
 * vertical step it asked for when the range rejected it (so the script can
 * act on a knob pushed past its end), 0 otherwise */
static int Opcode_Gfx0_KnobTakeNudge(OpenBGI_t* o, Thread_t* t) // 90 DA: h → v
{
	int32_t out;
	if(!GfxCall_KnobTakeNudge(o, Thread_Pop(t), &out))
		ScriptError(o, MSG_BAD_KNOB_HANDLE, t);
	Thread_Push(t, (uint32_t)out);
	return 0;
}

// the handle of the last knob flagged by a right click (0 when none); every flag is cleared
static int Opcode_Gfx0_KnobPollChanged(OpenBGI_t* o, Thread_t* t) // 90 DB: → h
{
	Thread_Push(t, Knob_PollChanged(o));
	return 0;
}

// whether the mouse may drag the knob's thumb (the default)
static int Opcode_Gfx0_KnobSetDraggable(OpenBGI_t* o, Thread_t* t) // 90 DC: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_KnobSetDraggable(o, h, f))
		ScriptError(o, MSG_BAD_KNOB_HANDLE, t);
	return 0;
}

/* set the global knob-wheel switch and push its previous value: while it
 * is on, the mouse wheel nudges the most recently captured knob instead of
 * being reported as the wheel keys */
static int Opcode_Gfx0_KnobSwapGlobal(OpenBGI_t* o, Thread_t* t) // 90 DD: v → old
{
	int v = (int)Thread_Pop(t);
	int old = Knob_GetGlobal(o);
	Knob_SetGlobal(o, v);
	Thread_Push(t, (uint32_t)old);
	return 0;
}

// put the knob on top of the captured list (the one the wheel nudges, see "90 DD")
static int Opcode_Gfx0_KnobCapture(OpenBGI_t* o, Thread_t* t) // 90 DE: h →
{
	if(!Knob_Capture(o, Thread_Pop(t)))
		ScriptError(o, MSG_BAD_KNOB_HANDLE, t);
	return 0;
}

/* take the knob off the captured list; the handle error is raised also for
 * a knob that was not captured */
static int Opcode_Gfx0_KnobRelease(OpenBGI_t* o, Thread_t* t) // 90 DF: h →
{
	if(!Knob_Release(o, Thread_Pop(t)))
		ScriptError(o, MSG_BAD_KNOB_HANDLE, t);
	return 0;
}

// -------------------------------------------------------------------------
// 90 CA : the aspect-keeping bicubic fit (1.616 on)
// -------------------------------------------------------------------------

/* src scaled into dst keeping its aspect ratio, centred, the bars beside
 * it cleared (BmpOp_FitBicubic) */
static int Opcode_Gfx0_FitBicubic(OpenBGI_t* o, Thread_t* t) // 90 CA (1.616 on): dst, src →
{
	int src = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	switch(BmpOp_FitBicubic(o, dst, src))
	{
		case 0x80000009u: GFX_ERROR(o, t, MSG_TONE_DST_INVALID, dst); break;
		case 0x8000000au: GFX_ERROR(o, t, MSG_GEN_SRC_INVALID, src); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 90 C3 : a bitmap at half the size (1.599 on)
// -------------------------------------------------------------------------

// dst made src at half its size, 2 x 2 pixels averaged (BmpOp_HalfSize); the save thumbnails
static int Opcode_Gfx0_HalfSize(OpenBGI_t* o, Thread_t* t) // 90 C3 (1.599 on): dst, src →
{
	int src = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	CheckBitmapNo(o, dst, t);
	CheckBitmapNo(o, src, t);
	switch(BmpOp_HalfSize(o, dst, src))
	{
		case 0x80000006u: GFX_ERROR(o, t, MSG_SRC_TOO_SMALL, src); break;
		case 0x80000009u: GFX_ERROR(o, t, MSG_TONE_DST_INVALID, dst); break;
		case 0x8000000au: GFX_ERROR(o, t, MSG_GEN_SRC_INVALID, src); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 90 C4, 90 C5 : picture files outside the archives (1.553 on)
// -------------------------------------------------------------------------

/* the picture file at `path` (BMP, PNG, JPEG, GIF; a path of the file
 * system, not an archive name) into bmp at its own size: mode 1 RGB32, 2
 * ARGB32, 3 GRAY8 (palette indices), -1 as the file is.  r 0 ok, with
 * mode -1 1 no such file and 2 not readable, -1 any other failure (also
 * a missing file with mode 1 .. 3) */
static int Opcode_Gfx0_BmpLoadPicture(OpenBGI_t* o, Thread_t* t) // 90 C4 (1.553 on): bmp, path, mode → r
{
	int mode = (int)Thread_Pop(t);
	const char* path = (const char*)PopPtr(o, t);
	int bmp = (int)Thread_Pop(t);
	uint32_t r;
	CheckBitmapNo(o, bmp, t);
	r = (uint32_t)BmpOp_LoadPicture(o, bmp, path, mode);
	Thread_Push(t, r == 0 ? 0 : r == 0x80000001u ? 1
			: r == 0x80000002u                   ? 2
												 : 0xFFFFFFFFu);
	return 0;
}

/* bmp as a picture file at `path` (the whole name, nothing added): format
 * 0 BMP, 1 JPEG at `quality`, 2 GIF, 3 TIFF, 4 PNG.  r 0 ok, 3 no bitmap,
 * 4 another format, 5 not written (GIF and TIFF are never written here) */
static int Opcode_Gfx0_BmpSavePicture(OpenBGI_t* o, Thread_t* t) // 90 C5 (1.553 on): path, format, quality, bmp → r
{
	int bmp = (int)Thread_Pop(t);
	int quality = (int)Thread_Pop(t);
	uint32_t format = Thread_Pop(t);
	const char* path = (const char*)PopPtr(o, t);
	uint32_t r;
	CheckBitmapNo(o, bmp, t);
	r = (uint32_t)BmpOp_SavePicture(o, bmp, path, format, quality);
	Thread_Push(t, r == 0 ? 0 : (r & 0xffu) >= 3 && (r & 0xffu) <= 5 ? (r & 0xffu)
																	 : 0xFFFFFFFFu);
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
static int Opcode_Gfx0_ToneCurveRegister(OpenBGI_t* o, Thread_t* t) // 90 CC (1.616 on): id, points →
{
	const int32_t* points = (const int32_t*)PopPtr(o, t);
	int32_t id = (int32_t)Thread_Pop(t);
	if(ToneCurve_Register(o, id, points) == 0x8000000fu)
		GFX_ERROR(o, t, MSG_TONE_BAD_ID, id);
	return 0;
}

/* dst becomes src (32-bit) through curve `curveId` with the monochrome
 * mix (colour, level 0 .. 256) and the colour film (effect 8 / 0x26,
 * colour, level 0 .. 256); see BmpOp_ToneCurve for the errors */
static int Opcode_Gfx0_ToneCurveApply(OpenBGI_t* o, Thread_t* t) // 90 CD (1.616 on): dst, src, monoColour, monoLevel, curveId, filmColour, effect, filmLevel →
{
	int filmLevel = (int)Thread_Pop(t);
	int effect = (int)Thread_Pop(t);
	uint32_t filmColour = Thread_Pop(t);
	int32_t curveId = (int32_t)Thread_Pop(t);
	int monoLevel = (int)Thread_Pop(t);
	uint32_t monoColour = Thread_Pop(t);
	int src = (int)Thread_Pop(t);
	int dst = (int)Thread_Pop(t);
	switch(BmpOp_ToneCurve(o, dst, src, curveId, monoColour, monoLevel, filmColour, effect, filmLevel))
	{
		case 0x80000009u: GFX_ERROR(o, t, MSG_TONE_DST_INVALID, dst); break;
		case 0x8000000au: GFX_ERROR(o, t, MSG_GEN_SRC_INVALID, src); break;
		case 0x8000000fu: GFX_ERROR(o, t, MSG_TONE_NO_CURVE, curveId); break;
		case 0x80000011u: GFX_ERROR(o, t, MSG_TONE_MODE_UNSUPPORTED, src); break;
		case 0x80000013u: GFX_ERROR(o, t, MSG_TONE_EFFECT_UNSUPPORTED, effect); break;
		case 0x80000014u: GFX_ERROR(o, t, MSG_TONE_BAD_FILM_LEVEL, filmLevel); break;
		case 0x80000015u: GFX_ERROR(o, t, MSG_TONE_BAD_MONO_LEVEL, monoLevel); break;
		default: break;
	}
	return 0;
}

// -------------------------------------------------------------------------
// 90 E0 .. 90 E9 : groups
// -------------------------------------------------------------------------

static int Opcode_Gfx0_GroupCreate(OpenBGI_t* o, Thread_t* t) // 90 E0: → h
{
	uint32_t h = GfxCall_GroupCreate(o);
	if(!h)
		ScriptError(o, MSG_NO_MORE_GROUPS, t);
	Thread_Push(t, h);
	return 0;
}

static int Opcode_Gfx0_GroupDelete(OpenBGI_t* o, Thread_t* t) // 90 E1: h →
{
	if(!GfxCall_GroupDelete(o, Thread_Pop(t)))
		ScriptError(o, MSG_BAD_GROUP_HANDLE, t);
	return 0;
}

static int Opcode_Gfx0_GroupShow(OpenBGI_t* o, Thread_t* t) // 90 E4: h, f →
{
	int f = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	if(!GfxCall_GroupShow(o, h, f))
		ScriptError(o, MSG_BAD_GROUP_HANDLE, t);
	return 0;
}

/* A group is an object without a picture that carries children: a child
 * added to it follows the group's position (at its offset), inherits its
 * level, fade and visibility, and has the group as its owner until it is
 * removed again. */

// the group's position and level; both are propagated to the children
static int Opcode_Gfx0_GroupSet(OpenBGI_t* o, Thread_t* t) // 90 E5: h, x, y, level →
{
	uint32_t level = Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	uint32_t h = Thread_Pop(t);
	CheckAlpha(o, level, t);
	if(!GfxCall_GroupSet(o, h, x, y, (int)level))
		ScriptError(o, MSG_BAD_GROUP_HANDLE, t);
	return 0;
}

/* add object `hObj` to the group at offset (dx, dy) from the group's
 * position; refused when the object is the group itself or already has an
 * owner */
static int Opcode_Gfx0_GroupAdd(OpenBGI_t* o, Thread_t* t) // 90 E8: hGroup, hObj, dx, dy →
{
	int dy = (int)Thread_Pop(t);
	int dx = (int)Thread_Pop(t);
	uint32_t hObj = Thread_Pop(t);
	uint32_t hGroup = Thread_Pop(t);
	switch(GfxCall_GroupAdd(o, hGroup, hObj, dx, dy))
	{
		case 1: ScriptError(o, MSG_BAD_OBJ_HANDLE, t); break;
		case 3: ScriptError(o, MSG_GROUP_SELF, t); break;
		case 4: ScriptError(o, MSG_OBJ_HAS_OWNER, t); break;
		case -1: ScriptError(o, MSG_BAD_GROUP_HANDLE, t); break;
		default: break;
	}
	return 0;
}

// take the object out of the group again; 2 when it is not a member
static int Opcode_Gfx0_GroupRemove(OpenBGI_t* o, Thread_t* t) // 90 E9: hGroup, hObj →
{
	uint32_t hObj = Thread_Pop(t);
	uint32_t hGroup = Thread_Pop(t);
	switch(GfxCall_GroupRemove(o, hGroup, hObj))
	{
		case 1: ScriptError(o, MSG_BAD_OBJ_HANDLE, t); break;
		case 2: ScriptError(o, MSG_NOT_IN_GROUP, t); break;
		case -1: ScriptError(o, MSG_BAD_GROUP_HANDLE, t); break;
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
static int Opcode_Gfx0_MoviePlay(OpenBGI_t* o, Thread_t* t) // 90 F0: file, x, y, w, h → length
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int y = (int)Thread_Pop(t);
	int x = (int)Thread_Pop(t);
	const char* file = (const char*)PopPtr(o, t);
	char path[0x104];
	int32_t length = 0;
	if(w <= 0 || h <= 0)
		GFX_ERROR(o, t, MSG_BAD_MOVIE_SIZE, w, h);
	while(!FileExists(o, NULL, file))
	{
		char msg[0x100];
		sprintf(msg, MSG_MOVIE_NOT_FOUND, file);
		RetryPrompt(o, msg);
	}
	FindMoviePath(o, path, file);
	Thread_Push(t, Movie_Play(o, &length, path, x, y, w, h) ? (uint32_t)length : 0u);
	return 0;
}

// stop the movie and repaint the window from the back buffer
static int Opcode_Gfx0_MovieStop(OpenBGI_t* o, Thread_t* t) // 90 F1: →
{
	Movie_Stop(o);
	Window_Repaint(o);
	return 0;
}

// whether a movie is still playing
static int Opcode_Gfx0_MoviePlaying(OpenBGI_t* o, Thread_t* t) // 90 F2: → f
{
	Thread_Push(t, (uint32_t)Movie_IsPlaying(o));
	return 0;
}

// the movie volume, 0 .. 0x80 (anything above is treated as 0)
static int Opcode_Gfx0_MovieVolume(OpenBGI_t* o, Thread_t* t) // 90 F3: v →
{
	Movie_SetVolume(o, Thread_Pop(t));
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
static int Opcode_Gfx0_SeqLoad(OpenBGI_t* o, Thread_t* t) // 90 F4 (1.69/472 on): &no, &info, arc, name →
{
	const char* name = (const char*)PopPtr(o, t);
	const char* arc = (const char*)PopPtr(o, t);
	int32_t* info = (int32_t*)PopPtr(o, t);
	int32_t* outNo = (int32_t*)PopPtr(o, t);
	Thread_SetWait(o, t, WaitSeqLoad_New(o, t, arc, name, outNo, info));
	return 2;
}

/* The first version of the instruction (1.69 build 451, the retail
 * Tayutama) reads the file on the calling thread: it pushes 0 when the
 * file is missing (nothing written), 1 when it could not be read, 2 when
 * it is not a movie file, and 0 once the sequence is registered. */
static int Opcode_Gfx0_SeqLoadSync(OpenBGI_t* o, Thread_t* t) // 90 F4 (1.69/451): &no, &info, arc, name → r
{
	const char* name = (const char*)PopPtr(o, t);
	const char* arc = (const char*)PopPtr(o, t);
	int32_t* info = (int32_t*)PopPtr(o, t);
	int32_t* outNo = (int32_t*)PopPtr(o, t);
	uint32_t size = GetFileSize_(o, arc, name), r = 0;
	if(size)
	{
		uint8_t* buf = (uint8_t*)Vm_TempAlloc(o, size);
		uint32_t n = LoadFile(o, buf, arc, name);
		Vm_TempKeep(o, buf);
		if(n != size)
			r = 1;
		else
			switch(BmSeq_Register(o, buf, size, outNo, info))
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
static int Opcode_Gfx0_SeqFree(OpenBGI_t* o, Thread_t* t) // 90 F5: no → r
{
	uint32_t r = BmSeq_Free(o, (int32_t)Thread_Pop(t));
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
static int Opcode_Gfx0_SeqDecode(OpenBGI_t* o, Thread_t* t) // 90 F6: bmp, no, frame → r
{
	uint32_t frame = Thread_Pop(t);
	int no = (int)Thread_Pop(t);
	int bmp = (int)Thread_Pop(t);
	uint32_t r;
	if(BmSeq_TakeDeferFlag(o))
	{
		Thread_SetWait(o, t, WaitSeqDecode_New(o, t, bmp, no, frame));
		return 2;
	}
	r = BmSeq_Decode(o, bmp, no, frame);
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
static int Opcode_Gfx0_SeqClone(OpenBGI_t* o, Thread_t* t) // 90 F7: no, &out → r
{
	int32_t* out = (int32_t*)PopPtr(o, t);
	int no = (int)Thread_Pop(t);
	uint32_t r = BmSeq_Clone(o, no, out);
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
static int Opcode_Gfx0_TargetClear(OpenBGI_t* o, Thread_t* t) // 90 F8: →
{
	Target_Clear(o);
	return 0;
}

// register sprite `h` as a click target; an error when the handle is not a sprite
static int Opcode_Gfx0_TargetAdd(OpenBGI_t* o, Thread_t* t) // 90 FA: h →
{
	if(!Target_Add(o, Thread_Pop(t)))
		ScriptError(o, MSG_TARGET_NOT_SPRITE, t);
	return 0;
}

// unregister the sprite; an error when it was not registered
static int Opcode_Gfx0_TargetRemove(OpenBGI_t* o, Thread_t* t) // 90 FB: h →
{
	if(!Target_Remove(o, Thread_Pop(t)))
		ScriptError(o, MSG_TARGET_NOT_REGISTERED, t);
	return 0;
}

// the index of the first target under the mouse (by its hit test), 0xFFFFFFFF for none
static int Opcode_Gfx0_TargetHit(OpenBGI_t* o, Thread_t* t) // 90 FC: → index
{
	Thread_Push(t, Target_Hit(o));
	return 0;
}

// the last sampled press state (1 pressed) of the target with that index
static int Opcode_Gfx0_TargetGet(OpenBGI_t* o, Thread_t* t) // 90 FD: index → pressed
{
	uint32_t out;
	if(!Target_Get(o, &out, Thread_Pop(t)))
		ScriptError(o, MSG_BAD_TARGET_NO, t);
	Thread_Push(t, out);
	return 0;
}

// -------------------------------------------------------------------------
// the table
// -------------------------------------------------------------------------

/* Fill o->vm.optable90 for the selected engine profile from the
 * registrations below (called once at start-up by Vm_TablesInit).  Each
 * entry names the opcode, its handler and the generations the handler is
 * valid for; an opcode whose meaning changed between builds has several
 * entries, and Vm_FillTable takes the one with the narrowest range that
 * covers the profile.  An opcode the profile defines without a matching
 * entry gets the not-implemented stub, one it does not define stays NULL. */
void Vm_Optable90Init(OpenBGI_t* o)
{
	static const OpEntry_t ops[] = {
		{0x00, Opcode_Gfx0_Present, GEN_FIRST, GEN_LAST},
		{0x01, Opcode_Gfx0_DisplayEnable, GEN_FIRST, GEN_LAST},
		{0x02, Opcode_Gfx0_SetFps, GEN_FIRST, GEN_LAST},
		{0x03, Opcode_Gfx0_SetCacheSize, GEN_FIRST, GEN_LAST},
		{0x04, Opcode_Gfx0_Snapshot, GEN_FIRST, GEN_LAST},
		{0x05, Opcode_Gfx0_SnapshotPrio, GEN_FIRST, GEN_LAST},
		{0x06, Opcode_Gfx0_SetProjCentre, GEN_1_529, GEN_LAST},
		{0x17, Opcode_Gfx0_BmpSetMode, GEN_1_492, GEN_LAST},
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
		{0x36, Opcode_Gfx0_ObjSetOffset2, GEN_1_492, GEN_LAST},
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
		{0x82, Opcode_Gfx0_WindowSetLayerOrder, GEN_1_492, GEN_LAST},
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
		{0xC0, Opcode_Gfx0_BmpCreateLikeFile, GEN_1_547, GEN_LAST},
		{0xC3, Opcode_Gfx0_HalfSize, GEN_1_599, GEN_LAST},
		{0xC4, Opcode_Gfx0_BmpLoadPicture, GEN_1_553, GEN_LAST},
		{0xC5, Opcode_Gfx0_BmpSavePicture, GEN_1_553, GEN_LAST},
		{0xCA, Opcode_Gfx0_FitBicubic, GEN_1_616, GEN_LAST},
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
	Vm_FillTable(o, o->vm.optable90, OPFAM_90, ops, BGI_COUNTOF(ops));
}
