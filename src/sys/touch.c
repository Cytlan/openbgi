/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * touch.c - the input mapper's touch screen: the contacts the host reports
 *           (OpenBGI_Touch of openbgi.h) become the mouse the game knows
 *
 * The contacts make one gesture at a time (o->input.touch.mode):
 *
 *   ONE     a finger is down and has not moved beyond the slop: lifting it
 *           is a tap, holding it long enough a long press, a second finger
 *           soon enough makes it TWO, moving it POINT or KNOB
 *   POINT   the finger moves: once the gesture is older than a flick the
 *           pointer follows it; lifting it within a flick after a long
 *           enough, mostly vertical way is a swipe
 *   KNOB    the drag began on a knob's thumb: the left button is held and
 *           the pointer follows the finger (the thumb that much off it)
 *   TWO     two fingers, not moved: lifting one soon enough is a two-finger
 *           tap; moving them makes SCROLL
 *   SCROLL  two fingers moving: a wheel notch for every scroll step their
 *           middle moves up or down
 *   DONE    the gesture is over (or more fingers than two came): nothing
 *           more happens until every finger has lifted
 *
 * What a gesture does goes into the mapper's queue of steps (steps.c): a
 * click is the pointer moved, a pass, the press, a pass, the release.  A
 * tap is aimed with the targets of the input mapper (InputMap_TapPoint,
 * src/sys/inputmap.c).
 *
 * Distances are in millimetres of the screen, made the picture's pixels
 * with the host's scale (OpenBGI_TouchScale), or as fractions of the
 * picture's height without it.
 */
#include <stdlib.h>
#include <string.h>

#include "bgi/input.h"
#include "bgi/inputmap.h"
#include "bgi/sys.h"
#include "bgi/core.h"

#define SLOP_MM       3   // a tap may slip this far
#define SNAP_MM       5   // a tap beside a target lands on it from this near
#define SWIPE_MM      12  // a swipe is at least this long
#define SCROLL_MM     8   // two fingers move this far for a wheel notch
#define PICTURE_MM    75  // without the host's scale: the picture's height on the screen
#define LONG_MS       500 // a finger held this long without moving is a long press
#define FLICK_MS      300 // a swipe ends within this; a drag that lasts longer points
#define TWO_WINDOW_MS 200 // the second finger of a two-finger gesture comes within this
#define TWO_TAP_MS    400 // a two-finger tap is over within this
#define WHEEL_NOTCH   120 // WHEEL_DELTA

enum
{
	TOUCH_MODE_IDLE,
	TOUCH_MODE_ONE,
	TOUCH_MODE_POINT,
	TOUCH_MODE_KNOB,
	TOUCH_MODE_TWO,
	TOUCH_MODE_SCROLL,
	TOUCH_MODE_DONE
};

// `mm` millimetres of the screen in the picture's pixels (at least 1)
static int Px(OpenBGI_t* o, int mm)
{
	int px, h = o->screen.pic.h > 0 ? o->screen.pic.h : 600;
	if(o->input.touch.ppi > 0)
		px = (mm * o->input.touch.ppi * 10 + 127) / 254;
	else
		px = (mm * h + PICTURE_MM / 2) / PICTURE_MM;
	return px > 0 ? px : 1;
}

// ---- the gestures -----------------------------------------------------------------------------------

static int Find(TouchState_t* t, int id)
{
	int i;
	for(i = 0; i < TOUCH_CONTACTS; i++)
		if(t->c[i].active && t->c[i].id == id)
			return i;
	return -1;
}

static int Active(const TouchState_t* t)
{
	int i, n = 0;
	for(i = 0; i < TOUCH_CONTACTS; i++)
		n += t->c[i].active != 0;
	return n;
}

// contact i is further than the slop from where it went down
static int Moved(OpenBGI_t* o, const TouchContact_t* c)
{
	long dx = c->x - c->x0, dy = c->y - c->y0, s = Px(o, SLOP_MM);
	return dx * dx + dy * dy > s * s;
}

// the middle of the two contacts of the gesture, now (`now`) or where they went down
static void Middle(const TouchState_t* t, int now, int out[2])
{
	const TouchContact_t *a = &t->c[t->a], *b = &t->c[t->b];
	out[0] = now ? (a->x + b->x) / 2 : (a->x0 + b->x0) / 2;
	out[1] = now ? (a->y + b->y) / 2 : (a->y0 + b->y0) / 2;
}

