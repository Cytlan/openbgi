/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * input.h - keyboard / mouse state (src/sys/input.c) and the mouse
 *           cursor (src/sys/cursor.c)
 *
 * Key state is tracked per virtual-key code in a table of 256 records fed
 * by the input the host hands in (openbgi.h, OpenBGI_Key and the rest;
 * src/sys/window.c).  Scripts see the keys through "layers": a layer
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
#include "openbgi.h"
#include "bgi/inputcfg.h"

struct DispObj; // the display object a mouse layer follows

// ---- raw key state -----------------------------------------------------
int GetKeyStateEx(OpenBGI_t* o, int vk);   // "80 11": bit 15 = down, honours swapped buttons
int GetKeyTrigger(OpenBGI_t* o, int vk);   // "80 1D": pending presses, bit 31 = first report while held
int GetKeyCounter(OpenBGI_t* o, int vk);   // presses since KeyState_Clear
int Key_IsRepeating(OpenBGI_t* o, int vk); // held past the auto-repeat delay (panel hot keys)
void Input_ClearStates(OpenBGI_t* o);      // everything but the counters
void KeyState_Clear(OpenBGI_t* o);         // everything, the counters too
void Input_Flush(OpenBGI_t* o);            // "80 16"
int Input_CheckSkip(OpenBGI_t* o);         // "80 17": the skip function is active
uint32_t InputSerial_Get(OpenBGI_t* o);    // "80 13": bumped on every input message
void InputSerial_Bump(OpenBGI_t* o);

// window-procedure entry points (window.c)
int Input_OnKeyDown(OpenBGI_t* o, int vk, int repeat);   // 1 for a fresh press
int Key_SetRepeatNew(OpenBGI_t* o, int vk, int on);      // "81 10" (1.69/472 on): auto-repeat counts as presses; the old flag
void Input_SetTweenKeyMask(OpenBGI_t* o, uint32_t mask); // "81 1F" (1.69/472 on): the keys that break a tween (0x2000)
void Input_SetKeysWhenInactive(OpenBGI_t* o, int on);    // "81 14" (1.529 on): key states are read while the window is inactive
uint32_t Input_TweenKeyMask(OpenBGI_t* o);               // ... | 0x180, what the waits poll

// ---- gamepad ("81 1B", 1.547 on; no OS back end reads one) -------------------
#define PAD_CONTROLS 36                                         // buttons 0 .. 31, then up, down, left, right
uint32_t Pad_SetKey(OpenBGI_t* o, uint32_t index, uint32_t vk); // 0 ok, 0x80000002 index past the table
void Input_OnKeyUp(OpenBGI_t* o, int vk);
void Input_CountPress(OpenBGI_t* o, int vk); // counter only (knob clicks)

// ---- switches ("80 10", "80 14", "80 15", "80 1E") ---------------------
void Set_InputEnable(OpenBGI_t* o, int on);
void Set_SkipKeyEnable(OpenBGI_t* o, int on);
void Set_SkipLatch(OpenBGI_t* o, int on);
int Input_SetSwapButtons(OpenBGI_t* o, uint32_t v); // 0 or 1; returns 1 when accepted
int Input_SwapButtons(OpenBGI_t* o);

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

/* "80 1B": assign up to 15 virtual keys (0-terminated) to a group
 * (0x40 .. 0x8000); 0 ok, 0x80000001 unknown group, 0x80000002 too many */
uint32_t SetStandardKey(OpenBGI_t* o, uint32_t group, const uint32_t* keys);
int CountKeysByMask(OpenBGI_t* o, uint32_t mask); // "80 1C": the counters of the groups in mask, summed
int StdKey_MaskBit(uint32_t mask, int vk);        // the bit of mask that vk belongs to (default assignment)

