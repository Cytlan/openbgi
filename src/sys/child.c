/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * child.c - the child windows of "B0 10" .. "B0 1C"; interface in
 *           bgi/display.h
 *
 * A script can open up to eight small pop-up windows next to the main one
 * (debugging aids and tools rather than game UI: senmomo's "Debug window"
 * on F11, for one).  Each has a back buffer in the screen pixel mode; the
 * drawing instructions render into it and present it at once.  A handle
 * is 0xff000000 | slot; every call but Child_Create checks only that the
 * handle names a slot, not that the slot is in use.
 *
 * What a window shows is its back buffer - through the display scaling
 * while that is on, offset, stretched and clipped as the main picture,
 * because the original blits the child windows through the same mapping
 * (Window_MapBlit).  A host with child windows of its own
 * (OpenBGI_Host_t.childWindows) makes the windows and gets that picture
 * at every present; its windows refuse the player's close, forward their
 * keys to the main window and copy the text of "B0 1C" on Ctrl+C.
 *
 * Otherwise (or with --child-windows=frame, or where the host fits the
 * game's window to its screen and --child-windows=host does not say
 * otherwise) the engine draws the windows
 * over the frame's picture, with a border and a caption bar as its
 * dialogs have them (src/ui/frame.c) - inactive in grey, the window
 * clicked last active in blue - each an overlay (Screen_Overlay, the
 * slots from SCREEN_OVERLAY_CHILD: over the text entry, under the
 * dialogs), the one shown or clicked last over the others.  A window
 * stays where it was put relative to the main window's client area
 * (screen positions are taken from and given back in terms of where that
 * lies), drawn moved into the picture when it would not fit there.  A
 * press on a window is its own and makes it the active one, whose Ctrl+C
 * copies its text, and the main window inactive (Window_ChildFocus) until
 * a press on the picture, or the window hidden or closed; a press on the
 * caption drags it; the presses on it and the moves over it are not the
 * main window's, and its other keys go to the main window as the pop-ups
 * forward them.
 */
#include "bgi/display.h"
#include "bgi/edit.h"
#include "bgi/gfx.h"
#include "bgi/inframe.h"
#include "bgi/input.h"
#include "bgi/ui.h"
#include "bgi/gfx/bitmap.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/typeface.h"
#include "bgi/host.h"
#include "bgi/core.h"

#define CHILD_TAG      0xff000000u // the top byte of a handle
#define CHILD_MIN_SIZE 0x20        // client width / height in pixels
#define CHILD_MAX_SIZE 0x400

/* the host's child windows, NULL when the engine draws them: also where
 * the game's window is fitted to its screen (MainWin_ViewOn), because a
 * window of the host's would be of the scripts' size and at their place,
 * neither of which is the screen's there (--child-windows=host has them
 * all the same) */
static const OpenBGI_ChildWindows_t* Host(OpenBGI_t* o)
{
	if(o->engine.optChildFrame == 2)
		return o->host.childWindows; // --child-windows=host
	return o->engine.optChildFrame || MainWin_ViewOn(o) ? NULL : o->host.childWindows;
}

// Clear the table (start-up; no windows exist yet).
void Child_InitTable(OpenBGI_t* o)
{
	memset(o->child.children, 0, sizeof o->child.children);
	o->child.childTableUp = 1;
	o->child.active = -1;
	o->child.drag = -1;
	o->child.pressed = 0;
	o->child.dirty = 1;
}

// Close every open child window (shutdown), and drop the captions' font.
void Child_DestroyAll(OpenBGI_t* o)
{
	int i;
	o->child.active = -1; // (the main window is not made active again on the way out)
	o->input.childFocus = 0;
	if(o->child.childTableUp)
		for(i = 0; i < CHILD_SLOTS; i++)
		{
			ChildWindow_t* c = &o->child.children[i];
			if(c->used)
				Child_Destroy(o, CHILD_TAG | (uint32_t)i);
			BGI_Free(c->back.pixels); // (what a window the host could not make left)
			BGI_Free(c->scaled.pixels);
			memset(c, 0, sizeof *c);
		}
	for(i = 0; i < CHILD_SLOTS; i++)
		if(o->screen.overlay[SCREEN_OVERLAY_CHILD + i].pix)
			Screen_Overlay(o, SCREEN_OVERLAY_CHILD + i, NULL, 0, 0, 0, 0, 0, 0); // (the windows the engine drew)
	Typeface_Close(o->child.face);
	o->child.face = NULL;
	BGI_Free(o->child.font);
	o->child.font = NULL;
	o->child.childTableUp = 0;
}

