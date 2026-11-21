/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * frame.c - the caption bar and the border of a dialog of the dialog
 *           toolkit (src/include/bgi/ui.h) where no window manager draws them:
 *           the engine's frame (src/sys/inframe.c, and the child windows
 *           of src/sys/child.c)
 *
 * The look is the classic one of the dialogs: a raised grey border, a
 * blue caption bar with the title in white (an inactive window's in grey,
 * its title light grey) and, when the dialog can be cancelled, a close box
 * at its right end.
 */
#include "bgi/ui.h"

#define CAPTION_LEFT   0x0a246a // the caption bar's colour at its left end and at its right
#define CAPTION_RIGHT  0xa6caf0
#define INACTIVE_LEFT  0x808080 // the same of an inactive window
#define INACTIVE_RIGHT 0xc0c0c0
#define INACTIVE_TEXT  0xd4d0c8 // its title
#define CLOSE_W        16       // the close box in pixels
#define CLOSE_H        14

void Ui_FrameSize(const UiDialog_t* d, int* w, int* h)
{
	*w = d->w + 2 * UI_BORDER;
	*h = d->h + UI_CAPTION_H + 2 * UI_BORDER;
}

// the close box's top left in the framed dialog
static void ClosePos(const UiDialog_t* d, int* x, int* y)
{
	*x = UI_BORDER + d->w - CLOSE_W - 2;
	*y = UI_BORDER + (UI_CAPTION_H - CLOSE_H) / 2;
}

void Ui_FrameDraw(const UiDialog_t* d, uint32_t* pix, int pitch, int closeDown)
{
	uint32_t left = d->inactive ? INACTIVE_LEFT : CAPTION_LEFT, right = d->inactive ? INACTIVE_RIGHT : CAPTION_RIGHT;
	int w, h, x, y;
	Ui_FrameSize(d, &w, &h);
	// the raised border: light above and left, dark below and right, the face inside
	Ui_FillRect(pix, pitch, w, h, 0, 0, w, h, UI_FACE);
	Ui_FillRect(pix, pitch, w, h, 1, 1, w - 2, 1, UI_LIGHT);
	Ui_FillRect(pix, pitch, w, h, 1, 1, 1, h - 2, UI_LIGHT);
	Ui_FillRect(pix, pitch, w, h, 0, h - 1, w, 1, 0x404040);
	Ui_FillRect(pix, pitch, w, h, w - 1, 0, 1, h, 0x404040);
	Ui_FillRect(pix, pitch, w, h, 1, h - 2, w - 2, 1, UI_SHADOW);
	Ui_FillRect(pix, pitch, w, h, w - 2, 1, 1, h - 2, UI_SHADOW);
	// the caption bar, shading from left to right, and the title
	for(x = 0; x < d->w; x++)
	{
		uint32_t rgb = 0;
		int shift;
		for(shift = 0; shift <= 16; shift += 8)
		{
			int a = (int)(left >> shift) & 0xff, b = (int)(right >> shift) & 0xff;
			rgb |= (uint32_t)(a + (b - a) * x / (d->w > 1 ? d->w - 1 : 1)) << shift;
		}
		Ui_FillRect(pix, pitch, w, h, UI_BORDER + x, UI_BORDER, 1, UI_CAPTION_H - 1, rgb);
	}
	if(d->font && d->font->draw)
		d->font->draw(d->font->ctx, pix, pitch, w - UI_BORDER - CLOSE_W - 4, h, UI_BORDER + 4, UI_BORDER + (UI_CAPTION_H - 1 - d->font->height) / 2,
			d->title, d->inactive ? INACTIVE_TEXT : 0xffffff);
	if(d->escButton >= 0)
	{ // the close box: a small raised button with a cross
		int i;
		ClosePos(d, &x, &y);
		Ui_FillRect(pix, pitch, w, h, x, y, CLOSE_W, CLOSE_H, UI_FACE);
		Ui_FillRect(pix, pitch, w, h, x, y, CLOSE_W, 1, closeDown ? 0x404040 : UI_LIGHT);
		Ui_FillRect(pix, pitch, w, h, x, y, 1, CLOSE_H, closeDown ? 0x404040 : UI_LIGHT);
		Ui_FillRect(pix, pitch, w, h, x, y + CLOSE_H - 1, CLOSE_W, 1, closeDown ? UI_LIGHT : 0x404040);
		Ui_FillRect(pix, pitch, w, h, x + CLOSE_W - 1, y, 1, CLOSE_H, closeDown ? UI_LIGHT : 0x404040);
		x += (CLOSE_W - 8) / 2 + closeDown;
		y += (CLOSE_H - 7) / 2 + closeDown;
		for(i = 0; i < 7; i++)
		{ // two pixels thick
			Ui_FillRect(pix, pitch, w, h, x + i, y + i, 2, 1, UI_TEXT);
			Ui_FillRect(pix, pitch, w, h, x + 6 - i, y + i, 2, 1, UI_TEXT);
		}
	}
	// the client area
	if(d->pix)
		for(y = 0; y < d->h; y++)
			for(x = 0; x < d->w; x++)
				pix[(size_t)(UI_BORDER + UI_CAPTION_H + y) * pitch + UI_BORDER + x] = d->pix[(size_t)y * d->w + x];
}

int Ui_FrameHit(const UiDialog_t* d, int x, int y, int* cx, int* cy)
{
	int w, h, bx, by;
	Ui_FrameSize(d, &w, &h);
	*cx = x - UI_BORDER;
	*cy = y - UI_BORDER - UI_CAPTION_H;
	if(x < 0 || y < 0 || x >= w || y >= h)
		return UI_HIT_NONE;
	if(*cx >= 0 && *cx < d->w && *cy >= 0 && *cy < d->h)
		return UI_HIT_CLIENT;
	ClosePos(d, &bx, &by);
	if(d->escButton >= 0 && x >= bx && x < bx + CLOSE_W && y >= by && y < by + CLOSE_H)
		return UI_HIT_CLOSE;
	if(y >= UI_BORDER && y < UI_BORDER + UI_CAPTION_H)
		return UI_HIT_CAPTION;
	return UI_HIT_NONE; // the border
}
