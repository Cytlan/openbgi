/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dynmem.c - the global memory, the dynamic global blocks and the
 *            resolution of tagged script pointers (src/include/bgi/sys.h)
 *
 * Scripts address memory through 32-bit tagged values (src/include/bgi/version.h:
 * tag << VM_TAG_SHIFT | offset).  Tag 0 is the global memory block that
 * "80 70" sizes and the save files hold; the running thread's code, data
 * and heap areas have their own tags; the tags from
 * o->engine.profile->dynFirst on are the dynamic blocks a script
 * allocates with "80 20" and frees with "80 21".  ResolvePtr turns such a
 * value into a host pointer and raises the script error for anything
 * unmapped.
 */
#include "bgi/sys.h"
#include "bgi/vm.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/core.h"

/* 1.535 on: the dynamic blocks come from five size-class pools instead of
 * one tag per block.  A pool has `count` slots of at most `maxSize` bytes;
 * a pointer into its slot s is
 * (firstTag + (s >> (26 - bits))) << 26 | (s & ((1 << (26 - bits)) - 1))
 * << bits | offset, so the pools take the tags 0x10 .. 0x3F between them
 * and a 4 KB block costs a 14-bit slot number plus 12 offset bits rather
 * than a tag of its own.  GAlloc takes the first pool whose maximum fits
 * the size and that still has a free slot (a full pool falls through to
 * the next, larger one). */

#define DYN_POOLED(o) (o->engine.profile->dynCount == 0) // the profile uses the pools

// the pool a tag belongs to, with the slot of (tag, offset); NULL when none
static DynPool_t* DynPool_Of(OpenBGI_t* o, uint32_t tag, uint32_t off, uint32_t* outSlot)
{
	size_t i;
	for(i = 0; i < BGI_COUNTOF(o->mem.dynPools); i++)
	{
		DynPool_t* p = &o->mem.dynPools[i];
		uint32_t tags = p->count >> (26 - p->bits); // the tags the pool spans
		if(tag >= p->firstTag && tag < p->firstTag + tags)
		{
			*outSlot = ((tag - p->firstTag) << (26 - p->bits)) | (off >> p->bits);
			return p;
		}
	}
	return NULL;
}

/* "80 70": the global memory becomes 0x1000 << shift bytes, shift in
 * 0 .. 12; the old block is freed (its content is lost) and the new one
 * zeroed.  Returns 1 on success, 0 for a shift out of range. */
int SetGlobalMemSize(OpenBGI_t* o, int shift)
{
	if(shift < 0 || shift > 12)
		return 0;
	if(o->vm.follow.live && o->mem.globalMem)
		Follow_Released(o, o->mem.globalMem, o->mem.globalMemSize);
	o->mem.globalMemSize = 0x1000u << shift;
	BGI_Free(o->mem.globalMem);
	o->mem.globalMem = (uint8_t*)BGI_Calloc(o->mem.globalMemSize);
	return 1;
}

// "80 71": zero the global memory
void ClearGlobalMem(OpenBGI_t* o)
{
	memset(o->mem.globalMem, 0, o->mem.globalMemSize);
}

static void DynPools_Ensure(OpenBGI_t* o) // the slot tables, made on first use (the profile is selected after DynMem_Init)
{
	size_t i;
	for(i = 0; i < BGI_COUNTOF(o->mem.dynPools); i++)
		if(!o->mem.dynPools[i].blocks)
		{
			o->mem.dynPools[i].blocks = (uint8_t**)BGI_Calloc(o->mem.dynPools[i].count * sizeof(uint8_t*));
			o->mem.dynPools[i].sizes = (uint32_t*)BGI_Calloc(o->mem.dynPools[i].count * sizeof(uint32_t));
		}
}

void DynMem_Init(OpenBGI_t* o)
{
	memset(o->mem.dynBlocks, 0, sizeof(o->mem.dynBlocks));
	o->mem.dynReady = 1;
}

// free every dynamic block (a reboot and the shutdown); the slot tables stay
void DynMem_FreeAll(OpenBGI_t* o)
{
	size_t i, j;
	if(!o->mem.dynReady)
		return;
	for(i = 0; i < o->engine.profile->dynCount; i++)
	{
		if(o->vm.follow.live && o->mem.dynBlocks[i])
			Follow_Released(o, o->mem.dynBlocks[i], o->mem.dynSizes[i]);
		BGI_Free(o->mem.dynBlocks[i]);
		o->mem.dynBlocks[i] = NULL;
	}
	for(i = 0; i < BGI_COUNTOF(o->mem.dynPools); i++)
		if(o->mem.dynPools[i].blocks)
		{
			for(j = 0; j < o->mem.dynPools[i].count; j++)
			{
				if(o->vm.follow.live && o->mem.dynPools[i].blocks[j])
					Follow_Released(o, o->mem.dynPools[i].blocks[j], o->mem.dynPools[i].sizes[j]);
				BGI_Free(o->mem.dynPools[i].blocks[j]);
			}
			BGI_Free(o->mem.dynPools[i].blocks); // the slot tables themselves: made again on the next use (DynPools_Ensure)
			BGI_Free(o->mem.dynPools[i].sizes);
			o->mem.dynPools[i].blocks = NULL;
			o->mem.dynPools[i].sizes = NULL;
		}
}

/* "80 20": allocate a dynamic block of `size` bytes and return its tagged
 * pointer, 0 when every slot is taken.  The block is zeroed: the original
 * takes it from the process heap as it comes, and the scripts of the later
 * games pass freshly allocated one-byte blocks as empty strings. */