// the slot of a handle, whether or not it is in use; NULL when the handle names none
static ChildWindow_t* Child_Find(OpenBGI_t* o, uint32_t handle)
{
	if((handle & 0xff000000u) != CHILD_TAG || (handle & 0xffffffu) >= CHILD_SLOTS)
		return NULL;
	return &o->child.children[handle & 0xffffffu];
}

/* the active window (a slot, -1 none): the main window is inactive while
 * there is one (Window_ChildFocus) */
static void SetActive(OpenBGI_t* o, int slot)
{
	if(o->child.active == slot)
		return;
	o->child.active = slot;
	o->child.dirty = 1;
	Window_ChildFocus(o, slot >= 0);
}

// ---- the picture -------------------------------------------------------------------------

/* What the window shows: its back buffer, or while the display is scaled
 * the back buffer as the original's blit puts it in the window (on black
 * where it puts nothing). */
static const Bmp_t* Picture(OpenBGI_t* o, ChildWindow_t* c)
{
	int x = 0, y = 0, dstH = c->back.h, clip[4];
	if(!Window_MapBlit(o, &x, &y, &dstH, clip))
	{
		BGI_Free(c->scaled.pixels);
		memset(&c->scaled, 0, sizeof c->scaled);
		return &c->back;
	}
	if(!c->scaled.pixels && !Bmp_AllocScreen(o, &c->scaled, c->back.w, c->back.h, 0))
		return &c->back;
	Bmp_Fill(&c->scaled, NULL, 0);
	Screen_StretchTo(&c->scaled, x, y, c->back.w, dstH, &c->back, clip);
	return &c->scaled;
}

// the back buffer changed: to the host's window, or drawn in the frame again
static void Present(OpenBGI_t* o, ChildWindow_t* c)
{
	const OpenBGI_ChildWindows_t* h = Host(o);
	if(!h)
	{
		o->child.dirty = 1;
		return;
	}
	if(c->win && h->present)
	{
		const Bmp_t* p = Picture(o, c);
		h->present(o->host.user, c->win, p->pixels, p->pitch, p->w, p->h, ModeBits(p->mode));
	}
}

// ---- the script's instructions ------------------------------------------------------------

/* "B0 10": a hidden pop-up window with a w x h client area at the screen
 * position (x, y); *out receives the handle.  0 ok, 0x80000001 w or h
 * outside 0x20 .. 0x400, 0x80000002 all slots in use.  The back buffer
 * starts black; the host's window gets it at once.  A window the host
 * could not make leaves its slot free, as a failed CreateWindowEx left
 * the original's, with the back buffer kept for the handle's calls
 * until the slot is taken again. */
