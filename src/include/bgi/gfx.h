/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * gfx.h - the graphics subsystem as seen from the VM core.
 *
 * The full object model (bitmaps, display objects, sprites, windows,
 * effectors, the compositor) is declared in the headers under src/include/bgi/gfx/;
 * this file collects the handful of entry points the scheduler and the
 * system-control instructions need, so that src/vm/ does not depend on the
 * renderer's internals: the creation and teardown of the manager set, the
 * movie player, the per-pass services of the particle and rain screens
 * and the knob registry of the main window.
 */
#ifndef BGI_GFX_H_
#define BGI_GFX_H_

#include "bgi/common.h"
#include "bgi/gfx/gfxmgr.h"

struct Thread;
struct Bitmap;

// ---- the manager itself ------------------------------------------------
/* o->gfx.gfx and every Gfx_* method are declared in gfx/gfxmgr.h.  The
 * window code calls them directly with o->gfx.gfx, as the original does. */

// the cdecl wrappers used by the opcode handlers and the cursor code
#include "bgi/gfx/gfxcall.h"

// ---- life cycle ---------------------------------------------------------
/* the graphics half of the engine's core set-up: the worker pool, the
 * graphics manager o->gfx.gfx and the bitmap manager o->gfx.bmpMgr;
 * bandPixels is the compositor band size derived from the CPU cache,
 * cpus the number of worker threads */
void Gfx_Create(OpenBGI_t* o, uint32_t bandPixels, int cpus);
/* tear down o->gfx.gfx, o->gfx.bmpMgr, the archive manager, the static
 * caches of the text engine and the loader, and the worker pool */
void Gfx_Destroy(OpenBGI_t* o);

void Blit_InitMmxTables(void);       // the scalers' MMX weight table (blit_scale.c); once at start-up
void Ptcl_InitGlobals(OpenBGI_t* o); // the particle pattern tables; once per process
void Ptcl_FreeGlobals(OpenBGI_t* o); // free them at shutdown
// the module copies of the manager pointers (image decoders / particles)
void Panel_SetGfxPtr(OpenBGI_t* o, struct Gfx* g);
void Panel_SetBmpMgrPtr(OpenBGI_t* o, struct BmpMgr* m);
void Ptcl_SetBmpMgrPtr(OpenBGI_t* o, struct BmpMgr* m);
void Pattern_SetBmpMgrPtr(OpenBGI_t* o, struct BmpMgr* m);

//  --- movies ("90 F0" .. "90 F3"; src/gfx/movie.c) --

/* play `path` in the (x, y, w, h) rectangle of the window; 1 when
 * playback started (*outLength receives the length in ms), 0 otherwise.
 * A missing audio device or renderer is a script error. */
int Movie_Play(OpenBGI_t* o, int32_t* outLength, const char* path, int x, int y, int w, int h);
int Movie_Stop(OpenBGI_t* o); // 1 when there was a graph to release
int Movie_IsPlaying(OpenBGI_t* o);
int Movie_SetVolume(OpenBGI_t* o, uint32_t v);   // 0 .. 0x80; 0 for anything above
void Movie_OnEvent(OpenBGI_t* o, int completed); // the player's WM_APP notification (OpenBGI_MovieEvent)
void Movie_Tick(OpenBGI_t* o);                   // every pass: the decoded movie's frame, and its end

/* The movies the host decodes and the engine shows (src/sys/moviefx.c,
 * OpenBGI_MovieDecoder_t): used when the host's player cannot play the
 * file, or the host has none. */
int MovieFx_Start(OpenBGI_t* o, const char* path, int x, int y, int w, int h, int32_t volume, int32_t* lengthMs);
void MovieFx_Stop(OpenBGI_t* o);
int MovieFx_IsRunning(OpenBGI_t* o);
void MovieFx_SetVolume(OpenBGI_t* o, int32_t volume);
int MovieFx_Tick(OpenBGI_t* o); // 1 while running, 0 when it just ended or nothing runs

