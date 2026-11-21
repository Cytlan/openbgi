/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * font.c - the Win32 back end: glyph rendering with GDI, the renderer
 *          the engine draws with here (OS_FontRenderer, src/host/os.h;
 *          OpenBGI_FontRenderer_t, openbgi.h)
 *
 * The original's text renderer draws each character with TextOutA into an
 * 8-bit DIB section whose 256-entry palette runs from white (index 0) to
 * black (index 255), at a supersampled size, and box-filters the result
 * itself (src/gfx/font.c).  The GDI text instruction draws into a 1-bit
 * DIB with DRAFT_QUALITY (src/gfx/monofont.c).  Both are reproduced here:
 * an OsFont_t is a GDI font selected into a memory DC with a DIB section
 * of either depth, kept per font and grown when a larger cell is asked
 * for.  FontRender / FontRenderMono copy the DIB's rows into the
 * caller's buffer after each TextOut.
 *
 * The calls are the wide-character ones: a face name is converted from
 * the engine's Shift-JIS before CreateFontW looks it up (the ANSI call
 * would read "ＭＳ ゴシック" (MS Gothic) in the system's code page and miss the font on
 * a system that is not Japanese), and a character is converted with code
 * page 932 - what TextOutA does for a SHIFTJIS_CHARSET font - and drawn
 * with TextOutW.
 */
#include "host/os_win32.h"

#include "host/os.h"
#include "host/os_common.h"
#include "bgi/fontfile.h"

typedef struct OsFont
{
	HFONT font;     // the GDI font (CreateFontW)
	HDC dc;         // a memory DC with the DIB and the font selected
	HBITMAP dib;    // the DIB section, NULL until the first render
	void* bits;     // its pixels (top-down)
	int w, h;       // its size (pixels)
	int bpp;        // 8 (FontOpen) or 1 (FontOpenMono, or after FontRenderMono)
	int pitch;      // bytes per DIB row
	HGDIOBJ oldBmp; // what the DC had before the DIB was selected
} OsFont_t;

// the record for a GDI font with a memory DC it is selected into; NULL (the font deleted) when the font or the DC is missing
static OsFont_t* FontNew(HFONT font, int bpp)
{
	OsFont_t* f;
	if(!font)
		return NULL;
	f = (OsFont_t*)BGI_Alloc(sizeof *f);
	memset(f, 0, sizeof *f);
	f->font = font;
	f->bpp = bpp;
	f->dc = CreateCompatibleDC(NULL);
	if(!f->dc)
	{
		DeleteObject(font);
		BGI_Free(f);
		return NULL;
	}
	SelectObject(f->dc, font);
	return f;
}

/* (Re)create the DIB when a w x h cell does not fit the current one; the
 * new DIB is at least as large as the old in both directions.  The 8-bit
 * palette runs white to black and the DC draws black on an opaque white
 * background; the 1-bit palette is black / white and the DC draws white
 * transparently, so a set bit is ink.  0 when GDI refuses the DIB (the old
 * one, if any, stays). */
