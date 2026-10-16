/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * map.c - Map, the scrolling tile map display object (class order 1).
 * Interface in inc/bgi/gfx/map.h.
 *
 * A map draws once four things have been set, in any order after the first:
 * the view and tile size ("90 76", Map_SetSize), the chipset bitmap
 * ("90 75", Map_Set), the terrain array ("90 78", Map_SetTerrain) and the
 * view position ("90 79", Map_SetView).  The cache bitmap is one tile
 * larger than the view in both directions; `wanted[]` holds the tile
 * number each cache cell should show and `drawn[]` the one it does show,
 * so a scroll re-blits only the cells that changed (Map_Refresh, run from
 * the draw method), and a pure pixel scroll inside a tile copies from the
 * cache without touching any tile.
 */
#include <stddef.h>
#include <string.h>

#include "bgi/common.h"
#include "bgi/gfx/map.h"

// DispObj(1, slotId) with nothing set up yet
void Map_Ctor(Map_t* m, int slotId)
{
	DispObj_Ctor(&m->obj, 1, slotId);
	m->obj.vt = &Map_Vtbl;
	m->sized = 0;
	m->hasTerrain = 0;
	m->viewValid = 0;
	m->hasChipset = 0;
}

void Map_Dtor(Map_t* m)
{
	m->obj.vt = &Map_Vtbl;
	Map_FreeChips(m);
	Map_FreeCache(m);
	Map_FreeTerrain(m);
	DispObj_Dtor(&m->obj);
}

