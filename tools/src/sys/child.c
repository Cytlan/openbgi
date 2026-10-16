/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * child.c - the child windows of "B0 10" .. "B0 1C"; interface in
 *           bgi/display.h
 *
 * A script can open up to eight small pop-up windows next to the main one
 * (debugging aids and tools rather than game UI).  Each has a back buffer
 * in the screen pixel mode; the drawing instructions render into it and
 * present it at once, and the window's paint event presents it again.
 * A handle is 0xff000000 | slot; every call but Child_Create checks only
 * that the handle names a slot, not that the slot is in use.
 *
 * The window system part - creating the pop-up, its window procedure
 * refusing a user close and forwarding key presses to the main window -
 * lives in the OS layer (OS_Child*); this file keeps the table and the
 * pixels.
 */
#include "bgi/display.h"
#include "bgi/gfx.h"
#include "bgi/gfx/bitmap.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/os.h"

#define CHILD_MAX      8           // slots, and so windows at a time
#define CHILD_TAG      0xff000000u // the top byte of a handle
#define CHILD_MIN_SIZE 0x20        // client width / height in pixels
#define CHILD_MAX_SIZE 0x400

typedef struct ChildWindow
{
	OsChild_t* win; // the window; NULL = free slot
	int closing;    // set by Child_Destroy: the window procedure may let the close through
	Bmp_t back;     // the client picture, screen pixel mode
	char* copyText; // "B0 1C" (1.69 build 472 on): what Ctrl+C in the window puts on the clipboard
} ChildWindow_t;

static ChildWindow_t gChildren[CHILD_MAX];
static int gChildTableUp; // the table has been initialised and not yet torn down

// Clear the table (start-up; no windows exist yet).
void Child_InitTable(void)
{
	memset(gChildren, 0, sizeof gChildren);
	gChildTableUp = 1;
}

// Close every open child window (shutdown).
void Child_DestroyAll(void)
{
	int i;
	if(!gChildTableUp)
		return;
	for(i = 0; i < CHILD_MAX; i++)
		if(gChildren[i].win)
			Child_Destroy(CHILD_TAG | (uint32_t)i);
	gChildTableUp = 0;
}

// the slot of a handle, whether or not it is in use; NULL when the handle names none
static ChildWindow_t* Child_Find(uint32_t handle)
{
	if((handle & 0xff000000u) != CHILD_TAG || (handle & 0xffffffu) >= CHILD_MAX)
		return NULL;
	return &gChildren[handle & 0xffffffu];
}

/* (The original also has a lookup of the slot owning a given window
 * handle, serving its child window procedure; the OS layer here keys its
 * child windows by slot index and calls Child_OnPaint / Child_OnDestroyed
 * with it directly.) */

/* "B0 10": a hidden pop-up window with a w x h client area at the screen
 * position (x, y); *out receives the handle.  0 ok, 0x80000001 w or h
 * outside 0x20 .. 0x400, 0x80000002 all slots in use.  The back buffer
 * starts black. */
uint32_t Child_Create(uint32_t* out, const char* title, int x, int y, int w, int h)
{
	ChildWindow_t* c;
	int slot, fw, fh;

	if(w < CHILD_MIN_SIZE || w > CHILD_MAX_SIZE || h < CHILD_MIN_SIZE || h > CHILD_MAX_SIZE)
		return 0x80000001;
	for(slot = 0; slot < CHILD_MAX; slot++)
		if(!gChildren[slot].win)
			break;
	if(slot == CHILD_MAX)
		return 0x80000002;
	c = &gChildren[slot];
	*out = CHILD_TAG | (uint32_t)slot;

	Bmp_AllocScreen(&c->back, w, h, 0);
	Bmp_Fill(&c->back, NULL, 0);
	// the frame is added so that the client area is w x h
	OS_FrameMetrics(&fw, &fh);
	c->win = OS_ChildCreate(slot, title, x, y, w + fw, h + fh);
	c->closing = 0;
	return 0;
}

/* "B0 1C" of 1.69 build 472 on: the text that Ctrl+C in the child window
 * copies to the clipboard.  Stored only: the OS layer's child windows
 * take no keys yet.  1 when the handle names a slot, 0 otherwise. */
int Child_SetCopyText(uint32_t handle, const char* text)
{
	ChildWindow_t* c = Child_Find(handle);
	if(!c)
		return 0;
	BGI_Free(c->copyText);
	c->copyText = (char*)BGI_Alloc(strlen(text) + 1);
	strcpy(c->copyText, text);
	return 1;
}

/* "B0 11": close the window.  The slot is freed when the window reports
 * its destruction (Child_OnDestroyed), not here.  1 when the handle names
 * a slot, 0 otherwise. */
int Child_Destroy(uint32_t handle)
{
	ChildWindow_t* c = Child_Find(handle);
	if(!c)
		return 0;
	c->closing = 1;
	OS_ChildClose(c->win);
	return 1;
}

// "B0 14": show (non-zero) or hide the window; 1 when the handle names a slot
int Child_Show(uint32_t handle, int show)
{
	ChildWindow_t* c = Child_Find(handle);
	if(!c)
		return 0;
	OS_ChildShow(c->win, show != 0);
	return 1;
}

