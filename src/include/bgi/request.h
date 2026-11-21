/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * request.h - what the engine asks its host (src/sys/request.c; the
 *             host's side is OpenBGI_NextRequest / OpenBGI_Answer of
 *             openbgi.h); not part of the original engine
 *
 * What the original asked the user in the middle of a pass - a message
 * box, a dialog, the game chooser, an installer window - is a request
 * here: queued with Request_Post (or Request_Box for a message box),
 * handed to the host in order, answered between two frames.  A message
 * box or a script's dialog may be the frame's instead, which the engine
 * draws and answers itself (Request_t.inframe, bgi/inframe.h).  While any is
 * unanswered (Request_Pending) the engine holds still: OpenBGI_RunFrame
 * runs nothing, and a pass that made the request goes on afterwards where
 * it stopped.  The one who asked either takes the answer (Request_Take,
 * usually from the step of a WaitStep, waitobj.h) or does not wait for it
 * (Request_Forget: a box that only informs).
 *
 * AskRetry is the original's "insert the disc" pair of boxes, which many
 * of its waits share: the message with OK / Cancel, and on Cancel "abort?"
 * with Yes / No.  Retry_t adds the 200 ms pause the loader's prompt makes
 * before it tries again (RetryPrompt, src/fs/paths.c).
 */
#ifndef BGI_REQUEST_H_
#define BGI_REQUEST_H_

#include "bgi/common.h"
#include "openbgi.h"

#define REQUEST_MAX    8 // requests outstanding at once; the engine holds still while one is, so a few suffice

// the life of a request (Request_t.state)
#define REQUEST_FREE   0
#define REQUEST_QUEUED 1 // made, not yet handed to the host
#define REQUEST_OUT    2 // with the host
#define REQUEST_DONE   3 // answered, not yet taken

typedef struct Request
{
	OpenBGI_Request_t q; // what the host is given
	int state;           // REQUEST_*
	int result;          // the answer
	int forgotten;       // nobody takes the answer: the request goes when it arrives
	int treat;           // REQUEST_TREAT_*: what the engine does before and after it
	int inframe;         // the engine draws it in the frame (bgi/inframe.h) instead of handing it to the host
	int cursorPrev;      // a box's: the cursor's show count before it
	char* text;          // a box's own copies of its strings
	char* caption;
} Request_t;

/* What the engine does around a request, as the original did around the
 * window it showed: when the request is made and when it is answered. */
#define REQUEST_TREAT_NONE   0
#define REQUEST_TREAT_BOX    1 // a message box: MsgBox_Begin / MsgBox_End (the cursor, the clock, the key states)
#define REQUEST_TREAT_DIALOG 2 // a script dialog ("B0 84" ..): the clock stands still, the key states are cleared after it
#define REQUEST_TREAT_CLOCK  3 // the clock stands still ("81 38")

/* Queue `q` (its id is assigned) with the treatment `treat`; the id, 0
 * when the queue is full (said on stderr; the asker takes 0 as the
 * answer). */
uint32_t Request_Post(OpenBGI_t* o, const OpenBGI_Request_t* q, int treat);
/* A message box; the strings are copied.  `treat` is REQUEST_TREAT_BOX
 * (the engine's boxes) or REQUEST_TREAT_NONE (the original's direct
 * MessageBox calls). */
uint32_t Request_Box(OpenBGI_t* o, const char* text, const char* caption, uint32_t flags, int treat);
/* 1 and *result when request `id` was answered (it is gone then); 0 while
 * it is not.  An id the queue does not know (0, or dropped) answers 0. */
int Request_Take(OpenBGI_t* o, uint32_t id, int* result);
void Request_Forget(OpenBGI_t* o, uint32_t id); // the answer is not wanted: the request goes when it arrives
int Request_Pending(OpenBGI_t* o);              // requests made and not yet answered
void Request_Clear(OpenBGI_t* o);               // the engine's shutdown: every request goes, answered or not
/* The oldest request drawn in the frame that is not up yet: it is up
 * then (answered with OpenBGI_Answer), *out its description; 0 when there
 * is none. */
int Request_NextInframe(OpenBGI_t* o, OpenBGI_Request_t* out);

// what AskRetry_Step and Retry_Step report
#define ASK_WAIT  0 // a box is up
#define ASK_RETRY 1 // try again
#define ASK_ABORT 2 // give up

typedef struct AskRetry
{
	int state;    // 0 nothing asked, 1 the message is up, 2 "abort?" is up
	uint32_t req; // the box that is up
} AskRetry_t;

/* One step of the pair: the message `text` (caption `caption`) with OK /
 * Cancel; OK retries, Cancel asks MSG_ASK_ABORT (caption `abortCaption`)
 * with Yes / No, Yes gives up.  `strictCancel`: only Cancel leads to the
 * question (the original's `== IDCANCEL`), else anything but OK does
 * (`!= IDOK`).  Called again until it reports ASK_RETRY or ASK_ABORT,
 * after which it starts over. */
int AskRetry_Step(OpenBGI_t* o, AskRetry_t* a, const char* text, const char* caption, const char* abortCaption, int strictCancel);

typedef struct Retry
{
	int active;     // a retry is under way
	AskRetry_t ask; // the boxes
	int pausing;    // OK was chosen: the 200 ms before the next try
	uint32_t until; // the end of the pause (host clock)
} Retry_t;

/* The loader's disc prompt (RetryPrompt): the strings "80 3F" kept, then
 * 200 ms before the next try.  ASK_WAIT while a box is up or the pause
 * runs, then ASK_RETRY or ASK_ABORT (the retry is over then). */
void Retry_Begin(OpenBGI_t* o, Retry_t* r);
int Retry_Step(OpenBGI_t* o, Retry_t* r);

#endif // BGI_REQUEST_H_
