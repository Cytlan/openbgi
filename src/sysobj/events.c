/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * events.c - the engine's event queue for the scripts (src/include/bgi/sysobj/events.h)
 *
 * A singly linked list with head and tail pointers; posting appends at
 * the tail, "80 A0" takes from the head.
 */
#include "bgi/sysobj/events.h"
#include "bgi/core.h"

typedef struct MsgNode // one queued event
{
	uint32_t v[3];        // kind and two arguments
	struct MsgNode* next; // the next newer event
} MsgNode_t;

// Drop every event.
void MsgQueue_Clear(OpenBGI_t* o)
{
	while(o->sysobj.msgHead)
	{
		MsgNode_t* n = o->sysobj.msgHead->next;
		BGI_Free(o->sysobj.msgHead);
		o->sysobj.msgHead = n;
	}
	o->sysobj.msgTail = NULL;
}

// Append the event (a, b, c) at the tail.
void MsgQueue_Post(OpenBGI_t* o, uint32_t a, uint32_t b, uint32_t c)
{
	MsgNode_t* n = (MsgNode_t*)BGI_Alloc(sizeof(MsgNode_t));
	n->v[0] = a;
	n->v[1] = b;
	n->v[2] = c;
	n->next = NULL;
	if(o->sysobj.msgTail)
		o->sysobj.msgTail->next = n;
	else
		o->sysobj.msgHead = n;
	o->sysobj.msgTail = n;
}

// Take the oldest event into out[3]; 1 when there was one, 0 when the queue is empty.
int MsgQueue_Get(OpenBGI_t* o, uint32_t out[3])
{
	MsgNode_t* n = o->sysobj.msgHead;
	if(!n)
		return 0;
	o->sysobj.msgHead = n->next;
	if(!o->sysobj.msgHead)
		o->sysobj.msgTail = NULL;
	out[0] = n->v[0];
	out[1] = n->v[1];
	out[2] = n->v[2];
	BGI_Free(n);
	return 1;
}
