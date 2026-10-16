/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * panel.h - script-driven button panels attached to message windows: the
 *           ObjProc controller base and its registry (src/sys/objproc.c),
 *           the button panel (src/sys/panel.c) and the sprite panel
 *           (src/sys/sprpanel.c)
 *
 * An ObjProc is a small controller object hooked into a display object's
 * `proc` slot and addressed by the script through an id ("90 B8" / "91 B8"
 * create one, "90 B9" deletes it, "80 A8" / "80 A9" / "80 AC" talk to it).
 * The two concrete kinds are panels: a set of bitmap buttons in groups laid
 * over a window, driven by mouse and keyboard, reporting through an event
 * queue the script drains with "90 BF".
 *
 *   ButtonPanel  "90 BA": buttons are pixels in the window's text layer
 *                         plus one virtual hit object each
 *   SpritePanel  "91 BA": buttons are the window's rotatable sub-sprites,
 *                         with frame animation and swing
 *
 * The record tables the script hands over are copied (Panel_CopyDesc,
 * SpritePanel_CopyDesc) with their tagged pointers resolved; the record
 * structures below give the field-by-field account.
 *
 * The original resolves the tagged pointers in place: its copies keep the
 * script layout and store host pointers in the 32-bit pointer fields.  The
 * reimplementation must run on 64-bit hosts, so the script layout
 * (*Script_t, read from script memory) and the host-side copy (*_t, with
 * real pointers and widened counts) are separate types; the field order
 * and meaning are the same.
 */
#ifndef BGI_PANEL_H_
#define BGI_PANEL_H_

#include "bgi/common.h"
#include "bgi/gfx/dispobj.h"

struct Thread;
struct Window;
struct Gfx;
struct BmpMgr;

// ---- the records in script memory ---------------------------------------

typedef struct PanelButton // one button of a button panel (0x3C bytes, in script memory and in the copies)
{
	int32_t enabled;     // 0 = the button does not exist (not drawn, not hittable)
	int32_t x, y;        // position (pixels) relative to the window's text area
	int32_t bmpNormal;   // the picture shown normally; -1 = none (the button is not drawn)
	int32_t bmpSelected; // the picture while it is the group's selection; -1 = the normal one
	int32_t bmpFocus;    // the picture while the pointer is on it; -1 = none
	int32_t hitMaskBmp;  // -2 cannot be hit, a bitmap number = hit by its pixel mask, else by the rectangle
	int32_t ov[2][3];    // two overlays (x, y, bitmap) for window items 2 and 3 while focused
	int32_t unused34;    // copied, never read (the sprite panel's record has `noRedecide` here)
	uint32_t hotkey;     // virtual key that decides the button (low 31 bits); bit 31: also on auto-repeat
} PanelButton_t;

typedef struct PanelGroupScript // one group of a button panel (0x34 bytes, in script memory)
{
	uint16_t count;       // buttons, 1..0x100
	uint16_t columns;     // columns of the grid the cursor keys move in, clamped to 1..count
	uint32_t buttons;     // tagged pointer to PanelButton[count]
	int32_t selected;     // the selected button at start; -1 none
	int32_t selectable;   // the group can become the current group
	int32_t hoverSelects; // moving the pointer onto a button selects it
	int32_t clickDecides; // a held left button repeats the click
	int32_t linkId;       // groups sharing an id (!= -1) are mutually exclusive; -1 = unlinked
	int32_t ov[2][3];     // two overlays (x, y, bitmap) for window items 0 and 1 while current
} PanelGroupScript_t;

typedef struct PanelDescScript // the descriptor of a button panel (0x20 bytes, in script memory)
{
	int32_t groupCount;     // 1..0x100
	uint32_t groups;        // tagged pointer to PanelGroup[groupCount]
	int32_t currentGroup;   // the current group at start; out of range = none
	int32_t modal;          // the panel takes its own key and mouse layers and the input focus
	int32_t mouse;          // (without modal) the window joins the mouse layer stack
	uint32_t inputMask;     // input bits the panel ignores
	int32_t keyMap;         // & 7: 0..3 built in, 4..7 installed with "91 BF" (panel.c)
	int32_t selectOnDecide; // deciding a button with the mouse also selects it
} PanelDescScript_t;

