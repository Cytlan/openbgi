/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bmptext.c - text drawn into bitmap slots: the font names and fallbacks, the
 *             text and measuring operations (src/include/bgi/gfx/bmpops.h)
 *
 * The scripts name fonts by number; this file keeps the number-to-face
 * registry and the fallback faces, and the BmpOp_Text* routines open the
 * numbered font in the bitmap manager's cache and draw through it.  A face
 * that is not installed is opened as its fallback ("B0 C7") or as one of
 * the fonts built into the binary (embfonts.h).  Their
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
#include "bgi/fs.h"
#include "bgi/embfonts.h"
#include "bgi/fontfile.h"
#include "bgi/sjis.h"
#include "bgi/vm.h"
#include "bgi/core.h"

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

// the number of `name`, registering it when it is new
static int FontNames_Add(OpenBGI_t* o, const char* name)
{
	FontName_t* n;
	for(n = o->font.fontNames; n; n = n->next)
		if(strcmp(n->name, name) == 0)
			return n->no;
	n = (FontName_t*)BGI_Alloc(sizeof *n);
	n->no = o->font.fontNameCount++;
	n->name = (char*)BGI_Alloc(strlen(name) + 1);
	strcpy(n->name, name);
	n->next = o->font.fontNames;
	o->font.fontNames = n;
	return n->no;
}

// the registered faces' names, all of them (the engine's end)
static void FontFiles_Free(OpenBGI_t* o);
static void FontFallbacks_Free(OpenBGI_t* o);

void FontNames_Free(OpenBGI_t* o)
{
	while(o->font.fontNames)
	{
		FontName_t* n = o->font.fontNames;
		o->font.fontNames = n->next;
		BGI_Free(n->name);
		BGI_Free(n);
	}
	o->font.fontNameCount = 0;
	FontFiles_Free(o);
	FontFallbacks_Free(o);
}

// register the two built-in faces as numbers 0 and 1 once
static void FontNames_Init(OpenBGI_t* o)
{
	if(o->font.fontNames)
		return;
	FontNames_Add(o, MSG_FONT_GOTHIC);
	FontNames_Add(o, MSG_FONT_MINCHO);
}

/* "B0 C0" (kind -1) / "B0 C1": the number of the face `name`, registered
 * when new.  `kind` (0 / 1) selects a charset in the original's face
 * preparation; the faces are resolved by name, so it is unused. */
int FontNames_Register(OpenBGI_t* o, const char* name, int kind)
{
	FontNames_Init(o);
	BGI_UNUSED(kind); // 0 / 1 select a charset in the original's face preparation; the faces are resolved by name
	return FontNames_Add(o, name);
}

// a game's font as data to the typefaces; 1 when they took it (gfx/typeface.c keeps its families)
static int AddFontData(OpenBGI_t* o, const uint8_t* data, uint32_t size)
{
	return Typefaces_AddMemory(o, data, size, 0);
}

// hand the fonts built into the binary to the typefaces, once
static void Embedded_Init(OpenBGI_t* o)
{
	int i;
	if(o->font.embeddedReady)
		return;
	o->font.embeddedReady = 1;
	for(i = 0; i < gEmbeddedFontCount; i++)
	{
		const EmbeddedFont_t* e = &gEmbeddedFonts[i];
		if(e->cls < 0 || e->cls >= EMBFONT_CLASSES || o->font.embeddedUtf8[e->cls][0]) // (the first of a class is the one)
			continue;
		if(!FontNames_AddSubstitute(o, e->data, e->size, e->cls))
			fprintf(stderr, "openbgi: the built-in font %s cannot be used\n", e->file);
	}
}

int FontNames_AddSubstitute(OpenBGI_t* o, const uint8_t* data, uint32_t size, int cls)
{
	char names[1][FONTFILE_NAME_MAX];
	if(cls < 0 || cls >= EMBFONT_CLASSES)
		return 0;
	if(FontFile_Families(data, size, 0, names, 1) < 1 || !Typefaces_AddSubstitute(o, data, size))
		return 0;
	snprintf(o->font.embeddedUtf8[cls], sizeof o->font.embeddedUtf8[0], "%s", names[0]);
	Sjis_FromUtf8(names[0], o->font.embeddedSjis[cls], sizeof o->font.embeddedSjis[0]);
	return 1;
}

// ASCII letters to lower case, in place
static void LowerAscii(char* s)
{
	for(; *s; s++)
		if(*s >= 'A' && *s <= 'Z')
			*s = (char)(*s + 32);
}