static int FontEnsureDib(OsFont_t* f, int w, int h)
{
	struct
	{
		BITMAPINFOHEADER bi;
		RGBQUAD pal[256];
	} bmi;
	HBITMAP dib;
	void* bits = NULL;
	int i;

	if(f->dib && w <= f->w && h <= f->h)
		return 1;
	if(w < f->w)
		w = f->w;
	if(h < f->h)
		h = f->h;

	memset(&bmi, 0, sizeof bmi);
	bmi.bi.biSize = sizeof bmi.bi;
	bmi.bi.biWidth = f->bpp == 1 ? (w + 31) & ~31 : w; // the original rounds the 1-bit DIB up to 32 pixels
	bmi.bi.biHeight = -h;                              // top-down
	bmi.bi.biPlanes = 1;
	bmi.bi.biBitCount = (WORD)f->bpp;
	bmi.bi.biCompression = BI_RGB;
	if(f->bpp == 8)
	{
		// index 0 white.. index 255 black
		for(i = 0; i < 256; i++)
		{
			bmi.pal[i].rgbRed = bmi.pal[i].rgbGreen = bmi.pal[i].rgbBlue = (BYTE)(255 - i);
			bmi.pal[i].rgbReserved = 0;
		}
		bmi.bi.biClrUsed = 256;
	}
	else
	{
		// a set bit is ink: index 1 is the text colour
		bmi.pal[0].rgbRed = bmi.pal[0].rgbGreen = bmi.pal[0].rgbBlue = 0;
		bmi.pal[1].rgbRed = bmi.pal[1].rgbGreen = bmi.pal[1].rgbBlue = 255;
		bmi.bi.biClrUsed = 2;
	}
	dib = CreateDIBSection(f->dc, (const BITMAPINFO*)&bmi, DIB_RGB_COLORS, &bits, NULL, 0);
	if(!dib || !bits)
	{
		if(dib)
			DeleteObject(dib);
		return 0;
	}
	if(f->dib)
	{
		SelectObject(f->dc, f->oldBmp);
		DeleteObject(f->dib);
	}
	f->oldBmp = SelectObject(f->dc, dib);
	f->dib = dib;
	f->bits = bits;
	f->w = w;
	f->h = h;
	f->pitch = ((bmi.bi.biWidth * f->bpp + 31) / 32) * 4;
	if(f->bpp == 8)
	{
		SetBkColor(f->dc, RGB(255, 255, 255)); // palette 0
		SetTextColor(f->dc, RGB(0, 0, 0));     // palette 255
		SetBkMode(f->dc, OPAQUE);
	}
	else
	{
		SetTextColor(f->dc, RGB(255, 255, 255)); // palette 1
		SetBkMode(f->dc, TRANSPARENT);
	}
	return 1;
}

// "B0 C2" of 1.69/472 on: AddFontResource; 1 when the file added at least one font
static int FontAddFile(const char* path)
{
	WCHAR wide[WIN32_WPATH];
	Win32_ToWide(path, wide, (int)BGI_COUNTOF(wide));
	return AddFontResourceW(wide) > 0;
}

// the enumeration callback of FontFaceExists: one match is enough
static int CALLBACK FaceExistsProc(const LOGFONTW* lf, const TEXTMETRICW* tm, DWORD type, LPARAM lp)
{
	BGI_UNUSED(lf);
	BGI_UNUSED(tm);
	BGI_UNUSED(type);
	*(int*)lp = 1;
	return 0;
}

/* 1 when a font family of exactly this name is installed, in any charset
 * (EnumFontFamiliesEx with the face name and DEFAULT_CHARSET).  The
 * original answers the question by creating the font and comparing the
 * family name GetOutlineTextMetrics reports; here the name is looked up
 * directly. */
static int FontFaceExists(const char* face)
{
	LOGFONTW lf;
	HDC dc = GetDC(NULL);
	int found = 0;
	memset(&lf, 0, sizeof lf);
	lf.lfCharSet = DEFAULT_CHARSET;
	Win32_ToWide(face, lf.lfFaceName, (int)BGI_COUNTOF(lf.lfFaceName));
	EnumFontFamiliesExW(dc, &lf, (FONTENUMPROCW)FaceExistsProc, (LPARAM)&found, 0);
	ReleaseDC(NULL, dc);
	return found;
}

// GDI's answer is exact: the same enumeration (the fonts from memory, which it does not list, the engine knows itself)
static int FontFaceInstalled(const char* face)
{
	return FontFaceExists(face);
}

// the enumeration of "B0 C4" (1.588 on) / "B0 C5" (1.653 on): every family of the charset appended to the buffer
typedef struct EnumFacesCtx
{
	int count; // families seen
	char* buf; // where the next name goes; NULL to count only
	int bytes; // bytes the names take altogether (each with its NUL)
} EnumFacesCtx_t;