// ---- layers -------------------------------------------------------------
void Input_ResetLayers(OpenBGI_t* o);             // clear, push layer 1 on both lists
void Input_FreeLayers(OpenBGI_t* o);              // free both lists and the focus list (OpenBGI_Delete)
void MouseLayer_Push(OpenBGI_t* o, uint32_t key); // unbounded rectangle
void KeyLayer_Push(OpenBGI_t* o, uint32_t key);
void MouseLayer_PushSprite(OpenBGI_t* o, struct DispObj* s); // a layer with the sprite's key that follows it
void MouseLayer_Pop(OpenBGI_t* o, uint32_t key);
void KeyLayer_Pop(OpenBGI_t* o, uint32_t key);
void MouseLayer_PopSprite(OpenBGI_t* o, struct DispObj* s);
int KeyLayer_IsTop(OpenBGI_t* o, uint32_t key);                 // the key layer receives keys
int MouseLayer_Hit(OpenBGI_t* o, uint32_t key);                 // the cursor is in the mouse layer and no higher one covers it
int MouseLayer_HitAt(OpenBGI_t* o, uint32_t key, int x, int y); // the same for the cursor at (x, y), input enabled or not
int MouseLayer_Unbounded(OpenBGI_t* o, uint32_t* out, int max); // the keys of the layers that cover everything, highest first; their count
/* the "80 1A" mask: bit 31 skip, bits 6..30 standard keys,
 * bits 0,1,2,4,5 the mouse buttons when the mouse layer is hit */
uint32_t Input_Poll(OpenBGI_t* o, uint32_t keyLayer, uint32_t mouseLayer);
uint32_t Input_PollHeld(OpenBGI_t* o, uint32_t mouseLayer); // buttons held past the repeat delay

// ---- input focus of wait objects -------------------------
void InputFocus_Add(OpenBGI_t* o, uint32_t waitSerial, uint32_t key);
void InputFocus_Remove(OpenBGI_t* o, uint32_t waitSerial);
int InputFocus_IsTop(OpenBGI_t* o, uint32_t key); // 1 when no registered wait has a higher key

/* ---- what the scripts test the pointer with (src/sys/inputmap.c) ----
 * The instructions that read the mouse for a script's own use note it
 * here, for the clickable targets (OpenBGI_Targets): a layer polled
 * ("80 1A", the buttons of "80 1D"), the buttons read directly ("80 11"),
 * an object hit-tested ("90 3D").  A note counts for INPUT_PROBE_MS of
 * the engine's clock. */
#define INPUT_PROBES   16
#define INPUT_PROBE_MS 500
enum
{
	PROBE_LAYER = 1, // v: the layer key
	PROBE_BUTTONS,   // v: the virtual key (1 or 2)
	PROBE_OBJECT     // v: the object's handle
};
typedef struct InputProbe
{
	uint32_t at; // GetTicks when it was last noted
	int what;    // PROBE_*, 0 for an empty entry
	uint32_t v;
} InputProbe_t;
void Input_NoteProbe(OpenBGI_t* o, int what, uint32_t v);

/* ---- the mapper's steps (src/sys/steps.c) ----
 * What a touch gesture or a pad's control does to the mouse goes into one
 * queue of steps - move the pointer, press or release a button, turn the
 * wheel, press or release a key - each with the number of passes to let
 * run between it and the step before.  Steps_Frame, at the start of a
 * frame that runs a pass, applies the steps that are due through the calls
 * a host's mouse and keyboard take (OpenBGI_Pointer, OpenBGI_Button,
 * OpenBGI_Wheel, OpenBGI_Key), so a click is: the pointer moved, a pass,
 * the press, a pass, the release.  A game's settings may ask for more
 * time before a click's press and its release (`tapDelay`, the "tapDelay"
 * of bgi/inputcfg.h): such a step also waits that long after the one
 * before.  While `applying` is set an input call is the mapper's own, not
 * the host's.
 *
 * A program that reads the buttons itself ("80 11", once a turn of its own
 * loop) would miss a press and a release that both fall between two of its
 * readings - a pass is much shorter than such a turn.  A step that changes
 * the left or the right button therefore waits until the program has read
 * what the step before made of that button, as long as the program read
 * the button shortly before that step (STEPS_READER_MS) and for no longer
 * than STEPS_READ_MS: one that reads no more holds nothing up.  A program
 * that counts presses or has the engine watch its targets loses none and
 * is not waited for. */
