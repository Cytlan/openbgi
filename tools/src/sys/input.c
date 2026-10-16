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

// -------------------------------------------------------------------------
// key state table (256 x 20 bytes)
// -------------------------------------------------------------------------
typedef struct KeyRec
{
	int32_t down;      // currently pressed
	int32_t consumed;  // the press was reported by GetKeyTrigger
	int32_t pending;   // presses not yet reported
	int32_t counter;   // presses since KeyState_Clear
	uint32_t repeatAt; // tick at which auto-repeat starts
	int32_t repeatNew; // from 1.69 build 472 ("81 10"): every WM_KEYDOWN counts as a new press
} KeyRec_t;

static KeyRec_t gKeyState[256];

int gInputEnable = 1;          // "80 10": the layers report nothing while 0
static int gSkipKeyEnable = 1; // "80 14": the skip keys count
static int gSkipLatch;         // "80 15": the skip function is held on without a key
static int gSkipFlushed;       // set by Input_Flush: a held skip key does not count until released
static uint32_t gInputSerial;  // bumped on every input message ("80 13")
static int gSwapButtons;       // "80 1E": 1 exchanges the left and right buttons
static int gKeysWhenInactive;  // "81 14": key states are read while the window is inactive

#define KEY_REPEAT_DELAY 500 // ms before auto-repeat

// virtual-key codes are 0..255; anything else is a script error
static void CheckVirtualKey(int vk)
{
	if(vk < 0 || vk > 0xff)
		ThrowScriptError(MSG_BAD_VIRTUAL_KEY);
}

uint32_t InputSerial_Get(void)
{
	return gInputSerial;
}

void InputSerial_Bump(void)
{
	gInputSerial++;
}

void Set_InputEnable(int on)
{
	gInputEnable = on;
}

void Set_SkipKeyEnable(int on)
{
	gSkipKeyEnable = on;
}

void Set_SkipLatch(int on)
{
	gSkipLatch = on;
}

int Input_SwapButtons(void)
{
	return gSwapButtons;
}

// "80 1E": 0 or 1; returns 1 when accepted, 0 for any other value
int Input_SetSwapButtons(uint32_t v)
{
	if(v > 1)
		return 0;
	gSwapButtons = (int)v;
	return 1;
}

/*
 * The asynchronous state of a key ("80 11"), with the left / right mouse
 * buttons exchanged when the script asked for it: the left button (1)
 * then reads either physical button, the right button (2) reads key 0,
 * which is never down - it acts as the left one.  When the system has
 * the buttons swapped as well, both are checked so that either physical
 * button works.  Returns the GetAsyncKeyState value: bit 15 = down.
 * From 1.529 on nothing is down while the window is inactive, unless
 * "81 14" allows it.
 */
int GetKeyStateEx(int vk)
{
	int second = 0, r;
	if(vk == 1 && gSwapButtons == 1)
		second = 2;
	else if(vk == 2 && gSwapButtons == 1)
		vk = 0;
	if(Mouse_SysSwapped())
	{ // SwapMouseButton state
		if(vk == 1)
		{
			vk = 2;
			second = second != 0; // 1 when the swap above also applied
		}
		else if(vk == 2)
		{
			vk = 1;
		}
	}
	r = OS_KeyState(vk) ? 0x8000 : 0;
	if(second)
		r |= OS_KeyState(second) ? 0x8000 : 0;
	// 1.529 on: nothing while the window is inactive, unless "81 14" allows it
	if(gEngine->gen >= GEN_1_529 && !Window_IsActive() && !gKeysWhenInactive)
		return 0;
	return r;
}

// "81 14" of 1.529 on
void Input_SetKeysWhenInactive(int on)
{
	gKeysWhenInactive = on;
}

/* "81 10" of 1.69 build 472 on: set the flag that makes the keyboard's
 * auto-repeat of key vk count as new presses (Input_OnKeyDown releases
 * the key before each press); the old value is returned. */
int Key_SetRepeatNew(int vk, int on)
{
	int old;
	CheckVirtualKey(vk);
	old = gKeyState[vk].repeatNew;
	gKeyState[vk].repeatNew = on;
	return old;
}

/* A press from the window's events; 1 when it was a fresh press (the key
 * was up), 0 for the keyboard's auto-repeat of a held key, which changes
 * nothing.  The mouse buttons (1, 2, 4, 5, 6) report every click to
 * GetKeyTrigger, a key only its first press while held.  `repeat` is not
 * used: a held key is recognised from the table. */