// the enumeration callback: append the family name (in the text encoding, with its NUL) when there is a buffer, count it either way
static int CALLBACK EnumFacesProc(const LOGFONTW* lf, const TEXTMETRICW* tm, DWORD type, LPARAM lp)
{
	EnumFacesCtx_t* c = (EnumFacesCtx_t*)lp;
	char name[0x100]; // 31 UTF-16 units come to less than this in either encoding
	int n = Win32_TextFromWide(lf->lfFaceName, name, (int)sizeof name) + 1;
	BGI_UNUSED(tm);
	BGI_UNUSED(type);
	c->count++;
	if(c->buf)
	{
		memcpy(c->buf, name, (size_t)n);
		c->buf += n;
	}
	c->bytes += n;
	return 1;
}

/* The installed family names of a charset (the Windows charset number:
 * 0x80 Shift-JIS, 0 ANSI, ..), each NUL-terminated into buf when it is
 * not NULL (the caller sizes it from a counting call), their total bytes
 * in *outBytes; the number of families.  The names are in the text
 * encoding: Shift-JIS, or UTF-8 in UTF-8 text mode. */
static int FontEnumFaces(char* buf, int charset, int* outBytes)
{
	LOGFONTW lf;
	EnumFacesCtx_t c;
	HDC dc = GetDC(NULL);
	memset(&lf, 0, sizeof lf);
	lf.lfCharSet = (BYTE)charset;
	c.count = 0;
	c.buf = buf;
	c.bytes = 0;
	EnumFontFamiliesExW(dc, &lf, (FONTENUMPROCW)EnumFacesProc, (LPARAM)&c, 0);
	ReleaseDC(NULL, dc);
	*outBytes = c.bytes;
	return c.count;
}

/* The engine's own fonts (built into the binary, or the player's) stand
 * in for MS Gothic and MS Mincho, whose average character is half as
 * wide as a full-width one: that is the `width` the engine asks for, and
 * CreateFont's for them.  GDI makes any font's average character that
 * wide, and the average of a font that counts its kanji in (its "OS/2"
 * table says which) is most of a full-width character: asked for the
 * same way it comes out squeezed, to three fifths with the built-in
 * fonts.  So the families of the engine's fonts are remembered here
 * (FontAddSubstitute), and a font of one is asked for at the width GDI
 * gives its average character when a full-width one is 2 * width wide
 * (OwnWidth).  A game's own font ("B0 C3") is opened as the original
 * opens it. */
typedef struct OwnFace
{
	WCHAR name[LF_FACESIZE];
	struct OwnFace* next;
} OwnFace_t;

static OwnFace_t* gOwnFaces; // the families of the engine's fonts handed to GDI

static int OwnFace_Is(const WCHAR* face)
{
	const OwnFace_t* f;
	for(f = gOwnFaces; f; f = f->next)
		if(lstrcmpiW(f->name, face) == 0)
			return 1;
	return 0;
}

// remember the families of every face of the font in `data`
static void OwnFace_Add(const void* data, uint32_t size)
{
	int faces = FontFile_Faces((const uint8_t*)data, size), i, k;
	for(i = 0; i < faces; i++)
	{
		char names[16][FONTFILE_NAME_MAX];
		int n = FontFile_Families((const uint8_t*)data, size, i, names, 16);
		for(k = 0; k < n && k < 16; k++)
		{
			WCHAR wide[LF_FACESIZE];
			OwnFace_t* f;
			if(!MultiByteToWideChar(CP_UTF8, 0, names[k], -1, wide, LF_FACESIZE) || OwnFace_Is(wide))
				continue;
			f = (OwnFace_t*)BGI_Alloc(sizeof *f);
			lstrcpyW(f->name, wide);
			f->next = gOwnFaces;
			gOwnFaces = f;
		}
	}
}