uint32_t Child_Create(OpenBGI_t* o, uint32_t* out, const char* title, int x, int y, int w, int h)
{
	const OpenBGI_ChildWindows_t* host = Host(o);
	ChildWindow_t* c;
	int slot;

	if(w < CHILD_MIN_SIZE || w > CHILD_MAX_SIZE || h < CHILD_MIN_SIZE || h > CHILD_MAX_SIZE)
		return 0x80000001;
	for(slot = 0; slot < CHILD_SLOTS; slot++)
		if(!o->child.children[slot].used)
			break;
	if(slot == CHILD_SLOTS)
		return 0x80000002;
	c = &o->child.children[slot];
	BGI_Free(c->back.pixels);
	BGI_Free(c->scaled.pixels);
	memset(c, 0, sizeof *c);
	*out = CHILD_TAG | (uint32_t)slot;

	Bmp_AllocScreen(o, &c->back, w, h, 0);
	Bmp_Fill(&c->back, NULL, 0);
	if(host)
	{
		c->win = host->create ? host->create(o->host.user, slot, title, x, y, w, h) : NULL;
		c->used = c->win != NULL;
		Present(o, c);
		return 0;
	}
	c->used = 1;
	snprintf(c->title, sizeof c->title, "%s", title);
	Display_ClientOrigin(o, &c->x, &c->y);
	c->x = x - c->x;
	c->y = y - c->y;
	c->fw = w + 2 * UI_BORDER;
	c->fh = h + UI_CAPTION_H + 2 * UI_BORDER;
	c->frame = (uint32_t*)BGI_Calloc((size_t)c->fw * (size_t)c->fh * 4);
	return 0;
}

/* "B0 1C" of 1.69 build 472 on: the text that Ctrl+C in the child window
 * copies to the clipboard.  1 when the handle names a slot, 0
 * otherwise. */
int Child_SetCopyText(OpenBGI_t* o, uint32_t handle, const char* text)
{
	const OpenBGI_ChildWindows_t* h = Host(o);
	ChildWindow_t* c = Child_Find(o, handle);
	if(!c)
		return 0;
	BGI_Free(c->copyText);
	c->copyText = (char*)BGI_Alloc(strlen(text) + 1);
	strcpy(c->copyText, text);
	if(h && c->win && h->setCopyText)
		h->setCopyText(o->host.user, c->win, c->copyText);
	return 1;
}

/* "B0 11": close the window; its slot is free again (the original frees it
 * in the window's WM_DESTROY, which the close sends before it returns).
 * 1 when the handle names a slot, 0 otherwise. */
int Child_Destroy(OpenBGI_t* o, uint32_t handle)
{
	const OpenBGI_ChildWindows_t* h = Host(o);
	ChildWindow_t* c = Child_Find(o, handle);
	int slot;
	if(!c)
		return 0;
	if(!c->used)
		return 1;
	slot = (int)(c - o->child.children);
	if(h && c->win && h->close)
		h->close(o->host.user, c->win);
	BGI_Free(c->back.pixels);
	BGI_Free(c->scaled.pixels);
	BGI_Free(c->copyText);
	BGI_Free(c->frame);
	memset(c, 0, sizeof *c);
	if(o->child.drag == slot)
		o->child.drag = -1;
	o->child.dirty = 1;
	if(o->child.active == slot)
		SetActive(o, -1); // (the main window active again, as the system activates the owner)
	return 1;
}

// "B0 14": show (non-zero) or hide the window; 1 when the handle names a slot
int Child_Show(OpenBGI_t* o, uint32_t handle, int show)
{
	const OpenBGI_ChildWindows_t* h = Host(o);
	ChildWindow_t* c = Child_Find(o, handle);
	if(!c)
		return 0;
	if(h)
	{
		if(c->win && h->show)
			h->show(o->host.user, c->win, show != 0);
		return 1;
	}
	if(!c->used)
		return 1;
	if(show && !c->shown)
		c->z = ++o->child.zNext; // shown: over the others
	c->shown = show != 0;
	o->child.dirty = 1;
	if(!c->shown && o->child.active == (int)(c - o->child.children))
		SetActive(o, -1);
	return 1;
}

// "B0 16": move the window's outer rectangle to screen position (x, y); 1 when the handle names a slot
int Child_Move(OpenBGI_t* o, uint32_t handle, int x, int y)
{
	const OpenBGI_ChildWindows_t* h = Host(o);
	ChildWindow_t* c = Child_Find(o, handle);
	int cx, cy;
	if(!c)
		return 0;
	if(h)
	{
		if(c->win && h->move)
			h->move(o->host.user, c->win, x, y);
		return 1;
	}
	Display_ClientOrigin(o, &cx, &cy);
	c->x = x - cx;
	c->y = y - cy;
	o->child.dirty = 1;
	return 1;
}

