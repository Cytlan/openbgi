/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * request.c - the requests the engine makes of its host and the host's
 *             side of them (src/include/bgi/request.h, OpenBGI_NextRequest and
 *             OpenBGI_Answer of include/openbgi.h); not part of the original
 *             engine
 *
 * The queue is a handful of slots in o->req.  A request is made (QUEUED),
 * handed to the host by OpenBGI_NextRequest (OUT), answered by
 * OpenBGI_Answer (DONE) and taken by whoever asked (FREE again), or freed
 * on its answer when nobody waits for it.  One the frame takes (inframe,
 * decided when it is made) goes out to the dialog of src/sys/inframe.c
 * instead (Request_NextInframe), which answers it the same way.  o->req.pending counts the
 * requests made and not yet answered: the scheduler looks at it after
 * every instruction (src/vm/sched.c).
 */
#include "bgi/request.h"
#include "bgi/error.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/input.h"
#include "bgi/host.h"
#include "bgi/inframe.h"
#include "bgi/strutil.h"
#include "bgi/sjis.h"
#include "bgi/core.h"

#include <stdio.h>
#include <string.h>

static void Request_Free(OpenBGI_t* o, Request_t* r)
{
	BGI_Free(r->text);
	BGI_Free(r->caption);
	memset(r, 0, sizeof *r);
	(void)o;
}

static Request_t* Request_Find(OpenBGI_t* o, uint32_t id)
{
	int i;
	if(!id)
		return NULL;
	for(i = 0; i < REQUEST_MAX; i++)
		if(o->req.items[i].state != REQUEST_FREE && o->req.items[i].q.id == id)
			return &o->req.items[i];
	return NULL;
}

static Request_t* Request_Queue(OpenBGI_t* o, const OpenBGI_Request_t* q)
{
	int i;
	for(i = 0; i < REQUEST_MAX; i++)
	{
		Request_t* r = &o->req.items[i];
		if(r->state != REQUEST_FREE)
			continue;
		memset(r, 0, sizeof *r);
		r->q = *q;
		if(++o->req.serial == 0) // 0 means "no request"
			o->req.serial = 1;
		r->q.id = o->req.serial;
		r->state = REQUEST_QUEUED;
		r->inframe = Inframe_Takes(o, q->kind);
		o->req.pending++;
		return r;
	}
	fprintf(stderr, "openbgi: too many questions for the host at once; one of kind %d is dropped\n", q->kind);
	return NULL;
}

// the treatment before the request (the box's strings are its own copies by then)
static void Request_TreatBegin(OpenBGI_t* o, Request_t* r, int treat)
{
	r->treat = treat;
	switch(treat)
	{
		case REQUEST_TREAT_BOX: r->cursorPrev = MsgBox_Begin(o, r->q.u.message.text, r->q.u.message.caption); break;
		case REQUEST_TREAT_DIALOG:
		case REQUEST_TREAT_CLOCK: Clock_BoxBegin(o); break;
		default: break;
	}
}

// and after its answer, as soon as the window is closed, before the next frame
static void Request_TreatEnd(OpenBGI_t* o, Request_t* r)
{
	switch(r->treat)
	{
		case REQUEST_TREAT_BOX: MsgBox_End(o, r->cursorPrev); break;
		case REQUEST_TREAT_DIALOG:
			Input_ClearStates(o);
			Clock_BoxEnd(o);
			break;
		case REQUEST_TREAT_CLOCK: Clock_BoxEnd(o); break;
		default: break;
	}
}

uint32_t Request_Post(OpenBGI_t* o, const OpenBGI_Request_t* q, int treat)
{
	Request_t* r = Request_Queue(o, q);
	if(!r)
		return 0;
	Request_TreatBegin(o, r, treat);
	return r->q.id;
}

/* A copy of a box's text as the host shows text: in the scripts' encoding
 * ("81 00").  The system scripts of the games that switch to UTF-8 still
 * hold their notices and questions in Shift-JIS, and Windows showed
 * those as they were; so in UTF-8 mode a text that is not UTF-8
 * (TextSniff) is converted, whatever in it is UTF-8 already kept. */
static char* BoxText(OpenBGI_t* o, const char* text)
{
	size_t n;
	char* out;
	if(!text)
		text = "";
	if(!o->enc.textUtf8 || TextSniff(text) != TEXT_SJIS)
		return BGI_Strdup(text);
	n = strlen(text) * 3 + 1;
	out = (char*)BGI_Alloc(n);
	Sjis_ToUtf8(text, out, n, 1);
	return out;
}

uint32_t Request_Box(OpenBGI_t* o, const char* text, const char* caption, uint32_t flags, int treat)
{
	OpenBGI_Request_t q;
	Request_t* r;
	memset(&q, 0, sizeof q);
	q.kind = OPENBGI_ASK_MESSAGE;
	q.u.message.flags = flags;
	r = Request_Queue(o, &q);
	if(!r)
		return 0;
	r->text = BoxText(o, text);
	r->caption = BoxText(o, caption);
	r->q.u.message.text = r->text;
	r->q.u.message.caption = r->caption;
	Request_TreatBegin(o, r, treat);
	return r->q.id;
}

