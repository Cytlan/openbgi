/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ftfont.c - glyphs with GDI's sizing, rasterised by FreeType
 *            (src/include/bgi/ftfont.h)
 *
 * The engine asks for GDI fonts by face name ("ＭＳ ゴシック" = MS Gothic,
 * "ＭＳ 明朝" = MS Mincho), a cell height and the average (half-width)
 * character width, and expects each character drawn at the top-left corner
 * of a buffer the way TextOut() does: the cell top at row 0, the baseline
 * at the font's ascent, full-width glyphs twice the given width.
 *
 * One FT_Face is shared by every font of a library opened on the same file
 * (or data) and face index; each FtFont owns an FT_Size, so sizes do not
 * interfere.  A character the face has no glyph for is looked up in
 * another font (the host's findChar), scaled to the same cell and put on
 * the same baseline.  GDI does much the same for the scripts its fonts
 * lack; here it is what lets Arabic or Hebrew text appear in a window
 * whose font is a Japanese one.
 *
 * GDI draws into an 8-bit palette DIB in the original, where it never
 * antialiases, so FtFont_Render rasterises in black and white; the engine
 * does its own oversampling and filtering (src/gfx/font.c).  The host's
 * own windows draw antialiased through FtFont_Glyph.
 *
 * An engine's library reads the font files through its host's files (a
 * FreeType stream over Host_NativeOpen); a library of the back ends' own
 * has FreeType open them.
 */
#include "bgi/ftfont.h"
#include "bgi/fontfile.h"
#include "bgi/host.h"

#ifdef BGI_HAVE_FREETYPE

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include FT_SIZES_H
#include FT_SYNTHESIS_H
#include FT_TRUETYPE_TABLES_H

// ---- the library ----------------------------------------------------------------

// one FT_Face, shared by every font of the library opened on the same file or data and face index
typedef struct FaceRec
{
	struct FaceRec* next; // the library's list
	char path[1024];      // the font file; "" for data
	const void* data;     // the data of a font in memory
	int index;            // the face within it (a .ttc holds several)
	FT_Face face;
	int refs; // the fonts using it; freed when the last one closes
} FaceRec_t;

#define FALLBACK_MAX  4 // other fonts one font may borrow glyphs from
#define FALLBACK_MISS 8 // character blocks remembered as having no fallback

typedef struct Fallback // another font that has glyphs the face lacks
{
	FaceRec_t* rec; // the shared face
	FT_Size size;   // scaled to this font's cell
} Fallback_t;

struct FtFont
{
	FtFonts_t* lib;
	struct FtFont* next;         // the library's open fonts
	FaceRec_t* rec;              // the shared face
	FT_Size size;                // this font's scaling of the shared face
	Fallback_t fb[FALLBACK_MAX]; // the fallback fonts opened so far
	int fbCount;
	uint32_t fbMiss[FALLBACK_MISS]; // blocks (code point >> 8) no font was found for: not searched again
	int fbMissCount;
	int height, width;   // as requested (pixels)
	int bold;            // synthetic emboldening when the face is not bold itself
	int italic;          // as requested
	int oblique;         // shear the outlines: the face itself is not italic
	int serif;           // the face name asked for the serif (Mincho) class: fallback fonts follow it
	int ascent;          // baseline row inside the cell
	FT_F26Dot6 emW, emH; // the character size handed to FreeType
	FT_Pos boldStrength; // 26.6 horizontal emboldening, 0 = none
};

#define MEMFONT_NAMES 16 // family names kept per font

// a font handed over as data
typedef struct MemFont
{
	struct MemFont* next;
	const uint8_t* data;
	uint8_t* owned; // the library's copy of the data, NULL when the data is the caller's
	uint32_t size;
	int count;                                   // names
	int face[MEMFONT_NAMES];                     // the face of a collection each name belongs to
	char name[MEMFONT_NAMES][FONTFILE_NAME_MAX]; // UTF-8
} MemFont_t;

struct FtFonts
{
	FT_Library ft;
	FaceRec_t* faces;                 // the open faces
	MemFont_t* mem;                   // the fonts handed over as data, newest first
	FtFont_t* fonts;                  // the open fonts
	const OpenBGI_FontFiles_t* files; // the host's font files, NULL for none
	void* user;
	OpenBGI_t* o; // the engine whose host's files the font files are read through; NULL: FreeType opens them
};

FtFonts_t* FtFonts_New(OpenBGI_t* o, const OpenBGI_FontFiles_t* files, void* user)
{
	FtFonts_t* l = (FtFonts_t*)calloc(1, sizeof *l);
	if(!l)
		return NULL;
	if(FT_Init_FreeType(&l->ft) != 0)
	{
		free(l);
		return NULL;
	}
	l->files = files;
	l->user = user;
	l->o = o;
	return l;
}

void FtFonts_Delete(FtFonts_t* l)
{
	if(!l)
		return;
	while(l->fonts)
		FtFont_Close(l->fonts);
	FT_Done_FreeType(l->ft); // (with any face still open)
	while(l->faces)
	{
		FaceRec_t* r = l->faces;
		l->faces = r->next;
		free(r);
	}
	while(l->mem) // (after the faces that read them)
	{
		MemFont_t* m = l->mem;
		l->mem = m->next;
		free(m->owned);
		free(m);
	}
	free(l);
}

// ---- names ----------------------------------------------------------------------

static int LowerAscii(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

// two names equal, ASCII letters in either case
static int EqualNoCase(const char* a, const char* b)
{
	for(; *a && LowerAscii((uint8_t)*a) == LowerAscii((uint8_t)*b); a++, b++)
	{
	}
	return LowerAscii((uint8_t)*a) == LowerAscii((uint8_t)*b);
}

// `sub` in `s`, ASCII letters in either case
static int ContainsNoCase(const char* s, const char* sub)
{
	size_t n = strlen(sub), i;
	for(; *s; s++)
	{
		for(i = 0; i < n && s[i] && LowerAscii((uint8_t)s[i]) == LowerAscii((uint8_t)sub[i]); i++)
		{
		}
		if(i == n)
			return 1;
	}
	return 0;
}

/* a name containing "明朝" (Mincho, the serif family), "mincho" or "serif"
 * picks the serif class, everything else the sans (gothic) class */
static int IsSerifName(const char* utf8)
{
	if(strstr(utf8, "\xe6\x98\x8e\xe6\x9c\x9d") != NULL) // 明朝
		return 1;
	return ContainsNoCase(utf8, "mincho") || ContainsNoCase(utf8, "serif");
}

// ---- fonts in memory -------------------------------------------------------------

int FtFonts_AddMemory(FtFonts_t* l, const void* data, uint32_t size, int copy)
{
	MemFont_t* m;
	int faces = FontFile_Faces((const uint8_t*)data, size), i;
	if(!l || !faces || !(m = (MemFont_t*)calloc(1, sizeof *m)))
		return 0;
	for(i = 0; i < faces && m->count < MEMFONT_NAMES; i++)
	{
		char names[MEMFONT_NAMES][FONTFILE_NAME_MAX];
		int n = FontFile_Families((const uint8_t*)data, size, i, names, MEMFONT_NAMES), k;
		for(k = 0; k < n && k < MEMFONT_NAMES && m->count < MEMFONT_NAMES; k++, m->count++)
		{
			m->face[m->count] = i;
			memcpy(m->name[m->count], names[k], FONTFILE_NAME_MAX);
		}
	}
	if(m->count && copy)
	{
		m->owned = (uint8_t*)malloc(size ? size : 1);
		if(m->owned)
			memcpy(m->owned, data, size);
	}
	if(!m->count || (copy && !m->owned))
	{
		free(m);
		return 0;
	}
	m->data = copy ? m->owned : (const uint8_t*)data;
	m->size = size;
	m->next = l->mem;
	l->mem = m;
	return 1;
}

// the font in memory that has the family: its data and face into `out`; 0 when none has
static int ResolveMemory(const FtFonts_t* l, const char* family, OpenBGI_FontFile_t* out)
{
	const MemFont_t* m;
	int i;
	for(m = l->mem; m; m = m->next)
		for(i = 0; i < m->count; i++)
			if(EqualNoCase(m->name[i], family))
			{
				out->path[0] = 0;
				out->data = m->data;
				out->size = m->size;
				out->index = m->face[i];
				return 1;
			}
	return 0;
}

int FtFonts_HasFamily(const FtFonts_t* l, const char* family)
{
	OpenBGI_FontFile_t file;
	return l && ResolveMemory(l, family, &file);
}

/* the file (or data) to open for a face: a file the host forces, then a
 * font in memory, then the host's match; 0 when there is none */
static int ResolveFace(const FtFonts_t* l, const char* face, int bold, int italic, OpenBGI_FontFile_t* out)
{
	OpenBGI_FontFile_t found;
	int r = 0;
	memset(&found, 0, sizeof found);
	if(l->files && l->files->find)
		r = l->files->find(l->user, face, IsSerifName(face), bold, italic, &found);
	if(r == OPENBGI_FONT_FORCED)
	{
		*out = found;
		return 1;
	}
	if(ResolveMemory(l, face, out))
		return 1;
	if(r == OPENBGI_FONT_FOUND)
	{
		*out = found;
		return 1;
	}
	return 0;
}

// ---- shared faces ----------------------------------------------------------------

/* a variable font (the Noto Sans CJK of Haiku is one) opened without a
 * named instance has its default coordinates, which need not be the
 * regular weight: the weight axis goes to 400, the others keep theirs */
static void SetRegularWeight(FtFonts_t* l, FT_Face face)
{
	FT_MM_Var* mm;
	FT_Fixed coords[16];
	FT_UInt i;
	if(FT_Get_MM_Var(face, &mm) != 0)
		return;
	if(mm->num_axis <= 16)
	{
		for(i = 0; i < mm->num_axis; i++)
		{
			coords[i] = mm->axis[i].def;
			if(mm->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't'))
				coords[i] = BGI_CLAMP((FT_Fixed)400 << 16, mm->axis[i].minimum, mm->axis[i].maximum);
		}
		FT_Set_Var_Design_Coordinates(face, mm->num_axis, coords);
	}
	FT_Done_MM_Var(l->ft, mm);
}

// a font file as FreeType reads it: a stream of the host's
typedef struct HostStream
{
	FT_StreamRec rec; // what FreeType is handed
	OpenBGI_t* o;
	HostFile_t* f;
} HostStream_t;

// FreeType's read: `count` bytes at `offset` (0 bytes: a seek alone, 0 when it succeeded)
static unsigned long HostStreamRead(FT_Stream s, unsigned long offset, unsigned char* buf, unsigned long count)
{
	HostStream_t* h = (HostStream_t*)s->descriptor.pointer;
	if(!Host_FileSeek(h->o, h->f, offset))
		return count ? 0 : 1;
	return count ? Host_FileRead(h->o, h->f, buf, (uint32_t)count) : 0;
}

// FreeType's close, when the face is done with (and when it could not be loaded)
static void HostStreamClose(FT_Stream s)
{
	HostStream_t* h = (HostStream_t*)s->descriptor.pointer;
	Host_FileClose(h->o, h->f);
	free(h);
}

// a face of a native file read through the engine's host; 0 when it does not open or load
static int HostFace(FtFonts_t* l, const char* path, int index, FT_Face* face)
{
	HostFile_t* f = Host_NativeOpen(l->o, path, OPENBGI_OPEN_READ);
	uint64_t size = f ? Host_FileSize(l->o, f) : 0;
	HostStream_t* h = f && size && size < 0x7fffffffu ? (HostStream_t*)calloc(1, sizeof *h) : NULL;
	FT_Open_Args args;
	if(!h)
	{
		Host_FileClose(l->o, f);
		return 0;
	}
	h->o = l->o;
	h->f = f;
	h->rec.size = (unsigned long)size;
	h->rec.descriptor.pointer = h;
	h->rec.read = HostStreamRead;
	h->rec.close = HostStreamClose;
	memset(&args, 0, sizeof args);
	args.flags = FT_OPEN_STREAM;
	args.stream = &h->rec;
	return FT_Open_Face(l->ft, &args, index, face) == 0; // (FreeType closes the stream when it fails)
}

// the shared face of a file (or data) and index: another reference to an open one, or a newly loaded one; NULL when FreeType cannot load it
static FaceRec_t* FaceAcquire(FtFonts_t* l, const OpenBGI_FontFile_t* file)
{
	FaceRec_t* r;
	for(r = l->faces; r; r = r->next)
	{
		if(r->index == file->index && (file->path[0] ? strcmp(r->path, file->path) == 0 : !r->path[0] && r->data == file->data))
		{
			r->refs++;
			return r;
		}
	}
	r = (FaceRec_t*)calloc(1, sizeof *r);
	if(!r)
		return NULL;
	if(file->path[0] ? (l->o ? !HostFace(l, file->path, file->index, &r->face) : FT_New_Face(l->ft, file->path, file->index, &r->face) != 0)
					 : !file->data || FT_New_Memory_Face(l->ft, (const FT_Byte*)file->data, (FT_Long)file->size, file->index, &r->face) != 0)
	{
		free(r);
		return NULL;
	}
	// a Unicode charmap is what the Shift-JIS table maps into; any charmap is better than none
	if(FT_Select_Charmap(r->face, FT_ENCODING_UNICODE) != 0 && r->face->num_charmaps > 0)
		FT_Set_Charmap(r->face, r->face->charmaps[0]);
	if(FT_HAS_MULTIPLE_MASTERS(r->face) && (file->index >> 16) == 0) // (fontconfig's index carries the instance it chose)
		SetRegularWeight(l, r->face);
	snprintf(r->path, sizeof r->path, "%s", file->path);
	r->data = file->path[0] ? NULL : file->data;
	r->index = file->index;
	r->refs = 1;
	r->next = l->faces;
	l->faces = r;
	return r;
}

// drop a reference; the face is unloaded and unlisted with the last one
static void FaceRelease(FtFonts_t* l, FaceRec_t* r)
{
	FaceRec_t** pp;
	if(--r->refs > 0)
		return;
	for(pp = &l->faces; *pp; pp = &(*pp)->next)
	{
		if(*pp == r)
		{
			*pp = r->next;
			break;
		}
	}
	FT_Done_Face(r->face);
	free(r);
}

// ---- opening ---------------------------------------------------------------------

/* GDI's cell is usWinAscent + usWinDescent of the OS/2 table (the hhea
 * values when there is none); the font is scaled so that this sum is
 * `height` pixels and the baseline sits at the scaled ascent */
static void CellMetrics(FT_Face face, FT_Pos* ascUnits, FT_Pos* descUnits)
{
	const TT_OS2* os2 = (const TT_OS2*)FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
	if(os2 && (os2->usWinAscent || os2->usWinDescent))
	{
		*ascUnits = os2->usWinAscent;
		*descUnits = os2->usWinDescent;
	}
	else
	{
		*ascUnits = face->ascender;
		*descUnits = -face->descender;
	}
	if(*ascUnits + *descUnits <= 0)
	{
		*ascUnits = face->units_per_EM;
		*descUnits = 0;
	}
}

/* scaled so that the GDI cell is `height` pixels and a full-width glyph
 * 2 * width (the em square is stretched when the two disagree), with
 * synthetic bold and italic when the face has none of its own */
FtFont_t* FtFont_Open(FtFonts_t* l, const char* face, int height, int width, int bold, int italic)
{
	OpenBGI_FontFile_t file;
	FtFont_t* f;
	FT_Face ft;
	FT_Pos asc, desc, cell, fullAdv;
	FT_UInt gi;

	if(!l || height <= 0)
		return NULL;
	if(!ResolveFace(l, face, bold, italic, &file))
		return NULL;
	f = (FtFont_t*)calloc(1, sizeof *f);
	if(!f)
		return NULL;
	f->rec = FaceAcquire(l, &file);
	if(!f->rec)
	{
		free(f);
		return NULL;
	}
	ft = f->rec->face;
	f->lib = l;
	f->height = height;
	f->width = width;
	f->bold = bold;
	f->italic = italic;
	f->serif = IsSerifName(face);

	CellMetrics(ft, &asc, &desc);
	cell = asc + desc;
	// vertical: em size such that ascent + descent == height
	f->emH = (FT_F26Dot6)(((int64_t)height * 64 * ft->units_per_EM + cell / 2) / cell);
	f->ascent = (int)(((int64_t)height * asc + cell / 2) / cell);
	/* horizontal: a full-width glyph (U+3042 "あ" as the specimen) must be
	 * 2 * width pixels wide; a font without one is assumed to have
	 * half-width characters of half an em */
	fullAdv = 0;
	gi = FT_Get_Char_Index(ft, 0x3042);
	if(gi && FT_Load_Glyph(ft, gi, FT_LOAD_NO_SCALE) == 0)
		fullAdv = ft->glyph->advance.x;
	if(fullAdv <= 0)
		fullAdv = ft->units_per_EM;
	if(width > 0)
		f->emW = (FT_F26Dot6)(((int64_t)width * 2 * 64 * ft->units_per_EM + fullAdv / 2) / fullAdv);
	else
		f->emW = f->emH; // nWidth 0: GDI picks the natural width

	if(FT_New_Size(ft, &f->size) != 0)
	{
		FaceRelease(l, f->rec);
		free(f);
		return NULL;
	}
	FT_Activate_Size(f->size);
	if(FT_Set_Char_Size(ft, f->emW, f->emH, 72, 72) != 0)
	{
		FT_Done_Size(f->size);
		FaceRelease(l, f->rec);
		free(f);
		return NULL;
	}
	/* FW_BOLD on a face that is not bold: GDI overstrikes by about one
	 * pixel; emulated by emboldening the outline horizontally by 1/24 em, at
	 * least one pixel */
	if(bold && !(ft->style_flags & FT_STYLE_FLAG_BOLD))
	{
		f->boldStrength = f->emH / 24;
		if(f->boldStrength < 64)
			f->boldStrength = 64;
	}
	// lfItalic on an upright face: GDI shears the glyphs; FreeType's
	// synthetic oblique (12 degrees) does the same
	f->oblique = italic && !(ft->style_flags & FT_STYLE_FLAG_ITALIC);
	f->next = l->fonts;
	l->fonts = f;
	return f;
}

// release the size and the face reference (and those of the fallback fonts)
void FtFont_Close(FtFont_t* f)
{
	FtFont_t** pp;
	int i;
	if(!f)
		return;
	for(pp = &f->lib->fonts; *pp; pp = &(*pp)->next)
	{
		if(*pp == f)
		{
			*pp = f->next;
			break;
		}
	}
	for(i = 0; i < f->fbCount; i++)
	{
		FT_Done_Size(f->fb[i].size);
		FaceRelease(f->lib, f->fb[i].rec);
	}
	FT_Done_Size(f->size);
	FaceRelease(f->lib, f->rec);
	free(f);
}

int FtFont_Height(const FtFont_t* f)
{
	return f ? f->height : 0;
}

// ---- fallback fonts ---------------------------------------------------------------

/* the face and size to draw `cp` with when the font's own face has no
 * glyph for it: one of the fallbacks already open, or a newly found one,
 * scaled so that its cell is as tall as the font's and stretched like it.
 * 0 when no font has the character (remembered per block of 256). */
static int FallbackFor(FtFont_t* f, uint32_t cp, FT_Face* outFace, FT_Size* outSize)
{
	FtFonts_t* l = f->lib;
	OpenBGI_FontFile_t file;
	int i;
	FaceRec_t* rec;
	FT_Size size;
	FT_Pos asc, desc, cell;
	FT_F26Dot6 emH, emW;

	for(i = 0; i < f->fbCount; i++)
	{
		if(FT_Get_Char_Index(f->fb[i].rec->face, cp))
		{
			*outFace = f->fb[i].rec->face;
			*outSize = f->fb[i].size;
			return 1;
		}
	}
	for(i = 0; i < f->fbMissCount; i++)
		if(f->fbMiss[i] == cp >> 8)
			return 0;
	rec = NULL;
	memset(&file, 0, sizeof file);
	if(f->fbCount < FALLBACK_MAX && l->files && l->files->findChar && l->files->findChar(l->user, cp, f->serif, f->bold, &file))
		rec = FaceAcquire(l, &file);
	if(rec && (rec == f->rec || FT_Get_Char_Index(rec->face, cp) == 0))
	{ // the font itself again, or a file without the glyph after all
		FaceRelease(l, rec);
		rec = NULL;
	}
	if(rec && FT_New_Size(rec->face, &size) != 0)
	{
		FaceRelease(l, rec);
		rec = NULL;
	}
	if(!rec)
	{
		if(f->fbMissCount < FALLBACK_MISS)
			f->fbMiss[f->fbMissCount++] = cp >> 8;
		return 0;
	}
	CellMetrics(rec->face, &asc, &desc);
	cell = asc + desc;
	emH = (FT_F26Dot6)(((int64_t)f->height * 64 * rec->face->units_per_EM + cell / 2) / cell);
	emW = (FT_F26Dot6)((int64_t)emH * f->emW / (f->emH ? f->emH : 1)); // as wide or narrow as the font itself
	FT_Activate_Size(size);
	FT_Set_Char_Size(rec->face, emW, emH, 72, 72);
	f->fb[f->fbCount].rec = rec;
	f->fb[f->fbCount].size = size;
	f->fbCount++;
	*outFace = rec->face;
	*outSize = size;
	return 1;
}

// ---- rendering --------------------------------------------------------------------

/* load, optionally embolden and shear, and render one glyph with the
 * font's size active (mono or 8-bit coverage as `mode` says), from the
 * font's face or - for a character it lacks - a fallback font's.  The
 * glyph slot holding the result, NULL when the glyph cannot be loaded or
 * rendered. */
static FT_GlyphSlot LoadGlyph(FtFont_t* f, uint32_t cp, FT_Render_Mode mode)
{
	FT_Face ft = f->rec->face;
	FT_Size size = f->size;
	FT_UInt gi = FT_Get_Char_Index(ft, cp);
	FT_Int32 flags = FT_LOAD_NO_BITMAP | (mode == FT_RENDER_MODE_MONO ? FT_LOAD_TARGET_MONO : FT_LOAD_TARGET_NORMAL);
	if(gi == 0 && cp > 0x20 && FallbackFor(f, cp, &ft, &size))
		gi = FT_Get_Char_Index(ft, cp);
	FT_Activate_Size(size);
	if(gi == 0 && cp != 0x20)
		gi = FT_Get_Char_Index(ft, 0x25a1); // "□" for an unmapped character, as GDI shows a box
	if(FT_Load_Glyph(ft, gi, flags) != 0)
		return NULL;
	if(f->boldStrength && ft->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
		FT_Outline_EmboldenXY(&ft->glyph->outline, f->boldStrength, 0);
	if(f->oblique && ft->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
		FT_GlyphSlot_Oblique(ft->glyph);
	return FT_Render_Glyph(ft->glyph, mode) == 0 ? ft->glyph : NULL;
}

int FtFont_Glyph(FtFont_t* f, uint32_t cp, int antialiased, FtGlyph_t* out)
{
	FT_GlyphSlot g;
	if(!f || !(g = LoadGlyph(f, cp, antialiased ? FT_RENDER_MODE_NORMAL : FT_RENDER_MODE_MONO)))
		return 0;
	out->bits = g->bitmap.buffer;
	out->pitch = g->bitmap.pitch;
	out->width = (int)g->bitmap.width;
	out->rows = (int)g->bitmap.rows;
	out->x = g->bitmap_left;
	out->y = f->ascent - g->bitmap_top; // the bitmap's top row relative to the cell top
	out->advance = (int)g->advance.x;
	out->mono = !antialiased;
	return 1;
}

/* clear the buffer and draw the character in monochrome (0 / 255), its
 * cell top-left at the buffer's: the glyph's bitmap is placed by its
 * bearing and the font's ascent, and clipped to w x h */
void FtFont_Render(FtFont_t* f, uint32_t cp, uint8_t* buf, int pitch, int w, int h)
{
	FT_GlyphSlot g;
	int x0, y0, y, x;
	int r;
	for(r = 0; r < h; r++)
		memset(buf + (size_t)r * (size_t)pitch, 0, (size_t)w);
	if(!f || !(g = LoadGlyph(f, cp, FT_RENDER_MODE_MONO)))
		return;
	x0 = g->bitmap_left;
	y0 = f->ascent - g->bitmap_top; // the bitmap's top row relative to the cell top
	for(y = 0; y < (int)g->bitmap.rows; y++)
	{
		const uint8_t* row = g->bitmap.buffer + (size_t)y * (size_t)g->bitmap.pitch;
		int dy = y0 + y;
		if(dy < 0 || dy >= h)
			continue;
		for(x = 0; x < (int)g->bitmap.width; x++)
		{
			int dx = x0 + x;
			if(dx < 0 || dx >= w)
				continue;
			if(row[x >> 3] & (0x80 >> (x & 7)))
				buf[(size_t)dy * (size_t)pitch + dx] = 0xff;
		}
	}
}

void FtFont_RenderMono(FtFont_t* f, uint32_t cp, uint8_t* bits, int pitch, int w, int h)
{
	FT_GlyphSlot g;
	int x0, y0, y, x;
	memset(bits, 0, (size_t)pitch * (size_t)h);
	if(!f || !(g = LoadGlyph(f, cp, FT_RENDER_MODE_MONO)))
		return;
	x0 = g->bitmap_left;
	y0 = f->ascent - g->bitmap_top;
	for(y = 0; y < (int)g->bitmap.rows; y++)
	{
		const uint8_t* row = g->bitmap.buffer + (size_t)y * (size_t)g->bitmap.pitch;
		int dy = y0 + y;
		if(dy < 0 || dy >= h)
			continue;
		for(x = 0; x < (int)g->bitmap.width; x++)
		{
			int dx = x0 + x;
			if(dx < 0 || dx >= w)
				continue;
			if(row[x >> 3] & (0x80 >> (x & 7)))
				bits[(size_t)dy * (size_t)pitch + (dx >> 3)] |= (uint8_t)(0x80 >> (dx & 7));
		}
	}
}

// the glyph's advance rounded to whole pixels
int FtFont_Advance(FtFont_t* f, uint32_t cp)
{
	FT_GlyphSlot g;
	if(!f || !(g = LoadGlyph(f, cp, FT_RENDER_MODE_MONO)))
		return 0;
	return (int)((g->advance.x + 32) >> 6);
}

int FtFont_FaceAdvance(FtFont_t* f, uint32_t cp)
{
	FT_Face ft;
	if(!f)
		return -1;
	ft = f->rec->face;
	FT_Activate_Size(f->size);
	if(FT_Load_Glyph(ft, FT_Get_Char_Index(ft, cp), FT_LOAD_NO_BITMAP | FT_LOAD_DEFAULT) != 0)
		return -1;
	return (int)ft->glyph->advance.x;
}

#else // !BGI_HAVE_FREETYPE: no rasteriser; a library cannot be made, so nothing below is reached

FtFonts_t* FtFonts_New(OpenBGI_t* o, const OpenBGI_FontFiles_t* files, void* user)
{
	BGI_UNUSED(o);
	BGI_UNUSED(files);
	BGI_UNUSED(user);
	return NULL;
}

void FtFonts_Delete(FtFonts_t* l)
{
	BGI_UNUSED(l);
}

int FtFonts_AddMemory(FtFonts_t* l, const void* data, uint32_t size, int copy)
{
	BGI_UNUSED(l);
	BGI_UNUSED(data);
	BGI_UNUSED(size);
	BGI_UNUSED(copy);
	return 0;
}

int FtFonts_HasFamily(const FtFonts_t* l, const char* family)
{
	BGI_UNUSED(l);
	BGI_UNUSED(family);
	return 0;
}

FtFont_t* FtFont_Open(FtFonts_t* l, const char* face, int height, int width, int bold, int italic)
{
	BGI_UNUSED(l);
	BGI_UNUSED(face);
	BGI_UNUSED(height);
	BGI_UNUSED(width);
	BGI_UNUSED(bold);
	BGI_UNUSED(italic);
	return NULL;
}

void FtFont_Close(FtFont_t* f)
{
	BGI_UNUSED(f);
}

int FtFont_Height(const FtFont_t* f)
{
	BGI_UNUSED(f);
	return 0;
}

int FtFont_Glyph(FtFont_t* f, uint32_t cp, int antialiased, FtGlyph_t* out)
{
	BGI_UNUSED(f);
	BGI_UNUSED(cp);
	BGI_UNUSED(antialiased);
	BGI_UNUSED(out);
	return 0;
}

void FtFont_Render(FtFont_t* f, uint32_t cp, uint8_t* buf, int pitch, int w, int h)
{
	int r;
	BGI_UNUSED(f);
	BGI_UNUSED(cp);
	for(r = 0; r < h; r++)
		memset(buf + (size_t)r * (size_t)pitch, 0, (size_t)w);
}

void FtFont_RenderMono(FtFont_t* f, uint32_t cp, uint8_t* bits, int pitch, int w, int h)
{
	BGI_UNUSED(f);
	BGI_UNUSED(cp);
	BGI_UNUSED(w);
	memset(bits, 0, (size_t)pitch * (size_t)h);
}

int FtFont_Advance(FtFont_t* f, uint32_t cp)
{
	BGI_UNUSED(f);
	BGI_UNUSED(cp);
	return 0;
}

int FtFont_FaceAdvance(FtFont_t* f, uint32_t cp)
{
	BGI_UNUSED(f);
	BGI_UNUSED(cp);
	return -1;
}

#endif // BGI_HAVE_FREETYPE
