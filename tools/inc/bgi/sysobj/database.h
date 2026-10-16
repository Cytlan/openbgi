/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * database.h - the global database file "BGI.gdb"
 *
 * What survives between runs of a game: the window position, the first
 * 0x400 bytes of global memory, the system area (gSysArea, which "80 82"
 * writes and "80 83" reads), the global names and the flag groups.
 * "80 80" loads it, "80 81" saves it; the engine reads the window
 * position from it when it creates the window.  The file is SDC-encoded
 * (codec.h); it lives next to the game until 1.69 build 444 and in the
 * user's data directory ("80 39") from build 472 on.
 */
#ifndef BGI_SYSOBJ_DATABASE_H_
#define BGI_SYSOBJ_DATABASE_H_

#include "bgi/common.h"

// write the database ("80 81"); 1 when the file was written completely
int GlobalDB_Save(void);
/* read the database into the live state ("80 80"): 0 ok, 0x80000001 no
 * file, 0x80000002 damaged (what was decoded so far stays applied) */
uint32_t GlobalDB_Load(void);
/* the window position the database holds, when the window would still be
 * on the screen from there; 1 and the position in *x, *y, else 0 */
int SavedWindowPos_Load(int* x, int* y);

// create the pieces the database covers (the system area, the flag manager, the names) / free them
void SysArea_Init(void);
void SysArea_Shutdown(void);

#endif // BGI_SYSOBJ_DATABASE_H_