#define INPUT_STEPS     64  // steps queued at most
#define STEPS_READER_MS 250 // a program that read a button this shortly before a step changed it reads it itself ..
#define STEPS_READ_MS   250 // .. and the button's next step waits this long at most for it to read the change
typedef struct InputStep
{
	int kind;   // INPUT_STEP_*
	int a;      // the button (INPUT_STEP_BUTTON), the wheel's delta (INPUT_STEP_WHEEL) or the virtual key (INPUT_STEP_KEY)
	int down;   // INPUT_STEP_BUTTON, INPUT_STEP_KEY: pressed or released
	int x, y;   // INPUT_STEP_MOVE: where to
	int gap;    // passes to let run between the step before and this one
	int wait;   // .. and milliseconds, at least (the press and the release of a click: the game's tap delay)
	int follow; // a move that follows a finger or a stick (a later one replaces it)
} InputStep_t;
enum
{
	INPUT_STEP_MOVE = 1,
	INPUT_STEP_BUTTON,
	INPUT_STEP_WHEEL,
	INPUT_STEP_KEY
};
typedef struct InputSteps
{
	InputStep_t q[INPUT_STEPS];
	int head, count;
	int applying;    // an input call under way is the mapper's own
	int tapDelay;    // milliseconds a click's press waits after the move, and its release after the press (InputMap_Configure)
	uint32_t lastAt; // when the last step was applied (Clock_HostMs)
	// the left and the right button as a program reads them itself ("80 11"; Steps_ButtonRead):
	int reads[2];          // it has read the button
	uint32_t readAt[2];    // .. last then (Clock_HostMs)
	int unread[2];         // a step changed the button and the program, which reads it, has not read it since
	uint32_t changedAt[2]; // when that step was applied
} InputSteps_t;
void Steps_Move(OpenBGI_t* o, int x, int y, int gap, int follow); // the pointer to (x, y), `gap` passes after the step before
void Steps_Button(OpenBGI_t* o, int button, int down);            // a pass after the step before, where the pointer is then
void Steps_Wheel(OpenBGI_t* o, int delta);
void Steps_Click(OpenBGI_t* o, int button, int x, int y); // the pointer to (x, y), a pass, the press, a pass, the release
void Steps_Tap(OpenBGI_t* o, int button);                 // the press and the release of a click where the pointer is then
void Steps_Follow(OpenBGI_t* o, int x, int y);            // the pointer follows to (x, y): a following move still queued last goes there instead
void Steps_ButtonRead(OpenBGI_t* o, int button);          // the program read the state of OPENBGI_BUTTON_LEFT or _RIGHT itself ("80 11")
void Steps_KeyNow(OpenBGI_t* o, int vk, int down);        // a key of the mapper's at once, not queued
void Steps_KeyTap(OpenBGI_t* o, int vk);                  // its press and release at once
void Steps_TextNow(OpenBGI_t* o, const char* utf8);       // typed text of the mapper's at once (the on-screen keyboard's)
int Steps_Pending(OpenBGI_t* o);                          // steps still queued
void Steps_Frame(OpenBGI_t* o);                           // apply the steps that are due

/* ---- touch (src/sys/touch.c; OpenBGI_Touch of openbgi.h) ----
 * The contacts the host reports become a gesture, and the gesture steps
 * of the mapper's queue. */
