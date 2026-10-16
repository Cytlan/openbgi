/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * arcwrite.c - writing PackFile / BURIKO ARC20 archives for the tools
 *              (interface in arcread.h)
 *
 * The counterpart of arcread.c: a list of named entries becomes an archive
 * image in the layout the games use - the 12-byte magic, the entry count,
 * one index record per entry (16-byte or 0x60-byte name, offset from the
 * end of the index, size, zero padding), then the data in order.  The
 * entries' bytes are stored as given (compressed or not: the engine
 * decodes DSC when it finds it).
 */
#include "bgi/arcread.h"

static void Wr32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

uint8_t* ArcWrite_Build(const ArcWriteEntry_t* entries, int count, int arc20, uint32_t* size)
{
	uint32_t recSize = arc20 ? 0x80 : 32, nameLen = arc20 ? 0x60 : 16;
	uint32_t indexSize = 16 + (uint32_t)count * recSize, total = indexSize, off = 0;
	uint8_t* out;
	int i;
	for(i = 0; i < count; i++)
		total += entries[i].size;
	out = (uint8_t*)calloc(total + 1, 1);
	memcpy(out, arc20 ? "BURIKO ARC20" : "PackFile    ", 12);
	Wr32(out + 12, (uint32_t)count);
	for(i = 0; i < count; i++)
	{
		uint8_t* rec = out + 16 + (uint32_t)i * recSize;
		size_t n = strlen(entries[i].name);
		if(n > nameLen - 1)
			n = nameLen - 1;
		memcpy(rec, entries[i].name, n);
		Wr32(rec + nameLen, off);
		Wr32(rec + nameLen + 4, entries[i].size);
		memcpy(out + indexSize + off, entries[i].data, entries[i].size);
		off += entries[i].size;
	}
	*size = total;
	return out;
}
