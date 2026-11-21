/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * windows.c - the graphics manager's message window operations: the
 *             "90 8x", "91 8x", "92 8x" and text instructions
 *             (src/include/bgi/gfx/gfxmgr.h)
 *
 * Sixteen slots, addressed by H_WINDOW | index.  A window owns its own
 * layers (frame, text, items, sub-sprites) and composes them itself; the
 * wrappers here hand the arguments on and dirty the window's footprint
 * (or, for the layer draws, only the rectangle the window reports as
 * touched).  Unless noted the result is 0 ok, 0xFF unknown handle, small
 * numbers for the window's errors; the wrappers without an error path
 * return 1 ok, 0 unknown handle.
 */
#include "mgr_internal.h"
#include "bgi/core.h"

void Gfx_DeleteAllWindows(OpenBGI_t* o, Gfx_t* g)
{
	Mgr_DeleteAll(o, g->windows, GFX_WINDOWS, &g->windowCount, &g->windowNextId);
}

/* "90 0C": the two settings every window's draw consults (all windows
 * shown or hidden, a level applied to all), then every window is
 * repainted */
void Gfx_WindowGlobal(OpenBGI_t* o, Gfx_t* g, int shown, int level)
{
	int i;

	Window_SetGlobalShown(o, shown);
	Window_SetGlobalLevel(o, level);
	for(i = 0; i < GFX_WINDOWS; i++)
		if(g->windows[i])
			Invalidate(o, g->windows[i]);
}

/* "90 80": create a window of w x h pixels in the first free slot and
 * return its handle in *outHandle.  0 ok, 9 the 16 slots are taken, 10
 * the size is rejected (the window is destroyed again). */
int Gfx_WindowCreate(OpenBGI_t* o, Gfx_t* g, uint32_t* outHandle, int w, int h)
{
	Window_t* win;
	uint32_t i;

	if(g->windowCount >= GFX_WINDOWS)
		return 9;
	i = Mgr_FreeSlot(g->windows);
	win = (Window_t*)BGI_Calloc(sizeof(Window_t));
	Window_Ctor(o, win, g->windowNextId++);
	if(!Window_CreateSurfaces(o, win, w, h))
	{
		win->obj.vt->destroy(o, &win->obj, 1);
		return 10;
	}
	// the slot is taken only once the surfaces exist
	g->windows[i] = &win->obj;
	g->windowCount++;
	Compositor_Add(o, g->comp, &win->obj);
	*outHandle = H_WINDOW | i;
	return 0;
}

// "90 81": 1 ok, 0 unknown handle
int Gfx_WindowDelete(OpenBGI_t* o, Gfx_t* g, uint32_t h)
{
	return Mgr_DeleteFromTable(o, g, g->windows, Gfx_FindWindow(g, h), h, &g->windowCount, 0);
}

/* "90 86": the frame layer from bitmap `bmp` (-1 clears it); only the
 * handle and the fourth argument are used, the two others are the
 * instruction's unused arguments.  0 ok, 1 the window has no surfaces,
 * 2 no such bitmap, 3 not a frame bitmap (Window_SetFrame 1..3 pass
 * through), 0x10 for its result 8, 0xFF unknown handle. */
int Gfx_WindowSetFrame(OpenBGI_t* o, Gfx_t* g, uint32_t h, int unused1, int unused2, int bmp)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);
	int r;

	(void)unused1;
	(void)unused2;
	if(!obj)
		return NO_OBJECT;
	r = Window_SetFrame(o, (Window_t*)obj, bmp);
	switch(r)
	{
		case 0:
			InvalidateIfVisible(o, obj);
			return 0;
		case 1:
		case 2:
		case 3:
			return r;
		case 8:
			return 0x10;
		default:
			return (int)h; // (original: returns the handle)
	}
}

// "90 85": position, effect, level and priority (`unused` is the instruction's spare argument); 1 ok, 0 unknown handle
int Gfx_WindowSet(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int effect, int level, int unused, int prio)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(!obj)
		return 0;
	InvalidateIfVisible(o, obj);
	Window_Set(o, (Window_t*)obj, x, y, effect, level, unused, prio);
	InvalidateIfVisible(o, obj);
	Compositor_Resort(o, g->comp, obj);
	return 1;
}

// "90 87": whether the item layers are punched out of the text layer; 1 ok, 0 unknown handle
int Gfx_WindowSetPunch(OpenBGI_t* o, Gfx_t* g, uint32_t h, int f)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(obj)
		Window_SetPunch(o, (Window_t*)obj, f);
	return obj != NULL;
}

/* "90 82" of 1.492 on: the pass order of the window's layers (0..5); the
 * whole composite is rebuilt.  0 ok, 0x14 bad order, 0xff no such
 * window. */
