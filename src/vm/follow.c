/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * follow.c - the marks of memory that holds what a program made of the
 *            engine's own program file (src/include/bgi/follow.h)
 *
 * Two kinds of marked memory are kept, both by the address in the host's
 * memory and with the content that was marked:
 *
 *   cells - 32-bit variables a marked value was stored in.  A cell holds
 *           while a load there reads the value that was stored; a store
 *           of an unmarked value removes it.
 *   runs  - bytes a loader instruction wrote: part of the program file's
 *           image, or a digest.  A run holds while every byte of it is
 *           what was written; the first load that finds another drops it.
 *
 * A run is compared with its memory, so it must not outlive it: the
 * places that free or move memory a program can address call
 * Follow_Released.
 *
 * A third kind outlives the run: the variables of the global memory that
 * held a marked value, kept beside the game for the runs that load a
 * save (Follow_Keep, Follow_Start).
 */
#include "bgi/follow.h"
#include "bgi/fs.h"
#include "bgi/core.h"
#include <stdio.h>

#define KEPT_FILE "follow.txt" // the note of Follow_Keep, in the engine's folder beside the game

// ---- cells ----------------------------------------------------------------------------------

static FollowCell_t* Bucket(OpenBGI_t* o, const uint8_t* p)
{
	uint64_t a = (uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ull;
	return o->vm.follow.cells[(a >> 40) % FOLLOW_BUCKETS];
}

static FollowCell_t* CellAt(OpenBGI_t* o, const uint8_t* p)
{
	FollowCell_t* b = Bucket(o, p);
	int i;
	for(i = 0; i < FOLLOW_WAYS; i++)
		if(b[i].addr == p)
			return &b[i];
	return NULL;
}

static void CellDrop(OpenBGI_t* o, FollowCell_t* c)
{
	c->addr = NULL;
	o->vm.follow.live--;
}

// the cell of p, made when there is none; a full bucket gives up its first
static void CellSet(OpenBGI_t* o, uint8_t* p, uint32_t v)
{
	FollowCell_t* c = CellAt(o, p);
	if(!c)
	{
		FollowCell_t* b = Bucket(o, p);
		int i;
		for(i = 0; i < FOLLOW_WAYS && b[i].addr; i++)
		{
		}
		c = &b[i < FOLLOW_WAYS ? i : 0];
		if(!c->addr)
			o->vm.follow.live++;
	}
	c->addr = p;
	c->value = v;
}

// ---- runs -----------------------------------------------------------------------------------

static void RunDrop(OpenBGI_t* o, FollowRun_t* r)
{
	r->addr = NULL;
	o->vm.follow.live--;
}

// whether the run still has the bytes that were marked; dropped when not
static int RunHolds(OpenBGI_t* o, FollowRun_t* r)
{
	if(memcmp(r->addr, r->ref, r->len) == 0)
		return 1;
	RunDrop(o, r);
	return 0;
}

// the run that has [p, p + n) inside it and still holds, NULL when none
static FollowRun_t* RunOver(OpenBGI_t* o, const uint8_t* p, uint32_t n)
{
	int i;
	for(i = 0; i < FOLLOW_RUNS; i++)
	{
		FollowRun_t* r = &o->vm.follow.runs[i];
		if(r->addr && p >= r->addr && n <= r->len && (size_t)(p - r->addr) <= r->len - n && RunHolds(o, r))
			return r;
	}
	return NULL;
}

void Follow_Bytes(OpenBGI_t* o, uint8_t* p, uint32_t len, const uint8_t* ref)
{
	Follow_t* f = &o->vm.follow;
	FollowRun_t* r = NULL;
	int i;
	if(len == 0 || (!ref && len > FOLLOW_OWN))
		return;
	for(i = 0; i < FOLLOW_RUNS && !r; i++)
		if(f->runs[i].addr == p)
			r = &f->runs[i];
	for(i = 0; i < FOLLOW_RUNS && !r; i++)
		if(!f->runs[i].addr)
			r = &f->runs[i];
	if(!r)
	{
		r = &f->runs[f->nextRun];
		f->nextRun = (f->nextRun + 1) % FOLLOW_RUNS;
	}
	if(!r->addr)
		f->live++;
	r->addr = p;
	r->len = len;
	if(ref)
		r->ref = ref;
	else
	{
		memcpy(r->own, p, len);
		r->ref = r->own;
	}
}

// ---- from one run to the next -----------------------------------------------------------

// whether the four bytes at p are in the global memory, and where
static int InGlobal(OpenBGI_t* o, const uint8_t* p, uint32_t* off)
{
	if(!o->mem.globalMem || p < o->mem.globalMem || (size_t)(p - o->mem.globalMem) + 4 > o->mem.globalMemSize)
		return 0;
	*off = (uint32_t)(p - o->mem.globalMem);
	return 1;
}

void Follow_Start(OpenBGI_t* o)
{
	Follow_t* f = &o->vm.follow;
	char* text = Fs_OwnRead(o, KEPT_FILE);
	const char* s = text;
	f->live -= f->keptCount;
	f->keptCount = 0;
	while(s && *s && f->keptCount < FOLLOW_KEPT)
	{
		unsigned off, value;
		if(sscanf(s, "global %x %x", &off, &value) == 2)
		{
			f->kept[f->keptCount].off = off;
			f->kept[f->keptCount].value = value;
			f->keptCount++;
		}
		s = strchr(s, '\n');
		if(s)
			s++;
	}
	f->live += f->keptCount;
	free(text);
}

void Follow_Keep(OpenBGI_t* o)
{
	Follow_t* f = &o->vm.follow;
	char text[FOLLOW_KEPT * 32];
	size_t len = 0;
	uint32_t i, k, j;
	int changed = 0;
	for(i = 0; i < FOLLOW_BUCKETS; i++)
		for(k = 0; k < FOLLOW_WAYS; k++)
		{
			const FollowCell_t* c = &f->cells[i][k];
			uint32_t off, v;
			if(!c->addr || !InGlobal(o, c->addr, &off))
				continue;
			memcpy(&v, c->addr, 4);
			if(v != c->value)
				continue; // (written over since)
			for(j = 0; j < f->keptCount && f->kept[j].off != off; j++)
			{
			}
			if(j < f->keptCount && f->kept[j].value == v)
				continue;
			if(j == f->keptCount)
			{
				if(f->keptCount == FOLLOW_KEPT)
					continue;
				f->keptCount++;
				f->live++;
			}
			f->kept[j].off = off;
			f->kept[j].value = v;
			changed = 1;
		}
	if(!changed)
		return;
	for(j = 0; j < f->keptCount; j++)
		len += (size_t)sprintf(text + len, "global %x %x\n", (unsigned)f->kept[j].off, (unsigned)f->kept[j].value);
	Fs_OwnWrite(o, KEPT_FILE, text); // (not written: a later run marks the value where it computes it, not where it loads it)
}

// ---- what the instructions ask --------------------------------------------------------------

int Follow_Loaded(OpenBGI_t* o, const uint8_t* p, uint32_t n, uint32_t v)
{
	if(n == 4)
	{
		FollowCell_t* c = CellAt(o, p);
		uint32_t off, j;
		if(c)
		{
			if(c->value == v)
				return 1;
			CellDrop(o, c); // something else was written there since
		}
		if(o->vm.follow.keptCount && InGlobal(o, p, &off))
			for(j = 0; j < o->vm.follow.keptCount; j++)
				if(o->vm.follow.kept[j].off == off && o->vm.follow.kept[j].value == v)
					return 1; // what a save brought back
	}
	return RunOver(o, p, n) != NULL;
}

void Follow_Stored(OpenBGI_t* o, uint8_t* p, uint32_t n, uint32_t v, int marked)
{
	if(marked && n == 4)
	{
		CellSet(o, p, v);
		return;
	}
	if(o->vm.follow.live)
	{
		FollowCell_t* c = CellAt(o, p);
		if(c)
			CellDrop(o, c);
	}
}

int Follow_Has(OpenBGI_t* o, const uint8_t* p, uint32_t len)
{
	int i;
	uint32_t k;
	for(i = 0; i < FOLLOW_RUNS; i++)
	{
		FollowRun_t* r = &o->vm.follow.runs[i];
		if(r->addr && r->addr < p + len && p < r->addr + r->len && RunHolds(o, r))
			return 1;
	}
	for(k = 0; k + 4 <= len; k++)
	{
		FollowCell_t* c = CellAt(o, p + k);
		uint32_t v;
		if(!c)
			continue;
		memcpy(&v, p + k, 4);
		if(c->value == v)
			return 1;
		CellDrop(o, c);
	}
	return 0;
}

void Follow_Copied(OpenBGI_t* o, uint8_t* dst, const uint8_t* src, uint32_t len)
{
	FollowRun_t was[FOLLOW_RUNS];
	int i;
	uint32_t k;
	memcpy(was, o->vm.follow.runs, sizeof was); // (Follow_Bytes may take a run's place)
	for(i = 0; i < FOLLOW_RUNS; i++)
	{
		const FollowRun_t* r = &was[i];
		// a run copied whole: the copy is a run of the same bytes
		if(r->addr && r->addr >= src && r->len <= len && (size_t)(r->addr - src) <= len - r->len && memcmp(r->addr, r->ref, r->len) == 0)
			Follow_Bytes(o, dst + (r->addr - src), r->len, r->ref == r->own ? NULL : r->ref);
	}
	for(k = 0; k + 4 <= len; k++)
	{
		FollowCell_t* c = CellAt(o, src + k);
		uint32_t v;
		if(!c)
			continue;
		memcpy(&v, src + k, 4);
		if(c->value == v)
			CellSet(o, dst + k, v);
	}
}

void Follow_Released(OpenBGI_t* o, const uint8_t* base, size_t size)
{
	Follow_t* f = &o->vm.follow;
	int i, k;
	for(i = 0; i < FOLLOW_RUNS; i++)
	{
		FollowRun_t* r = &f->runs[i];
		if(r->addr && (!base || (r->addr + r->len > base && r->addr < base + size)))
			RunDrop(o, r);
	}
	for(i = 0; i < FOLLOW_BUCKETS; i++)
		for(k = 0; k < FOLLOW_WAYS; k++)
		{
			FollowCell_t* c = &f->cells[i][k];
			if(c->addr && (!base || (c->addr >= base && c->addr < base + size)))
				CellDrop(o, c);
		}
}
