/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * input.c - keyboard / mouse state, input layers, standard keys;
 *           interface in bgi/input.h
 *
 * The window's events (window.c) feed key presses and releases into a
 * table of 256 records, one per virtual-key code.  Scripts never see raw
 * messages: they poll "triggers" (a press that has not been consumed yet,
 * "80 1D"), counters (presses since the state was last cleared, "80 12" /
 * "80 1C") and the standard-key groups (several virtual keys folded into
 * one bit of the "80 1A" mask).
 *
 * Two lists of "layers" arbitrate who may read input.  Every layer carries
 * a 32-bit key, lists are sorted by descending key and only the layer with
 * the highest key counts:
 *   * key layers   - a script thread reads keys when its layer is on top
 *                    (KeyLayer_IsTop);
 *   * mouse layers - rectangles (or sprites); the cursor hits the layer
 *                    with the highest key whose rectangle contains it, and
 *                    higher layers that contain the cursor shadow lower ones
 *                    (MouseLayer_Hit).
 * "80 18" / "80 19" push and pop a layer on both lists at once, the wait
 * objects do the same for their lifetime, and the scheduler polls the
 * base layer (key 1) every pass.
 * Besides the real virtual-key codes the engine uses 0x0e / 0x0f for mouse
 * wheel up / down and 0xc1 .. 0xc4 for groups of keys (see GetKeyTrigger).
 */
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/error.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/gfx.h"
#include "bgi/core.h"

// -------------------------------------------------------------------------
// key state table (256 x 20 bytes)
// -------------------------------------------------------------------------

#define KEY_REPEAT_DELAY 500 // ms before auto-repeat

// virtual-key codes are 0..255; anything else is a script error
static void CheckKeyCode(OpenBGI_t* o, int vk)
{
	if(vk < 0 || vk > 0xff)
		ThrowScriptError(o, MSG_BAD_VIRTUAL_KEY);
}

uint32_t InputSerial_Get(OpenBGI_t* o)
{
	return o->input.inputSerial;
}

void InputSerial_Bump(OpenBGI_t* o)
{
	o->input.inputSerial++;
}

void Set_InputEnable(OpenBGI_t* o, int on)
{
	o->input.inputEnable = on;
}

void Set_SkipKeyEnable(OpenBGI_t* o, int on)
{
	o->input.skipKeyEnable = on;
}

void Set_SkipLatch(OpenBGI_t* o, int on)
{
	o->input.skipLatch = on;
}

int Input_SwapButtons(OpenBGI_t* o)
{
	return o->input.swapButtons;
}

// "80 1E": 0 or 1; returns 1 when accepted, 0 for any other value
int Input_SetSwapButtons(OpenBGI_t* o, uint32_t v)
{
	if(v > 1)
		return 0;
	o->input.swapButtons = (int)v;
	return 1;
}

/*
 * The asynchronous state of a key ("80 11"), with the left / right mouse
 * buttons exchanged when the script asked for it: the left button (1)
 * then reads either button, the right button (2) reads key 0, which is
 * never down - it acts as the left one.  Returns the GetAsyncKeyState
 * value: bit 15 = down.  The state is what the host reported
 * (Input_KeyDown); the original reads the physical buttons and exchanges
 * them itself when the system has them swapped, which the host's report
 * of the primary button as the left one already is.  From 1.529 on
 * nothing is down while the window is inactive, unless "81 14" allows
 * it.
 */
int GetKeyStateEx(OpenBGI_t* o, int vk)
{
	int second = 0, r;
	if(vk == 1 && o->input.swapButtons == 1)
		second = 2;
	else if(vk == 2 && o->input.swapButtons == 1)
		vk = 0;
	r = Input_KeyDown(o, vk) ? 0x8000 : 0;
	if(second)
		r |= Input_KeyDown(o, second) ? 0x8000 : 0;
	// 1.529 on: nothing while the window is inactive, unless "81 14" allows it
	if(o->engine.profile->gen >= GEN_1_529 && !Window_IsActive(o) && !o->input.keysWhenInactive)
		return 0;
	return r;
}

// "81 14" of 1.529 on
void Input_SetKeysWhenInactive(OpenBGI_t* o, int on)
{
	o->input.keysWhenInactive = on;
}

/* "81 10" of 1.69 build 472 on: set the flag that makes the keyboard's
 * auto-repeat of key vk count as new presses (Input_OnKeyDown releases
 * the key before each press); the old value is returned. */
