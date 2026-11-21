/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bmpload.c - loading image files into bitmap slots (src/include/bgi/gfx/bmpops.h):
 *             the "90 10" / "92 1F" loaders, the Windows BMP decoder and
 *             the engine's own raw image format; picture files outside the
 *             archives in and out ("90 C4" / "90 C5", through picture.h)
 *
 * A raw image (what the loader's decoders leave in the buffer) starts with
 * a 16-byte header of 16-bit words: width, height, bits per pixel, format
 * (0 plain, 1 delta-coded planes), a word at +8 that marks a 32-bit image
 * as a vector map when it is 4 (from 1.547 on as a height field when it is
 * 5 and as an opaque picture when it is 7), a flag word at +0xA and the base size at
 * +0xC / +0xE (1.529 on); the pixels follow at +0x10.
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
#include "bgi/version.h"
#include "bgi/picture.h"
#include "bgi/vm.h"
#include "bgi/core.h"

#define LOAD_BUFFER 0x2000000 // 32 MB: the engine's load buffer

/* "90 10": load an image file into a slot.  The loader's cache is consulted
 * first; a Windows BMP is decoded directly, anything else must be a raw
 * image (as left by LoadFile after decompression).  0 ok, else the
 * decoder's code: 0x80000001 not a BMP and not a raw image either, ..2 ..7
 * as BmpOp_DecodeBmp, 0x80000004 a raw image of an unsupported depth,
 * 0x80000008 the manager refused the raw image.  A zero-length file hits
 * an uninitialised result in the original (it returns `name`); so does
 * this code. */
int BmpOp_LoadFile(OpenBGI_t* o, int slot, const char* arc, const char* name)
{
	uint8_t* buf = (uint8_t*)Vm_TempAlloc(o, LOAD_BUFFER); // the load may ask for the disc
	uint32_t size = 0;
	int r;

	if(!Loader_CacheLookup(o, buf, &size, arc, name))
	{
		size = LoadFile(o, buf, arc, name);
		Loader_CacheAdd(o, arc, name, buf, size);
	}
	Vm_TempKeep(o, buf);
	if(size == 0)
	{
		BGI_Free(buf);
		return (int)(intptr_t)name; // the original returns the dead `name` argument here
	}
	r = BmpOp_DecodeBmp(o, slot, buf);
	if(r == (int)0x80000001) // not a BMP: try the raw image
	{
		switch(BmpOp_PutRaw(o, slot, buf))
		{
			case 0: r = 0; break;
			case 1: r = (int)0x80000004; break;
			case 2: r = (int)0x80000008; break;
			default: break; // keeps 0x80000001
		}
	}
	BGI_Free(buf);
	return r;
}

// "92 1F": a Windows BMP file of the file system (not an archive) into a slot; the decoder's result, -1 when the file cannot be read
int BmpOp_LoadLocal(OpenBGI_t* o, int slot, const char* path)
{
	uint8_t* buf = (uint8_t*)BGI_Alloc(LOAD_BUFFER);
	int r = -1;

	if(ReadWholeFile(o, buf, NULL, path) > 0)
		r = BmpOp_DecodeBmp(o, slot, buf);
	BGI_Free(buf);
	return r;
}

/* Decode an uncompressed Windows BMP (8 bpp -> GRAY8, 16 -> RGB16, 24 ->
 * RGB32, 32 -> ARGB32) into a slot through BmpMgr_PutPixels.  0 ok,
 * 0x80000001 not a BMP, ..2 not a 40-byte BITMAPINFOHEADER, ..3 planes
 * != 1, ..4 unsupported depth, ..5 compressed, ..6 empty, ..7 the manager
 * refused. */
int BmpOp_DecodeBmp(OpenBGI_t* o, int slot, const void* data)
{
	const uint8_t* d = (const uint8_t*)data;
	uint32_t w, h, bpp, rowBytes, srcOff, y;
	int mode, ok;
	uint8_t* pixels;

	if(d[0] != 'B' || d[1] != 'M')
		return (int)0x80000001;
	if(*(const uint32_t*)(d + 0xe) != 0x28) // biSize
		return (int)0x80000002;
	if(*(const uint16_t*)(d + 0x1a) != 1) // biPlanes
		return (int)0x80000003;
	bpp = *(const uint16_t*)(d + 0x1c);
	switch(bpp)
	{
		case 8: mode = PM_GRAY8; break;
		case 16: mode = PM_RGB16; break;
		case 24: mode = PM_RGB32; break;
		case 32: mode = PM_ARGB32; break;
		default: return (int)0x80000004;
	}
	if(*(const uint32_t*)(d + 0x1e) != 0) // biCompression
		return (int)0x80000005;
	w = *(const uint32_t*)(d + 0x12);
	h = *(const uint32_t*)(d + 0x16);
	if(w == 0 || h == 0)
		return (int)0x80000006;
	// the file's rows are bottom-up and padded to four bytes; the manager
	// wants packed top-down rows
	rowBytes = w * (bpp >> 3);
	pixels = (uint8_t*)BGI_Alloc((size_t)rowBytes * h);
	srcOff = 0;
	for(y = 0; y < h; y++)
	{
		memcpy(pixels + (size_t)(h - 1 - y) * rowBytes, d + *(const uint32_t*)(d + 0xa) + srcOff, rowBytes);
		srcOff = (srcOff + rowBytes + 3) & ~3u;
	}
	ok = BmpMgr_PutPixels(o, o->gfx.bmpMgr, slot, (int)w, (int)h, mode, pixels);
	BGI_Free(pixels);
	return ok ? 0 : (int)0x80000007;
}

/* Unpack a raw image into dst (header and pixels): format 0 is plain and
 * copied, format 1 carries planar deltas (RawImage_Unfilter).  Other
 * formats leave dst untouched.  Always 1. */
int RawImage_Unpack(void* dst, const void* src)
{
	const uint8_t* s = (const uint8_t*)src;
	uint16_t fmt = *(const uint16_t*)(s + 6);

	if(fmt == 0)
	{
		uint32_t w = *(const uint16_t*)s, h = *(const uint16_t*)(s + 2), bpp = *(const uint16_t*)(s + 4);
		memcpy(dst, src, 0x10 + (size_t)(bpp >> 3) * h * w);
	}
	else if(fmt == 1)
		RawImage_Unfilter(dst, src);
	return 1;
}

/* Format 1 stores one plane per channel; every byte is a delta to
 * the previous sample of its channel, and the pixels are visited in
 * serpentine order (odd rows right to left).  The result is interleaved
 * format 0 (the header copied with its format word set to 0). */
void RawImage_Unfilter(void* dst, const void* src)
{
	const uint8_t* s = (const uint8_t*)src;
	uint8_t* d = (uint8_t*)dst;
	int w = *(const uint16_t*)s, h = *(const uint16_t*)(s + 2);
	int channels = *(const uint16_t*)(s + 4) >> 3;
	const uint8_t* plane[8];
	uint8_t prev[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	int c, y, x, idx = 0, pos = 0;

	for(c = 0; c < channels; c++)
		plane[c] = s + 0x10 + (size_t)c * (size_t)(w * h);
	for(y = 0; y < h; y++)
	{
		uint8_t* row = d + 0x10 + (size_t)y * (size_t)(channels * w);
		int step = (y & 1) ? -channels : channels;
		for(x = 0; x < w; x++, idx++)
		{
			for(c = 0; c < channels; c++)
			{
				prev[c] = (uint8_t)(prev[c] + plane[c][idx]);
				row[pos + c] = prev[c];
			}
			pos += step;
		}
		pos -= step; // stay on the last pixel: the next row walks the other way
	}
	memcpy(d, s, 0x10);
	*(uint16_t*)(d + 6) = 0;
}

/* The pixel mode of a raw image header: 8 bpp -> GRAY8, 16 -> RGB16, 24
 * -> RGB32, 48 -> PM_VECDIST, 32 -> ARGB32, or by the word at +8: 4
 * PM_VECTOR and, from 1.547 on, 5 PM_HEIGHT and 7 RGB32 (a 24-bit picture
 * the decoders of those builds widened to 32 bits).  -1 for another depth. */
static int RawHeaderMode(OpenBGI_t* o, const uint8_t* d)
{
	uint32_t kind = *(const uint16_t*)(d + 8);
	switch(*(const uint16_t*)(d + 4))
	{
		case 8: return PM_GRAY8;
		case 16: return PM_RGB16;
		case 24: return PM_RGB32;
		case 48: return PM_VECDIST;
		case 32:
			if(kind == 4)
				return PM_VECTOR;
			if(o->engine.profile->gen >= GEN_1_547 && kind == 5)
				return PM_HEIGHT;
			if(o->engine.profile->gen >= GEN_1_547 && kind == 7)
				return PM_RGB32;
			return PM_ARGB32;
		default: return -1;
	}
}

/* Put a raw image into a slot, in the mode RawHeaderMode gives.  0 ok, 1
 * unsupported depth, 2 the manager refused. */
int BmpOp_PutRaw(OpenBGI_t* o, int slot, const void* data)
{
	const uint8_t* d = (const uint8_t*)data;
	uint32_t bpp = *(const uint16_t*)(d + 4);
	uint32_t w = *(const uint16_t*)d, h = *(const uint16_t*)(d + 2);
	int mode = RawHeaderMode(o, d), r;
	uint8_t* buf;

	if(mode < 0)
		return 1;
	buf = (uint8_t*)BGI_Alloc((size_t)(bpp >> 3) * h * w + 0x10);
	if(!RawImage_Unpack(buf, data))
		r = 1;
	else
		r = BmpMgr_PutPixels(o, o->gfx.bmpMgr, slot, *(const uint16_t*)buf, *(const uint16_t*)(buf + 2), mode, buf + 0x10) ? 0 : 2;
	/* 1.529 on: the header word at +0xA set to 1 flags a base size in the
	 * words at +0xC / +0xE (see BmpSlot_t) */
	if(r == 0 && *(const uint16_t*)(d + 0xa) == 1)
		BmpMgr_SetBaseSize(o->gfx.bmpMgr, slot, *(const uint16_t*)(d + 0xc), *(const uint16_t*)(d + 0xe));
	BGI_Free(buf);
	return r;
}

/* "90 C0" (1.547 on): make the slot a bitmap of the size, pixel mode and
 * base size of the picture file arc:name, all pixels 0, from the file's
 * first 0x30 bytes as stored (LoadFileRange: no decoding, no disc prompt,
 * no loader cache).  The scripts use it during a high-speed skip behind a
 * covered screen, where only the pictures' geometry matters.  The file
 * must be a CompressedBG or an uncompressed raw image (width and height
 * not 0, 8, 24 or 32 bits, format word 0 or 1).  0 ok, -1 not found,
 * BMP_ERR_NOT_BG not in that format (or shorter than the header),
 * 0x80000009 the slot could not be made. */
int BmpOp_CreateLikeFile(OpenBGI_t* o, int slot, const char* arc, const char* name)
{
	uint8_t hdr[0x30];
	const uint8_t* h = hdr;
	uint32_t bpp;
	int mode;
	switch(LoadFileRange(o, hdr, arc, name, 0, sizeof hdr))
	{
		case 0: break;
		case 2:
		case 3: return (int)BMP_ERR_NOT_BG;
		default: return -1;
	}
	if(BGI_IsCbg(hdr))
		h = hdr + 0x10; // the raw image header the decoder would produce
	else
	{
		bpp = *(const uint16_t*)(h + 4);
		if(!*(const uint16_t*)h || !*(const uint16_t*)(h + 2) || (bpp != 8 && bpp != 24 && bpp != 32) ||
			*(const uint16_t*)(h + 6) > 1)
			return (int)BMP_ERR_NOT_BG;
	}
	mode = RawHeaderMode(o, h);
	if(!BmpOp_Create(o, slot, *(const uint16_t*)h, *(const uint16_t*)(h + 2), mode))
		return (int)0x80000009u;
	if(*(const uint16_t*)(h + 0xa) == 1)
		BmpMgr_SetBaseSize(o->gfx.bmpMgr, slot, *(const uint16_t*)(h + 0xc), *(const uint16_t*)(h + 0xe));
	BmpOp_Clear(o, slot, 0);
	return 0;
}

/* "90 C4" (1.553 on): the picture file at `path` (as given, not in an
 * archive; BMP, PNG, JPEG or GIF by its content) into the slot at its own
 * size, through the "90 14" path - so an ARGB32 picture is un-composited
 * against the "90 0F" key colour.  mode 2 makes it ARGB32, 1 the same
 * relabelled RGB32, 3 GRAY8 from the indices of an 8-bit palette picture
 * (taken out of 4-byte aligned rows as if they were packed, so a width
 * that is not a multiple of 4 shears, as in the original), -1 whichever
 * of the three the file's own format is.  0 ok; mode -1: 0x80000001 no
 * such file, 0x80000002 not readable or of another format; any other
 * failure -1 (a missing file with an explicit mode, a colour picture as
 * GRAY8, a mode other than these). */
int BmpOp_LoadPicture(OpenBGI_t* o, int slot, const char* path, int mode)
{
	FsFile_t* f = Fs_FileOpenRead(o, path ? path : "");
	uint8_t* data = NULL;
	uint32_t size = 0;
	Picture_t pic;
	int ok = 0;
	memset(&pic, 0, sizeof pic);
	if(f)
	{
		size = Fs_FileSize(f);
		if(size && size <= 0x10000000u)
		{
			data = (uint8_t*)BGI_Alloc(size);
			ok = Fs_FileRead(f, data, size) == size;
		}
		Fs_FileClose(f);
	}
	ok = ok && Picture_Decode(data, size, &pic);
	BGI_Free(data);
	if(mode == -1)
	{
		if(!ok)
			return (int)(Fs_FileAttrs(o, path ? path : "") == OPENBGI_INVALID_ATTRS ? 0x80000001u : 0x80000002u);
		if(pic.kind == PIC_OTHER)
		{
			Picture_Free(&pic);
			return (int)0x80000002u;
		}
		mode = (int)pic.kind; // PIC_RGB24 / PIC_ARGB32 / PIC_INDEXED8 are the modes 1 / 2 / 3
	}
	if(!ok || mode < 1 || mode > 3 || (mode == 3 && !pic.index))
	{
		Picture_Free(&pic);
		return -1;
	}
	if(mode == 3)
	{
		size_t stride = ((size_t)pic.width + 3) & ~(size_t)3;
		uint8_t* rows = (uint8_t*)BGI_Calloc(stride * pic.height);
		uint32_t y;
		for(y = 0; y < pic.height; y++)
			memcpy(rows + (size_t)y * stride, pic.index + (size_t)y * pic.width, pic.width);
		BmpMgr_PutPixels(o, o->gfx.bmpMgr, slot, (int)pic.width, (int)pic.height, PM_GRAY8, rows);
		BGI_Free(rows);
	}
	else
	{
		BmpMgr_PutPixels(o, o->gfx.bmpMgr, slot, (int)pic.width, (int)pic.height, PM_ARGB32, pic.bgra);
		if(mode == 1)
			BmpMgr_SetMode(o->gfx.bmpMgr, slot, PM_RGB32);
	}
	Picture_Free(&pic);
	return 0;
}

/* "90 C5" (1.553 on): the whole bitmap as a picture file at exactly
 * `path`, format 0 BMP, 1 JPEG of `quality` (0 .. 100), 4 PNG; the alpha
 * is dropped (the original hands the pixels to GDI+ as a 32-bit DIB,
 * which has none).  0 ok, 0x80000003 no bitmap in the slot, 0x80000004 a
 * format above 4, 0x80000005 not written (GIF and TIFF, 2 and 3, have no
 * encoder here; a quality outside 0 .. 100; a pixel format that is not
 * a picture; the file could not be written). */
int BmpOp_SavePicture(OpenBGI_t* o, int slot, const char* path, uint32_t format, int quality)
{
	Bmp_t b;
	uint8_t* bgrx;
	uint8_t* file = NULL;
	size_t pitch, size = 0;
	int x, y, ok = 0;
	if(!BmpMgr_GetInfo(o->gfx.bmpMgr, &b, slot) || !b.pixels || b.w <= 0 || b.h <= 0)
		return (int)0x80000003u;
	if(format > 4)
		return (int)0x80000004u;
	if(format == 2 || format == 3 || (format == 1 && (quality < 0 || quality > 100)))
		return (int)0x80000005u;
	pitch = (size_t)b.w * 4;
	bgrx = (uint8_t*)BGI_Alloc(pitch * (size_t)b.h);
	for(y = 0; y < b.h; y++)
	{
		const uint8_t* s = b.pixels + (size_t)y * (size_t)b.pitch;
		uint8_t* dst = bgrx + (size_t)y * pitch;
		for(x = 0; x < b.w; x++, dst += 4)
			switch(b.mode)
			{
				case PM_RGB32:
				case PM_ARGB32: memcpy(dst, s + x * 4, 4); break;
				case PM_RGB16:
				{
					uint32_t v = (uint32_t)s[x * 2] | (uint32_t)s[x * 2 + 1] << 8;
					dst[2] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
					dst[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
					dst[0] = (uint8_t)((v & 31) * 255 / 31);
					break;
				}
				case PM_GRAY8: dst[0] = dst[1] = dst[2] = s[x]; break;
				default: BGI_Free(bgrx); return (int)0x80000005u;
			}
	}
	if(format == 0)
		file = Bmp_Encode24(bgrx, (uint32_t)b.w, (uint32_t)b.h, pitch, &size);
	else if(format == 1)
		file = Jpeg_Encode(bgrx, (uint32_t)b.w, (uint32_t)b.h, pitch, quality, &size);
	else
		file = Png_Encode24(bgrx, (uint32_t)b.w, (uint32_t)b.h, pitch, &size);
	BGI_Free(bgrx);
	if(file && path)
	{
		FsFile_t* f = Fs_FileCreate(o, path);
		if(f)
		{
			ok = Fs_FileWrite(f, file, (uint32_t)size) == size;
			Fs_FileClose(f);
		}
	}
	BGI_Free(file);
	return ok ? 0 : (int)0x80000005u;
}
