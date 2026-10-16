/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * history.c - the message back-log (inc/bgi/sysobj/history.h)
 *
 * A doubly linked list from the newest record to the oldest; the strings
 * are heap copies made at insertion.
 */
#include "bgi/sysobj/history.h"

typedef struct HistNode // one record
{
	uint32_t v[9]; // the nine integers of the script
	char* arc;     // heap copies, NULL when not given
	char* file;
	char* name;
	char* msg; // always present
	char* reading;
	struct HistNode* newer; // NULL at the newest record
	struct HistNode* older; // NULL at the oldest record
} HistNode_t;

static HistNode_t* gHistOldest; // the two ends of the list, NULL when empty
static HistNode_t* gHistNewest;
static uint32_t gHistMax;   // records allowed (History_Reset)
static uint32_t gHistCount; // records in the list

// free a record and its strings (the caller has unlinked it)
static void HistNode_Free(HistNode_t* n)
{
	if(!n)
		return;
	BGI_Free(n->arc);
	BGI_Free(n->file);
	BGI_Free(n->name);
	BGI_Free(n->msg);
	BGI_Free(n->reading);
	BGI_Free(n);
}

// Drop every record and set the bound.
void History_Reset(int max)
{
	HistNode_t* n = gHistNewest;
	while(n)
	{
		HistNode_t* older = n->older;
		HistNode_Free(n);
		n = older;
	}
	gHistOldest = NULL;
	gHistNewest = NULL;
	gHistMax = (uint32_t)max;
	gHistCount = 0;
}

int History_Count(void)
{
	return (int)gHistCount;
}

// a heap copy of an optional string; -1 when it is `maxLen` characters or longer
static int HistDup(char** dst, const char* s, size_t maxLen)
{
	if(!s)
	{
		*dst = NULL;
		return 0;
	}
	if(strlen(s) >= maxLen)
		return -1;
	*dst = BGI_Strdup(s);
	return 0;
}

/* Append a record as the newest.  The length limits are checked in the
 * order arc, file, name, msg, reading and the result code names the first
 * field that is too long; nothing is kept then. */
uint32_t History_Add(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7,
	uint32_t a8, uint32_t a9, const char* arc, const char* file, const char* name, const char* msg,
	const char* reading)
{
	HistNode_t* n = (HistNode_t*)BGI_Calloc(sizeof(HistNode_t));
	uint32_t err = 0;
	n->v[0] = a1;
	n->v[1] = a2;
	n->v[2] = a3;
	n->v[3] = a4;
	n->v[4] = a5;
	n->v[5] = a6;
	n->v[6] = a7;
	n->v[7] = a8;
	n->v[8] = a9;

	if(HistDup(&n->arc, arc, 0x20) < 0)
		err = 0x80000001u;
	else if(HistDup(&n->file, file, 0x20) < 0)
		err = 0x80000002u;
	else if(HistDup(&n->name, name, 0x20) < 0)
		err = 0x80000003u;
	else if(strlen(msg) >= 0x100)
		err = 0x80000004u;
	else if(HistDup(&n->reading, reading, 0x200) < 0)
		err = 0x80000005u;
	if(err)
	{
		HistNode_Free(n);
		return err;
	}
	n->msg = BGI_Strdup(msg);

	n->newer = NULL;
	n->older = gHistNewest;
	if(gHistNewest)
		gHistNewest->newer = n;
	else
		gHistOldest = n;
	gHistNewest = n;

	if(gHistCount < gHistMax)
	{
		gHistCount++;
	}
	else
	{
		HistNode_t* old = gHistOldest; // the list is full: the oldest record goes
		gHistOldest = old->newer;
		gHistOldest->older = NULL;
		HistNode_Free(old);
	}
	return 0;
}

// History_Add from a record; empty strings count as "not given".
uint32_t History_AddStruct(const void* recp)
{
	const HistoryRec_t* r = (const HistoryRec_t*)recp;
	return History_Add(r->v1, r->v[0], r->v[1], r->v[2], r->v[3], r->v[4], r->v[5], r->v[6], r->v[7],
		r->arc[0] ? r->arc : NULL, r->file[0] ? r->file : NULL, r->name[0] ? r->name : NULL, r->msg,
		r->reading[0] ? r->reading : NULL);
}

/* Copy record `idx` (0 = newest) into a HistoryRec_t: the first 0x200
 * bytes are cleared and filled, the reading (beyond them) only on request,
 * so a 0x200-byte buffer suffices without it.  1 ok, 0 for a bad index. */
int History_Get(void* dst, int idx, int withReading)
{
	HistoryRec_t* r = (HistoryRec_t*)dst;
	HistNode_t* n = gHistNewest;
	int i;
	if((uint32_t)idx >= gHistCount)
		return 0;
	for(i = 0; i < idx; i++)
		n = n->older;
	memset(r, 0, 0x200);
	r->v1 = n->v[0];
	for(i = 0; i < 8; i++)
		r->v[i] = n->v[i + 1];
	if(n->arc)
		strcpy(r->arc, n->arc);
	if(n->file)
		strcpy(r->file, n->file);
	if(n->name)
		strcpy(r->name, n->name);
	strcpy(r->msg, n->msg);
	if(withReading && n->reading)
		strcpy(r->reading, n->reading);
	return 1;
}
