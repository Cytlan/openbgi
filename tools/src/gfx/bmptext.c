/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bmptext.c - text drawn into bitmap slots: the font names and fallbacks, the
 *             text and measuring operations (inc/bgi/gfx/bmpops.h)
 *
 * The scripts name fonts by number; this file keeps the number-to-face
 * registry and the fallback faces, and the BmpOp_Text* routines open the
 * numbered font in the bitmap manager's cache and draw through it.  Their
 * result codes are 0 ok, 0x80000001 bad size, 0x80000002 bad width,
 * 0x80000003 unknown font number, 0x80000004 no bitmap in the slot.
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

/* The font numbers.  Up to 1.69 build 444 two faces exist (0 MS Gothic,
 * 1 MS Mincho).  Build 472 made it a registry: the same two are
 * registered at start-up and "B0 C0" / "B0 C1" add faces by name,
 * returning their numbers; "B0 C2" adds a font file of the game directory
 * to the system first. */
typedef struct FontName
{
	int no;                // the number the scripts use
	char* name;            // the face name
	struct FontName* next; // the registry is a list, newest first
} FontName_t;
static FontName_t* gFontNames;
static int gFontNameCount; // the next number to hand out

// the number of `name`, registering it when it is new
static int FontNames_Add(const char* name)
{
	FontName_t* n;
	for(n = gFontNames; n; n = n->next)
		if(strcmp(n->name, name) == 0)
			return n->no;
	n = (FontName_t*)BGI_Alloc(sizeof *n);
	n->no = gFontNameCount++;
	n->name = (char*)BGI_Alloc(strlen(name) + 1);
	strcpy(n->name, name);
	n->next = gFontNames;
	gFontNames = n;
	return n->no;
}

// register the two built-in faces as numbers 0 and 1 once
static void FontNames_Init(void)
{
	if(gFontNames)
		return;
	FontNames_Add(MSG_FONT_GOTHIC);
	FontNames_Add(MSG_FONT_MINCHO);
}

/* "B0 C0" (kind -1) / "B0 C1": the number of the face `name`, registered
 * when new.  `kind` (0 / 1) selects a charset in the original's face
 * preparation; the OS layer resolves faces by name, so it is unused. */
int FontNames_Register(const char* name, int kind)
{
	FontNames_Init();
	BGI_UNUSED(kind); // 0 / 1 select a charset in the original's face preparation; the OS layer resolves faces by name
	return FontNames_Add(name);
}

// "B0 C2": add a font file next to the game to the system; 1 when the system took it
int FontNames_AddFile(const char* file)
{
	char path[0x208];
	snprintf(path, sizeof path, "%s%s", gBaseDir, file);
	return OS_FontAddFile(path);
}

/* "B0 C7" of 1.535 on: the face to use when a face is
 * not installed (that build compares the family name the font system
 * actually opened and retries with the fallback) */
typedef struct FontFallback
{
	char* face;                // the face the scripts ask for
	char* fallback;            // the face to open instead; NULL = none
	struct FontFallback* next; // a list, newest first
} FontFallback_t;
static FontFallback_t* gFontFallbacks;

// set (or with NULL clear) the fallback of `face`
void FontNames_SetFallback(const char* face, const char* fallback)
{
	FontFallback_t* f;
	for(f = gFontFallbacks; f; f = f->next)
		if(strcmp(f->face, face) == 0)
			break;
	if(!f)
	{
		f = (FontFallback_t*)BGI_Alloc(sizeof *f);
		f->face = (char*)BGI_Alloc(strlen(face) + 1);
		strcpy(f->face, face);
		f->fallback = NULL;
		f->next = gFontFallbacks;
		gFontFallbacks = f;
	}
	BGI_Free(f->fallback);
	f->fallback = NULL;
	if(fallback)
	{
		f->fallback = (char*)BGI_Alloc(strlen(fallback) + 1);
		strcpy(f->fallback, fallback);
	}
}

// the face to open: `face` itself, or its fallback when it is not installed and one is set
const char* FontNames_Resolve(const char* face)
{
	FontFallback_t* f;
	if(!gFontFallbacks || OS_FontFaceExists(face))
		return face;
	for(f = gFontFallbacks; f; f = f->next)
		if(strcmp(f->face, face) == 0 && f->fallback)
			return f->fallback;
	return face;
}

/* "B0 C3" (1.494 on): add a font file of an archive to the system (a NULL
 * archive means the game directory, as "B0 C2"); 1 when the system took
 * it, 0 when the file is missing or refused */
int FontNames_AddFileFromArc(const char* arc, const char* file)
{
	uint8_t* buf;
	uint32_t n;
	int r;
	if(!arc)
		return FontNames_AddFile(file);
	buf = (uint8_t*)BGI_Alloc(0x2000000);
	n = LoadFile(buf, arc, file);
	r = n ? OS_FontAddMemory(buf, n) : 0;
	BGI_Free(buf);
	return r;
}

// the face name of font number `no`, NULL for an unknown number
const char* FontNameByNo(int no)
{
	FontName_t* n;
	FontNames_Init();
	for(n = gFontNames; n; n = n->next)
		if(n->no == no)
			return n->name;
	return NULL;
}

/* Open font number `fontNo` at `size` pixels, `widthPct` percent width,
 * bold or not, in the manager's font cache; *outHandle receives the cache
 * handle.  0 ok, 0x80000001 bad size, 0x80000002 bad width, 0x80000003
 * unknown font number; the original returns the dead `bold` argument on
 * any other cache result, and so does this code. */
