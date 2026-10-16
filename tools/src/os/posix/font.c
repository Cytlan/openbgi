/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * font.c - the POSIX back end's fonts (FreeType): the OS_Font* services of
 *          inc/bgi/os.h and the antialiased text of inc/bgi/os_posix.h
 *
 * The engine asks for GDI fonts by face name ("ＭＳ ゴシック" = MS Gothic,
 * "ＭＳ 明朝" = MS Mincho), a cell height and the average (half-width)
 * character width, and expects each character drawn at the top-left corner
 * of a buffer the way TextOut() does: the cell top at row 0, the baseline
 * at the font's ascent, full-width glyphs twice the given width.
 *
 * The face name is resolved to a font file through fontconfig when it was
 * available at build time (BGI_HAVE_FONTCONFIG) and otherwise by looking for
 * well-known Japanese fonts under the usual directories; the environment
 * variables BGI_FONT_GOTHIC and BGI_FONT_MINCHO (or BGI_FONT for both) name
 * a file to use instead.  One FT_Face is shared by every font opened on the
 * same file; each OsFont owns an FT_Size so sizes do not interfere.
 *
 * A character the face has no glyph for is looked up in another font
 * (fontconfig's best match for the character, or a few well-known files),
 * scaled to the same cell and put on the same baseline.  GDI does much the
 * same for the scripts its fonts lack; here it is what lets Arabic or
 * Hebrew text appear in a window whose font is a Japanese one.
 *
 * GDI draws into an 8-bit palette DIB in the original, where it never
 * antialiases, so OS_FontRender() rasterises in monochrome; the engine does
 * its own oversampling and filtering (src/gfx/font.c).  The dialogs and the
 * text entry of this back end draw antialiased through OsPosix_FontDraw().
 */
#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_posix.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_SIZES_H
#include FT_SYNTHESIS_H
#include FT_TRUETYPE_TABLES_H

#ifdef BGI_HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#include <unistd.h>
#endif

// ---- shared faces -------------------------------------------------------------

// one FT_Face, shared by every OsFont opened on the same file and face index
typedef struct FaceRec
{
	struct FaceRec* next; // the list gFaces
	char path[1024];      // the font file
	int index;            // the face within it (a .ttc holds several)
	FT_Face face;
	int refs; // the OsFonts using it; freed when the last one closes
} FaceRec_t;

#define FALLBACK_MAX  4 // other fonts one OsFont may borrow glyphs from
#define FALLBACK_MISS 8 // character blocks remembered as having no fallback

typedef struct Fallback // another font that has glyphs the face lacks
{
	FaceRec_t* rec; // the shared face
	FT_Size size;   // scaled to this font's cell
} Fallback_t;

struct OsFont
{
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

static FT_Library gFt;    // NULL until the first font is opened
static FaceRec_t* gFaces; // the open faces

// the FreeType library, created on the first use; 0 when that fails
static int FtInit(void)
{
	if(gFt)
		return 1;
	return FT_Init_FreeType(&gFt) == 0;
}

// OS_Shutdown: release the libraries' global state, unless a font is still open (its face would die with FreeType)
void OsPosix_FontShutdown(void)
{
	if(gFaces) // fonts the engine never closed: leave the faces to the process exit
		return;
	if(gFt)
	{
		FT_Done_FreeType(gFt);
		gFt = NULL;
	}
#ifdef BGI_HAVE_FONTCONFIG
	FcFini();
#endif
}

// ---- locating a font file -------------------------------------------------------

// case-insensitive substring test (strcasestr is not in POSIX)
static int ContainsNoCase(const char* s, const char* sub)
{
	size_t n = strlen(sub);
	for(; *s; s++)
		if(strncasecmp(s, sub, n) == 0)
			return 1;
	return 0;
}

/* the engine's face names are Shift-JIS (here already UTF-8); a name
 * containing "明朝" (Mincho, the serif family), "mincho" or "serif" picks
 * the serif class, everything else the sans (gothic) class */
static int IsMinchoName(const char* utf8)
{
	if(strstr(utf8, "\xe6\x98\x8e\xe6\x9c\x9d") != NULL) // 明朝
		return 1;
	return ContainsNoCase(utf8, "mincho") || ContainsNoCase(utf8, "serif");
}

// recursive search (4 levels, hidden entries skipped) of `dir` for a file called `name`, case-insensitive; the path into out
static int FindFontFile(const char* dir, const char* name, int depth, char* out, size_t n)
{
	DIR* d = opendir(dir);
	struct dirent* e;
	int found = 0;
	if(!d)
		return 0;
	while(!found && (e = readdir(d)) != NULL)
	{
		char path[1024];
		struct stat st;
		if(e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if(stat(path, &st) != 0)
			continue;
		if(S_ISDIR(st.st_mode))
		{
			if(depth < 4)
				found = FindFontFile(path, name, depth + 1, out, n);
		}
		else if(strcasecmp(e->d_name, name) == 0)
		{
			snprintf(out, n, "%s", path);
			found = 1;
		}
	}
	closedir(d);
	return found;
}

/* without fontconfig: well-known Japanese fonts in order of preference
 * (Debian's alternatives links first), DejaVu as the last resort so that
 * Latin text at least renders */
static const char* const kGothicFiles[] = {"fonts-japanese-gothic.ttf", "ipag.ttf", "ipagp.ttf", "NotoSansCJK-Regular.ttc",
	"NotoSansCJKjp-Regular.otf", "NotoSansJP-Regular.otf", "NotoSansCJK-Medium.ttc", "VL-Gothic-Regular.ttf",
	"TakaoGothic.ttf", "DroidSansJapanese.ttf", "wqy-microhei.ttc", "DejaVuSans.ttf", NULL};
static const char* const kMinchoFiles[] = {"fonts-japanese-mincho.ttf", "ipam.ttf", "ipamp.ttf", "NotoSerifCJK-Regular.ttc",
	"NotoSerifCJKjp-Regular.otf", "NotoSerifJP-Regular.otf", "TakaoMincho.ttf", "DejaVuSerif.ttf", NULL};

/* the fallback without fontconfig: the first of the class's well-known
 * files found under the system and user font directories, then the other
 * class's; 0 when none is installed */
static int ResolveByScan(int mincho, char* out, size_t n)
{
	const char* home = getenv("HOME");
	char userFonts[2][1024];
	const char* dirs[6];
	int ndirs = 0, i, pass;
	dirs[ndirs++] = "/etc/alternatives";
	dirs[ndirs++] = "/usr/share/fonts";
	dirs[ndirs++] = "/usr/local/share/fonts";
	if(home)
	{
		snprintf(userFonts[0], sizeof userFonts[0], "%s/.fonts", home);
		snprintf(userFonts[1], sizeof userFonts[1], "%s/.local/share/fonts", home);
		dirs[ndirs++] = userFonts[0];
		dirs[ndirs++] = userFonts[1];
	}
	for(pass = 0; pass < 2; pass++)
	{
		const char* const* names = (mincho ^ pass) ? kMinchoFiles : kGothicFiles; // the other class as a fallback
		for(; *names; names++)
			for(i = 0; i < ndirs; i++)
				if(FindFontFile(dirs[i], *names, 0, out, n))
					return 1;
	}
	return 0;
}

#ifdef BGI_HAVE_FONTCONFIG
/* ask fontconfig for the family (with the class's generic family and the
 * Japanese language as further hints, so that an unknown name still gets
 * a Japanese font), the weight and the slant; the file and face index of
 * the best match, 0 when fontconfig has nothing */
static int ResolveByFontconfig(const char* utf8, int mincho, int bold, int italic, char* out, size_t n, int* index)
{
	FcPattern* pat;
	FcPattern* match;
	FcResult res;
	FcChar8* file = NULL;
	int ok = 0;
	if(!FcInit())
		return 0;
	pat = FcPatternCreate();
	FcPatternAddString(pat, FC_FAMILY, (const FcChar8*)utf8);
	FcPatternAddString(pat, FC_FAMILY, (const FcChar8*)(mincho ? "serif" : "sans-serif"));
	FcPatternAddString(pat, FC_LANG, (const FcChar8*)"ja");
	FcPatternAddInteger(pat, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
	FcPatternAddInteger(pat, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
	FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	match = FcFontMatch(NULL, pat, &res);
	if(match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
	{
		snprintf(out, n, "%s", (const char*)file);
		if(FcPatternGetInteger(match, FC_INDEX, 0, index) != FcResultMatch)
			*index = 0;
		ok = 1;
	}
	if(match)
		FcPatternDestroy(match);
	FcPatternDestroy(pat);
	return ok;
}
#endif

/* the font file (and face index) for a face name: the environment
 * override of its class first (BGI_FONT_MINCHO / BGI_FONT_GOTHIC, then
 * BGI_FONT), then fontconfig, then the directory scan; 0 when nothing
 * was found */
static int ResolveFace(const char* sjisFace, int bold, int italic, char* out, size_t n, int* index)
{
	char utf8[0x200];
	const char* env;
	int mincho;
	OsPosix_SjisToUtf8(sjisFace, utf8, sizeof utf8);
	mincho = IsMinchoName(utf8);
	*index = 0;
	env = getenv(mincho ? "BGI_FONT_MINCHO" : "BGI_FONT_GOTHIC");
	if(!env || !*env)
		env = getenv("BGI_FONT");
	if(env && *env)
	{
		snprintf(out, n, "%s", env);
		return 1;
	}
#ifdef BGI_HAVE_FONTCONFIG
	if(ResolveByFontconfig(utf8, mincho, bold, italic, out, n, index))
		return 1;
#endif
	return ResolveByScan(mincho, out, n);
}

// the shared face of a file and index: another reference to an open one, or a newly loaded one; NULL when FreeType cannot load it
static FaceRec_t* FaceAcquire(const char* path, int index)
{
	FaceRec_t* r;
	for(r = gFaces; r; r = r->next)
	{
		if(r->index == index && strcmp(r->path, path) == 0)
		{
			r->refs++;
			return r;
		}
	}
	r = (FaceRec_t*)calloc(1, sizeof *r);
	if(!r)
		return NULL;
	if(FT_New_Face(gFt, path, index, &r->face) != 0)
	{
		free(r);
		return NULL;
	}
	// a Unicode charmap is what the Shift-JIS table maps into; any charmap is better than none
	if(FT_Select_Charmap(r->face, FT_ENCODING_UNICODE) != 0 && r->face->num_charmaps > 0)
		FT_Set_Charmap(r->face, r->face->charmaps[0]);
	snprintf(r->path, sizeof r->path, "%s", path);
	r->index = index;
	r->refs = 1;
	r->next = gFaces;
	gFaces = r;
	return r;
}

// drop a reference; the face is unloaded and unlisted with the last one
static void FaceRelease(FaceRec_t* r)
{
	FaceRec_t** pp;
	if(--r->refs > 0)
		return;
	for(pp = &gFaces; *pp; pp = &(*pp)->next)
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

// ---- opening -------------------------------------------------------------------

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

// the GDI text instruction's font: FreeType has no DRAFT_QUALITY, so it is the same face at size x size / 2
OsFont_t* OS_FontOpenMono(const char* face, int size, int bold)
{
	return OS_FontOpen(face, size, size / 2, bold, 0);
}

// "B0 C2": add the file to fontconfig's application fonts, so that its family name resolves; 0 without fontconfig
int OS_FontAddFile(const char* path)
{
	char native[0x400];
	OsPosix_SjisToUtf8(path, native, sizeof native);
#ifdef BGI_HAVE_FONTCONFIG
	if(FcInit() && FcConfigAppFontAddFile(NULL, (const FcChar8*)native))
		return 1;
#endif
	return 0;
}

// 1 when fontconfig lists a font of exactly that family; without fontconfig every face "exists"
int OS_FontFaceExists(const char* face)
{
#ifdef BGI_HAVE_FONTCONFIG
	char utf8[0x200];
	FcPattern* pat;
	FcObjectSet* os;
	FcFontSet* set;
	int found = 0;
	OsPosix_SjisToUtf8(face, utf8, sizeof utf8);
	if(!FcInit())
		return 0;
	pat = FcPatternCreate();
	FcPatternAddString(pat, FC_FAMILY, (const FcChar8*)utf8);
	os = FcObjectSetBuild(FC_FAMILY, (char*)NULL);
	set = FcFontList(NULL, pat, os);
	found = set && set->nfont > 0;
	if(set)
		FcFontSetDestroy(set);
	FcObjectSetDestroy(os);
	FcPatternDestroy(pat);
	return found;
#else
	BGI_UNUSED(face);
	return 1; // the scan picks the nearest Japanese font anyway
#endif
}

/* the faces of fontconfig that cover the charset's language (ja for
 * Shift-JIS, zh-tw for Big5, zh-cn for GB2312, ko for Hangul, any for
 * ANSI), each family once; a name that the text encoding cannot spell is
 * left out.  Without fontconfig the list is empty. */
int OS_FontEnumFaces(char* buf, int charset, int* outBytes)
{
	int count = 0, bytes = 0;
#ifdef BGI_HAVE_FONTCONFIG
	const char* lang = charset == 0x80 ? "ja" : charset == 0x86 ? "zh-tw"
		: charset == 0x88                                       ? "zh-cn"
		: charset == 0x81                                       ? "ko"
																: NULL;
	FcPattern* pat;
	FcObjectSet* os;
	FcFontSet* set;
	char seen[0x400][0x40]; // the family names already listed (fontconfig lists every style)
	int nseen = 0, i;
	if(!FcInit())
	{
		*outBytes = 0;
		return 0;
	}
	pat = FcPatternCreate();
	if(lang)
		FcPatternAddString(pat, FC_LANG, (const FcChar8*)lang);
	os = FcObjectSetBuild(FC_FAMILY, (char*)NULL);
	set = FcFontList(NULL, pat, os);
	for(i = 0; set && i < set->nfont; i++)
	{
		FcChar8* fam = NULL;
		char name[0x100];
		const uint8_t* u;
		int n = 0, j, dup = 0;
		if(FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &fam) != FcResultMatch || !fam)
			continue;
		// the name in the text encoding
		if(gOsTextUtf8)
			n = snprintf(name, sizeof name, "%s", (const char*)fam);
		else
		{
			for(u = (const uint8_t*)fam; *u && n < (int)sizeof name - 3;)
			{
				int len, m;
				uint32_t cp = OsCommon_Utf8SeqLen(u) ? OsCommon_Utf8Decode(u, &len) : (len = 1, (uint32_t)*u);
				m = OsPosix_UnicodeToSjis(cp, name + n);
				if(m == 0)
				{
					n = -1; // not spellable
					break;
				}
				n += m;
				u += len;
			}
			if(n < 0)
				continue;
			name[n] = 0;
		}
		if(n <= 0 || n >= (int)sizeof name - 1)
			continue;
		for(j = 0; j < nseen; j++)
			if(strncmp(seen[j], name, sizeof seen[j]) == 0)
				dup = 1;
		if(dup)
			continue;
		if(nseen < (int)BGI_COUNTOF(seen))
			snprintf(seen[nseen++], sizeof seen[0], "%s", name);
		if(buf)
		{
			memcpy(buf + bytes, name, (size_t)n + 1);
		}
		bytes += n + 1;
		count++;
	}
	if(set)
		FcFontSetDestroy(set);
	FcObjectSetDestroy(os);
	FcPatternDestroy(pat);
#else
	BGI_UNUSED(buf);
	BGI_UNUSED(charset);
#endif
	*outBytes = bytes;
	return count;
}

// "B0 C3": a font from an archive; 0 without fontconfig or when the file cannot be written
int OS_FontAddMemory(const void* data, uint32_t size)
{
	// fontconfig takes files: the data goes to a temporary file under /tmp that is never removed
	static int counter;
	char path[0x100];
	FILE* f;
	snprintf(path, sizeof path, "/tmp/bgi-font-%d-%d.ttf", (int)getpid(), counter++);
	f = fopen(path, "wb");
	if(!f)
		return 0;
	fwrite(data, 1, size, f);
	fclose(f);
#ifdef BGI_HAVE_FONTCONFIG
	if(FcInit() && FcConfigAppFontAddFile(NULL, (const FcChar8*)path))
		return 1;
#endif
	return 0;
}

/* open a font as CreateFontA would: the face resolved to a file, scaled so
 * that the GDI cell is `height` pixels and a full-width glyph 2 * width
 * (the em square is stretched when the two disagree), with synthetic
 * bold and italic when the face has none of its own.  NULL for a height
 * of 0, when no font file can be found or FreeType cannot load it. */
OsFont_t* OS_FontOpen(const char* face, int height, int width, int bold, int italic)
{
	char path[1024];
	int index = 0;
	OsFont_t* f;
	FT_Face ft;
	FT_Pos asc, desc, cell, fullAdv;
	FT_UInt gi;

	if(height <= 0 || !FtInit())
		return NULL;
	if(!ResolveFace(face, bold, italic, path, sizeof path, &index))
		return NULL;
	f = (OsFont_t*)calloc(1, sizeof *f);
	if(!f)
		return NULL;
	f->rec = FaceAcquire(path, index);
	if(!f->rec)
	{
		free(f);
		return NULL;
	}
	ft = f->rec->face;
	f->height = height;
	f->width = width;
	f->bold = bold;
	f->italic = italic;
	{
		char utf8[0x200];
		OsPosix_SjisToUtf8(face, utf8, sizeof utf8);
		f->serif = IsMinchoName(utf8);
	}

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
		FaceRelease(f->rec);
		free(f);
		return NULL;
	}
	FT_Activate_Size(f->size);
	if(FT_Set_Char_Size(ft, f->emW, f->emH, 72, 72) != 0)
	{
		FT_Done_Size(f->size);
		FaceRelease(f->rec);
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
	return f;
}

// release the size and the face reference (and those of the fallback fonts); accepts NULL
void OS_FontClose(OsFont_t* f)
{
	int i;
	if(!f)
		return;
	for(i = 0; i < f->fbCount; i++)
	{
		FT_Done_Size(f->fb[i].size);
		FaceRelease(f->fb[i].rec);
	}
	FT_Done_Size(f->size);
	FaceRelease(f->rec);
	free(f);
}

// ---- rendering ----------------------------------------------------------------

// the Unicode code point of a character of `len` bytes in the text encoding
static uint32_t SjisCodePoint(const char* sjis, int len)
{
	return OsCommon_CodePoint(sjis, len, OsPosix_SjisChar);
}

// ---- fallback fonts ------------------------------------------------------------

/* a font file that has a glyph for `cp`, other than the font's own: asked
 * of fontconfig (the best scalable match of the font's class that covers
 * the character), or found among a few well-known files that cover the
 * scripts Japanese fonts lack; 0 when there is none */
static int ResolveFallbackFile(OsFont_t* f, uint32_t cp, char* out, size_t n, int* index)
{
	static const char* const kFiles[] = {"DejaVuSans.ttf", "NotoSansArabic-Regular.ttf", "NotoNaskhArabic-Regular.ttf",
		"NotoSansHebrew-Regular.ttf", "FreeSans.ttf", "FreeSerif.ttf", "unifont.otf", "unifont.ttf", NULL};
	static const char* const kDirs[] = {"/usr/share/fonts", "/usr/local/share/fonts", NULL};
	const char* const* name;
	const char* const* dir;
	*index = 0;
#ifdef BGI_HAVE_FONTCONFIG
	if(FcInit())
	{
		FcPattern* pat = FcPatternCreate();
		FcCharSet* cs = FcCharSetCreate();
		FcPattern* match;
		FcResult res;
		FcChar8* file = NULL;
		int ok = 0;
		FcCharSetAddChar(cs, (FcChar32)cp);
		FcPatternAddCharSet(pat, FC_CHARSET, cs);
		FcPatternAddString(pat, FC_FAMILY, (const FcChar8*)(f->serif ? "serif" : "sans-serif"));
		FcPatternAddInteger(pat, FC_WEIGHT, f->bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
		FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
		FcConfigSubstitute(NULL, pat, FcMatchPattern);
		FcDefaultSubstitute(pat);
		match = FcFontMatch(NULL, pat, &res);
		if(match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
		{
			snprintf(out, n, "%s", (const char*)file);
			if(FcPatternGetInteger(match, FC_INDEX, 0, index) != FcResultMatch)
				*index = 0;
			ok = 1;
		}
		if(match)
			FcPatternDestroy(match);
		FcCharSetDestroy(cs);
		FcPatternDestroy(pat);
		if(ok)
			return 1; // (the caller checks that the file really has the glyph)
	}
#endif
	BGI_UNUSED(cp);
	for(name = kFiles; *name; name++)
		for(dir = kDirs; *dir; dir++)
			if(FindFontFile(*dir, *name, 0, out, n))
				return 1;
	return 0;
}

/* the face and size to draw `cp` with when the font's own face has no
 * glyph for it: one of the fallbacks already open, or a newly found one,
 * scaled so that its cell is as tall as the font's and stretched like it.
 * 0 when no font has the character (remembered per block of 256). */
static int FallbackFor(OsFont_t* f, uint32_t cp, FT_Face* outFace, FT_Size* outSize)
{
	char path[1024];
	int i, index = 0;
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
	if(f->fbCount < FALLBACK_MAX && ResolveFallbackFile(f, cp, path, sizeof path, &index))
		rec = FaceAcquire(path, index);
	if(rec && (rec == f->rec || FT_Get_Char_Index(rec->face, cp) == 0))
	{ // the font itself again, or a file without the glyph after all
		FaceRelease(rec);
		rec = NULL;
	}
	if(rec && FT_New_Size(rec->face, &size) != 0)
	{
		FaceRelease(rec);
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

/* load, optionally embolden and shear, and render one glyph with the
 * font's size active (mono or 8-bit coverage as `mode` says), from the
 * font's face or - for a character it lacks - a fallback font's.  The
 * glyph slot holding the result, NULL when the glyph cannot be loaded or
 * rendered. */
static FT_GlyphSlot LoadGlyph(OsFont_t* f, uint32_t cp, FT_Render_Mode mode)
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

/* clear the buffer and draw the character in monochrome (0 / 255), its
 * cell top-left at the buffer's: the glyph's bitmap is placed by its
 * bearing and the font's ascent, and clipped to w x h */
void OS_FontRender(OsFont_t* f, const char* sjis, int len, uint8_t* buf, int pitch, int w, int h)
{
	FT_GlyphSlot g;
	int x0, y0, y, x;
	int r;
	for(r = 0; r < h; r++)
		memset(buf + (size_t)r * (size_t)pitch, 0, (size_t)w);
	if(!f || !(g = LoadGlyph(f, SjisCodePoint(sjis, len), FT_RENDER_MODE_MONO)))
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

// the same into a 1-bit bitmap (MSB first, pitch bytes per row): a set bit is ink
void OS_FontRenderMono(OsFont_t* f, const char* sjis, int len, uint8_t* bits, int pitch, int w, int h)
{
	FT_GlyphSlot g;
	int x0, y0, y, x;
	memset(bits, 0, (size_t)pitch * (size_t)h);
	if(!f || !(g = LoadGlyph(f, SjisCodePoint(sjis, len), FT_RENDER_MODE_MONO)))
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

// the glyph's advance rounded to whole pixels; 0 for a glyph that cannot be loaded
int OS_FontCharAdvance(OsFont_t* f, const char* sjis, int len)
{
	FT_GlyphSlot g;
	if(!f || !(g = LoadGlyph(f, SjisCodePoint(sjis, len), FT_RENDER_MODE_MONO)))
		return 0;
	return (int)((g->advance.x + 32) >> 6);
}

// ---- antialiased text for the back end's own windows ---------------------------

// the next character of a string in the text encoding: its length (1 .. 4 bytes) and code point
static int NextChar(const char* s, uint32_t* cp)
{
	int len = 1;
	if(gOsTextUtf8)
	{
		int n = OsCommon_Utf8SeqLen((const uint8_t*)s);
		len = n ? n : 1;
	}
	else if(OsCommon_SjisIsLead((uint8_t)s[0]) && s[1])
		len = 2;
	*cp = SjisCodePoint(s, len);
	return len;
}

/* draw `sjis` into a 32-bit 0x00RRGGBB buffer (pitch in pixels) with its
 * cell top-left at (x, y), blending the coverage with the colour over
 * what is there and clipping to w x h; returns the pen position after
 * the last character (x itself without a font) */
int OsPosix_FontDraw(OsFont_t* f, uint32_t* dst, int pitch, int w, int h, int x, int y, const char* sjis, uint32_t rgb)
{
	int pen = x;
	if(!f)
		return x;
	while(*sjis)
	{
		uint32_t cp;
		FT_GlyphSlot g;
		int yy, xx;
		sjis += NextChar(sjis, &cp);
		g = LoadGlyph(f, cp, FT_RENDER_MODE_NORMAL);
		if(!g)
			continue;
		for(yy = 0; yy < (int)g->bitmap.rows; yy++)
		{
			int dy = y + f->ascent - g->bitmap_top + yy;
			const uint8_t* row = g->bitmap.buffer + (size_t)yy * (size_t)g->bitmap.pitch;
			if(dy < 0 || dy >= h)
				continue;
			for(xx = 0; xx < (int)g->bitmap.width; xx++)
			{
				int dx = pen + g->bitmap_left + xx;
				uint32_t a = row[xx], d, r, gg, b;
				if(dx < 0 || dx >= w || a == 0)
					continue;
				d = dst[(size_t)dy * (size_t)pitch + dx];
				r = (((rgb >> 16) & 0xff) * a + ((d >> 16) & 0xff) * (255 - a)) / 255;
				gg = (((rgb >> 8) & 0xff) * a + ((d >> 8) & 0xff) * (255 - a)) / 255;
				b = ((rgb & 0xff) * a + (d & 0xff) * (255 - a)) / 255;
				dst[(size_t)dy * (size_t)pitch + dx] = (r << 16) | (gg << 8) | b;
			}
		}
		pen += (int)((g->advance.x + 32) >> 6);
	}
	return pen;
}

// the advance of `sjis` in pixels (the first `bytes` bytes, or all when < 0), without rendering; 0 without a font
int OsPosix_FontTextWidth(OsFont_t* f, const char* sjis, int bytes)
{
	int pen = 0;
	const char* end = bytes < 0 ? NULL : sjis + bytes;
	if(!f)
		return 0;
	while(*sjis && (!end || sjis < end))
	{
		uint32_t cp;
		FT_Face ft = f->rec->face;
		FT_UInt gi;
		sjis += NextChar(sjis, &cp);
		FT_Activate_Size(f->size);
		gi = FT_Get_Char_Index(ft, cp);
		if(FT_Load_Glyph(ft, gi, FT_LOAD_NO_BITMAP | FT_LOAD_DEFAULT) == 0)
			pen += (int)((ft->glyph->advance.x + 32) >> 6);
	}
	return pen;
}

int OsPosix_FontHeight(OsFont_t* f)
{
	return f ? f->height : 0;
}

// the gothic face at `height` pixels for the back end's own user interface
OsFont_t* OsPosix_FontUi(int height)
{
	// "ＭＳ ゴシック" (MS Gothic) in Shift-JIS
	return OS_FontOpen("\x82\x6c\x82\x72\x20\x83\x53\x83\x56\x83\x62\x83\x4e", height, height / 2, 0, 0);
}
