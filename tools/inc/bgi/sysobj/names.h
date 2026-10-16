/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * names.h - the global name table
 *
 * A set of short strings the scripts register ("80 84") and test for ("80
 * 85"), saved with the global database.  Until 1.599 it is a fixed table
 * of GNAME_COUNT slots of GNAME_SIZE bytes filled from the front; 1.616 on
 * keep the names in the string table GNAME_TABLE instead (no fixed count,
 * duplicates folded), while the database file still stores the first
 * GNAME_COUNT of them in the fixed slots.
 */
#ifndef BGI_SYSOBJ_NAMES_H_
#define BGI_SYSOBJ_NAMES_H_

#include "bgi/common.h"

#define GNAME_COUNT 0x1000 // slots of the fixed table (and of the database section)
#define GNAME_SIZE  0x20   // bytes per name, terminator included

// the fixed table of the builds before 1.616 (unused, but kept zeroed, by the later ones)
extern char gNameTable[GNAME_COUNT][GNAME_SIZE];

// register a name ("80 84"); 1 when it is present or was added, 0 when the table is full
int GName_Add(const char* name);
// 1 when the name is registered ("80 85")
int GName_Exists(const char* name);

/* The database's view of the names: the first GNAME_COUNT in fixed slots.
 * GName_ToSlots fills `slots` from the live table (the string table of
 * 1.616 on), GName_FromSlots replaces the live table with `slots`. */
void GName_ToSlots(char (*slots)[GNAME_SIZE]);
void GName_FromSlots(const char (*slots)[GNAME_SIZE]);

#endif // BGI_SYSOBJ_NAMES_H_