int Key_SetRepeatNew(OpenBGI_t* o, int vk, int on)
{
	int old;
	CheckKeyCode(o, vk);
	old = o->input.keyState[vk].repeatNew;
	o->input.keyState[vk].repeatNew = on;
	return old;
}

/* A press from the window's events; 1 when it was a fresh press (the key
 * was up), 0 for the keyboard's auto-repeat of a held key, which changes
 * nothing.  The mouse buttons (1, 2, 4, 5, 6) report every click to
 * GetKeyTrigger, a key only its first press while held.  `repeat` is not
 * used: a held key is recognised from the table. */
int Input_OnKeyDown(OpenBGI_t* o, int vk, int repeat)
{
	KeyRec_t* k;
	int fresh;
	BGI_UNUSED(repeat);
	CheckKeyCode(o, vk);
	k = &o->input.keyState[vk];
	if(k->repeatNew)
		Input_OnKeyUp(o, vk);
	fresh = k->down == 0;
	if(vk == 1 || vk == 2 || vk == 4 || vk == 5 || vk == 6)
	{
		k->consumed = 0; // buttons report every click
	}
	else if(k->down)
	{
		k->down = 1; // key auto-repeat: nothing new
		return fresh;
	}
	k->pending++;
	k->counter++;
	k->repeatAt = GetTicks(o) + KEY_REPEAT_DELAY;
	k->down = 1;
	return fresh;
}

// A release from the window's events.
void Input_OnKeyUp(OpenBGI_t* o, int vk)
{
	CheckKeyCode(o, vk);
	o->input.keyState[vk].down = 0;
	o->input.keyState[vk].consumed = 0;
}

// Count a press without a state change (a button click that went to a knob).
void Input_CountPress(OpenBGI_t* o, int vk)
{
	CheckKeyCode(o, vk);
	o->input.keyState[vk].counter++;
}

// a held key past its auto-repeat delay (the panels' hot keys)
int Key_IsRepeating(OpenBGI_t* o, int vk)
{
	const KeyRec_t* k = &o->input.keyState[vk];
	if(!k->down)
		return 0;
	return GetTicks(o) >= k->repeatAt;
}

/*
 * The pending presses of a key, consumed by the call.  Bit 31 is set
 * when the key is still held and this is the first report of the press.
 * 0xc1 .. 0xc4 stand for the groups below (decide, cancel, up + wheel
 * up, down + wheel down) and sum their members.  A key whose release
 * message was lost (held in the table, up in the system) is released
 * first.
 */
int GetKeyTrigger(OpenBGI_t* o, int vk)
{
	const uint32_t* const groups[4] = {o->input.stdKeys[STDKEY_DECIDE], o->input.stdKeys[STDKEY_CANCEL], o->input.wheelUpKeys, o->input.wheelDownKeys};
	KeyRec_t* k;
	uint32_t r;
	CheckKeyCode(o, vk);
	k = &o->input.keyState[vk];
	if(k->down && !(((uint32_t)GetKeyStateEx(o, vk) >> 15) & 1))
		Input_OnKeyUp(o, vk); // the release message was lost

	if(vk < 0xc1)
	{
		r = (uint32_t)k->pending;
		if(k->down && !k->consumed)
		{
			r |= 0x80000000u;
			k->consumed = 1;
		}
		k->pending = 0;
		return (int)r;
	}
	else
	{
		const uint32_t* list = vk - 0xc1 < 4 ? groups[vk - 0xc1] : NULL;
		uint32_t held = 0, sum = 0;
		if(list)
		{
			for(; *list; list++)
			{
				uint32_t t = (uint32_t)GetKeyTrigger(o, (int)*list);
				if(t)
				{
					held |= t & 0x80000000u;
					sum += t & 0x7fffffffu;
				}
			}
		}
		return (int)(held | sum);
	}
}

int GetKeyCounter(OpenBGI_t* o, int vk)
{
	return o->input.keyState[vk].counter;
}

// Forget everything, the counters included (the engine's reset).
void KeyState_Clear(OpenBGI_t* o)
{
	memset(o->input.keyState, 0, sizeof o->input.keyState);
}

// Forget the pressed state and the pending presses of every key; the counters stay.
void Input_ClearStates(OpenBGI_t* o)
{
	int i;
	for(i = 0; i < 256; i++)
	{
		o->input.keyState[i].down = 0;
		o->input.keyState[i].consumed = 0;
		o->input.keyState[i].pending = 0;
		o->input.keyState[i].repeatAt = 0;
	}
}

