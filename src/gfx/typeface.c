/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * typeface.c - the fonts the engine draws with (src/include/bgi/gfx/typeface.h)
 *
 * The host's renderer when it has one (OpenBGI_Host_t.fontRenderer: GDI,
 * a page's canvas), the engine's FreeType library (bgi/ftfont.h)
 * otherwise, made on the first font and fed with the host's font files
 * (OpenBGI_Host_t.fontFiles) and the fonts handed over as data.
 *
 * The families of every font handed over as data are kept here for both:
 * they count as installed (a system that takes a font from memory opens it
 * by name without listing it), and those of a game's font file ("B0 C2")
 * are listed among the installed faces of the charsets their "OS/2" table
 * names, as Windows lists a font added with AddFontResource.
 */
#include "bgi/gfx/typeface.h"
#include "bgi/ftfont.h"
#include "bgi/fontfile.h"
#include "bgi/fs.h"
#include "bgi/host.h"
#include "bgi/sjis.h"
#include "bgi/core.h"

struct Typeface
{
	OpenBGI_t* o;
	struct Typeface* next; // the engine's open typefaces
	void* native;          // the host renderer's font
	FtFont_t* ft;          // the engine's own
	int smoothH;           // Typeface_OpenSmooth's: the cell height it was opened at (TYPEFACE_SMOOTH times the one asked for)
	uint8_t* smooth;       // one character rendered at that size (Typeface_SmoothText), made on the first use
};

// a family of a font handed over as data
typedef struct MemFamily
{
	char name[FONTFILE_NAME_MAX]; // UTF-8
	int listed;                   // listed by Typefaces_Enum ("B0 C2", the first name of each face)
	uint32_t codePages;           // the face's ulCodePageRange1
	struct MemFamily* next;
} MemFamily_t;

// ---- the text encoding ----------------------------------------------------------

// the length of the UTF-8 sequence at s (2 .. 4), 0 when it is none
static int Utf8SeqLen(const uint8_t* s)
{
	int n, i;
	if(s[0] < 0xc2 || s[0] > 0xf4)
		return 0;
	n = s[0] >= 0xf0 ? 4 : s[0] >= 0xe0 ? 3
										: 2;
	for(i = 1; i < n; i++)
		if((s[i] & 0xc0) != 0x80)
			return 0;
	return n;
}

// a UTF-8 sequence (its lead byte decides the length) to its code point; *outLen = the bytes taken
static uint32_t Utf8Decode(const uint8_t* u, int* outLen)
{
	uint32_t cp;
	int n = 1;
	if(u[0] < 0x80)
	{
		*outLen = 1;
		return u[0];
	}
	cp = 0;
	while(u[0] & (0x80u >> n))
	{
		cp = (cp << 6) | (u[n] & 0x3f);
		n++;
	}
	cp |= ((uint32_t)u[0] & (0x7fu >> n)) << (6 * (n - 1));
	*outLen = n;
	return cp;
}

/* the code point of a character of `len` bytes in the scripts' text
 * encoding: a UTF-8 sequence (a surrogate pair written as two three-byte
 * sequences joined), or Shift-JIS with the half-width katakana and 0x5C as
 * the backslash */
static uint32_t CodePoint(OpenBGI_t* o, const char* s, int len)
{
	const uint8_t* u = (const uint8_t*)s;
	if(o->enc.textUtf8)
	{
		int n;
		uint32_t cp = Utf8Decode(u, &n);
		if(cp - 0xd800u < 0x400u && len > n)
		{
			uint32_t lo = ((u[n] & 0x0f) << 12) | ((u[n + 1] & 0x3f) << 6) | (u[n + 2] & 0x3f);
			if(lo - 0xdc00u < 0x400u)
				cp = (((cp - 0xd800u) << 10) | (lo - 0xdc00u)) + 0x10000u;
		}
		return cp;
	}
	if(len >= 2 && ((u[0] >= 0x81 && u[0] <= 0x9f) || (u[0] >= 0xe0 && u[0] <= 0xfc)))
		return Sjis_CodePoint((uint16_t)((u[0] << 8) | u[1]));
	if(u[0] >= 0xa1 && u[0] <= 0xdf)
		return 0xff61 + (u[0] - 0xa1);
	return u[0];
}

// ---- the families of the fonts handed over ----------------------------------------

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

