/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * names.h - the global name table
 *
 * A set of short strings the scripts register ("80 84") and test for ("80
 * 85"), saved with the global database.  Until 1.599 it is a fixed table
 * of GName_Slots() slots of GNAME_SIZE bytes filled from the front:
 * 0x1000, and 0x4000 from 1.529 on.  1.616 on keep the names in the
 * string table GNAME_TABLE instead (no fixed count, duplicates folded).
 */
#ifndef BGI_SYSOBJ_NAMES_H_
#define BGI_SYSOBJ_NAMES_H_

#include "bgi/common.h"

#define GNAME_COUNT     0x1000 // slots of the fixed table until 1.494 (and of the first database layout)
#define GNAME_COUNT_MAX 0x4000 // slots from 1.529 to 1.599
#define GNAME_SIZE      0x20   // bytes per name, terminator included

int GName_Slots(OpenBGI_t* o); // the fixed table's slots in the selected build

// register a name ("80 84"); 1 when it is present or was added, 0 when the table is full
int GName_Add(OpenBGI_t* o, const char* name);
// 1 when the name is registered ("80 85")
int GName_Exists(OpenBGI_t* o, const char* name);

/* The older database layouts' view of the names: `count` fixed slots.
 * GName_ToSlots fills them from the live table (the first `count` names
 * of the string table of 1.616 on, cut to GNAME_SIZE - 1 characters);
 * GName_FromSlots replaces the live table with the names in them. */
void GName_ToSlots(OpenBGI_t* o, char (*slots)[GNAME_SIZE], int count);
void GName_FromSlots(OpenBGI_t* o, const char (*slots)[GNAME_SIZE], int count);

#endif // BGI_SYSOBJ_NAMES_H_
