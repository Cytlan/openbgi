/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * font.c - glyphs for the WebAssembly back end, drawn by the canvas: the
 *          renderer the engine draws with on a page (OS_FontRenderer,
 *          src/host/os.h; OpenBGI_FontRenderer_t, openbgi.h)
 *
 * No FreeType and no font files: the browser's own Japanese fonts draw each
 * character into an off-screen canvas, and the coverage is read back
 * (getImageData) and thresholded into the 8-bit (FontRender) or 1-bit
 * (FontRenderMono) cell the engine asked for, like the GDI mono glyphs
 * the original takes before it box-filters them itself.
 *
 * The GDI sizing contract (openbgi.h): `height` is the cell height, ascent plus
 * descent, and a full-width glyph is `2 * width` pixels wide.  The browser
 * reports the cell of a trial size through measureText's font bounding box,
 * which gives the pixel size to ask for, and the advance of "あ" gives the
 * horizontal scale.
 *
 * A font handed over as data (FontAddMemory: a game's own, the ones built
 * into the binary) becomes a FontFace of the page under its family names,
 * which then come first in the stack of a font asked for by one of them.
 * Font files cannot be added and no face list exists, so FontAddFile and
 * FontEnumFaces report failure, FontFaceExists says yes to every name and
 * FontFaceInstalled only to the fonts handed over.  The WasmJs_*
 * functions are the page side (EM_JS); tools/wasm/fakebrowser.c stands in
 * for them on a host.
 */
#include "host/os.h"
#include "host/os_common.h"
#include "host/os_wasm.h"
#include "bgi/fontfile.h"
#include "bgi/sjis.h"

#include <emscripten/emscripten.h>

typedef struct OsFont
{
	int id;            // the page-side record: an index into Module.bgiFonts
	int height, width; // the cell height and half-width advance asked for, in pixels
} OsFont_t;

/* Open a font on the page: a record {font: the CSS font string, ascent: the
 * baseline's distance from the top of the cell, sx: the horizontal scale};
 * the result is its index in Module.bgiFonts.  The face name only decides
 * between the serif ("明朝", Mincho) and the sans stacks: a page cannot
 * promise a face, only a style.  The size is found by measuring a trial
 * size of 100 px: the font box's ascent and descent give the cell per pixel
 * of font size, so the size whose cell is `height` follows; the advance of
 * "あ" at that size, against `2 * width`, gives the scale (1 when `width`
 * is 0). */
// clang-format off
EM_JS(int, WasmJs_FontOpen, (const char* faceUtf8, int height, int width, int bold, int italic), {
	var name = UTF8ToString(faceUtf8);
	var face = name.toLowerCase();
	var serif = face.indexOf('mincho') >= 0 || face.indexOf('明朝') >= 0 || face.indexOf('ming') >= 0;
	var family = serif
		? '"MS Mincho", "MS PMincho", "Yu Mincho", "Hiragino Mincho ProN", "Noto Serif CJK JP", "Noto Serif JP", serif'
		: '"MS Gothic", "MS UI Gothic", "Yu Gothic", "Meiryo", "Hiragino Kaku Gothic ProN", "Noto Sans CJK JP", "Noto Sans JP", sans-serif';
	if(Module.bgiFontFaces && Module.bgiFontFaces[face])
	{ // a font handed over: its own face first (the quote as a character code: the C side must not see one)
		var dq = String.fromCharCode(34);
		family = dq + Module.bgiFontFaces[face].split(dq).join("") + dq + ', ' + family;
	}
	var style = (italic ? 'italic ' : "") + (bold ? 'bold ' : "");
	if(!Module.bgiFontCanvas)
	{
		Module.bgiFontCanvas = document.createElement('canvas');
		Module.bgiFontCanvas.width = 256;
		Module.bgiFontCanvas.height = 256;
		Module.bgiFonts = [];
	}
	var ctx = Module.bgiFontCanvas.getContext('2d', { willReadFrequently: true });
	var trial = 100;
	ctx.setTransform(1, 0, 0, 1, 0, 0);
	ctx.font = style + trial + 'px ' + family;
	var m = ctx.measureText('あ');
	var asc = m.fontBoundingBoxAscent, desc = m.fontBoundingBoxDescent;
	if(!(asc > 0) || !(desc >= 0))
	{ // an older browser without the font box: typical CJK proportions
		asc = trial * 0.88;
		desc = trial * 0.12;
	}
	var cell = asc + desc;
	var px = height * trial / cell; // the font size whose cell is `height`
	var ascent = height * asc / cell;
	ctx.font = style + px.toFixed(2) + 'px ' + family;
	var adv = ctx.measureText('あ').width;
	if(!(adv > 0))
		adv = px;
	var sx = width > 0 ? (2 * width) / adv : 1; // a full-width glyph must be 2 * width wide
	Module.bgiFonts.push({ font: ctx.font, ascent: ascent, sx: sx });
	return Module.bgiFonts.length - 1;
})
// clang-format on

