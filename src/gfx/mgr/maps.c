/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * maps.c - the graphics manager's tile map operations: the "90 7x"
 *          instructions (src/include/bgi/gfx/gfxmgr.h)
 *
 * A map shows a window onto a terrain of tiles cut from a chipset bitmap.
 * Eight slots, addressed by H_MAP | index.
 */
#include "mgr_internal.h"

void Gfx_DeleteAllMaps(OpenBGI_t* o, Gfx_t* g)
{
	Mgr_DeleteAll(o, g->maps, GFX_MAPS, &g->mapCount, &g->mapNextId);
}

// "90 70": a new map in the first free slot; the handle, 0 when all 8 are in use
uint32_t Gfx_MapCreate(OpenBGI_t* o, Gfx_t* g)
{
	Map_t* m;
	uint32_t i;

	if(g->mapCount >= GFX_MAPS)
		return 0;
	i = Mgr_FreeSlot(g->maps);
	m = (Map_t*)BGI_Calloc(sizeof(Map_t));
	Map_Ctor(o, m, g->mapNextId++);
	g->maps[i] = &m->obj;
	Compositor_Add(o, g->comp, &m->obj);
	g->mapCount++;
	return H_MAP | i;
}

// "90 71": 1 ok, 0 unknown handle
int Gfx_MapDelete(OpenBGI_t* o, Gfx_t* g, uint32_t h)
{
	return Mgr_DeleteFromTable(o, g, g->maps, Gfx_FindMap(g, h), h, &g->mapCount, 0);
}

// "90 74": show / hide; 1 ok, 0 unknown handle
int Gfx_MapShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on)
{
	return Mgr_ShowBracket(o, Gfx_FindMap(g, h), on);
}

/* "90 75": the chipset bitmap, then position, effect, level and priority.
 * 0 ok, 1 the chipset bitmap is unusable, 0xFF unknown handle.  The
 * wrapper expects Map_Set to return non-zero on success and 0 for an
 * unusable chipset; Map_Set as written returns 0 in both cases, so the
 * wrapper reports 1 whatever happened. */
int Gfx_MapSet(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int bmp, int effect, int level, int prio)
{
	DispObj_t* obj = Gfx_FindMap(g, h);

	if(!obj)
		return NO_OBJECT;
	InvalidateIfVisible(o, obj);
	if(!Map_Set(o, (Map_t*)obj, x, y, bmp, effect, level, prio))
		return 1;
	InvalidateIfVisible(o, obj);
	Compositor_Resort(o, g->comp, obj);
	return 0;
}

/* "90 76": the view size in tiles and the tile size in pixels.  0 ok, 2
 * the view is too large (Map_SetSize 0x80000002), 3 the tile size is bad
 * (0x80000003), 0xFF unknown handle. */
int Gfx_MapSetSize(OpenBGI_t* o, Gfx_t* g, uint32_t h, int cols, int rows, int chipW, int chipH)
{
	DispObj_t* obj = Gfx_FindMap(g, h);
	uint32_t r;
	int was;

	if(!obj)
		return NO_OBJECT;
	was = IsVisible(o, obj);
	if(was)
		Invalidate(o, obj);
	r = (uint32_t)Map_SetSize(o, (Map_t*)obj, cols, rows, chipW, chipH);
	if(r == 0)
	{
		if(was)
			Invalidate(o, obj);
		return 0;
	}
	if(r == 0x80000002u)
		return 2;
	if(r == 0x80000003u)
		return 3;
	return (int)h;
}

// "90 78": the terrain, w x hgt tile numbers; 0 ok, 4 a dimension is 0, 0xFF unknown handle
int Gfx_MapSetTerrain(Gfx_t* g, uint32_t h, int w, int hgt, const uint16_t* data)
{
	DispObj_t* obj = Gfx_FindMap(g, h);

	if(!obj)
		return NO_OBJECT;
	return Map_SetTerrain((Map_t*)obj, w, hgt, data) ? 0 : 4;
}

/* "90 79": the view position - the terrain cell (col, row) at the top
 * left, the pixel offset (offX, offY) inside it, `wrap` for a torus.  0 ok,
 * 5 the map has no size or terrain yet or a value is out of range, 0xFF
 * unknown handle. */
int Gfx_MapSetView(OpenBGI_t* o, Gfx_t* g, uint32_t h, int col, int row, int offX, int offY, int wrap)
{
	DispObj_t* obj = Gfx_FindMap(g, h);

	if(!obj)
		return NO_OBJECT;
	if(!Map_SetView((Map_t*)obj, col, row, offX, offY, wrap))
		return 5;
	InvalidateIfVisible(o, obj);
	return 0;
}

// "90 7A": every cell showing `tile` is redrawn from the chipset; 0 ok, 6 the map has no size yet, 0xFF unknown handle
int Gfx_MapInvalidateChip(Gfx_t* g, uint32_t h, int tile)
{
	DispObj_t* obj = Gfx_FindMap(g, h);

	if(!obj)
		return NO_OBJECT;
	return Map_InvalidateChip((Map_t*)obj, tile) ? 0 : 6;
}
