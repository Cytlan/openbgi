/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * edit.c - the text entry of the POSIX back end ("B0 2x" of the scripts)
 *          (inc/bgi/os.h, x11_internal.h)
 *
 * The original's EDIT control sits over the picture; it draws its text
 * centred with a transparent background on top of whatever the engine
 * presented under it (edit_paint).  This back end draws the field itself:
 * on every refresh the engine's present of the entry's rectangle is
 * captured into a buffer (gX11Capture, fed by the blits of window.c
 * while it is active), the text is drawn onto that with the field widget
 * of field.c, and the result is put on the window.  The keys reach it
 * through X11Edit_Key from the event pump while it has the focus.
 */
#include "x11_internal.h"

static struct
{
	int visible, focus;  // shown over the picture / has the keyboard
	int x, y, w, h;      // the control's rectangle in client pixels
	X11Field_t field;    // the text and its editing state
	struct OsFont* font; // the font of OS_EditSetFont, NULL before one was set
	int fontSize;        // its size as requested
	char face[0x80];     // its face name (Shift-JIS)
	uint32_t rgb;        // the text colour, 0x00RRGGBB
} gEdit;

int X11Edit_Visible(void)
{
	return gEdit.visible;
}

int X11Edit_HasFocus(void)
{
	return gEdit.focus;
}

// the entry as the original creates it with the window: hidden, 1 x 1, empty
void X11Edit_Create(void)
{
	memset(&gEdit, 0, sizeof gEdit);
	gEdit.w = gEdit.h = 1;
	gEdit.rgb = 0;
	X11Field_SetText(&gEdit.field, "");
}

/* repaint the entry: capture what the engine presents under it
 * (edit_paint with the capture active), draw the text centred over that,
 * put the result on the window.  Nothing while hidden or without a
 * window. */
void X11Edit_Present(void)
{
	int w = gEdit.w, h = gEdit.h;
	OsSurface_t s;
	if(!gEdit.visible || !gX11Alive || w <= 0 || h <= 0)
		return;
	free(gX11Capture.pix);
	gX11Capture.pix = (uint32_t*)calloc((size_t)w * h, 4);
	if(!gX11Capture.pix)
		return;
	gX11Capture.x = gEdit.x;
	gX11Capture.y = gEdit.y;
	gX11Capture.w = w;
	gX11Capture.h = h;
	gX11Capture.active = 1;
	if(gX11Handlers.edit_paint)
		gX11Handlers.edit_paint();
	gX11Capture.active = 0;
	X11Field_Draw(&gEdit.field, gX11Capture.pix, w, w, h, 0, 0, w, h, gEdit.font, gEdit.rgb, 1, gEdit.focus, 0);
	s.pixels = gX11Capture.pix;
	s.pitch = w * 4;
	s.width = w;
	s.height = h;
	s.bpp = 32;
	X11_BlitSurface(&gX11MainDc, gEdit.x, gEdit.y, w, h, &s, 0, 0, w, h, 0, NULL);
}

// show: paint the entry; hide: drop the focus and have the engine repaint the picture it covered
void OS_EditShow(int show)
{
	gEdit.visible = show != 0;
	if(show)
		X11Edit_Present();
	else
	{
		gEdit.focus = 0;
		if(gX11Handlers.paint && gX11Alive && gX11Shown)
			gX11Handlers.paint(); // uncover the picture
	}
}

// MoveWindow in client coordinates (never smaller than 1 x 1), then a repaint
void OS_EditMove(int x, int y, int w, int h)
{
	gEdit.x = x;
	gEdit.y = y;
	gEdit.w = w > 0 ? w : 1;
	gEdit.h = h > 0 ? h : 1;
	X11Edit_Present();
}

// replace the font: the same face and size the original's CreateFontA asks for, then a repaint
void OS_EditSetFont(const char* face, int size)
{
	if(gEdit.font)
		OS_FontClose(gEdit.font);
	snprintf(gEdit.face, sizeof gEdit.face, "%s", face);
	gEdit.fontSize = size;
	gEdit.font = OS_FontOpen(face, size, size / 2, 0, 0); // CreateFontA(size, size / 2, .., FW_THIN, ..)
	X11Edit_Present();
}

// EM_LIMITTEXT: the limit in bytes; 0 or one beyond the buffer means unlimited (the buffer size still applies)
void OS_EditLimitText(int maxChars)
{
	gEdit.field.limit = maxChars > 0 && maxChars < (int)sizeof gEdit.field.text ? maxChars : 0;
}

void OS_EditSetText(const char* t)
{
	X11Field_SetText(&gEdit.field, t);
	X11Edit_Present();
}

void OS_EditSelectAll(void)
{
	X11Field_SelectAll(&gEdit.field);
	X11Edit_Present();
}

/* SetFocus(the control / the main window): the entry takes the keys
 * while visible, and the input method's focus follows so that an IME
 * composes only into the field */
void OS_EditFocus(int toEdit)
{
	gEdit.focus = toEdit != 0 && gEdit.visible;
	if(gX11Xic)
	{
		if(gEdit.focus)
			XSetICFocus(gX11Xic);
		else
			XUnsetICFocus(gX11Xic);
	}
	X11Edit_Present();
}

// GetWindowText: the text into buf (n bytes, NUL-terminated, cut when too long); the length copied
int OS_EditGetText(char* buf, size_t n)
{
	size_t len = strlen(gEdit.field.text);
	if(n == 0)
		return 0;
	if(len >= n)
		len = n - 1;
	memcpy(buf, gEdit.field.text, len);
	buf[len] = 0;
	return (int)len;
}

void OS_EditSetColour(uint32_t rgb)
{
	gEdit.rgb = rgb & 0xffffff;
	X11Edit_Present();
}

void OS_EditRefresh(void)
{
	X11Edit_Present();
}

// "B0 29": single-byte printable characters are swallowed (the field widget checks the flag)
void OS_EditRejectAscii(int reject)
{
	gEdit.field.noAscii = reject;
}

// "B0 23": the IME open state is not known here; reported as closed
int OS_EditImeOpen(void)
{
	return 0; // the XIM state is not tracked
}

// the control goes with the window: release the font, hide, drop the focus
void OS_EditDestroy(void)
{
	if(gEdit.font)
		OS_FontClose(gEdit.font);
	gEdit.font = NULL;
	gEdit.visible = 0;
	gEdit.focus = 0;
}

/* a key press while the entry is visible and has the focus (the pump
 * calls it first): Tab and Enter hand the focus back as the original's
 * subclass does, everything else edits the field; 1 when consumed, 0
 * when the entry is not taking keys */
int X11Edit_Key(XKeyEvent* ev)
{
	int sys, vk, r;
	if(!gEdit.visible || !gEdit.focus)
		return 0;
	vk = X11_EventVk(ev, &sys);
	if(vk == 0x09 || vk == 0x0d)
	{ // Tab / Enter: select everything and hand the focus back
		X11Field_SelectAll(&gEdit.field);
		OS_EditFocus(0);
		return 1;
	}
	r = X11Field_Key(&gEdit.field, ev, gX11Xic);
	if(r >= 0)
	{
		if(r > 0)
			X11Edit_Present();
		return 1;
	}
	return 1; // other keys go nowhere while the control has the focus
}