uint32_t GAlloc(OpenBGI_t* o, uint32_t size)
{
	size_t i, j;
	if(DYN_POOLED(o))
	{
		DynPools_Ensure(o);
		for(i = 0; i < BGI_COUNTOF(o->mem.dynPools); i++)
		{
			DynPool_t* p = &o->mem.dynPools[i];
			if(size > p->maxSize)
				continue;
			for(j = 0; j < p->count; j++)
				if(!p->blocks[j])
				{
					uint32_t slot = (uint32_t)j;
					p->blocks[j] = (uint8_t*)BGI_Calloc(size ? size : 1);
					p->sizes[j] = size ? size : 1;
					return ((p->firstTag + (slot >> (26 - p->bits))) << 26) |
						((slot & ((1u << (26 - p->bits)) - 1u)) << p->bits);
				}
		}
		return 0;
	}
	for(i = 0; i < o->engine.profile->dynCount; i++)
		if(!o->mem.dynBlocks[i])
		{
			o->mem.dynBlocks[i] = (uint8_t*)BGI_Calloc(size ? size : 1);
			o->mem.dynSizes[i] = size ? size : 1;
			return ((uint32_t)i + o->engine.profile->dynFirst) << VM_TAG_SHIFT(o);
		}
	return 0;
}

/* "80 21": free the dynamic block `ptr` points at.  1 on success, 0 when
 * ptr is not the start of an allocated block (the pooled layout of 1.535
 * on included). */
int GFree(OpenBGI_t* o, uint32_t ptr)
{
	uint32_t slot;
	if(DYN_POOLED(o))
	{
		DynPool_t* p = DynPool_Of(o, ptr >> 26, ptr & 0x3ffffffu, &slot);
		DynPools_Ensure(o);
		if(!p || (ptr & ((1u << p->bits) - 1u)) || !p->blocks[slot]) // must point at the block start
			return 0;
		if(o->vm.follow.live)
			Follow_Released(o, p->blocks[slot], p->sizes[slot]);
		BGI_Free(p->blocks[slot]);
		p->blocks[slot] = NULL;
		return 1;
	}
	slot = (ptr >> VM_TAG_SHIFT(o)) - o->engine.profile->dynFirst;
	if(slot >= o->engine.profile->dynCount)
		return 0;
	if(ptr & VM_OFFSET_MASK(o)) // must point at the block start
		return 0;
	if(!o->mem.dynBlocks[slot])
		return 0;
	if(o->vm.follow.live)
		Follow_Released(o, o->mem.dynBlocks[slot], o->mem.dynSizes[slot]);
	BGI_Free(o->mem.dynBlocks[slot]);
	o->mem.dynBlocks[slot] = NULL;
	return 1;
}

// the block of dynamic slot `slot` (tag - dynFirst; not for the pooled layout); NULL when free or out of range
uint8_t* DynMemBase(OpenBGI_t* o, uint32_t slot)
{
	return slot < o->engine.profile->dynCount ? o->mem.dynBlocks[slot] : NULL;
}

/* The host address of the tagged pointer `ptr` for thread t: NULL for 0,
 * else the global memory, the thread's code, data or heap area, or a
 * dynamic block.  A tag below the dynamic range that is none of those, or
 * a dynamic slot that is not allocated, is a script error (the message
 * names the pointer); no range check is made within an area. */
void* ResolvePtr(OpenBGI_t* o, uint32_t ptr, Thread_t* t)
{
	uint32_t tag = ptr >> VM_TAG_SHIFT(o);
	uint32_t off = ptr & VM_OFFSET_MASK(o);
	char msg[0x100];
	uint8_t* base;

	if(ptr == 0)
		return NULL;
	if(tag == 0)
		return o->mem.globalMem + off;
	if(tag == o->engine.profile->tagCode)
		return t->code + off;
	if(tag == o->engine.profile->tagData)
		return t->data + off;
	if(tag == o->engine.profile->tagHeap)
		return Thread_HeapPtr(t, off);
	if(tag < o->engine.profile->dynFirst)
	{
		sprintf(msg, MSG_BAD_POINTER_REGION, (unsigned)ptr);
		ScriptError(o, msg, t);
	}
	if(DYN_POOLED(o))
	{
		uint32_t slot;
		DynPool_t* p = DynPool_Of(o, tag, off, &slot);
		DynPools_Ensure(o);
		base = p ? p->blocks[slot] : NULL;
		if(!base)
		{
			sprintf(msg, MSG_BAD_POINTER, (unsigned)ptr);
			ScriptError(o, msg, t);
		}
		return base + (off & ((1u << p->bits) - 1u)); // the offset within the slot
	}
	base = DynMemBase(o, tag - o->engine.profile->dynFirst);
	if(!base)
	{
		sprintf(msg, MSG_BAD_POINTER, (unsigned)ptr);
		ScriptError(o, msg, t);
	}
	return base + off;
}

// pop a tagged pointer from the thread's stack and resolve it
void* PopPtr(OpenBGI_t* o, Thread_t* t)
{
	return ResolvePtr(o, Thread_Pop(t), t);
}

// resolve the n tagged pointers of in[] into out[] (the string lists of "80 38", "80 F2", "80 F4", "80 F5")
void ResolvePtrArray(OpenBGI_t* o, void** out, Thread_t* t, const uint32_t* in, int n)
{
	int i;
	for(i = 0; i < n; i++)
		out[i] = ResolvePtr(o, in[i], t);
}
