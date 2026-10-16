/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * hvl.c - the hash verification list "BGI.hvl" and its file hash
 *         (install_internal.h; the script entry points in inc/bgi/install.h)
 *
 * The list sits next to the engine on the disc: a 16-byte header
 * ("BHV_____", the record count at offset 12) and 0x40-byte records of a
 * file name (0x38 bytes) and the file's 8-byte hash.  The installer
 * verifies every copied file that is listed (copy.c); the scripts can
 * hash files and buffers themselves ("80 E9", "81 E9").
 *
 * The hash over the bytes c of a file: h = h * 233 + c; b4 += (uint8_t)h;
 * b5 ^= (uint8_t)h; b6 += c; b7 ^= c, all starting at 0.  Its 8-byte form
 * (in the records and in the state the scripts hold) is h as stored in
 * memory followed by b4 .. b7.
 */
#include "install_internal.h"
#include "bgi/file.h"

// feed n bytes at p into the hash state
void HvHash_Update(HvHash_t* s, const uint8_t* p, uint32_t n)
{
	uint32_t i;
	for(i = 0; i < n; i++)
	{
		uint8_t c = p[i];
		s->h = s->h * 233u + c;
		s->b4 = (uint8_t)(s->b4 + (uint8_t)s->h);
		s->b5 ^= (uint8_t)s->h;
		s->b6 = (uint8_t)(s->b6 + c);
		s->b7 ^= c;
	}
}

/* "81 E9": continue the hash held in the 8-byte `state` (in script memory,
 * possibly unaligned) over the n bytes at data; the state starts as eight
 * zero bytes. */
void HvHash_UpdateBuf(uint8_t* state, const void* data, uint32_t n)
{
	HvHash_t hash;
	memcpy(&hash.h, state, 4);
	hash.b4 = state[4];
	hash.b5 = state[5];
	hash.b6 = state[6];
	hash.b7 = state[7];
	HvHash_Update(&hash, (const uint8_t*)data, n);
	memcpy(state, &hash.h, 4);
	state[4] = hash.b4;
	state[5] = hash.b5;
	state[6] = hash.b6;
	state[7] = hash.b7;
}

/* "80 E9" of 1.69 build 472 on: the hash of a whole file, read in 64 KB
 * blocks, into the 8 bytes at `out` (zeroed first); 1 done, 0 when the
 * file cannot be opened (`out` is untouched then). */
int HvHash_File(uint8_t* out, const char* path)
{
	File_t f;
	HvHash_t hash = {0, 0, 0, 0, 0};
	uint8_t* buf;
	uint32_t remaining;
	File_Ctor(&f);
	if(!File_OpenRead(&f, path))
	{
		File_Dtor(&f);
		return 0;
	}
	memset(out, 0, 8);
	remaining = File_Size(&f);
	buf = (uint8_t*)BGI_Alloc(0x10000);
	while(remaining)
	{
		uint32_t n = remaining < 0x10000u ? remaining : 0x10000u;
		File_Read(&f, buf, n);
		HvHash_Update(&hash, buf, n);
		remaining -= n;
	}
	BGI_Free(buf);
	File_Close(&f);
	File_Dtor(&f);
	memcpy(out, &hash.h, 4);
	out[4] = hash.b4;
	out[5] = hash.b5;
	out[6] = hash.b6;
	out[7] = hash.b7;
	return 1;
}

/* Load the list at `path`: the records (0x40 bytes each) into a new buffer
 * at *outRecs, their number into *outCount.  Returns 0, or 0x80000001 when
 * the file cannot be opened, 0x80000002 for a bad header, 0x80000003 when
 * the records are short; the outputs are untouched then. */
uint32_t Hvl_Load(int* outCount, uint8_t** outRecs, const char* path)
{
	File_t f;
	uint8_t hdr[16];
	uint32_t r = 0;
	File_Ctor(&f);
	if(!File_OpenRead(&f, path))
	{
		r = 0x80000001u;
	}
	else if(File_Read(&f, hdr, 16) != 16 || memcmp(hdr, "BHV_____", 8) != 0)
	{
		r = 0x80000002u;
	}
	else
	{
		uint32_t count, size;
		uint8_t* recs;
		memcpy(&count, hdr + 12, 4);
		size = count << 6;                           // 0x40 bytes per record
		recs = (uint8_t*)BGI_Alloc(size ? size : 1); // an empty list still gets a buffer to free
		if(File_Read(&f, recs, size) != size)
		{
			BGI_Free(recs);
			r = 0x80000003u;
		}
		else
		{
			*outCount = (int)count;
			*outRecs = recs;
		}
	}
	File_Dtor(&f);
	return r;
}
