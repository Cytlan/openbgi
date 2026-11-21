/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * font.c - the POSIX back end's fonts: the system's font files for the
 *          engine's FreeType rasteriser (OS_FontFiles, src/host/os.h,
 *          openbgi.h) and the antialiased text of the back end's own
 *          windows (src/host/os_posix.h)
 *
 * A family is resolved to a font file through fontconfig when it was
 * available at build time (BGI_HAVE_FONTCONFIG) and otherwise by looking
 * for well-known Japanese fonts under the usual directories (on Haiku the
 * system's and the user's data/fonts directories), the serif ones for a
 * name of the Mincho class.  The environment variables BGI_FONT_GOTHIC and
 * BGI_FONT_MINCHO (or BGI_FONT for both) name a file that takes the place
 * of every family of their class, the engine's own fonts included
 * (OPENBGI_FONT_FORCED).  A character a font lacks is looked up with
 * fontconfig's best match for it, or among a few well-known files that
 * cover the scripts Japanese fonts lack.
 *
 * The back end's own windows - the boxes and dialogs, the text entry -
 * draw with a FreeType library of their own (bgi/ftfont.h) on the same
 * files, which also gets the fonts the engine was handed as data
 * (`added`), so that the text entry opens a game's own font as the engine
 * does.  The glyphs are antialiased there; the engine's are not.
 */
#include "host/os.h"
#include "host/os_common.h"
#include "host/os_posix.h"
#include "bgi/ftfont.h"
#include "bgi/sjis.h"
#include "bgi/ui.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef BGI_HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif

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

/* the directories searched for font files without fontconfig: the
 * system's (recursively), the user's under $HOME, and links to fonts
 * (Debian's fonts-japanese-* alternatives) */
#if defined(BGI_HAIKU)
#define FONT_SYSTEM_DIRS "/boot/system/data/fonts", "/boot/system/non-packaged/data/fonts"
#define FONT_USER_DIRS   "config/data/fonts", "config/non-packaged/data/fonts",
#define FONT_LINK_DIRS
#elif defined(BGI_MACOS)
#define FONT_SYSTEM_DIRS "/System/Library/Fonts", "/Library/Fonts"
#define FONT_USER_DIRS   "Library/Fonts",
#define FONT_LINK_DIRS
#elif defined(BGI_ANDROID)
#define FONT_SYSTEM_DIRS "/system/fonts", "/product/fonts"
#define FONT_USER_DIRS
#define FONT_LINK_DIRS
#else
#define FONT_SYSTEM_DIRS "/usr/share/fonts", "/usr/local/share/fonts"
#define FONT_USER_DIRS   ".fonts", ".local/share/fonts",
#define FONT_LINK_DIRS   "/etc/alternatives",
#endif

/* without fontconfig: well-known Japanese fonts in order of preference
 * (Debian's alternatives links first; Android's Noto, the Droid fallback of
 * older Androids; macOS's Arial Unicode, whose name the system's Hiragino
 * files lack in ASCII), DejaVu as the last resort so that Latin text at
 * least renders */
static const char* const kGothicFiles[] = {"fonts-japanese-gothic.ttf", "ipag.ttf", "ipagp.ttf", "NotoSansCJK-Regular.ttc",
	"NotoSansCJKjp-Regular.otf", "NotoSansJP-Regular.otf", "NotoSansCJK-Medium.ttc", "NotoSansCJKjp-VF.otf",
	"VL-Gothic-Regular.ttf", "TakaoGothic.ttf", "DroidSansJapanese.ttf", "DroidSansFallbackFull.ttf", "Arial Unicode.ttf",
	"wqy-microhei.ttc", "DejaVuSans.ttf", NULL};
static const char* const kMinchoFiles[] = {"fonts-japanese-mincho.ttf", "ipam.ttf", "ipamp.ttf", "NotoSerifCJK-Regular.ttc",
	"NotoSerifCJKjp-Regular.otf", "NotoSerifJP-Regular.otf", "TakaoMincho.ttf", "DejaVuSerif.ttf", NULL};

/* the fallback without fontconfig: the first of the class's well-known
 * files found under the system and user font directories, then the other
 * class's; 0 when none is installed */
