/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * font.c - glyphs for the WebAssembly back end, drawn by the canvas
 *
 * No FreeType and no font files: the browser's own Japanese fonts draw each
 * character into an off-screen canvas, and the coverage is read back
 * (getImageData) and thresholded into the 8-bit (OS_FontRender) or 1-bit
 * (OS_FontRenderMono) cell the engine asked for, like the GDI mono glyphs
 * the original takes before it box-filters them itself.
 *
 * The GDI sizing contract (os.h): `height` is the cell height, ascent plus
 * descent, and a full-width glyph is `2 * width` pixels wide.  The browser
 * reports the cell of a trial size through measureText's font bounding box,
 * which gives the pixel size to ask for, and the advance of "あ" gives the
 * horizontal scale.
 *
 * The interface is the font section of os.h.  Font files cannot be added
 * and no face list exists, so OS_FontAddFile / OS_FontAddMemory /
 * OS_FontEnumFaces report failure and OS_FontFaceExists says yes to every
 * name.  The WasmJs_* functions are the page side (EM_JS);
 * tools/wasm/fakebrowser.c stands in for them on a host.
 */
#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_wasm.h"

#include <emscripten/emscripten.h>

struct OsFont
{
	int id;            // the page-side record: an index into Module.bgiFonts
	int height, width; // the cell height and half-width advance asked for, in pixels
};

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
	var face = UTF8ToString(faceUtf8).toLowerCase();
	var serif = face.indexOf('mincho') >= 0 || face.indexOf('明朝') >= 0 || face.indexOf('ming') >= 0;
	var family = serif
		? '"MS Mincho", "MS PMincho", "Yu Mincho", "Hiragino Mincho ProN", "Noto Serif CJK JP", "Noto Serif JP", serif'
		: '"MS Gothic", "MS UI Gothic", "Yu Gothic", "Meiryo", "Hiragino Kaku Gothic ProN", "Noto Sans CJK JP", "Noto Sans JP", sans-serif';
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
});
// clang-format on

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
});
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
});
// clang-format on

int OS_FontAddFile(const char* path)
{
	BGI_UNUSED(path);
	return 0; // the page draws with the browser's fonts; the fallback faces apply
}

int OS_FontAddMemory(const void* data, uint32_t size)
{
	BGI_UNUSED(data);
	BGI_UNUSED(size);
	return 0;
}

int OS_FontFaceExists(const char* face)
{
	BGI_UNUSED(face);
	return 1; // every face resolves to one of the two stacks
}

// no face list on a page: no faces, no bytes
int OS_FontEnumFaces(char* buf, int charset, int* outBytes)
{
	BGI_UNUSED(buf);
	BGI_UNUSED(charset);
	*outBytes = 0;
	return 0;
}

/* The glyph font of os.h: the face name (text encoding) goes to the page as
 * UTF-8, `height` and `width` in pixels, `bold` and `italic` as flags.
 * NULL for a height of 0 or less or when out of memory. */
OsFont_t* OS_FontOpen(const char* face, int height, int width, int bold, int italic)
{
	char utf[0x200];
	OsFont_t* f;
	if(height <= 0)
		return NULL;
	f = (OsFont_t*)calloc(1, sizeof *f);
	if(!f)
		return NULL;
	OsWasm_SjisToUtf8(face ? face : "", utf, sizeof utf);
	f->id = WasmJs_FontOpen(utf, height, width, bold, italic);
	f->height = height;
	f->width = width;
	return f;
}

// the GDI text font: the same font with a half-width advance of size / 2, as CreateFontA(size, size / 2, ..)
OsFont_t* OS_FontOpenMono(const char* face, int size, int bold)
{
	return OS_FontOpen(face, size, size / 2, bold, 0);
}

void OS_FontClose(OsFont_t* f)
{
	free(f); // the page-side records are small and few; they stay
}

// the code point of a `len`-byte character in the text encoding (Shift-JIS or UTF-8)
static uint32_t CodePoint(const char* sjis, int len)
{
	return OsCommon_CodePoint(sjis, len, OsWasm_SjisChar);
}

// clear the w x h cell of the 8-bit buffer, then draw (NULL font: a blank cell)
void OS_FontRender(OsFont_t* f, const char* sjis, int len, uint8_t* buf, int pitch, int w, int h)
{
	int r;
	for(r = 0; r < h; r++)
		memset(buf + (size_t)r * (size_t)pitch, 0, (size_t)w);
	if(!f)
		return;
	WasmJs_FontRender(f->id, (int)CodePoint(sjis, len), buf, pitch, w, h, 0);
}

// the advance of the character in pixels (os.h); 0 without a font
int OS_FontCharAdvance(OsFont_t* f, const char* sjis, int len)
{
	if(!f)
		return 0;
	return WasmJs_FontAdvance(f->id, (int)CodePoint(sjis, len));
}

// clear the 1-bit bitmap (pitch * h bytes), then draw (NULL font: a blank cell)
void OS_FontRenderMono(OsFont_t* f, const char* sjis, int len, uint8_t* bits, int pitch, int w, int h)
{
	memset(bits, 0, (size_t)pitch * (size_t)h);
	if(!f)
		return;
	WasmJs_FontRender(f->id, (int)CodePoint(sjis, len), bits, pitch, w, h, 1);
}
