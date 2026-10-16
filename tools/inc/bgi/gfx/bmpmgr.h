/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bmpmgr.h - the bitmap manager: the numbered bitmap slots of the scripts,
 * the ripple definitions and the text drawing front end (src/gfx/bmpmgr.c).
 *
 * Script bitmaps live in numbered slots.  A slot owns its pixels through a
 * PixBuf and carries a generation counter, so that an object which keeps a
 * bitmap number can notice that the script replaced the bitmap behind it
 * The manager also owns the ripple definitions of
 * instructions "92 00" / "92 01" and the font cache used for text.  There
 * is one manager, gBmpMgr, created at start-up with the slot count of the
 * engine configuration.
 */
#ifndef BGI_GFX_BMPMGR_H_
#define BGI_GFX_BMPMGR_H_

#include "bgi/pixbuf.h"
#include "bgi/gfx/bitmap.h"
#include "bgi/gfx/font.h"

// ---- slots and ripples -------------------------------------------------------
typedef struct BmpSlot
{
	PixBuf_t* buf;      // the pixel buffer; NULL = empty slot
	Bmp_t bmp;          // the descriptor of the bitmap in the slot (pixels point into buf)
	int32_t generation; // bumped on every (re)creation unless keepGen is set; what objects compare
	/* 1.529 on: the "base size" an image file declares in its header
	 * (words +0xC / +0xE when word +0xA is 1; the game's design size of a
	 * picture stored at another resolution).  Zero until an image sets it,
	 * -1 after the slot is freed; "92 16" reads it, "91 1F" copies it. */
	int32_t baseW;
	int32_t baseH;
} BmpSlot_t;

typedef struct RippleDef // one of the 8 ripple definitions of "92 00" / "92 01"
{
	int32_t defined; // 1 once defined; the table is valid
	int32_t pos;     // the running position; always 0 (nothing advances it)
	int32_t base;    // the wrap-around base; always 0
	int32_t period;  // entries per ring (one sine cycle and its silent cycles)
	int32_t total;   // period * rings: entries in the table
	int16_t* table;  // pairs of int16 (x, y), `total` of them
} RippleDef_t;

typedef struct BmpMgr
{
	FontCache_t* fonts;     // the font cache the text drawing opens fonts in
	BmpSlot_t* slots;       // `count` slots
	int32_t count;          // the number of slots (a valid number is 0 .. count - 1)
	int32_t nextGen;        // the generation the next created slot gets
	int32_t keepGen;        // "90 0B": re-created slots keep their generation
	RippleDef_t ripples[8]; // the ripple definitions
} BmpMgr_t;

extern BmpMgr_t* gBmpMgr; // the one manager of the engine

BmpMgr_t* BmpMgr_New(int count);          // allocate a manager with `count` empty slots and a font cache
void BmpMgr_Delete(BmpMgr_t* m);          // free every slot, the ripples, the font cache and the manager
void BmpMgr_FreeAll(BmpMgr_t* m);         // free every slot
int BmpMgr_FreeSlot(BmpMgr_t* m, int no); // "90 12": free slot `no`; 1 if it was in use, 0 if empty or invalid

/* "90 11": allocate slot `no` as an uninitialised w x h bitmap of pixel
 * mode `mode` (0 .. 6), freeing what was there; 1 ok, 0 for an invalid
 * slot number, mode or a failed allocation (the slot is then empty) */
int BmpMgr_Create(BmpMgr_t* m, int no, int w, int h, int mode);
int BmpMgr_GetInfo(BmpMgr_t* m, Bmp_t* out, int no);       // "90 16": the slot's descriptor into *out; 1 if the slot is in use
int BmpMgr_SetBaseSize(BmpMgr_t* m, int no, int w, int h); // 1.529 on ("92 12"): 1 for a valid slot number
int BmpMgr_GetBaseSize(BmpMgr_t* m, int32_t* out, int no); // 1.529 on ("92 16"): out[0], out[1]; 1 for a valid slot number
int BmpMgr_Generation(BmpMgr_t* m, int no);                // the slot's generation; -1 if empty or invalid
// "90 13": clear (colour 0) or fill the bitmap; 1 ok, 0 no bitmap
int BmpMgr_Clear(BmpMgr_t* m, int no, uint32_t colour);
uint32_t BmpMgr_SetMode(BmpMgr_t* m, int no, int mode); // 1.494 on, "90 17": relabel ARGB32 as RGB32; 0 ok, 0xb no bitmap, 0x15 impossible
/* "90 14": create and fill from packed pixels (24 bpp for RGB32, native
 * otherwise); an ARGB32 result is un-composited against gBmpOption.
 * 1 ok, 0 when the slot could not be created */