#define TOUCH_CONTACTS 10 // fingers followed at once
typedef struct TouchContact
{
	int id;      // the host's
	int active;  // down
	int x0, y0;  // where it went down (the picture's pixels)
	int x, y;    // where it is
	uint32_t t0; // when it went down (Clock_HostMs)
} TouchContact_t;
typedef struct TouchState
{
	TouchContact_t c[TOUCH_CONTACTS];
	int mode;                 // TOUCH_MODE_* of src/sys/touch.c
	int a, b;                 // the gesture's contacts (indices into c), -1 for none
	uint32_t t0;              // when the gesture began
	int knob, knobDx, knobDy; // a drag from here takes hold of a knob, the thumb that much off the finger
	int scrollRef;            // two-finger scroll: the fingers' middle when the last notch was counted
	int ppi;                  // OpenBGI_TouchScale: the picture's pixels per inch, 0 unknown
	int drag;                 // "EF 32": a finger is the left button held where it is (TOUCH_MODE_HOLD), for a program that scrolls under it
	// the last tap, for the debugger: where it was, where it clicked, the target there
	int tapValid, tapX, tapY, tapToX, tapToY, tapKind;
	uint32_t tapOwner;
	int tapSerial; // counts the taps
} TouchState_t;
void Touch_Frame(OpenBGI_t* o);           // the start of a frame: the gestures' timers (a long press, a pointing finger at rest)
void Touch_SetDrag(OpenBGI_t* o, int on); // "EF 32": fingers as a held left button from the next one that goes down
// a pad's state as OpenBGI_Pad takes it, for any port: the host's, or the engine's own (PAD_PORT_OWN), which is no pad that "is there"
void Pad_Port(OpenBGI_t* o, int port, const OpenBGI_PadState_t* state);

/* ---- pads and actions (src/sys/pad.c; OpenBGI_Pad, OpenBGI_Action of openbgi.h) ----
 * A pad's controls are the 36 of the games' own pad keys ("81 1B", above:
 * the buttons 0 .. 31, then up, down, left, right); each does an action
 * (`bind`) unless the game gave it a key.  `mode` is what the pad drove
 * last, which decides what CONFIRM does. */
#define PAD_HOST_PORTS  4 // the host's pads (OpenBGI_Pad)
#define PAD_PORTS       5 // .. and the engine's own after them: its on-screen controls (overlay.c)
#define PAD_PORT_OWN    4
#define PAD_ARROW_MAX_W 96 // the drawn pointer at its largest
#define PAD_ARROW_MAX_H 144
enum
{
	PAD_MODE_ADVANCE, // nothing: CONFIRM goes on
	PAD_MODE_FOCUS,   // the directions put a focus on a target: CONFIRM clicks it
	PAD_MODE_POINTER  // a stick moved the pointer: CONFIRM is the left button
};
typedef struct PadMap
{
	OpenBGI_PadState_t port[PAD_PORTS]; // each pad as it was handed over last
	uint32_t attached;                  // a bit per port that has a pad
	uint32_t buttons;                   // the pads as one: the buttons down,
	uint32_t dirs;                      // .. and the directions (OPENBGI_DPAD_*): the D-pads, and for a game's own keys the left sticks
	int mode;                           // PAD_MODE_*
	int focusKind;                      // the focused target (PAD_MODE_FOCUS): its kind, owner, group and index
	uint32_t focusOwner;
	int focusGroup, focusIndex;
	uint32_t focusCheck;            // when it was looked for last (Clock_HostMs)
	int repeat;                     // the direction held (an OPENBGI_ACTION_*), 0 none,
	uint32_t repeatAt;              // .. and when it counts again
	uint32_t stickAt;               // the clock at the sticks' last step
	int32_t fracX, fracY;           // the pointer's way not yet a pixel (1/65536)
	int32_t wheelFrac;              // the wheel's turn not yet a notch (1/65536)
	int lx, ly, ry;                 // the sticks as one: the strongest of the pads
	int leftHeld, rightHeld;        // the mouse button an action holds down
	int skipKey;                    // the skip key an action holds down, 0 none
	uint16_t bind[PAD_CONTROLS];    // what each control does: 0 nothing, an OPENBGI_ACTION_*, or an INPUT_BIND_* of bgi/inputcfg.h
	int ownKeysOff;                 // the game's own pad keys ("81 1B") are not used
	int stickSpeed;                 // how fast a stick moves the pointer, percent of the usual
	int arrowFor;                   // for whom the pointer is drawn (INPUT_POINTER_* of bgi/inputcfg.h),
	int arrowSize;                  // .. and how large (INPUT_POINTER_SMALL .. _LARGE)
	int arrowShown, arrowX, arrowY; // the pointer the engine draws: where it is shown,
	int arrowW, arrowH;             // .. its picture's size (0: not drawn yet),
	int arrowFrom;                  // .. and the height it was drawn at
	uint32_t arrow[PAD_ARROW_MAX_W * PAD_ARROW_MAX_H];
} PadMap_t;
void Pad_Init(OpenBGI_t* o);                     // the bindings to start with (OpenBGI_New)
void Pad_Frame(OpenBGI_t* o);                    // the start of a frame: the directions' repeat, the sticks, the focus kept on its target
void Pad_Shown(OpenBGI_t* o);                    // after the pass: the drawn pointer where the pointer is now
void Pad_Stop(OpenBGI_t* o);                     // the run is over: the drawn pointer goes, nothing is held or focused
int Pad_HasTurn(OpenBGI_t* o);                   // a pad was used last, or one is there and nothing was used yet: what is asked comes up where a pad reaches it
int Pad_OwnsPointer(OpenBGI_t* o);               // a pad put the pointer where it is
int Pad_NearPointer(OpenBGI_t* o, int x, int y); // .. and (x, y) is nearly that place: a host's report of it is no mouse move
const char* Pad_ModeName(OpenBGI_t* o);