int Gfx_WindowSetLayerOrder(OpenBGI_t* o, Gfx_t* g, uint32_t h, int order)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);
	Window_t* w = (Window_t*)obj;
	int was, now;
	if(!obj)
		return 0xff;
	if(Window_SetLayerOrder(w, order))
		return 0x14;
	Window_RecompositeAll(o, w);
	was = IsVisible(o, obj);
	now = IsVisible(o, obj);
	if(was || now)
		Invalidate(o, obj);
	return 0;
}

// "90 88": the text area (x, y, w, hgt inside the window); 0 ok, 4 out of the text layer, 0xFF unknown handle
int Gfx_WindowSetTextArea(Gfx_t* g, uint32_t h, int x, int y, int w, int hgt)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(!obj)
		return NO_OBJECT;
	return Window_SetTextArea((Window_t*)obj, x, y, w, hgt) ? 0 : 4;
}

// "90 89": the text area; 0 ok, 0xFF unknown handle
int Gfx_WindowGetTextArea(Gfx_t* g, Rect_t* out, uint32_t h)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(!obj)
		return NO_OBJECT;
	Window_GetTextArea((Window_t*)obj, out);
	return 0;
}

// "91 8A": the text draw style (0..1); 0 ok, 0x12 bad style, 0xFF unknown handle
int Gfx_WindowSetDrawStyle(Gfx_t* g, uint32_t h, int style)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(!obj)
		return NO_OBJECT;
	return Window_SetDrawStyle((Window_t*)obj, style) ? 0 : 0x12;
}

// "91 8B": the swing style (0..2); 0 ok, 0x13 bad style, 0xFF unknown handle
int Gfx_WindowSetSwingStyle(Gfx_t* g, uint32_t h, int style)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(!obj)
		return NO_OBJECT;
	return Window_SetSwingStyle((Window_t*)obj, style) ? 0 : 0x13;
}

/* "91 88": the window's font.  The proportional flag and the ruby
 * reservation are stored first, then the font is opened.  0 ok, 5 size
 * out of range, 6 width percentage out of range, 7 the font could not be
 * created (the font cache's 0x80000002 .. 0x80000004), 0xFF unknown
 * handle. */
int Gfx_WindowSetFont(OpenBGI_t* o, Gfx_t* g, uint32_t h, const char* face, int size, int widthPct, int bold,
	int proportional, int rubyReserve)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);
	uint32_t r;

	if(!obj)
		return NO_OBJECT;
	Window_SetProportional((Window_t*)obj, proportional);
	Window_SetRubyReserve((Window_t*)obj, rubyReserve);
	r = (uint32_t)Window_SetFont(o, (Window_t*)obj, face, size, widthPct, bold);
	switch(r)
	{
		case 0: return 0;
		case 0x80000002u: return 5;
		case 0x80000003u: return 6;
		case 0x80000004u: return 7;
		default: return (int)h;
	}
}

// "91 89": the character spacing in percent (0..800); 0 ok, 8 out of range, 0xFF unknown handle
int Gfx_WindowSetSpacing(Gfx_t* g, uint32_t h, int pct)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(!obj)
		return NO_OBJECT;
	return Window_SetSpacing((Window_t*)obj, pct) ? 0 : 8;
}

// "90 84": show / hide; 1 ok, 0 unknown handle
int Gfx_WindowShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on)
{
	return Mgr_ShowBracket(o, Gfx_FindWindow(g, h), on);
}

// "92 88": show / hide the frame layer; 1 ok, 0 unknown handle
int Gfx_WindowShowFrame(OpenBGI_t* o, Gfx_t* g, uint32_t h, int f)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(obj)
	{
		Window_ShowFrame(o, (Window_t*)obj, f);
		InvalidateIfVisible(o, obj);
	}
	return obj != NULL;
}

// "92 8A": fill the frame layer with a colour; 0 ok, 0xFF unknown handle
int Gfx_WindowFillFrame(OpenBGI_t* o, Gfx_t* g, uint32_t h, uint32_t colour)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(!obj)
		return NO_OBJECT;
	Window_FillFrame(o, (Window_t*)obj, colour);
	return 0;
}

/* the result mapping shared by drawToFrame / drawToText: 0 ok (the touched
 * screen rectangle is handed to the compositor), 2 bitmap missing, 4..7
 * (the blit failed) -> 0xC..0xF; 1 (the window has no surfaces) and
 * anything else return the handle */
static int WindowDrawResult(OpenBGI_t* o, Gfx_t* g, DispObj_t* obj, int r, const Rect_t* touched, uint32_t h)
{
	switch(r)
	{
		case 0:
			if(IsVisible(o, obj))
				Gfx_AddDirty(g, obj->vt->sortKey(o, obj), touched);
			return 0;
		case 2: return 2;
		case 4: return 0xc;
		case 5: return 0xd;
		case 6: return 0xe;
		case 7: return 0xf;
		default: return (int)h;
	}
}

