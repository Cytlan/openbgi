/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * savefile.c - the save files "BGInnnn.cad" (src/include/bgi/file.h)
 *
 * A save file is the global memory with a 0x40-byte header:
 *
 *     +00  the time stamp (a SYSTEMTIME, 16 bytes)
 *     +10  the comment (40 bytes, NUL padded)
 *     +38  the sum and +39 the XOR of the scrambled memory bytes
 *     +3A  two bytes of the generator, +3C the "scrambled" flag
 *     +40  the global memory (o->mem.globalMemSize bytes)
 *
 * Unless "80 74" turns it off, the memory is scrambled by adding the
 * output of the hash generator seeded with the time stamp's millisecond
 * field; the sum and XOR let a load detect a damaged file.  "80 78"
 * writes a slot, "80 79" loads one, "80 7B" checks one and "80 7A" reads
 * its header; the files live in the base directory (read from the
 * alternative one too).
 */
#include "bgi/file.h"
#include "bgi/codec.h"
#include "bgi/sys.h"
#include "bgi/engine.h"
#include "bgi/core.h"

void Set_SaveEncode(OpenBGI_t* o, int on)
{
	o->files.saveEncode = on;
}

// "BGI0012.cad" for slot 12
void SaveFileName(char* dst, int slot)
{
	sprintf(dst, "%s%.4d%s", "BGI", slot, ".cad");
}

// scramble the memory part of a save image in place and fill in the checks
static void SaveScramble(OpenBGI_t* o, uint8_t* s)
{
	uint32_t i;
	BGI_HashSeed(o, *(uint16_t*)(s + 0xe)); // the millisecond field of the time stamp
	s[0x38] = s[0x39] = 0;
	for(i = 0; i < o->mem.globalMemSize; i++)
	{
		s[0x40 + i] = (uint8_t)(s[0x40 + i] + (uint8_t)BGI_HashNext(o));
		s[0x38] = (uint8_t)(s[0x38] + s[0x40 + i]);
		s[0x39] ^= s[0x40 + i];
	}
	s[0x3a] = (uint8_t)BGI_HashNext(o);
	s[0x3b] = (uint8_t)BGI_HashNext(o);
	s[0x3c] = 1;
}

// the inverse; 1 when the checks match (the memory is unscrambled either way)
static int SaveUnscramble(OpenBGI_t* o, uint8_t* s)
{
	uint8_t sum = 0, xr = 0;
	uint32_t i;
	BGI_HashSeed(o, *(uint16_t*)(s + 0xe));
	for(i = 0; i < o->mem.globalMemSize; i++)
	{
		xr ^= s[0x40 + i];
		sum = (uint8_t)(sum + s[0x40 + i]);
		s[0x40 + i] = (uint8_t)(s[0x40 + i] - (uint8_t)BGI_HashNext(o));
	}
	return s[0x38] == sum && s[0x39] == xr;
}

// "80 78": write the global memory with a comment into slot `slot`; 1 when the file was written completely.
int SaveWrite(OpenBGI_t* o, int slot, const char* comment)
{
	char name[0x104];
	uint32_t size = o->mem.globalMemSize + 0x40;
	uint8_t* s = (uint8_t*)BGI_Calloc(size);
	OpenBGI_DateTime_t lt;
	int ok;
	SaveFileName(name, slot);
	Clock_LocalTime(o, &lt);
	memcpy(s, &lt, 16);
	memset(s + 0x10, 0, 0x28);
	strcpy((char*)s + 0x10, comment); // the comment field is 40 bytes; the copy is not bounded
	if(o->vm.follow.live)
		Follow_Keep(o); // (what the save holds of the engine's own program file: follow.h)
	memcpy(s + 0x40, o->mem.globalMem, o->mem.globalMemSize);
	if(o->files.saveEncode)
		SaveScramble(o, s);
	ok = (uint32_t)WriteTextFile(o, name, s, size) == size;
	BGI_Free(s);
	return ok;
}

/* read and unscramble a slot into s (o->mem.globalMemSize + 0x40
 * bytes); 0 ok, 1 missing, 2 wrong size (the global memory size differs
 * from the one saved), 3 corrupt (the checks do not match) */
static int SaveRead(OpenBGI_t* o, uint8_t* s, int slot)
{
	char name[0x104];
	uint32_t n;
	SaveFileName(name, slot);
	n = ReadFileAt(o, s, name);
	if(n != o->mem.globalMemSize + 0x40)
		return n ? 2 : 1;
	if(o->files.saveEncode && !SaveUnscramble(o, s))
		return 3;
	return 0;
}

// "80 79": load slot `slot` into the global memory; the codes of SaveRead (the memory is untouched on failure).
int SaveLoad(OpenBGI_t* o, int slot)
{
	uint8_t* s = (uint8_t*)BGI_Alloc(o->mem.globalMemSize + 0x40);
	int r = SaveRead(o, s, slot);
	if(r == 0)
		memcpy(o->mem.globalMem, s + 0x40, o->mem.globalMemSize);
	BGI_Free(s);
	return r;
}

// "80 7B": check slot `slot` without loading it; the codes of SaveRead.
int SaveCheck(OpenBGI_t* o, int slot)
{
	uint8_t* s = (uint8_t*)BGI_Alloc(o->mem.globalMemSize + 0x40);
	int r = SaveRead(o, s, slot);
	BGI_Free(s);
	return r;
}

// "80 7A": the 0x40-byte header of slot `slot` into dst; 0 ok, 1 missing, 2 wrong size (no checks).
int SaveReadHeader(OpenBGI_t* o, int slot, void* dst)
{
	char name[0x104];
	uint32_t size = o->mem.globalMemSize + 0x40, n;
	uint8_t* s = (uint8_t*)BGI_Alloc(size);
	int r;
	SaveFileName(name, slot);
	n = ReadFileAt(o, s, name);
	if(n == size)
	{
		memcpy(dst, s, 0x40);
		r = 0;
	}
	else
		r = n ? 2 : 1;
	BGI_Free(s);
	return r;
}