static void Map_Destroy(DispObj_t* o, int flags)
{
	Map_Dtor((Map_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

// refresh stale cells, then copy the dirty piece of the visible window of the cache; nothing before the view and chipset are set
static void Map_Draw(DispObj_t* o, Bmp_t* dst, const Rect_t* local, uint32_t minKey)
{
	Map_t* m = (Map_t*)o;
	Bmp_t view;
	(void)minKey;
	if(!m->viewValid || !m->hasChipset)
		return;
	if(m->dirty)
		Map_Refresh(m);
	view = m->cache;
	Bmp_Crop(&view, &m->src);
	Bmp_Crop(&view, local);
	Bmp_BlitEffect(dst, &view, DispObj_GetEffect(o), DispObj_EffectiveLevel(o));
}

/* "90 75": the chipset, then position, effect, level and priority (only
 * when the chipset was accepted).  Returns 0 in both cases: the manager
 * wrapper expects a non-zero result for success and so cannot tell a
 * refused chipset (Map_SetChipset returned 0) from an accepted one. */
int Map_Set(Map_t* m, int x, int y, int bmp, int effect, int level, int prio)
{
	int r = Map_SetChipset(m, bmp);
	if(r == 0)
		return 0;
	m->obj.vt->setPos(&m->obj, x, y);
	DispObj_SetEffect(&m->obj, effect);
	m->obj.vt->setLevel(&m->obj, level);
	DispObj_SetPriority(&m->obj, (uint32_t)prio);
	return 0;
}

/* "90 76": the view in tiles and the tile size in pixels.  The view is at
 * most 0x400 x 0x300 pixels, a tile at most 0x100 x 0x100; the object
 * takes the view's pixel size.  The old cache, chip table and view are
 * dropped; the new cache starts cleared with every cell stale.  0 ok,
 * 0x80000002 bad view, 0x80000003 bad tile size. */
int Map_SetSize(Map_t* m, int cols, int rows, int chipW, int chipH)
{
	size_t cells, i;
	if((uint32_t)chipW == 0 || (uint32_t)chipW > 0x100 || (uint32_t)chipH == 0 || (uint32_t)chipH > 0x100)
		return (int)0x80000003;
	if((uint32_t)cols == 0 || (uint32_t)cols > 0x400u / (uint32_t)chipW ||
		(uint32_t)rows == 0 || (uint32_t)rows > 0x300u / (uint32_t)chipH ||
		!m->obj.vt->setSize(&m->obj, cols * chipW, rows * chipH))
		return (int)0x80000002;
	Map_FreeCache(m);
	Map_FreeChips(m);
	Map_ResetView(m);
	m->cacheRows = rows + 1;
	m->cacheCols = cols + 1;
	m->viewRows = rows;
	m->viewCols = cols;
	m->chipW = chipW;
	m->chipH = chipH;
	Bmp_AllocScreen(&m->cache, m->cacheCols * chipW, m->cacheRows * chipH, 1);
	Bmp_Clear(&m->cache, NULL);
	cells = (size_t)m->cacheCols * (size_t)m->cacheRows;
	m->drawn = (uint16_t*)BGI_Alloc(cells * 2);
	m->wanted = (uint16_t*)BGI_Alloc(cells * 2);
	for(i = 0; i < cells; i++)
	{
		m->drawn[i] = 0xffff;
		m->wanted[i] = 0;
	}
	m->sized = 1;
	return 0;
}

/* "90 78": the terrain, w x h tile numbers in row-major order (0xFFFF =
 * empty); the array is copied, the view is forgotten and every cell made
 * stale.  1 ok, 0 when a dimension is 0. */
int Map_SetTerrain(Map_t* m, int w, int h, const uint16_t* data)
{
	if((uint32_t)w == 0 || (uint32_t)h == 0)
		return 0;
	Map_FreeTerrain(m);
	Map_ResetView(m);
	Map_InvalidateAllCells(m);
	m->terrH = h;
	m->hasTerrain = 1;
	m->terrW = w;
	m->terrain = (uint16_t*)BGI_Alloc((size_t)w * (size_t)h * 2);
	memcpy(m->terrain, data, (size_t)w * (size_t)h * 2);
	return 1;
}

/* "90 79": the view position.  (col, row) is the terrain cell at the
 * cache's top-left, (offX, offY) the pixel offset of the view inside it
 * (less than a tile).  With `wrap` the terrain restarts at index 0 past
 * its edges (a torus), otherwise the cells beyond it stay empty.  The
 * cache content only changes when col or row change.  1 ok, 0 before the
 * size and terrain are set or when a value is out of range. */
int Map_SetView(Map_t* m, int col, int row, int offX, int offY, int wrap)
{
	if(!m->sized || !m->hasTerrain)
		return 0;
	if((uint32_t)col >= (uint32_t)m->terrW || (uint32_t)row >= (uint32_t)m->terrH ||
		(uint32_t)offX >= (uint32_t)m->chipW || (uint32_t)offY >= (uint32_t)m->chipH)
		return 0;
	if(col != m->viewCol || row != m->viewRow)
	{
		uint32_t ty = (uint32_t)row;
		int cy, cx;
		m->viewCol = col;
		m->viewRow = row;
		if(!wrap)
		{
			// the cells past the terrain's edge stay empty
			size_t cells = (size_t)m->cacheCols * (size_t)m->cacheRows, i;
			for(i = 0; i < cells; i++)
				m->wanted[i] = 0xffff;
		}
		for(cy = 0; (uint32_t)cy < (uint32_t)m->cacheRows; cy++, ty++)
		{
			uint32_t tx = (uint32_t)col;
			if(ty >= (uint32_t)m->terrH)
			{
				if(!wrap)
					break;
				ty = 0;
			}
			for(cx = 0; (uint32_t)cx < (uint32_t)m->cacheCols; cx++, tx++)
			{
				if(tx >= (uint32_t)m->terrW)
				{
					if(!wrap)
						break;
					tx = 0;
				}
				m->wanted[cy * m->cacheCols + cx] = m->terrain[ty * (uint32_t)m->terrW + tx];
			}
		}
		m->dirty = 1;
	}
	m->offY = offY;
	m->src.t = offY;
	m->offX = offX;
	m->src.l = offX;
	m->src.r = m->viewCols * m->chipW + offX - 1;
	m->src.b = m->viewRows * m->chipH + offY - 1;
	m->viewValid = 1;
	return 1;
}

/* "90 7A": every cache cell showing `tile` is made stale (the script
 * repainted that tile inside the chipset bitmap, e.g. animated water).
 * 1 ok, 0 before the size is set. */
int Map_InvalidateChip(Map_t* m, int tile)
{
	int cy, cx;
	if(!m->sized)
		return 0;
	for(cy = 0; (uint32_t)cy < (uint32_t)m->cacheRows; cy++)
	{
		for(cx = 0; (uint32_t)cx < (uint32_t)m->cacheCols; cx++)
		{
			int i = cy * m->cacheCols + cx;
			if((int)m->drawn[i] == tile)
				m->drawn[i] = (uint16_t)~m->wanted[i]; // anything but `wanted`
		}
	}
	m->dirty = 1;
	return 1;
}

// every cell stale (the dirty flag is left to the caller); 0 before the size is set
int Map_InvalidateAllCells(Map_t* m)
{
	int cy, cx;
	if(!m->sized)
		return 0;
	for(cy = 0; (uint32_t)cy < (uint32_t)m->cacheRows; cy++)
		for(cx = 0; (uint32_t)cx < (uint32_t)m->cacheCols; cx++)
			m->drawn[cy * m->cacheCols + cx] = (uint16_t)~m->wanted[cy * m->cacheCols + cx];
	return 1;
}

/* Re-blit the cells whose tile changed.  An empty (0xFFFF) or
 * out-of-range tile clears the cell (effect 0x41 with tile 0 giving the
 * size); a chipset that was freed or reloaded blanks the whole cache.
 * 1 when the cache was updated, 0 when there was nothing to draw from. */
int Map_Refresh(Map_t* m)
{
	Bmp_t sheet;
	int cy, cx;
	if(!m->viewValid || !m->hasChipset)
		return 0;
	if(!BmpMgr_GetInfo(gDispBmpMgr, &sheet, m->chipBmp) ||
		BmpMgr_Generation(gDispBmpMgr, m->chipBmp) != m->chipGen)
	{
		Bmp_Clear(&m->cache, NULL);
		m->dirty = 0;
		return 0;
	}
	for(cy = 0; (uint32_t)cy < (uint32_t)m->cacheRows; cy++)
	{
		for(cx = 0; (uint32_t)cx < (uint32_t)m->cacheCols; cx++)
		{
			int i = cy * m->cacheCols + cx;
			uint16_t want = m->wanted[i];
			if(m->drawn[i] == want)
				continue;
			if(want == 0xffff || (uint32_t)want >= (uint32_t)m->chipCount)
				Bmp_Blit(&m->cache, cx * m->chipW, cy * m->chipH, &m->chips[0], 0x41, 0);
			else
				Bmp_Blit(&m->cache, cx * m->chipW, cy * m->chipH, &m->chips[want], 0x80, 0);
			m->drawn[i] = want;
		}
	}
	m->dirty = 0;
	return 1;
}

/* Slice the chipset bitmap into tiles, numbered row by row from the
 * top-left; each tile is a descriptor pointing into the bitmap manager's
 * pixels, valid while the bitmap's generation stays the same.  The bitmap
 * must hold at least one tile each way and be pixel-compatible with the
 * cache.  1 ok, 0 before the size is set or when the bitmap is unusable. */
int Map_SetChipset(Map_t* m, int bmp)
{
	Bmp_t info;
	uint32_t cols, rows, r, c;
	Bmp_t* chip;
	if(!m->sized)
		return 0;
	if(!BmpMgr_GetInfo(gDispBmpMgr, &info, bmp))
		return 0;
	cols = (uint32_t)info.w / (uint32_t)m->chipW;
	rows = (uint32_t)info.h / (uint32_t)m->chipH;
	if(cols == 0 || rows == 0)
		return 0;
	if(!Bmp_ModesMatch(&info, &m->cache))
		return 0;
	Map_FreeChips(m);
	m->hasChipset = 1;
	m->chipBmp = bmp;
	m->chipGen = BmpMgr_Generation(gDispBmpMgr, bmp);
	m->chipCount = (int32_t)(rows * cols);
	m->chips = (Bmp_t*)BGI_Alloc((size_t)m->chipCount * sizeof(Bmp_t));
	chip = m->chips;
	for(r = 0; r < rows; r++)
	{
		uint8_t* px = info.pixels + (ptrdiff_t)m->chipH * info.pitch * (ptrdiff_t)r;
		for(c = 0; c < cols; c++, chip++)
		{
			chip->pixels = px;
			chip->pitch = info.pitch;
			chip->w = m->chipW;
			chip->h = m->chipH;
			chip->mode = info.mode;
			chip->bpp = info.bpp;
			px += (ptrdiff_t)m->chipW * info.bpp;
		}
	}
	return 1;
}

// drop the chip table; 1 if there was one
int Map_FreeChips(Map_t* m)
{
	int had = m->hasChipset;
	if(had)
	{
		BGI_Free(m->chips);
		m->chips = NULL;
		m->hasChipset = 0;
	}
	return had;
}

// drop the cache bitmap and the cell tables (the map is unsized again); 1 if there were any
int Map_FreeCache(Map_t* m)
{
	int had = m->sized;
	if(had)
	{
		BGI_Free(m->cache.pixels);
		m->cache.pixels = NULL;
		BGI_Free(m->drawn);
		m->drawn = NULL;
		BGI_Free(m->wanted);
		m->wanted = NULL;
		m->sized = 0;
	}
	return had;
}

// drop the terrain copy; 1 if there was one
int Map_FreeTerrain(Map_t* m)
{
	int had = m->hasTerrain;
	if(had)
	{
		BGI_Free(m->terrain);
		m->terrain = NULL;
		m->hasTerrain = 0;
	}
	return had;
}

// forget the view: the next Map_SetView refills every cell
void Map_ResetView(Map_t* m)
{
	m->viewValid = 0;
	m->viewCol = m->viewRow = -1;
	m->offX = m->offY = -1;
}

const DispObjVtbl_t Map_Vtbl = {
	Map_Destroy,
	DispObj_SetVisible,
	DispObj_IsVisible,
	DispObj_Invalidate,
	Map_Draw,
	DispObj_SortKey,
	DispObj_LocalRect,
	DispObj_ScreenRect,
	DispObj_SetPosEx,
	DispObj_SetPos,
	DispObj_GetPos,
	DispObj_GetDrawPos,
	DispObj_SetOffset,
	DispObj_SetFixedPos,
	DispObj_GetFixedPos,
	DispObj_SetLevel,
	DispObj_GetLevel,
	DispObj_SetProgress,
	DispObj_SetParam,
	DispObj_HitTest,
	DispObj_NopNotify,
	DispObj_BuildCache,
	DispObj_SetSize,
	DispObj_GetParam,
};
