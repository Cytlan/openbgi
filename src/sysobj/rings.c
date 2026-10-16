/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * rings.c - bounded most-recent-first record lists (inc/bgi/sysobj/rings.h)
 *
 * The rings are a singly linked list in creation order (newest ring
 * first), each with a singly linked list of its records, newest first.
 */
#include "bgi/sysobj/rings.h"

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

static Ring_t* gRingList;    // every ring, newest first
static uint32_t gRingSerial; // the last id handed out

// the ring with id `id`, NULL when there is none
static Ring_t* Ring_Find(uint32_t id)
{
	Ring_t* r;
	for(r = gRingList; r; r = r->next)
		if(r->id == id)
			return r;
	return NULL;
}

// Create an empty ring with the next id in *outId: 0 ok, 2 for a zero count or size.
uint32_t Ring_Create(uint32_t* outId, uint32_t maxCount, uint32_t elemSize)
{
	Ring_t* r;
	if(maxCount == 0 || elemSize == 0)
		return 2;
	r = (Ring_t*)BGI_Alloc(sizeof(Ring_t));
	r->id = ++gRingSerial;
	r->maxCount = maxCount;
	r->elemSize = elemSize;
	r->items = NULL;
	r->next = gRingList;
	gRingList = r;
	*outId = r->id;
	return 0;
}

// Delete a ring with its records: 0 ok, 1 unknown ring.
uint32_t Ring_Delete(uint32_t id)
{
	Ring_t **link = &gRingList, *r;
	RingItem_t* it;
	for(r = gRingList; r; link = &r->next, r = r->next)
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

void Ring_DeleteAll(void)
{
	while(gRingList)
		Ring_Delete(gRingList->id);
}

// The records in ring `id` into *out: 0 ok, 1 unknown ring.
uint32_t Ring_Count(uint32_t* out, uint32_t id)
{
	Ring_t* r = Ring_Find(id);
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
uint32_t Ring_Push(uint32_t id, const void* data)
{
	Ring_t* r = Ring_Find(id);
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
uint32_t Ring_Get(void* dst, uint32_t id, uint32_t idx)
{
	Ring_t* r = Ring_Find(id);
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
uint32_t Ring_Remove(uint32_t id, uint32_t idx, uint32_t count)
{
	Ring_t* r = Ring_Find(id);
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