int BmpMgr_PutPixels(BmpMgr_t* m, int no, int w, int h, int mode, const void* data);
/* "90 15": packed pixels out (24 bpp for RGB32).  0 ok, 9 no bitmap,
 * 0xa buffer too small.  The second argument is never read. */
int BmpMgr_GetPixels(BmpMgr_t* m, void* dst, int32_t unused, int32_t size, int no);
int BmpMgr_ToGray(BmpMgr_t* m, int dstNo, int srcNo); // "92 18": dstNo created as the luminance of srcNo; 0 ok, 9 no source, 0xa not created
int BmpMgr_InvertGray(BmpMgr_t* m, int no);           // "92 19": 0 ok, 7 not GRAY8, 0xb no bitmap
int Bmp_Uncomposite(Bmp_t* b);                        // undo the composition over gBmpOption; 1 done, 0 nothing to do

/* ripples ("92 00" / "92 01"): a definition is a table of (x, y) int16
 * displacement pairs - `rings` repetitions of one sine cycle of `period`
 * (times 4) entries with amplitude `amplitude`, each followed by count - 1
 * silent cycles; the Ex form surrounds the cycle with `fadeIn` growing and
 * `fadeOut` decaying copies (amplitude / 2^k).  0 ok, 0x10 bad number */
int BmpMgr_DefineRipple(BmpMgr_t* m, int no, int period, int amplitude, int count, int rings);
int BmpMgr_DefineRippleEx(BmpMgr_t* m, int no, int period, int amplitude, int fadeIn, int fadeOut,
	int count, int rings);
void BmpMgr_FreeRipples(BmpMgr_t* m); // drop all eight definitions
/* whether `rings` rings (4 entries each) are available from phase
 * `selector` of definition `no`: *ok = 1 / 0; 0 ok, 0x10 bad number,
 * 0x11 not defined */
int BmpMgr_RippleCheck(BmpMgr_t* m, int* ok, int no, int selector, int rings);
/* fill the ring table of a ripple blitter (rings * 4 entries of
 * {int16 ax, int16 ay}, ax = ay = the x displacement scaled by level / 256)
 * from phase `selector`; the RippleCheck codes, 0x12 not enough rings */
int BmpMgr_RippleFill(BmpMgr_t* m, uint32_t* table, int no, int selector, int level, int rings);

// fonts and text (forwarders to the font cache, and the drawing routines)
int BmpMgr_FontOpen(BmpMgr_t* m, int* handle, const char* name, int size, int widthPct, int bold); // FontCache_Open: 0 ok, 0x80000002 size, ..3 width, ..4 face
// "91 0F" of 1.529 on: open with the two extra-width values (FontCache_OpenExtra)
int BmpMgr_FontOpenExtra(BmpMgr_t* m, int* handle, const char* name, int size, int widthPct, int bold, int extraUpright,
	int extraItalic);
int BmpMgr_FontInfo(BmpMgr_t* m, FontInfo_t* out, int handle);                                       // FontCache_GetInfo: 1 ok, 0 unknown handle
int BmpMgr_FontRequest(BmpMgr_t* m, const char* name, int size, int widthPct, int bold, int amount); // "90 0E": FontCache_Request
/* "7D": `len` bytes at `data` as "OOOO : xx xx .." lines of 16, drawn
 * with font `handle` at (x, y); the line background is black.  Always 0
 * (also for an unknown font, which draws nothing) */
int BmpMgr_DrawHexDump(BmpMgr_t* m, Bmp_t* dst, int x, int y, const void* data, int len, int handle, uint32_t colour);
/* draw `str` into dst starting at (x, y) with the font `handle`, each
 * glyph blitted with `effect` / `level` (0 / 0 for a plain alpha copy);
 * `proportional` packs glyphs by their ink extent, `wrap` breaks lines at
 * the bitmap's right edge (with one kinsoku character allowed to overhang),
 * spacingPct is the line spacing in percent of the size.  *measure receives
 * the line count when wrapping, else the widest line in pixels.  1 ok, 0
 * unknown font. */
int BmpMgr_DrawTextEx(BmpMgr_t* m, Bmp_t* dst, int* measure, int x, int y, const char* str, int handle,
	uint32_t colour, int effect, int level, int proportional, int wrap, int spacingPct);
// BmpMgr_DrawTextEx without wrapping
int BmpMgr_DrawText(BmpMgr_t* m, Bmp_t* dst, int* measure, int x, int y, const char* str, int handle,
	uint32_t colour, int effect, int level, int proportional);

#endif // BGI_GFX_BMPMGR_H_
