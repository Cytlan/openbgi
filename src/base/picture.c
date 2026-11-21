/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * picture.c - picture files outside the archives for "90 C4" / "90 C5"
 *             (picture.h): recognising a file by its content, the BMP and
 *             GIF readers, PNG through png.h, JPEG in jpeg.c, and the BMP
 *             and PNG writers
 *
 * The kind each reader reports is the pixel format GDI+ gives such a file
 * (some of these are educated guesses): a 24-bit BMP,
 * a colour JPEG or an opaque colour PNG is 24-bit colour; a BMP with an
 * alpha mask, a PNG with alpha or a transparency chunk, and a grey PNG
 * are 32-bit with alpha; an 8-bit BMP, an 8-bit palette PNG, a GIF and a
 * grey JPEG are indexed; 1- and 4-bit indices, 16 bits per pixel, a
 * 32-bit BMP without alpha and 16-bit PNG samples are formats "90 C4"
 * mode -1 refuses, though the other modes read them.
 */
#include "bgi/picture.h"
#include "bgi/bytes.h"
#include "bgi/png.h"

#include <stdlib.h>
#include <string.h>

// a picture of w x h: zeroed pixels, the index plane when `indexed`; 0 when it is too large
static int NewPicture(Picture_t* out, uint32_t w, uint32_t h, PicKind_t kind, int indexed)
{
	memset(out, 0, sizeof *out);
	if(w == 0 || h == 0 || (uint64_t)w * h > 0x2000000u)
		return 0;
	out->width = w;
	out->height = h;
	out->kind = kind;
	out->bgra = (uint8_t*)BGI_Calloc((size_t)w * h * 4);
	if(indexed)
		out->index = (uint8_t*)BGI_Calloc((size_t)w * h);
	return 1;
}

void Picture_Free(Picture_t* p)
{
	BGI_Free(p->bgra);
	BGI_Free(p->index);
	p->bgra = p->index = NULL;
}

// ---- BMP ----------------------------------------------------------------------------

// the position of the lowest set bit of a mask and its width, for the bit-field formats
static void MaskShift(uint32_t m, int* shift, int* bits)
{
	*shift = *bits = 0;
	if(!m)
		return;
	while(!(m & 1))
	{
		m >>= 1;
		(*shift)++;
	}
	while(m & 1)
	{
		m >>= 1;
		(*bits)++;
	}
}

// a field of a pixel scaled to 8 bits
static uint8_t Field(uint32_t px, uint32_t mask, int shift, int bits)
{
	uint32_t v;
	if(!mask || !bits)
		return 0;
	v = (px & mask) >> shift;
	if(bits >= 8)
		return (uint8_t)(v >> (bits - 8));
	return (uint8_t)((v * 255u + ((1u << bits) - 1u) / 2u) / ((1u << bits) - 1u));
}

/* the RLE4 / RLE8 data of an indexed BMP into the index plane (rows from
 * the bottom); what is not painted stays index 0 */
static void DecodeRle(const uint8_t* s, const uint8_t* end, uint8_t* idx, uint32_t w, uint32_t h, int four)
{
	uint32_t x = 0, y = 0;
	while(s + 1 < end && y < h)
	{
		uint32_t n = s[0], c = s[1];
		s += 2;
		if(n)
		{ // a run
			uint32_t i;
			for(i = 0; i < n && x < w; i++, x++)
				idx[(size_t)(h - 1 - y) * w + x] = (uint8_t)(four ? (i & 1 ? c & 15 : c >> 4) : c);
		}
		else if(c == 0)
			x = 0, y++; // end of line
		else if(c == 1)
			break; // end of bitmap
		else if(c == 2)
		{ // delta
			if(s + 1 >= end)
				break;
			x += s[0];
			y += s[1];
			s += 2;
		}
		else
		{ // c literal indices, padded to a whole word
			uint32_t i, bytes = four ? (c + 1) / 2 : c;
			for(i = 0; i < c && s + (four ? i / 2 : i) < end; i++, x++)
				if(x < w && y < h)
					idx[(size_t)(h - 1 - y) * w + x] = (uint8_t)(four ? (i & 1 ? s[i / 2] & 15 : s[i / 2] >> 4) : s[i]);
			s += (bytes + 1) & ~1u;
		}
	}
}

