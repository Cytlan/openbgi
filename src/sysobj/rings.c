/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * rings.c - bounded most-recent-first record lists (src/include/bgi/sysobj/rings.h)
 *
 * The rings are a singly linked list in creation order (newest ring
 * first), each with a singly linked list of its records, newest first.
 */
#include "bgi/sysobj/rings.h"
#include "bgi/core.h"

typedef struct RingItem // one record
{
	void* data;            // elemSize bytes, owned
	struct RingItem* next; // the next older record
} RingItem_t;

typedef struct Ring // one ring
{
	uint32_t id;       // the id Ring_Create handed out
	uint32_t maxCount; // records kept
	uint32_t elemSize; // bytes per record
	RingItem_t* items; // newest first
	struct Ring* next; // the next older ring
} Ring_t;

// the ring with id `id`, NULL when there is none
static Ring_t* Ring_Find(OpenBGI_t* o, uint32_t id)
{
	Ring_t* r;
	for(r = o->sysobj.ringList; r; r = r->next)
		if(r->id == id)
			return r;
	return NULL;
}

// Create an empty ring with the next id in *outId: 0 ok, 2 for a zero count or size.
uint32_t Ring_Create(OpenBGI_t* o, uint32_t* outId, uint32_t maxCount, uint32_t elemSize)
{
	Ring_t* r;
	if(maxCount == 0 || elemSize == 0)
		return 2;
	r = (Ring_t*)BGI_Alloc(sizeof(Ring_t));
	r->id = ++o->sysobj.ringSerial;
	r->maxCount = maxCount;
	r->elemSize = elemSize;
	r->items = NULL;
	r->next = o->sysobj.ringList;
	o->sysobj.ringList = r;
	*outId = r->id;
	return 0;
}

// Delete a ring with its records: 0 ok, 1 unknown ring.
uint32_t Ring_Delete(OpenBGI_t* o, uint32_t id)
{
	Ring_t **link = &o->sysobj.ringList, *r;
	RingItem_t* it;
	for(r = o->sysobj.ringList; r; link = &r->next, r = r->next)
	{
		if(r->id != id)
			continue;
		*link = r->next;
		for(it = r->items; it;)
		{
			RingItem_t* next = it->next;
			BGI_Free(it->data);
			BGI_Free(it);
			it = next;
		}
		BGI_Free(r);
		return 0;
	}
	return 1;
}

void Ring_DeleteAll(OpenBGI_t* o)
{
	while(o->sysobj.ringList)
		Ring_Delete(o, o->sysobj.ringList->id);
}

// The records in ring `id` into *out: 0 ok, 1 unknown ring.
uint32_t Ring_Count(OpenBGI_t* o, uint32_t* out, uint32_t id)
{
	Ring_t* r = Ring_Find(o, id);
	RingItem_t* it;
	uint32_t n = 0;
	if(!r)
		return 1;
	for(it = r->items; it; it = it->next)
		n++;
	*out = n;
	return 0;
}

// Insert a copy of the record at the front, then trim the list to maxCount records: 0 ok, 1 unknown ring.
uint32_t Ring_Push(OpenBGI_t* o, uint32_t id, const void* data)
{
	Ring_t* r = Ring_Find(o, id);
	RingItem_t *it, *prev = NULL;
	uint32_t i;
	if(!r)
		return 1;
	it = (RingItem_t*)BGI_Alloc(sizeof(RingItem_t));
	it->data = BGI_Alloc(r->elemSize);
	memcpy(it->data, data, r->elemSize);
	it->next = r->items;
	r->items = it;
	for(i = 0, it = r->items; it; i++)
	{
		RingItem_t* next = it->next;
		if(i < r->maxCount)
		{
			prev = it;
		}
		else
		{
			BGI_Free(it->data);
			BGI_Free(it);
			prev->next = NULL;
		}
		it = next;
	}
	return 0;
}

// Copy record `idx` (0 = newest) into dst: 0 ok, 1 unknown ring, 2 index out of range.
uint32_t Ring_Get(OpenBGI_t* o, void* dst, uint32_t id, uint32_t idx)
{
	Ring_t* r = Ring_Find(o, id);
	RingItem_t* it;
	uint32_t i;
	if(!r)
		return 1;
	for(i = 0, it = r->items; it; it = it->next, i++)
	{
		if(i == idx)
		{
			memcpy(dst, it->data, r->elemSize);
			return 0;
		}
	}
	return 2;
}

/* "80 9E" (1.616 on): remove `count` records of ring `id` from index `idx`
 * on (0 = newest; fewer when the list ends first): 0 ok, 1 unknown ring,
 * 2 index out of range (past the last record). */
uint32_t Ring_Remove(OpenBGI_t* o, uint32_t id, uint32_t idx, uint32_t count)
{
	Ring_t* r = Ring_Find(o, id);
	RingItem_t **link, *it;
	uint32_t i;
	if(!r)
		return 1;
	link = &r->items;
	for(i = 0; i < idx; i++)
	{
		if(!*link)
			return 2;
		link = &(*link)->next;
	}
	if(!*link)
		return 2;
	for(i = 0; i < count && *link; i++)
	{
		it = *link;
		*link = it->next;
		BGI_Free(it->data);
		BGI_Free(it);
	}
	return 0;
}
