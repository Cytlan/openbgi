/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * flags.h - named groups of bit flags
 *
 * A FlagMgr_t holds named groups, each a bit vector of any length (MSB of
 * the first byte is bit 0): until 1.599 up to a fixed number of groups
 * with names of 1 to 23 characters, from 1.616 on any number with names
 * of any length.  The scripts register groups ("80 88"), set and read
 * single bits ("80 89", "80 8B") and set runs of bits ("80 8A") through
 * the GFlag_* wrappers on the engine's manager o->sysobj.flagMgr,
 * which is saved with the global database (database.c).
 */
#ifndef BGI_SYSOBJ_FLAGS_H_
#define BGI_SYSOBJ_FLAGS_H_

#include "bgi/common.h"

typedef struct FlagMgr FlagMgr_t;

/* result codes of the FlagMgr_* methods ("80 89" .. "80 8B" push them
 * renumbered: 0 ok, 1 unknown group, 2 bad index, 3 bad count) */
#define FLAG_OK          0
#define FLAG_ERR_NAME    0x8001 // the name is empty or longer than 23 characters (until 1.599)
#define FLAG_ERR_BITS    0x8002 // a group with zero bits
#define FLAG_ERR_FULL    0x8003 // no free group slot (until 1.599)
#define FLAG_ERR_UNKNOWN 0x8008 // the group is not registered
#define FLAG_ERR_INDEX   0x8009 // the bit index is out of range
#define FLAG_ERR_COUNT   0x800a // a bad count (FlagMgr_SetBits)

// a manager with room for `maxGroups` groups (until 1.599; no limit after); FlagMgr_Delete frees it with its groups
FlagMgr_t* FlagMgr_New(uint32_t maxGroups);
void FlagMgr_Delete(FlagMgr_t* m);
// drop every group (the manager stays usable)
void FlagMgr_Clear(FlagMgr_t* m);

/* Register a group of `bits` flags, all cleared, or resize an existing
 * group of that name keeping as many of its bits as fit.  FLAG_OK or one
 * of FLAG_ERR_NAME / BITS / FULL. */
uint32_t FlagMgr_Register(OpenBGI_t* o, FlagMgr_t* m, const char* name, uint32_t bits);
// register and fill the bits from `src` ((bits + 7) / 8 bytes); the codes of FlagMgr_Register
uint32_t FlagMgr_RegisterWithData(OpenBGI_t* o, FlagMgr_t* m, const char* name, uint32_t bits, const uint8_t* src);
/* "81 80" (1.636 on): a group from another database merged in - a new
 * group with these bits, or the group grown to at least `bits` with
 * `src` OR'ed into its first (bits + 7) / 8 bytes; the codes of
 * FlagMgr_Register */
uint32_t FlagMgr_Merge(OpenBGI_t* o, FlagMgr_t* m, const char* name, uint32_t bits, const uint8_t* src);
// set (val != 0) or clear bit `idx` of the group; FLAG_OK, FLAG_ERR_UNKNOWN, FLAG_ERR_INDEX
uint32_t FlagMgr_SetBit(FlagMgr_t* m, const char* name, uint32_t idx, uint32_t val);
// set or clear `count` bits from `idx` on (count 1 .. 0x10000 within the group); + FLAG_ERR_COUNT
uint32_t FlagMgr_SetBits(FlagMgr_t* m, const char* name, uint32_t idx, uint32_t val, uint32_t count);
/* read bit `idx` into *out: the masked byte (non-zero when the bit is
 * set, not 0 / 1 - the scripts test it for zero) */
uint32_t FlagMgr_GetBit(FlagMgr_t* m, uint32_t* out, const char* name, uint32_t idx);

// the same on o->sysobj.flagMgr, as the "80 8x" instructions use them
int GFlag_RegisterGroup(OpenBGI_t* o, const char* name, uint32_t bits);                             // "80 88"; 1 ok, 0 any error
uint32_t GFlag_SetBit(OpenBGI_t* o, const char* name, uint32_t idx, uint32_t val);                  // "80 89"
uint32_t GFlag_SetBits(OpenBGI_t* o, const char* name, uint32_t idx, uint32_t val, uint32_t count); // "80 8A"
uint32_t GFlag_GetBit(OpenBGI_t* o, uint32_t* out, const char* name, uint32_t idx);                 // "80 8B"

/* The groups as the database stores them (database.c): group `i` of the
 * `count` in use, in the order they were registered */
typedef struct FlagGroup
{
	char* name;    // until 1.599 at most 23 characters
	uint32_t bits; // the number of flags
	uint8_t* data; // (bits + 7) / 8 bytes, bit 0 in the MSB of the first
} FlagGroup_t;
uint32_t FlagMgr_Count(const FlagMgr_t* m);
const FlagGroup_t* FlagMgr_Group(const FlagMgr_t* m, uint32_t i);
static inline uint32_t FlagBytes(uint32_t bits) // the bytes a bit vector takes
{
	return (bits + 7) >> 3;
}

#endif // BGI_SYSOBJ_FLAGS_H_