// the host-side copies (Panel_CopyDesc): the same fields with real pointers
typedef struct PanelGroup
{
	int32_t count;          // widened from the 16-bit field
	int32_t columns;        // widened from the 16-bit field; zeroed once the panel built its grid
	PanelButton_t* buttons; // own copy of the button table
	int32_t selected;       // the current selection once the panel runs
	int32_t selectable;
	int32_t hoverSelects;
	int32_t clickDecides;
	int32_t linkId;
	int32_t ov[2][3];
} PanelGroup_t;

typedef struct PanelDesc
{
	int32_t groupCount;
	PanelGroup_t* groups; // own copy of the group table
	int32_t currentGroup;
	int32_t modal;
	int32_t mouse;
	uint32_t inputMask;
	int32_t keyMap;
	int32_t selectOnDecide;
} PanelDesc_t;

/* the sprite panel's records: the same roles, different layout; each
 * button is a sub-sprite of the window */
typedef struct SpriteButton // one button of a sprite panel (0xC4 bytes)
{
	int32_t enabled;        // the button can be hit, selected and decided
	int32_t shown;          // drawn (but not hittable) although disabled
	int32_t x, y;           // position of the hit object (pixels, relative to the window, not to its text area)
	int32_t ox, oy;         // rotation centre offset; the sub-sprite sits at (x + ox, y + oy) with pivot (ox, oy)
	int32_t frames;         // animation frames (consecutive bitmap numbers from the shown picture on); 0 / 1 = none
	int32_t frameMs;        // milliseconds per frame; 0 = no animation
	int32_t bmpNormal;      // the picture shown normally
	int32_t bmpFocus;       // while the pointer is on it; -1 = none
	int32_t bmpSelected;    // while it is the group's selection; -1 = none
	int32_t bmpSelFocus;    // while selected and focused; -1 = none
	int32_t hitMaskBmp;     // as PanelButton_t
	int32_t noRedecide;     // the button cannot be decided while it is the selection
	int32_t swingPhases;    // phases of the swing (up to 8); 0 = no swing
	int32_t swing[8][3];    // per phase: (angle turned, 16.16 degrees; duration, ms; unused)
	int32_t swingAngle0;    // start angle (16.16 degrees)
	int32_t swingFocusOnly; // swing only while the pointer is on the button
	int32_t swingPause;     // losing the pointer pauses the swing instead of resetting it
	int32_t ov[2][3];       // two overlays (x, y, bitmap) for window items 2 and 3 while focused
	/* unused up to 1.69; flags from 1.494 on: 0x02 the sub-sprite's priority
	 * is the button's y, 0x10 (1.588 on) it is the `shown` value, 0x40
	 * (1.640 on) the button is never drawn; from 1.588 on a button whose
	 * `shown` is 0 is not drawn either */
	int32_t flags;
} SpriteButton_t;

typedef struct SpriteGroupScript // one group of a sprite panel (0x40 bytes, in script memory)
{
	int32_t count;    // buttons; SpritePanel_CopyDesc validates the low 16 bits against 1..0x100
	int32_t columns;  // columns of the grid the cursor keys move in, clamped to 1..count
	uint32_t buttons; // tagged pointer to SpriteButton[count]
	int32_t selected; // the selected button at start; -1 none
	int32_t unused10;
	int32_t selectable; // as PanelGroupScript_t
	int32_t hoverSelects;
	int32_t clickDecides;
	int32_t linkId;
	int32_t ov[2][3]; // two overlays (x, y, bitmap) for window items 0 and 1 while current
	int32_t unused3C;
} SpriteGroupScript_t;

typedef struct SpriteDescScript // the descriptor of a sprite panel (0x28 bytes, in script memory)
{
	int32_t groupCount;   // 1..0x100
	uint32_t groups;      // tagged pointer to SpriteGroup[groupCount]
	int32_t currentGroup; // as PanelDescScript_t
	int32_t modal;
	int32_t mouse;
	uint32_t inputMask;
	int32_t keyMap;
	int32_t selectOnDecide;
	int32_t inputDisabled; // start with the input switched off (message 0x10000003 of "80 AC" turns it on)
	int32_t unused24;
} SpriteDescScript_t;

// the host-side copies (SpritePanel_CopyDesc)
typedef struct SpriteGroup
{
	int32_t count;
	int32_t columns;
	SpriteButton_t* buttons; // own copy of the button table
	int32_t selected;        // the current selection once the panel runs
	int32_t unused10;
	int32_t selectable;
	int32_t hoverSelects;
	int32_t clickDecides;
	int32_t linkId;
	int32_t ov[2][3];
	int32_t unused3C;
} SpriteGroup_t;