int Input_OnKeyDown(int vk, int repeat)
{
	KeyRec_t* k;
	int fresh;
	BGI_UNUSED(repeat);
	CheckVirtualKey(vk);
	k = &gKeyState[vk];
	if(k->repeatNew)
		Input_OnKeyUp(vk);
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
	k->repeatAt = GetTicks() + KEY_REPEAT_DELAY;
	k->down = 1;
	return fresh;
}

// A release from the window's events.
void Input_OnKeyUp(int vk)
{
	CheckVirtualKey(vk);
	gKeyState[vk].down = 0;
	gKeyState[vk].consumed = 0;
}

// Count a press without a state change (a button click that went to a knob).
void Input_CountPress(int vk)
{
	CheckVirtualKey(vk);
	gKeyState[vk].counter++;
}

// a held key past its auto-repeat delay (the panels' hot keys)
int Key_IsRepeating(int vk)
{
	const KeyRec_t* k = &gKeyState[vk];
	if(!k->down)
		return 0;
	return GetTicks() >= k->repeatAt;
}

/*
 * The pending presses of a key, consumed by the call.  Bit 31 is set
 * when the key is still held and this is the first report of the press.
 * 0xc1 .. 0xc4 stand for the groups below (decide, cancel, up + wheel
 * up, down + wheel down) and sum their members.  A key whose release
 * message was lost (held in the table, up in the system) is released
 * first.
 */
int GetKeyTrigger(int vk)
{
	static const uint32_t* const groups[4] = {
		gStdKeys[STDKEY_DECIDE], gStdKeys[STDKEY_CANCEL],
		gWheelUpKeys, gWheelDownKeys};
	KeyRec_t* k;
	uint32_t r;
	CheckVirtualKey(vk);
	k = &gKeyState[vk];
	if(k->down && !(((uint32_t)GetKeyStateEx(vk) >> 15) & 1))
		Input_OnKeyUp(vk); // the release message was lost

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
				uint32_t t = (uint32_t)GetKeyTrigger((int)*list);
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

int GetKeyCounter(int vk)
{
	return gKeyState[vk].counter;
}

// Forget everything, the counters included (the engine's reset).
void KeyState_Clear(void)
{
	memset(gKeyState, 0, sizeof gKeyState);
}

// Forget the pressed state and the pending presses of every key; the counters stay.
void Input_ClearStates(void)
{
	int i;
	for(i = 0; i < 256; i++)
	{
		gKeyState[i].down = 0;
		gKeyState[i].consumed = 0;
		gKeyState[i].pending = 0;
		gKeyState[i].repeatAt = 0;
	}
}

// -------------------------------------------------------------------------
// standard keys
// -------------------------------------------------------------------------

// the 24 key lists in mask-bit order; each list is 0-terminated
uint32_t gStdKeys[STDKEY_COUNT][16] = {
	{0x01},       // bit 0        left button
	{0x02},       // bit 1        right button
	{0x04},       // bit 2        middle button
	{0x05},       // bit 4        X button 1
	{0x06},       // bit 5        X button 2
	{0x0e},       // bit 6  0x40  wheel up    ("80 1B")
	{0x0f},       // bit 7  0x80  wheel down
	{0x0d},       // bit 8  0x100 decide: Enter
	{0x20},       // bit 9  0x200 cancel: Space
	{0x26},       // bit 12 0x1000 up
	{0x28},       // bit 13 0x2000 down
	{0x25},       // bit 14 0x4000 left
	{0x27},       // bit 15 0x8000 right
	{0x31, 0x61}, // bit 16 '1' / numpad 1
	{0x32, 0x62}, // bit 17 '2'
	{0x33, 0x63}, // bit 18 '3'
	{0x34, 0x64}, // bit 19 '4'
	{0x35, 0x65}, // bit 20 '5'
	{0x36, 0x66}, // bit 21 '6'
	{0x37, 0x67}, // bit 22 '7'
	{0x38, 0x68}, // bit 23 '8'
	{0x39, 0x69}, // bit 24 '9'
	{0x30, 0x60}, // bit 25 '0'
	{0x09},       // bit 30 Tab
};
// the mask bit of each list, in the same order
static const uint32_t gStdKeyBits[STDKEY_COUNT] = {
	0x1, 0x2, 0x4, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x1000, 0x2000,
	0x4000, 0x8000, 0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000,
	0x400000, 0x800000, 0x1000000, 0x2000000, 0x40000000};
uint32_t gSkipKeys[16] = {0x11};           // the skip keys (bit 31): Ctrl
uint32_t gWheelUpKeys[3] = {0x26, 0x0e};   // 0xc3 = up + wheel up
uint32_t gWheelDownKeys[3] = {0x28, 0x0f}; // 0xc4 = down + wheel down

/* "80 1B": replace the keys of one of the assignable groups, named by its
 * mask bit (0x40 .. 0x8000; from 1.529 on also 0x40000000 Tab and
 * 0x80000000 the skip keys) with a 0-terminated list of at most 15.
 * 0 ok, 0x80000001 unknown group, 0x80000002 too many keys. */
uint32_t SetStandardKey(uint32_t group, const uint32_t* keys)
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
			if(gEngine->gen < GEN_1_529)
				return 0x80000001u;
			slot = STDKEY_TAB;
			break;
		case 0x80000000:
			if(gEngine->gen < GEN_1_529)
				return 0x80000001u;
			memcpy(gSkipKeys, keys, (size_t)(n + 1) * sizeof(uint32_t));
			return 0;
		default: return 0x80000001u;
	}
	memcpy(gStdKeys[slot], keys, (size_t)(n + 1) * sizeof(uint32_t));
	return 0;
}

