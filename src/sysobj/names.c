/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * names.c - the global name table (src/include/bgi/sysobj/names.h)
 *
 * The scripts register names with "80 84" and ask with "80 85" whether a
 * name is known; the set is part of the global database (database.c).
 * Two storages, by engine generation: the fixed table
 * o->sysobj.nameTable until 1.599 (0x1000 slots, 0x4000 from 1.529 on),
 * the string table GNAME_TABLE from 1.616 on.
 */
#include "bgi/sysobj/names.h"
#include "bgi/sysobj/strtables.h"
#include "bgi/version.h"
#include "bgi/core.h"

// 1.616 on keep the names in the string table; "80 D8" (clear the string tables) leaves it alone
#define GNAME_IN_STRTAB(o) (o->engine.profile->gen >= GEN_1_616)

int GName_Slots(OpenBGI_t* o)
{
	return o->engine.profile->gen >= GEN_1_529 ? GNAME_COUNT_MAX : GNAME_COUNT;
}

/* Register `name`.  The fixed table is filled from the front and the first
 * empty slot ends the search, so a name is added once.  Returns 1 when the
 * name is present or was added, 0 when the table is full. */
int GName_Add(OpenBGI_t* o, const char* name)
{
	int i;
	if(GNAME_IN_STRTAB(o))
	{
		StrTab_Add(o, GNAME_TABLE, name); // folds duplicates itself
		return 1;
	}
	for(i = 0; i < GName_Slots(o); i++)
	{
		if(o->sysobj.nameTable[i][0] == 0)
		{
			strcpy(o->sysobj.nameTable[i], name);
			return 1;
		}
		if(strcmp(o->sysobj.nameTable[i], name) == 0)
			return 1;
	}
	return 0;
}

// 1 when `name` is registered
int GName_Exists(OpenBGI_t* o, const char* name)
{
	int i;
	if(GNAME_IN_STRTAB(o))
		return StrTab_Find(o, GNAME_TABLE, name) >= 0;
	for(i = 0; i < GName_Slots(o); i++)
	{
		if(o->sysobj.nameTable[i][0] == 0)
			return 0; // the first empty slot ends the table
		if(strcmp(o->sysobj.nameTable[i], name) == 0)
			return 1;
	}
	return 0;
}

void GName_ToSlots(OpenBGI_t* o, char (*slots)[GNAME_SIZE], int count)
{
	int i, n;
	memset(slots, 0, (size_t)count * GNAME_SIZE);
	if(!GNAME_IN_STRTAB(o))
	{
		memcpy(slots, o->sysobj.nameTable, (size_t)(count < GName_Slots(o) ? count : GName_Slots(o)) * GNAME_SIZE);
		return;
	}
	n = StrTab_Count(o, GNAME_TABLE);
	for(i = 0; i < n && i < count; i++)
	{
		char buf[0x400];
		int32_t len;
		if(StrTab_Length(o, &len, GNAME_TABLE, (uint32_t)i) != 0 || len >= (int32_t)sizeof buf ||
			StrTab_Get(o, buf, GNAME_TABLE, (uint32_t)i) != 0)
			continue;
		memcpy(slots[i], buf, (size_t)(len < GNAME_SIZE ? len : GNAME_SIZE - 1));
	}
}

void GName_FromSlots(OpenBGI_t* o, const char (*slots)[GNAME_SIZE], int count)
{
	int i;
	if(!GNAME_IN_STRTAB(o))
	{
		memset(o->sysobj.nameTable, 0, sizeof o->sysobj.nameTable);
		memcpy(o->sysobj.nameTable, slots, (size_t)(count < GName_Slots(o) ? count : GName_Slots(o)) * GNAME_SIZE);
		for(i = 0; i < GNAME_COUNT_MAX; i++)
			o->sysobj.nameTable[i][GNAME_SIZE - 1] = 0;
		return;
	}
	StrTab_Remove(o, GNAME_TABLE);
	for(i = 0; i < count; i++)
		if(slots[i][0])
		{
			char name[GNAME_SIZE];
			memcpy(name, slots[i], GNAME_SIZE - 1);
			name[GNAME_SIZE - 1] = 0;
			StrTab_Add(o, GNAME_TABLE, name);
		}
}
