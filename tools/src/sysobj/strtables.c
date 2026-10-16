/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * strtables.c - numbered string lists (inc/bgi/sysobj/strtables.h)
 *
 * The tables are a singly linked list in creation order, each with a
 * singly linked list of its strings in insertion order (so an index is
 * stable until the table is dropped).
 */
#include "bgi/sysobj/strtables.h"
#include "bgi/version.h"

typedef struct StrItem // one string of a table
{
	char* s;              // the string, owned
	struct StrItem* next; // the next string, in insertion order
} StrItem_t;

typedef struct StrTab // one table
{
	uint32_t id;         // the id the script chose
	uint32_t count;      // strings in `items`
	StrItem_t* items;    // the strings, oldest first
	struct StrTab* next; // the next table, in creation order
} StrTab_t;

static StrTab_t* gStrTabList; // every table, oldest first

// the table with id `id`, NULL when there is none
static StrTab_t* StrTab_Lookup(uint32_t id)
{
	StrTab_t* t;
	for(t = gStrTabList; t && t->id != id; t = t->next)
		;
	return t;
}

// free the strings of t and t itself (the caller has unlinked it)
static void StrTab_FreeItems(StrTab_t* t)
{
	StrItem_t* it;
	for(it = t->items; it;)
	{
		StrItem_t* n = it->next;
		BGI_Free(it->s);
		BGI_Free(it);
		it = n;
	}
	BGI_Free(t);
}

// Drop every table, or every table but GNAME_TABLE ("80 D8" passes keepNames; it matters from 1.616 on, when the names live there).
void StrTab_Clear(int keepNames)
{
	StrTab_t **link = &gStrTabList, *t;
	while((t = *link) != NULL)
	{
		if(keepNames && t->id == GNAME_TABLE)
		{
			link = &t->next;
			continue;
		}
		*link = t->next;
		StrTab_FreeItems(t);
	}
}

int StrTab_Count(uint32_t id)
{
	StrTab_t* t = StrTab_Lookup(id);
	return t ? (int)t->count : 0;
}

// the table, created empty at the end of the list when it is missing
static StrTab_t* StrTab_Need(uint32_t id)
{
	StrTab_t **tlink = &gStrTabList, *t;
	while(*tlink && (*tlink)->id != id)
		tlink = &(*tlink)->next;
	if(*tlink)
		return *tlink;
	t = (StrTab_t*)BGI_Alloc(sizeof(StrTab_t));
	t->id = id;
	t->count = 0;
	t->items = NULL;
	t->next = NULL;
	*tlink = t;
	return t;
}

// append a copy of s; its index
static int StrTab_Append(StrTab_t* t, const char* s)
{
	StrItem_t **ilink, *it;
	for(ilink = &t->items; *ilink; ilink = &(*ilink)->next)
		;
	it = (StrItem_t*)BGI_Alloc(sizeof(StrItem_t));
	it->s = BGI_Strdup(s);
	it->next = NULL;
	*ilink = it;
	return (int)t->count++;
}

// the index of the first string equal to s, -1 when absent or the table is unknown
int StrTab_Find(uint32_t id, const char* s)
{
	StrTab_t* t = StrTab_Lookup(id);
	StrItem_t* it;
	int i;
	if(!t)
		return -1;
	for(i = 0, it = t->items; it; it = it->next, i++)
		if(strcmp(it->s, s) == 0)
			return i;
	return -1;
}

/* Append a copy of s to the table (created when needed) and return its
 * index.  1.599 on keep each string once per table: adding a string that
 * is present yields its index ("80 DC" pushes that index). */
int StrTab_Add(uint32_t id, const char* s)
{
	StrTab_t* t = StrTab_Need(id);
	if(gEngine->gen >= GEN_1_599)
	{
		int i = StrTab_Find(id, s);
		if(i >= 0)
			return i;
	}
	return StrTab_Append(t, s);
}

// drop table `id`; 1 when it existed
int StrTab_Remove(uint32_t id)
{
	StrTab_t **tlink = &gStrTabList, *t;
	while(*tlink && (*tlink)->id != id)
		tlink = &(*tlink)->next;
	t = *tlink;
	if(!t)
		return 0;
	*tlink = t->next;
	StrTab_FreeItems(t);
	return 1;
}

/* Replace table `id` by the `count` NUL-terminated strings packed one
 * after the other at strs ("80 DA"); count 0 only drops the table.  The
 * strings are appended as they are, duplicates included.  1 done, 0 for a
 * negative count or a NULL strs. */
int StrTab_SetAll(uint32_t id, int count, const char* strs)
{
	StrTab_t* t;
	int i;
	if(count == 0)
	{
		StrTab_Remove(id);
		return 1;
	}
	if(count < 0 || !strs)
		return 0;
	StrTab_Remove(id);
	t = StrTab_Need(id);
	for(i = 0; i < count; i++)
	{
		StrTab_Append(t, strs);
		strs += strlen(strs) + 1;
	}
	return 1;
}

/* Pack every string of table `id`, terminators included, into dst in
 * table order ("80 DB"); a NULL dst only measures.  The bytes written (or
 * needed), 0 for an unknown table. */
int StrTab_Gather(char* dst, uint32_t id)
{
	StrTab_t* t = StrTab_Lookup(id);
	StrItem_t* it;
	int total = 0;
	if(!t)
		return 0;
	for(it = t->items; it; it = it->next)
	{
		int n = (int)strlen(it->s) + 1;
		if(dst)
		{
			memcpy(dst, it->s, (size_t)n);
			dst += n;
		}
		total += n;
	}
	return total;
}

// the length of string `idx` (without terminator) into *outLen ("80 DE"): 0 ok, 0x80000001 unknown table, 0x80000002 bad index
int StrTab_Length(int32_t* outLen, uint32_t id, uint32_t idx)
{
	StrTab_t* t = StrTab_Lookup(id);
	StrItem_t* it;
	uint32_t i;
	if(!t)
		return (int)0x80000001u;
	for(i = 0, it = t->items; it; it = it->next, i++)
		if(i == idx)
		{
			if(outLen)
				*outLen = (int32_t)strlen(it->s);
			return 0;
		}
	return (int)0x80000002u;
}

// copy string `idx` into dst ("80 DD"): 0 ok, 0x80000001 unknown table, 0x80000002 bad index
int StrTab_Get(char* dst, uint32_t id, uint32_t idx)
{
	StrTab_t* t = StrTab_Lookup(id);
	StrItem_t* it;
	uint32_t i;
	if(!t)
		return (int)0x80000001u;
	for(i = 0, it = t->items; it; it = it->next, i++)
	{
		if(i == idx)
		{
			strcpy(dst, it->s);
			return 0;
		}
	}
	return (int)0x80000002u;
}