/* ---- the on-screen keyboard (src/sys/osk.c; openbgi.h, OpenBGI_Action) ---- */
#define OSK_COLS 17 // keys a row of the table
#define OSK_ROWS 5  // its rows; the row below has the keys OSK_KEY_*
enum
{
	OSK_PAGE_HIRA,
	OSK_PAGE_KATA,
	OSK_PAGE_WIDE,
	OSK_PAGE_HALF,
	OSK_PAGES
};
enum
{
	OSK_KEY_HIRA, // the tables
	OSK_KEY_KATA,
	OSK_KEY_WIDE,
	OSK_KEY_HALF,
	OSK_KEY_SPACE,
	OSK_KEY_LEFT, // the caret
	OSK_KEY_RIGHT,
	OSK_KEY_DELETE,
	OSK_KEY_DONE,
	OSK_KEYS
};
struct Typeface;
typedef struct Osk
{
	int up;                // it is shown
	int page;              // OSK_PAGE_*
	int row, col;          // the key the directions are on: of the table, or with row OSK_ROWS an OSK_KEY_*
	int x, y, w, h;        // where it lies in the picture
	int keyW, keyH;        // a table key's size
	uint32_t* pix;         // its picture, w x h
	struct Typeface* face; // its font; NULL when none could be had: the keys are blank
	int faceH;             // the font's cell height
	int utf8;              // the text encoding the font was opened in
	int dirty;             // it has to be painted again
	int press;             // the key the pointer's left button holds down (row * OSK_COLS + col), -1 none
	int held;              // the pointer's buttons that went down on it (a bit each): their releases are its too
	int padShown;          // the key the directions are on is marked (a pad is in use)
	int picW, picH;        // the picture size it was laid out for
	int fieldY, fieldH;    // .. and the field it was placed clear of
	int field;             // a text field had the keys at the end of the last frame (OpenBGI_Frame_t.textInput, were no keyboard up),
	int fieldEntry;        // .. the scripts' text entry (1) or a dialog's field (0),
	OpenBGI_Rect_t rect;   // .. and where it lies
	int fieldWas;          // `field` a frame earlier
	int want;              // bring it up as soon as a field has the keys (CONFIRM on the text entry)
	int tapSerial;         // the touch screen's tap that was looked at last (o->input.touch.tapSerial)
} Osk_t;
void Osk_Shown(OpenBGI_t* o, OpenBGI_Frame_t* f); // the end of a frame: whether a text field has the keys; the keyboard painted, textInput cleared while it is up
void Osk_Frame(OpenBGI_t* o);                     // before a pass: it comes up or goes
int Osk_Up(OpenBGI_t* o);
void Osk_Open(OpenBGI_t* o);                                      // show it, when a text field has the keys
void Osk_Close(OpenBGI_t* o);                                     // put it away
void Osk_Action(OpenBGI_t* o, int action, int down);              // an action while it is up
int Osk_Button(OpenBGI_t* o, int button, int down, int x, int y); // a press or release of the pointer: 1 when it is the keyboard's
int Osk_Over(OpenBGI_t* o, int x, int y);                         // the keyboard lies at (x, y)
void Osk_Free(OpenBGI_t* o);                                      // OpenBGI_Stop, OpenBGI_Delete