// -------------------------------------------------------------------------
// standard keys
// -------------------------------------------------------------------------

// the 24 key lists in mask-bit order; each list is 0-terminated

// the mask bit of each list, in the same order
static const uint32_t gStdKeyBits[STDKEY_COUNT] = {
	0x1, 0x2, 0x4, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x1000, 0x2000,
	0x4000, 0x8000, 0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000,
	0x400000, 0x800000, 0x1000000, 0x2000000, 0x40000000};

/* "80 1B": replace the keys of one of the assignable groups, named by its
 * mask bit (0x40 .. 0x8000; from 1.529 on also 0x40000000 Tab and
 * 0x80000000 the skip keys) with a 0-terminated list of at most 15.
 * 0 ok, 0x80000001 unknown group, 0x80000002 too many keys. */
uint32_t SetStandardKey(OpenBGI_t* o, uint32_t group, const uint32_t* keys)
{
	int n = 0, slot;
	while(keys[n])
		n++;
	if(n >= 0x10)
		return 0x80000002u;
	switch(group)
	{
		case 0x40: slot = STDKEY_WHEEL_UP; break;
		case 0x80: slot = STDKEY_WHEEL_DOWN; break;
		case 0x100: slot = STDKEY_DECIDE; break;
		case 0x200: slot = STDKEY_CANCEL; break;
		case 0x1000: slot = STDKEY_UP; break;
		case 0x2000: slot = STDKEY_DOWN; break;
		case 0x4000: slot = STDKEY_LEFT; break;
		case 0x8000: slot = STDKEY_RIGHT; break;
		case 0x40000000: // 1.529 on: Tab and the skip keys too
			if(o->engine.profile->gen < GEN_1_529)
				return 0x80000001u;
			slot = STDKEY_TAB;
			break;
		case 0x80000000:
			if(o->engine.profile->gen < GEN_1_529)
				return 0x80000001u;
			memcpy(o->input.skipKeys, keys, (size_t)(n + 1) * sizeof(uint32_t));
			return 0;
		default: return 0x80000001u;
	}
	memcpy(o->input.stdKeys[slot], keys, (size_t)(n + 1) * sizeof(uint32_t));
	return 0;
}

// the trigger mask of the key groups (bits 6 .. 30); the triggers are consumed
static uint32_t StdKeys_PollTriggers(OpenBGI_t* o)
{
	uint32_t mask = 0;
	int g;
	for(g = STDKEY_WHEEL_UP; g < STDKEY_COUNT; g++)
	{
		const uint32_t* k;
		for(k = o->input.stdKeys[g]; *k; k++)
		{
			if(GetKeyTrigger(o, (int)*k))
			{ // stops at the first hit: the other members keep their triggers
				mask |= gStdKeyBits[g];
				break;
			}
		}
	}
	return mask;
}

/* the standard-key mask the tween waits break on, 0x2000 (down) in the
 * reference; "81 1F" of 1.69 build 472 on makes it configurable
 * (or'ed with 0x180, wheel down and decide, by the waits) */
void Input_SetTweenKeyMask(OpenBGI_t* o, uint32_t mask)
{
	o->input.tweenKeyMask = mask;
}
uint32_t Input_TweenKeyMask(OpenBGI_t* o)
{
	return o->input.tweenKeyMask | 0x180;
}

/* 1.547 on ("81 1B"): the keys of a gamepad's controls - the buttons 0 ..
 * 31 and the directions up, down, left, right as 32 .. 35 (the hat, or
 * the stick past a quarter of its travel).  The original reads a
 * DirectInput joystick and taps the key of a control (a press and a
 * release) when it goes down; 0 is none.  No host hands a gamepad over
 * yet, so the table is only kept. */

// 0 ok, 0x80000002 an index past the table
uint32_t Pad_SetKey(OpenBGI_t* o, uint32_t index, uint32_t vk)
{
	if(index >= PAD_CONTROLS)
		return 0x80000002;
	o->input.padKeys[index] = vk;
	return 0;
}

// "80 1C": the counters of every key in the groups of `mask`, summed
int CountKeysByMask(OpenBGI_t* o, uint32_t mask)
{
	int g, sum = 0;
	for(g = 0; g < STDKEY_COUNT; g++)
	{
		const uint32_t* k;
		if(!(mask & gStdKeyBits[g]))
			continue;
		for(k = o->input.stdKeys[g]; *k; k++)
			sum += GetKeyCounter(o, (int)*k);
	}
	return sum;
}