static int DecodeBmp(const uint8_t* d, size_t size, Picture_t* out)
{
	uint32_t off, hs, w, bpp, comp, colors, masks[4] = {0, 0, 0, 0}, x, y;
	int32_t h;
	int topDown, shift[4], bits[4], i, alphaMask;
	const uint8_t* pal;
	uint32_t palEntry, palCount;
	size_t stride;
	PicKind_t kind;
	if(size < 26 || d[0] != 'B' || d[1] != 'M')
		return 0;
	off = Le32(d + 10);
	hs = Le32(d + 14);
	if(hs == 12)
	{ // OS/2 core header
		w = Le16(d + 18);
		h = (int16_t)Le16(d + 20);
		bpp = Le16(d + 24);
		comp = 0;
		colors = 0;
		palEntry = 3;
	}
	else if(hs >= 40 && size >= 14u + hs)
	{
		w = Le32(d + 18);
		h = (int32_t)Le32(d + 22);
		bpp = Le16(d + 28);
		comp = Le32(d + 30);
		colors = Le32(d + 46);
		palEntry = 4;
		if(comp == 3 || comp == 6)
		{ // bit fields: in the header from version 3 on, else after it
			const uint8_t* m = hs >= 52 ? d + 54 : d + 14 + hs;
			if(m + (comp == 6 || hs >= 56 ? 16 : 12) > d + size)
				return 0;
			masks[0] = Le32(m);
			masks[1] = Le32(m + 4);
			masks[2] = Le32(m + 8);
			if(comp == 6 || hs >= 56)
				masks[3] = Le32(m + 12);
		}
	}
	else
		return 0;
	topDown = h < 0;
	if(topDown)
		h = -h;
	if(w == 0 || h == 0 || w > 0x8000 || h > 0x8000 || off >= size)
		return 0;
	pal = d + 14 + hs + ((comp == 3 && hs == 40) ? 12 : (comp == 6 && hs == 40) ? 16
																				: 0);
	palCount = bpp <= 8 ? (colors && colors <= (1u << bpp) ? colors : 1u << bpp) : 0;
	if(bpp <= 8 && pal + (size_t)palCount * palEntry > d + size)
		palCount = (uint32_t)((size_t)(d + size - pal) / palEntry);
	alphaMask = (bpp == 16 || bpp == 32) && (comp == 3 || comp == 6) && masks[3] != 0;
	switch(bpp)
	{
		case 8:
			kind = PIC_INDEXED8;
			break;
		case 24:
			kind = PIC_RGB24;
			break;
		case 32:
			kind = alphaMask ? PIC_ARGB32 : PIC_OTHER;
			break;
		case 1:
		case 4:
		case 16:
			kind = PIC_OTHER;
			break;
		default:
			return 0;
	}
	if((comp == 1 && bpp != 8) || (comp == 2 && bpp != 4) || ((comp == 3 || comp == 6) && bpp != 16 && bpp != 32) ||
		comp > 6 || comp == 4 || comp == 5)
		return 0; // (4 and 5 are JPEG and PNG inside a BMP)
	if(!NewPicture(out, w, (uint32_t)h, kind, bpp <= 8))
		return 0;
	if(bpp <= 8)
	{
		uint8_t* idx = bpp == 8 ? out->index : (uint8_t*)BGI_Calloc((size_t)w * (size_t)h);
		if(comp == 1 || comp == 2)
			DecodeRle(d + off, d + size, idx, w, (uint32_t)h, comp == 2); // (RLE rows are always from the bottom)
		else
		{
			stride = ((size_t)w * bpp + 31) / 32 * 4;
			for(y = 0; y < (uint32_t)h; y++)
			{
				const uint8_t* row = d + off + (size_t)(topDown ? y : (uint32_t)h - 1 - y) * stride;
				for(x = 0; x < w; x++)
				{
					size_t bit = (size_t)x * bpp;
					if(row + bit / 8 >= d + size)
						break;
					idx[(size_t)y * w + x] = (uint8_t)((row[bit / 8] >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1u));
				}
			}
		}
		for(i = 0; i < (int)(w * (uint32_t)h); i++)
		{
			uint32_t k = idx[i];
			uint8_t* p = out->bgra + (size_t)i * 4;
			if(k < palCount)
			{
				p[0] = pal[k * palEntry];
				p[1] = pal[k * palEntry + 1];
				p[2] = pal[k * palEntry + 2];
			}
			p[3] = 0xFF;
		}
		if(idx != out->index)
			BGI_Free(idx);
		return 1;
	}
	if(comp == 0)
	{ // the default layouts: 5-5-5, B G R, B G R x
		masks[0] = bpp == 16 ? 0x7C00u : 0xFF0000u;
		masks[1] = bpp == 16 ? 0x03E0u : 0xFF00u;
		masks[2] = bpp == 16 ? 0x001Fu : 0xFFu;
	}
	for(i = 0; i < 4; i++)
		MaskShift(masks[i], &shift[i], &bits[i]);
	stride = ((size_t)w * bpp + 31) / 32 * 4;
	for(y = 0; y < (uint32_t)h; y++)
	{
		const uint8_t* row = d + off + (size_t)(topDown ? y : (uint32_t)h - 1 - y) * stride;
		uint8_t* dst = out->bgra + (size_t)y * w * 4;
		for(x = 0; x < w; x++, dst += 4)
		{
			const uint8_t* s = row + (size_t)x * (bpp / 8);
			uint32_t px;
			if(s + bpp / 8 > d + size)
				break;
			if(bpp == 24)
			{
				dst[0] = s[0];
				dst[1] = s[1];
				dst[2] = s[2];
				dst[3] = 0xFF;
				continue;
			}
			px = bpp == 16 ? Le16(s) : Le32(s);
			dst[2] = Field(px, masks[0], shift[0], bits[0]);
			dst[1] = Field(px, masks[1], shift[1], bits[1]);
			dst[0] = Field(px, masks[2], shift[2], bits[2]);
			dst[3] = alphaMask ? Field(px, masks[3], shift[3], bits[3]) : 0xFF;
		}
	}
	return 1;
}