// a tap at (x, y): aimed at the targets, noted for the debugger, clicked
static void Tap(OpenBGI_t* o, int x, int y)
{
	TouchState_t* t = &o->input.touch;
	int to[2];
	InputMap_TapPoint(o, x, y, Px(o, SNAP_MM), to, &t->tapKind, &t->tapOwner);
	t->tapValid = 1;
	t->tapSerial++;
	t->tapX = x;
	t->tapY = y;
	t->tapToX = to[0];
	t->tapToY = to[1];
	Steps_Click(o, OPENBGI_BUTTON_LEFT, to[0], to[1]);
}

// two fingers moved: a wheel notch for every scroll step their middle went down (away from the player) or up
static void Scroll(OpenBGI_t* o)
{
	TouchState_t* t = &o->input.touch;
	int m[2], step = Px(o, SCROLL_MM);
	Middle(t, 1, m);
	while(m[1] - t->scrollRef >= step)
	{
		Steps_Wheel(o, WHEEL_NOTCH);
		t->scrollRef += step;
	}
	while(t->scrollRef - m[1] >= step)
	{
		Steps_Wheel(o, -WHEEL_NOTCH);
		t->scrollRef -= step;
	}
}

static void Down(OpenBGI_t* o, int id, int x, int y)
{
	TouchState_t* t = &o->input.touch;
	uint32_t now = Clock_HostMs(o);
	int i = Find(t, id), at[2];
	if(i < 0)
		for(i = 0; i < TOUCH_CONTACTS && t->c[i].active; i++)
			;
	if(i >= TOUCH_CONTACTS)
		return; // more fingers than are followed
	if(t->c[i].active)
		return; // (the same contact down again: nothing new)
	memset(&t->c[i], 0, sizeof t->c[i]);
	t->c[i].id = id;
	t->c[i].active = 1;
	t->c[i].x0 = t->c[i].x = x;
	t->c[i].y0 = t->c[i].y = y;
	t->c[i].t0 = now;
	switch(t->mode)
	{
		case TOUCH_MODE_IDLE:
			t->mode = TOUCH_MODE_ONE;
			t->a = i;
			t->b = -1;
			t->t0 = now;
			t->knob = InputMap_KnobPoint(o, x, y, Px(o, SNAP_MM), at);
			t->knobDx = t->knob ? at[0] - x : 0;
			t->knobDy = t->knob ? at[1] - y : 0;
			break;
		case TOUCH_MODE_ONE:
			if(now - t->t0 <= TWO_WINDOW_MS && !Moved(o, &t->c[t->a]))
			{
				t->mode = TOUCH_MODE_TWO;
				t->b = i;
			}
			break;
		case TOUCH_MODE_TWO:
		case TOUCH_MODE_SCROLL: t->mode = TOUCH_MODE_DONE; break; // a third finger: nothing of this
		default: break;                                           // (pointing, a knob held: the finger is ignored)
	}
}

static void Move(OpenBGI_t* o, int id, int x, int y)
{
	TouchState_t* t = &o->input.touch;
	uint32_t now = Clock_HostMs(o);
	int i = Find(t, id), m[2];
	if(i < 0)
		return;
	t->c[i].x = x;
	t->c[i].y = y;
	switch(t->mode)
	{
		case TOUCH_MODE_ONE:
			if(i != t->a || !Moved(o, &t->c[i]))
				break;
			if(t->knob)
			{ // the thumb is taken where it is, and follows the finger
				t->mode = TOUCH_MODE_KNOB;
				Steps_Move(o, t->c[i].x0 + t->knobDx, t->c[i].y0 + t->knobDy, 0, 0);
				Steps_Button(o, OPENBGI_BUTTON_LEFT, 1);
				Steps_Follow(o, x + t->knobDx, y + t->knobDy);
				break;
			}
			t->mode = TOUCH_MODE_POINT;
			// fall through
		case TOUCH_MODE_POINT:
			if(i == t->a && now - t->t0 >= FLICK_MS)
				Steps_Follow(o, x, y);
			break;
		case TOUCH_MODE_KNOB:
			if(i == t->a)
				Steps_Follow(o, x + t->knobDx, y + t->knobDy);
			break;
		case TOUCH_MODE_TWO:
			if(i != t->a && i != t->b)
				break;
			if(!Moved(o, &t->c[t->a]) && !Moved(o, &t->c[t->b]))
				break;
			t->mode = TOUCH_MODE_SCROLL;
			Middle(t, 0, m);
			t->scrollRef = m[1];
			Scroll(o);
			break;
		case TOUCH_MODE_SCROLL:
			if(i == t->a || i == t->b)
				Scroll(o);
			break;
		default: break;
	}
}