// the families of every face of a font handed over as data
static void MemFamilies_Add(OpenBGI_t* o, const uint8_t* data, uint32_t size, int listed)
{
	int faces = FontFile_Faces(data, size), i;
	for(i = 0; i < faces; i++)
	{
		char names[16][FONTFILE_NAME_MAX];
		int n = FontFile_Families(data, size, i, names, 16), k;
		uint32_t pages = FontFile_CodePages(data, size, i);
		for(k = 0; k < n && k < 16; k++)
		{
			MemFamily_t* m = (MemFamily_t*)BGI_Alloc(sizeof *m);
			snprintf(m->name, sizeof m->name, "%s", names[k]);
			m->listed = listed && k == 0;
			m->codePages = pages;
			m->next = o->font.memFamilies;
			o->font.memFamilies = m;
		}
	}
}

// whether a font handed over as data has the face (in the text encoding)
static int MemFamilies_Has(OpenBGI_t* o, const char* face)
{
	char utf[0x200];
	const MemFamily_t* m;
	Sjis_ToUtf8(face, utf, sizeof utf, o->enc.textUtf8);
	for(m = o->font.memFamilies; m; m = m->next)
		if(EqualNoCase(m->name, utf))
			return 1;
	return 0;
}

// ---- the engine's own rasteriser ---------------------------------------------------

// the engine's FreeType library, made on the first call; NULL in a build without one
static FtFonts_t* Lib(OpenBGI_t* o)
{
	if(!o->font.ft && !o->font.ftTried)
	{
		o->font.ftTried = 1;
		o->font.ft = FtFonts_New(o, o->host.fontFiles, o->host.user);
	}
	return o->font.ft;
}

// what the host knows of the family of `face` (OPENBGI_FONT_* flags); -1 when it has no such service
static int HostFamily(OpenBGI_t* o, const char* face)
{
	const OpenBGI_FontFiles_t* files = o->host.fontFiles;
	char utf[0x200];
	if(!files || !files->family)
		return -1;
	Sjis_ToUtf8(face, utf, sizeof utf, o->enc.textUtf8);
	return files->family(o->host.user, utf);
}

// ---- typefaces ---------------------------------------------------------------------

// a typeface of one of the two kinds, listed with the engine's; NULL when `native` and `ft` are both NULL
static Typeface_t* Wrap(OpenBGI_t* o, void* native, FtFont_t* ft)
{
	Typeface_t* t;
	if(!native && !ft)
		return NULL;
	t = (Typeface_t*)BGI_Calloc(sizeof *t);
	t->o = o;
	t->native = native;
	t->ft = ft;
	t->next = o->font.typefaces;
	o->font.typefaces = t;
	return t;
}

Typeface_t* Typeface_Open(OpenBGI_t* o, const char* face, int height, int width, int bold, int italic)
{
	const OpenBGI_FontRenderer_t* r = o->host.fontRenderer;
	char utf[0x200];
	if(r)
		return Wrap(o, r->open ? r->open(o->host.user, face, height, width, bold, italic) : NULL, NULL);
	Sjis_ToUtf8(face, utf, sizeof utf, o->enc.textUtf8);
	return Wrap(o, NULL, FtFont_Open(Lib(o), utf, height, width, bold, italic));
}

// FreeType has no DRAFT_QUALITY: the same face at size x size / 2
Typeface_t* Typeface_OpenMono(OpenBGI_t* o, const char* face, int size, int bold)
{
	const OpenBGI_FontRenderer_t* r = o->host.fontRenderer;
	if(r)
		return Wrap(o, r->openMono ? r->openMono(o->host.user, face, size, bold) : NULL, NULL);
	return Typeface_Open(o, face, size, size / 2, bold, 0);
}

void Typeface_Close(Typeface_t* t)
{
	OpenBGI_t* o;
	Typeface_t** pp;
	if(!t)
		return;
	o = t->o;
	for(pp = &o->font.typefaces; *pp; pp = &(*pp)->next)
	{
		if(*pp == t)
		{
			*pp = t->next;
			break;
		}
	}
	if(t->native && o->host.fontRenderer && o->host.fontRenderer->close)
		o->host.fontRenderer->close(o->host.user, t->native);
	FtFont_Close(t->ft);
	BGI_Free(t->smooth);
	BGI_Free(t);
}

void Typeface_Render(Typeface_t* t, const char* ch, int len, uint8_t* buf, int pitch, int w, int h)
{
	const OpenBGI_FontRenderer_t* r;
	if(!t)
		return;
	r = t->o->host.fontRenderer;
	if(t->native)
	{
		if(r->render)
			r->render(t->o->host.user, t->native, ch, len, buf, pitch, w, h);
	}
	else
		FtFont_Render(t->ft, CodePoint(t->o, ch, len), buf, pitch, w, h);
}