// ---- GIF ----------------------------------------------------------------------------

/* the LZW data of an image (the sub-blocks after the minimum code size)
 * into `count` indices; what the data does not reach stays as it is */
static void Lzw(const uint8_t* s, const uint8_t* end, int minSize, uint8_t* outIdx, size_t count)
{
	uint16_t prefix[4096];
	uint8_t suffix[4096], first[4096], stack[4097];
	int clear = 1 << minSize, eoi = clear + 1, next = clear + 2, size = minSize + 1, old = -1, i;
	uint32_t acc = 0;
	int nbits = 0;
	size_t off = 0, blockLeft = 0;
	for(i = 0; i < clear; i++)
	{
		prefix[i] = 0xFFFF;
		suffix[i] = first[i] = (uint8_t)i;
	}
	while(off < count)
	{
		int code, c, sp = 0;
		while(nbits < size)
		{
			if(!blockLeft)
			{
				if(s >= end || *s == 0)
					return;
				blockLeft = *s++;
			}
			if(s >= end)
				return;
			acc |= (uint32_t)*s++ << nbits;
			nbits += 8;
			blockLeft--;
		}
		code = (int)(acc & ((1u << size) - 1u));
		acc >>= size;
		nbits -= size;
		if(code == clear)
		{
			next = clear + 2;
			size = minSize + 1;
			old = -1;
			continue;
		}
		if(code == eoi)
			return;
		if(old < 0)
		{
			if(code >= clear)
				return;
			outIdx[off++] = (uint8_t)code;
			old = code;
			continue;
		}
		if(code > next || (code == next && next >= 4096))
			return; // a code that cannot occur yet
		c = code;
		if(code == next)
		{ // the KwKwK case: the old string and its own first character
			stack[sp++] = first[old];
			c = old;
		}
		while(c >= clear && sp < 4096)
		{
			stack[sp++] = suffix[c];
			c = prefix[c];
		}
		stack[sp++] = (uint8_t)c;
		if(next < 4096)
		{
			prefix[next] = (uint16_t)old;
			suffix[next] = (uint8_t)c;
			first[next] = first[old];
			next++;
			if(next == (1 << size) && size < 12)
				size++;
		}
		while(sp && off < count)
			outIdx[off++] = stack[--sp];
		old = code;
	}
}