/* the class of a face (in the text encoding; EMBFONT_* of embfonts.h):
 * the serif design when its name says Mincho ("明朝") or serif, else the
 * gothic one; the proportional pitch when its name says so the way the
 * fonts of Windows do - a "P" before the design's name ("ＭＳ Ｐゴシック",
 * "MS PGothic", "MS PMincho") or "UI" ("MS UI Gothic") - else the fixed
 * pitch, which is also what Windows gives the engine for a face it does
 * not have.  --stand-in-font=gothic or =mincho has every face stood in
 * for by that design. */
static int FaceClass(OpenBGI_t* o, const char* face)
{
	static const char* const kProportional[] = {
		"\xef\xbc\xb0", // Ｐ (the full-width letter of "ＭＳ Ｐゴシック" and "ＭＳ Ｐ明朝")
		"\xef\xbd\x90", // ｐ
		"pgothic", "pmincho", "p gothic", "p mincho",
		"p\xe3\x82\xb4\xe3\x82\xb7", // Pゴシ(ック)
		"p\xe6\x98\x8e\xe6\x9c\x9d", // P明朝
		"ui gothic",
		"\xef\xbc\xb5\xef\xbc\xa9", // ＵＩ
	};
	char utf[0x200];
	int cls = EMBFONT_GOTHIC;
	size_t i;
	Sjis_ToUtf8(face, utf, sizeof utf, o->enc.textUtf8);
	LowerAscii(utf);
	if(strstr(utf, "\xe6\x98\x8e\xe6\x9c\x9d") || strstr(utf, "mincho") || strstr(utf, "serif")) // 明朝
		cls = EMBFONT_MINCHO;
	if(strstr(utf, "sans")) // ("sans serif" is not a serif face)
		cls = EMBFONT_GOTHIC;
	if(o->engine.optStandIn) // the command line's --stand-in-font= says the design
		cls = o->engine.optStandIn == 2 ? EMBFONT_MINCHO : EMBFONT_GOTHIC;
	for(i = 0; i < sizeof kProportional / sizeof kProportional[0]; i++)
		if(strstr(utf, kProportional[i]))
			return cls | EMBFONT_PGOTHIC;
	return cls;
}

// whether the face itself would be opened
static int FaceInstalled(OpenBGI_t* o, const char* face)
{
	return Typefaces_Installed(o, face);
}

/* From 1.492 on the font files added are remembered by the name the
 * script gave ("B0 C2" and "B0 C3" share the list): adding one again
 * answers 1 without adding it again. */
typedef struct FontFile
{
	char* name;
	struct FontFile* next;
} FontFile_t;

static int FontFiles_Known(OpenBGI_t* o, const char* file)
{
	FontFile_t* f;
	for(f = o->font.fontFiles; f; f = f->next)
		if(strcmp(f->name, file) == 0)
			return 1;
	return 0;
}

static void FontFiles_Remember(OpenBGI_t* o, const char* file)
{
	FontFile_t* f = (FontFile_t*)BGI_Alloc(sizeof *f);
	f->name = BGI_Strdup(file);
	f->next = o->font.fontFiles;
	o->font.fontFiles = f;
}

static void FontFiles_Free(OpenBGI_t* o)
{
	while(o->font.fontFiles)
	{
		FontFile_t* f = (FontFile_t*)o->font.fontFiles;
		o->font.fontFiles = f->next;
		BGI_Free(f->name);
		BGI_Free(f);
	}
}

