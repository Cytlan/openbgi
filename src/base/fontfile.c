/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fontfile.c - the family names of a font file (src/include/bgi/fontfile.h)
 *
 * The sfnt structure: a collection header ("ttcf") with the offsets of its
 * faces, or a single face; each face a table directory, of which the
 * "name" table is read.  Its records in Unicode (the Unicode platform, and
 * the Windows platform's encodings 1 and 10) are UTF-16BE; the Macintosh
 * platform's Roman names are taken when they are plain ASCII.
 */
#include "bgi/fontfile.h"
#include "bgi/bytes.h"

typedef struct Names
{
	char (*out)[FONTFILE_NAME_MAX];
	int max, count;
} Names_t;

// a name the list does not have yet
static void Add(Names_t* l, const char* name)
{
	int i;
	if(!name[0])
		return;
	for(i = 0; i < l->count && i < l->max; i++)
		if(strcmp(l->out[i], name) == 0)
			return;
	if(l->count < l->max)
		snprintf(l->out[l->count], FONTFILE_NAME_MAX, "%s", name);
	l->count++;
}

// one name record's string as UTF-8 into out; 0 when its encoding is not one taken
static int Decode(const uint8_t* s, uint32_t len, uint32_t platform, uint32_t encoding, char* out, size_t n)
{
	size_t off = 0;
	uint32_t i;
	if(platform == 1)
	{ // Macintosh: Roman, ASCII only
		if(encoding != 0)
			return 0;
		for(i = 0; i < len && off + 1 < n; i++)
		{
			if(s[i] >= 0x80)
				return 0;
			out[off++] = (char)s[i];
		}
		out[off] = 0;
		return 1;
	}
	if(!(platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10))))
		return 0;
	for(i = 0; i + 1 < len;)
	{
		uint32_t c = Be16(s + i);
		i += 2;
		if(c >= 0xd800 && c < 0xdc00 && i + 1 < len && Be16(s + i) >= 0xdc00 && Be16(s + i) < 0xe000)
		{
			c = 0x10000 + ((c - 0xd800) << 10) + (Be16(s + i) - 0xdc00);
			i += 2;
		}
		if(off + 5 >= n)
			break;
		if(c < 0x80)
			out[off++] = (char)c;
		else if(c < 0x800)
		{
			out[off++] = (char)(0xc0 | (c >> 6));
			out[off++] = (char)(0x80 | (c & 0x3f));
		}
		else if(c < 0x10000)
		{
			out[off++] = (char)(0xe0 | (c >> 12));
			out[off++] = (char)(0x80 | ((c >> 6) & 0x3f));
			out[off++] = (char)(0x80 | (c & 0x3f));
		}
		else
		{
			out[off++] = (char)(0xf0 | (c >> 18));
			out[off++] = (char)(0x80 | ((c >> 12) & 0x3f));
			out[off++] = (char)(0x80 | ((c >> 6) & 0x3f));
			out[off++] = (char)(0x80 | (c & 0x3f));
		}
	}
	out[off] = 0;
	return 1;
}

/* the table `tag` of the face whose table directory is at `face`: its
 * offset in the data, with its length in *len; 0 when the face has none
 * or it does not lie within the data */
static uint32_t FindTable(const uint8_t* data, size_t size, uint32_t face, const char* tag, uint32_t* len)
{
	uint32_t tables, i, off = 0;
	*len = 0;
	if((size_t)face + 12 > size)
		return 0;
	tables = Be16(data + face + 4);
	for(i = 0; i < tables; i++)
	{
		const uint8_t* r;
		if((size_t)face + 12 + (size_t)(i + 1) * 16 > size)
			return 0;
		r = data + face + 12 + i * 16;
		if(memcmp(r, tag, 4) == 0)
		{
			off = Be32(r + 8);
			*len = Be32(r + 12);
		}
	}
	if(!off || (size_t)off + *len > size || off + *len < off)
		return 0;
	return off;
}

/* the family names of the face whose table directory is at `face`: with
 * `english` 1 only the Windows platform's English (US) ones, with 0 all
 * the others */
static void FaceNames(const uint8_t* data, size_t size, uint32_t face, int english, Names_t* l)
{
	uint32_t i, name, nameLen, count, strings;
	name = FindTable(data, size, face, "name", &nameLen);
	if(!name || nameLen < 6)
		return;
	count = Be16(data + name + 2);
	strings = name + Be16(data + name + 4);
	for(i = 0; i < count && 6 + (size_t)(i + 1) * 12 <= nameLen; i++)
	{
		const uint8_t* r = data + name + 6 + i * 12;
		uint32_t platform = Be16(r), encoding = Be16(r + 2), lang = Be16(r + 4), id = Be16(r + 6);
		uint32_t len = Be16(r + 8), off = strings + Be16(r + 10);
		int isEnglish = platform == 3 && lang == 0x0409;
		char text[FONTFILE_NAME_MAX];
		if(id != 1 || isEnglish != english || (size_t)off + len > size)
			continue;
		if(Decode(data + off, len, platform, encoding, text, sizeof text))
			Add(l, text);
	}
}

int FontFile_Faces(const uint8_t* data, size_t size)
{
	uint32_t faces;
	if(size < 12)
		return 0;
	if(memcmp(data, "ttcf", 4) == 0)
	{
		faces = Be32(data + 8);
		return faces <= 256 && 12 + (size_t)faces * 4 <= size ? (int)faces : 0;
	}
	return Be32(data) == 0x00010000u || memcmp(data, "OTTO", 4) == 0 || memcmp(data, "true", 4) == 0;
}

// the offset of face `face`'s table directory; 0 for a single font
static uint32_t FaceDir(const uint8_t* data, int face)
{
	return memcmp(data, "ttcf", 4) == 0 ? Be32(data + 12 + (size_t)face * 4) : 0;
}

int FontFile_Families(const uint8_t* data, size_t size, int face, char (*out)[FONTFILE_NAME_MAX], int max)
{
	Names_t l;
	uint32_t dir;
	if(face < 0 || face >= FontFile_Faces(data, size))
		return 0;
	dir = FaceDir(data, face);
	l.out = out;
	l.max = max;
	l.count = 0;
	FaceNames(data, size, dir, 1, &l);
	FaceNames(data, size, dir, 0, &l);
	return l.count;
}

uint32_t FontFile_CodePages(const uint8_t* data, size_t size, int face)
{
	uint32_t os2, len;
	if(face < 0 || face >= FontFile_Faces(data, size))
		return 0;
	os2 = FindTable(data, size, FaceDir(data, face), "OS/2", &len);
	if(!os2 || len < 86 || Be16(data + os2) < 1) // version 0 has no code page ranges
		return 0;
	return Be32(data + os2 + 78);
}
