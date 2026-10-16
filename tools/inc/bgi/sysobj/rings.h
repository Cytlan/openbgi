/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * rings.h - bounded most-recent-first record lists
 *
 * A ring keeps the last `maxCount` records of `elemSize` bytes pushed into
 * it, index 0 being the newest.  The scripts create ("80 98") and delete
 * ("80 99") rings, which are addressed by the id creation hands out, push
 * ("80 9C") and read ("80 9D") records, remove a run of them ("80 9E",
 * 1.616 on) and ask for the count ("80 9A"); a fresh engine state drops
 * them all.
 */
#ifndef BGI_SYSOBJ_RINGS_H_
#define BGI_SYSOBJ_RINGS_H_

#include "bgi/common.h"

// a new ring ("80 98"), its id (from 1, never reused) in *outId; 0 ok, 2 for a zero count or size
uint32_t Ring_Create(uint32_t* outId, uint32_t maxCount, uint32_t elemSize);
// delete a ring with its records ("80 99"); 0 ok, 1 unknown ring
uint32_t Ring_Delete(uint32_t id);
// delete every ring (the engine's reset)
void Ring_DeleteAll(void);
// the records held, in *out ("80 9A"); 0 ok, 1 unknown ring
uint32_t Ring_Count(uint32_t* out, uint32_t id);
// push `elemSize` bytes as the newest record, dropping the oldest beyond maxCount ("80 9C"); 0 ok, 1 unknown ring
uint32_t Ring_Push(uint32_t id, const void* data);
// record `idx` (0 = newest) into dst ("80 9D"); 0 ok, 1 unknown ring, 2 index out of range
uint32_t Ring_Get(void* dst, uint32_t id, uint32_t idx);
// remove `count` records from index `idx` on ("80 9E", 1.616 on); 0 ok, 1 unknown ring, 2 index out of range
uint32_t Ring_Remove(uint32_t id, uint32_t idx, uint32_t count);

#endif // BGI_SYSOBJ_RINGS_H_