// ---- manager-wide operations --------------------------------------------
void Gfx_ResetAll(OpenBGI_t* o);            // reset + free every bitmap slot
void BmpMgr_SetOption(OpenBGI_t* o, int v); // "90 0B": re-created bitmap slots keep their generation

// ---- per-pass services (the frame services of src/vm/sched.c) -------------
void Ptcl_UpdateAll(OpenBGI_t* o); // bring every particle engine up to the present
void Ptcl_AutoClear(OpenBGI_t* o); // forget the automatic redraw list ("C0 09")
void Ptcl_AutoTick(OpenBGI_t* o);  // redraw the screens on that list when due
void Rain_UpdateAll(OpenBGI_t* o); // run the stages of every rain screen
void Rain_FrameTick(OpenBGI_t* o); // redraw the rain screens at the effect frame rate
// Panel_DeleteAll / Panel_PollAll: panel.h

// the knob (slider) registry of the main window (src/sys/knobreg.c)
typedef struct KnobReg KnobReg_t;
struct KnobReg // a registered knob (o->input.knobHead is the list's dummy head)
{
	uint32_t handle;      // the knob's object handle
	DispObj_t* knob;      // the knob object
	uint32_t sortKey;     // the knob's layer key, the list is sorted by it
	int changed;          // a right click happened; "90 DB" reports and clears it
	struct KnobReg* next; // lower keys follow
};
int Knob_Register(OpenBGI_t* o, uint32_t h);                   // "90 D0": 1 ok, 0 not a knob
int Knob_Unregister(OpenBGI_t* o, uint32_t h);                 // "90 D1": 1 if it was registered
KnobReg_t* Knob_UnderMouse(OpenBGI_t* o);                      // the registered knob under the mouse, NULL when none
void Knob_BeginDrag(OpenBGI_t* o, KnobReg_t* k, int x, int y); // start a drag at (x, y); NULL ends the drag
KnobReg_t* Knob_ActiveDrag(OpenBGI_t* o);                      // the knob being dragged, NULL when none
void Knob_MarkChanged(KnobReg_t* k);                           // k->changed = 1 (right button, window procedure)
DispObj_t* Knob_TopCaptured(OpenBGI_t* o);                     // the most recently captured knob, NULL when none
int Knob_NudgeCaptured(OpenBGI_t* o, int down);                // the wheel moves the top captured knob a step; 1 if a knob is captured
void Knob_UnregisterAll(OpenBGI_t* o);                         // drop every registration (shutdown, restart)
void Knob_ReleaseAll(OpenBGI_t* o);                            // empty the captured list (shutdown)
int Knob_Release(OpenBGI_t* o, uint32_t h);                    // "90 DF": 1 if it was captured
int Knob_Capture(OpenBGI_t* o, uint32_t h);                    // "90 DE": 1 if the handle is a knob
uint32_t Knob_PollChanged(OpenBGI_t* o);                       // "90 DB": last flagged handle, flags cleared
uint32_t Knob_DragHandle(OpenBGI_t* o);                        // "91 DB" (1.588 on): the handle of the knob being dragged, 0 when none
void Knob_SetGlobal(OpenBGI_t* o, int on);                     // "90 DD": the wheel goes to the captured knob
int Knob_GetGlobal(OpenBGI_t* o);

void Knob_Update(OpenBGI_t* o); // per pass: the dragged knob follows the mouse

// icon / menu / panel selection requires the window to be active ("90 AF")
void Select_RequireActive(OpenBGI_t* o, int on); // the three below
void IconSelect_SetRequireActive(OpenBGI_t* o, int on);
void MenuSelect_SetRequireActive(OpenBGI_t* o, int on);
void Panel_SetRequireActive(OpenBGI_t* o, int on);

// a knob that captured the wheel ("90 DD"; o->input.captureHead, src/sys/knobreg.c)
typedef struct KnobCapture
{
	DispObj_t* knob;
	struct KnobCapture* next; // captured earlier
} KnobCapture_t;

#endif // BGI_GFX_H_
