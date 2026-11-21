/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * icon.c - a program's icon (bgi/icon.h): out of a Windows executable,
 *          as pictures, and OpenBGI's own; not part of the original engine
 *
 * A Windows program (PE) keeps its resources in a tree of three levels -
 * the kind, the name, the language - whose leaves say where the bytes
 * are.  An icon is two kinds of them: a group (kind 14), which lists the
 * sizes, and one picture (kind 3) for each.  An .ico file is the same
 * list with the pictures behind it, so Icon_FromExe has only to put the
 * two together.
 *
 * A picture is a PNG file or a bitmap without its file header: the
 * colours bottom row first, then a mask of one bit a pixel (set: nothing
 * there), the height in the header counting both.  A 32-bit bitmap has
 * its own alpha instead - unless all of it is 0, as in icons older than
 * that custom, which then go by their mask.
 *
 * Every read is bounded by the bytes given: damaged data gives no icon.
 */
#include "bgi/icon.h"
#include "bgi/bytes.h"
#include "bgi/picture.h"

#define RT_ICON       3u
#define RT_GROUP_ICON 14u
#define MAX_PICTURES  64u      // of one group
#define MAX_SIDE      1024     // pixels of a picture's side
#define MAX_ICO       0x800000 // bytes of an .ico put together

// ---- the program's resources ---------------------------------------------------------------

typedef struct Pe
{
	const uint8_t* d;
	size_t n;
	size_t sections; // the section table
	uint32_t count;  // .. and its entries
	size_t rsrc;     // the resources' place in the file
	uint32_t rsrcSize;
} Pe_t;

// where the bytes at the address `rva` of the loaded program are in the file; 0 when nowhere
static size_t PeOffset(const Pe_t* pe, uint32_t rva, uint32_t size)
{
	uint32_t i;
	for(i = 0; i < pe->count; i++)
	{
		const uint8_t* s = pe->d + pe->sections + (size_t)i * 40;
		uint32_t va = Le32(s + 12), rawSize = Le32(s + 16), raw = Le32(s + 20);
		if(rva >= va && rva - va < rawSize && size <= rawSize - (rva - va))
		{
			size_t off = (size_t)raw + (rva - va);
			return off < pe->n && size <= pe->n - off ? off : 0;
		}
	}
	return 0;
}

static int PeOpen(Pe_t* pe, const uint8_t* d, size_t n)
{
	size_t nt, opt, dirs;
	uint32_t optSize, magic, rva;
	memset(pe, 0, sizeof *pe);
	pe->d = d;
	pe->n = n;
	if(n < 0x40 || d[0] != 'M' || d[1] != 'Z')
		return 0;
	nt = Le32(d + 0x3c);
	if(nt > n || n - nt < 24 + 96 || memcmp(d + nt, "PE\0\0", 4) != 0)
		return 0;
	pe->count = Le16(d + nt + 6);
	optSize = Le16(d + nt + 20);
	opt = nt + 24;
	if(optSize > n - opt)
		return 0;
	pe->sections = opt + optSize;
	if(pe->count > 96 || (size_t)pe->count * 40 > n - pe->sections)
		return 0;
	magic = Le16(d + opt);
	dirs = magic == 0x20b ? 112 : 96; // (64-bit programs have a longer header before the directories)
	if(magic != 0x10b && magic != 0x20b)
		return 0;
	if(optSize < dirs + 3 * 8 || Le32(d + opt + dirs - 4) < 3)
		return 1; // a program without resources
	rva = Le32(d + opt + dirs + 2 * 8);
	pe->rsrcSize = Le32(d + opt + dirs + 2 * 8 + 4);
	pe->rsrc = rva ? PeOffset(pe, rva, pe->rsrcSize) : 0;
	if(!pe->rsrc)
		pe->rsrcSize = 0;
	return 1;
}

int Icon_IsExe(const uint8_t* exe, size_t n)
{
	Pe_t pe;
	return PeOpen(&pe, exe, n);
}

/* The entry of the directory at `dir` (an offset in the resources) whose
 * id is `id`, or its first entry when `first`: what it points at into
 * *to, with the top bit set for another directory.  0 when there is none. */
static int RsrcEntry(const Pe_t* pe, uint32_t dir, uint32_t id, int first, uint32_t* to)
{
	const uint8_t* p;
	uint32_t count, i;
	if(dir > pe->rsrcSize || pe->rsrcSize - dir < 16)
		return 0;
	p = pe->d + pe->rsrc + dir;
	count = Le16(p + 12) + Le16(p + 14);
	if(count > (pe->rsrcSize - dir - 16) / 8)
		return 0;
	for(i = 0; i < count; i++)
	{
		const uint8_t* e = p + 16 + (size_t)i * 8;
		if(first || Le32(e) == id)
		{
			*to = Le32(e + 4);
			return 1;
		}
	}
	return 0;
}

