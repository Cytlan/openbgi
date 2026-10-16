/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * strgroups.h - named groups of string pairs (1.653 on)
 *
 * "81 D8" hands the engine one block of groups, each a name and a list of
 * key / value string pairs; "81 DA" looks a key up in a group.  The block
 * layout: u32 group count, then per group its name (NUL-terminated), u32
 * pair count and the pairs as consecutive NUL-terminated strings.  Loading
 * a block adds to the groups already known (an existing group grows);
 * loading NULL drops everything.
 */
#ifndef BGI_SYSOBJ_STRGROUPS_H_
#define BGI_SYSOBJ_STRGROUPS_H_

#include "bgi/common.h"

// drop every group (the engine's reset)
void StrGroups_Clear(void);
/* parse a block of `size` bytes into the groups ("81 D8"); 1 when exactly
 * `size` bytes were consumed (or data is NULL, which only clears) */
int StrGroups_Load(const uint8_t* data, uint32_t size);
/* the value stored with `key` in `group` ("81 DA"), NULL when either is
 * unknown; the string stays owned by the group */
const char* StrGroups_Lookup(const char* group, const char* key);

#endif // BGI_SYSOBJ_STRGROUPS_H_