static int DecodeGif(const uint8_t* d, size_t size, Picture_t* out)
{
	const uint8_t* p = d + 13;
	const uint8_t* end = d + size;
	const uint8_t* gct = NULL;
	uint32_t sw, sh, gctN = 0, bg;
	int trans = -1;
	if(size < 13 || memcmp(d, "GIF", 3) != 0)
		return 0;
	sw = Le16(d + 6);
	sh = Le16(d + 8);
	bg = d[11];
	if(d[10] & 0x80)
	{
		gctN = 2u << (d[10] & 7);
		gct = p;
		p += gctN * 3;
	}
	while(p < end)
	{
		if(*p == 0x21)
		{ // an extension: the graphic control one may name a transparent index
			if(p + 2 > end)
				return 0;
			if(p[1] == 0xF9 && p + 7 <= end && p[2] >= 4 && (p[3] & 1))
				trans = p[6];
			p += 2;
			while(p < end && *p)
				p += 1 + *p;
			p++;
			continue;
		}
		if(*p == 0x2C)
		{ // the first image
			uint32_t ix, iy, iw, ih, ltN = 0, x, y, k;
			const uint8_t* pal = gct;
			uint32_t palN = gctN;
			uint8_t* frame;
			int interlaced, minSize;
			if(p + 10 > end)
				return 0;
			ix = Le16(p + 1);
			iy = Le16(p + 3);
			iw = Le16(p + 5);
			ih = Le16(p + 7);
			interlaced = (p[9] & 0x40) != 0;
			if(p[9] & 0x80)
			{
				ltN = 2u << (p[9] & 7);
				pal = p + 10;
				palN = ltN;
			}
			p += 10 + ltN * 3;
			if(p >= end || !pal || pal + palN * 3 > end)
				return 0;
			minSize = *p++;
			if(minSize < 2 || minSize > 11)
				return 0;
			if(sw == 0 || sh == 0)
				sw = ix + iw, sh = iy + ih;
			if(!NewPicture(out, sw, sh, PIC_INDEXED8, 1))
				return 0;
			// outside the image: the transparent index when there is one, else the background
			memset(out->index, trans >= 0 ? trans : (int)bg, (size_t)sw * sh);
			frame = (uint8_t*)BGI_Alloc((size_t)iw * ih + 1);
			memset(frame, trans >= 0 ? trans : 0, (size_t)iw * ih + 1);
			Lzw(p, end, minSize, frame, (size_t)iw * ih);
			for(y = 0; y < ih; y++)
			{
				uint32_t row = y;
				if(interlaced)
				{ // passes of every 8th row from 0, every 8th from 4, every 4th from 2, every 2nd from 1
					uint32_t n1 = (ih + 7) / 8, n2 = (ih + 3) / 8, n3 = (ih + 1) / 4;
					row = y < n1 ? y * 8 : y < n1 + n2 ? (y - n1) * 8 + 4
						: y < n1 + n2 + n3             ? (y - n1 - n2) * 4 + 2
													   : (y - n1 - n2 - n3) * 2 + 1;
				}
				if(iy + row >= sh)
					continue;
				for(x = 0; x < iw && ix + x < sw; x++)
					out->index[(size_t)(iy + row) * sw + ix + x] = frame[(size_t)y * iw + x];
			}
			BGI_Free(frame);
			for(k = 0; k < sw * sh; k++)
			{
				uint32_t i = out->index[k];
				uint8_t* dst = out->bgra + (size_t)k * 4;
				if(i < palN)
				{
					dst[0] = pal[i * 3 + 2];
					dst[1] = pal[i * 3 + 1];
					dst[2] = pal[i * 3];
				}
				dst[3] = (int)i == trans ? 0 : 0xFF;
			}
			return 1;
		}
		if(*p == 0x3B)
			break;
		return 0;
	}
	return 0;
}

// ---- PNG ----------------------------------------------------------------------------

/* through png.h; the kind from the header's colour type and depth, and
 * for an 8-bit palette the indices found back from the palette (with its
 * transparency) */
static int DecodePngFile(const uint8_t* d, size_t size, Picture_t* out)
{
	PngImage_t img;
	char err[64];
	uint32_t depth, type, n, i;
	const uint8_t* plte = NULL;
	const uint8_t* trns = NULL;
	uint32_t plteN = 0, trnsN = 0;
	int hasTrns = 0;
	size_t pos = 8;
	if(size < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8) != 0)
		return 0;
	depth = d[24];
	type = d[25];
	while(pos + 12 <= size)
	{ // the palette and transparency chunks
		uint32_t len = Be32(d + pos);
		if(len > size - pos - 12)
			break;
		if(memcmp(d + pos + 4, "PLTE", 4) == 0)
			plte = d + pos + 8, plteN = len / 3;
		else if(memcmp(d + pos + 4, "tRNS", 4) == 0)
			trns = d + pos + 8, trnsN = len, hasTrns = 1;
		else if(memcmp(d + pos + 4, "IDAT", 4) == 0)
			break;
		pos += 12 + (size_t)len;
	}
	if(!Png_Decode(d, size, &img, err, sizeof err))
		return 0;
	Png_Convert(&img, 4);
	if(!NewPicture(out, img.width, img.height, PIC_OTHER, type == 3 && depth == 8))
	{
		free(img.pixels);
		return 0;
	}
	n = img.width * img.height;
	memcpy(out->bgra, img.pixels, (size_t)n * 4);
	free(img.pixels);
	if(depth == 16)
		out->kind = PIC_OTHER;
	else if(type == 3)
		out->kind = depth == 8 ? PIC_INDEXED8 : PIC_OTHER;
	else if(type == 2)
		out->kind = hasTrns ? PIC_ARGB32 : PIC_RGB24;
	else
		out->kind = PIC_ARGB32; // grey, grey with alpha, colour with alpha
	if(out->index)
		for(i = 0; i < n; i++)
		{
			const uint8_t* p = out->bgra + (size_t)i * 4;
			uint32_t k;
			for(k = 0; k < plteN; k++)
				if(plte[k * 3] == p[2] && plte[k * 3 + 1] == p[1] && plte[k * 3 + 2] == p[0] &&
					(k < trnsN ? trns[k] : 0xFF) == p[3])
					break;
			out->index[i] = (uint8_t)(k < plteN ? k : 0);
		}
	return 1;
}

