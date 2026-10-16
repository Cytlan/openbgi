/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.c - the message Window display object: construction, the display
 *            object interface, and compositing of its layers (inc/bgi/gfx/window.h)
 *
 * A window is a stack of three same-sized layers - the frame, the text and
 * the sub-sprites (plus a few "items": small bitmaps placed on the text) -
 * composed into one buffer that the display object draws.  The frame and
 * item operations live in window_layers.c, the text layer with its fonts
 * and cursor in window_text.c.  Windows are created by "90 80" through the
 * graphics manager (src/gfx/mgr/windows.c), which also maps the result
 * codes of this file onto the script's.
 */
#include "window_internal.h"

int gWindowsShown = 1; // "90 0C": all windows hidden when 0
int gWindowsLevel = 0; // "90 0C": the transparency added to every window

// Show or hide every window at once ("90 0C").
void Window_SetGlobalShown(int f)
{
	gWindowsShown = f;
}

// The transparency (0..0x100) every window is drawn with on top of its own ("90 0C").
void Window_SetGlobalLevel(int level)
{
	gWindowsLevel = level;
}

// ---- construction ------------------------------------------------------------------------------

static PixBuf_t* NewPixBuf(void)
{
	PixBuf_t* b = (PixBuf_t*)BGI_Alloc(sizeof(PixBuf_t));
	PixBuf_Ctor(b);
	return b;
}

static void DeletePixBuf(PixBuf_t* b)
{
	if(b)
	{
		PixBuf_Dtor(b);
		BGI_Free(b);
	}
}

/* construct a window of class order 3 with every layer switched off and
 * no surfaces (Window_CreateSurfaces gives it its size); `slotId` is the
 * manager's object id.  The effect is 1 (alpha blend) at level 0. */
void Window_Ctor(Window_t* w, int slotId)
{
	int i;

	DispObj_Ctor(&w->obj, 3, slotId);
	w->obj.vt = &Window_Vtbl;

	// the eleven pixel holders; the layers themselves come with
	// Window_CreateSurfaces
	w->compBuf = NewPixBuf();
	w->frameBuf = NewPixBuf();
	w->textBuf = NewPixBuf();
	for(i = 0; i < WIN_ITEMS; i++)
	{
		w->item[i].buf = NewPixBuf();
		w->item[i].bmp.pixels = NULL;
		Window_ItemSet(w, i, 0, 0, 0);
		Window_ItemEnable(w, i, 0);
	}
	w->subCount = 0;
	w->sub = NULL;
	w->subComp = NULL;
	w->ready = 0;
	Window_SetLayerOrder(w, 0);
	Window_SetPunch(w, 0);
	Window_ShowFrame(w, 0);
	Window_SetTextLevel(w, 0);
	Window_ShowText(w, 0);
	Window_RecompositeAll(w);      // a no-op: not ready yet
	DispObj_SetEffect(&w->obj, 1); // alpha blend
	DispObj_SetLevel(&w->obj, 0);
	w->font = 0;
	w->fontSize = 0;
	Window_SetSpacing(w, 0);
	Window_SetProportional(w, 0);
	Window_SetRubyReserve(w, 0);
	Window_SetDrawStyle(w, 0);
	Window_SetSwingStyle(w, 0);
	Window_SetTable(w, NULL);
}

// release the layers, the items and the sub-sprites (the font handle stays with the cache)
void Window_Dtor(Window_t* w)
{
	int i;

	w->obj.vt = &Window_Vtbl;
	Window_DocDrop(w);
	DeletePixBuf(w->compBuf);
	DeletePixBuf(w->frameBuf);
	DeletePixBuf(w->textBuf);
	for(i = 0; i < WIN_ITEMS; i++)
		DeletePixBuf(w->item[i].buf);
	Window_SubAlloc(w, 0);
	DispObj_Dtor(&w->obj);
}