static int ResolveByScan(int mincho, char* out, size_t n)
{
	static const char* const kSystem[] = {FONT_LINK_DIRS FONT_SYSTEM_DIRS, NULL};
	static const char* const kUser[] = {FONT_USER_DIRS NULL};
	const char* home = getenv("HOME");
	char userFonts[4][1024];
	const char* dirs[8];
	int ndirs = 0, i, pass;
	for(i = 0; kSystem[i] && ndirs < 4; i++)
		dirs[ndirs++] = kSystem[i];
	for(i = 0; home && kUser[i] && i < 4; i++)
	{
		snprintf(userFonts[i], sizeof userFonts[i], "%s/%s", home, kUser[i]);
		dirs[ndirs++] = userFonts[i];
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

/* the file the environment names for the class (BGI_FONT_MINCHO /
 * BGI_FONT_GOTHIC, then BGI_FONT), NULL when it names none */
static const char* EnvFile(int mincho)
{
	const char* env = getenv(mincho ? "BGI_FONT_MINCHO" : "BGI_FONT_GOTHIC");
	if(!env || !*env)
		env = getenv("BGI_FONT");
	return env && *env ? env : NULL;
}

/* a font file that has a glyph for `cp`: asked of fontconfig (the best
 * scalable match of the class that covers the character), or found among
 * a few well-known files that cover the scripts Japanese fonts lack; 0
 * when there is none */
static int ResolveFallbackFile(uint32_t cp, int serif, int bold, char* out, size_t n, int* index)
{
	static const char* const kFiles[] = {"DejaVuSans.ttf", "NotoSansArabic-Regular.ttf", "NotoNaskhArabic-Regular.ttf",
		"NotoSansHebrew-Regular.ttf", "FreeSans.ttf", "FreeSerif.ttf", "unifont.otf", "unifont.ttf", NULL};
	static const char* const kDirs[] = {FONT_SYSTEM_DIRS, NULL};
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
		FcPatternAddString(pat, FC_FAMILY, (const FcChar8*)(serif ? "serif" : "sans-serif"));
		FcPatternAddInteger(pat, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
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
	BGI_UNUSED(serif);
	BGI_UNUSED(bold);
	for(name = kFiles; *name; name++)
		for(dir = kDirs; *dir; dir++)
			if(FindFontFile(*dir, *name, 0, out, n))
				return 1;
	return 0;
}

// ---- the font files of the system (OS_FontFiles) --------------------------------

/* the file for a family: the environment's for its class, then
 * fontconfig's best match, then the directory scan */
static int FilesFind(void* user, const char* family, int serif, int bold, int italic, OpenBGI_FontFile_t* out)
{
	const char* env = EnvFile(serif);
	BGI_UNUSED(user);
	out->data = NULL;
	out->size = 0;
	out->index = 0;
	if(env)
	{
		snprintf(out->path, sizeof out->path, "%s", env);
		return OPENBGI_FONT_FORCED;
	}
#ifdef BGI_HAVE_FONTCONFIG
	if(ResolveByFontconfig(family, serif, bold, italic, out->path, sizeof out->path, &out->index))
		return OPENBGI_FONT_FOUND;
#else
	BGI_UNUSED(family);
	BGI_UNUSED(bold);
	BGI_UNUSED(italic);
#endif
	return ResolveByScan(serif, out->path, sizeof out->path) ? OPENBGI_FONT_FOUND : 0;
}

static int FilesFindChar(void* user, uint32_t cp, int serif, int bold, OpenBGI_FontFile_t* out)
{
	BGI_UNUSED(user);
	out->data = NULL;
	out->size = 0;
	return ResolveFallbackFile(cp, serif, bold, out->path, sizeof out->path, &out->index) ? OPENBGI_FONT_FOUND : 0;
}

/* fontconfig lists a family of exactly that name (without fontconfig the
 * back end cannot tell); the environment forces a file for its class */
static int FilesFamily(void* user, const char* family)
{
	int r = EnvFile(IsMinchoName(family)) ? OPENBGI_FONT_FORCED : 0;
#ifdef BGI_HAVE_FONTCONFIG
	FcPattern* pat;
	FcObjectSet* os;
	FcFontSet* set;
	BGI_UNUSED(user);
	if(!FcInit())
		return r;
	pat = FcPatternCreate();
	FcPatternAddString(pat, FC_FAMILY, (const FcChar8*)family);
	os = FcObjectSetBuild(FC_FAMILY, (char*)NULL);
	set = FcFontList(NULL, pat, os);
	if(set && set->nfont > 0)
		r |= OPENBGI_FONT_FOUND;
	if(set)
		FcFontSetDestroy(set);
	FcObjectSetDestroy(os);
	FcPatternDestroy(pat);
	return r;
#else
	BGI_UNUSED(user);
	BGI_UNUSED(family);
	return r | OPENBGI_FONT_UNKNOWN; // (the scan picks the nearest Japanese font anyway)
#endif
}

/* the families of fontconfig that cover the language, in its order (each
 * style of a family is one entry: the engine lists each name once); none
 * without fontconfig */
static void FilesFamilies(void* user, const char* lang, void (*each)(void* ctx, const char* family), void* ctx)
{
#ifdef BGI_HAVE_FONTCONFIG
	FcPattern* pat;
	FcObjectSet* os;
	FcFontSet* set;
	int i;
	BGI_UNUSED(user);
	if(!FcInit())
		return;
	pat = FcPatternCreate();
	if(lang)
		FcPatternAddString(pat, FC_LANG, (const FcChar8*)lang);
	os = FcObjectSetBuild(FC_FAMILY, (char*)NULL);
	set = FcFontList(NULL, pat, os);
	for(i = 0; set && i < set->nfont; i++)
	{
		FcChar8* fam = NULL;
		if(FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &fam) == FcResultMatch && fam)
			each(ctx, (const char*)fam);
	}
	if(set)
		FcFontSetDestroy(set);
	FcObjectSetDestroy(os);
	FcPatternDestroy(pat);
#else
	BGI_UNUSED(user);
	BGI_UNUSED(lang);
	BGI_UNUSED(each);
	BGI_UNUSED(ctx);
#endif
}

static FtFonts_t* UiFonts(void);

// a font the engine was handed as data: the back end's windows find it too
static void FilesAdded(void* user, const void* data, uint32_t size)
{
	BGI_UNUSED(user);
	FtFonts_AddMemory(UiFonts(), data, size, 1);
}

static const OpenBGI_FontFiles_t kFiles = {sizeof(OpenBGI_FontFiles_t), FilesFind, FilesFindChar, FilesFamily, FilesFamilies, FilesAdded};

const OpenBGI_FontFiles_t* OS_FontFiles(void)
{
	return &kFiles;
}

// the engine rasterises on this platform: no renderer
const OpenBGI_FontRenderer_t* OS_FontRenderer(void)
{
	return NULL;
}

// ---- antialiased text for the back end's own windows -----------------------------

static FtFonts_t* gUi; // the windows' FreeType library: NULL until the first use

static FtFonts_t* UiFonts(void)
{
	if(!gUi)
		gUi = FtFonts_New(NULL, &kFiles, NULL);
	return gUi;
}

// OS_Shutdown: the windows' library and fontconfig's state
void OsPosix_FontShutdown(void)
{
	FtFonts_Delete(gUi);
	gUi = NULL;
#ifdef BGI_HAVE_FONTCONFIG
	FcFini();
#endif
}

FtFont_t* OsPosix_FontOpen(const char* face, int height, int width)
{
	char utf8[0x200];
	OsCommon_SjisToUtf8(face, utf8, sizeof utf8);
	return FtFont_Open(UiFonts(), utf8, height, width, 0, 0);
}

void OsPosix_FontClose(FtFont_t* f)
{
	FtFont_Close(f);
}

// the gothic face at `height` pixels for the back end's own user interface
FtFont_t* OsPosix_FontUi(int height)
{
	// "ＭＳ ゴシック" (MS Gothic) in Shift-JIS
	return OsPosix_FontOpen("\x82\x6c\x82\x72\x20\x83\x53\x83\x56\x83\x62\x83\x4e", height, height / 2);
}

// the next character of a string in the text encoding: its length (1 .. 4 bytes) and code point
static int NextChar(const char* s, uint32_t* cp)
{
	int len = 1;
	if(gOsTextUtf8)
	{
		int n = Utf8_SeqLen((const uint8_t*)s);
		len = n ? n : 1;
	}
	else if(Sjis_IsLead((uint8_t)s[0]) && s[1])
		len = 2;
	*cp = OsCommon_CodePoint(s, len);
	return len;
}

/* draw `sjis` into a 32-bit 0x00RRGGBB buffer (pitch in pixels) with its
 * cell top-left at (x, y), blending the coverage with the colour over
 * what is there and clipping to w x h; returns the pen position after
 * the last character (x itself without a font) */
int OsPosix_FontDraw(FtFont_t* f, uint32_t* dst, int pitch, int w, int h, int x, int y, const char* sjis, uint32_t rgb)
{
	int pen = x;
	if(!f)
		return x;
	while(*sjis)
	{
		uint32_t cp;
		FtGlyph_t g;
		int yy, xx;
		sjis += NextChar(sjis, &cp);
		if(!FtFont_Glyph(f, cp, 1, &g))
			continue;
		for(yy = 0; yy < g.rows; yy++)
		{
			int dy = y + g.y + yy;
			const uint8_t* row = g.bits + (size_t)yy * (size_t)g.pitch;
			if(dy < 0 || dy >= h)
				continue;
			for(xx = 0; xx < g.width; xx++)
			{
				int dx = pen + g.x + xx;
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
		pen += (g.advance + 32) >> 6;
	}
	return pen;
}

// the advance of `sjis` in pixels (the first `bytes` bytes, or all when < 0), without rendering; 0 without a font
int OsPosix_FontTextWidth(FtFont_t* f, const char* sjis, int bytes)
{
	int pen = 0;
	const char* end = bytes < 0 ? NULL : sjis + bytes;
	if(!f)
		return 0;
	while(*sjis && (!end || sjis < end))
	{
		uint32_t cp;
		int adv;
		sjis += NextChar(sjis, &cp);
		adv = FtFont_FaceAdvance(f, cp);
		if(adv >= 0)
			pen += (adv + 32) >> 6;
	}
	return pen;
}

static int UiDraw(void* ctx, uint32_t* pix, int pitch, int w, int h, int x, int y, const char* text, uint32_t rgb)
{
	return OsPosix_FontDraw((FtFont_t*)ctx, pix, pitch, w, h, x, y, text, rgb);
}

static int UiWidth(void* ctx, const char* text, int bytes)
{
	return OsPosix_FontTextWidth((FtFont_t*)ctx, text, bytes);
}

void OsPosix_UiFont(UiFont_t* out, FtFont_t* f)
{
	out->ctx = f;
	out->height = f ? FtFont_Height(f) : 0;
	out->draw = UiDraw;
	out->width = UiWidth;
}
