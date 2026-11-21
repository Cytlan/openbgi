/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * database.h - the global database file "BGI.gdb"
 *
 * What survives between runs of a game: the window position, the first
 * 0x400 bytes of global memory, the system area (o->mem.sysArea, which
 * "80 82" writes and "80 83" reads), the global names and the flag
 * groups.  "80 80" loads it, "80 81" saves it; the engine reads the
 * window position from it when it creates the window; "81 80" merges
 * another copy into the live state.  The file is SDC-encoded (codec.h);
 * it lives next to the game until 1.69 build 444 and in the user's data
 * directory ("80 39") from build 451 on.
 */
#ifndef BGI_SYSOBJ_DATABASE_H_
#define BGI_SYSOBJ_DATABASE_H_

#include "bgi/common.h"

// write the database ("80 81"); 1 when the file was written completely
int GlobalDB_Save(OpenBGI_t* o);
/* read the database into the live state ("80 80"): 0 ok, 0x80000001 no
 * file, 0x80000002 damaged (what was decoded so far stays applied) */
uint32_t GlobalDB_Load(OpenBGI_t* o);
/* the window position the database holds, when the window would still be
 * on the screen from there; 1 and the position in *x, *y, else 0 */
int SavedWindowPos_Load(OpenBGI_t* o, int* x, int* y); /* "81 80" (1.636 on): merge the SDC-encoded GDB 3.00 file image `img`
 * into the live state - its names added to the live ones, its flag groups
 * OR'ed into them - and copy its global memory prefix (0x400 bytes) to
 * `globals` and its system area (0x100000 bytes) to `sysarea` when those
 * are not NULL, zero-padded; *globalsSize = 0x400 and *sysareaSize =
 * 0x100000 always.  0 ok, 0x80000002 not SDC, a wrong decoded size, not
 * GDB 3.00 */
uint32_t GlobalDB_MergeImage(OpenBGI_t* o, const void* img, void* globals, uint32_t* globalsSize, void* sysarea, uint32_t* sysareaSize);

// create the pieces the database covers (the system area, the flag manager, the names) / free them
void SysArea_Init(OpenBGI_t* o);
void SysArea_Shutdown(OpenBGI_t* o);

#endif // BGI_SYSOBJ_DATABASE_H_
