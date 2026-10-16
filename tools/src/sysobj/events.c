/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * events.c - the engine's event queue for the scripts (inc/bgi/sysobj/events.h)
 *
 * A singly linked list with head and tail pointers; posting appends at
 * the tail, "80 A0" takes from the head.
 */
#include "bgi/sysobj/events.h"

typedef struct MsgNode // one queued event
{
	uint32_t v[3];        // kind and two arguments
	struct MsgNode* next; // the next newer event
} MsgNode_t;

static MsgNode_t* gMsgHead; // the oldest event, NULL when the queue is empty
static MsgNode_t* gMsgTail; // the newest event, NULL when the queue is empty

// Drop every event.
void MsgQueue_Clear(void)
{
	while(gMsgHead)
	{
		MsgNode_t* n = gMsgHead->next;
		BGI_Free(gMsgHead);
		gMsgHead = n;
	}
	gMsgTail = NULL;
}

// Append the event (a, b, c) at the tail.
void MsgQueue_Post(uint32_t a, uint32_t b, uint32_t c)
{
	MsgNode_t* n = (MsgNode_t*)BGI_Alloc(sizeof(MsgNode_t));
	n->v[0] = a;
	n->v[1] = b;
	n->v[2] = c;
	n->next = NULL;
	if(gMsgTail)
		gMsgTail->next = n;
	else
		gMsgHead = n;
	gMsgTail = n;
}

// Take the oldest event into out[3]; 1 when there was one, 0 when the queue is empty.
int MsgQueue_Get(uint32_t out[3])
{
	MsgNode_t* n = gMsgHead;
	if(!n)
		return 0;
	gMsgHead = n->next;
	if(!gMsgHead)
		gMsgTail = NULL;
	out[0] = n->v[0];
	out[1] = n->v[1];
	out[2] = n->v[2];
	BGI_Free(n);
	return 1;
}