/* CreateFont's width for a font of `face` whose half-width character is
 * to be `width` wide: `width` itself, but for a font of the engine's the
 * width of its average character at that scale - GDI's own figures of the font
 * at its natural proportions (width 0), the kanji U+56FD for the
 * full-width character */
static int OwnWidth(const WCHAR* face, int height, int width, int weight, int italic)
{
	static const WCHAR kFull[] = {0x56fd, 0};
	HFONT font;
	HDC dc;
	int out = width;
	if(width <= 0 || !OwnFace_Is(face))
		return width;
	font = CreateFontW(height, 0, 0, 0, weight, italic, 0, 0, SHIFTJIS_CHARSET, OUT_TT_PRECIS, 0, PROOF_QUALITY, FIXED_PITCH, face);
	dc = CreateCompatibleDC(NULL);
	if(font && dc)
	{
		HGDIOBJ old = SelectObject(dc, font);
		TEXTMETRICW tm;
		SIZE full;
		if(GetTextMetricsW(dc, &tm) && GetTextExtentPoint32W(dc, kFull, 1, &full) && tm.tmAveCharWidth > 0 && full.cx > 0)
			out = (int)(((int64_t)tm.tmAveCharWidth * 2 * width + full.cx / 2) / full.cx);
		SelectObject(dc, old);
	}
	if(dc)
		DeleteDC(dc);
	if(font)
		DeleteObject(font);
	return out > 0 ? out : 1;
}

// "B0 C3" of 1.492 on: AddFontMemResourceEx of a font file in memory (the data may be freed afterwards); 1 = ok
static int FontAddMemory(const void* data, uint32_t size)
{
	DWORD n = 0;
	return AddFontMemResourceEx((void*)data, size, NULL, &n) != NULL;
}

// a font of the engine's own: the same, and its faces are opened at the font's own proportions from then on
static int FontAddSubstitute(const void* data, uint32_t size)
{
	if(!FontAddMemory(data, size))
		return 0;
	OwnFace_Add(data, size);
	return 1;
}

/* The glyph rasteriser's font: CreateFont(height, width, 0, 0, FW_THIN /
 * FW_BOLD, italic, 0, 0, SHIFTJIS_CHARSET, OUT_TT_PRECIS, 0, PROOF_QUALITY,
 * FIXED_PITCH, face) over an 8-bit DIB; NULL when the font cannot be
 * created. */
static OsFont_t* FontOpen(const char* face, int height, int width, int bold, int italic)
{
	WCHAR wface[0x100];
	HFONT font;
	Win32_ToWide(face, wface, (int)BGI_COUNTOF(wface));
	width = OwnWidth(wface, height, width, bold ? FW_BOLD : FW_THIN, italic ? 1 : 0);
	font = CreateFontW(height, width, 0, 0, bold ? FW_BOLD : FW_THIN, italic ? 1 : 0, 0, 0, SHIFTJIS_CHARSET,
		OUT_TT_PRECIS, 0, PROOF_QUALITY, FIXED_PITCH, wface);
	return FontNew(font, 8);
}

/* the GDI text instruction's font - CreateFont(size, size / 2, 0, 0,
 * FW_NORMAL / FW_BOLD, 0, 0, 0, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS, 0,
 * DRAFT_QUALITY, FIXED_PITCH | FF_ROMAN, face) - over a 1-bit DIB */
static OsFont_t* FontOpenMono(const char* face, int size, int bold)
{
	WCHAR wface[0x100];
	HFONT font;
	Win32_ToWide(face, wface, (int)BGI_COUNTOF(wface));
	font = CreateFontW(size, OwnWidth(wface, size, size / 2, bold ? FW_BOLD : FW_NORMAL, 0), 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0,
		SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS, 0, DRAFT_QUALITY, FIXED_PITCH | FF_ROMAN, wface);
	return FontNew(font, 1);
}

