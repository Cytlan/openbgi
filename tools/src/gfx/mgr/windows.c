/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * windows.c - the graphics manager's message window operations: the
 *             "90 8x", "91 8x", "92 8x" and text instructions
 *             (inc/bgi/gfx/gfxmgr.h)
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

void Gfx_DeleteAllWindows(Gfx_t* g)
{
	Mgr_DeleteAll(g->windows, GFX_WINDOWS, &g->windowCount, &g->windowNextId);
}

/* "90 0C": the two globals every window's draw consults (all windows
 * shown or hidden, a level applied to all), then every window is
 * repainted */
void Gfx_WindowGlobal(Gfx_t* g, int shown, int level)
{
	int i;

	Window_SetGlobalShown(shown);
	Window_SetGlobalLevel(level);
	for(i = 0; i < GFX_WINDOWS; i++)
		if(g->windows[i])
			Invalidate(g->windows[i]);
}

/* "90 80": create a window of w x h pixels in the first free slot and
 * return its handle in *outHandle.  0 ok, 9 the 16 slots are taken, 10
 * the size is rejected (the window is destroyed again). */
int Gfx_WindowCreate(Gfx_t* g, uint32_t* outHandle, int w, int h)
{
	Window_t* win;
	uint32_t i;

	if(g->windowCount >= GFX_WINDOWS)
		return 9;
	i = Mgr_FreeSlot(g->windows);
	win = (Window_t*)BGI_Calloc(sizeof(Window_t));
	Window_Ctor(win, g->windowNextId++);
	if(!Window_CreateSurfaces(win, w, h))
	{
		win->obj.vt->destroy(&win->obj, 1);
		return 10;
	}
	// the slot is taken only once the surfaces exist
	g->windows[i] = &win->obj;
	g->windowCount++;
	Compositor_Add(g->comp, &win->obj);
	*outHandle = H_WINDOW | i;
	return 0;
}

// "90 81": 1 ok, 0 unknown handle
int Gfx_WindowDelete(Gfx_t* g, uint32_t h)
{
	return Mgr_DeleteFromTable(g, g->windows, Gfx_FindWindow(g, h), h, &g->windowCount, 0);
}

/* "90 86": the frame layer from bitmap `bmp` (-1 clears it); only the
 * handle and the fourth argument are used, the two others are the
 * instruction's unused arguments.  0 ok, 1 the window has no surfaces,
 * 2 no such bitmap, 3 not a frame bitmap (Window_SetFrame 1..3 pass
 * through), 0x10 for its result 8, 0xFF unknown handle. */
int Gfx_WindowSetFrame(Gfx_t* g, uint32_t h, int unused1, int unused2, int bmp)
{
	DispObj_t* o = Gfx_FindWindow(g, h);
	int r;

	(void)unused1;
	(void)unused2;
	if(!o)
		return NO_OBJECT;
	r = Window_SetFrame((Window_t*)o, bmp);
	switch(r)
	{
		case 0:
			InvalidateIfVisible(o);
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
int Gfx_WindowSet(Gfx_t* g, uint32_t h, int x, int y, int effect, int level, int unused, int prio)
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(!o)
		return 0;
	InvalidateIfVisible(o);
	Window_Set((Window_t*)o, x, y, effect, level, unused, prio);
	InvalidateIfVisible(o);
	Compositor_Resort(g->comp, o);
	return 1;
}

// "90 87": whether the item layers are punched out of the text layer; 1 ok, 0 unknown handle
int Gfx_WindowSetPunch(Gfx_t* g, uint32_t h, int f)
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(o)
		Window_SetPunch((Window_t*)o, f);
	return o != NULL;
}

/* "90 82" of 1.494 on: the pass order of the window's layers (0..5); the
 * whole composite is rebuilt.  0 ok, 0x14 bad order, 0xff no such
 * window. */
int Gfx_WindowSetLayerOrder(Gfx_t* g, uint32_t h, int order)
{
	DispObj_t* o = Gfx_FindWindow(g, h);
	Window_t* w = (Window_t*)o;
	int was, now;
	if(!o)
		return 0xff;
	if(Window_SetLayerOrder(w, order))
		return 0x14;
	Window_RecompositeAll(w);
	was = IsVisible(o);
	now = IsVisible(o);
	if(was || now)
		Invalidate(o);
	return 0;
}

