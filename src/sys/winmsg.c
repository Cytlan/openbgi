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
#include "bgi/core.h"

// ---- window-message waits ---------------------------------------------------------------

// Subscribe a thread to a message number (one record per call; nothing is deduplicated).
void WinMsgWait_Register(OpenBGI_t* o, Thread_t* t, uint32_t msg)
{
	WinMsgRec_t* r = (WinMsgRec_t*)BGI_Alloc(sizeof(WinMsgRec_t));
	r->thread = t;
	r->msg = msg;
	r->arrived = 0;
	r->next = o->winmsg.winMsgList;
	o->winmsg.winMsgList = r;
}

// A message arrived: every subscriber of its number records its parameters.
void WinMsgWait_Dispatch(OpenBGI_t* o, uint32_t msg, uint32_t wParam, uint32_t lParam)
{
	WinMsgRec_t* r;
	for(r = o->winmsg.winMsgList; r; r = r->next)
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
void WinMsgWait_Unregister(OpenBGI_t* o, Thread_t* t, uint32_t msg)
{
	WinMsgRec_t** link;
	for(link = &o->winmsg.winMsgList; *link; link = &(*link)->next)
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
int WinMsgWait_Poll(OpenBGI_t* o, WinMsgRec_t* out, Thread_t* t, uint32_t msg)
{
	WinMsgRec_t* r;
	for(r = o->winmsg.winMsgList; r; r = r->next)
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
void WinMsgWait_Clear(OpenBGI_t* o)
{
	while(o->winmsg.winMsgList)
	{
		WinMsgRec_t* n = o->winmsg.winMsgList->next;
		BGI_Free(o->winmsg.winMsgList);
		o->winmsg.winMsgList = n;
	}
}

// ---- drag and drop ------------------------------------------------------------------------

// "80 6C": accept (or refuse) dropped files; the last dropped path is forgotten either way.
void EnableDragDrop(OpenBGI_t* o, int on)
{
	o->winmsg.dropEnabled = on;
	MainWin_DragAccept(o, on);
	memset(o->winmsg.droppedFile, 0, sizeof o->winmsg.droppedFile);
}

// A file was dropped on the window (or its path sent by a second instance): remember it.
void DragDrop_OnFile(OpenBGI_t* o, const char* path)
{
	if(strlen(path) < 0x104) // a longer path is ignored
		strcpy(o->winmsg.droppedFile, path);
}

// "80 6D": the last dropped path into buf; 1 when there is one and drops are enabled, else 0.
int GetDroppedFile(OpenBGI_t* o, char* buf)
{
	if(!o->winmsg.dropEnabled || o->winmsg.droppedFile[0] == 0)
		return 0;
	strcpy(buf, o->winmsg.droppedFile);
	return 1;
}
