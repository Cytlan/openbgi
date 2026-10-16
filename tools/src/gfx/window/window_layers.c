/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window_layers.c - the frame layer, the items and the sub-sprites of a
 *                   message window (inc/bgi/gfx/window.h)
 *
 * The frame is the window's background bitmap ("90 86", "92 88" .. "92 8A");
 * items are small bitmaps the engine's own waits place over the text (the
 * cursor animation and menu highlights of src/wait, the panel buttons of
 * src/sys/panel.c), and sub-sprites are rotatable projected sprites the
 * sprite panel ("90 B7" / "91 BA", src/sys/sprpanel.c) draws over the
 * text.  Every change recomposites the touched rectangle (see
 * Window_Recomposite in window.c).
 */
#include "window_internal.h"

// ---- frame layer -------------------------------------------------------------------------------

/* "90 86": copy script bitmap `bmp` into the frame layer and show the
 * frame; -1 clears the frame and hides it.  0 ok, 1 not ready, 2 no such
 * bitmap. */
int Window_SetFrame(Window_t* w, int bmp)
{
	Bmp_t info;

	if(!w->ready)
		return 1;
	if(bmp != -1)
	{
		if(!BmpMgr_GetInfo(gDispBmpMgr, &info, bmp))
			return 2;
		Bmp_Clear(&w->frame, NULL);
		Bmp_Blit(&w->frame, 0, 0, &info, 0x80, 0);
	}
	else
	{
		Bmp_Clear(&w->frame, NULL);
	}
	Window_ShowFrame(w, bmp != -1);
	Window_RecompositeAll(w);
	return 0;
}

/* "90 85": position, effect, level and priority of the display object.
 * Six arguments; the fifth is not used at this level. */
void Window_Set(Window_t* w, int x, int y, int effect, int level, int unused, int prio)
{
	(void)unused;
	w->obj.vt->setPos(&w->obj, x, y);
	DispObj_SetEffect(&w->obj, effect);
	w->obj.vt->setLevel(&w->obj, level);
	DispObj_SetPriority(&w->obj, (uint32_t)prio);
}

void Window_SetPunch(Window_t* w, int f) // "90 87": cut the item shapes out of the text layer
{
	w->punchItems = f;
	Window_RecompositeAll(w);
}

void Window_ShowFrame(Window_t* w, int f) // "92 88"
{
	w->frameVisible = f;
	Window_RecompositeAll(w);
}

// "92 8A": fill the frame layer with `colour`; returns the colour, or 1 when not ready
uint32_t Window_FillFrame(Window_t* w, uint32_t colour)
{
	if(!w->ready)
		return 1;
	Bmp_Fill(&w->frame, NULL, colour);
	Window_RecompositeAll(w);
	return colour;
}

// See window_internal.h.
int Window_BlitResult(int r, int fallback)
{
	switch(r)
	{
		case 0: return 0;
		case 1: return 4;
		case 2: return 5;
		case 3: return 6;
		case 4: return 7;
		default: return fallback; // unreachable: Bmp_Blit returns 0..4
	}
}

/* "92 89": blit script bitmap `bmp` into the frame layer at (x, y) with
 * `effect` / `level` and recomposite the touched rectangle; *outScreen
 * receives that rectangle in screen coordinates.  0 ok, 1 not ready, 2
 * bitmap missing, 4..7 the blit failed (Window_BlitResult). */
int Window_DrawToFrame(Window_t* w, Rect_t* outScreen, int x, int y, int bmp, int effect, int level)
{
	Bmp_t info;
	Rect_t r2;
	int32_t pos[2];
	int r;

	if(!w->ready)
		return 1;
	if(!BmpMgr_GetInfo(gDispBmpMgr, &info, bmp))
		return 2;
	r = Bmp_Blit(&w->frame, x, y, &info, effect, level);
	if(r == 0)
	{
		Rect_FromBmp(outScreen, &w->frame);
		Rect_FromBmp(&r2, &info);
		Rect_Offset(&r2, x, y);
		Rect_Clip(outScreen, &r2);
		w->obj.vt->getPos(&w->obj, pos);
		Rect_Offset(outScreen, pos[0], pos[1]);
		Window_RecompositeAt(w, x, y, &info);
		return 0;
	}
	return Window_BlitResult(r, bmp);
}

// ---- items -------------------------------------------------------------------------------------