/* A font's data as a FontFace of the page under each of its family names
 * (`names`: UTF-8, one after the other, NUL-terminated, an empty one at the
 * end), waited for until it has loaded: 1 when the page took it. */
// clang-format off
EM_ASYNC_JS(int, WasmJs_FontAdd, (const void* data, uint32_t size, const char* names), {
	if(typeof FontFace === 'undefined' || typeof document === 'undefined' || !document.fonts)
		return 0;
	var bytes = HEAPU8.slice(data, data + size);
	var ok = 0;
	for(var p = names; HEAPU8[p];)
	{
		var q = p;
		while(HEAPU8[q])
			q++;
		var name = UTF8ToString(p, q - p);
		p = q + 1;
		try
		{
			var ff = new FontFace(name.split(String.fromCharCode(34)).join(""), bytes);
			await ff.load();
			document.fonts.add(ff);
			Module.bgiFontFaces = Module.bgiFontFaces || {};
			Module.bgiFontFaces[name.toLowerCase()] = name;
			ok = 1;
		}
		catch(e)
		{
			console.warn('openbgi: font "' + name + '" not taken:', e);
		}
	}
	return ok;
})
// clang-format on

// the faces of the fonts handed over (FontFaceInstalled), UTF-8 in lower case
static char gFaces[64][FONTFILE_NAME_MAX];
static int gFaceCount;

/* Draw code point `cp` at the top-left of a w x h cell and write the
 * coverage into `buf` (pitch bytes per row): with `mono` 0 a byte per
 * pixel, 0 or 255; with 1 a bit per pixel, MSB first.  A pixel is ink when
 * the canvas's alpha is at least 128.  The cell is only written where there
 * is ink: the caller clears the buffer first. */
// clang-format off
EM_JS(void, WasmJs_FontRender, (int id, int cp, unsigned char* buf, int pitch, int w, int h, int mono), {
	var rec = Module.bgiFonts && Module.bgiFonts[id];
	if(!rec || w <= 0 || h <= 0)
		return;
	var cv = Module.bgiFontCanvas;
	if(cv.width < w || cv.height < h)
	{ // the off-screen canvas grows to the largest cell asked for
		cv.width = Math.max(cv.width, w);
		cv.height = Math.max(cv.height, h);
	}
	var ctx = cv.getContext('2d', { willReadFrequently: true });
	ctx.setTransform(1, 0, 0, 1, 0, 0);
	ctx.clearRect(0, 0, w, h);
	ctx.font = rec.font;
	ctx.fillStyle = '#000';
	ctx.textBaseline = 'alphabetic';
	ctx.textAlign = 'left';
	ctx.setTransform(rec.sx, 0, 0, 1, 0, 0); // the horizontal scale applies to the drawing only
	ctx.fillText(String.fromCodePoint(cp), 0, rec.ascent);
	ctx.setTransform(1, 0, 0, 1, 0, 0);
	var data = ctx.getImageData(0, 0, w, h).data;
	for(var y = 0; y < h; y++)
	{
		var row = buf + y * pitch;
		for(var x = 0; x < w; x++)
		{
			var ink = data[(y * w + x) * 4 + 3] >= 128; // the alpha channel of the RGBA pixel
			if(mono)
			{
				if(ink)
					HEAPU8[row + (x >> 3)] |= 0x80 >> (x & 7);
			}
			else if(ink)
				HEAPU8[row + x] = 255;
		}
	}
})
// clang-format on

/* The advance of code point `cp` in the font, in pixels: measureText's
 * width with the font's horizontal scale applied, rounded to the nearest
 * pixel.  0 for an unknown font. */
// clang-format off
EM_JS(int, WasmJs_FontAdvance, (int id, int cp), {
	var rec = Module.bgiFonts && Module.bgiFonts[id];
	if(!rec)
		return 0;
	var ctx = Module.bgiFontCanvas.getContext('2d', { willReadFrequently: true });
	ctx.setTransform(1, 0, 0, 1, 0, 0);
	ctx.font = rec.font;
	return Math.round(ctx.measureText(String.fromCodePoint(cp)).width * rec.sx);
})
// clang-format on

static int FontAddFile(const char* path)
{
	BGI_UNUSED(path);
	return 0; // the page draws with the browser's fonts; the fallback faces apply
}