// the DIB (deselected first), the DC, the font and the record; accepts NULL
static void FontClose(OsFont_t* f)
{
	if(!f)
		return;
	if(f->dc)
	{
		if(f->dib)
		{
			SelectObject(f->dc, f->oldBmp);
			DeleteObject(f->dib);
		}
		DeleteDC(f->dc);
	}
	if(f->font)
		DeleteObject(f->font);
	BGI_Free(f);
}

/* The character (`len` bytes) as UTF-16 into wide[2]; the units, 1 or 2.
 * Shift-JIS bytes go through code page 932, which is the conversion
 * TextOutA makes for a SHIFTJIS_CHARSET font; with the text encoding set
 * to UTF-8 the bytes are decoded to one code point, as the original's
 * 1.653+ renderer does with MultiByteToWideChar(65001) (a surrogate pair
 * above U+FFFF). */
static int FontCharWide(const char* sjis, int len, WCHAR* wide)
{
	int wn;
	if(gOsTextUtf8)
	{
		uint32_t cp = OsCommon_CodePoint(sjis, len);
		if(cp >= 0x10000)
		{
			wide[0] = (WCHAR)(0xd800 + ((cp - 0x10000) >> 10));
			wide[1] = (WCHAR)(0xdc00 + ((cp - 0x10000) & 0x3ff));
			return 2;
		}
		wide[0] = (WCHAR)cp;
		return 1;
	}
	wn = MultiByteToWideChar(WIN32_CP_SJIS, 0, sjis, len, wide, 2);
	if(wn <= 0)
	{ // an empty or over-long character, or no code page 932: the first byte as it is
		wide[0] = (WCHAR)(len > 0 ? (uint8_t)sjis[0] : ' ');
		wn = 1;
	}
	return wn;
}

/* Clear the DIB and draw the character at its top-left corner with
 * TextOutW.  0 when the DIB cannot be made large enough for w x h. */
static int FontDraw(OsFont_t* f, const char* sjis, int len, int w, int h)
{
	WCHAR wide[2];
	int y, wn;
	if(!FontEnsureDib(f, w, h))
		return 0;
	for(y = 0; y < h; y++)
		memset((uint8_t*)f->bits + (size_t)y * (size_t)f->pitch, 0, (size_t)f->pitch);
	wn = FontCharWide(sjis, len, wide);
	TextOutW(f->dc, 0, 0, wide, wn);
	GdiFlush(); // the DIB bits are only valid once GDI has finished drawing
	return 1;
}

/* The w x h cell of 8-bit coverage into buf (pitch bytes per row): the
 * buffer is cleared, then the character drawn and the DIB's rows copied
 * over, 0 blank .. 255 ink.  A NULL font or a failed draw leaves the
 * cleared buffer. */
static void FontRender(OsFont_t* f, const char* sjis, int len, uint8_t* buf, int pitch, int w, int h)
{
	int y;
	for(y = 0; y < h; y++)
		memset(buf + (size_t)y * (size_t)pitch, 0, (size_t)w);
	if(!f || !FontDraw(f, sjis, len, w, h))
		return;
	for(y = 0; y < h; y++)
		memcpy(buf + (size_t)y * (size_t)pitch, (const uint8_t*)f->bits + (size_t)y * (size_t)f->pitch, (size_t)w);
}

// GetTextExtentPoint32 of the character: its advance in pixels; 0 without a font or when GDI fails
static int FontCharAdvance(OsFont_t* f, const char* sjis, int len)
{
	WCHAR wide[2];
	SIZE sz;
	if(!f || !f->dc)
		return 0;
	sz.cx = 0;
	if(!GetTextExtentPoint32W(f->dc, wide, FontCharWide(sjis, len, wide), &sz))
		return 0;
	return (int)sz.cx;
}

