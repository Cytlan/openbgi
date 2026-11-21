/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * targets.c - sprites registered as click targets (src/include/bgi/sysobj/targets.h)
 *
 * A singly linked list, most recently registered first; the sprites stay
 * owned by the graphics manager.
 */
#include "bgi/sysobj/targets.h"
#include "bgi/input.h"
#include "bgi/inputmap.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/dispobj.h"
#include "bgi/core.h"

typedef struct Target // one registered sprite
{
	uint32_t index;      // the running number at registration; never reused, the count only grows
	uint32_t handle;     // the sprite's handle
	DispObj_t* sprite;   // the sprite itself (not owned)
	uint32_t hit;        // the button state sampled by Target_UpdateAll
	struct Target* next; // the next older target
} Target_t;

// Unregister every target and restart the indices.
void Target_Clear(OpenBGI_t* o)
{
	while(o->sysobj.targetHead)
		Target_Remove(o, o->sysobj.targetHead->handle);
	o->sysobj.targetCount = 0;
}

// Register a sprite and push its mouse layer: 1 ok, 0 when `id` is not a sprite handle.
int Target_Add(OpenBGI_t* o, uint32_t id)
{
	DispObj_t* s = Gfx_FindSprite(o->gfx.gfx, id);
	Target_t* t;
	if(!s)
		return 0;
	MouseLayer_PushSprite(o, s);
	t = (Target_t*)BGI_Alloc(sizeof *t);
	t->index = o->sysobj.targetCount;
	t->handle = id;
	t->sprite = s;
	t->hit = 0;
	t->next = o->sysobj.targetHead;
	o->sysobj.targetCount++;
	o->sysobj.targetHead = t;
	return 1;
}

// Unregister a sprite and pop its mouse layer: 1 ok, 0 when it was not registered.
int Target_Remove(OpenBGI_t* o, uint32_t id)
{
	Target_t** pp;
	for(pp = &o->sysobj.targetHead; *pp; pp = &(*pp)->next)
	{
		Target_t* t = *pp;
		if(t->handle == id)
		{
			MouseLayer_PopSprite(o, t->sprite);
			*pp = t->next;
			BGI_Free(t);
			return 1;
		}
	}
	return 0;
}

// The index of the first target (most recently registered first) whose screen rectangle holds the mouse, 0xffffffff for none.
uint32_t Target_Hit(OpenBGI_t* o)
{
	int pos[2];
	Target_t* t;
	GetMouseClientPos(o, pos);
	for(t = o->sysobj.targetHead; t; t = t->next)
	{
		Rect_t r;
		t->sprite->vt->screenRect(o, t->sprite, &r);
		if(pos[0] >= r.l && pos[0] <= r.r && pos[1] >= r.t && pos[1] <= r.b)
			return t->index;
	}
	return 0xffffffffu;
}

// The last sampled press state of target `idx` into *out: 1 ok, 0 for an unknown index.
int Target_Get(OpenBGI_t* o, uint32_t* out, uint32_t idx)
{
	Target_t* t;
	for(t = o->sysobj.targetHead; t; t = t->next)
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
void Target_UpdateAll(OpenBGI_t* o)
{
	Target_t* t;
	for(t = o->sysobj.targetHead; t; t = t->next)
		t->hit = Input_Poll(o, 0, t->sprite->vt->sortKey(o, t->sprite)) & 1;
}

// a click at (x, y) reaches sprite target d->ctx: its layer is hit there, as the sampling polls it
static int ReachSprite(OpenBGI_t* o, const TargetDesc_t* d, int x, int y)
{
	const Target_t* t = (const Target_t*)d->ctx;
	return MouseLayer_HitAt(o, t->sprite->vt->sortKey(o, t->sprite), x, y);
}

/* The registered sprites that are shown, as clickable targets
 * (bgi/inputmap.h): the left button pressed over one is sampled each pass
 * for "90 FD". */
void Target_Targets(OpenBGI_t* o, TargetSink_t* s)
{
	const Target_t* t;
	for(t = o->sysobj.targetHead; t; t = t->next)
	{
		TargetDesc_t d;
		if(!t->sprite->vt->isVisible(o, t->sprite))
			continue;
		TargetDesc_Init(&d, OPENBGI_TARGET_SPRITE, t->sprite->vt->sortKey(o, t->sprite));
		t->sprite->vt->screenRect(o, t->sprite, &d.rect);
		d.owner = t->handle;
		d.index = (int)t->index;
		d.reach = ReachSprite;
		d.ctx = t;
		TargetSink_Add(s, &d);
	}
}
