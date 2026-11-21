/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * strgroups.c - named groups of string pairs (src/include/bgi/sysobj/strgroups.h)
 *
 * The groups and their entries are singly linked lists, newest first, so
 * a pair loaded later shadows an earlier one with the same key.  Which
 * string of a pair is the key is taken from the block's order (the
 * first); no game examined so far uses "81 DA", so this is unconfirmed.
 */
#include "bgi/sysobj/strgroups.h"
#include "bgi/bytes.h"
#include "bgi/core.h"

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

// Drop every group with its pairs.
void StrGroups_Clear(OpenBGI_t* o)
{
	while(o->sysobj.strGroups)
	{
		StrGroup_t* g = o->sysobj.strGroups;
		o->sysobj.strGroups = g->next;
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
static StrGroup_t* StrGroups_Find(OpenBGI_t* o, const char* name)
{
	StrGroup_t* g;
	for(g = o->sysobj.strGroups; g; g = g->next)
		if(strcmp(g->name, name) == 0)
			return g;
	return NULL;
}

/* Parse a block into the groups (see the header for the layout): a group
 * already known receives the new pairs, a new one is added.  The block is
 * trusted: nothing checks that it stays within `size` while parsing; the
 * result 1 only says that it ended exactly there.  A NULL block clears. */
int StrGroups_Load(OpenBGI_t* o, const uint8_t* data, uint32_t size)
{
	const uint8_t* p = data;
	uint32_t count, i;
	if(!data)
	{
		StrGroups_Clear(o);
		return 1;
	}
	count = Le32(p);
	p += 4;
	for(i = 0; i < count; i++)
	{
		StrGroup_t* g = StrGroups_Find(o, (const char*)p);
		uint32_t n, j;
		if(!g)
		{
			g = (StrGroup_t*)BGI_Calloc(sizeof *g);
			g->name = BGI_Strdup((const char*)p);
			g->next = o->sysobj.strGroups;
			o->sysobj.strGroups = g;
		}
		p += strlen((const char*)p) + 1;
		n = Le32(p);
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
const char* StrGroups_Lookup(OpenBGI_t* o, const char* group, const char* key)
{
	StrGroup_t* g = StrGroups_Find(o, group);
	StrGroupEntry_t* e;
	if(!g)
		return NULL;
	for(e = g->entries; e; e = e->next)
		if(strcmp(e->key, key) == 0)
			return e->value;
	return NULL;
}
