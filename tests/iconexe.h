/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/iconexe.h - a Windows program with an icon, written out byte by
 *                   byte, for the tests that read one (tests/icon.c,
 *                   tests/launcher.c)
 *
 * The program is a header, one section and nothing to run: the section
 * holds the resources, a group of icons and its pictures.
 */
#ifndef BGI_TESTS_ICONEXE_H_
#define BGI_TESTS_ICONEXE_H_

#include <stdint.h>
#include <string.h>

#define ICONEXE_RVA 0x1000u // where the section is loaded
#define ICONEXE_RAW 0x200u  // .. and where it is in the file

static inline void IconExe_Put16(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void IconExe_Put32(uint8_t* p, uint32_t v)
{
	IconExe_Put16(p, v);
	IconExe_Put16(p + 2, v >> 16);
}

/* A picture of an icon as a bitmap of 32 bits: side x side pixels, each
 * blue `b`, green `g`, red `r` and opaque but the top left one, which is
 * clear; the header, the rows from the bottom up, the mask.  Its size. */
static inline uint32_t IconExe_Picture(uint8_t* out, int side, int b, int g, int r)
{
	uint32_t maskRow = (((uint32_t)side + 31) / 32) * 4, n = 40 + (uint32_t)(side * side) * 4 + maskRow * (uint32_t)side;
	int x, y;
	memset(out, 0, n);
	IconExe_Put32(out, 40);
	IconExe_Put32(out + 4, (uint32_t)side);
	IconExe_Put32(out + 8, (uint32_t)side * 2);
	IconExe_Put16(out + 12, 1);
	IconExe_Put16(out + 14, 32);
	for(y = 0; y < side; y++)
		for(x = 0; x < side; x++)
		{
			uint8_t* p = out + 40 + ((size_t)(side - 1 - y) * (size_t)side + (size_t)x) * 4;
			p[0] = (uint8_t)b;
			p[1] = (uint8_t)g;
			p[2] = (uint8_t)r;
			p[3] = (uint8_t)(x == 0 && y == 0 ? 0 : 255);
		}
	return n;
}

// a directory of the resources with `count` entries by number; where its entries begin
static inline uint32_t IconExe_Dir(uint8_t* rsrc, uint32_t at, uint32_t count)
{
	memset(rsrc + at, 0, 16 + count * 8);
	IconExe_Put16(rsrc + at + 14, count);
	return at + 16;
}

/* The program into `out` (at least 0x2000 bytes): a group of `count`
 * icons (at most 4) of the sides side[i], the picture of icon i in the
 * colour (shade + i * 10, 0x40, 0x80), with a header of 32 bits or, with
 * `wide`, of 64.  `missing` leaves the picture of that icon out of the
 * resources though the group names it (-1: none).  `tail`, when it is
 * not NULL, is put after the section as it is.  The file's size. */
static inline size_t IconExe_Make(uint8_t* out, int count, const int* side, int shade, int wide, int missing, const char* tail)
{
	const uint32_t nt = 0x40, opt = nt + 24, optSize = wide ? 240 : 224, dirs = wide ? 112 : 96, sect = opt + optSize;
	uint8_t* rsrc = out + ICONEXE_RAW;
	uint32_t at, entries, leaves, data, i, kept = 0, size;
	memset(out, 0, 0x2000);
	out[0] = 'M';
	out[1] = 'Z';
	IconExe_Put32(out + 0x3c, nt);
	memcpy(out + nt, "PE\0\0", 4);
	IconExe_Put16(out + nt + 4, wide ? 0x8664 : 0x14c);
	IconExe_Put16(out + nt + 6, 1); // one section
	IconExe_Put16(out + nt + 20, optSize);
	IconExe_Put16(out + opt, wide ? 0x20b : 0x10b);
	IconExe_Put32(out + opt + dirs - 4, 16); // the directories
	memcpy(out + sect, ".rsrc", 5);
	IconExe_Put32(out + sect + 12, ICONEXE_RVA);
	IconExe_Put32(out + sect + 20, ICONEXE_RAW);
	for(i = 0; i < (uint32_t)count; i++)
		kept += (int)i != missing;
	// the directories: the kinds, then for each kind its ids, then for each id its language
	entries = IconExe_Dir(rsrc, 0, 2);
	at = 16 + 2 * 8;
	IconExe_Put32(rsrc + entries, 3); // RT_ICON
	IconExe_Put32(rsrc + entries + 4, 0x80000000u | at);
	{
		uint32_t ids = IconExe_Dir(rsrc, at, kept), k = 0;
		at += 16 + kept * 8;
		for(i = 0; i < (uint32_t)count; i++)
		{
			if((int)i == missing)
				continue;
			IconExe_Put32(rsrc + ids + k * 8, i + 1);
			IconExe_Put32(rsrc + ids + k * 8 + 4, 0x80000000u | at);
			IconExe_Put32(rsrc + IconExe_Dir(rsrc, at, 1), 0x409);
			at += 16 + 8;
			k++;
		}
	}
	IconExe_Put32(rsrc + entries + 8, 14); // RT_GROUP_ICON
	IconExe_Put32(rsrc + entries + 12, 0x80000000u | at);
	IconExe_Put32(rsrc + IconExe_Dir(rsrc, at, 1), 1);
	IconExe_Put32(rsrc + at + 16 + 4, 0x80000000u | (at + 24));
	at += 24;
	IconExe_Put32(rsrc + IconExe_Dir(rsrc, at, 1), 0x409);
	at += 24;
	// the leaves in the order of the languages above (the pictures, then the group), then what they name
	leaves = at;
	data = leaves + (kept + 1) * 16;
	{
		uint32_t k = 0, lang = 16 + 2 * 8 + 16 + kept * 8; // the first picture's language directory
		uint8_t* group = rsrc + data;
		IconExe_Put16(group + 2, 1);
		IconExe_Put16(group + 4, (uint32_t)count);
		for(i = 0; i < (uint32_t)count; i++)
		{
			uint8_t* e = group + 6 + i * 14;
			e[0] = e[1] = (uint8_t)side[i];
			IconExe_Put16(e + 4, 1);
			IconExe_Put16(e + 6, 32);
			IconExe_Put16(e + 12, i + 1);
		}
		size = 6 + (uint32_t)count * 14;
		IconExe_Put32(rsrc + lang + kept * 24 + 24 + 16 + 4, leaves + kept * 16); // the group's language -> its leaf
		IconExe_Put32(rsrc + leaves + kept * 16, ICONEXE_RVA + data);
		IconExe_Put32(rsrc + leaves + kept * 16 + 4, size);
		data += size;
		for(i = 0; i < (uint32_t)count; i++)
		{
			uint32_t n;
			if((int)i == missing)
				continue;
			n = IconExe_Picture(rsrc + data, side[i], shade + (int)i * 10, 0x40, 0x80);
			IconExe_Put32(group + 6 + i * 14 + 8, n);
			IconExe_Put32(rsrc + lang + k * 24 + 16 + 4, leaves + k * 16);
			IconExe_Put32(rsrc + leaves + k * 16, ICONEXE_RVA + data);
			IconExe_Put32(rsrc + leaves + k * 16 + 4, n);
			data += n;
			k++;
		}
	}
	IconExe_Put32(out + opt + dirs + 2 * 8, ICONEXE_RVA); // the resources' directory
	IconExe_Put32(out + opt + dirs + 2 * 8 + 4, data);
	IconExe_Put32(out + sect + 8, data);
	IconExe_Put32(out + sect + 16, data);
	size = ICONEXE_RAW + data;
	if(tail)
	{
		memcpy(out + size, tail, strlen(tail));
		size += (uint32_t)strlen(tail);
	}
	return size;
}

#endif // BGI_TESTS_ICONEXE_H_