// the vtable's destroy: flags & 1 frees the object as well
static void Window_Destroy(DispObj_t* o, int flags)
{
	Window_Dtor((Window_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

// ---- drawing -----------------------------------------------------------------------------------

/* the vtable's draw: the composite is blitted with the object's effect;
 * the window's own level and the global window level are combined as
 * opacities:
 *   level = 0x100 - ((0x100 - own) * (0x100 - global)) >> 8
 * Nothing is drawn while gWindowsShown is off. */
static void Window_Draw(DispObj_t* o, Bmp_t* dst, const Rect_t* local, uint32_t minKey)
{
	Window_t* w = (Window_t*)o;
	Bmp_t src;
	int own, level;

	(void)minKey;
	if(!gWindowsShown)
		return;
	src = w->comp;
	Bmp_Crop(&src, local);
	own = DispObj_EffectiveLevel(o);
	level = (int)(0x100u - (((uint32_t)(0x100 - own) * (uint32_t)(0x100 - gWindowsLevel)) >> 8));
	Bmp_BlitEffect(dst, &src, DispObj_GetEffect(o), level);
}

/* "90 80": allocate the composite, frame and text layers at cw x ch
 * pixels (a width below 0x20 or a height below 0x14 counts 32-pixel
 * cells), clear the frame and the text, reset the text area to the whole
 * layer and size the display object.  Returns `ready` (the setSize
 * result, 1), 0 when the size is 0 or above the build's limit. */
int Window_CreateSurfaces(Window_t* w, int cw, int ch)
{
	uint32_t uw = (uint32_t)cw, uh = (uint32_t)ch;

	w->ready = 0;
	if(uw < 0x20)
		uw <<= 5; // small numbers are counts of 32-pixel cells
	if(uh < 0x14)
		uh <<= 5;
	// 1024 x 768 at most up to 1.494; 1920 x 32768 from 1.529 on
	if(uw == 0 || uw > (gEngine->gen >= GEN_1_529 ? 0x780u : 0x400u) || uh == 0 ||
		uh > (gEngine->gen >= GEN_1_529 ? 0x8000u : 0x300u))
		return w->ready;
	w->w = (int)uw;
	w->h = (int)uh;

	DispObj_AllocLike(&w->obj, &w->comp, w->compBuf, (int)uw, (int)uh, NULL);
	Window_ShowFrame(w, 0);
	DispObj_AllocLike(&w->obj, &w->frame, w->frameBuf, (int)uw, (int)uh, NULL);
	Bmp_Clear(&w->frame, NULL);
	Window_ShowText(w, 0);
	DispObj_AllocLike(&w->obj, &w->text, w->textBuf, (int)uw, (int)uh, NULL);
	Rect_FromBmp(&w->textArea, &w->text);
	Window_ResetCursor(w);
	Window_ClearText(w); // recomposites nothing: not ready yet
	w->ready = w->obj.vt->setSize(&w->obj, (int)uw, (int)uh);
	return w->ready;
}

/* "90 83": copy the composite into `dst` (the manager makes a bitmap of
 * the window's size for it).  Both views are cropped to the composite's
 * own rectangle - the clipped intersection the routine also computes is
 * not what it uses.  Returns `ready`; nothing is copied when the window
 * is not ready. */
int Window_CopyComposite(Window_t* w, Bmp_t* dst)
{
	Bmp_t d, s;
	Rect_t dr, sr;

	if(w->ready)
	{
		d = *dst;
		s = w->comp;
		Rect_FromBmp(&dr, &d);
		Rect_FromBmp(&sr, &s);
		Rect_Clip(&dr, &sr); // computed, unused (original)
		Bmp_Crop(&d, &sr);
		Bmp_Crop(&s, &sr);
		Blit_Copy(&d, &s);
	}
	return w->ready;
}

// ---- compositing -------------------------------------------------------------------------------

// rebuild the whole composite; returns `ready`
int Window_RecompositeAll(Window_t* w)
{
	Rect_t r;

	Rect_FromBmp(&r, &w->comp);
	return Window_Recomposite(w, &r);
}

// the frame pass: the frame layer copied into `dst` (the view of rectangle `r`), or black
static void Window_ComposeFrame(Window_t* w, Bmp_t* dst, const Rect_t* r)
{
	Bmp_t src;
	if(w->frameVisible)
	{
		src = w->frame;
		Bmp_Crop(&src, r);
		Bmp_BlitEffect(dst, &src, 0x80, 0);
	}
	else
	{
		Bmp_Clear(dst, NULL);
	}
}

/* the text pass: the text layer alpha-blended by textLevel - with
 * punchItems the enabled items are first cut out of a copy of the text
 * (effect 0x40, level 1) - then the enabled items in index order,
 * alpha-blended by their level.  The items are drawn even while the text
 * layer is hidden. */
static void Window_ComposeText(Window_t* w, Bmp_t* dst, const Rect_t* r)
{
	Bmp_t src, tmp;
	int i;
	if(w->textVisible)
	{
		src = w->text;
		Bmp_Crop(&src, r);
		if(w->punchItems)
		{
			Bmp_AllocScreen(&tmp, src.w, src.h, 1);
			Bmp_BlitEffect(&tmp, &src, 0x80, 0);
			for(i = 0; i < WIN_ITEMS; i++)
			{
				WinItem_t* it = &w->item[i];
				if(it->enabled && it->bmp.pixels)
					Bmp_Blit(&tmp, it->x - r->l, it->y - r->t, &it->bmp, 0x40, 1);
			}
			Bmp_BlitEffect(dst, &tmp, 1, w->textLevel);
			Bmp_Free(&tmp);
		}
		else
		{
			Bmp_BlitEffect(dst, &src, 1, w->textLevel);
		}
	}
	for(i = 0; i < WIN_ITEMS; i++)
	{
		WinItem_t* it = &w->item[i];
		if(it->enabled && it->bmp.pixels)
			Bmp_Blit(dst, it->x - r->l, it->y - r->t, &it->bmp, 1, it->level);
	}
}

/* the sub-sprite pass: the private compositor renders them into the
 * composite (its surface) after being handed the *unclipped* rectangle
 * as dirty */
static void Window_ComposeSubs(Window_t* w, const Rect_t* want)
{
	if(w->subComp)
	{
		Rect_t* scratch = (Rect_t*)BGI_Alloc((size_t)w->subCount * sizeof(Rect_t));
		Rect_t dirty = *want;
		Compositor_AddDirty(w->subComp, 0, &dirty);
		Compositor_Render(w->subComp, scratch);
		BGI_Free(scratch);
	}
}

/* rebuild the rectangle `want` of the composite from the three passes
 * (frame, text with its items, sub-sprites) in the order "90 82" set.
 * Returns `ready`; nothing happens when the window is not ready or the
 * rectangle misses the composite. */
int Window_Recomposite(Window_t* w, const Rect_t* want)
{
	Rect_t r, all;
	Bmp_t dst;
	int pass;

	if(!w->ready)
		return w->ready;
	r = *want;
	Rect_FromBmp(&all, &w->comp);
	if(!Rect_Clip(&r, &all))
		return w->ready;

	dst = w->comp;
	Bmp_Crop(&dst, &r);

	/* the three passes in the order "90 82" set (frame, text with its items,
	 * sub-sprites by default); nothing clears the composite but the frame
	 * pass, so an order that puts the frame later draws over what the
	 * earlier passes left */
	for(pass = 0; pass < 3; pass++)
	{
		switch(w->layerOrder[pass])
		{
			case 0: Window_ComposeFrame(w, &dst, &r); break;
			case 1: Window_ComposeText(w, &dst, &r); break;
			case 2: Window_ComposeSubs(w, want); break;
			default: break;
		}
	}
	return w->ready;
}

/* "90 82" of 1.494 on: the pass order by number: 0 frame, text, subs;
 * 1 frame, subs, text; 2 text, frame, subs; 3 text, subs, frame; 4 subs,
 * frame, text; 5 subs, text, frame.  0 ok, 1 when the number is out of
 * range (nothing changes).  The caller recomposites. */
int Window_SetLayerOrder(Window_t* w, int order)
{
	static const int32_t kOrders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
	if((unsigned)order >= 6u)
		return 1;
	memcpy(w->layerOrder, kOrders[order], sizeof w->layerOrder);
	return 0;
}

// recomposite the footprint of `src` placed at (x, y); returns `ready`
int Window_RecompositeAt(Window_t* w, int x, int y, const Bmp_t* src)
{
	Rect_t r;

	Rect_FromBmp(&r, src);
	Rect_Offset(&r, x, y);
	return Window_Recomposite(w, &r);
}

// ---- only destroy and draw are overridden ------------------------------------------------------

const DispObjVtbl_t Window_Vtbl = {
	Window_Destroy,
	DispObj_SetVisible,
	DispObj_IsVisible,
	DispObj_Invalidate,
	Window_Draw,
	DispObj_SortKey,
	DispObj_LocalRect,
	DispObj_ScreenRect,
	DispObj_SetPosEx,
	DispObj_SetPos,
	DispObj_GetPos,
	DispObj_GetDrawPos,
	DispObj_SetOffset,
	DispObj_SetFixedPos,
	DispObj_GetFixedPos,
	DispObj_SetLevel,
	DispObj_GetLevel,
	DispObj_SetProgress,
	DispObj_SetParam,
	DispObj_HitTest,
	DispObj_NopNotify,
	DispObj_BuildCache,
	DispObj_SetSize, DispObj_GetParam};
