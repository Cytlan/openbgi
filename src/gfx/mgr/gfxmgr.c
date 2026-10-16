/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * gfxmgr.c - the graphics manager Gfx_t (inc/bgi/gfx/gfxmgr.h): its life cycle,
 *            the back buffer and the handle machinery
 *
 * The manager owns the display objects (one table per class, addressed
 * by handles), the background, the compositor that renders them into the
 * back buffer, and the scaled copy of the back buffer a window larger
 * than the picture is presented from.  The frame cycle is in frame.c, the
 * background operations in background.c, the per-class object wrappers
 * in the other files of this directory.
 *
 * The back buffer is w x (h + 2) rows with the picture starting at row 1,
 * so that the unclipped MMX-style loops of the blitters may touch one
 * guard row above and below; its rows are padded to 4 bytes like a DIB.
 * When the window is scaled, a second buffer (`scaled`) receives
 * stretched copies of the changed rectangles.
 *
 * Gfx_Create / Gfx_Destroy at the end build and tear down the global set
 * (worker pool, manager, bitmap manager) for the engine; everything else
 * works on a Gfx_t handed in.
 */
#include <string.h>

#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/objects.h"
#include "bgi/gfx/text.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx.h"
#include "bgi/engine.h"
#include "bgi/file.h"
#include "bgi/sys.h"

Gfx_t* gGfx = NULL;
BmpMgr_t* gGfxBmpMgr = NULL;

// ---- life cycle --------------------------------------------------------------------------------

/* an empty manager: no back buffer yet, a compositor with 0x400 dirty
 * rectangles and the given band size, a type 1 background shown in mode
 * 0, every object table empty.  `hWnd` is only stored. */
void Gfx_Ctor(Gfx_t* g, void* hWnd, uint32_t bandPixels, WorkerPool_t* pool)
{
	memset(g, 0, sizeof *g);
	g->hWnd = hWnd;
	g->pool = pool;
	g->comp = (Compositor_t*)BGI_Alloc(sizeof(Compositor_t));
	// the compositor renders into `back` and reads the screen rectangle
	// that follows it (ScreenBmp)
	Compositor_Ctor(g->comp, 0x400, (ScreenBmp_t*)&g->back, bandPixels, pool);
	g->backMem = g->scaledMem = NULL;
	g->back.pixels = NULL;
	g->back.w = g->back.h = 0;
	g->scaleMode = 0;

	g->background = Background_Create(1);
	Compositor_SetBgType(g->comp, g->background->typeId);
	Compositor_Add(g->comp, &g->background->obj);
	Gfx_BgShow(g, 1, 0);

	// the tables were zeroed above; the counters and ids start at 0
	g->copyJobCount = 0;
	g->copyJobs = NULL;
	g->copyJobNext = 0;
	g->projCentre[0] = g->projCentre[1] = -1;
}

// ---- the projection centre (1.535 on) -----------------------------------

// "90 06": the vanishing point of the projected sprites (pixels)
void Gfx_SetProjCentre(Gfx_t* g, int x, int y)
{
	g->projCentre[0] = x;
	g->projCentre[1] = y;
}

// the projection centre when one inside the back buffer was set: 1, else 0 (the sprites use the buffer's centre)
int Gfx_GetProjCentre(Gfx_t* g, int32_t out[2])
{
	if(g->projCentre[0] < 0 || g->projCentre[0] >= g->back.w || g->projCentre[1] < 0 || g->projCentre[1] >= g->back.h)
		return 0;
	out[0] = g->projCentre[0];
	out[1] = g->projCentre[1];
	return 1;
}