/* the bit of an "80 1A" mask that a virtual key belongs to, by the
 * default assignment (not the "80 1B" lists); 0 for a key in no group */
int StdKey_MaskBit(uint32_t mask, int vk)
{
	static const struct
	{
		int vk;
		int shift;
	} map[] = {
		{0x01, 0}, {0x02, 1}, {0x04, 2}, {0x05, 4}, {0x06, 5},
		{0x0e, 6}, {0x0f, 7}, {0x0d, 8}, {0x20, 9},
		{0x26, 12}, {0x28, 13}, {0x25, 14}, {0x27, 15},
		{0x31, 16}, {0x32, 17}, {0x33, 18}, {0x34, 19}, {0x35, 20},
		{0x36, 21}, {0x37, 22}, {0x38, 23}, {0x39, 24}, {0x30, 25},
		{0x61, 16}, {0x62, 17}, {0x63, 18}, {0x64, 19}, {0x65, 20},
		{0x66, 21}, {0x67, 22}, {0x68, 23}, {0x69, 24}, {0x60, 25},
		{0x09, 30}, {0xc1, 8}, {0xc2, 9}};
	size_t i;
	for(i = 0; i < BGI_COUNTOF(map); i++)
		if(map[i].vk == vk)
			return (int)((mask >> map[i].shift) & 1);
	return 0;
}

// -------------------------------------------------------------------------
// skip
// -------------------------------------------------------------------------

// "80 16": forget the pending state of the skip keys; a skip key still held does not count until released
void Input_Flush(OpenBGI_t* o)
{
	const uint32_t* k;
	o->input.skipFlushed = 1;
	for(k = o->input.skipKeys; *k; k++)
		GetKeyTrigger(o, (int)*k);
}

/*
 * "80 17": is the "skip" function active?  A skip key (Ctrl by default)
 * held while the window is active and not minimised, or the latch set by
 * "80 15".  After Input_Flush only a new press counts until every skip
 * key has been released.  Requires the skip-key switch ("80 14") and
 * input to be enabled ("80 10").
 */
int Input_CheckSkip(OpenBGI_t* o)
{
	int latched = o->input.skipLatch;
	int anyHeld = 0;
	const uint32_t* k;

	if(Window_IsActive(o) && o->input.inputEnable && o->input.skipKeys[0])
	{
		for(k = o->input.skipKeys; *k; k++)
		{
			int down = GetKeyStateEx(o, (int)*k) & 0x8000;
			if(down)
				anyHeld = 1;
			if(o->input.skipFlushed)
			{
				if(GetKeyTrigger(o, (int)*k) && Window_IsRestored(o))
					latched = 1;
			}
			else if(down && Window_IsRestored(o))
			{
				return o->input.skipKeyEnable ? 1 : 0;
			}
		}
	}
	if(o->input.skipFlushed && !anyHeld)
		o->input.skipFlushed = 0;
	if(latched && o->input.skipKeyEnable)
		return 1;
	return 0;
}

// -------------------------------------------------------------------------
// layers
// -------------------------------------------------------------------------
typedef struct InputLayer
{
	uint32_t key;            // priority key
	int32_t l, t, r, b;      // rectangle (mouse layers without a sprite), inclusive
	DispObj_t* sprite;       // sprite whose screen rectangle the layer is, or NULL
	struct InputLayer* next; // lower keys follow
} InputLayer_t;

// the default mouse layer covers every coordinate
static const int32_t gFullRect[4] = {INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX};

// free a whole list
static void Layers_Clear(InputLayer_t** head)
{
	while(*head)
	{
		InputLayer_t* n = (*head)->next;
		BGI_Free(*head);
		*head = n;
	}
}

// insert before the first layer whose key is not above `key` (so a new layer goes above its equals)
static void Layer_Push(InputLayer_t** head, uint32_t key, const int32_t rect[4], DispObj_t* sprite)
{
	InputLayer_t* n = (InputLayer_t*)BGI_Alloc(sizeof(InputLayer_t));
	InputLayer_t** link = head;
	n->key = key;
	n->l = rect[0];
	n->t = rect[1];
	n->r = rect[2];
	n->b = rect[3];
	n->sprite = sprite;
	while(*link && (*link)->key > key)
		link = &(*link)->next;
	n->next = *link;
	*link = n;
}