// "90 88": the text area (x, y, w, hgt inside the window); 0 ok, 4 out of the text layer, 0xFF unknown handle
int Gfx_WindowSetTextArea(Gfx_t* g, uint32_t h, int x, int y, int w, int hgt)
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(!o)
		return NO_OBJECT;
	return Window_SetTextArea((Window_t*)o, x, y, w, hgt) ? 0 : 4;
}

// "90 89": the text area; 0 ok, 0xFF unknown handle
int Gfx_WindowGetTextArea(Gfx_t* g, Rect_t* out, uint32_t h)
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(!o)
		return NO_OBJECT;
	Window_GetTextArea((Window_t*)o, out);
	return 0;
}

// "91 8A": the text draw style (0..1); 0 ok, 0x12 bad style, 0xFF unknown handle
int Gfx_WindowSetDrawStyle(Gfx_t* g, uint32_t h, int style)
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(!o)
		return NO_OBJECT;
	return Window_SetDrawStyle((Window_t*)o, style) ? 0 : 0x12;
}

// "91 8B": the swing style (0..2); 0 ok, 0x13 bad style, 0xFF unknown handle
int Gfx_WindowSetSwingStyle(Gfx_t* g, uint32_t h, int style)
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(!o)
		return NO_OBJECT;
	return Window_SetSwingStyle((Window_t*)o, style) ? 0 : 0x13;
}

/* "91 88": the window's font.  The proportional flag and the ruby
 * reservation are stored first, then the font is opened.  0 ok, 5 size
 * out of range, 6 width percentage out of range, 7 the font could not be
 * created (the font cache's 0x80000002 .. 0x80000004), 0xFF unknown
 * handle. */
int Gfx_WindowSetFont(Gfx_t* g, uint32_t h, const char* face, int size, int widthPct, int bold,
	int proportional, int rubyReserve)
{
	DispObj_t* o = Gfx_FindWindow(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	Window_SetProportional((Window_t*)o, proportional);
	Window_SetRubyReserve((Window_t*)o, rubyReserve);
	r = (uint32_t)Window_SetFont((Window_t*)o, face, size, widthPct, bold);
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
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(!o)
		return NO_OBJECT;
	return Window_SetSpacing((Window_t*)o, pct) ? 0 : 8;
}

// "90 84": show / hide; 1 ok, 0 unknown handle
int Gfx_WindowShow(Gfx_t* g, uint32_t h, int on)
{
	return Mgr_ShowBracket(Gfx_FindWindow(g, h), on);
}

// "92 88": show / hide the frame layer; 1 ok, 0 unknown handle
int Gfx_WindowShowFrame(Gfx_t* g, uint32_t h, int f)
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(o)
	{
		Window_ShowFrame((Window_t*)o, f);
		InvalidateIfVisible(o);
	}
	return o != NULL;
}

// "92 8A": fill the frame layer with a colour; 0 ok, 0xFF unknown handle
int Gfx_WindowFillFrame(Gfx_t* g, uint32_t h, uint32_t colour)
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(!o)
		return NO_OBJECT;
	Window_FillFrame((Window_t*)o, colour);
	return 0;
}

/* the result mapping shared by drawToFrame / drawToText: 0 ok (the touched
 * screen rectangle is handed to the compositor), 2 bitmap missing, 4..7
 * (the blit failed) -> 0xC..0xF; 1 (the window has no surfaces) and
 * anything else return the handle */