// "92 89": draw bitmap `bmp` into the frame layer at (x, y); the codes of WindowDrawResult, 0xFF unknown handle
int Gfx_WindowDrawToFrame(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int bmp, int effect, int level)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);
	Rect_t touched;
	int r;

	if(!obj)
		return NO_OBJECT;
	r = Window_DrawToFrame(o, (Window_t*)obj, &touched, x, y, bmp, effect, level);
	return WindowDrawResult(o, g, obj, r, &touched, h);
}

/* "90 83": create bitmap bmpNo of the window's size (ARGB when the screen
 * is RGB32) and copy the composite into it.  0 ok, 2 the bitmap manager
 * refuses the number, 0xFF unknown handle. */
int Gfx_WindowCopyToBitmap(OpenBGI_t* o, Gfx_t* g, int bmpNo, uint32_t h)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);
	Rect_t lr;
	Bmp_t info;
	int mode;

	if(!obj)
		return NO_OBJECT;
	obj->vt->localRect(o, obj, &lr);
	mode = ScreenMode(o);
	if(mode == PM_RGB32)
		mode = PM_ARGB32;
	if(!BmpMgr_Create(o->gfx.gfxBmpMgr, bmpNo, lr.r - lr.l + 1, lr.b - lr.t + 1, mode))
		return 2;
	BmpMgr_GetInfo(o->gfx.gfxBmpMgr, &info, bmpNo);
	Window_CopyComposite((Window_t*)obj, &info);
	return 0;
}

// "92 8C": show / hide the text layer; the composite is rebuilt.  1 ok, 0 unknown handle
int Gfx_WindowShowText(OpenBGI_t* o, Gfx_t* g, uint32_t h, int f)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(obj)
	{
		Window_ShowText((Window_t*)obj, f);
		Window_RecompositeAll(o, (Window_t*)obj);
		InvalidateIfVisible(o, obj);
	}
	return obj != NULL;
}

// "92 8D": draw bitmap `bmp` into the text layer at (x, y); the codes of WindowDrawResult, 0xFF unknown handle
int Gfx_WindowDrawToText(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int bmp, int effect, int level)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);
	Rect_t touched;
	int r;

	if(!obj)
		return NO_OBJECT;
	r = Window_DrawToText(o, (Window_t*)obj, &touched, x, y, bmp, effect, level);
	return WindowDrawResult(o, g, obj, r, &touched, h);
}

/* "91 91" / "91 93" / "92 91": draw `str` into the text layer at the
 * cursor at once (no wait); `parseTags` interprets the ruby markup, `hang`
 * is passed to the layout as its hanging mode, `style` is the shadow
 * style.  0 ok, 0x11 nothing was drawn (the window has no font), 0xFF
 * unknown handle. */
int Gfx_WindowDrawText(OpenBGI_t* o, Gfx_t* g, uint32_t h, const char* str, int parseTags, int hang, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5])
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(!obj)
		return NO_OBJECT;
	if(!Window_DrawText(o, (Window_t*)obj, str, parseTags, hang, colour, rubyColour, style))
		return 0x11;
	InvalidateIfVisible(o, obj);
	return 0;
}

// "92 8E": cursor home, text cleared, items off, sub-sprites gone; 1 ok, 0 unknown handle
int Gfx_WindowClear(OpenBGI_t* o, Gfx_t* g, uint32_t h)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(obj)
	{
		Window_ResetCursor((Window_t*)obj);
		Window_ClearText(o, (Window_t*)obj);
		Window_ItemsDisableAll(o, (Window_t*)obj);
		Window_SubAlloc(o, (Window_t*)obj, 0);
		InvalidateIfVisible(o, obj);
	}
	return obj != NULL;
}

// "91 8C": the text cursor; 1 ok, 0 unknown handle
int Gfx_WindowSetCursor(Gfx_t* g, uint32_t h, int x, int y)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(obj)
		Window_SetCursor((Window_t*)obj, x, y);
	return obj != NULL;
}

// "91 8D": the text cursor; 1 ok, 0 unknown handle
int Gfx_WindowGetCursor(Gfx_t* g, int32_t out[2], uint32_t h)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(obj)
		Window_GetCursor((Window_t*)obj, out);
	return obj != NULL;
}

// "91 8E": *out = whether the cursor is at the start of a line; 0 ok, 0xFF unknown handle
int Gfx_WindowCursorStatus(OpenBGI_t* o, Gfx_t* g, int* out, uint32_t h)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(!obj)
		return NO_OBJECT;
	*out = Window_CursorAtLineStart(o, (Window_t*)obj);
	return 0;
}

// "90 A7": the window's menu colour table (16 words; NULL removes it); 1 ok, 0 unknown handle
int Gfx_WindowSetTable(Gfx_t* g, uint32_t h, const int32_t* table)
{
	DispObj_t* obj = Gfx_FindWindow(g, h);

	if(obj)
		Window_SetTable((Window_t*)obj, table);
	return obj != NULL;
}