/* "B0 17": the window's screen position as the window manager reports it
 * (in the frame: where it lies over the main window's client area now);
 * 1 when the handle names a slot */
int Child_GetPos(OpenBGI_t* o, int32_t out[2], uint32_t handle)
{
	const OpenBGI_ChildWindows_t* h = Host(o);
	ChildWindow_t* c = Child_Find(o, handle);
	int x = 0, y = 0;
	if(!c)
		return 0;
	if(h)
	{
		if(c->win && h->getPos)
			h->getPos(o->host.user, c->win, &x, &y);
	}
	else if(c->used)
	{
		Display_ClientOrigin(o, &x, &y);
		x += c->x;
		y += c->y;
	}
	out[0] = x;
	out[1] = y;
	return 1;
}

// "B0 15": the window's title; 1 when the handle names a slot
int Child_SetTitle(OpenBGI_t* o, uint32_t handle, const char* title)
{
	const OpenBGI_ChildWindows_t* h = Host(o);
	ChildWindow_t* c = Child_Find(o, handle);
	if(!c)
		return 0;
	if(h)
	{
		if(c->win && h->setTitle)
			h->setTitle(o->host.user, c->win, title);
		return 1;
	}
	snprintf(c->title, sizeof c->title, "%s", title);
	o->child.dirty = 1;
	return 1;
}

// "B0 18": fill the back buffer with a colour and present it; 1 when the handle names a slot
int Child_Fill(OpenBGI_t* o, uint32_t handle, uint32_t colour)
{
	ChildWindow_t* c = Child_Find(o, handle);
	if(!c)
		return 0;
	Bmp_Fill(&c->back, NULL, colour);
	Present(o, c);
	return 1;
}

/* "B0 19": blit managed bitmap `bmp` into the back buffer at (x, y) with
 * a blit effect and level (Bmp_Blit), then present.  0 ok, 0xffffffff
 * bad handle, 0x80000003 no such bitmap, 0x80000004 incompatible pixel
 * modes, 0x80000005 unknown effect, 0x80000006 level out of range,
 * 0x80000007 nothing inside the client area, 0x80000008 any other blit
 * result. */