// the trigger mask of the key groups (bits 6 .. 30); the triggers are consumed
static uint32_t StdKeys_PollTriggers(void)
{
	uint32_t mask = 0;
	int g;
	for(g = STDKEY_WHEEL_UP; g < STDKEY_COUNT; g++)
	{
		const uint32_t* k;
		for(k = gStdKeys[g]; *k; k++)
		{
			if(GetKeyTrigger((int)*k))
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
static uint32_t gTweenKeyMask = 0x2000;
void Input_SetTweenKeyMask(uint32_t mask)
{
	gTweenKeyMask = mask;
}
uint32_t Input_TweenKeyMask(void)
{
	return gTweenKeyMask | 0x180;
}

// "80 1C": the counters of every key in the groups of `mask`, summed
int CountKeysByMask(uint32_t mask)
{
	int g, sum = 0;
	for(g = 0; g < STDKEY_COUNT; g++)
	{
		const uint32_t* k;
		if(!(mask & gStdKeyBits[g]))
			continue;
		for(k = gStdKeys[g]; *k; k++)
			sum += GetKeyCounter((int)*k);
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
void Input_Flush(void)
{
	const uint32_t* k;
	gSkipFlushed = 1;
	for(k = gSkipKeys; *k; k++)
		GetKeyTrigger((int)*k);
}

/*
 * "80 17": is the "skip" function active?  A skip key (Ctrl by default)
 * held while the window is active and not minimised, or the latch set by
 * "80 15".  After Input_Flush only a new press counts until every skip
 * key has been released.  Requires the skip-key switch ("80 14") and
 * input to be enabled ("80 10").
 */
int Input_CheckSkip(void)
{
	int latched = gSkipLatch;
	int anyHeld = 0;
	const uint32_t* k;

	if(Window_IsActive() && gInputEnable && gSkipKeys[0])
	{
		for(k = gSkipKeys; *k; k++)
		{
			int down = GetKeyStateEx((int)*k) & 0x8000;
			if(down)
				anyHeld = 1;
			if(gSkipFlushed)
			{
				if(GetKeyTrigger((int)*k) && Window_IsRestored())
					latched = 1;
			}
			else if(down && Window_IsRestored())
			{
				return gSkipKeyEnable ? 1 : 0;
			}
		}
	}
	if(gSkipFlushed && !anyHeld)
		gSkipFlushed = 0;
	if(latched && gSkipKeyEnable)
		return 1;
	return 0;
}

// -------------------------------------------------------------------------
// layers
// -------------------------------------------------------------------------
typedef struct InputLayer
{
	uint32_t key;            // priority key
	int32_t l, t, r, b;      // rectangle (mouse layers), inclusive
	DispObj_t* sprite;       // sprite that owns the rectangle, or NULL
	struct InputLayer* next; // lower keys follow
} InputLayer_t;

static InputLayer_t* gMouseLayers; // highest key first
static InputLayer_t* gKeyLayers;   // highest key first

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
void MouseLayer_Push(uint32_t key)
{
	Layer_Push(&gMouseLayers, key, gFullRect, NULL);
}

// a key layer (its rectangle is unused)
void KeyLayer_Push(uint32_t key)
{
	static const int32_t zero[4] = {0, 0, 0, 0};
	Layer_Push(&gKeyLayers, key, zero, NULL);
}

/* a mouse layer that follows a sprite (its key is the sprite's input
 * priority, its rectangle is refreshed on every hit test) */
void MouseLayer_PushSprite(DispObj_t* s)
{
	static const int32_t zero[4] = {0, 0, 0, 0};
	Layer_Push(&gMouseLayers, s->vt->sortKey(s), zero, s);
}

void MouseLayer_Pop(uint32_t key)
{
	Layer_PopKey(&gMouseLayers, key);
}

void KeyLayer_Pop(uint32_t key)
{
	Layer_PopKey(&gKeyLayers, key);
}

void MouseLayer_PopSprite(DispObj_t* s)
{
	Layer_PopSprite(&gMouseLayers, s);
}

// Clear both lists and push the base layer (key 1) on each (the engine's reset and shutdown).
void Input_ResetLayers(void)
{
	Layers_Clear(&gMouseLayers);
	Layers_Clear(&gKeyLayers);
	MouseLayer_Push(1);
	KeyLayer_Push(1);
}

// whether a key layer receives keys: its key is at least the top one's, and input is enabled
int KeyLayer_IsTop(uint32_t key)
{
	if(!gInputEnable || !gKeyLayers)
		return 0;
	return key >= gKeyLayers->key;
}

/*
 * Does the cursor hit the mouse layer `key`?  Layers are visited from
 * the highest key down.  A higher layer that contains the cursor (and,
 * for sprite layers, whose sprite is visible and opaque there) hides the
 * requested layer; the first layer with a lower key ends the search.
 * 0 while input is disabled.
 */
int MouseLayer_Hit(uint32_t key)
{
	InputLayer_t* l;
	int pos[2];
	if(!gInputEnable)
		return 0;
	GetMouseClientPos(pos);
	for(l = gMouseLayers; l; l = l->next)
	{
		int inside;
		if(l->sprite)
		{
			Rect_t r;
			if(!l->sprite->vt->isVisible(l->sprite))
				continue;
			l->sprite->vt->screenRect(l->sprite, &r); // the layer follows the sprite
			l->l = r.l;
			l->t = r.t;
			l->r = r.r;
			l->b = r.b;
		}
		if(l->key < key)
			return 0;
		inside = pos[0] >= l->l && pos[0] <= l->r && pos[1] >= l->t && pos[1] <= l->b;
		if(!inside)
			continue;
		if(l->sprite && !l->sprite->vt->hitTest(l->sprite, pos[0] - l->l, pos[1] - l->t))
			continue;
		return l->key == key;
	}
	return 0;
}

/* "80 1A": the input pending for a pair of layers, as a mask: bit 31
 * skip, bits 6 .. 30 the standard-key groups (when the key layer is on
 * top), bits 0, 1, 2, 4, 5 the mouse buttons (when the mouse layer is
 * hit).  The pending input is consumed; "80 18" / "80 19" and the wait
 * objects call it to swallow what is pending for a layer. */
uint32_t Input_Poll(uint32_t keyLayer, uint32_t mouseLayer)
{
	uint32_t mask = 0;
	if(KeyLayer_IsTop(keyLayer))
	{
		mask = StdKeys_PollTriggers();
		if(Input_CheckSkip())
			mask |= 0x80000000u;
	}
	if(MouseLayer_Hit(mouseLayer))
	{
		if(GetKeyTrigger(1))
			mask |= 0x1;
		if(GetKeyTrigger(2))
			mask |= 0x2;
		if(GetKeyTrigger(4))
			mask |= 0x4;
		if(GetKeyTrigger(5))
			mask |= 0x10;
		if(GetKeyTrigger(6))
			mask |= 0x20;
	}
	return mask;
}

// the mouse buttons held past the repeat delay (the same bits as Input_Poll), for the wait classes
uint32_t Input_PollHeld(uint32_t mouseLayer)
{
	uint32_t mask = 0;
	if(MouseLayer_Hit(mouseLayer))
	{
		if(Key_IsRepeating(1))
			mask |= 0x1;
		if(Key_IsRepeating(2))
			mask |= 0x2;
		if(Key_IsRepeating(4))
			mask |= 0x4;
		if(Key_IsRepeating(5))
			mask |= 0x10;
		if(Key_IsRepeating(6))
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
static FocusNode_t* gFocusList; // highest key first

/* Register a wait object (the menu and icon waits) with its layer key;
 * the node is inserted after every node with a key >= the new one. */
void InputFocus_Add(uint32_t waitSerial, uint32_t key)
{
	FocusNode_t **link = &gFocusList, *n;
	while(*link && (*link)->key >= key)
		link = &(*link)->next;
	n = (FocusNode_t*)BGI_Alloc(sizeof(FocusNode_t));
	n->serial = waitSerial;
	n->key = key;
	n->next = *link;
	*link = n;
}

// Unregister a wait object by its serial.
void InputFocus_Remove(uint32_t waitSerial)
{
	FocusNode_t** link;
	for(link = &gFocusList; *link; link = &(*link)->next)
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

// whether a layer key has the focus among the registered waits (1 when none is registered)
int InputFocus_IsTop(uint32_t key)
{
	if(!gFocusList)
		return 1;
	return key >= gFocusList->key;
}