// the compositor, the background, every object and the buffers
void Gfx_Dtor(Gfx_t* g)
{
	if(g->comp)
	{
		Compositor_Dtor(g->comp);
		BGI_Free(g->comp);
	}
	if(g->background)
		g->background->obj.vt->destroy(&g->background->obj, 1);
	Gfx_DeleteAllSprites(g);
	Gfx_DeleteAllFilters(g);
	Gfx_DeleteAllEffectors(g);
	Gfx_DeleteAllMaps(g);
	Gfx_DeleteAllWindows(g);
	Gfx_DeleteAllParticles(g);
	Gfx_DeleteAllRains(g);
	Gfx_DeleteAllKnobs(g);
	Gfx_DeleteAllGroups(g);
	Gfx_FreeScreen(g);
}

// the bitmap manager the wrappers look bitmaps up in
void Gfx_SetBmpMgrPtr(BmpMgr_t* m)
{
	gGfxBmpMgr = m;
}

/* back to a bare screen: every object is deleted, the window globals,
 * the global effect and the draw limit are reset.  As in the original the
 * rain screens are not deleted (they survive a reset) and the current
 * background object is kept, only re-added to the emptied compositor
 * and shown in mode 0. */
void Gfx_Reset(Gfx_t* g)
{
	Compositor_ClearDirty(g->comp);
	Compositor_RemoveAll(g->comp);
	Compositor_Add(g->comp, &g->background->obj);
	Gfx_BgShow(g, 1, 0);
	Gfx_DeleteAllSprites(g);
	Gfx_DeleteAllFilters(g);
	Gfx_DeleteAllEffectors(g);
	Gfx_DeleteAllMaps(g);
	Gfx_DeleteAllWindows(g);
	Gfx_DeleteAllParticles(g);
	Gfx_DeleteAllKnobs(g);
	Gfx_DeleteAllGroups(g);
	Gfx_WindowGlobal(g, 0, 0);
	Gfx_SetGlobalEffect(g, 1);
	Gfx_SetDrawLimit(g, 0);
}

// release the back buffer and the scaled surface
void Gfx_FreeScreen(Gfx_t* g)
{
	if(g->backMem)
	{
		BGI_Free(g->backMem);
		g->backMem = NULL;
	}
	if(g->scaledMem)
	{
		BGI_Free(g->scaledMem);
		g->scaledMem = NULL;
	}
}

// the row pitch of a DIB of width w in the given pixel mode: padded to 4 bytes
static int32_t DibPitch(int w, int mode)
{
	return (int32_t)((((uint32_t)w * (uint32_t)ModeBits(mode) + 31u) >> 5) << 2);
}

/* allocate the back buffer of w x h pixels in pixel mode `mode` (replacing
 * any previous one) and make `mode` the screen mode.  The buffer is a
 * top-down bitmap of h + 2 rows with the visible rows starting at row 1;
 * both guard rows are zeroed.  With `scaled` a second surface of exactly
 * sw x sh rows is created for the stretched presentation, with
 * `scaleMode` indexing the stretch factor table (Gfx_ScaleFactor). */
void Gfx_SetupScreen(Gfx_t* g, int w, int h, int mode, int scaled, int sw, int sh, int scaleMode)
{
	int32_t pitch;

	Gfx_FreeScreen(g);
	Gfx_SetScreenMode(mode);

	pitch = DibPitch(w, mode);
	g->backMem = (uint8_t*)BGI_Alloc((size_t)pitch * (size_t)(h + 2));
	g->back.pitch = pitch;
	g->back.pixels = g->backMem + pitch; // row 1
	g->back.w = w;
	g->back.h = h;
	g->back.mode = mode;
	g->back.bpp = ScreenBytesPerPixel();
	Bmp_Clear(&g->back, NULL);
	memset(g->backMem, 0, (size_t)pitch);                                   // guard row 0
	memset(g->backMem + (size_t)pitch * (size_t)(h + 1), 0, (size_t)pitch); // guard row h+1

	if(scaled)
	{
		pitch = DibPitch(sw, mode);
		g->scaledMem = (uint8_t*)BGI_Alloc((size_t)pitch * (size_t)sh);
		g->scaled.pixels = g->scaledMem;
		g->scaled.pitch = pitch;
		g->scaled.w = sw;
		g->scaled.h = sh;
		g->scaled.mode = mode;
		g->scaled.bpp = ScreenBytesPerPixel();
		Bmp_Clear(&g->scaled, NULL);
		g->scaleMode = scaleMode;
	}
	else
	{
		memset(&g->scaled, 0, sizeof g->scaled);
	}
}