// ---- which device the player uses (src/sys/pad.c) --------------------------------------------
enum
{
	INPUT_DEV_NONE,
	INPUT_DEV_MOUSE,
	INPUT_DEV_KEYBOARD,
	INPUT_DEV_TOUCH,
	INPUT_DEV_PAD
};
/* An input call of the host's own (not the mapper's, InputSteps_t.applying)
 * says which device the player took up: a pad's focus and drawn pointer go
 * with a mouse, a keyboard or a touch, the on-screen keyboard with a key. */
void Input_DeviceUsed(OpenBGI_t* o, int device);
void InputMap_Frame(OpenBGI_t* o); // the start of a frame that runs a pass: Touch_Frame, Pad_Frame, Osk_Frame, Steps_Frame
void InputMap_Stop(OpenBGI_t* o);  // OpenBGI_Stop, OpenBGI_Delete: the keyboard and the drawn pointer go, the steps are dropped
/* A game starts: the input settings of openbgi.json (bgi/inputcfg.h) - the
 * ones for every game, then the game's own, either may be NULL - over the
 * built-in bindings and what the host says holds unless they say
 * otherwise (its "defaults"), and the host's (OpenBGI_InputSettings) over
 * those.
 * The two stay in use, without being copied, until the next call. */
void InputMap_Configure(OpenBGI_t* o, const struct InputSettings* all, const struct InputSettings* game);
void InputMap_Free(OpenBGI_t* o); // OpenBGI_Delete: the host's settings go

// ---- cursor (src/sys/cursor.c) ------------------------------------------

void GetMouseClientPos(OpenBGI_t* o, int xy[2]);                                     // "80 08": back-buffer coordinates; 0,0 without a window
void MouseMove_Start(OpenBGI_t* o, int x, int y, int steps, int a4, int a5, int a6); // "80 1F": (x, y, curve, duration ms, fps, cancelOnMove)
void SetCursorAutoHide(OpenBGI_t* o, uint32_t ms);                                   // "B0 05"
int Cursor_IsVisible(OpenBGI_t* o);                                                  // "B0 06": not auto-hidden
void Cursor_Refresh(OpenBGI_t* o);                                                   // show again before a dialog
void Cursor_Update(OpenBGI_t* o);                                                    // per pass
int SetCursorSprite(OpenBGI_t* o, uint32_t sprite, int dx, int dy);                  // "B0 04": 0 ok, -1 the sprite cannot be shown
void CursorSprite_Update(OpenBGI_t* o);                                              // per pass
int ShowCursorCount(OpenBGI_t* o, int show);                                         // returns the previous state
int Cursor_IsShown(OpenBGI_t* o);

// a key's entry of the key state table (o->input.keyState, 256 of them)
typedef struct KeyRec
{
	int32_t down;      // currently pressed
	int32_t consumed;  // the press was reported by GetKeyTrigger
	int32_t pending;   // presses not yet reported
	int32_t counter;   // presses since KeyState_Clear
	uint32_t repeatAt; // tick at which auto-repeat starts
	int32_t repeatNew; // from 1.69 build 472 ("81 10"): every WM_KEYDOWN counts as a new press
} KeyRec_t;

#endif // BGI_INPUT_H_