void Typeface_RenderMono(Typeface_t* t, const char* ch, int len, uint8_t* bits, int pitch, int w, int h)
{
	const OpenBGI_FontRenderer_t* r;
	if(!t)
		return;
	r = t->o->host.fontRenderer;
	if(t->native)
	{
		if(r->renderMono)
			r->renderMono(t->o->host.user, t->native, ch, len, bits, pitch, w, h);
	}
	else
		FtFont_RenderMono(t->ft, CodePoint(t->o, ch, len), bits, pitch, w, h);
}

int Typeface_Advance(Typeface_t* t, const char* ch, int len)
{
	const OpenBGI_FontRenderer_t* r;
	if(!t)
		return 0;
	r = t->o->host.fontRenderer;
	if(t->native)
		return r->advance ? r->advance(t->o->host.user, t->native, ch, len) : 0;
	return FtFont_Advance(t->ft, CodePoint(t->o, ch, len));
}

// ---- smooth text --------------------------------------------------------------------

#define SMOOTH_W(t) (2 * (t)->smoothH + TYPEFACE_SMOOTH) // the large rendering's width: two full-width cells and the offset

Typeface_t* Typeface_OpenSmooth(OpenBGI_t* o, const char* face, int height, int width)
{
	Typeface_t* t = Typeface_Open(o, face, height * TYPEFACE_SMOOTH, width * TYPEFACE_SMOOTH, 0, 0);
	if(t)
		t->smoothH = height * TYPEFACE_SMOOTH;
	return t;
}

// the bytes of the character at s in the scripts' text encoding
static int CharLen(OpenBGI_t* o, const char* s)
{
	const uint8_t* u = (const uint8_t*)s;
	if(o->enc.textUtf8)
	{
		int n = Utf8SeqLen(u);
		return n ? n : 1;
	}
	return ((u[0] >= 0x81 && u[0] <= 0x9f) || (u[0] >= 0xe0 && u[0] <= 0xfc)) && u[1] ? 2 : 1;
}

// the advance of the first `bytes` bytes of `text` (all when < 0) at the large size
static int SmoothPen(Typeface_t* t, const char* text, int bytes)
{
	int pen = 0, i = 0;
	while(text[i] && (bytes < 0 || i < bytes))
	{
		int n = CharLen(t->o, text + i);
		pen += Typeface_Advance(t, text + i, n);
		i += n;
	}
	return pen;
}

int Typeface_SmoothWidth(Typeface_t* t, const char* text, int bytes)
{
	if(!t || !t->smoothH)
		return 0;
	return (SmoothPen(t, text, bytes) + TYPEFACE_SMOOTH / 2) / TYPEFACE_SMOOTH;
}

// coverage `a` (0 .. 255) of `rgb` over pixel *d: blended over 0x00RRGGBB, or composited over 0xAARRGGBB
static void SmoothPut(uint32_t* d, uint32_t a, uint32_t rgb, int alpha)
{
	uint32_t da = *d >> 24, oa, r, g, b;
	if(!alpha)
	{
		r = (((rgb >> 16) & 0xff) * a + ((*d >> 16) & 0xff) * (255 - a)) / 255;
		g = (((rgb >> 8) & 0xff) * a + ((*d >> 8) & 0xff) * (255 - a)) / 255;
		b = ((rgb & 0xff) * a + (*d & 0xff) * (255 - a)) / 255;
		*d = (r << 16) | (g << 8) | b;
		return;
	}
	if(!da)
	{
		*d = (a << 24) | (rgb & 0xffffff);
		return;
	}
	da = da * (255 - a) / 255; // what shows through of what is there
	oa = a + da;
	r = (((rgb >> 16) & 0xff) * a + ((*d >> 16) & 0xff) * da) / oa;
	g = (((rgb >> 8) & 0xff) * a + ((*d >> 8) & 0xff) * da) / oa;
	b = ((rgb & 0xff) * a + (*d & 0xff) * da) / oa;
	*d = (oa << 24) | (r << 16) | (g << 8) | b;
}

/* Each character rendered at the large size at its quarter-pixel offset,
 * filtered down to coverage and put into the buffer with `rgb`. */
