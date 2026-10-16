/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * strtables.h - numbered string lists
 *
 * A table is a list of strings under an id the script chooses; strings
 * are appended ("80 DC"), counted ("80 D9"), read by index ("80 DD"),
 * measured ("80 DE"), gathered into one block ("80 DB") and replaced
 * wholesale ("80 DA"); "80 DA", "80 DB" and "80 DE" exist from 1.599 on.
 * "80 D8" clears every table but - from 1.616 on - the one the global
 * names live in (GNAME_TABLE, names.h).  1.599 on keep each string once
 * per table, and "80 DC" yields the string's index.
 */
#ifndef BGI_SYSOBJ_STRTABLES_H_
#define BGI_SYSOBJ_STRTABLES_H_

#include "bgi/common.h"

#define GNAME_TABLE 0x80000000u // the table of the global names (1.616 on)

// drop every table ("80 D8"); with keepNames the global names' table stays
void StrTab_Clear(int keepNames);
// the strings in table `id` ("80 D9"; 0 for an unknown table)
int StrTab_Count(uint32_t id);
/* append a copy of `s` to table `id`, creating the table when needed
 * ("80 DC"); 1.599 on return the index of an existing equal string
 * instead of adding it again.  The string's index. */
int StrTab_Add(uint32_t id, const char* s);
// the index of `s` in the table, -1 when absent (or the table is unknown)
int StrTab_Find(uint32_t id, const char* s);
// string `idx` into dst ("80 DD"); 0 ok, 0x80000001 unknown table, 0x80000002 bad index
int StrTab_Get(char* dst, uint32_t id, uint32_t idx);
// the length of string `idx` into *outLen ("80 DE"); the codes of StrTab_Get
int StrTab_Length(int32_t* outLen, uint32_t id, uint32_t idx);
// drop table `id`; 1 when it existed
int StrTab_Remove(uint32_t id);
/* replace the table by `count` NUL-terminated strings following each
 * other at `strs` ("80 DA"; count 0 drops the table); 1 done, 0 for a
 * negative count or no strings */
int StrTab_SetAll(uint32_t id, int count, const char* strs);
/* every string of the table, terminators included, one after the other
 * into dst (when not NULL) ("80 DB"); the bytes they take, 0 for an
 * unknown table */
int StrTab_Gather(char* dst, uint32_t id);

#endif // BGI_SYSOBJ_STRTABLES_H_
