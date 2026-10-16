/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dicts.c - string-keyed record tables (inc/bgi/sysobj/dicts.h)
 *
 * The dictionaries are listed by id (a counter from 0, reset with the
 * engine state); each is a singly linked list of entries in insertion
 * order, searched linearly.
 */
#include "bgi/sysobj/dicts.h"

typedef struct DictEntry // one entry
{
	char* key;              // owned copy
	void* data;             // elemSize bytes, owned
	struct DictEntry* next; // the next younger entry
} DictEntry_t;

typedef struct Dict // one dictionary
{
	uint32_t elemSize;    // bytes per record
	DictEntry_t* entries; // in insertion order
} Dict_t;

typedef struct DictNode // the id table
{
	uint32_t id;           // the id Dict_Create handed out
	Dict_t* dict;          // the dictionary, owned
	struct DictNode* next; // the next older dictionary
} DictNode_t;

static DictNode_t* gDictList; // every dictionary, newest first
static uint32_t gDictSerial;  // the next id

// an empty dictionary; a zero elemSize (which Dict_Create rejects) would mean 0x400
static Dict_t* Dict_New(uint32_t elemSize)
{
	Dict_t* d = (Dict_t*)BGI_Alloc(sizeof(Dict_t));
	d->elemSize = elemSize ? elemSize : 0x400;
	d->entries = NULL;
	return d;
}

// unlink and free the entry of `key`; 0 ok, 0x80000001 unknown key
static uint32_t Dict_RemoveKey(Dict_t* d, const char* key)
{
	DictEntry_t** link = &d->entries;
	while(*link)
	{
		DictEntry_t* e = *link;
		if(strcmp(e->key, key) == 0)
		{
			*link = e->next;
			BGI_Free(e->key);
			BGI_Free(e->data);
			BGI_Free(e);
			return 0;
		}
		link = &e->next;
	}
	return 0x80000001u;
}

// free a dictionary with its entries
static void Dict_Free(Dict_t* d)
{
	while(d->entries)
		Dict_RemoveKey(d, d->entries->key);
	BGI_Free(d);
}

// replace the data of an existing key or append a new entry
static void Dict_SetKey(Dict_t* d, const char* key, const void* data)
{
	DictEntry_t **link = &d->entries, *e;
	while(*link && strcmp((*link)->key, key) != 0)
		link = &(*link)->next;
	if(!*link)
	{
		e = (DictEntry_t*)BGI_Alloc(sizeof(DictEntry_t));
		e->key = BGI_Strdup(key);
		e->data = BGI_Alloc(d->elemSize);
		e->next = NULL;
		*link = e;
	}
	memcpy((*link)->data, data, d->elemSize);
}

// 0 ok, 0x80000001 unknown key
static uint32_t Dict_GetKey(Dict_t* d, void* dst, const char* key)
{
	DictEntry_t* e;
	for(e = d->entries; e; e = e->next)
	{
		if(strcmp(e->key, key) == 0)
		{
			memcpy(dst, e->data, d->elemSize);
			return 0;
		}
	}
	return 0x80000001u;
}

// 0 ok, 0x80000002 no entry at that position
static uint32_t Dict_GetAt(Dict_t* d, void* dst, uint32_t idx)
{
	DictEntry_t* e;
	uint32_t i = 0;
	for(e = d->entries; e; e = e->next, i++)
	{
		if(i == idx)
		{
			memcpy(dst, e->data, d->elemSize);
			return 0;
		}
	}
	return 0x80000002u;
}

// the id table's node for dictionary `id`, NULL when there is none
static DictNode_t* DictTable_Find(uint32_t id)
{
	DictNode_t* n;
	for(n = gDictList; n; n = n->next)
		if(n->id == id)
			return n;
	return NULL;
}

// Create a dictionary with the next id (ids count from 0): 0 ok, 0x80000001 for an elemSize below 2.
uint32_t Dict_Create(uint32_t* outId, uint32_t elemSize)
{
	DictNode_t* n;
	if(elemSize <= 1)
		return 0x80000001u;
	n = (DictNode_t*)BGI_Alloc(sizeof(DictNode_t));
	n->id = gDictSerial++;
	n->dict = Dict_New(elemSize);
	n->next = gDictList;
	gDictList = n;
	*outId = n->id;
	return 0;
}

// Delete a dictionary with its entries: 0 ok, 0x80000002 unknown dictionary.
uint32_t Dict_Delete(uint32_t id)
{
	DictNode_t** link = &gDictList;
	while(*link)
	{
		DictNode_t* n = *link;
		if(n->id == id)
		{
			*link = n->next;
			if(n->dict)
				Dict_Free(n->dict);
			BGI_Free(n);
			return 0;
		}
		link = &n->next;
	}
	return 0x80000002u;
}

// Delete every dictionary and restart the ids at 0.
void Dict_DeleteAll(void)
{
	while(gDictList)
		Dict_Delete(gDictList->id);
	gDictSerial = 0;
}

// Store a copy of the record at data under `key`, replacing an existing one: 0 ok, 0x80000002 unknown dictionary.
uint32_t Dict_Set(uint32_t id, const char* key, const void* data)
{
	DictNode_t* n = DictTable_Find(id);
	if(!n)
		return 0x80000002u;
	Dict_SetKey(n->dict, key, data);
	return 0;
}

// Remove the entry of `key`: 0 ok, 0x80000002 unknown dictionary, 0x80000003 unknown key.
uint32_t Dict_Remove(uint32_t id, const char* key)
{
	DictNode_t* n = DictTable_Find(id);
	if(!n)
		return 0x80000002u;
	return Dict_RemoveKey(n->dict, key) ? 0x80000003u : 0;
}

/* Copy a record into dst: by key when one is given, else the one at
 * position `idx` in insertion order.  0 ok, 0x80000002 unknown
 * dictionary, 0x80000003 unknown key or position. */
uint32_t Dict_Get(void* dst, uint32_t id, const char* key, uint32_t idx)
{
	DictNode_t* n = DictTable_Find(id);
	uint32_t r;
	if(!n)
		return 0x80000002u;
	r = key ? Dict_GetKey(n->dict, dst, key) : Dict_GetAt(n->dict, dst, idx);
	return r ? 0x80000003u : 0;
}
