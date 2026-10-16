/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * targets.c - sprites registered as click targets (inc/bgi/sysobj/targets.h)
 *
 * A singly linked list, most recently registered first; the sprites stay
 * owned by the graphics manager.
 */
#include "bgi/sysobj/targets.h"
#include "bgi/input.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/dispobj.h"

typedef struct Target // one registered sprite
{
	uint32_t index;      // the running number at registration; never reused, the count only grows
	uint32_t handle;     // the sprite's handle
	DispObj_t* sprite;   // the sprite itself (not owned)
	uint32_t hit;        // the button state sampled by Target_UpdateAll
	struct Target* next; // the next older target
} Target_t;

static Target_t* gTargetHead; // every target, newest first
static uint32_t gTargetCount; // registrations since the last clear (the next index)

// Unregister every target and restart the indices.
void Target_Clear(void)
{
	while(gTargetHead)
		Target_Remove(gTargetHead->handle);
	gTargetCount = 0;
}

// Register a sprite and push its mouse layer: 1 ok, 0 when `id` is not a sprite handle.
int Target_Add(uint32_t id)
{
	DispObj_t* s = Gfx_FindSprite(gGfx, id);
	Target_t* t;
	if(!s)
		return 0;
	MouseLayer_PushSprite(s);
	t = (Target_t*)BGI_Alloc(sizeof *t);
	t->index = gTargetCount;
	t->handle = id;
	t->sprite = s;
	t->hit = 0;
	t->next = gTargetHead;
	gTargetCount++;
	gTargetHead = t;
	return 1;
}

// Unregister a sprite and pop its mouse layer: 1 ok, 0 when it was not registered.
int Target_Remove(uint32_t id)
{
	Target_t** pp;
	for(pp = &gTargetHead; *pp; pp = &(*pp)->next)
	{
		Target_t* t = *pp;
		if(t->handle == id)
		{
			MouseLayer_PopSprite(t->sprite);
			*pp = t->next;
			BGI_Free(t);
			return 1;
		}
	}
	return 0;
}

// The index of the first target (most recently registered first) whose screen rectangle holds the mouse, 0xffffffff for none.
uint32_t Target_Hit(void)
{
	int pos[2];
	Target_t* t;
	GetMouseClientPos(pos);
	for(t = gTargetHead; t; t = t->next)
	{
		Rect_t r;
		t->sprite->vt->screenRect(t->sprite, &r);
		if(pos[0] >= r.l && pos[0] <= r.r && pos[1] >= r.t && pos[1] <= r.b)
			return t->index;
	}
	return 0xffffffffu;
}

// The last sampled press state of target `idx` into *out: 1 ok, 0 for an unknown index.
int Target_Get(uint32_t* out, uint32_t idx)
{
	Target_t* t;
	for(t = gTargetHead; t; t = t->next)
	{
		if(t->index == idx)
		{
			*out = t->hit;
			return 1;
		}
	}
	return 0;
}

// Sample the left button over each target's layer (bit 0 of the poll result).
void Target_UpdateAll(void)
{
	Target_t* t;
	for(t = gTargetHead; t; t = t->next)
		t->hit = Input_Poll(0, t->sprite->vt->sortKey(t->sprite)) & 1;
}