// remove the first (highest) layer with that key; 1 when there was one
static int Layer_PopKey(InputLayer_t** head, uint32_t key)
{
	for(; *head; head = &(*head)->next)
	{
		if((*head)->key == key)
		{
			InputLayer_t* n = *head;
			*head = n->next;
			BGI_Free(n);
			return 1;
		}
	}
	return 0;
}

// remove the first layer owned by that sprite; 1 when there was one
static int Layer_PopSprite(InputLayer_t** head, DispObj_t* s)
{
	for(; *head; head = &(*head)->next)
	{
		if((*head)->sprite == s)
		{
			InputLayer_t* n = *head;
			*head = n->next;
			BGI_Free(n);
			return 1;
		}
	}
	return 0;
}

// a mouse layer covering every coordinate
void MouseLayer_Push(OpenBGI_t* o, uint32_t key)
{
	Layer_Push(&o->input.mouseLayers, key, gFullRect, NULL);
}

// a key layer (its rectangle is unused)
void KeyLayer_Push(OpenBGI_t* o, uint32_t key)
{
	static const int32_t zero[4] = {0, 0, 0, 0};
	Layer_Push(&o->input.keyLayers, key, zero, NULL);
}

/* a mouse layer that follows a sprite (its key is the sprite's input
 * priority, its rectangle is refreshed on every hit test) */
void MouseLayer_PushSprite(OpenBGI_t* o, DispObj_t* s)
{
	static const int32_t zero[4] = {0, 0, 0, 0};
	Layer_Push(&o->input.mouseLayers, s->vt->sortKey(o, s), zero, s);
}

void MouseLayer_Pop(OpenBGI_t* o, uint32_t key)
{
	Layer_PopKey(&o->input.mouseLayers, key);
}

void KeyLayer_Pop(OpenBGI_t* o, uint32_t key)
{
	Layer_PopKey(&o->input.keyLayers, key);
}

void MouseLayer_PopSprite(OpenBGI_t* o, DispObj_t* s)
{
	Layer_PopSprite(&o->input.mouseLayers, s);
}

// Clear both lists and push the base layer (key 1) on each (the engine's reset and shutdown).
void Input_ResetLayers(OpenBGI_t* o)
{
	Layers_Clear(&o->input.mouseLayers);
	Layers_Clear(&o->input.keyLayers);
	MouseLayer_Push(o, 1);
	KeyLayer_Push(o, 1);
}

// whether a key layer receives keys: its key is at least the top one's, and input is enabled
int KeyLayer_IsTop(OpenBGI_t* o, uint32_t key)
{
	if(!o->input.inputEnable || !o->input.keyLayers)
		return 0;
	return key >= o->input.keyLayers->key;
}

/*
 * Would the cursor at back-buffer position (x, y) hit the mouse layer
 * `key`?  Layers are visited from the highest key down.  A higher layer
 * that contains the point (and, for sprite layers, whose sprite is
 * visible and opaque there) hides the requested layer; the first layer
 * with a lower key ends the search.  Whether input is enabled is not
 * asked; nothing changes.
 */
int MouseLayer_HitAt(OpenBGI_t* o, uint32_t key, int x, int y)
{
	const InputLayer_t* l;
	for(l = o->input.mouseLayers; l; l = l->next)
	{
		Rect_t r;
		if(l->sprite)
		{
			if(!l->sprite->vt->isVisible(o, l->sprite))
				continue;
			l->sprite->vt->screenRect(o, l->sprite, &r); // the layer follows the sprite
		}
		else
		{
			r.l = l->l;
			r.t = l->t;
			r.r = l->r;
			r.b = l->b;
		}
		if(l->key < key)
			return 0;
		if(x < r.l || x > r.r || y < r.t || y > r.b)
			continue;
		if(l->sprite && !l->sprite->vt->hitTest(o, l->sprite, x - r.l, y - r.t))
			continue;
		return l->key == key;
	}
	return 0;
}

// Does the cursor hit the mouse layer `key` (MouseLayer_HitAt where it is)?  0 while input is disabled.
int MouseLayer_Hit(OpenBGI_t* o, uint32_t key)
{
	int pos[2];
	if(!o->input.inputEnable)
		return 0;
	GetMouseClientPos(o, pos);
	return MouseLayer_HitAt(o, key, pos[0], pos[1]);
}

