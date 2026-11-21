/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pad.c - the input mapper's pads and actions (OpenBGI_Pad and
 *         OpenBGI_Action of openbgi.h): a pad's controls become what the
 *         player means, and that the mouse and the keys the game knows
 *
 * A control that changed (the 32 buttons and the four directions of the
 * games' own pad keys, "81 1B") taps the key the game gave it, as the
 * original's joystick poll does, or does what it is bound to
 * (`o->input.pad.bind`: an action, or with a game's settings a key, a
 * mouse button or the wheel - InputMap_Configure, bgi/inputcfg.h).  An action goes to the on-screen keyboard while
 * that is up (osk.c), to the dialog drawn in the frame while one is
 * (as its keys: Tab, the arrows, Enter, Escape; DialogAction), else to
 * the game:
 *
 *   - CONFIRM and CANCEL are clicks, BACKLOG and FORWARD wheel notches,
 *     SKIP the skip key - steps of the mapper's queue (steps.c), a step a
 *     frame;
 *   - the directions move a focus over the targets of the input mapper
 *     (InputMap_Focusables: what a click reaches now) and put the pointer
 *     on the focused one; without targets they are the arrow keys;
 *   - the left stick moves the pointer, the right one turns the wheel
 *     (Pad_Frame, by the host's clock).
 *
 * `mode` is what the pad drove last and decides what CONFIRM does: go on
 * (nothing driven: a click where the screen takes one to go on; on a
 * screen that has only targets the focus shows first, as for a
 * direction; Enter where nothing takes a click), click the focus, or be
 * the left button under a stick's pointer.  The focus is kept by the target's kind, owner, group
 * and index and looked for again ten times a second: a target that moved
 * takes the pointer along, one that went takes the focus with it when
 * the screen has something to go on with, else the nearest target gets
 * it.
 *
 * On a host that shows no pointer (OPENBGI_CAP_POINTER) the engine draws
 * an arrow over the picture where the pointer is while a pad drives it
 * (Pad_Shown), as long as the game shows the system's cursor.  A mouse, a
 * keyboard or a touch the player takes up ends all of that
 * (Input_DeviceUsed); the settings may have the arrow drawn for a mouse
 * too - also over a dialog and the keyboard drawn in the picture, where
 * a mouse points and a pad does not - or never, and say its size
 * ("pointer", "pointerSize" of bgi/inputcfg.h).
 */
#include <stdlib.h>
#include <string.h>

#include "bgi/input.h"
#include "bgi/host.h"
#include "bgi/inputmap.h"
#include "bgi/inputcfg.h"
#include "bgi/inframe.h"
#include "bgi/edit.h"
#include "bgi/display.h"
#include "bgi/sys.h"
#include "bgi/core.h"

#define REPEAT_FIRST_MS 400  // a direction held this long repeats,
#define REPEAT_NEXT_MS  120  // .. this often
#define STICK_DEAD      6554 // a stick within a fifth of its travel is at rest
#define STICK_QUARTER   8192 // a game's own direction keys: the stick past a quarter of its travel
#define POINTER_MS      1100 // the pointer crosses the picture's height in this time with the stick at its end
#define WHEEL_PER_S     8    // wheel notches a second with the stick at its end
#define FOCUS_CHECK_MS  100  // the focus is looked for again this often
#define KNOB_STEPS      16   // a direction moves a knob this part of its travel
#define FOCUS_MAX       256  // targets looked at
#define WHEEL_NOTCH     120  // WHEEL_DELTA
#define NEAR            3    // a host's pointer this close to the pad's is the pad's (Pad_NearPointer)

void Pad_Init(OpenBGI_t* o)
{
	uint16_t* b = o->input.pad.bind;
	memset(b, 0, sizeof o->input.pad.bind);
	b[0] = OPENBGI_ACTION_CONFIRM;  // the bottom face button
	b[1] = OPENBGI_ACTION_CANCEL;   // the right one
	b[3] = OPENBGI_ACTION_KEYBOARD; // the top one
	b[4] = OPENBGI_ACTION_BACKLOG;  // L1
	b[5] = OPENBGI_ACTION_FORWARD;  // R1
	b[7] = OPENBGI_ACTION_CANCEL;   // START
	b[11] = OPENBGI_ACTION_SKIP;    // R2
	b[32] = OPENBGI_ACTION_UP;
	b[33] = OPENBGI_ACTION_DOWN;
	b[34] = OPENBGI_ACTION_LEFT;
	b[35] = OPENBGI_ACTION_RIGHT;
	o->input.pad.ownKeysOff = 0;
	o->input.pad.stickSpeed = 100;
	o->input.pad.arrowFor = INPUT_POINTER_PAD;
	o->input.pad.arrowSize = INPUT_POINTER_NORMAL;
}

void Pad_Stop(OpenBGI_t* o)
{
	PadMap_t* p = &o->input.pad;
	if(p->arrowShown)
		Screen_Overlay(o, SCREEN_OVERLAY_POINTER, NULL, 0, 0, 0, 0, 0, 0);
	p->arrowShown = 0;
	p->mode = PAD_MODE_ADVANCE;
	p->repeat = 0;
	p->leftHeld = p->rightHeld = 0;
	p->skipKey = 0;
}

// ---- which device -----------------------------------------------------------------------------

void Input_DeviceUsed(OpenBGI_t* o, int device)
{
	PadMap_t* p = &o->input.pad;
	if(device == INPUT_DEV_KEYBOARD)
		Osk_Close(o);
	if(device != INPUT_DEV_PAD && o->input.device == INPUT_DEV_PAD)
	{ // the pad's focus and pointer go with it
		p->mode = PAD_MODE_ADVANCE;
		p->repeat = 0;
	}
	if(device == INPUT_DEV_PAD && o->input.device != INPUT_DEV_PAD)
		Edit_ToFrame(o); // a text entry of the host's is out of a pad's reach: the engine's takes its place
	o->input.device = device;
}

int Pad_HasTurn(OpenBGI_t* o)
{
	return o->input.device == INPUT_DEV_PAD || (o->input.device == INPUT_DEV_NONE && o->input.pad.attached);
}

int Pad_OwnsPointer(OpenBGI_t* o)
{
	return o->input.device == INPUT_DEV_PAD && o->input.pad.mode != PAD_MODE_ADVANCE;
}

int Pad_NearPointer(OpenBGI_t* o, int x, int y)
{
	return Pad_OwnsPointer(o) && abs(x - o->input.pointerX) <= NEAR && abs(y - o->input.pointerY) <= NEAR;
}

const char* Pad_ModeName(OpenBGI_t* o)
{
	static const char* const names[] = {"nothing driven", "the focus", "the pointer"};
	return names[o->input.pad.mode <= PAD_MODE_POINTER ? o->input.pad.mode : 0];
}

// ---- the focus --------------------------------------------------------------------------------

// the focused target's index in t, -1 when it is not there
static int FindFocus(const PadMap_t* p, const OpenBGI_Target_t* t, int n)
{
	int i;
	for(i = 0; i < n; i++)
		if(t[i].kind == p->focusKind && t[i].owner == p->focusOwner && t[i].group == p->focusGroup && t[i].index == p->focusIndex)
			return i;
	return -1;
}

// how far the picture's pixel (x, y) lies from the rectangle (0 inside), squared
static long DistanceSq(const OpenBGI_Rect_t* r, int x, int y)
{
	long dx = 0, dy = 0;
	if(x < r->x)
		dx = r->x - x;
	else if(x >= r->x + r->w)
		dx = x - (r->x + r->w - 1);
	if(y < r->y)
		dy = r->y - y;
	else if(y >= r->y + r->h)
		dy = y - (r->y + r->h - 1);
	return dx * dx + dy * dy;
}

// the target nearest to the pointer; n > 0
static int Nearest(OpenBGI_t* o, const OpenBGI_Target_t* t, int n)
{
	int i, best = 0;
	long bestD = DistanceSq(&t[0].rect, o->input.pointerX, o->input.pointerY);
	for(i = 1; i < n; i++)
	{
		long d = DistanceSq(&t[i].rect, o->input.pointerX, o->input.pointerY);
		if(d < bestD)
		{
			best = i;
			bestD = d;
		}
	}
	return best;
}

/* where the focus first shows: the target under the pointer (the game
 * marks it already), else a panel's or a menu's selection, else the one
 * nearest to the pointer; n > 0 */
static int FirstFocus(OpenBGI_t* o, const OpenBGI_Target_t* t, int n)
{
	int i;
	for(i = 0; i < n; i++)
		if(t[i].flags & OPENBGI_TARGET_HOVER)
			return i;
	for(i = 0; i < n; i++)
		if(t[i].flags & OPENBGI_TARGET_SELECTED)
			return i;
	return Nearest(o, t, n);
}

/* the target next to t[cur] in the direction of `action`: the one whose
 * centre lies that way with the least of its distance along the way plus
 * twice its offset across it (none across when the two overlap there);
 * first among those within a cone of about 60 degrees, then among all
 * that lie that way; -1 for none */
static int Toward(const OpenBGI_Target_t* t, int n, int cur, int action)
{
	const OpenBGI_Rect_t* a = &t[cur].rect;
	int cx = a->x + a->w / 2, cy = a->y + a->h / 2, pass, i, best = -1;
	long bestScore = 0;
	for(pass = 0; pass < 2 && best < 0; pass++)
		for(i = 0; i < n; i++)
		{
			const OpenBGI_Rect_t* b = &t[i].rect;
			long x = b->x + b->w / 2, y = b->y + b->h / 2, along, across, score;
			int overlap;
			if(i == cur)
				continue;
			if(action == OPENBGI_ACTION_UP || action == OPENBGI_ACTION_DOWN)
			{
				along = action == OPENBGI_ACTION_UP ? cy - y : y - cy;
				across = labs(x - cx);
				overlap = b->x < a->x + a->w && a->x < b->x + b->w;
			}
			else
			{
				along = action == OPENBGI_ACTION_LEFT ? cx - x : x - cx;
				across = labs(y - cy);
				overlap = b->y < a->y + a->h && a->y < b->y + b->h;
			}
			if(along <= 0)
				continue;
			if(overlap)
				across = 0;
			if(pass == 0 && across > 2 * along)
				continue;
			score = along + 2 * across;
			if(best < 0 || score < bestScore)
			{
				best = i;
				bestScore = score;
			}
		}
	return best;
}

// the focus on target t: the pointer goes to where a click reaches it
static void SetFocus(OpenBGI_t* o, const OpenBGI_Target_t* t)
{
	PadMap_t* p = &o->input.pad;
	p->mode = PAD_MODE_FOCUS;
	p->focusKind = t->kind;
	p->focusOwner = t->owner;
	p->focusGroup = t->group;
	p->focusIndex = t->index;
	p->focusCheck = Clock_HostMs(o);
	Steps_Follow(o, t->clickX, t->clickY);
}

// the focused target into *out; 0 when there is none
static int FocusTarget(OpenBGI_t* o, OpenBGI_Target_t* out)
{
	OpenBGI_Target_t t[FOCUS_MAX];
	int n, i;
	if(o->input.pad.mode != PAD_MODE_FOCUS)
		return 0;
	n = InputMap_Focusables(o, t, FOCUS_MAX);
	i = FindFocus(&o->input.pad, t, n < FOCUS_MAX ? n : FOCUS_MAX);
	if(i < 0)
		return 0;
	*out = t[i];
	return 1;
}

/* a direction on a slider's knob along its travel: the thumb taken where
 * a click reaches it, moved a step of its travel and let go; 0 when the
 * direction is not the knob's */
static int KnobNudge(OpenBGI_t* o, const OpenBGI_Target_t* t, int action)
{
	int x = t->clickX, y = t->clickY, step;
	if(t->drag.w > t->rect.w && (action == OPENBGI_ACTION_LEFT || action == OPENBGI_ACTION_RIGHT))
	{
		step = t->drag.w / KNOB_STEPS > 0 ? t->drag.w / KNOB_STEPS : 1;
		x += action == OPENBGI_ACTION_LEFT ? -step : step;
	}
	else if(t->drag.h > t->rect.h && (action == OPENBGI_ACTION_UP || action == OPENBGI_ACTION_DOWN))
	{
		step = t->drag.h / KNOB_STEPS > 0 ? t->drag.h / KNOB_STEPS : 1;
		y += action == OPENBGI_ACTION_UP ? -step : step;
	}
	else
		return 0;
	Steps_Follow(o, t->clickX, t->clickY);
	Steps_Button(o, OPENBGI_BUTTON_LEFT, 1);
	Steps_Move(o, x, y, 1, 0);
	Steps_Button(o, OPENBGI_BUTTON_LEFT, 0);
	o->input.pad.focusCheck = Clock_HostMs(o);
	return 1;
}

// a direction: the focus shown, moved, or - without a target - the arrow key
static void Navigate(OpenBGI_t* o, int action)
{
	static const int kArrow[4] = {0x26, 0x28, 0x25, 0x27};
	PadMap_t* p = &o->input.pad;
	OpenBGI_Target_t t[FOCUS_MAX];
	int n = InputMap_Focusables(o, t, FOCUS_MAX), cur, next;
	if(n > FOCUS_MAX)
		n = FOCUS_MAX;
	if(n == 0)
	{
		if(p->mode == PAD_MODE_FOCUS)
			p->mode = PAD_MODE_ADVANCE;
		Steps_KeyTap(o, kArrow[action - OPENBGI_ACTION_UP]);
		return;
	}
	cur = p->mode == PAD_MODE_FOCUS ? FindFocus(p, t, n) : -1;
	if(cur < 0)
		next = FirstFocus(o, t, n); // the first press only shows it
	else if(t[cur].kind == OPENBGI_TARGET_KNOB && KnobNudge(o, &t[cur], action))
		return;
	else
		next = Toward(t, n, cur, action);
	if(next >= 0)
		SetFocus(o, &t[next]);
}

// nothing is focused: the focus shows where it first does; 0 when the screen has no target
static int Reveal(OpenBGI_t* o)
{
	OpenBGI_Target_t t[FOCUS_MAX];
	int n = InputMap_Focusables(o, t, FOCUS_MAX);
	if(n <= 0)
		return 0;
	SetFocus(o, &t[FirstFocus(o, t, n)]);
	return 1;
}

// the focus kept on its target (Pad_Frame)
static void Refocus(OpenBGI_t* o)
{
	PadMap_t* p = &o->input.pad;
	OpenBGI_Target_t t[FOCUS_MAX];
	int n = InputMap_Focusables(o, t, FOCUS_MAX), i, at[2];
	if(n > FOCUS_MAX)
		n = FOCUS_MAX;
	p->focusCheck = Clock_HostMs(o);
	i = FindFocus(p, t, n);
	if(i >= 0)
	{ // where it is now
		if(t[i].clickX != o->input.pointerX || t[i].clickY != o->input.pointerY)
			Steps_Follow(o, t[i].clickX, t[i].clickY);
	}
	else if(n == 0 || InputMap_AdvancePoint(o, at))
		p->mode = PAD_MODE_ADVANCE; // the screen went on: so does CONFIRM
	else
		SetFocus(o, &t[Nearest(o, t, n)]);
}

// ---- the actions ------------------------------------------------------------------------------

static void Confirm(OpenBGI_t* o, int down)
{
	PadMap_t* p = &o->input.pad;
	OpenBGI_Target_t t;
	int at[2];
	if(!down)
	{
		if(p->leftHeld)
			Steps_Button(o, OPENBGI_BUTTON_LEFT, 0);
		p->leftHeld = 0;
		return;
	}
	if(p->mode == PAD_MODE_FOCUS && !FocusTarget(o, &t))
	{ // the focused target went a moment ago: where the focus is now shows first
		Refocus(o);
		if(p->mode == PAD_MODE_FOCUS)
			return;
	}
	if(p->mode == PAD_MODE_POINTER)
	{ // a mouse's left button, held as long as the control is
		Steps_Button(o, OPENBGI_BUTTON_LEFT, 1);
		p->leftHeld = 1;
	}
	else if(FocusTarget(o, &t))
	{
		if(t.kind == OPENBGI_TARGET_ENTRY)
			o->input.osk.want = 1; // the click gives it the keys; then its keyboard
		Steps_Follow(o, t.clickX, t.clickY);
		Steps_Tap(o, OPENBGI_BUTTON_LEFT);
	}
	else if(o->input.osk.field)
		Osk_Open(o); // a text field has the keys: its keyboard
	else if(InputMap_AdvancePoint(o, at))
	{ // the screen goes on: where the pointer is when a click there does it, else where one does
		if(at[0] == o->input.pointerX && at[1] == o->input.pointerY && !Steps_Pending(o))
			Steps_Tap(o, OPENBGI_BUTTON_LEFT);
		else
			Steps_Click(o, OPENBGI_BUTTON_LEFT, at[0], at[1]);
	}
	else if(!Reveal(o))
		Steps_KeyTap(o, 0x0d); // nothing takes a click: Enter
}

static void Cancel(OpenBGI_t* o, int down)
{
	PadMap_t* p = &o->input.pad;
	int at[2];
	if(!down)
	{
		if(p->rightHeld)
			Steps_Button(o, OPENBGI_BUTTON_RIGHT, 0);
		p->rightHeld = 0;
		return;
	}
	if(p->mode == PAD_MODE_FOCUS && InputMap_AdvancePoint(o, at))
	{ // back to going on: the focus goes, and the pointer off its target
		p->mode = PAD_MODE_ADVANCE;
		Steps_Follow(o, at[0], at[1]);
		return;
	}
	Steps_Button(o, OPENBGI_BUTTON_RIGHT, 1);
	p->rightHeld = 1;
}

static void Skip(OpenBGI_t* o, int down)
{
	PadMap_t* p = &o->input.pad;
	if(down && !p->skipKey)
	{
		p->skipKey = (int)(o->input.skipKeys[0] & 0xff);
		if(!p->skipKey)
			p->skipKey = 0x11; // Ctrl
		Steps_KeyNow(o, p->skipKey, 1);
	}
	else if(!down && p->skipKey)
	{
		Steps_KeyNow(o, p->skipKey, 0);
		p->skipKey = 0;
	}
}

// the dialog's next control, or the one before: Tab, Shift+Tab
static void DialogTab(OpenBGI_t* o, int back)
{
	if(back)
		Steps_KeyNow(o, 0x10, 1);
	Steps_KeyTap(o, 0x09);
	if(back)
		Steps_KeyNow(o, 0x10, 0);
}

/* an action while a dialog drawn in the frame is up: its keys.  Up and
 * down step through a list's rows or a picker's values and go from
 * control to control otherwise; left and right go along a row of buttons
 * and from control to control otherwise; BACKLOG and FORWARD always go
 * from control to control. */
static void DialogAction(OpenBGI_t* o, int action, int down)
{
	int focus = Inframe_Focus(o);
	if(!down)
		return;
	switch(action)
	{
		case OPENBGI_ACTION_UP:
		case OPENBGI_ACTION_DOWN:
			if(focus == INFRAME_FOCUS_LIST || focus == INFRAME_FOCUS_PICKER)
				Steps_KeyTap(o, action == OPENBGI_ACTION_UP ? 0x26 : 0x28);
			else
				DialogTab(o, action == OPENBGI_ACTION_UP);
			break;
		case OPENBGI_ACTION_LEFT:
		case OPENBGI_ACTION_RIGHT:
			if(focus == INFRAME_FOCUS_BUTTON)
				Steps_KeyTap(o, action == OPENBGI_ACTION_LEFT ? 0x25 : 0x27);
			else
				DialogTab(o, action == OPENBGI_ACTION_LEFT);
			break;
		case OPENBGI_ACTION_BACKLOG: DialogTab(o, 1); break;
		case OPENBGI_ACTION_FORWARD: DialogTab(o, 0); break;
		case OPENBGI_ACTION_CONFIRM:
			if(o->input.osk.field)
				Osk_Open(o);
			else
				Steps_KeyTap(o, 0x0d);
			break;
		case OPENBGI_ACTION_KEYBOARD: Osk_Open(o); break;
		case OPENBGI_ACTION_CANCEL: Steps_KeyTap(o, 0x1b); break;
		default: break;
	}
}

// an action to whom it is for now: the keyboard, a dialog, the game
static void Dispatch(OpenBGI_t* o, int action, int down)
{
	PadMap_t* p = &o->input.pad;
	if(!down && ((action == OPENBGI_ACTION_CONFIRM && p->leftHeld) || (action == OPENBGI_ACTION_CANCEL && p->rightHeld) || (action == OPENBGI_ACTION_SKIP && p->skipKey)))
	{ // what the action holds for the game is let go, whoever has the turn by now
		if(action == OPENBGI_ACTION_CONFIRM)
			Confirm(o, 0);
		else if(action == OPENBGI_ACTION_CANCEL)
			Cancel(o, 0);
		else
			Skip(o, 0);
		return;
	}
	if(Osk_Up(o))
	{
		Osk_Action(o, action, down);
		return;
	}
	if(Inframe_Up(o))
	{
		DialogAction(o, action, down);
		return;
	}
	switch(action)
	{
		case OPENBGI_ACTION_CONFIRM: Confirm(o, down); break;
		case OPENBGI_ACTION_CANCEL: Cancel(o, down); break;
		case OPENBGI_ACTION_SKIP: Skip(o, down); break;
		case OPENBGI_ACTION_BACKLOG:
			if(down)
				Steps_Wheel(o, WHEEL_NOTCH);
			break;
		case OPENBGI_ACTION_FORWARD:
			if(down)
				Steps_Wheel(o, -WHEEL_NOTCH);
			break;
		case OPENBGI_ACTION_UP:
		case OPENBGI_ACTION_DOWN:
		case OPENBGI_ACTION_LEFT:
		case OPENBGI_ACTION_RIGHT:
			if(down)
				Navigate(o, action);
			break;
		case OPENBGI_ACTION_KEYBOARD:
			if(down)
				Osk_Open(o);
			break;
		default: break;
	}
}

// an action begins or ends; a direction held repeats (Pad_Frame)
static void Action(OpenBGI_t* o, int action, int down)
{
	PadMap_t* p = &o->input.pad;
	if(action >= OPENBGI_ACTION_UP && action <= OPENBGI_ACTION_RIGHT)
	{
		if(down)
		{
			p->repeat = action;
			p->repeatAt = Clock_HostMs(o) + REPEAT_FIRST_MS;
		}
		else if(p->repeat == action)
			p->repeat = 0;
	}
	Dispatch(o, action, down);
}

void OpenBGI_Action(OpenBGI_t* o, int action, int down)
{
	if(action < OPENBGI_ACTION_CONFIRM || action > OPENBGI_ACTION_KEYBOARD)
		return;
	if(action != OPENBGI_ACTION_SKIP && action != OPENBGI_ACTION_BACKLOG && action != OPENBGI_ACTION_FORWARD)
		Input_DeviceUsed(o, INPUT_DEV_PAD); // (those three leave a touch screen's or a mouse's turn alone)
	Action(o, action, down != 0);
}

// ---- the pads ---------------------------------------------------------------------------------

// control `ctl` (a button 0 .. 31, or 32 .. 35 a direction) went down or up
static void Control(OpenBGI_t* o, int ctl, int down)
{
	PadMap_t* p = &o->input.pad;
	uint32_t vk = p->ownKeysOff ? 0 : o->input.padKeys[ctl];
	if(vk && !Osk_Up(o) && !Inframe_Up(o))
	{ // the game's own key: a press and a release when the control goes down, as the original's joystick poll
		if(down && vk < 256)
			Steps_KeyTap(o, (int)vk);
		return;
	}
	switch(p->bind[ctl] & INPUT_BIND_KIND)
	{
		case 0:
			if(p->bind[ctl])
				Action(o, p->bind[ctl], down);
			break;
		case INPUT_BIND_KEY: Steps_KeyNow(o, p->bind[ctl] & 0xff, down); break; // held as long as the control is
		case INPUT_BIND_MOUSE: Steps_Button(o, p->bind[ctl] & 0xff, down); break;
		case INPUT_BIND_WHEEL:
			if(down)
				Steps_Wheel(o, p->bind[ctl] & 0xff ? -WHEEL_NOTCH : WHEEL_NOTCH);
			break;
		default: break;
	}
}

// the game gave a direction a key of its own: the left stick is directions too, as in the original
static int OwnDirections(OpenBGI_t* o)
{
	const uint32_t* k = o->input.padKeys;
	return !o->input.pad.ownKeysOff && (k[32] || k[33] || k[34] || k[35]);
}

void OpenBGI_Pad(OpenBGI_t* o, int port, const OpenBGI_PadState_t* state)
{
	PadMap_t* p = &o->input.pad;
	OpenBGI_PadState_t none;
	uint32_t buttons = 0, dirs = 0, was, changed;
	int i, lx = 0, ly = 0, ry = 0;
	if(port < 0 || port >= PAD_PORTS)
		return;
	memset(&none, 0, sizeof none);
	none.size = sizeof none;
	p->attached = state ? p->attached | 1u << port : p->attached & ~(1u << port);
	if(!state)
		state = &none;
	Host_Take(&p->port[port], sizeof p->port[port], state); // (what the host's state lacks is at rest)
	for(i = 0; i < PAD_PORTS; i++)
	{ // the pads as one: every button down, the stick that is furthest out
		const OpenBGI_PadState_t* s = &p->port[i];
		buttons |= s->buttons;
		dirs |= s->dpad & 15;
		if(abs(s->leftX) > abs(lx))
			lx = s->leftX;
		if(abs(s->leftY) > abs(ly))
			ly = s->leftY;
		if(abs(s->rightY) > abs(ry))
			ry = s->rightY;
	}
	p->lx = lx;
	p->ly = ly;
	p->ry = ry;
	if(OwnDirections(o))
	{
		dirs |= ly < -STICK_QUARTER ? OPENBGI_DPAD_UP : 0;
		dirs |= ly > STICK_QUARTER ? OPENBGI_DPAD_DOWN : 0;
		dirs |= lx < -STICK_QUARTER ? OPENBGI_DPAD_LEFT : 0;
		dirs |= lx > STICK_QUARTER ? OPENBGI_DPAD_RIGHT : 0;
	}
	if(buttons != p->buttons || dirs != p->dirs || abs(lx) > STICK_DEAD || abs(ly) > STICK_DEAD || abs(ry) > STICK_DEAD)
		Input_DeviceUsed(o, INPUT_DEV_PAD);
	// what went up first, then what went down
	was = p->buttons;
	p->buttons = buttons;
	changed = was ^ buttons;
	for(i = 0; i < 32; i++)
		if((changed >> i) & 1 && !((buttons >> i) & 1))
			Control(o, i, 0);
	for(i = 0; i < 32; i++)
		if((changed >> i) & 1 && ((buttons >> i) & 1))
			Control(o, i, 1);
	was = p->dirs;
	p->dirs = dirs;
	changed = was ^ dirs;
	for(i = 0; i < 4; i++)
		if((changed >> i) & 1 && !((dirs >> i) & 1))
			Control(o, 32 + i, 0);
	for(i = 0; i < 4; i++)
		if((changed >> i) & 1 && ((dirs >> i) & 1))
			Control(o, 32 + i, 1);
}

// ---- once a frame -----------------------------------------------------------------------------

// how far a stick at `v` is out of its rest, 0 .. 65536, squared so that a little is little
static int32_t Deflection(int v)
{
	int32_t a = abs(v), f;
	if(a <= STICK_DEAD)
		return 0;
	f = (int32_t)(((int64_t)(a - STICK_DEAD) << 16) / (32767 - STICK_DEAD));
	if(f > 65536)
		f = 65536;
	f = (int32_t)(((int64_t)f * f) >> 16);
	return v < 0 ? -f : f;
}

// v kept within 0 .. max
static int Within(int v, int max)
{
	if(v < 0)
		return 0;
	return v > max ? max : v;
}

// the sticks over `dt` ms: the left one moves the pointer, the right one turns the wheel
static void Sticks(OpenBGI_t* o, uint32_t dt)
{
	PadMap_t* p = &o->input.pad;
	int32_t fx = Deflection(p->lx), fy = Deflection(p->ly), fw = Deflection(p->ry);
	int w = o->screen.pic.w, h = o->screen.pic.h, dx, dy, x, y;
	if(!o->screen.pic.pixels || w <= 0 || h <= 0)
		return;
	if((fx || fy) && !OwnDirections(o))
	{ // the picture's height in POINTER_MS at the stick's end, at the usual speed
		int speed = p->stickSpeed > 0 ? p->stickSpeed : 100;
		p->fracX += (int32_t)((int64_t)fx * h * (int32_t)dt * speed / (POINTER_MS * 100));
		p->fracY += (int32_t)((int64_t)fy * h * (int32_t)dt * speed / (POINTER_MS * 100));
		dx = p->fracX / 65536;
		dy = p->fracY / 65536;
		p->fracX -= dx * 65536;
		p->fracY -= dy * 65536;
		if(dx || dy)
		{
			x = Within(o->input.pointerX + dx, w - 1);
			y = Within(o->input.pointerY + dy, h - 1);
			p->mode = PAD_MODE_POINTER;
			Steps_Follow(o, x, y);
		}
	}
	else
		p->fracX = p->fracY = 0;
	if(fw)
	{ // up is away from the player
		p->wheelFrac += (int32_t)((int64_t)fw * WHEEL_PER_S * (int32_t)dt / 1000);
		while(p->wheelFrac >= 65536)
		{
			Steps_Wheel(o, -WHEEL_NOTCH);
			p->wheelFrac -= 65536;
		}
		while(p->wheelFrac <= -65536)
		{
			Steps_Wheel(o, WHEEL_NOTCH);
			p->wheelFrac += 65536;
		}
	}
	else
		p->wheelFrac = 0;
}

void Pad_Frame(OpenBGI_t* o)
{
	PadMap_t* p = &o->input.pad;
	uint32_t now = Clock_HostMs(o), dt = now - p->stickAt;
	p->stickAt = now;
	if(p->repeat && (int32_t)(now - p->repeatAt) >= 0)
	{
		Dispatch(o, p->repeat, 1);
		p->repeatAt = now + REPEAT_NEXT_MS;
	}
	if(o->input.device != INPUT_DEV_PAD)
		return;
	if(dt > 0 && !Osk_Up(o) && !Inframe_Up(o))
		Sticks(o, dt > 50 ? 50 : dt); // (a long frame is not a long way)
	if(p->mode == PAD_MODE_FOCUS && now - p->focusCheck >= FOCUS_CHECK_MS && !Steps_Pending(o))
		Refocus(o);
}

// ---- the pointer the engine draws -------------------------------------------------------------

/* The arrow's outline, in tenths of the 19 parts its height has: the tip,
 * the wing's end, the tail's four corners, the left edge's foot. */
#define ARROW_UNITS 190
#define ARROW_SUB   4  // samples a pixel, each way
#define ARROW_FIX   16 // a pixel in the units the samples are counted in
static const int16_t kArrow[7][2] = {{0, 0}, {118, 118}, {69, 118}, {94, 176}, {70, 187}, {44, 129}, {0, 170}};

// how high the arrow is drawn on a picture `picH` high
static int ArrowHeight(const PadMap_t* p, int picH)
{
	int h = picH / 36;
	if(h < 16)
		h = 16;
	if(p->arrowSize == INPUT_POINTER_SMALL)
		h = h * 3 / 4;
	else if(p->arrowSize == INPUT_POINTER_LARGE)
		h = h * 3 / 2;
	return h > PAD_ARROW_MAX_H ? PAD_ARROW_MAX_H : h;
}

/* The arrow `height` pixels high into p->arrow: white with a black edge
 * a twenty-fourth of its height wide (whole pixels, one at least), its
 * tip at the picture's first pixel, smooth at its edges (what a pixel has
 * of the arrow is its alpha, what that has of the edge its darkness). */
static void DrawArrow(PadMap_t* p, int height)
{
	int32_t pt[7][2];
	int64_t edge = (int64_t)(height / 24) * ARROW_FIX;
	int w = (height * 118 + ARROW_UNITS - 1) / ARROW_UNITS + 1, x, y, i, sx, sy;
	if(w > PAD_ARROW_MAX_W)
		w = PAD_ARROW_MAX_W;
	if(edge < ARROW_FIX)
		edge = ARROW_FIX;
	for(i = 0; i < 7; i++)
	{
		pt[i][0] = kArrow[i][0] * height * ARROW_FIX / ARROW_UNITS;
		pt[i][1] = kArrow[i][1] * height * ARROW_FIX / ARROW_UNITS;
	}
	for(y = 0; y < height; y++)
		for(x = 0; x < w; x++)
		{
			int in = 0, white = 0;
			for(sy = 0; sy < ARROW_SUB; sy++)
				for(sx = 0; sx < ARROW_SUB; sx++)
				{
					int32_t px = x * ARROW_FIX + ARROW_FIX / ARROW_SUB / 2 + sx * (ARROW_FIX / ARROW_SUB);
					int32_t py = y * ARROW_FIX + ARROW_FIX / ARROW_SUB / 2 + sy * (ARROW_FIX / ARROW_SUB);
					int inside = 0, far = 1;
					for(i = 0; i < 7; i++)
					{
						const int32_t *a = pt[i], *b = pt[(i + 1) % 7];
						int64_t dx = b[0] - a[0], dy = b[1] - a[1], qx = px - a[0], qy = py - a[1];
						int64_t along = qx * dx + qy * dy, len = dx * dx + dy * dy, cross = qx * dy - qy * dx, d2;
						if((a[1] > py) != (b[1] > py) && (cross > 0) == (dy > 0))
							inside = !inside; // the sample's row crosses this side to its right
						if(along <= 0)
							d2 = qx * qx + qy * qy;
						else if(along >= len)
							d2 = (px - b[0]) * (int64_t)(px - b[0]) + (py - b[1]) * (int64_t)(py - b[1]);
						else
							d2 = cross * cross / len;
						if(d2 < edge * edge)
							far = 0;
					}
					in += inside;
					white += inside && far;
				}
			if(in)
			{
				uint32_t alpha = (uint32_t)(in * 255 / (ARROW_SUB * ARROW_SUB)), grey = (uint32_t)(white * 255 / in);
				p->arrow[y * w + x] = alpha << 24 | grey << 16 | grey << 8 | grey;
			}
			else
				p->arrow[y * w + x] = 0;
		}
	p->arrowW = w;
	p->arrowH = height;
	p->arrowFrom = height;
}

void Pad_Shown(OpenBGI_t* o)
{
	PadMap_t* p = &o->input.pad;
	int height = ArrowHeight(p, o->screen.pic.h);
	int byMouse = p->arrowFor == INPUT_POINTER_ALWAYS && o->input.device == INPUT_DEV_MOUSE;                       // a mouse the host shows no pointer for
	int over = Osk_Up(o) || Inframe_Up(o);                                                                         // the keyboard or a dialog in the picture: a pad goes from key to key there, a mouse points
	int games = o->cursor.cursorShown && Cursor_IsVisible(o) && !o->cursor.curSprite && o->cursor.cursorShape < 2; // the game shows the system's arrow
	int show = p->arrowFor != INPUT_POINTER_NEVER && !(o->host.caps & OPENBGI_CAP_POINTER) && o->screen.pic.pixels &&
		(byMouse ? over || games : Pad_OwnsPointer(o) && !over && games);
	if(!show)
	{
		if(p->arrowShown)
			Screen_Overlay(o, SCREEN_OVERLAY_POINTER, NULL, 0, 0, 0, 0, 0, 0);
		p->arrowShown = 0;
		return;
	}
	if(p->arrowShown && height == p->arrowFrom && p->arrowX == o->input.pointerX && p->arrowY == o->input.pointerY)
		return;
	if(height != p->arrowFrom)
		DrawArrow(p, height);
	p->arrowShown = 1;
	p->arrowX = o->input.pointerX;
	p->arrowY = o->input.pointerY;
	Screen_Overlay(o, SCREEN_OVERLAY_POINTER, p->arrow, p->arrowW, p->arrowX, p->arrowY, p->arrowW, p->arrowH, 1);
}