// "B0 C2": add a font file next to the game to the typefaces; 1 when they took it (or, 1.492 on, had it already)
int FontNames_AddFile(OpenBGI_t* o, const char* file)
{
	char path[0x208];
	int r;
	if(o->engine.profile->gen >= GEN_1_492 && FontFiles_Known(o, file))
		return 1;
	snprintf(path, sizeof path, "%s%s", o->files.baseDir, file);
	r = Typefaces_AddFile(o, path);
	if(r && o->engine.profile->gen >= GEN_1_492)
		FontFiles_Remember(o, file);
	return r;
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

// set (or with NULL clear) the fallback of `face`
void FontNames_SetFallback(OpenBGI_t* o, const char* face, const char* fallback)
{
	FontFallback_t* f;
	for(f = o->font.fontFallbacks; f; f = f->next)
		if(strcmp(f->face, face) == 0)
			break;
	if(!f)
	{
		f = (FontFallback_t*)BGI_Alloc(sizeof *f);
		f->face = (char*)BGI_Alloc(strlen(face) + 1);
		strcpy(f->face, face);
		f->fallback = NULL;
		f->next = o->font.fontFallbacks;
		o->font.fontFallbacks = f;
	}
	BGI_Free(f->fallback);
	f->fallback = NULL;
	if(fallback)
	{
		f->fallback = (char*)BGI_Alloc(strlen(fallback) + 1);
		strcpy(f->fallback, fallback);
	}
}

static void FontFallbacks_Free(OpenBGI_t* o)
{
	while(o->font.fontFallbacks)
	{
		FontFallback_t* f = (FontFallback_t*)o->font.fontFallbacks;
		o->font.fontFallbacks = f->next;
		BGI_Free(f->face);
		BGI_Free(f->fallback);
		BGI_Free(f);
	}
}

/* the face to open: `face` itself when it is installed; else its
 * fallback when one is set and installed; else the font built into the
 * binary of its class (without one: the same design at the other pitch,
 * then the other design at the same pitch, then whatever there is); else
 * the fallback, or the face for the fonts' finder to find something in
 * its place */
const char* FontNames_Resolve(OpenBGI_t* o, const char* face)
{
	FontFallback_t* f;
	const char* fallback = NULL;
	static const int kOrder[4] = {0, EMBFONT_PGOTHIC, EMBFONT_MINCHO, EMBFONT_PGOTHIC | EMBFONT_MINCHO}; // xor'ed onto the class
	int cls, i, any = 0;
	Embedded_Init(o);
	for(i = 0; i < EMBFONT_CLASSES; i++)
		any |= o->font.embeddedUtf8[i][0] != 0;
	if(!o->font.fontFallbacks && !any)
		return face; // nothing to open in its place
	if(FaceInstalled(o, face))
		return face;
	for(f = o->font.fontFallbacks; f; f = f->next)
		if(strcmp(f->face, face) == 0 && f->fallback)
		{
			fallback = f->fallback;
			break;
		}
	if(fallback && FaceInstalled(o, fallback))
		return fallback;
	cls = FaceClass(o, face);
	for(i = 0; i < 4; i++)
	{
		int c = cls ^ kOrder[i];
		if(o->font.embeddedUtf8[c][0])
			return o->enc.textUtf8 ? o->font.embeddedUtf8[c] : o->font.embeddedSjis[c];
	}
	return fallback ? fallback : face;
}

/* "B0 C3" (1.492 on): add a font file of an archive to the typefaces (a
 * NULL archive means the game directory, as "B0 C2"); 1 when they took
 * it or had it already.  The size is asked first, the way "80 35" finds
 * it (a loose copy wins), so an entry that is not there answers 0 instead
 * of the loader's "does not exist" error - a translation patch's boot
 * program names a font that no archive holds to keep the game's own
 * faces out and get the fallbacks; 0 also when the entry cannot be read
 * whole or the typefaces refuse it. */
int FontNames_AddFileFromArc(OpenBGI_t* o, const char* arc, const char* file)
{
	uint8_t* buf;
	uint32_t n, got;
	int r = 0;
	if(!arc)
		return FontNames_AddFile(o, file);
	if(FontFiles_Known(o, file))
		return 1;
	n = GetFileSize_(o, arc, file);
	if(!n)
		return 0;
	buf = (uint8_t*)Vm_TempAlloc(o, n); // the load may ask for the disc
	got = LoadFile(o, buf, arc, file);
	Vm_TempKeep(o, buf);
	if(got == n && AddFontData(o, buf, n))
	{
		FontFiles_Remember(o, file);
		r = 1;
	}
	BGI_Free(buf);
	return r;
}

// the face name of font number `no`, NULL for an unknown number
const char* FontNameByNo(OpenBGI_t* o, int no)
{
	FontName_t* n;
	FontNames_Init(o);
	for(n = o->font.fontNames; n; n = n->next)
		if(n->no == no)
			return n->name;
	return NULL;
}

/* Open font number `fontNo` at `size` pixels, `widthPct` percent width,
 * bold or not, in the manager's font cache; *outHandle receives the cache
 * handle.  0 ok, 0x80000001 bad size, 0x80000002 bad width, 0x80000003
 * unknown font number; the original returns the dead `bold` argument on
 * any other cache result, and so does this code. */
int BmpOp_FontOpen(OpenBGI_t* o, int* outHandle, int fontNo, int size, int widthPct, int bold)
{
	switch((uint32_t)BmpMgr_FontOpen(o, o->gfx.bmpMgr, outHandle, FontNameByNo(o, fontNo), size, widthPct, bold))
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
int BmpOp_Text(OpenBGI_t* o, int bmp, int x, int y, const char* str, int fontNo, int size, int widthPct, int bold, int prop,
	uint32_t colour, int32_t* measure)
{
	Bmp_t d;
	int handle, r;
	if(!BmpMgr_GetInfo(o->gfx.bmpMgr, &d, bmp))
		return (int)0x80000004;
	r = BmpOp_FontOpen(o, &handle, fontNo, size, widthPct, bold);
	if(r == 0)
		BmpMgr_DrawText(o, o->gfx.bmpMgr, &d, measure, x, y, str, handle, colour, 0, 0, prop);
	return r;
}

/* "92 1D": as BmpOp_Text, but wrapped at the bitmap's right edge with a
 * line spacing of `spacingPct` percent of the size; *measure receives the
 * line count. */
int BmpOp_TextEx(OpenBGI_t* o, int bmp, int x, int y, const char* str, int fontNo, int size, int widthPct, int bold, int prop,
	uint32_t colour, int spacingPct, int32_t* measure)
{
	Bmp_t d;
	int handle, r;
	if(!BmpMgr_GetInfo(o->gfx.bmpMgr, &d, bmp))
		return (int)0x80000004;
	r = BmpOp_FontOpen(o, &handle, fontNo, size, widthPct, bold);
	if(r == 0)
		BmpMgr_DrawTextEx(o, o->gfx.bmpMgr, &d, measure, x, y, str, handle, colour, 0, 0, prop, 1, spacingPct);
	return r;
}

/* "92 1E": text through a temporary 1-bit font (MonoFont) with `spacing`
 * extra pixels per character; *outWidth receives the width drawn.
 * 0x80000003 for an unknown font number, 0x80000001 when the size is out
 * of range, 0x80000004 without a bitmap */
int BmpOp_TextMono(OpenBGI_t* o, int bmp, int x, int y, const char* str, int fontNo, int size, int bold, int spacing,
	uint32_t colour, int32_t* outWidth)
{
	Bmp_t d;
	const char* name;
	MonoFont_t* font;
	int r;

	if(!BmpMgr_GetInfo(o->gfx.bmpMgr, &d, bmp))
		return (int)0x80000004;
	name = FontNameByNo(o, fontNo);
	if(!name)
		return (int)0x80000003;
	font = (MonoFont_t*)BGI_Calloc(sizeof(MonoFont_t));
	MonoFont_Ctor(font);
	r = MonoFont_Init(o, font, name, size, bold, 16);
	if(r == 0)
		MonoText_Draw(o, &d, x, y, str, font, spacing, colour, outWidth);
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
int BmpOp_DrawText(OpenBGI_t* o, int bmp, int32_t* outLines, int x, int y, const char* str, int parseTags, const char* tags,
	int fontNo, int size, int widthPct, int bold, int prop, int hang, int spacing, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5])
{
	Bmp_t d;
	int handle, r;
	if(!BmpMgr_GetInfo(o->gfx.bmpMgr, &d, bmp))
		return (int)0x80000004;
	r = BmpOp_FontOpen(o, &handle, fontNo, size, widthPct, bold);
	if(r == 0)
		Text_DrawToBitmap(o, &d, outLines, x, y, str, parseTags, tags, handle, prop, hang, 0, spacing, colour,
			rubyColour, style, 0, 0);
	return r;
}

/* "7D": dump `len` bytes as hex lines into a bitmap, in MS Gothic at
 * `fontSize` (BmpMgr_DrawHexDump); `t` is unused.  0 ok, 0x80000002
 * without a bitmap; a font error is 0x80000001 for a bad size, otherwise
 * the original returns its (unset) handle variable, and so does this
 * code. */
int Vm_DebugText(OpenBGI_t* o, uint32_t bmp, int x, int y, const void* data, uint32_t len, int fontSize, uint32_t colour,
	struct Thread* t)
{
	Bmp_t d;
	int handle = 0, r;
	(void)t;

	if(!BmpMgr_GetInfo(o->gfx.bmpMgr, &d, (int)bmp))
		return (int)0x80000002;
	r = BmpMgr_FontOpen(o, o->gfx.bmpMgr, &handle, FontNameByNo(o, 0), fontSize, 100, 0);
	if(r == 0)
	{
		BmpMgr_DrawHexDump(o, o->gfx.bmpMgr, &d, x, y, data, len, handle, colour);
		return 0;
	}
	if(r == (int)0x80000002)
		return (int)0x80000001;
	return handle; // the original returns the handle variable on other errors
}