/* (The setters below take the item index unchecked in the original - an
 * index past the eight items would write into the sub-sprite fields - and
 * every caller passes 0..7.  The range check of Window_ItemRefresh keeps
 * that true here: an index of 8 or more does nothing.) */

// switch item `i` on or off; both its old and its new footprint are recomposited
void Window_ItemEnable(Window_t* w, int i, int f)
{
	if(!Window_ItemRefresh(w, i, 0))
		return;
	w->item[i].enabled = f;
	Window_ItemRefresh(w, i, 1);
}

// place item `i` at (x, y) in the window with the compositing transparency `level`
void Window_ItemSet(Window_t* w, int i, int x, int y, int level)
{
	if(!Window_ItemRefresh(w, i, 0))
		return;
	w->item[i].x = x;
	w->item[i].y = y;
	w->item[i].level = level;
	Window_ItemRefresh(w, i, 1);
}

/* give item `i` a private copy of `src` (which must be in a
 * screen-compatible pixel mode; 4 otherwise).  0 ok, also for an index out
 * of range (nothing is copied then). */
int Window_ItemSetBitmap(Window_t* w, int i, const Bmp_t* src)
{
	WinItem_t* it = &w->item[i];

	if(!Bmp_ScreenCompatible(src))
		return 4;
	if(!Window_ItemRefresh(w, i, 0))
		return 0;
	DispObj_AllocLike(&w->obj, &it->bmp, it->buf, src->w, src->h, src);
	Bmp_BlitEffect(&it->bmp, src, 0x80, 0);
	Window_ItemRefresh(w, i, 1);
	return 0;
}

void Window_ItemsDisableAll(Window_t* w)
{
	int i;

	for(i = 0; i < WIN_ITEMS; i++)
		Window_ItemEnable(w, i, 0);
}

// screen rectangle of an enabled item with pixels; 0 otherwise
int Window_ItemRect(Window_t* w, Rect_t* out, int i)
{
	WinItem_t* it = &w->item[i];
	int32_t pos[2];

	if(!it->enabled || !it->bmp.pixels)
		return 0;
	w->obj.vt->getPos(&w->obj, pos);
	Rect_FromBmp(out, &it->bmp);
	Rect_Offset(out, it->x + pos[0], it->y + pos[1]);
	return 1;
}

/* recomposite the item's footprint with `enabled` temporarily
 * forced to `forcedEnabled`.  The setters call it with 0 before and 1
 * after a change so both the old and the new footprint are rebuilt.  Only
 * an item that is enabled and has pixels does anything; returns i < 8. */
int Window_ItemRefresh(Window_t* w, int i, int forcedEnabled)
{
	WinItem_t* it;
	int saved, ok = i < WIN_ITEMS;

	if(!ok)
		return 0;
	it = &w->item[i];
	if(!it->enabled || !it->bmp.pixels)
		return ok;
	saved = it->enabled;
	it->enabled = forcedEnabled;
	Window_RecompositeAt(w, it->x, it->y, &it->bmp);
	it->enabled = saved;
	return ok;
}

// ---- sub-sprites -------------------------------------------------------------------------------

/* destroy the current set, then allocate `n` empty slots and a private
 * compositor rendering into the composite bitmap; n == 0 leaves the
 * window without sub-sprites.  The projection offsets are taken from the
 * back buffer's size: the sprites' world origin is the screen centre. */
void Window_SubAlloc(Window_t* w, int n)
{
	Bmp_t back;
	uint32_t i;

	if(w->sub)
	{
		for(i = 0; i < (uint32_t)w->subCount; i++)
		{
			Compositor_Remove(w->subComp, w->sub[i]);
			if(w->sub[i])
				w->sub[i]->vt->destroy(w->sub[i], 1);
		}
		BGI_Free(w->sub);
	}
	if(w->subComp)
	{
		Compositor_Dtor(w->subComp);
		BGI_Free(w->subComp);
	}
	if((uint32_t)n == 0)
	{
		w->subCount = 0;
		w->sub = NULL;
		w->subComp = NULL;
		return;
	}
	w->subCount = n;
	w->sub = (DispObj_t**)BGI_Calloc((size_t)n * sizeof(DispObj_t*));
	memset(&w->subSurface, 0, sizeof w->subSurface);
	memset(w->pad2A4, 0, sizeof w->pad2A4);
	w->subSurface.bmp = w->comp;
	Rect_FromBmp(&w->subSurface.screen, &w->comp);
	w->subComp = (Compositor_t*)BGI_Alloc(sizeof(Compositor_t));
	Compositor_Ctor(w->subComp, n, &w->subSurface, 0xffffffffu, NULL);
	Compositor_ClearDirty(w->subComp);
	Gfx_CopyBackBmp(gDispGfx, &back);
	w->subOffX = -(int32_t)((uint32_t)back.w >> 1);
	w->subOffY = -(int32_t)((uint32_t)back.h >> 1);
	w->subDist = (int32_t)((uint32_t)back.w >> 1);
}