/* re-create the buffers (Gfx_SetupScreen); the background, the filters
 * and the effectors match the new size; everything is rendered and
 * repainted.  1 when a back buffer exists afterwards. */
int Gfx_Resize(Gfx_t* g, int w, int h, int mode, int scaled, int sw, int sh, int scaleMode)
{
	int i;

	Gfx_SetupScreen(g, w, h, mode, scaled, sw, sh, scaleMode);
	Rect_FromBmp(&g->screen, &g->back);
	((const BackgroundVtbl_t*)g->background->obj.vt)->matchScreenSize(g->background);
	for(i = 0; i < GFX_FILTERS; i++)
		if(g->filters[i])
			Filter_MatchScreenSize((Filter_t*)g->filters[i]);
	for(i = 0; i < GFX_EFFECTORS; i++)
		if(g->effectors[i])
			Effector_MatchScreenSize((Effector_t*)g->effectors[i]);
	Gfx_RenderAll(g);
	Compositor_InvalidateAll(g->comp);
	return g->back.pixels != NULL;
}

// a view of the back buffer (all zero without a manager)
void Gfx_CopyBackBmp(Gfx_t* g, Bmp_t* out)
{
	if(g)
		*out = g->back;
	else
		memset(out, 0, sizeof *out);
}

void Gfx_GetScreenRect(Gfx_t* g, Rect_t* out)
{
	*out = g->screen;
}

// a view of the scaled surface; 1 when there is one, 0 when the window is not scaled
int Gfx_GetScaledBmp(Gfx_t* g, Bmp_t* out)
{
	if(g->scaledMem)
		*out = g->scaled;
	return g->scaledMem != NULL;
}

// ---- handles -----------------------------------------------------------------------------------

// the object of handle h in a table of n slots, NULL when the class byte or the index does not match
static DispObj_t* Lookup(DispObj_t** table, uint32_t n, uint32_t h, uint32_t cls)
{
	if((h & 0xFF000000u) != cls)
		return NULL;
	if(HANDLE_INDEX(h) >= n)
		return NULL;
	return table[HANDLE_INDEX(h)];
}

// the per-class lookups: the object, or NULL when h is not a live handle of that class
DispObj_t* Gfx_FindSprite(Gfx_t* g, uint32_t h)
{
	return Lookup(g->sprites, GFX_SPRITES, h, H_SPRITE);
}

DispObj_t* Gfx_FindFilter(Gfx_t* g, uint32_t h)
{
	return Lookup(g->filters, GFX_FILTERS, h, H_FILTER);
}

DispObj_t* Gfx_FindEffector(Gfx_t* g, uint32_t h)
{
	return Lookup(g->effectors, GFX_EFFECTORS, h, H_EFFECTOR);
}

DispObj_t* Gfx_FindMap(Gfx_t* g, uint32_t h)
{
	return Lookup(g->maps, GFX_MAPS, h, H_MAP);
}

DispObj_t* Gfx_FindWindow(Gfx_t* g, uint32_t h)
{
	return Lookup(g->windows, GFX_WINDOWS, h, H_WINDOW);
}

DispObj_t* Gfx_FindParticle(Gfx_t* g, uint32_t h)
{
	return Lookup(g->particles, GFX_PARTICLES, h, H_PARTICLE);
}

DispObj_t* Gfx_FindRain(Gfx_t* g, uint32_t h)
{
	return Lookup(g->rains, GFX_RAINS, h, H_RAIN);
}

DispObj_t* Gfx_FindKnob(Gfx_t* g, uint32_t h)
{
	return Lookup(g->knobs, GFX_KNOBS, h, H_KNOB);
}

DispObj_t* Gfx_FindGroup(Gfx_t* g, uint32_t h)
{
	return Lookup(g->groups, GFX_GROUPS, h, H_GROUP);
}