int Request_Take(OpenBGI_t* o, uint32_t id, int* result)
{
	Request_t* r = Request_Find(o, id);
	if(!r)
	{
		*result = 0;
		return 1;
	}
	if(r->state != REQUEST_DONE)
		return 0;
	*result = r->result;
	Request_Free(o, r);
	return 1;
}

void Request_Forget(OpenBGI_t* o, uint32_t id)
{
	Request_t* r = Request_Find(o, id);
	if(!r)
		return;
	if(r->state == REQUEST_DONE)
		Request_Free(o, r);
	else
		r->forgotten = 1;
}

int Request_Pending(OpenBGI_t* o)
{
	return o->req.pending;
}

void Request_Clear(OpenBGI_t* o)
{
	int i;
	for(i = 0; i < REQUEST_MAX; i++)
		if(o->req.items[i].state != REQUEST_FREE)
			Request_Free(o, &o->req.items[i]);
	o->req.pending = 0;
}

// ---- the host's side --------------------------------------------------------------

// the oldest request not handed out yet, of the host's or of the frame's: out then
static int Request_Next(OpenBGI_t* o, int inframe, OpenBGI_Request_t* out)
{
	Request_t* first = NULL;
	int i;
	for(i = 0; i < REQUEST_MAX; i++)
	{
		Request_t* r = &o->req.items[i];
		if(r->state == REQUEST_QUEUED && r->inframe == inframe && (!first || (int32_t)(r->q.id - first->q.id) < 0))
			first = r; // the oldest (the ids grow and wrap)
	}
	if(!first)
		return 0;
	first->state = REQUEST_OUT;
	*out = first->q;
	return 1;
}

int Request_NextInframe(OpenBGI_t* o, OpenBGI_Request_t* out)
{
	return Request_Next(o, 1, out);
}

int OpenBGI_NextRequest(OpenBGI_t* o, OpenBGI_Request_t* out)
{
	OpenBGI_Request_t q; // whole here; the host gets what its own holds of it
	if(!Request_Next(o, 0, &q))
		return 0;
	Host_Give(out, &q, sizeof q);
	return 1;
}

void OpenBGI_Answer(OpenBGI_t* o, uint32_t id, int result)
{
	Request_t* r = Request_Find(o, id);
	if(!r || r->state == REQUEST_DONE)
		return;
	r->result = result;
	r->state = REQUEST_DONE;
	o->req.pending--;
	Request_TreatEnd(o, r);
	if(r->forgotten)
		Request_Free(o, r);
}

// ---- the "insert the disc" boxes ----------------------------------------------------

int AskRetry_Step(OpenBGI_t* o, AskRetry_t* a, const char* text, const char* caption, const char* abortCaption, int strictCancel)
{
	int r;
	switch(a->state)
	{
		case 0:
			a->req = MsgBox_Ask(o, text, caption, OPENBGI_MB_OKCANCEL | OPENBGI_MB_ICONINFO);
			a->state = 1;
			return ASK_WAIT;
		case 1:
			if(!Request_Take(o, a->req, &r))
				return ASK_WAIT;
			if(strictCancel ? r != OPENBGI_IDCANCEL : r == OPENBGI_IDOK)
			{
				a->state = 0;
				return ASK_RETRY;
			}
			a->req = MsgBox_Ask(o, MSG_ASK_ABORT, abortCaption, OPENBGI_MB_YESNO | OPENBGI_MB_ICONQUESTION | OPENBGI_MB_DEFBUTTON2);
			a->state = 2;
			return ASK_WAIT;
		default:
			if(!Request_Take(o, a->req, &r))
				return ASK_WAIT;
			a->state = 0;
			return r == OPENBGI_IDYES ? ASK_ABORT : ASK_RETRY;
	}
}

void Retry_Begin(OpenBGI_t* o, Retry_t* r)
{
	memset(r, 0, sizeof *r);
	r->active = 1;
	Retry_Step(o, r); // the first box goes up at once
}

/* The prompt of the original's RetryPrompt shows the strings of "80 3F"
 * the other way round: the message as the box's text is installTitle, its
 * caption installMsg (also the caption of "abort?").  Only Cancel asks
 * whether to abort. */
int Retry_Step(OpenBGI_t* o, Retry_t* r)
{
	int a;
	if(!r->active)
		return ASK_RETRY;
	if(r->pausing)
	{
		if((int32_t)(Clock_HostMs(o) - r->until) < 0)
			return ASK_WAIT;
		r->active = r->pausing = 0;
		return ASK_RETRY;
	}
	a = AskRetry_Step(o, &r->ask, o->files.installTitle, o->files.installMsg, o->files.installMsg, 1);
	if(a == ASK_WAIT)
		return ASK_WAIT;
	if(a == ASK_ABORT)
	{
		r->active = 0;
		return ASK_ABORT;
	}
	r->pausing = 1;
	r->until = Clock_HostMs(o) + 200;
	return ASK_WAIT;
}