// the bytes of the resource a leaf at `leaf` names; NULL when they are not in the file
static const uint8_t* RsrcData(const Pe_t* pe, uint32_t leaf, uint32_t* size)
{
	const uint8_t* p;
	size_t off;
	if(leaf > pe->rsrcSize || pe->rsrcSize - leaf < 16)
		return NULL;
	p = pe->d + pe->rsrc + leaf;
	*size = Le32(p + 4);
	off = PeOffset(pe, Le32(p), *size);
	return off ? pe->d + off : NULL;
}

// the resource of the kind `kind` with the id `id` (the first of the kind when `first`), in its first language
static const uint8_t* Rsrc(const Pe_t* pe, uint32_t kind, uint32_t id, int first, uint32_t* size)
{
	uint32_t to;
	if(!pe->rsrcSize || !RsrcEntry(pe, 0, kind, 0, &to) || !(to & 0x80000000u))
		return NULL;
	if(!RsrcEntry(pe, to & 0x7fffffffu, id, first, &to) || !(to & 0x80000000u))
		return NULL;
	if(!RsrcEntry(pe, to & 0x7fffffffu, 0, 1, &to) || (to & 0x80000000u))
		return NULL;
	return RsrcData(pe, to, size);
}

uint8_t* Icon_FromExe(const uint8_t* exe, size_t n, size_t* size)
{
	Pe_t pe;
	const uint8_t* group;
	uint32_t groupSize, count, i, kept = 0;
	size_t total, at;
	uint8_t* ico;
	*size = 0;
	if(!PeOpen(&pe, exe, n))
		return NULL;
	group = Rsrc(&pe, RT_GROUP_ICON, 0, 1, &groupSize);
	if(!group || groupSize < 6 || Le16(group + 2) != 1)
		return NULL;
	count = Le16(group + 4);
	if(!count || count > MAX_PICTURES || groupSize < 6 + count * 14)
		return NULL;
	total = 6 + (size_t)count * 16;
	for(i = 0; i < count; i++)
	{ // (a picture that is not there is left out of the file)
		uint32_t picSize;
		if(Rsrc(&pe, RT_ICON, Le16(group + 6 + i * 14 + 12), 0, &picSize) && picSize)
			total += picSize;
	}
	if(total > MAX_ICO)
		return NULL;
	ico = (uint8_t*)BGI_Alloc(total);
	memset(ico, 0, 6 + (size_t)count * 16);
	at = 6 + (size_t)count * 16;
	for(i = 0; i < count; i++)
	{
		const uint8_t* e = group + 6 + i * 14;
		uint32_t picSize;
		const uint8_t* pic = Rsrc(&pe, RT_ICON, Le16(e + 12), 0, &picSize);
		uint8_t* out = ico + 6 + (size_t)kept * 16;
		if(!pic || !picSize)
			continue;
		memcpy(out, e, 8);
		PutLe32(out + 8, picSize);
		PutLe32(out + 12, (uint32_t)at);
		memcpy(ico + at, pic, picSize);
		at += picSize;
		kept++;
	}
	if(!kept)
	{
		BGI_Free(ico);
		return NULL;
	}
	PutLe16(ico + 2, 1);
	PutLe16(ico + 4, kept);
	if(kept < count)
	{ // the pictures move up to the shorter list
		size_t gap = (size_t)(count - kept) * 16, head = 6 + (size_t)count * 16;
		memmove(ico + head - gap, ico + head, at - head);
		for(i = 0; i < kept; i++)
			PutLe32(ico + 6 + (size_t)i * 16 + 12, Le32(ico + 6 + (size_t)i * 16 + 12) - (uint32_t)gap);
		at -= gap;
	}
	*size = at;
	return ico;
}

// ---- the pictures of an .ico file ------------------------------------------------------------