int BmpOp_FontOpen(int* outHandle, int fontNo, int size, int widthPct, int bold)
{
	switch((uint32_t)BmpMgr_FontOpen(gBmpMgr, outHandle, FontNameByNo(fontNo), size, widthPct, bold))
	{
		case 0: return 0;
		case 0x80000002u: return (int)0x80000001;
		case 0x80000003u: return (int)0x80000002;
		case 0x80000004u: return (int)0x80000003;
		default: return bold; // the original returns the dead `bold` argument here
	}
}

/* "92 1C": draw `str` unwrapped into the bitmap at (x, y) with the
 * numbered font (`prop` packs the glyphs by their ink extent); *measure
 * receives the widest line in pixels.  The BmpOp_FontOpen codes,
 * 0x80000004 without a bitmap. */
int BmpOp_Text(int bmp, int x, int y, const char* str, int fontNo, int size, int widthPct, int bold, int prop,
	uint32_t colour, int32_t* measure)
{
	Bmp_t d;
	int handle, r;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, bmp))
		return (int)0x80000004;
	r = BmpOp_FontOpen(&handle, fontNo, size, widthPct, bold);
	if(r == 0)
		BmpMgr_DrawText(gBmpMgr, &d, measure, x, y, str, handle, colour, 0, 0, prop);
	return r;
}

/* "92 1D": as BmpOp_Text, but wrapped at the bitmap's right edge with a
 * line spacing of `spacingPct` percent of the size; *measure receives the
 * line count. */
int BmpOp_TextEx(int bmp, int x, int y, const char* str, int fontNo, int size, int widthPct, int bold, int prop,
	uint32_t colour, int spacingPct, int32_t* measure)
{
	Bmp_t d;
	int handle, r;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, bmp))
		return (int)0x80000004;
	r = BmpOp_FontOpen(&handle, fontNo, size, widthPct, bold);
	if(r == 0)
		BmpMgr_DrawTextEx(gBmpMgr, &d, measure, x, y, str, handle, colour, 0, 0, prop, 1, spacingPct);
	return r;
}

/* "92 1E": text through a temporary 1-bit font (MonoFont) with `spacing`
 * extra pixels per character; *outWidth receives the width drawn.
 * 0x80000003 for an unknown font number, 0x80000001 when the size is out
 * of range, 0x80000004 without a bitmap */
int BmpOp_TextMono(int bmp, int x, int y, const char* str, int fontNo, int size, int bold, int spacing,
	uint32_t colour, int32_t* outWidth)
{
	Bmp_t d;
	const char* name;
	MonoFont_t* font;
	int r;

	if(!BmpMgr_GetInfo(gBmpMgr, &d, bmp))
		return (int)0x80000004;
	name = FontNameByNo(fontNo);
	if(!name)
		return (int)0x80000003;
	font = (MonoFont_t*)BGI_Calloc(sizeof(MonoFont_t));
	MonoFont_Ctor(font);
	r = MonoFont_Init(font, name, size, bold, 16);
	if(r == 0)
		MonoText_Draw(&d, x, y, str, font, spacing, colour, outWidth);
	else if(r == (int)0x80000002)
		r = (int)0x80000001;
	MonoFont_Dtor(font);
	BGI_Free(font);
	return r;
}

/* "91 9C" / "91 9D" / "92 9C": the rich text layout (Text_DrawToBitmap:
 * markup, ruby, hanging punctuation, the shadow style) into a bitmap with
 * no swing, effect 0 and level 0; *outLines receives the line count.  The
 * BmpOp_Text codes. */
int BmpOp_DrawText(int bmp, int32_t* outLines, int x, int y, const char* str, int parseTags, const char* tags,
	int fontNo, int size, int widthPct, int bold, int prop, int hang, int spacing, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5])
{
	Bmp_t d;
	int handle, r;
	if(!BmpMgr_GetInfo(gBmpMgr, &d, bmp))
		return (int)0x80000004;
	r = BmpOp_FontOpen(&handle, fontNo, size, widthPct, bold);
	if(r == 0)
		Text_DrawToBitmap(&d, outLines, x, y, str, parseTags, tags, handle, prop, hang, 0, spacing, colour,
			rubyColour, style, 0, 0);
	return r;
}

/* "7D": dump `len` bytes as hex lines into a bitmap, in MS Gothic at
 * `fontSize` (BmpMgr_DrawHexDump); `t` is unused.  0 ok, 0x80000002
 * without a bitmap; a font error is 0x80000001 for a bad size, otherwise
 * the original returns its (unset) handle variable, and so does this
 * code. */
int Vm_DebugText(uint32_t bmp, int x, int y, const void* data, uint32_t len, int fontSize, uint32_t colour,
	struct Thread* t)
{
	Bmp_t d;
	int handle = 0, r;
	(void)t;

	if(!BmpMgr_GetInfo(gBmpMgr, &d, (int)bmp))
		return (int)0x80000002;
	r = BmpMgr_FontOpen(gBmpMgr, &handle, FontNameByNo(0), fontSize, 100, 0);
	if(r == 0)
	{
		BmpMgr_DrawHexDump(gBmpMgr, &d, x, y, data, len, handle, colour);
		return 0;
	}
	if(r == (int)0x80000002)
		return (int)0x80000001;
	return handle; // the original returns the handle variable on other errors
}