// the family names of the first face (a page takes no other of a collection) to the page
static int FontAddMemory(const void* data, uint32_t size)
{
	char names[8][FONTFILE_NAME_MAX];
	char list[8 * FONTFILE_NAME_MAX + 1];
	int n = FontFile_Families((const uint8_t*)data, size, 0, names, 8), i;
	size_t o = 0;
	if(n > 8)
		n = 8;
	for(i = 0; i < n; i++)
	{
		size_t len = strlen(names[i]);
		memcpy(list + o, names[i], len + 1);
		o += len + 1;
	}
	list[o] = 0;
	if(n <= 0 || !WasmJs_FontAdd(data, size, list))
		return 0;
	for(i = 0; i < n && gFaceCount < (int)BGI_COUNTOF(gFaces); i++)
	{
		char* p;
		snprintf(gFaces[gFaceCount], sizeof gFaces[0], "%s", names[i]);
		for(p = gFaces[gFaceCount]; *p; p++)
			if(*p >= 'A' && *p <= 'Z')
				*p = (char)(*p + 32);
		gFaceCount++;
	}
	return 1;
}

static int FontFaceExists(const char* face)
{
	BGI_UNUSED(face);
	return 1; // every face resolves to one of the two stacks
}

// what the page has for certain: the fonts handed over
static int FontFaceInstalled(const char* face)
{
	char utf[0x200];
	char* p;
	int i;
	OsCommon_SjisToUtf8(face, utf, sizeof utf);
	for(p = utf; *p; p++)
		if(*p >= 'A' && *p <= 'Z')
			*p = (char)(*p + 32);
	for(i = 0; i < gFaceCount; i++)
		if(strcmp(gFaces[i], utf) == 0)
			return 1;
	return 0;
}

// no face list on a page: no faces, no bytes
static int FontEnumFaces(char* buf, int charset, int* outBytes)
{
	BGI_UNUSED(buf);
	BGI_UNUSED(charset);
	*outBytes = 0;
	return 0;
}

/* The glyph font: the face name (text encoding) goes to the page as
 * UTF-8, `height` and `width` in pixels, `bold` and `italic` as flags.
 * NULL for a height of 0 or less or when out of memory. */
static OsFont_t* FontOpen(const char* face, int height, int width, int bold, int italic)
{
	char utf[0x200];
	OsFont_t* f;
	if(height <= 0)
		return NULL;
	f = (OsFont_t*)calloc(1, sizeof *f);
	if(!f)
		return NULL;
	OsCommon_SjisToUtf8(face ? face : "", utf, sizeof utf);
	f->id = WasmJs_FontOpen(utf, height, width, bold, italic);
	f->height = height;
	f->width = width;
	return f;
}

// the GDI text font: the same font with a half-width advance of size / 2, as CreateFontA(size, size / 2, ..)
static OsFont_t* FontOpenMono(const char* face, int size, int bold)
{
	return FontOpen(face, size, size / 2, bold, 0);
}

static void FontClose(OsFont_t* f)
{
	free(f); // the page-side records are small and few; they stay
}

// the code point of a `len`-byte character in the text encoding (Shift-JIS or UTF-8)
static uint32_t CodePoint(const char* sjis, int len)
{
	return OsCommon_CodePoint(sjis, len);
}

// clear the w x h cell of the 8-bit buffer, then draw (NULL font: a blank cell)
static void FontRender(OsFont_t* f, const char* sjis, int len, uint8_t* buf, int pitch, int w, int h)
{
	int r;
	for(r = 0; r < h; r++)
		memset(buf + (size_t)r * (size_t)pitch, 0, (size_t)w);
	if(!f)
		return;
	WasmJs_FontRender(f->id, (int)CodePoint(sjis, len), buf, pitch, w, h, 0);
}

// the advance of the character in pixels; 0 without a font
static int FontCharAdvance(OsFont_t* f, const char* sjis, int len)
{
	if(!f)
		return 0;
	return WasmJs_FontAdvance(f->id, (int)CodePoint(sjis, len));
}

// clear the 1-bit bitmap (pitch * h bytes), then draw (NULL font: a blank cell)
static void FontRenderMono(OsFont_t* f, const char* sjis, int len, uint8_t* bits, int pitch, int w, int h)
{
	memset(bits, 0, (size_t)pitch * (size_t)h);
	if(!f)
		return;
	WasmJs_FontRender(f->id, (int)CodePoint(sjis, len), bits, pitch, w, h, 1);
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
	RendererEnumFaces, NULL};

// the engine draws with the page's canvas here
const OpenBGI_FontRenderer_t* OS_FontRenderer(void)
{
	return &kRenderer;
}

const OpenBGI_FontFiles_t* OS_FontFiles(void)
{
	return NULL;
}