typedef struct SpriteDesc
{
	int32_t groupCount;
	SpriteGroup_t* groups; // own copy of the group table
	int32_t currentGroup;
	int32_t modal;
	int32_t mouse;
	uint32_t inputMask;
	int32_t keyMap;
	int32_t selectOnDecide;
	int32_t inputDisabled;
	int32_t unused24;
} SpriteDesc_t;

// ---- ObjProc: the controller base (src/sys/objproc.c) ------------------------

typedef struct ObjProc ObjProc_t;

typedef struct ProcMsg // a queued "80 AC" message: n dwords
{
	int n;          // 1..0x100
	uint32_t* data; // own copy of the dwords
	struct ProcMsg* next;
} ProcMsg_t;

typedef struct ObjProcVtbl
{
	void (*destroy)(ObjProc_t* p);                               // deleting destructor
	int (*poll)(ObjProc_t* p);                                   // per pass; non-zero stops Panel_PollAll
	int (*onMessage)(ObjProc_t* p, int n, const uint32_t* data); // a message with data[0] != 0 (0 is "set enabled")
} ObjProcVtbl_t;

struct ObjProc
{
	const ObjProcVtbl_t* vt;
	uint32_t id;       // from gProcSerial; what the script addresses
	int kind;          // 0x80 for the panels
	DispObj_t* obj;    // the display object whose proc slot this is
	int enabled;       // "80 A8" / "80 A9", or message 0 of "80 AC"
	int dirty;         // drew something since the last flush
	ProcMsg_t msgHead; // dummy head of the message queue; msgHead.next is the first message
};

extern struct Gfx* gPanelGfx;       // the graphics manager the panels draw through
extern struct BmpMgr* gPanelBmpMgr; // the bitmap manager the button bitmap numbers refer to
extern uint32_t gProcSerial;        // the last id handed out
extern int gProcPresent;            // a dirty proc asks for a present

void ObjProc_Ctor(ObjProc_t* p, int kind, DispObj_t* obj);          // takes the object's proc slot, assigns the next id
void ObjProc_Dtor(ObjProc_t* p);                                    // drops the pending messages, leaves the proc slot
int ObjProc_PostMessage(ObjProc_t* p, int n, const uint32_t* data); // "80 AC": queue n (1..0x100) dwords; 1 ok, 0 bad count
void ObjProc_ProcessMessages(ObjProc_t* p);                         // deliver the queued messages (the start of a poll)
void ObjProc_MarkDirty(ObjProc_t* p);
void ObjProc_Flush(ObjProc_t* p);                           // a dirty proc asks for a present (gProcPresent) and is clean again
int ObjProc_IsVisible(ObjProc_t* p);                        // the object is not above the draw limit
static inline void ObjProc_SetEnabled(ObjProc_t* p, int on) // "80 A8" (the original folds this with DispObj_SetPriority)
{
	p->enabled = on;
}
static inline int ObjProc_GetEnabled(ObjProc_t* p) // "80 A9" (the original folds this with DispObj_GetPriority)
{
	return p->enabled;
}

// ---- the registry of procs by id (src/sys/objproc.c) --------------------------
ObjProc_t* SubObj_Find(uint32_t id); // NULL for an unknown id
int SubObj_Delete(uint32_t id);      // "90 B9": destroy the proc; 1 ok, 0 unknown id
void Panel_DeleteAll(void);          // destroy every proc
int Panel_PollAll(void);             // poll every proc once per pass; 0 when a poll asked to stop
int Panel_SetPollMode(int mode);     // "80 AF" (1.573 on): 0 poll before the input of a pass, 1 after the pass; 1 ok
int Panel_GetPollMode(void);
void Panel_PumpAll(void);                     // deliver the messages of every proc without polling (mode 1's early pass)
int Gfx_ObjHasProc(uint32_t h);               // "90 51", "90 81": whether object h has a proc
uint32_t SubObj_Create(uint32_t h, int kind); // "90 B8" (kind 0, button panel), "91 B8" (kind 1, sprite panel): the id, 0 for a bad kind

// ---- the panels ------------------------------------------------------------------
typedef struct ButtonPanel ButtonPanel_t;

typedef struct PanelInfo // one entry per button, in group order (the "flat index")
{
	int hasBitmap;         // the button is drawn (and hittable)
	int group, index;      // the button's group and its index in the group
	PanelButton_t* button; // its record in the panel's own group tables
	DispObj_t* virt;       // the virtual hit object; NULL without a bitmap
} PanelInfo_t;

