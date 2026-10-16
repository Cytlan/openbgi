/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * flags.c - named groups of bit flags (inc/bgi/sysobj/flags.h)
 *
 * The groups live in a fixed array of slots; a slot with no bits is free.
 * Registration scans the slots in order, so a name is found by a linear
 * search and a new group takes the first free slot.
 */
#include "bgi/sysobj/flags.h"

struct FlagMgr
{
	uint32_t count;      // groups in use
	uint32_t capacity;   // slots
	FlagGroup_t* groups; // capacity slots
};

FlagMgr_t* gFlagMgr;

// the byte holding bit `idx` and the mask of the bit inside it
static uint32_t FlagByte(uint32_t idx)
{
	return idx >> 3;
}

static uint8_t FlagMask(uint32_t idx)
{
	return (uint8_t)(0x80 >> (idx & 7));
}

// A manager with `maxGroups` empty slots.
FlagMgr_t* FlagMgr_New(uint32_t maxGroups)
{
	FlagMgr_t* m = (FlagMgr_t*)BGI_Alloc(sizeof(FlagMgr_t));
	m->count = 0;
	m->capacity = maxGroups;
	m->groups = (FlagGroup_t*)BGI_Calloc(maxGroups * sizeof(FlagGroup_t));
	return m;
}

// Drop every group; the slots are free again.
void FlagMgr_Clear(FlagMgr_t* m)
{
	uint32_t i;
	for(i = 0; i < m->count; i++)
	{
		BGI_Free(m->groups[i].data);
		memset(&m->groups[i], 0, sizeof(FlagGroup_t));
	}
	m->count = 0;
}

// Free the manager and its groups.
void FlagMgr_Delete(FlagMgr_t* m)
{
	FlagMgr_Clear(m);
	BGI_Free(m->groups);
	BGI_Free(m);
}

uint32_t FlagMgr_Capacity(const FlagMgr_t* m)
{
	return m->capacity;
}

const FlagGroup_t* FlagMgr_Group(const FlagMgr_t* m, uint32_t i)
{
	return &m->groups[i];
}

// the group of that name among the slots in use, NULL when unknown
static FlagGroup_t* FlagMgr_Find(FlagMgr_t* m, const char* name)
{
	uint32_t i;
	for(i = 0; i < m->count; i++)
		if(strcmp(m->groups[i].name, name) == 0)
			return &m->groups[i];
	return NULL;
}

/* Register a group or resize an existing one.  The scan runs over all
 * slots: the first empty slot takes a new group (bits cleared), a slot
 * with the same name is re-allocated with its old bits kept as far as
 * they fit. */
uint32_t FlagMgr_Register(FlagMgr_t* m, const char* name, uint32_t bits)
{
	size_t len = strlen(name);
	uint32_t bytes = FlagBytes(bits);
	uint32_t i;

	if(len == 0 || len > 0x17)
		return FLAG_ERR_NAME;
	if(bytes == 0)
		return FLAG_ERR_BITS;

	for(i = 0; i < m->capacity; i++)
	{
		FlagGroup_t* g = &m->groups[i];
		if(g->name[0] == 0)
		{
			strcpy(g->name, name);
			g->bits = bits;
			g->data = (uint8_t*)BGI_Calloc(bytes);
			m->count++;
			return FLAG_OK;
		}
		if(strcmp(g->name, name) == 0)
		{
			uint8_t* nd = (uint8_t*)BGI_Calloc(bytes);
			uint32_t keep = FlagBytes(g->bits);
			if(keep > bytes)
				keep = bytes;
			memcpy(nd, g->data, keep);
			BGI_Free(g->data);
			g->bits = bits;
			g->data = nd;
			return FLAG_OK;
		}
	}
	return FLAG_ERR_FULL;
}

// Register a group and load its bits from `src` (the database restores groups this way).
uint32_t FlagMgr_RegisterWithData(FlagMgr_t* m, const char* name, uint32_t bits, const uint8_t* src)
{
	uint32_t r = FlagMgr_Register(m, name, bits);
	if(r == FLAG_OK)
		memcpy(FlagMgr_Find(m, name)->data, src, FlagBytes(bits));
	return r;
}

// Set (val != 0) or clear bit `idx` of group `name`: FLAG_OK, FLAG_ERR_UNKNOWN, FLAG_ERR_INDEX.
uint32_t FlagMgr_SetBit(FlagMgr_t* m, const char* name, uint32_t idx, uint32_t val)
{
	FlagGroup_t* g = FlagMgr_Find(m, name);
	if(!g)
		return FLAG_ERR_UNKNOWN;
	if(idx >= g->bits)
		return FLAG_ERR_INDEX;
	if(val)
		g->data[FlagByte(idx)] |= FlagMask(idx);
	else
		g->data[FlagByte(idx)] &= (uint8_t)~FlagMask(idx);
	return FLAG_OK;
}

/* Set or clear `count` consecutive bits from `idx` on; the codes of
 * FlagMgr_SetBit plus FLAG_ERR_COUNT for a count of 0, above 0x10000 or
 * reaching past the group. */
uint32_t FlagMgr_SetBits(FlagMgr_t* m, const char* name, uint32_t idx, uint32_t val, uint32_t count)
{
	FlagGroup_t* g = FlagMgr_Find(m, name);
	uint32_t i;
	if(!g)
		return FLAG_ERR_UNKNOWN;
	if(idx >= g->bits)
		return FLAG_ERR_INDEX;
	if(count == 0 || count > 0x10000 || idx + count > g->bits)
		return FLAG_ERR_COUNT;
	for(i = 0; i < count; i++, idx++)
	{
		if(val)
			g->data[FlagByte(idx)] |= FlagMask(idx);
		else
			g->data[FlagByte(idx)] &= (uint8_t)~FlagMask(idx);
	}
	return FLAG_OK;
}

/* Read one bit: *out gets the masked byte, so any non-zero value means
 * "set" (the scripts test it for zero); the codes of FlagMgr_SetBit. */
uint32_t FlagMgr_GetBit(FlagMgr_t* m, uint32_t* out, const char* name, uint32_t idx)
{
	FlagGroup_t* g = FlagMgr_Find(m, name);
	if(!g)
		return FLAG_ERR_UNKNOWN;
	if(idx >= g->bits)
		return FLAG_ERR_INDEX;
	*out = g->data[FlagByte(idx)] & FlagMask(idx);
	return FLAG_OK;
}

// ---- the global manager, as the "80 8x" instructions see it ------------------------------

// "80 88": 1 ok, 0 for any error
int GFlag_RegisterGroup(const char* name, uint32_t bits)
{
	return FlagMgr_Register(gFlagMgr, name, bits) == FLAG_OK;
}

uint32_t GFlag_SetBit(const char* name, uint32_t idx, uint32_t val)
{
	return FlagMgr_SetBit(gFlagMgr, name, idx, val);
}

uint32_t GFlag_SetBits(const char* name, uint32_t idx, uint32_t val, uint32_t count)
{
	return FlagMgr_SetBits(gFlagMgr, name, idx, val, count);
}

uint32_t GFlag_GetBit(uint32_t* out, const char* name, uint32_t idx)
{
	return FlagMgr_GetBit(gFlagMgr, out, name, idx);
}