uint32_t Child_DrawBitmap(OpenBGI_t* o, uint32_t handle, int x, int y, int bmp, int effect, int level)
{
	ChildWindow_t* c = Child_Find(o, handle);
	Bmp_t src;
	if(!c)
		return 0xffffffff;
	if(!BmpMgr_GetInfo(o->gfx.bmpMgr, &src, bmp))
		return 0x80000003;
	switch(Bmp_Blit(&c->back, x, y, &src, effect, level))
	{
		case 0:
			Present(o, c);
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
uint32_t Child_DrawText(OpenBGI_t* o, uint32_t handle, int x, int y, const char* str, int fontNo, int size, int widthPct, int bold,
	int proportional, uint32_t colour, int32_t* measure)
{
	ChildWindow_t* c = Child_Find(o, handle);
	int handleFont = 0;
	uint32_t r;
	if(!c)
		return 0xffffffff;
	r = (uint32_t)BmpMgr_FontOpen(o, o->gfx.bmpMgr, &handleFont, FontNameByNo(o, fontNo), size, widthPct, bold);
	switch(r)
	{
		case 0:
			BmpMgr_DrawText(o, o->gfx.bmpMgr, &c->back, measure, x, y, str, handleFont, colour, 0, 0, proportional);
			Present(o, c);
			return 0;
		case 0x80000002: return 0x80000009;   // size
		case 0x80000003: return 0x8000000a;   // width
		case 0x80000004: return 0x8000000b;   // font number (no face name)
		default: return (uint32_t)handleFont; // the original returns the font handle slot for any other code
	}
}

// ---- the windows in the frame ---------------------------------------------------------------

// the captions' font: the dialogs' typeface, made on first use
static int FontDraw(void* ctx, uint32_t* pix, int pitch, int w, int h, int x, int y, const char* text, uint32_t rgb)
{
	return Typeface_SmoothText((Typeface_t*)ctx, pix, pitch, w, h, x, y, text, rgb, 0);
}

static int FontWidth(void* ctx, const char* text, int bytes)
{
	return Typeface_SmoothWidth((Typeface_t*)ctx, text, bytes);
}

static const UiFont_t* Font(OpenBGI_t* o)
{
	if(!o->child.font)
	{
		UiFont_t* f = (UiFont_t*)BGI_Calloc(sizeof *f);
		o->child.face = Inframe_OpenFont(o);
		f->ctx = o->child.face;
		f->height = UI_FONT_H;
		if(o->child.face)
		{
			f->draw = FontDraw;
			f->width = FontWidth;
		}
		o->child.font = f;
	}
	return o->child.font;
}

// the window's top left in the picture: where it was put, moved into the picture when it would not fit there
static void Place(OpenBGI_t* o, const ChildWindow_t* c, int* x, int* y)
{
	*x = c->x;
	*y = c->y;
	if(*x + c->fw > o->screen.pic.w)
		*x = o->screen.pic.w - c->fw;
	if(*y + c->fh > o->screen.pic.h)
		*y = o->screen.pic.h - c->fh;
	if(*x < 0)
		*x = 0;
	if(*y < 0)
		*y = 0;
}

/* the framing of window c for the toolkit: its client size, its title,
 * a close box (the system menu's, which refuses the player's close) */
static void Framing(OpenBGI_t* o, const ChildWindow_t* c, UiDialog_t* d)
{
	memset(d, 0, sizeof *d);
	d->w = c->back.w;
	d->h = c->back.h;
	d->font = Font(o);
	d->escButton = 0;
	d->defButton = -1;
	d->inactive = o->child.active != (int)(c - o->child.children);
	snprintf(d->title, sizeof d->title, "%s", c->title);
}

// the window with its border and caption bar into its frame buffer
static void Draw(OpenBGI_t* o, ChildWindow_t* c)
{
	UiDialog_t d;
	if(!c->frame)
		return;
	Framing(o, c, &d);
	Ui_FrameDraw(&d, c->frame, c->fw, 0);
	Screen_ToRgb32(c->frame + (size_t)(UI_BORDER + UI_CAPTION_H) * (size_t)c->fw + UI_BORDER, c->fw, Picture(o, c));
}

/* At the end of a frame: when anything changed (a window made, shown,
 * hidden, moved, dragged, drawn into, clicked, the picture's size), the
 * shown windows are drawn again and laid over the picture in their
 * stacking order. */
void Child_Frame(OpenBGI_t* o, OpenBGI_Frame_t* f)
{
	ChildWindow_t* order[CHILD_SLOTS];
	int n = 0, i, j;
	BGI_UNUSED(f);
	if(Host(o) || !o->child.childTableUp)
		return;
	if(o->screen.pic.w != o->child.picW || o->screen.pic.h != o->child.picH)
	{
		o->child.picW = o->screen.pic.w;
		o->child.picH = o->screen.pic.h;
		o->child.dirty = 1;
	}
	if(!o->child.dirty)
		return;
	o->child.dirty = 0;
	for(i = 0; i < CHILD_SLOTS; i++)
	{
		ChildWindow_t* c = &o->child.children[i];
		if(!c->used || !c->shown || !c->frame)
			continue;
		for(j = n; j > 0 && order[j - 1]->z > c->z; j--)
			order[j] = order[j - 1];
		order[j] = c;
		n++;
	}
	for(i = 0; i < CHILD_SLOTS; i++)
		if(i < n)
		{
			int x, y;
			Draw(o, order[i]);
			Place(o, order[i], &x, &y);
			Screen_Overlay(o, SCREEN_OVERLAY_CHILD + i, order[i]->frame, order[i]->fw, x, y, order[i]->fw, order[i]->fh, 0);
		}
		else if(o->screen.overlay[SCREEN_OVERLAY_CHILD + i].pix)
			Screen_Overlay(o, SCREEN_OVERLAY_CHILD + i, NULL, 0, 0, 0, 0, 0, 0);
}

/* the shown window on top at (x, y) of the picture, its top left in *wx,
 * *wy; NULL when there is none there */
static ChildWindow_t* At(OpenBGI_t* o, int x, int y, int* wx, int* wy)
{
	ChildWindow_t* best = NULL;
	int i;
	for(i = 0; i < CHILD_SLOTS; i++)
	{
		ChildWindow_t* c = &o->child.children[i];
		int cx, cy;
		if(!c->used || !c->shown || !c->frame)
			continue;
		Place(o, c, &cx, &cy);
		if(x < cx || y < cy || x >= cx + c->fw || y >= cy + c->fh || (best && best->z > c->z))
			continue;
		best = c;
		*wx = cx;
		*wy = cy;
	}
	return best;
}

/* A press on a window is its own: it becomes the active window, over the
 * others, the text entry loses the focus, and on the caption the left
 * button drags it; that button's release is the window's too.  A press
 * elsewhere leaves no window active. */
int Child_Button(OpenBGI_t* o, int button, int down, int x, int y)
{
	ChildWindow_t* c;
	UiDialog_t d;
	int wx = 0, wy = 0, cx, cy, slot;
	if(Host(o) || !o->child.childTableUp || button < 0 || button > 4)
		return 0;
	if(!down)
	{
		if(!(o->child.pressed & (1 << button)))
			return 0;
		o->child.pressed &= ~(1 << button);
		if(button == OPENBGI_BUTTON_LEFT)
			o->child.drag = -1;
		return 1;
	}
	c = At(o, x, y, &wx, &wy);
	if(!c)
	{
		SetActive(o, -1);
		return 0;
	}
	slot = (int)(c - o->child.children);
	if(c->z != o->child.zNext)
	{
		c->z = ++o->child.zNext;
		o->child.dirty = 1;
	}
	SetActive(o, slot);
	Edit_Unfocus(o);
	o->child.pressed |= 1 << button;
	Framing(o, c, &d);
	if(button == OPENBGI_BUTTON_LEFT && Ui_FrameHit(&d, x - wx, y - wy, &cx, &cy) == UI_HIT_CAPTION)
	{
		o->child.drag = slot;
		o->child.dragX = x - wx;
		o->child.dragY = y - wy;
		c->x = wx; // from where it is drawn
		c->y = wy;
	}
	return 1;
}

// the pointer drags a window by its caption; the moves over a window are its own
int Child_Pointer(OpenBGI_t* o, int x, int y)
{
	int wx, wy;
	if(Host(o) || !o->child.childTableUp)
		return 0;
	if(o->child.drag >= 0)
	{
		ChildWindow_t* c = &o->child.children[o->child.drag];
		if(c->x != x - o->child.dragX || c->y != y - o->child.dragY)
		{
			c->x = x - o->child.dragX;
			c->y = y - o->child.dragY;
			o->child.dirty = 1;
		}
		return 1;
	}
	return At(o, x, y, &wx, &wy) != NULL;
}

// whether a window drawn in the frame lies at (x, y), so that a click there is its own; nothing changes
int Child_Over(OpenBGI_t* o, int x, int y)
{
	int wx, wy;
	if(Host(o) || !o->child.childTableUp)
		return 0;
	return At(o, x, y, &wx, &wy) != NULL;
}

/* Ctrl+C while a window is the active one: its copy text to the clipboard
 * (nothing without one), the key not the main window's; its other keys
 * are, as the pop-ups forward them. */
int Child_Key(OpenBGI_t* o, int vk, int down, int flags)
{
	ChildWindow_t* c;
	BGI_UNUSED(flags);
	if(Host(o) || !o->child.childTableUp || o->child.active < 0 || vk != 'C' || !down || !Input_KeyDown(o, 0x11))
		return 0;
	c = &o->child.children[o->child.active];
	if(c->copyText)
		Host_ClipboardSetText(o, c->copyText);
	return 1;
}
