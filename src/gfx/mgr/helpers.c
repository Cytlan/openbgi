/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * helpers.c - the brackets the per-class wrappers of the graphics manager
 *             share (mgr_internal.h): show / hide, slot allocation, the
 *             deletion of one object and of a whole table
 */
#include "mgr_internal.h"

// show or hide; one repaint when the effective visibility changed.  1 ok, 0 for no object
int Mgr_ShowBracket(OpenBGI_t* o, DispObj_t* obj, int on)
{
	int was, now;

	if(!obj)
		return 0;
	was = IsVisible(o, obj);
	obj->vt->setVisible(o, obj, on);
	now = IsVisible(o, obj);
	if(was != now)
		Invalidate(o, obj);
	return 1;
}

// the index of the first NULL entry; the caller guarantees there is one
uint32_t Mgr_FreeSlot(DispObj_t** table)
{
	uint32_t i = 0;
	while(table[i])
		i++;
	return i;
}

// destroy (and free) every object of the n slots; the count and the running id restart at 0
void Mgr_DeleteAll(OpenBGI_t* o, DispObj_t** table, int n, int32_t* count, int32_t* nextId)
{
	int i;
	for(i = 0; i < n; i++)
	{
		if(table[i])
		{
			table[i]->vt->destroy(o, table[i], 1);
			table[i] = NULL;
		}
	}
	*count = 0;
	*nextId = 0;
}

/* delete the object `o` found under handle `h`: repaint what it covered
 * (the whole screen with `invalidateAll`, for objects that affect
 * everything below them), remove it from the compositor, destroy and free
 * it, clear the slot.  1 ok, 0 when `o` is NULL. */
int Mgr_DeleteFromTable(OpenBGI_t* o, Gfx_t* g, DispObj_t** table, DispObj_t* obj, uint32_t h, int32_t* count, int invalidateAll)
{
	uint32_t idx;

	if(!obj)
		return 0;
	if(IsVisible(o, obj))
	{
		if(invalidateAll)
			Compositor_InvalidateAll(g->comp);
		else
			Invalidate(o, obj);
	}
	Compositor_Remove(g->comp, obj);
	idx = HANDLE_INDEX(h);
	if(table[idx])
		table[idx]->vt->destroy(o, table[idx], 1);
	table[idx] = NULL;
	(*count)--;
	return 1;
}