int Picture_Decode(const uint8_t* data, size_t size, Picture_t* out)
{
	memset(out, 0, sizeof *out);
	if(size >= 2 && data[0] == 'B' && data[1] == 'M')
		return DecodeBmp(data, size, out);
	if(size >= 8 && memcmp(data, "\x89PNG", 4) == 0)
		return DecodePngFile(data, size, out);
	if(size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
		return Jpeg_Decode(data, size, out);
	if(size >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0))
		return DecodeGif(data, size, out);
	return 0;
}

// ---- writers ------------------------------------------------------------------------

uint8_t* Bmp_Encode24(const uint8_t* bgrx, uint32_t w, uint32_t h, size_t pitch, size_t* size)
{
	size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3, total = 54 + stride * h;
	uint8_t* f;
	uint32_t x, y;
	if(w == 0 || h == 0 || total > 0x7fffffffu)
		return NULL;
	f = (uint8_t*)BGI_Calloc(total);
	f[0] = 'B';
	f[1] = 'M';
	f[2] = (uint8_t)total, f[3] = (uint8_t)(total >> 8), f[4] = (uint8_t)(total >> 16), f[5] = (uint8_t)(total >> 24);
	f[10] = 54;
	f[14] = 40;
	f[18] = (uint8_t)w, f[19] = (uint8_t)(w >> 8), f[20] = (uint8_t)(w >> 16), f[21] = (uint8_t)(w >> 24);
	f[22] = (uint8_t)h, f[23] = (uint8_t)(h >> 8), f[24] = (uint8_t)(h >> 16), f[25] = (uint8_t)(h >> 24);
	f[26] = 1;
	f[28] = 24;
	{
		size_t img = stride * h;
		f[34] = (uint8_t)img, f[35] = (uint8_t)(img >> 8), f[36] = (uint8_t)(img >> 16), f[37] = (uint8_t)(img >> 24);
	}
	for(y = 0; y < h; y++)
	{ // rows from the bottom
		const uint8_t* s = bgrx + (size_t)(h - 1 - y) * pitch;
		uint8_t* dst = f + 54 + (size_t)y * stride;
		for(x = 0; x < w; x++)
		{
			dst[x * 3] = s[x * 4];
			dst[x * 3 + 1] = s[x * 4 + 1];
			dst[x * 3 + 2] = s[x * 4 + 2];
		}
	}
	*size = total;
	return f;
}

uint8_t* Png_Encode24(const uint8_t* bgrx, uint32_t w, uint32_t h, size_t pitch, size_t* size)
{
	uint8_t* rgb;
	uint8_t* png;
	uint8_t* r;
	uint32_t x, y;
	if(w == 0 || h == 0)
		return NULL;
	rgb = (uint8_t*)BGI_Alloc((size_t)w * h * 3);
	for(y = 0; y < h; y++)
		for(x = 0; x < w; x++)
			memcpy(rgb + ((size_t)y * w + x) * 3, bgrx + (size_t)y * pitch + (size_t)x * 4, 3);
	png = Png_Encode(rgb, w, h, 3, (size_t)w * 3, 6, size);
	BGI_Free(rgb);
	if(!png)
		return NULL;
	r = (uint8_t*)BGI_Alloc(*size); // png.h hands out malloc() memory, picture.h BGI_Alloc
	memcpy(r, png, *size);
	free(png);
	return r;
}
