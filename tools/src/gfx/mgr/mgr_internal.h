/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * mgr_internal.h - what the graphics manager's files share (src/gfx/mgr/)
 *
 * The public interface is inc/bgi/gfx/gfxmgr.h.  gfxmgr.c holds the
 * manager itself (its life cycle, the back buffer, the handles), frame.c
 * the frame cycle, background.c the background operations; the per-class
 * files (sprites.c, filters.c, maps.c, windows.c, screens.c, knobs.c,
 * groups.c) hold the wrappers the instructions call for one class of
 * display object each, generic.c those that take any handle, and
 * helpers.c the brackets below that the wrappers share.
 *
 * All wrappers follow the same shape: resolve the handle (NO_OBJECT when
 * it does not resolve); if the object is visible, invalidate it so that
 * the compositor repaints what it covered; apply the change through the
 * object's own method; if the object is visible now, invalidate it again
 * (its new footprint) and re-sort it when the change may have moved it in
 * the draw order; map the object's 0x8000000n result codes onto small
 * numbers.  The exact bracket differs per wrapper and each one keeps its
 * own order.
 */
#ifndef BGI_GFX_MGR_INTERNAL_H_
#define BGI_GFX_MGR_INTERNAL_H_

#include <string.h>

#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/sprite.h"
#include "bgi/gfx/objects.h"
#include "bgi/gfx/map.h"
#include "bgi/gfx/window.h"
#include "bgi/gfx/screens.h"

#define NO_OBJECT 0xff // the result of every wrapper for an unknown handle

// the object's effective visibility (shown, enabled, not faded out, not fully transparent)
static inline int IsVisible(DispObj_t* o)
{
	return o->vt->isVisible(o);
}

// hand the object's footprint to the compositor as dirty
static inline void Invalidate(DispObj_t* o)
{
	o->vt->invalidate(o);
}

static inline void InvalidateIfVisible(DispObj_t* o)
{
	if(IsVisible(o))
		Invalidate(o);
}

/* the show / hide bracket shared by every *Show wrapper: invalidate only
 * when the effective visibility changed - the footprint is the same
 * before and after, so one call is enough.  1 ok, 0 for no object. */
int Mgr_ShowBracket(DispObj_t* o, int on);
// the first free slot of a table (the caller has checked that the table is not full)
uint32_t Mgr_FreeSlot(DispObj_t** table);
// destroy every object of a table and reset its counters
void Mgr_DeleteAll(DispObj_t** table, int n, int32_t* count, int32_t* nextId);
/* the common delete: repaint what the object covered (the whole screen
 * with invalidateAll), take it out of the compositor, destroy it, free the
 * slot.  1 ok, 0 for no object. */
int Mgr_DeleteFromTable(Gfx_t* g, DispObj_t** table, DispObj_t* o, uint32_t h, int32_t* count, int invalidateAll);

#endif // BGI_GFX_MGR_INTERNAL_H_
