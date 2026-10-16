/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * winmsg.c - what the scripts see of the window's messages: the waits on
 *            a message number ("80 54") and the dropped files ("80 6C" /
 *            "80 6D"); interface in bgi/display.h
 *
 * A thread that waits for a window message registers (thread, message
 * number) here; the window's event handlers (window.c) dispatch every
 * message they see, and the wait object polls its record until the
 * message has arrived.  Dropped files come the same way: the window's
 * drop event stores the path, "80 6D" reads it.
 */
#include "bgi/display.h"
#include "bgi/vm.h"
#include "bgi/os.h"

// ---- window-message waits ---------------------------------------------------------------

static WinMsgRec_t* gWinMsgList; // one record per subscribed (thread, message)

// Subscribe a thread to a message number (one record per call; nothing is deduplicated).
void WinMsgWait_Register(Thread_t* t, uint32_t msg)
{
	WinMsgRec_t* r = (WinMsgRec_t*)BGI_Alloc(sizeof(WinMsgRec_t));
	r->thread = t;
	r->msg = msg;
	r->arrived = 0;
	r->next = gWinMsgList;
	gWinMsgList = r;
}

// A message arrived: every subscriber of its number records its parameters.
void WinMsgWait_Dispatch(uint32_t msg, uint32_t wParam, uint32_t lParam)
{
	WinMsgRec_t* r;
	for(r = gWinMsgList; r; r = r->next)
	{
		if(r->msg == msg)
		{
			r->arrived = 1;
			r->wParam = wParam;
			r->lParam = lParam;
		}
	}
}

// Drop the thread's subscription to a message number (the first matching record).
void WinMsgWait_Unregister(Thread_t* t, uint32_t msg)
{
	WinMsgRec_t** link;
	for(link = &gWinMsgList; *link; link = &(*link)->next)
	{
		if((*link)->thread == t && (*link)->msg == msg)
		{
			WinMsgRec_t* r = *link;
			*link = r->next;
			BGI_Free(r);
			return;
		}
	}
}

/* Copy the thread's record for the message into *out and clear its
 * arrival flag; 1 when the thread is subscribed, 0 when not.  The caller
 * reads out->arrived to learn whether the message came. */
int WinMsgWait_Poll(WinMsgRec_t* out, Thread_t* t, uint32_t msg)
{
	WinMsgRec_t* r;
	for(r = gWinMsgList; r; r = r->next)
	{
		if(r->thread == t && r->msg == msg)
		{
			*out = *r;
			out->next = NULL;
			r->arrived = 0;
			return 1;
		}
	}
	return 0;
}

// Forget every subscription (shutdown).
void WinMsgWait_Clear(void)
{
	while(gWinMsgList)
	{
		WinMsgRec_t* n = gWinMsgList->next;
		BGI_Free(gWinMsgList);
		gWinMsgList = n;
	}
}

// ---- drag and drop ------------------------------------------------------------------------

static int gDropEnabled;         // "80 6C": the window accepts dropped files
static char gDroppedFile[0x104]; // the last dropped path (MAX_PATH bytes); empty when none

// "80 6C": accept (or refuse) dropped files; the last dropped path is forgotten either way.
void EnableDragDrop(int on)
{
	gDropEnabled = on;
	OS_DragAccept(on);
	memset(gDroppedFile, 0, sizeof gDroppedFile);
}

// A file was dropped on the window (or its path sent by a second instance): remember it.
void DragDrop_OnFile(const char* path)
{
	if(strlen(path) < 0x104) // a longer path is ignored
		strcpy(gDroppedFile, path);
}

// "80 6D": the last dropped path into buf; 1 when there is one and drops are enabled, else 0.
int GetDroppedFile(char* buf)
{
	if(!gDropEnabled || gDroppedFile[0] == 0)
		return 0;
	strcpy(buf, gDroppedFile);
	return 1;
}
