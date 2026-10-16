/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * map.h - the Map display object (class order 1), a scrolling tile map;
 * implemented in src/gfx/map.c.
 *
 * A chipset bitmap sliced into equal tiles, a terrain array of 16-bit tile
 * numbers (0xFFFF = empty) and a view position.  The object keeps a private
 * cache bitmap one tile larger than the view and re-blits only the cells
 * whose tile number changed.  Instructions "90 70".."90 7A".
 */
#ifndef BGI_GFX_MAP_H_
#define BGI_GFX_MAP_H_

#include "bgi/gfx/dispobj.h"

typedef struct Map
{
	DispObj_t obj;
	int32_t sized;                // setSize done
	int32_t viewCols, viewRows;   // visible tiles
	int32_t cacheCols, cacheRows; // view + 1
	int32_t chipW, chipH;         // tile size, pixels, 1..0x100
	Bmp_t cache;                  // cacheCols*chipW x cacheRows*chipH, screen mode with alpha
	uint16_t* drawn;              // tile painted in each cache cell
	uint16_t* wanted;             // tile that should be there
	int32_t hasTerrain;
	int32_t terrW, terrH;     // terrain size, in tiles
	uint16_t* terrain;        // owned copy, row-major
	int32_t viewValid;        // setView done
	int32_t viewCol, viewRow; // terrain cell at the cache's top-left (-1 unset)
	int32_t offX, offY;       // pixel offset of the view inside that cell
	Rect_t src;               // visible part of the cache
	int32_t dirty;            // some drawn[] != wanted[]
	int32_t hasChipset;
	int32_t chipBmp;   // the chipset bitmap number
	int32_t chipGen;   // its generation when set
	int32_t chipCount; // tiles in the chipset
	Bmp_t* chips;      // one view per tile, into the chipset bitmap
} Map_t;

extern const DispObjVtbl_t Map_Vtbl;

void Map_Ctor(Map_t* m, int slotId);
void Map_Dtor(Map_t* m);
/* "90 75": chipset, position, effect, level, priority.  Always 0 - the
 * failure of Map_SetChipset is not reported (see map.c) */
int Map_Set(Map_t* m, int x, int y, int bmp, int effect, int level, int prio);
/* "90 76": view in tiles and tile size; 0 ok, 0x80000002 bad view,
 * 0x80000003 bad tile size */
int Map_SetSize(Map_t* m, int cols, int rows, int chipW, int chipH);
int Map_SetTerrain(Map_t* m, int w, int h, const uint16_t* data);          // "90 78"; 1 ok
int Map_SetView(Map_t* m, int col, int row, int offX, int offY, int wrap); // "90 79"; 1 ok
int Map_InvalidateChip(Map_t* m, int tile);                                // "90 7A": re-blit cells showing `tile`; 1 ok
int Map_InvalidateAllCells(Map_t* m);                                      // 1 ok
int Map_Refresh(Map_t* m);                                                 // 1 if the cache was updated
int Map_SetChipset(Map_t* m, int bmp);                                     // 1 ok
int Map_FreeChips(Map_t* m);                                               // 1 if there was a chip table
int Map_FreeCache(Map_t* m);                                               // 1 if there was a cache
int Map_FreeTerrain(Map_t* m);                                             // 1 if there was a terrain
void Map_ResetView(Map_t* m);

#endif // BGI_GFX_MAP_H_