// "B0 16": move the window's outer rectangle to screen position (x, y); 1 when the handle names a slot
int Child_Move(uint32_t handle, int x, int y)
{
	ChildWindow_t* c = Child_Find(handle);
	int fw, fh;
	if(!c)
		return 0;
	OS_FrameMetrics(&fw, &fh);
	OS_ChildMove(c->win, x, y, c->back.w + fw, c->back.h + fh);
	return 1;
}

// "B0 17": the window's screen position as the window manager reports it; 1 when the handle names a slot
int Child_GetPos(int32_t out[2], uint32_t handle)
{
	ChildWindow_t* c = Child_Find(handle);
	int x, y;
	if(!c)
		return 0;
	OS_ChildGetPos(c->win, &x, &y);
	out[0] = x;
	out[1] = y;
	return 1;
}

// "B0 15": the window's title; 1 when the handle names a slot
int Child_SetTitle(uint32_t handle, const char* title)
{
	ChildWindow_t* c = Child_Find(handle);
	if(!c)
		return 0;
	OS_ChildSetTitle(c->win, title);
	return 1;
}

// present the whole back buffer in the window
static void Child_Present(ChildWindow_t* c)
{
	OsDc_t* dc = OS_ChildGetDc(c->win);
	Window_DcBlit(dc, 0, 0, &c->back);
	OS_ChildReleaseDc(c->win, dc);
}

// "B0 18": fill the back buffer with a colour and present it; 1 when the handle names a slot
int Child_Fill(uint32_t handle, uint32_t colour)
{
	ChildWindow_t* c = Child_Find(handle);
	if(!c)
		return 0;
	Bmp_Fill(&c->back, NULL, colour);
	Child_Present(c);
	return 1;
}

/* "B0 19": blit managed bitmap `bmp` into the back buffer at (x, y) with
 * a blit effect and level (Bmp_Blit), then present.  0 ok, 0xffffffff
 * bad handle, 0x80000003 no such bitmap, 0x80000004 incompatible pixel
 * modes, 0x80000005 unknown effect, 0x80000006 level out of range,
 * 0x80000007 nothing inside the client area, 0x80000008 any other blit
 * result. */
uint32_t Child_DrawBitmap(uint32_t handle, int x, int y, int bmp, int effect, int level)
{
	ChildWindow_t* c = Child_Find(handle);
	Bmp_t src;
	if(!c)
		return 0xffffffff;
	if(!BmpMgr_GetInfo(gBmpMgr, &src, bmp))
		return 0x80000003;
	switch(Bmp_Blit(&c->back, x, y, &src, effect, level))
	{
		case 0:
			Child_Present(c);
			return 0;
		case 1: return 0x80000004; // pixel modes
		case 2: return 0x80000005; // effect
		case 3: return 0x80000006; // level
		case 4: return 0x80000007; // no overlap with the client
		default: return 0x80000008;
	}
}

/* "B0 1A": draw `str` at (x, y) into the back buffer with the bitmap
 * manager's font cache (font number, pixel size, width in percent, bold,
 * proportional spacing, colour), then present; *measure receives the
 * width of the widest line.  0 ok, 0xffffffff bad handle, 0x80000009 bad
 * size, 0x8000000a bad width, 0x8000000b unknown font number. */
uint32_t Child_DrawText(uint32_t handle, int x, int y, const char* str, int fontNo, int size, int widthPct, int bold,
	int proportional, uint32_t colour, int32_t* measure)
{
	ChildWindow_t* c = Child_Find(handle);
	int handleFont = 0;
	uint32_t r;
	if(!c)
		return 0xffffffff;
	r = (uint32_t)BmpMgr_FontOpen(gBmpMgr, &handleFont, FontNameByNo(fontNo), size, widthPct, bold);
	switch(r)
	{
		case 0:
			BmpMgr_DrawText(gBmpMgr, &c->back, measure, x, y, str, handleFont, colour, 0, 0, proportional);
			Child_Present(c);
			return 0;
		case 0x80000002: return 0x80000009;   // size
		case 0x80000003: return 0x8000000a;   // width
		case 0x80000004: return 0x8000000b;   // font number (no face name)
		default: return (uint32_t)handleFont; // the original returns the font handle slot for any other code
	}
}

// ---- the window procedure ----------------------------------------------------------------

// The window asks to be painted: present the back buffer.
void Child_OnPaint(int index, void* dc)
{
	if((unsigned)index < CHILD_MAX)
		Window_DcBlit(dc, 0, 0, &gChildren[index].back);
}

/* The window is gone (closed by Child_Destroy, or by the system when the
 * main window dies): free the pixels and the copy text, clear the slot. */
void Child_OnDestroyed(int index)
{
	if((unsigned)index < CHILD_MAX)
	{
		BGI_Free(gChildren[index].back.pixels);
		BGI_Free(gChildren[index].copyText);
		memset(&gChildren[index], 0, sizeof gChildren[index]);
	}
}
