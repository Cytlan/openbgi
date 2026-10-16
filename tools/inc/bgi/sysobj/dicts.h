/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dicts.h - string-keyed record tables
 *
 * A dictionary maps strings to records of a fixed size, kept in insertion
 * order so that they can also be read by position.  The scripts create
 * ("80 D0") and delete ("80 D1") them, addressing them by the id creation
 * hands out, and set, remove and read entries ("80 D2" .. "80 D4"); a
 * fresh engine state drops them all.
 */
#ifndef BGI_SYSOBJ_DICTS_H_
#define BGI_SYSOBJ_DICTS_H_

#include "bgi/common.h"

// a new dictionary of `elemSize`-byte records (at least 2), its id in *outId ("80 D0"); 0 ok, 0x80000001 bad size
uint32_t Dict_Create(uint32_t* outId, uint32_t elemSize);
// delete a dictionary with its entries ("80 D1"); 0 ok, 0x80000002 unknown dictionary
uint32_t Dict_Delete(uint32_t id);
// delete every dictionary and restart the ids (the engine's reset)
void Dict_DeleteAll(void);
// store a copy of the record under `key`, replacing an existing one ("80 D2"); 0 ok, 0x80000002 unknown dictionary
uint32_t Dict_Set(uint32_t id, const char* key, const void* data);
// remove the entry of `key` ("80 D3"); 0 ok, 0x80000002 unknown dictionary, 0x80000003 unknown key
uint32_t Dict_Remove(uint32_t id, const char* key);
/* the record of `key`, or the one at position `idx` (insertion order)
 * when key is NULL, into dst ("80 D4"); 0 ok, 0x80000002 unknown
 * dictionary, 0x80000003 unknown key / position */
uint32_t Dict_Get(void* dst, uint32_t id, const char* key, uint32_t idx);

#endif // BGI_SYSOBJ_DICTS_H_