typedef struct PanelEvent // a queued event: what "90 BF" reads (the messages are listed in panel.c)
{
	uint32_t msg, a, b;
	struct PanelEvent* next;
} PanelEvent_t;

/* the hooks a panel class fills in; the button panel's defaults are in
 * panel.c, the sprite panel overrides most of them in sprpanel.c */
typedef struct PanelVtbl
{
	ObjProcVtbl_t base;
	void (*animate)(ButtonPanel_t* p);                                                  // the start of every update
	int (*finishOnDecide)(ButtonPanel_t* p);                                            // whether a decision ends the panel
	int (*setFocus)(ButtonPanel_t* p, int btn, int force);                              // the focus to flat index btn (-1 none); 1 when redrawn
	int (*getBitmap)(ButtonPanel_t* p, int32_t* out, int g, int i, int focus, int sel); // the picture of (g, i) in that state; 1 ok
	int (*setSelection)(ButtonPanel_t* p, int g, int i);                                // select (g, i), -1 clears; 1 when it changed
	int (*canDecide)(ButtonPanel_t* p, int g, int i);                                   // whether (g, i) may be decided now
	int (*decideAt)(ButtonPanel_t* p, int g, int i, int flag);                          // record a decision by (group, index); 1 ok
	int (*decide)(ButtonPanel_t* p, int btn, int flag);                                 // record a decision by flat index, -1 cancel; 1 ok
	void (*stop)(ButtonPanel_t* p);                                                     // release the tables and the input layers
	void (*clickOutside)(ButtonPanel_t* p);                                             // a click with no button under the pointer
} PanelVtbl_t;
#define PANEL_VT(p) ((const PanelVtbl_t*)(p)->p.vt)

struct ButtonPanel
{
	ObjProc_t p;        // the controller base
	int isSprite;       // 1 for a sprite panel
	struct Window* win; // the window the panel is attached to
	DispObj_t* virt;    // size-less virtual child: the window's mouse-layer entry
	int started;        // the panel is running (between Start and its end)
	int groupCount;
	PanelGroup_t* groups; // own copies of the groups and their button tables
	int currentGroup;     // the group the keys move in; -1 none
	int modal, mouse;     // as PanelDescScript_t
	uint32_t inputMask;   // input bits the panel ignores
	int keyMap;           // 0..7
	int selectOnDecide;
	int32_t (*grid)[2];                          // {columns, rows} per group
	int total;                                   // buttons over all groups
	PanelInfo_t* buttons;                        // `total` entries, in group order
	int32_t clickX, clickY;                      // pointer offset inside the hit button at the last hit test (-1, -1 for a miss)
	int decidedGroup, decidedIndex, decidedFlag; // the last decision (-1, -1 for a cancel) and its flag
	int focus;                                   // flat index of the button under the pointer, -1 none
	int32_t lastMouse[2];                        // pointer position of the last hover selection
	int hover;                                   // flat index last reported with event 0x10000001
	int inputEnabled;                            // message 0x10000003 of "80 AC"
	uint32_t input;                              // this pass's input bits
	PanelEvent_t evHead;                         // dummy head of the event queue; evHead.next is the first event
};