/* the object of any handle: 0 is the background; the class tables are
 * tried in order (the class byte makes at most one match).  NULL for a
 * handle that resolves nowhere. */
DispObj_t* Gfx_FindObject(Gfx_t* g, uint32_t h)
{
	DispObj_t* o;

	if(h == 0)
		return &g->background->obj;
	if((o = Gfx_FindSprite(g, h)) != NULL)
		return o;
	if((o = Gfx_FindFilter(g, h)) != NULL)
		return o;
	if((o = Gfx_FindEffector(g, h)) != NULL)
		return o;
	if((o = Gfx_FindMap(g, h)) != NULL)
		return o;
	if((o = Gfx_FindWindow(g, h)) != NULL)
		return o;
	if((o = Gfx_FindParticle(g, h)) != NULL)
		return o;
	if((o = Gfx_FindRain(g, h)) != NULL)
		return o;
	if((o = Gfx_FindKnob(g, h)) != NULL)
		return o;
	return Gfx_FindGroup(g, h);
}

// ---- creation and teardown of the manager set ----------------------------------------

WorkerPool_t* gWorkerPool;

/* The graphics half of the engine's core set-up (Engine_CreateCore): the
 * worker pool of `cpus` threads, the graphics manager gGfx with a
 * compositor band of `bandPixels` pixels, and the bitmap manager gBmpMgr,
 * each handed to every module that keeps its own copy of the pointer.
 * The blitters are tuned for the CPU: SSE / SSE2 when present, and the
 * tuning switched off only on an Intel family-15 (Pentium 4) CPU. */
void Gfx_Create(uint32_t bandPixels, int cpus)
{
	Gfx_t* g;

	gWorkerPool = WorkerPool_New(cpus);
	g = (Gfx_t*)BGI_Calloc(sizeof(Gfx_t));
	Gfx_Ctor(g, NULL, bandPixels, gWorkerPool); // the HWND stays inside the OS layer
	gGfx = g;
	DispObj_SetGfxPtr(g);
	Text_SetGfxPtr(g);
	Panel_SetGfxPtr(g);
	Blit_InitTables();
	Blit_SetSse(Cpu_HasSse());
	Blit_SetSse2(Cpu_HasSse2());
	Blit_SetTuning(gSysInfo.vendor != 0 || gSysInfo.family != 0xf);
	gBmpMgr = BmpMgr_New(0x4000); // 4096 up to 1.69 build 444, 16384 after; the scripts' numbers are checked per generation (BMP_NO_LIMIT)
	Gfx_SetBmpMgrPtr(gBmpMgr);
	DispObj_SetBmpMgrPtr(gBmpMgr);
	Text_SetBmpMgrPtr(gBmpMgr);
	Panel_SetBmpMgrPtr(gBmpMgr);
	Ptcl_SetBmpMgrPtr(gBmpMgr);
	Pattern_SetBmpMgrPtr(gBmpMgr);
}

/* tear the set down in the original's order (not the reverse of the
 * creation): the manager, the bitmap manager, the archive manager, the
 * static caches of the text engine and the loader, then the worker pool */
void Gfx_Destroy(void)
{
	if(gGfx)
	{
		Gfx_Dtor(gGfx);
		BGI_Free(gGfx);
		gGfx = NULL;
	}
	if(gBmpMgr)
	{
		BmpMgr_Delete(gBmpMgr);
		gBmpMgr = NULL;
	}
	ToneCurve_FreeAll(); // (the original keeps them on the manager)
	if(gArcMgr)
	{
		FileSet_Delete(gArcMgr); // (original: the pointer is left dangling; nothing uses it afterwards)
		gArcMgr = NULL;
	}
	Text_FreeGlyphSheet();
	Text_FreeItemBitmaps();
	Ruby_Clear();
	Loader_FreeCache();
	if(gWorkerPool)
		WorkerPool_Delete(gWorkerPool);
}