static void Up(OpenBGI_t* o, int id, int cancel)
{
	TouchState_t* t = &o->input.touch;
	uint32_t now = Clock_HostMs(o);
	int i = Find(t, id), m[2];
	if(i < 0)
		return;
	t->c[i].active = 0;
	switch(t->mode)
	{
		case TOUCH_MODE_ONE:
			if(i != t->a)
				break;
			if(!cancel)
				Tap(o, t->c[i].x0, t->c[i].y0);
			t->mode = TOUCH_MODE_DONE;
			break;
		case TOUCH_MODE_POINT:
		{
			long dx = t->c[i].x - t->c[i].x0, dy = t->c[i].y - t->c[i].y0;
			if(i != t->a)
				break;
			if(!cancel && now - t->t0 <= FLICK_MS && labs(dy) >= Px(o, SWIPE_MM) && labs(dy) >= 2 * labs(dx))
				Steps_Wheel(o, dy > 0 ? WHEEL_NOTCH : -WHEEL_NOTCH); // a swipe down: away from the player (the backlog)
			t->mode = TOUCH_MODE_DONE;
			break;
		}
		case TOUCH_MODE_KNOB:
			if(i != t->a)
				break;
			Steps_Button(o, OPENBGI_BUTTON_LEFT, 0); // let go of the thumb where it went
			t->mode = TOUCH_MODE_DONE;
			break;
		case TOUCH_MODE_TWO:
			if(i != t->a && i != t->b)
				break;
			if(!cancel && now - t->t0 <= TWO_TAP_MS)
			{
				Middle(t, 0, m);
				Steps_Click(o, OPENBGI_BUTTON_RIGHT, m[0], m[1]);
			}
			t->mode = TOUCH_MODE_DONE;
			break;
		case TOUCH_MODE_SCROLL:
			if(i == t->a || i == t->b)
				t->mode = TOUCH_MODE_DONE;
			break;
		default: break;
	}
	if(!Active(t))
	{
		t->mode = TOUCH_MODE_IDLE;
		t->a = t->b = -1;
	}
}

void OpenBGI_Touch(OpenBGI_t* o, int id, int phase, int x, int y)
{
	if(phase == OPENBGI_TOUCH_DOWN)
		Input_DeviceUsed(o, INPUT_DEV_TOUCH);
	switch(phase)
	{
		case OPENBGI_TOUCH_DOWN: Down(o, id, x, y); break;
		case OPENBGI_TOUCH_MOVE: Move(o, id, x, y); break;
		case OPENBGI_TOUCH_UP: Up(o, id, 0); break;
		case OPENBGI_TOUCH_CANCEL: Up(o, id, 1); break;
		default: break;
	}
}

void OpenBGI_TouchScale(OpenBGI_t* o, int picturePixelsPerInch)
{
	o->input.touch.ppi = picturePixelsPerInch > 0 ? picturePixelsPerInch : 0;
}

/* The start of a frame that runs a pass, or of one while a dialog drawn
 * in the frame is up: a finger held long enough is a long press (the
 * right button where it went down), and a pointing finger that stood
 * still past the flick has the pointer brought to it. */
void Touch_Frame(OpenBGI_t* o)
{
	TouchState_t* t = &o->input.touch;
	uint32_t now = Clock_HostMs(o);
	if(t->mode == TOUCH_MODE_ONE && now - t->t0 >= LONG_MS)
	{
		Steps_Click(o, OPENBGI_BUTTON_RIGHT, t->c[t->a].x0, t->c[t->a].y0);
		t->mode = TOUCH_MODE_DONE;
	}
	else if(t->mode == TOUCH_MODE_POINT && now - t->t0 >= FLICK_MS)
		Steps_Follow(o, t->c[t->a].x, t->c[t->a].y);
}