static int WindowDrawResult(Gfx_t* g, DispObj_t* o, int r, const Rect_t* touched, uint32_t h)
{
	switch(r)
	{
		case 0:
			if(IsVisible(o))
				Gfx_AddDirty(g, o->vt->sortKey(o), touched);
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
int Gfx_WindowDrawToFrame(Gfx_t* g, uint32_t h, int x, int y, int bmp, int effect, int level)
{
	DispObj_t* o = Gfx_FindWindow(g, h);
	Rect_t touched;
	int r;

	if(!o)
		return NO_OBJECT;
	r = Window_DrawToFrame((Window_t*)o, &touched, x, y, bmp, effect, level);
	return WindowDrawResult(g, o, r, &touched, h);
}

/* "90 83": create bitmap bmpNo of the window's size (ARGB when the screen
 * is RGB32) and copy the composite into it.  0 ok, 2 the bitmap manager
 * refuses the number, 0xFF unknown handle. */
int Gfx_WindowCopyToBitmap(Gfx_t* g, int bmpNo, uint32_t h)
{
	DispObj_t* o = Gfx_FindWindow(g, h);
	Rect_t lr;
	Bmp_t info;
	int mode;

	if(!o)
		return NO_OBJECT;
	o->vt->localRect(o, &lr);
	mode = ScreenMode();
	if(mode == PM_RGB32)
		mode = PM_ARGB32;
	if(!BmpMgr_Create(gGfxBmpMgr, bmpNo, lr.r - lr.l + 1, lr.b - lr.t + 1, mode))
		return 2;
	BmpMgr_GetInfo(gGfxBmpMgr, &info, bmpNo);
	Window_CopyComposite((Window_t*)o, &info);
	return 0;
}

// "92 8C": show / hide the text layer; the composite is rebuilt.  1 ok, 0 unknown handle
int Gfx_WindowShowText(Gfx_t* g, uint32_t h, int f)
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(o)
	{
		Window_ShowText((Window_t*)o, f);
		Window_RecompositeAll((Window_t*)o);
		InvalidateIfVisible(o);
	}
	return o != NULL;
}

// "92 8D": draw bitmap `bmp` into the text layer at (x, y); the codes of WindowDrawResult, 0xFF unknown handle
int Gfx_WindowDrawToText(Gfx_t* g, uint32_t h, int x, int y, int bmp, int effect, int level)
{
	DispObj_t* o = Gfx_FindWindow(g, h);
	Rect_t touched;
	int r;

	if(!o)
		return NO_OBJECT;
	r = Window_DrawToText((Window_t*)o, &touched, x, y, bmp, effect, level);
	return WindowDrawResult(g, o, r, &touched, h);
}

/* "91 91" / "91 93" / "92 91": draw `str` into the text layer at the
 * cursor at once (no wait); `parseTags` interprets the ruby markup, `hang`
 * is passed to the layout as its hanging mode, `style` is the shadow
 * style.  0 ok, 0x11 nothing was drawn (the window has no font), 0xFF
 * unknown handle. */
int Gfx_WindowDrawText(Gfx_t* g, uint32_t h, const char* str, int parseTags, int hang, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5])
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(!o)
		return NO_OBJECT;
	if(!Window_DrawText((Window_t*)o, str, parseTags, hang, colour, rubyColour, style))
		return 0x11;
	InvalidateIfVisible(o);
	return 0;
}

// "92 8E": cursor home, text cleared, items off, sub-sprites gone; 1 ok, 0 unknown handle
int Gfx_WindowClear(Gfx_t* g, uint32_t h)
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(o)
	{
		Window_ResetCursor((Window_t*)o);
		Window_ClearText((Window_t*)o);
		Window_ItemsDisableAll((Window_t*)o);
		Window_SubAlloc((Window_t*)o, 0);
		InvalidateIfVisible(o);
	}
	return o != NULL;
}

// "91 8C": the text cursor; 1 ok, 0 unknown handle
int Gfx_WindowSetCursor(Gfx_t* g, uint32_t h, int x, int y)
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(o)
		Window_SetCursor((Window_t*)o, x, y);
	return o != NULL;
}

// "91 8D": the text cursor; 1 ok, 0 unknown handle
int Gfx_WindowGetCursor(Gfx_t* g, int32_t out[2], uint32_t h)
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(o)
		Window_GetCursor((Window_t*)o, out);
	return o != NULL;
}

// "91 8E": *out = whether the cursor is at the start of a line; 0 ok, 0xFF unknown handle
int Gfx_WindowCursorStatus(Gfx_t* g, int* out, uint32_t h)
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(!o)
		return NO_OBJECT;
	*out = Window_CursorAtLineStart((Window_t*)o);
	return 0;
}

// "90 A7": the window's menu colour table (16 words; NULL removes it); 1 ok, 0 unknown handle
int Gfx_WindowSetTable(Gfx_t* g, uint32_t h, const int32_t* table)
{
	DispObj_t* o = Gfx_FindWindow(g, h);

	if(o)
		Window_SetTable((Window_t*)o, table);
	return o != NULL;
}
