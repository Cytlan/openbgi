/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * flags.c - named groups of bit flags (src/include/bgi/sysobj/flags.h)
 *
 * The groups live in an array in the order they were registered (none is
 * ever removed but all at once), so a name is found by a linear search
 * and a new group goes at the end.  Until 1.599 the array has a fixed
 * number of slots and names are 1 to 23 characters; 1.616 on keep their
 * groups in a list without either limit, which the array grows to
 * match.
 */
#include "bgi/sysobj/flags.h"
#include "bgi/version.h"
#include "bgi/core.h"

struct FlagMgr
{
	uint32_t count;      // groups in use
	uint32_t capacity;   // slots allocated
	uint32_t limit;      // the groups allowed until 1.599
	FlagGroup_t* groups; // capacity slots, the first count in use
};

// the byte holding bit `idx` and the mask of the bit inside it
static uint32_t FlagByte(uint32_t idx)
{
	return idx >> 3;
}

static uint8_t FlagMask(uint32_t idx)
{
	return (uint8_t)(0x80 >> (idx & 7));
}

FlagMgr_t* FlagMgr_New(uint32_t maxGroups)
{
	FlagMgr_t* m = (FlagMgr_t*)BGI_Alloc(sizeof(FlagMgr_t));
	m->count = 0;
	m->capacity = maxGroups ? maxGroups : 16;
	m->limit = maxGroups;
	m->groups = (FlagGroup_t*)BGI_Calloc(m->capacity * sizeof(FlagGroup_t));
	return m;
}

// Drop every group.
void FlagMgr_Clear(FlagMgr_t* m)
{
	uint32_t i;
	for(i = 0; i < m->count; i++)
	{
		BGI_Free(m->groups[i].name);
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

uint32_t FlagMgr_Count(const FlagMgr_t* m)
{
	return m->count;
}

const FlagGroup_t* FlagMgr_Group(const FlagMgr_t* m, uint32_t i)
{
	return &m->groups[i];
}

// the group of that name, NULL when unknown
static FlagGroup_t* FlagMgr_Find(FlagMgr_t* m, const char* name)
{
	uint32_t i;
	for(i = 0; i < m->count; i++)
		if(strcmp(m->groups[i].name, name) == 0)
			return &m->groups[i];
	return NULL;
}

/* Register a group or resize an existing one: a new group's bits are
 * cleared, a resized group keeps its old bits as far as they fit. */
uint32_t FlagMgr_Register(OpenBGI_t* o, FlagMgr_t* m, const char* name, uint32_t bits)
{
	const int limited = o->engine.profile->gen < GEN_1_616;
	size_t len = strlen(name);
	uint32_t bytes = FlagBytes(bits);
	FlagGroup_t* g;

	if(limited && (len == 0 || len > 0x17))
		return FLAG_ERR_NAME;
	if(bytes == 0)
		return FLAG_ERR_BITS;
	g = FlagMgr_Find(m, name);
	if(g)
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
	if(limited && m->limit && m->count >= m->limit)
		return FLAG_ERR_FULL;
	if(m->count == m->capacity)
	{
		FlagGroup_t* ng = (FlagGroup_t*)BGI_Calloc(m->capacity * 2 * sizeof(FlagGroup_t));
		memcpy(ng, m->groups, m->capacity * sizeof(FlagGroup_t));
		BGI_Free(m->groups);
		m->groups = ng;
		m->capacity *= 2;
	}
	g = &m->groups[m->count++];
	g->name = BGI_Strdup(name);
	g->bits = bits;
	g->data = (uint8_t*)BGI_Calloc(bytes);
	return FLAG_OK;
}

// Register a group and load its bits from `src` (the database restores groups this way).
uint32_t FlagMgr_RegisterWithData(OpenBGI_t* o, FlagMgr_t* m, const char* name, uint32_t bits, const uint8_t* src)
{
	uint32_t r = FlagMgr_Register(o, m, name, bits);
	if(r == FLAG_OK)
		memcpy(FlagMgr_Find(m, name)->data, src, FlagBytes(bits));
	return r;
}

uint32_t FlagMgr_Merge(OpenBGI_t* o, FlagMgr_t* m, const char* name, uint32_t bits, const uint8_t* src)
{
	FlagGroup_t* g = FlagMgr_Find(m, name);
	uint32_t r, i, n;
	if(!g)
		return FlagMgr_RegisterWithData(o, m, name, bits, src);
	r = FlagMgr_Register(o, m, name, g->bits > bits ? g->bits : bits);
	if(r != FLAG_OK)
		return r;
	g = FlagMgr_Find(m, name);
	for(i = 0, n = FlagBytes(bits); i < n; i++)
		g->data[i] |= src[i];
	return FLAG_OK;
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

// ---- the engine's manager, as the "80 8x" instructions see it ----------------------------

// "80 88": 1 ok, 0 for any error
int GFlag_RegisterGroup(OpenBGI_t* o, const char* name, uint32_t bits)
{
	return FlagMgr_Register(o, o->sysobj.flagMgr, name, bits) == FLAG_OK;
}

uint32_t GFlag_SetBit(OpenBGI_t* o, const char* name, uint32_t idx, uint32_t val)
{
	return FlagMgr_SetBit(o->sysobj.flagMgr, name, idx, val);
}

uint32_t GFlag_SetBits(OpenBGI_t* o, const char* name, uint32_t idx, uint32_t val, uint32_t count)
{
	return FlagMgr_SetBits(o->sysobj.flagMgr, name, idx, val, count);
}

uint32_t GFlag_GetBit(OpenBGI_t* o, uint32_t* out, const char* name, uint32_t idx)
{
	return FlagMgr_GetBit(o->sysobj.flagMgr, out, name, idx);
}