/* The monochrome variant: the w x h cell as 1-bit rows (MSB first, pitch
 * bytes per row, a set bit is ink) into bits.  A font from FontOpenMono
 * is expected; any other is switched to a 1-bit DIB for good (its 8-bit
 * one is dropped). */
static void FontRenderMono(OsFont_t* f, const char* sjis, int len, uint8_t* bits, int pitch, int w, int h)
{
	int y, rowBytes = (w + 7) / 8;
	for(y = 0; y < h; y++)
		memset(bits + (size_t)y * (size_t)pitch, 0, (size_t)rowBytes);
	if(!f)
		return;
	if(f->bpp != 1)
	{
		if(f->dib)
		{
			SelectObject(f->dc, f->oldBmp);
			DeleteObject(f->dib);
			f->dib = NULL;
			f->w = f->h = 0;
		}
		f->bpp = 1;
	}
	if(!FontDraw(f, sjis, len, w, h))
		return;
	for(y = 0; y < h; y++)
		memcpy(bits + (size_t)y * (size_t)pitch, (const uint8_t*)f->bits + (size_t)y * (size_t)f->pitch, (size_t)rowBytes);
}

// ---- the renderer (OS_FontRenderer) -------------------------------------------------

static void* RendererOpen(void* user, const char* face, int height, int width, int bold, int italic)
{
	BGI_UNUSED(user);
	return FontOpen(face, height, width, bold, italic);
}

static void* RendererOpenMono(void* user, const char* face, int size, int bold)
{
	BGI_UNUSED(user);
	return FontOpenMono(face, size, bold);
}

static void RendererClose(void* user, void* font)
{
	BGI_UNUSED(user);
	FontClose((OsFont_t*)font);
}

static void RendererRender(void* user, void* font, const char* ch, int len, uint8_t* buf, int pitch, int w, int h)
{
	BGI_UNUSED(user);
	FontRender((OsFont_t*)font, ch, len, buf, pitch, w, h);
}

static void RendererRenderMono(void* user, void* font, const char* ch, int len, uint8_t* bits, int pitch, int w, int h)
{
	BGI_UNUSED(user);
	FontRenderMono((OsFont_t*)font, ch, len, bits, pitch, w, h);
}

static int RendererAdvance(void* user, void* font, const char* ch, int len)
{
	BGI_UNUSED(user);
	return FontCharAdvance((OsFont_t*)font, ch, len);
}

static int RendererAddFile(void* user, const char* path)
{
	BGI_UNUSED(user);
	return FontAddFile(path);
}

static int RendererAddMemory(void* user, const void* data, uint32_t size)
{
	BGI_UNUSED(user);
	return FontAddMemory(data, size);
}

static int RendererAddSubstitute(void* user, const void* data, uint32_t size)
{
	BGI_UNUSED(user);
	return FontAddSubstitute(data, size);
}

static int RendererFaceExists(void* user, const char* face)
{
	BGI_UNUSED(user);
	return FontFaceExists(face);
}

static int RendererFaceInstalled(void* user, const char* face)
{
	BGI_UNUSED(user);
	return FontFaceInstalled(face);
}

static int RendererEnumFaces(void* user, char* buf, int charset, int* bytes)
{
	BGI_UNUSED(user);
	return FontEnumFaces(buf, charset, bytes);
}

static const OpenBGI_FontRenderer_t kRenderer = {sizeof(OpenBGI_FontRenderer_t), RendererOpen, RendererOpenMono, RendererClose, RendererRender,
	RendererRenderMono, RendererAdvance, RendererAddFile, RendererAddMemory, RendererFaceExists, RendererFaceInstalled,
	RendererEnumFaces, RendererAddSubstitute};

// the engine draws with GDI here, as the original
const OpenBGI_FontRenderer_t* OS_FontRenderer(void)
{
	return &kRenderer;
}

const OpenBGI_FontFiles_t* OS_FontFiles(void)
{
	return NULL;
}