int Typeface_SmoothText(Typeface_t* t, uint32_t* pix, int pitch, int w, int h, int x, int y, const char* text, uint32_t rgb, int alpha)
{
	int pen = 0, sw; // in the large rendering's pixels from x
	if(!t || !t->smoothH)
		return x;
	sw = SMOOTH_W(t);
	if(!t->smooth)
		t->smooth = (uint8_t*)BGI_Alloc((size_t)sw * (size_t)t->smoothH);
	while(*text)
	{
		int n = CharLen(t->o, text), sub = pen % TYPEFACE_SMOOTH, gx, gy;
		memset(t->smooth, 0, (size_t)sw * (size_t)t->smoothH);
		Typeface_Render(t, text, n, t->smooth + sub, sw, sw - sub, t->smoothH);
		for(gy = 0; gy < t->smoothH / TYPEFACE_SMOOTH; gy++)
		{
			int dy = y + gy;
			if(dy < 0 || dy >= h)
				continue;
			for(gx = 0; gx < sw / TYPEFACE_SMOOTH; gx++)
			{
				const uint8_t* q = t->smooth + (size_t)gy * TYPEFACE_SMOOTH * sw + (size_t)gx * TYPEFACE_SMOOTH;
				uint32_t a = 0;
				int dx = x + pen / TYPEFACE_SMOOTH + gx, i, j;
				if(dx < 0 || dx >= w)
					continue;
				for(j = 0; j < TYPEFACE_SMOOTH; j++)
					for(i = 0; i < TYPEFACE_SMOOTH; i++)
						a += q[j * sw + i];
				if((a /= TYPEFACE_SMOOTH * TYPEFACE_SMOOTH) != 0)
					SmoothPut(&pix[(size_t)dy * (size_t)pitch + (size_t)dx], a, rgb, alpha);
			}
		}
		pen += Typeface_Advance(t, text, n);
		text += n;
	}
	return x + (pen + TYPEFACE_SMOOTH / 2) / TYPEFACE_SMOOTH;
}

// ---- the fonts ---------------------------------------------------------------------

/* a font's data to the renderer or the engine's library, its families
 * remembered (and listed when `listed`); `keep` when the data stays as it
 * is; `substitute` for a font of the engine's own, which a renderer that
 * tells the two apart takes as such.  1 when it was taken. */
static int AddData(OpenBGI_t* o, const uint8_t* data, uint32_t size, int keep, int listed, int substitute)
{
	const OpenBGI_FontRenderer_t* r = o->host.fontRenderer;
	if(r)
	{
		int (*add)(void*, const void*, uint32_t) = substitute && r->addSubstitute ? r->addSubstitute : r->addMemory;
		if(!add || !add(o->host.user, data, size))
			return 0;
	}
	else
	{
		if(!FtFonts_AddMemory(Lib(o), data, size, !keep))
			return 0;
		if(o->host.fontFiles && o->host.fontFiles->added)
			o->host.fontFiles->added(o->host.user, data, size);
	}
	MemFamilies_Add(o, data, size, listed);
	return 1;
}

int Typefaces_AddMemory(OpenBGI_t* o, const void* data, uint32_t size, int keep)
{
	return AddData(o, (const uint8_t*)data, size, keep, 0, 0);
}

int Typefaces_AddSubstitute(OpenBGI_t* o, const void* data, uint32_t size)
{
	return AddData(o, (const uint8_t*)data, size, 1, 0, 1);
}

/* The renderer takes a file on disk by its path (AddFontResource), one
 * only a zip game's archive holds from memory; the engine's library takes
 * either as data, listed. */
int Typefaces_AddFile(OpenBGI_t* o, const char* path)
{
	const OpenBGI_FontRenderer_t* r = o->host.fontRenderer;
	uint32_t size;
	uint8_t* data;
	int ok;
	if(r && Host_FileAttrs(o, path) != OPENBGI_INVALID_ATTRS)
		return r->addFile ? r->addFile(o->host.user, path) : 0;
	data = Fs_LoadFile(o, path, &size);
	ok = data && AddData(o, data, size, 0, !r, 0);
	free(data);
	return ok;
}

int Typefaces_Exists(OpenBGI_t* o, const char* face)
{
	const OpenBGI_FontRenderer_t* r = o->host.fontRenderer;
	int known;
	if(r)
		return r->faceExists ? r->faceExists(o->host.user, face) : 1;
	if(MemFamilies_Has(o, face))
		return 1;
	known = HostFamily(o, face);
	return known < 0 || (known & (OPENBGI_FONT_FOUND | OPENBGI_FONT_UNKNOWN)) != 0;
}