/* The keys of the mouse layers that cover every coordinate (pushed
 * without a sprite), highest first, one per layer: up to `max` into
 * `out`; returns how many there are. */
int MouseLayer_Unbounded(OpenBGI_t* o, uint32_t* out, int max)
{
	const InputLayer_t* l;
	int n = 0;
	for(l = o->input.mouseLayers; l; l = l->next)
	{
		if(l->sprite || l->l != gFullRect[0] || l->t != gFullRect[1] || l->r != gFullRect[2] || l->b != gFullRect[3])
			continue;
		if(n < max)
			out[n] = l->key;
		n++;
	}
	return n;
}

/* "80 1A": the input pending for a pair of layers, as a mask: bit 31
 * skip, bits 6 .. 30 the standard-key groups (when the key layer is on
 * top), bits 0, 1, 2, 4, 5 the mouse buttons (when the mouse layer is
 * hit).  The pending input is consumed; "80 18" / "80 19" and the wait
 * objects call it to swallow what is pending for a layer. */
uint32_t Input_Poll(OpenBGI_t* o, uint32_t keyLayer, uint32_t mouseLayer)
{
	uint32_t mask = 0;
	if(KeyLayer_IsTop(o, keyLayer))
	{
		mask = StdKeys_PollTriggers(o);
		if(Input_CheckSkip(o))
			mask |= 0x80000000u;
	}
	if(MouseLayer_Hit(o, mouseLayer))
	{
		if(GetKeyTrigger(o, 1))
			mask |= 0x1;
		if(GetKeyTrigger(o, 2))
			mask |= 0x2;
		if(GetKeyTrigger(o, 4))
			mask |= 0x4;
		if(GetKeyTrigger(o, 5))
			mask |= 0x10;
		if(GetKeyTrigger(o, 6))
			mask |= 0x20;
	}
	return mask;
}

// the mouse buttons held past the repeat delay (the same bits as Input_Poll), for the wait classes
uint32_t Input_PollHeld(OpenBGI_t* o, uint32_t mouseLayer)
{
	uint32_t mask = 0;
	if(MouseLayer_Hit(o, mouseLayer))
	{
		if(Key_IsRepeating(o, 1))
			mask |= 0x1;
		if(Key_IsRepeating(o, 2))
			mask |= 0x2;
		if(Key_IsRepeating(o, 4))
			mask |= 0x4;
		if(Key_IsRepeating(o, 5))
			mask |= 0x10;
		if(Key_IsRepeating(o, 6))
			mask |= 0x20;
	}
	return mask;
}

// -------------------------------------------------------------------------
// input focus of wait objects
// -------------------------------------------------------------------------
typedef struct FocusNode
{
	uint32_t serial;        // the wait object's serial
	uint32_t key;           // its layer key
	struct FocusNode* next; // lower or equal keys follow
} FocusNode_t;

/* Register a wait object (the menu and icon waits) with its layer key;
 * the node is inserted after every node with a key >= the new one. */
void InputFocus_Add(OpenBGI_t* o, uint32_t waitSerial, uint32_t key)
{
	FocusNode_t **link = &o->input.focusList, *n;
	while(*link && (*link)->key >= key)
		link = &(*link)->next;
	n = (FocusNode_t*)BGI_Alloc(sizeof(FocusNode_t));
	n->serial = waitSerial;
	n->key = key;
	n->next = *link;
	*link = n;
}

// Unregister a wait object by its serial.
void InputFocus_Remove(OpenBGI_t* o, uint32_t waitSerial)
{
	FocusNode_t** link;
	for(link = &o->input.focusList; *link; link = &(*link)->next)
	{
		if((*link)->serial == waitSerial)
		{
			FocusNode_t* n = *link;
			*link = n->next;
			BGI_Free(n);
			return;
		}
	}
}

// Free both lists, the base layers too, and the waits registered for the focus (OpenBGI_Delete).
void Input_FreeLayers(OpenBGI_t* o)
{
	Layers_Clear(&o->input.mouseLayers);
	Layers_Clear(&o->input.keyLayers);
	while(o->input.focusList)
	{
		FocusNode_t* n = o->input.focusList;
		o->input.focusList = n->next;
		BGI_Free(n);
	}
}

// whether a layer key has the focus among the registered waits (1 when none is registered)
int InputFocus_IsTop(OpenBGI_t* o, uint32_t key)
{
	if(!o->input.focusList)
		return 1;
	return key >= o->input.focusList->key;
}
