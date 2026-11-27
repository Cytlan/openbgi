/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * steps.c - the input mapper's queue of steps: what the touch screen
 *           (touch.c), a pad (pad.c) and the on-screen keyboard (osk.c)
 *           do to the mouse and the keys the game knows, a step a frame
 *
 * A step waits in the queue with the number of passes to let run between
 * it and the step before; Steps_Frame, which OpenBGI_RunFrame calls
 * before a pass, applies the steps that are due - up to one that has
 * passes to wait, or time (a click's tap delay) - through the calls a
 * host's devices take.  While it
 * does (and in Steps_KeyNow, Steps_KeyTap and Steps_TextNow, which do not
 * queue) `o->input.steps.applying` says that the call is the mapper's
 * own: the engine tells its own input from a mouse or a keyboard the
 * player took up (window.c, Input_DeviceUsed).
 *
 * A button's step waits for a program that reads the buttons itself to
 * have read the step before (Steps_ButtonRead, Unread; bgi/input.h).
 */
#include <string.h>

#include "bgi/input.h"
#include "bgi/sys.h"
#include "bgi/core.h"

// append a step; a step after another lets at least a pass run first
static void Push(OpenBGI_t* o, const InputStep_t* s)
{
	InputSteps_t* q = &o->input.steps;
	InputStep_t* n;
	if(q->count == INPUT_STEPS)
		return; // (a gesture queues a handful; more were never taken)
	n = &q->q[(q->head + q->count) % INPUT_STEPS];
	*n = *s;
	if(q->count > 0 && n->gap < 1)
		n->gap = 1;
	q->count++;
}

void Steps_Move(OpenBGI_t* o, int x, int y, int gap, int follow)
{
	InputStep_t s;
	memset(&s, 0, sizeof s);
	s.kind = INPUT_STEP_MOVE;
	s.x = x;
	s.y = y;
	s.gap = gap;
	s.follow = follow;
	Push(o, &s);
}

void Steps_Button(OpenBGI_t* o, int button, int down)
{
	InputStep_t s;
	memset(&s, 0, sizeof s);
	s.kind = INPUT_STEP_BUTTON;
	s.a = button;
	s.down = down;
	s.gap = 1;
	Push(o, &s);
}

void Steps_Wheel(OpenBGI_t* o, int delta)
{
	InputStep_t s;
	memset(&s, 0, sizeof s);
	s.kind = INPUT_STEP_WHEEL;
	s.a = delta;
	Push(o, &s);
}

// a button step of a click: it also waits the game's tap delay after the step before
static void TapButton(OpenBGI_t* o, int button, int down)
{
	InputStep_t s;
	memset(&s, 0, sizeof s);
	s.kind = INPUT_STEP_BUTTON;
	s.a = button;
	s.down = down;
	s.gap = 1;
	s.wait = o->input.steps.tapDelay;
	Push(o, &s);
}

void Steps_Tap(OpenBGI_t* o, int button)
{
	TapButton(o, button, 1);
	TapButton(o, button, 0);
}

void Steps_Click(OpenBGI_t* o, int button, int x, int y)
{
	Steps_Move(o, x, y, 0, 0);
	Steps_Tap(o, button);
}

void Steps_Follow(OpenBGI_t* o, int x, int y)
{
	InputSteps_t* q = &o->input.steps;
	if(q->count > 0)
	{
		InputStep_t* last = &q->q[(q->head + q->count - 1) % INPUT_STEPS];
		if(last->kind == INPUT_STEP_MOVE && last->follow)
		{
			last->x = x;
			last->y = y;
			return;
		}
	}
	else if(o->input.pointerX == x && o->input.pointerY == y)
		return; // there already
	Steps_Move(o, x, y, 0, 1);
}

// the place of a step's button among the two a program reads itself: 0 left, 1 right, -1 another
static int ReadButton(int button)
{
	return button == OPENBGI_BUTTON_LEFT ? 0 : button == OPENBGI_BUTTON_RIGHT ? 1 : -1;
}

void Steps_ButtonRead(OpenBGI_t* o, int button)
{
	InputSteps_t* q = &o->input.steps;
	int b = ReadButton(button);
	if(b < 0)
		return;
	q->reads[b] = 1;
	q->readAt[b] = Clock_HostMs(o);
	q->unread[b] = 0;
}

// a step changed a button: a program that read it shortly before has that to read first
static void Changed(OpenBGI_t* o, int button)
{
	InputSteps_t* q = &o->input.steps;
	int b = ReadButton(button);
	if(b < 0)
		return;
	q->changedAt[b] = Clock_HostMs(o);
	q->unread[b] = q->reads[b] && q->changedAt[b] - q->readAt[b] <= STEPS_READER_MS;
}

// whether a step of `button` has to wait: the program has not read the step before, and is given time to
static int Unread(OpenBGI_t* o, int button)
{
	InputSteps_t* q = &o->input.steps;
	int b = ReadButton(button);
	if(b < 0 || !q->unread[b])
		return 0;
	if(Clock_HostMs(o) - q->changedAt[b] < STEPS_READ_MS)
		return 1;
	q->unread[b] = 0; // it reads the button no more
	return 0;
}

// one step through the calls a host's devices take; the host's pointer follows a move
static void Apply(OpenBGI_t* o, const InputStep_t* s)
{
	o->input.steps.lastAt = Clock_HostMs(o);
	o->input.steps.applying++;
	switch(s->kind)
	{
		case INPUT_STEP_MOVE:
			OpenBGI_Pointer(o, s->x, s->y);
			o->input.pointerMoved = 1; // (OpenBGI_Frame_t.pointerMoved)
			break;
		case INPUT_STEP_BUTTON:
			OpenBGI_Button(o, s->a, s->down, o->input.pointerX, o->input.pointerY, 0);
			Changed(o, s->a);
			break;
		case INPUT_STEP_WHEEL: OpenBGI_Wheel(o, s->a); break;
		case INPUT_STEP_KEY: OpenBGI_Key(o, s->a, s->down, 0); break;
	}
	o->input.steps.applying--;
}

void Steps_KeyNow(OpenBGI_t* o, int vk, int down)
{
	o->input.steps.applying++;
	OpenBGI_Key(o, vk, down, 0);
	o->input.steps.applying--;
}

void Steps_KeyTap(OpenBGI_t* o, int vk)
{
	Steps_KeyNow(o, vk, 1);
	Steps_KeyNow(o, vk, 0);
}

void Steps_TextNow(OpenBGI_t* o, const char* utf8)
{
	o->input.steps.applying++;
	OpenBGI_Text(o, utf8);
	o->input.steps.applying--;
}

int Steps_Pending(OpenBGI_t* o)
{
	return o->input.steps.count;
}

void Steps_Frame(OpenBGI_t* o)
{
	InputSteps_t* q = &o->input.steps;
	InputStep_t step;
	int applied = 0;
	while(q->count > 0)
	{
		InputStep_t* s = &q->q[q->head];
		if(s->gap > 0)
		{
			if(applied)
				break; // this frame's pass comes first
			if(--s->gap > 0)
				break; // (the pass of the frame before counted)
		}
		if(s->wait > 0 && Clock_HostMs(o) - q->lastAt < (uint32_t)s->wait)
			break; // the game's tap delay is not over
		if(s->kind == INPUT_STEP_BUTTON && Unread(o, s->a))
			break; // the program has yet to read what the step before did to the button
		step = *s;
		q->head = (q->head + 1) % INPUT_STEPS;
		q->count--;
		Apply(o, &step);
		applied = 1;
	}
}
