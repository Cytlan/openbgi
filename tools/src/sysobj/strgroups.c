/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * strgroups.c - named groups of string pairs (inc/bgi/sysobj/strgroups.h)
 *
 * The groups and their entries are singly linked lists, newest first, so
 * a pair loaded later shadows an earlier one with the same key.  Which
 * string of a pair is the key is taken from the block's order (the
 * first); no game examined so far uses "81 DA", so this is unconfirmed.
 */
#include "bgi/sysobj/strgroups.h"

typedef struct StrGroupEntry // one key / value pair
{
	char* key; // owned copies
	char* value;
	struct StrGroupEntry* next; // the next older pair
} StrGroupEntry_t;

typedef struct StrGroup // one named group
{
	char* name;               // owned copy
	StrGroupEntry_t* entries; // newest first
	struct StrGroup* next;    // the next older group
} StrGroup_t;

static StrGroup_t* gStrGroups; // every group, newest first

// Drop every group with its pairs.
void StrGroups_Clear(void)
{
	while(gStrGroups)
	{
		StrGroup_t* g = gStrGroups;
		gStrGroups = g->next;
		while(g->entries)
		{
			StrGroupEntry_t* e = g->entries;
			g->entries = e->next;
			BGI_Free(e->key);
			BGI_Free(e->value);
			BGI_Free(e);
		}
		BGI_Free(g->name);
		BGI_Free(g);
	}
}

// the group called `name`, NULL when there is none
static StrGroup_t* StrGroups_Find(const char* name)
{
	StrGroup_t* g;
	for(g = gStrGroups; g; g = g->next)
		if(strcmp(g->name, name) == 0)
			return g;
	return NULL;
}

// a little-endian u32 of the block
static uint32_t ReadU32(const uint8_t* p)
{
	return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* Parse a block into the groups (see the header for the layout): a group
 * already known receives the new pairs, a new one is added.  The block is
 * trusted: nothing checks that it stays within `size` while parsing; the
 * result 1 only says that it ended exactly there.  A NULL block clears. */
int StrGroups_Load(const uint8_t* data, uint32_t size)
{
	const uint8_t* p = data;
	uint32_t count, i;
	if(!data)
	{
		StrGroups_Clear();
		return 1;
	}
	count = ReadU32(p);
	p += 4;
	for(i = 0; i < count; i++)
	{
		StrGroup_t* g = StrGroups_Find((const char*)p);
		uint32_t n, j;
		if(!g)
		{
			g = (StrGroup_t*)BGI_Calloc(sizeof *g);
			g->name = BGI_Strdup((const char*)p);
			g->next = gStrGroups;
			gStrGroups = g;
		}
		p += strlen((const char*)p) + 1;
		n = ReadU32(p);
		p += 4;
		for(j = 0; j < n; j++)
		{
			StrGroupEntry_t* e = (StrGroupEntry_t*)BGI_Calloc(sizeof *e);
			e->key = BGI_Strdup((const char*)p);
			p += strlen((const char*)p) + 1;
			e->value = BGI_Strdup((const char*)p);
			p += strlen((const char*)p) + 1;
			e->next = g->entries;
			g->entries = e;
		}
	}
	return (uint32_t)(p - data) == size;
}

// The value of `key` in `group` (the most recently loaded one), NULL when either is unknown.
const char* StrGroups_Lookup(const char* group, const char* key)
{
	StrGroup_t* g = StrGroups_Find(group);
	StrGroupEntry_t* e;
	if(!g)
		return NULL;
	for(e = g->entries; e; e = e->next)
		if(strcmp(e->key, key) == 0)
			return e->value;
	return NULL;
}
