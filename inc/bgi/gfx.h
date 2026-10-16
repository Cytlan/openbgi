/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * gfx.h - the graphics subsystem as seen from the VM core.
 *
 * The full object model (bitmaps, display objects, sprites, windows,
 * effectors, the compositor) is declared in the headers under inc/bgi/gfx/;
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
/* gGfx and every Gfx_* method are declared in gfx/gfxmgr.h.  The window
 * code calls them directly with gGfx, as the original does. */

// the cdecl wrappers used by the opcode handlers and the cursor code
#include "bgi/gfx/gfxcall.h"

// ---- life cycle ---------------------------------------------------------
/* the graphics half of the engine's core set-up: the worker pool, the
 * graphics manager gGfx and the bitmap manager gBmpMgr; bandPixels is the
 * compositor band size derived from the CPU cache, cpus the number of
 * worker threads */
void Gfx_Create(uint32_t bandPixels, int cpus);
/* tear down gGfx, gBmpMgr, the archive manager, the static caches of the
 * text engine and the loader, and the worker pool */
void Gfx_Destroy(void);
extern struct WorkerPool* gWorkerPool;
void Blit_InitMmxTables(void); // the scalers' MMX weight table (blit_scale.c); once at start-up
void Ptcl_InitGlobals(void);   // the particle pattern tables; once per process
void Ptcl_FreeGlobals(void);   // free them at shutdown
// the module copies of the manager pointers (image decoders / particles)
void Panel_SetGfxPtr(struct Gfx* g);
void Panel_SetBmpMgrPtr(struct BmpMgr* m);
void Ptcl_SetBmpMgrPtr(struct BmpMgr* m);
void Pattern_SetBmpMgrPtr(struct BmpMgr* m);

//  --- movies ("90 F0" .. "90 F3"; src/gfx/movie.c) --
extern int gMoviePlaying; // a movie is running (the window paints black, no style toggle)
/* play `path` in the (x, y, w, h) rectangle of the window; 1 when
 * playback started (*outLength receives the length in ms), 0 otherwise.
 * A missing audio device or renderer is a script error. */
int Movie_Play(int32_t* outLength, const char* path, int x, int y, int w, int h);
int Movie_Stop(void); // 1 when there was a graph to release
int Movie_IsPlaying(void);
int Movie_SetVolume(uint32_t v);   // 0 .. 0x80; 0 for anything above
void Movie_OnEvent(int completed); // the WM_APP notification of the window procedure
void Movie_Tick(void);             // every pass: the ffmpeg player's frame, and its end

/* The ffmpeg player (src/sys/moviefx.c): used when the OS layer's own
 * player cannot play the file - always on POSIX.  Needs the ffmpeg
 * program (and ffprobe for the length) on the PATH or beside the binary. */
int MovieFx_Start(const char* path, int x, int y, int w, int h, int32_t volume, int32_t* lengthMs);
void MovieFx_Stop(void);
int MovieFx_IsRunning(void);
void MovieFx_SetVolume(int32_t volume);
int MovieFx_Tick(void); // 1 while running, 0 when it just ended or nothing runs

// ---- manager-wide operations --------------------------------------------
void Gfx_ResetAll(void);      // reset + free every bitmap slot
void BmpMgr_SetOption(int v); // "90 0B": re-created bitmap slots keep their generation

// ---- per-pass services called from Vm_FrameServices ---------------------
void Ptcl_UpdateAll(void); // bring every particle engine up to the present
void Ptcl_AutoClear(void); // forget the automatic redraw list ("C0 09")
void Ptcl_AutoTick(void);  // redraw the screens on that list when due
void Rain_UpdateAll(void); // run the stages of every rain screen
void Rain_FrameTick(void); // redraw the rain screens at the effect frame rate
// Panel_DeleteAll / Panel_PollAll: panel.h

// the knob (slider) registry of the main window (src/sys/knobreg.c)
typedef struct KnobReg KnobReg_t;
int Knob_Register(uint32_t h);                   // "90 D0": 1 ok, 0 not a knob
int Knob_Unregister(uint32_t h);                 // "90 D1": 1 if it was registered
KnobReg_t* Knob_UnderMouse(void);                // the registered knob under the mouse, NULL when none
void Knob_BeginDrag(KnobReg_t* k, int x, int y); // start a drag at (x, y); NULL ends the drag
KnobReg_t* Knob_ActiveDrag(void);                // the knob being dragged, NULL when none
void Knob_MarkChanged(KnobReg_t* k);             // k->changed = 1 (right button, window procedure)
DispObj_t* Knob_TopCaptured(void);               // the most recently captured knob, NULL when none
int Knob_NudgeCaptured(int down);                // the wheel moves the top captured knob a step; 1 if a knob is captured
void Knob_UnregisterAll(void);                   // drop every registration (shutdown, restart)
void Knob_ReleaseAll(void);                      // empty the captured list (shutdown)
int Knob_Release(uint32_t h);                    // "90 DF": 1 if it was captured
int Knob_Capture(uint32_t h);                    // "90 DE": 1 if the handle is a knob
uint32_t Knob_PollChanged(void);                 // "90 DB": last flagged handle, flags cleared
uint32_t Knob_DragHandle(void);                  // "91 DB" (1.588 on): the handle of the knob being dragged, 0 when none
void Knob_SetGlobal(int on);                     // "90 DD": the wheel goes to the captured knob
int Knob_GetGlobal(void);
extern int gKnobGlobal;
void Knob_Update(void); // per pass: the dragged knob follows the mouse

// icon / menu / panel selection requires the window to be active ("90 AF")
void Select_RequireActive(int on); // the three below
void IconSelect_SetRequireActive(int on);
void MenuSelect_SetRequireActive(int on);
void Panel_SetRequireActive(int on);

#endif // BGI_GFX_H_