// panel.c internals shared with the sprite panel (described in panel.c)
void ButtonPanel_Ctor(ButtonPanel_t* p, struct Window* win);
void ButtonPanel_Dtor(ButtonPanel_t* p);
int ButtonPanel_Poll(ObjProc_t* p);
int ButtonPanel_OnMessage(ObjProc_t* p, int n, const uint32_t* data);
int ButtonPanel_HitTest(ButtonPanel_t* p, int32_t* outOffset, int useMask);
int ButtonPanel_HitTestStore(ButtonPanel_t* p); // masked, records the click offset
DispObj_t* Panel_MakeHitObject(ButtonPanel_t* p, int x, int y, const Bmp_t* bmp, int maskBmp);
int ButtonPanel_SetCurrentGroup(ButtonPanel_t* p, int g);
int ButtonPanel_ApplyLink(ButtonPanel_t* p, int g);
int ButtonPanel_CheckHotkeys(ButtonPanel_t* p);
int ButtonPanel_Decide(ButtonPanel_t* p, int btn, int flag);
int ButtonPanel_DecideAt(ButtonPanel_t* p, int g, int i, int flag);
void ButtonPanel_Stop(ButtonPanel_t* p);
void ButtonPanel_PushEvent(ButtonPanel_t* p, uint32_t msg, uint32_t a, uint32_t b);
int ButtonPanel_DropEvent(ButtonPanel_t* p);
void ButtonPanel_DrawOverlays(ButtonPanel_t* p, int firstItem, int32_t ov[2][3], const Rect_t* area);
void ButtonPanel_DirtyItem(ButtonPanel_t* p, int item);
void ButtonPanel_FinishStart(ButtonPanel_t* p, int selectedAtStart); // the input-layer tail of both Start()s
int ButtonPanel_Update(ButtonPanel_t* p);                            // one pass; 1 when the panel is finished
void ButtonPanel_GetState(ButtonPanel_t* p, int32_t out[6]);         // "90 BC"
int ButtonPanel_GetSelections(ButtonPanel_t* p, int32_t* out);       // "90 BE": the group count
int ButtonPanel_PopEvent(ButtonPanel_t* p, uint32_t out[3]);         // "90 BF": 1 when there was an event
static inline int ButtonPanel_IsSprite(ButtonPanel_t* p)
{
	return p->isSprite;
}
static inline int ButtonPanel_CurrentGroup(ButtonPanel_t* p) // "90 BD"
{
	return p->currentGroup;
}
const uint32_t* Panel_UserKeyMap(int no); // objproc.c: user map no (4 .. 7)

// the key maps: 24 actions (enum PanelAction of panel.c), one per standard-key bit
void Panel_InitKeyMaps(void);                         // clear the user maps on first use
int Panel_SetKeyMap(int no, const uint32_t* actions); // "91 BF": install user map no (4..7); 1 accepted, 0 bad number
void Panel_SetRequireActive(int on);                  // panels only react while the main window is active
int Panel_Allowed(void);                              // whether the panels may react now (see above)

ButtonPanel_t* ButtonPanel_New(struct Window* win);                  // allocate a button panel on win
ButtonPanel_t* SpritePanel_New(struct Window* win);                  // allocate a sprite panel on win
uint32_t ButtonPanel_Start(ButtonPanel_t* p, const PanelDesc_t* d);  // "90 BA": 0 ok / 0x80000001 bad group count / 0x80000002 bad button count
uint32_t SpritePanel_Start(ButtonPanel_t* p, const SpriteDesc_t* d); // "91 BA": the same results
uint32_t Panel_Paint(struct Window* w, const PanelDesc_t* d);        // "90 B6": paint without a panel; the same results
uint32_t SpritePanel_Paint(struct Window* w, const SpriteDesc_t* d); // "90 B7"

// the script entry points (objproc.c), by proc id
void Panel_FreeDesc(PanelDesc_t* d);
int Panel_CopyDesc(PanelDesc_t** out, const PanelDescScript_t* d, struct Thread* t); // 0 ok / 2 bad group table / 3 bad button table
void SpritePanel_FreeDesc(SpriteDesc_t* d);
int SpritePanel_CopyDesc(SpriteDesc_t** out, const SpriteDescScript_t* d, struct Thread* t); // the same results
int Panel_Start(uint32_t id, const void* desc, struct Thread* t);                            // "90 BA": 0 ok / 1 no panel with that id / 2, 3 as CopyDesc / 4 a sprite panel
int SprPanel_Start(uint32_t id, const void* desc, struct Thread* t);                         // "91 BA": the same, 4 for a button panel
int SubObj_GetCursor(int32_t out[6], uint32_t id);                                           // "90 BC": ButtonPanel_GetState; 1 when id is a panel
int SubObj_GetGroup(int32_t* out, uint32_t id);                                              // "90 BD": the current group (-1 none); 1 when id is a panel
int SubObj_GetSelections(int32_t* out, uint32_t id);                                         // "90 BE": ButtonPanel_GetSelections; 1 when id is a panel
int SubObj_PopEvent(uint32_t out[3], uint32_t id);                                           // "90 BF": ButtonPanel_PopEvent (zeros for an empty queue); 1 when id is a panel
int Panel_Draw(uint32_t h, const void* desc, struct Thread* t);                              // "90 B6": 0 ok / 1 no such window / 2 / 3 as CopyDesc
int SprPanel_Draw(uint32_t h, const void* desc, struct Thread* t);                           // "90 B7": the same results

#endif // BGI_PANEL_H_
