/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bmpload.c - loading image files into bitmap slots (inc/bgi/gfx/bmpops.h):
 *             the "90 10" / "92 1F" loaders, the Windows BMP decoder and
 *             the engine's own raw image format
 *
 * A raw image (what the loader's decoders leave in the buffer) starts with
 * a 16-byte header of 16-bit words: width, height, bits per pixel, format
 * (0 plain, 1 delta-coded planes), a word at +8 that marks a 32-bit image
 * as a vector map when it is 4, a flag word at +0xA and the base size at
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
#include "bgi/os.h"

#define LOAD_BUFFER 0x2000000 // 32 MB: the engine's load buffer

/* "90 10": load an image file into a slot.  The loader's cache is consulted
 * first; a Windows BMP is decoded directly, anything else must be a raw
 * image (as left by LoadFile after decompression).  0 ok, else the
 * decoder's code: 0x80000001 not a BMP and not a raw image either, ..2 ..7
 * as BmpOp_DecodeBmp, 0x80000004 a raw image of an unsupported depth,
 * 0x80000008 the manager refused the raw image.  A zero-length file hits
 * an uninitialised result in the original (it returns `name`); so does
 * this code. */
int BmpOp_LoadFile(int slot, const char* arc, const char* name)
{
	uint8_t* buf = (uint8_t*)BGI_Alloc(LOAD_BUFFER);
	uint32_t size = 0;
	int r;

	if(!Loader_CacheLookup(buf, &size, arc, name))
	{
		size = LoadFile(buf, arc, name);
		Loader_CacheAdd(arc, name, buf, size);
	}
	if(size == 0)
	{
		BGI_Free(buf);
		return (int)(intptr_t)name; // the original returns the dead `name` argument here
	}
	r = BmpOp_DecodeBmp(slot, buf);
	if(r == (int)0x80000001) // not a BMP: try the raw image
	{
		switch(BmpOp_PutRaw(slot, buf))
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
int BmpOp_LoadLocal(int slot, const char* path)
{
	uint8_t* buf = (uint8_t*)BGI_Alloc(LOAD_BUFFER);
	int r = -1;

	if(ReadWholeFile(buf, NULL, path) > 0)
		r = BmpOp_DecodeBmp(slot, buf);
	BGI_Free(buf);
	return r;
}

/* Decode an uncompressed Windows BMP (8 bpp -> GRAY8, 16 -> RGB16, 24 ->
 * RGB32, 32 -> ARGB32) into a slot through BmpMgr_PutPixels.  0 ok,
 * 0x80000001 not a BMP, ..2 not a 40-byte BITMAPINFOHEADER, ..3 planes
 * != 1, ..4 unsupported depth, ..5 compressed, ..6 empty, ..7 the manager
 * refused. */
int BmpOp_DecodeBmp(int slot, const void* data)
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
	ok = BmpMgr_PutPixels(gBmpMgr, slot, (int)w, (int)h, mode, pixels);
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

/* Put a raw image into a slot: 8 bpp -> GRAY8, 16 -> RGB16, 24 -> RGB32,
 * 32 -> ARGB32 (or PM_VECTOR when the header word at +8 is 4), 48 ->
 * PM_VECDIST.  0 ok, 1 unsupported depth, 2 the manager refused. */
int BmpOp_PutRaw(int slot, const void* data)
{
	const uint8_t* d = (const uint8_t*)data;
	uint32_t bpp = *(const uint16_t*)(d + 4);
	uint32_t w = *(const uint16_t*)d, h = *(const uint16_t*)(d + 2);
	int mode, r;
	uint8_t* buf;

	switch(bpp)
	{
		case 8: mode = PM_GRAY8; break;
		case 16: mode = PM_RGB16; break;
		case 24: mode = PM_RGB32; break;
		case 32: mode = *(const uint16_t*)(d + 8) == 4 ? PM_VECTOR : PM_ARGB32; break;
		case 48: mode = PM_VECDIST; break;
		default: return 1;
	}
	buf = (uint8_t*)BGI_Alloc((size_t)(bpp >> 3) * h * w + 0x10);
	if(!RawImage_Unpack(buf, data))
		r = 1;
	else
		r = BmpMgr_PutPixels(gBmpMgr, slot, *(const uint16_t*)buf, *(const uint16_t*)(buf + 2), mode, buf + 0x10) ? 0 : 2;
	/* 1.529 on: the header word at +0xA set to 1 flags a base size in the
	 * words at +0xC / +0xE (see BmpSlot_t) */
	if(r == 0 && *(const uint16_t*)(d + 0xa) == 1)
		BmpMgr_SetBaseSize(gBmpMgr, slot, *(const uint16_t*)(d + 0xc), *(const uint16_t*)(d + 0xe));
	BGI_Free(buf);
	return r;
}