int Typefaces_Installed(OpenBGI_t* o, const char* face)
{
	const OpenBGI_FontRenderer_t* r = o->host.fontRenderer;
	int known;
	if(MemFamilies_Has(o, face))
		return 1;
	if(r)
		return r->faceInstalled ? r->faceInstalled(o->host.user, face) : 0;
	known = HostFamily(o, face);
	return known > 0 && (known & (OPENBGI_FONT_FOUND | OPENBGI_FONT_FORCED)) != 0;
}

// ---- listing -----------------------------------------------------------------------

#define ENUM_SEEN 0x400 // family names remembered to list each once

typedef struct EnumCtx
{
	OpenBGI_t* o;
	char* buf;          // where the names go; NULL to count them
	int bytes, count;   // what was listed so far
	char (*seen)[0x40]; // the names already listed
	int nseen;
} EnumCtx_t;

/* one family (UTF-8) into the list, in the text encoding: left out when
 * Shift-JIS cannot spell it, when it is too long or listed already */
static void EnumOne(void* ctx, const char* family)
{
	EnumCtx_t* e = (EnumCtx_t*)ctx;
	char name[0x100];
	int n = 0, j;
	if(e->o->enc.textUtf8)
		n = snprintf(name, sizeof name, "%s", family);
	else
	{
		const uint8_t* u;
		for(u = (const uint8_t*)family; *u && n < (int)sizeof name - 3;)
		{
			int len, m;
			uint32_t cp = Utf8SeqLen(u) ? Utf8Decode(u, &len) : (len = 1, (uint32_t)*u);
			m = Sjis_FromCodePoint(cp, name + n);
			if(m == 0)
				return; // not spellable
			n += m;
			u += len;
		}
		name[n] = 0;
	}
	if(n <= 0 || n >= (int)sizeof name - 1)
		return;
	for(j = 0; j < e->nseen; j++)
		if(strncmp(e->seen[j], name, sizeof e->seen[j]) == 0)
			return;
	if(e->nseen < ENUM_SEEN)
		snprintf(e->seen[e->nseen++], sizeof e->seen[0], "%s", name);
	if(e->buf)
		memcpy(e->buf + e->bytes, name, (size_t)n + 1);
	e->bytes += n + 1;
	e->count++;
}

/* The host's families of the charset's language (ja for Shift-JIS,
 * zh-cn for GB2312, zh-tw for Big5, ko for Hangul, any for the rest),
 * then the families of the game's font files whose "OS/2" table names the
 * charset's code page (932, 936, 950, 949, 1252), each family once. */
int Typefaces_Enum(OpenBGI_t* o, char* buf, int charset, int* outBytes)
{
	const OpenBGI_FontRenderer_t* r = o->host.fontRenderer;
	const OpenBGI_FontFiles_t* files = o->host.fontFiles;
	const char* lang = charset == 0x80 ? "ja" : charset == 0x86 ? "zh-cn"
		: charset == 0x88                                       ? "zh-tw"
		: charset == 0x81                                       ? "ko"
																: NULL;
	uint32_t pages = charset == 0x80 ? 1u << 17 : charset == 0x86 ? 1u << 18
		: charset == 0x88                                         ? 1u << 20
		: charset == 0x81                                         ? 1u << 19
		: charset == 0                                            ? 1u << 0
																  : ~0u;
	EnumCtx_t e;
	const MemFamily_t* m;
	if(r)
	{
		if(r->enumFaces)
			return r->enumFaces(o->host.user, buf, charset, outBytes);
		*outBytes = 0;
		return 0;
	}
	memset(&e, 0, sizeof e);
	e.o = o;
	e.buf = buf;
	e.seen = (char(*)[0x40])BGI_Alloc((size_t)ENUM_SEEN * sizeof *e.seen);
	if(files && files->families)
		files->families(o->host.user, lang, EnumOne, &e);
	for(m = o->font.memFamilies; m; m = m->next)
		if(m->listed && (m->codePages & pages))
			EnumOne(&e, m->name);
	BGI_Free(e.seen);
	*outBytes = e.bytes;
	return e.count;
}

void Typefaces_Free(OpenBGI_t* o)
{
	while(o->font.typefaces)
		Typeface_Close(o->font.typefaces);
	FtFonts_Delete(o->font.ft);
	o->font.ft = NULL;
	o->font.ftTried = 0;
	while(o->font.memFamilies)
	{
		MemFamily_t* m = o->font.memFamilies;
		o->font.memFamilies = m->next;
		BGI_Free(m);
	}
}
