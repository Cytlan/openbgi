/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * input.h - keyboard / mouse state (src/sys/input.c) and the mouse
 *           cursor (src/sys/cursor.c)
 *
 * Key state is tracked per virtual-key code in a table of 256 records fed
 * by the window's events.  Scripts see the keys through "layers": a layer
 * is identified by a 32-bit key, usually (priority << 20) | 0xfffff, and
 * only the top-most key layer (or the mouse layer under the cursor)
 * receives input.  Standard-key groups (bits of the "80 1A" mask) map
 * several virtual keys to one logical function (decide, cancel, up, ...).
 *
 * Virtual keys 0x0e / 0x0f are the mouse wheel (up / down); 0xc1 .. 0xc4
 * are the decide / cancel / up+wheel / down+wheel groups.
 */
#ifndef BGI_INPUT_H_
#define BGI_INPUT_H_

#include "bgi/common.h"

struct DispObj; // the display object a mouse layer follows

// ---- raw key state -----------------------------------------------------
extern int gInputEnable;        // "80 10": the layers report nothing while 0
int GetKeyStateEx(int vk);      // "80 11": bit 15 = down, honours swapped buttons
int GetKeyTrigger(int vk);      // "80 1D": pending presses, bit 31 = first report while held
int GetKeyCounter(int vk);      // presses since KeyState_Clear
int Key_IsRepeating(int vk);    // held past the auto-repeat delay (panel hot keys)
void Input_ClearStates(void);   // everything but the counters
void KeyState_Clear(void);      // everything, the counters too
void Input_Flush(void);         // "80 16"
int Input_CheckSkip(void);      // "80 17": the skip function is active
uint32_t InputSerial_Get(void); // "80 13": bumped on every input message
void InputSerial_Bump(void);

// window-procedure entry points (window.c)
int Input_OnKeyDown(int vk, int repeat);   // 1 for a fresh press
int Key_SetRepeatNew(int vk, int on);      // "81 10" (1.69/472 on): auto-repeat counts as presses; the old flag
void Input_SetTweenKeyMask(uint32_t mask); // "81 1F" (1.69/472 on): the keys that break a tween (0x2000)
void Input_SetKeysWhenInactive(int on);    // "81 14" (1.529 on): key states are read while the window is inactive
uint32_t Input_TweenKeyMask(void);         // ... | 0x180, what the waits poll
void Input_OnKeyUp(int vk);
void Input_CountPress(int vk); // counter only (knob clicks)

// ---- switches ("80 10", "80 14", "80 15", "80 1E") ---------------------
void Set_InputEnable(int on);
void Set_SkipKeyEnable(int on);
void Set_SkipLatch(int on);
int Input_SetSwapButtons(uint32_t v); // 0 or 1; returns 1 when accepted
int Input_SwapButtons(void);

// ---- standard keys ------------------------------------------------------
enum // the groups, in the order of their bits in the "80 1A" mask
{
	STDKEY_LBUTTON,    // bit 0
	STDKEY_RBUTTON,    // bit 1
	STDKEY_MBUTTON,    // bit 2
	STDKEY_XBUTTON1,   // bit 4
	STDKEY_XBUTTON2,   // bit 5
	STDKEY_WHEEL_UP,   // bit 6; from here on assignable with "80 1B"
	STDKEY_WHEEL_DOWN, // bit 7
	STDKEY_DECIDE,     // bit 8
	STDKEY_CANCEL,     // bit 9
	STDKEY_UP,         // bit 12
	STDKEY_DOWN,       // bit 13
	STDKEY_LEFT,       // bit 14
	STDKEY_RIGHT,      // bit 15
	STDKEY_1,          // bit 16
	STDKEY_2,          // bit 17
	STDKEY_3,          // bit 18
	STDKEY_4,          // bit 19
	STDKEY_5,          // bit 20
	STDKEY_6,          // bit 21
	STDKEY_7,          // bit 22
	STDKEY_8,          // bit 23
	STDKEY_9,          // bit 24
	STDKEY_0,          // bit 25
	STDKEY_TAB,        // bit 30 (assignable from 1.529 on)
	STDKEY_COUNT
};
extern uint32_t gStdKeys[STDKEY_COUNT][16]; // 0-terminated lists
extern uint32_t gSkipKeys[16];              // the skip keys (bit 31), 0-terminated
extern uint32_t gWheelUpKeys[3];            // group 0xc3: up + wheel up
extern uint32_t gWheelDownKeys[3];          // group 0xc4: down + wheel down
/* "80 1B": assign up to 15 virtual keys (0-terminated) to a group
 * (0x40 .. 0x8000); 0 ok, 0x80000001 unknown group, 0x80000002 too many */
uint32_t SetStandardKey(uint32_t group, const uint32_t* keys);
int CountKeysByMask(uint32_t mask);        // "80 1C": the counters of the groups in mask, summed
int StdKey_MaskBit(uint32_t mask, int vk); // the bit of mask that vk belongs to (default assignment)

// ---- layers -------------------------------------------------------------
void Input_ResetLayers(void);       // clear, push layer 1 on both lists
void MouseLayer_Push(uint32_t key); // unbounded rectangle
void KeyLayer_Push(uint32_t key);
void MouseLayer_PushSprite(struct DispObj* s); // a layer with the sprite's key that follows it
void MouseLayer_Pop(uint32_t key);
void KeyLayer_Pop(uint32_t key);
void MouseLayer_PopSprite(struct DispObj* s);
int KeyLayer_IsTop(uint32_t key); // the key layer receives keys
int MouseLayer_Hit(uint32_t key); // the cursor is in the mouse layer and no higher one covers it
/* the "80 1A" mask: bit 31 skip, bits 6..30 standard keys,
 * bits 0,1,2,4,5 the mouse buttons when the mouse layer is hit */
uint32_t Input_Poll(uint32_t keyLayer, uint32_t mouseLayer);
uint32_t Input_PollHeld(uint32_t mouseLayer); // buttons held past the repeat delay

// ---- input focus of wait objects -------------------------
void InputFocus_Add(uint32_t waitSerial, uint32_t key);
void InputFocus_Remove(uint32_t waitSerial);
int InputFocus_IsTop(uint32_t key); // 1 when no registered wait has a higher key

// ---- cursor (src/sys/cursor.c) ------------------------------------------
extern int gCursorShape;                                               // 0..4, "80 67"
void GetMouseClientPos(int xy[2]);                                     // "80 08": back-buffer coordinates; 0,0 without a window
void ClientToScreenPos(int out[2], const int in[2]);                   // the inverse
void MouseMove_Start(int x, int y, int steps, int a4, int a5, int a6); // "80 1F": (x, y, curve, duration ms, fps, cancelOnMove)
void SetCursorAutoHide(uint32_t ms);                                   // "B0 05"
int Cursor_IsVisible(void);                                            // "B0 06": not auto-hidden
void Cursor_Refresh(void);                                             // show again before a dialog
void Cursor_Update(void);                                              // per pass
void Mouse_ProbeSwap(void);                                            // cache SwapMouseButton state
int Mouse_SysSwapped(void);
int SetCursorSprite(uint32_t sprite, int dx, int dy); // "B0 04": 0 ok, -1 the sprite cannot be shown
void CursorSprite_Update(void);                       // per pass
int ShowCursorCount(int show);                        // returns the previous state
int Cursor_IsShown(void);

#endif // BGI_INPUT_H_