// a bitmap picture (`d`, `n` bytes: the header, the colour table, the colours, the mask); 1 when it could be read
static int DecodeBitmap(const uint8_t* d, size_t n, IconImage_t* out, int* bits)
{
	uint32_t head, bpp, colours, x, y;
	int32_t w, h;
	size_t rowBytes, maskBytes, table, pixels, mask;
	int anyAlpha = 0;
	if(n < 40)
		return 0;
	head = Le32(d);
	w = (int32_t)Le32(d + 4);
	h = (int32_t)Le32(d + 8) / 2; // the colours and the mask
	bpp = Le16(d + 14);
	if(head < 40 || head > n || Le32(d + 16) != 0 || w < 1 || h < 1 || w > MAX_SIDE || h > MAX_SIDE)
		return 0;
	if(bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
		return 0;
	colours = bpp <= 8 ? Le32(d + 32) : 0;
	if(bpp <= 8 && (!colours || colours > (1u << bpp)))
		colours = 1u << bpp;
	rowBytes = (((size_t)w * bpp + 31) / 32) * 4;
	maskBytes = (((size_t)w + 31) / 32) * 4;
	table = head;
	pixels = table + (size_t)colours * 4;
	mask = pixels + rowBytes * (size_t)h;
	if(mask > n)
		return 0;
	out->w = w;
	out->h = h;
	out->bgra = (uint8_t*)BGI_Alloc((size_t)w * (size_t)h * 4);
	for(y = 0; y < (uint32_t)h; y++)
	{
		const uint8_t* row = d + pixels + rowBytes * (size_t)((uint32_t)h - 1 - y);
		const uint8_t* mrow = mask + maskBytes * (size_t)h <= n ? d + mask + maskBytes * (size_t)((uint32_t)h - 1 - y) : NULL;
		uint8_t* o = out->bgra + (size_t)y * (size_t)w * 4;
		for(x = 0; x < (uint32_t)w; x++, o += 4)
		{
			int clear = mrow ? (mrow[x >> 3] >> (7 - (x & 7))) & 1 : 0;
			if(bpp == 32)
			{
				memcpy(o, row + (size_t)x * 4, 4);
				anyAlpha |= o[3];
				continue;
			}
			if(bpp == 24)
				memcpy(o, row + (size_t)x * 3, 3);
			else
			{
				uint32_t i = bpp == 8 ? row[x] : bpp == 4 ? (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15u
														  : (row[x >> 3] >> (7 - (x & 7))) & 1u;
				if(i < colours)
					memcpy(o, d + table + (size_t)i * 4, 3);
				else
					o[0] = o[1] = o[2] = 0;
			}
			o[3] = clear ? 0 : 0xff;
		}
	}
	if(bpp == 32 && !anyAlpha)
	{ // no alpha at all: an icon of the older kind, which goes by its mask
		for(y = 0; y < (uint32_t)h; y++)
		{
			const uint8_t* mrow = mask + maskBytes * (size_t)h <= n ? d + mask + maskBytes * (size_t)((uint32_t)h - 1 - y) : NULL;
			uint8_t* o = out->bgra + (size_t)y * (size_t)w * 4;
			for(x = 0; x < (uint32_t)w; x++)
				o[(size_t)x * 4 + 3] = mrow && ((mrow[x >> 3] >> (7 - (x & 7))) & 1) ? 0 : 0xff;
		}
	}
	*bits = (int)bpp;
	return 1;
}

int Icon_Decode(const uint8_t* ico, size_t n, IconImage_t* out, int max)
{
	uint32_t count, i;
	int kept = 0, k, bitsOf[32];
	if(n < 6 || Le16(ico) != 0 || Le16(ico + 2) != 1 || max < 1)
		return 0;
	if(max > 32)
		max = 32;
	count = Le16(ico + 4);
	if(count > MAX_PICTURES || (size_t)count * 16 > n - 6)
		return 0;
	for(i = 0; i < count; i++)
	{
		const uint8_t* e = ico + 6 + (size_t)i * 16;
		uint32_t size = Le32(e + 8), off = Le32(e + 12);
		IconImage_t img;
		int bits = 32, at;
		memset(&img, 0, sizeof img);
		if(off > n || size > n - off || size < 8)
			continue;
		if(memcmp(ico + off, "\x89PNG", 4) == 0)
		{
			Picture_t pic;
			memset(&pic, 0, sizeof pic);
			if(!Picture_Decode(ico + off, size, &pic) || !pic.bgra || pic.width > MAX_SIDE || pic.height > MAX_SIDE)
			{
				Picture_Free(&pic);
				continue;
			}
			img.w = (int)pic.width;
			img.h = (int)pic.height;
			img.bgra = pic.bgra;
			pic.bgra = NULL;
			Picture_Free(&pic);
		}
		else if(!DecodeBitmap(ico + off, size, &img, &bits))
			continue;
		// one picture a size, the one with the most colours; the largest first
		for(at = 0; at < kept && !(out[at].w == img.w && out[at].h == img.h); at++)
		{
		}
		if(at < kept)
		{
			if(bits > bitsOf[at])
			{
				BGI_Free(out[at].bgra);
				out[at] = img;
				bitsOf[at] = bits;
			}
			else
				BGI_Free(img.bgra);
			continue;
		}
		for(at = 0; at < kept && out[at].w * out[at].h >= img.w * img.h; at++)
		{
		}
		if(at >= max)
		{
			BGI_Free(img.bgra);
			continue;
		}
		if(kept == max)
			BGI_Free(out[--kept].bgra);
		for(k = kept; k > at; k--)
		{
			out[k] = out[k - 1];
			bitsOf[k] = bitsOf[k - 1];
		}
		out[at] = img;
		bitsOf[at] = bits;
		kept++;
	}
	return kept;
}

void Icon_Free(IconImage_t* images, int count)
{
	int i;
	for(i = 0; i < count; i++)
	{
		BGI_Free(images[i].bgra);
		images[i].bgra = NULL;
	}
}

// ---- OpenBGI's own ---------------------------------------------------------------------------

/* The icon the engine carries: an .ico file of four PNG pictures, of the
 * drawing in icons/openbgi.svg (`make icon` writes the table). */
static const uint8_t kOwn[] = {
#include "ownicon.inc"
};

int Icon_Own(IconImage_t* out, int max)
{
	return Icon_Decode(kOwn, sizeof kOwn, out, max);
}