/* the sprite panel ("90 B7" / "91 BA"): put a projected sprite (mode 5,
 * effect 0x20, level 0, smoothing on) of script bitmap `bmp` into slot
 * `i`, replacing what was there.  Its world position is
 * (x - screenW/2, y - screenH/2, z); the sprite adds half the screen size
 * back when it projects, so (x, y) end up as window coordinates.  (ox, oy)
 * is the pivot, `angle` the rotation, `prio` the sort priority.  0 ok, 9
 * bad index, 2 the sprite rejected the bitmap (the slot is empty then). */
int Window_SubCreate(Window_t* w, int i, int bmp, int x, int y, int z, int ox, int oy, int32_t angle, int prio)
{
	Sprite_t* s;
	int r;

	if((uint32_t)i >= (uint32_t)w->subCount)
		return 9;
	if(w->sub[i])
	{
		Compositor_Remove(w->subComp, w->sub[i]);
		if(w->sub[i])
			w->sub[i]->vt->destroy(w->sub[i], 1);
	}
	s = (Sprite_t*)BGI_Calloc(sizeof(Sprite_t));
	Sprite_Ctor(s, i);
	w->sub[i] = &s->obj;
	r = Sprite_SetProjectedEx(s, (int32_t)((uint32_t)(x + w->subOffX) << 16),
		(int32_t)((uint32_t)(y + w->subOffY) << 16),
		(int32_t)((uint32_t)z << 16),
		bmp, -1, 0, -1, ox, oy, angle, w->subDist, 1, 1, 0x20, 0, prio);
	if(r != 0)
	{
		if(w->sub[i])
			w->sub[i]->vt->destroy(w->sub[i], 1);
		w->sub[i] = NULL;
		return 2;
	}
	w->sub[i]->vt->setVisible(w->sub[i], 1);
	Compositor_Add(w->subComp, w->sub[i]);
	return 0;
}

// change the bitmap of sub-sprite `i`; 0 ok, 9 bad index or empty slot, 2 the sprite rejected it
int Window_SubSetBitmap(Window_t* w, int i, int bmp)
{
	if((uint32_t)i >= (uint32_t)w->subCount || w->sub[i] == NULL)
		return 9;
	return Sprite_ChangeBitmap((Sprite_t*)w->sub[i], bmp) != 0 ? 2 : 0;
}

// rotate sub-sprite `i` to `angle`; 0 ok, 9 bad index or empty slot
int Window_SubSetAngle(Window_t* w, int i, int32_t angle)
{
	if((uint32_t)i >= (uint32_t)w->subCount || w->sub[i] == NULL)
		return 9;
	Sprite_SetAngle((Sprite_t*)w->sub[i], angle);
	return 0;
}

/* recomposite the sub-sprite's rectangle; *outScreen receives
 * it in screen coordinates.  1 ok, 0 for a bad index or empty slot. */
int Window_SubRefresh(Window_t* w, Rect_t* outScreen, int i)
{
	int32_t pos[2];

	if((uint32_t)i >= (uint32_t)w->subCount || w->sub[i] == NULL)
		return 0;
	w->sub[i]->vt->screenRect(w->sub[i], outScreen);
	Window_Recomposite(w, outScreen);
	w->obj.vt->getPos(&w->obj, pos);
	Rect_Offset(outScreen, pos[0], pos[1]);
	return 1;
}

// the same with the sprite hidden while recompositing (erase); 1 ok, 0 bad index or empty slot
int Window_SubErase(Window_t* w, Rect_t* outScreen, int i)
{
	if((uint32_t)i >= (uint32_t)w->subCount || w->sub[i] == NULL)
		return 0;
	w->sub[i]->vt->setVisible(w->sub[i], 0);
	Window_SubRefresh(w, outScreen, i);
	w->sub[i]->vt->setVisible(w->sub[i], 1);
	return 1;
}
