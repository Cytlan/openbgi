/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * history.h - the message back-log
 *
 * The text the game showed, newest first, for the back-log screens: each
 * record carries nine integers the script chooses (speaker, voice,
 * flags ...), the archive, file and name of the message, the message
 * itself and an optional reading (yomigana) list.  The list is bounded;
 * the oldest record goes when it is full.  "80 90" sets the bound, "80
 * 94" / "80 96" add records, "80 95" / "80 97" read them back.
 */
#ifndef BGI_SYSOBJ_HISTORY_H_
#define BGI_SYSOBJ_HISTORY_H_

#include "bgi/common.h"

/* one record as the scripts pass ("80 96") and receive ("80 95", "80 97")
 * it: 0x200 bytes, plus the reading (0x400 in all) on request */
typedef struct HistoryRec
{
	uint32_t v1;         // the first integer
	uint8_t pad0[0x3c];  // unused
	uint32_t v[8];       // the other eight integers
	uint8_t pad1[0x40];  // unused
	char arc[0x20];      // the archive the message came from, "" when not given
	char file[0x20];     // the file, "" when not given
	char name[0x20];     // the name of the message, "" when not given
	char msg[0x100];     // the message text
	char reading[0x200]; // the reading list, History_Get with withReading only
} HistoryRec_t;

// drop every record and allow `max` records from now on ("80 90")
void History_Reset(int max);
// the records in the log ("80 91")
int History_Count(void);
/* append a record ("80 94"; the oldest goes when the list is full); NULL
 * strings are "not given" (msg must be present).  0 ok, 0x80000001 ..
 * 0x80000005 for a string that is too long: arc, file, name (at most 31
 * characters), msg (255), reading (511); nothing is added then. */
uint32_t History_Add(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7,
	uint32_t a8, uint32_t a9, const char* arc, const char* file, const char* name, const char* msg,
	const char* reading);
// the same from a HistoryRec_t ("80 96"); empty strings count as "not given"
uint32_t History_AddStruct(const void* rec);
/* record `idx` (0 = newest) into a HistoryRec_t ("80 95"; "80 97" with
 * withReading, which also fills the reading beyond the first 0x200 bytes);
 * 1 ok, 0 for a bad index */
int History_Get(void* dst, int idx, int withReading);

#endif // BGI_SYSOBJ_HISTORY_H_
