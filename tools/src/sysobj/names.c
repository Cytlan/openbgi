/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * names.c - the global name table (inc/bgi/sysobj/names.h)
 *
 * The scripts register names with "80 84" and ask with "80 85" whether a
 * name is known; the set is part of the global database (database.c).
 * Two storages, by engine generation: the fixed table gNameTable until
 * 1.599, the string table GNAME_TABLE from 1.616 on.
 */
#include "bgi/sysobj/names.h"
#include "bgi/sysobj/strtables.h"
#include "bgi/version.h"

char gNameTable[GNAME_COUNT][GNAME_SIZE];

// 1.616 on keep the names in the string table; "80 D8" (clear the string tables) leaves it alone
#define GNAME_IN_STRTAB (gEngine->gen >= GEN_1_616)

/* Register `name`.  The fixed table is filled from the front and the first
 * empty slot ends the search, so a name is added once.  Returns 1 when the
 * name is present or was added, 0 when the table is full. */
int GName_Add(const char* name)
{
	int i;
	if(GNAME_IN_STRTAB)
	{
		StrTab_Add(GNAME_TABLE, name); // folds duplicates itself
		return 1;
	}
	for(i = 0; i < GNAME_COUNT; i++)
	{
		if(gNameTable[i][0] == 0)
		{
			strcpy(gNameTable[i], name);
			return 1;
		}
		if(strcmp(gNameTable[i], name) == 0)
			return 1;
	}
	return 0;
}

// 1 when `name` is registered
int GName_Exists(const char* name)
{
	int i;
	if(GNAME_IN_STRTAB)
		return StrTab_Find(GNAME_TABLE, name) >= 0;
	for(i = 0; i < GNAME_COUNT; i++)
	{
		if(gNameTable[i][0] == 0)
			return 0; // the first empty slot ends the table
		if(strcmp(gNameTable[i], name) == 0)
			return 1;
	}
	return 0;
}

/* Copy the names into the fixed-slot layout of the database file: the
 * fixed table as it is, or the first GNAME_COUNT entries of the string
 * table truncated to GNAME_SIZE - 1 characters each. */
void GName_ToSlots(char (*slots)[GNAME_SIZE])
{
	int i, n;
	if(!GNAME_IN_STRTAB)
	{
		memcpy(slots, gNameTable, sizeof gNameTable);
		return;
	}
	memset(slots, 0, sizeof gNameTable);
	n = StrTab_Count(GNAME_TABLE);
	for(i = 0; i < n && i < GNAME_COUNT; i++)
	{
		char buf[0x400];
		size_t len;
		if(StrTab_Get(buf, GNAME_TABLE, (uint32_t)i) != 0)
			continue;
		len = strlen(buf);
		memcpy(slots[i], buf, len < GNAME_SIZE ? len : GNAME_SIZE - 1);
	}
}

// Replace the live names with the fixed-slot layout read from the database file.
void GName_FromSlots(const char (*slots)[GNAME_SIZE])
{
	int i;
	if(!GNAME_IN_STRTAB)
	{
		memcpy(gNameTable, slots, sizeof gNameTable);
		return;
	}
	StrTab_Remove(GNAME_TABLE);
	for(i = 0; i < GNAME_COUNT; i++)
		if(slots[i][0])
			StrTab_Add(GNAME_TABLE, slots[i]);
}
