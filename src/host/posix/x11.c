/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * x11.c - the POSIX back end's connection to the X server, the pixel
 *         conversions between the engine's surfaces and the visual, window
 *         titles, and the key mapping (x11_internal.h, src/host/os_x11.h)
 *
 * The engine draws by blitting its back buffer (16-bit RGB555 or 32-bit)
 * into a window; the rows are converted to the visual's pixel layout and
 * sent with XPutImage.  X keysyms are mapped to Windows virtual-key codes
 * because that is what the scripts use, and typed text is converted from
 * the input method's UTF-8 to Shift-JIS, which is what the engine's text
 * entry and dialogs store.
 */
#include "x11_internal.h"
#include "bgi/sjis.h"

// ---- connection --------------------------------------------------------------------------------

Display* gX11Dpy; // NULL until X11_Open succeeds
int gX11Screen;
Window gX11Root;
Visual* gX11Visual;
int gX11Depth;
Atom gX11WmDelete, gX11WmProtocols, gX11NetWmName, gX11Utf8String;
XIM gX11Im; // NULL without an input method

Atom gX11AtomNetWmState, gX11AtomNetWmStateFs, gX11AtomNetFrameExtents, gX11AtomNetWmPid, gX11AtomSupportingWmCheck;
Atom gX11AtomNetWorkarea;
static Atom aNetWmWindowType, aNetWmWindowTypeDialog, aWmState;
int gX11VisRShift, gX11VisGShift, gX11VisBShift, gX11VisRBits, gX11VisGBits, gX11VisBBits; // the visual's channels
static int gHostLsb;                                                                       // the host is little-endian: the byte order of the XImages

OsHostHandlers_t gX11Input;

/* Connect to the display named by DISPLAY (once; later calls return 1 at
 * once), read the default visual's channel layout, intern the atoms and
 * open the input method.  0 when there is no display: the window and
 * dialog services then do nothing, which lets the engine run headless. */
int X11_Open(void)
{
	XVisualInfo tmpl, *vi;
	int n, test = 1;
	if(gX11Dpy)
		return 1;
	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");
	gX11Dpy = XOpenDisplay(NULL);
	if(!gX11Dpy)
		return 0;
	gX11Screen = DefaultScreen(gX11Dpy);
	gX11Root = RootWindow(gX11Dpy, gX11Screen);
	gX11Visual = DefaultVisual(gX11Dpy, gX11Screen);
	gX11Depth = DefaultDepth(gX11Dpy, gX11Screen);
	gHostLsb = *(const char*)&test;

	// channel layout of the default visual (TrueColor is assumed; anything else draws garbage, not crashes)
	tmpl.visualid = gX11Visual->visualid; // what XVisualIDFromVisual returns (Xlibe leaves that one out)
	vi = XGetVisualInfo(gX11Dpy, VisualIDMask, &tmpl, &n);
	gX11VisRShift = 16, gX11VisGShift = 8, gX11VisBShift = 0, gX11VisRBits = gX11VisGBits = gX11VisBBits = 8;
	if(vi && n > 0)
	{
		unsigned long m;
		int s, b;
		for(m = vi->red_mask, s = 0; m && !(m & 1); m >>= 1, s++)
		{
		}
		for(b = 0; m & 1; m >>= 1, b++)
		{
		}
		if(b)
			gX11VisRShift = s, gX11VisRBits = b;
		for(m = vi->green_mask, s = 0; m && !(m & 1); m >>= 1, s++)
		{
		}
		for(b = 0; m & 1; m >>= 1, b++)
		{
		}
		if(b)
			gX11VisGShift = s, gX11VisGBits = b;
		for(m = vi->blue_mask, s = 0; m && !(m & 1); m >>= 1, s++)
		{
		}
		for(b = 0; m & 1; m >>= 1, b++)
		{
		}
		if(b)
			gX11VisBShift = s, gX11VisBBits = b;
	}
	if(vi)
		XFree(vi);

	gX11WmDelete = XInternAtom(gX11Dpy, "WM_DELETE_WINDOW", False);
	gX11WmProtocols = XInternAtom(gX11Dpy, "WM_PROTOCOLS", False);
	gX11NetWmName = XInternAtom(gX11Dpy, "_NET_WM_NAME", False);
	gX11Utf8String = XInternAtom(gX11Dpy, "UTF8_STRING", False);
	gX11AtomNetWmState = XInternAtom(gX11Dpy, "_NET_WM_STATE", False);
	gX11AtomSupportingWmCheck = XInternAtom(gX11Dpy, "_NET_SUPPORTING_WM_CHECK", False);
	gX11AtomNetWmStateFs = XInternAtom(gX11Dpy, "_NET_WM_STATE_FULLSCREEN", False);
	gX11AtomNetFrameExtents = XInternAtom(gX11Dpy, "_NET_FRAME_EXTENTS", False);
	aNetWmWindowType = XInternAtom(gX11Dpy, "_NET_WM_WINDOW_TYPE", False);
	aNetWmWindowTypeDialog = XInternAtom(gX11Dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	aWmState = XInternAtom(gX11Dpy, "WM_STATE", False);
	gX11AtomNetWmPid = XInternAtom(gX11Dpy, "_NET_WM_PID", False);
	gX11AtomNetWorkarea = XInternAtom(gX11Dpy, "_NET_WORKAREA", False);
#ifndef BGI_HAIKU
	XSelectInput(gX11Dpy, gX11Root, PropertyChangeMask | StructureNotifyMask); // (the desktop's scale, its work area, the screen's size)
#endif

	if(XSupportsLocale())
		gX11Im = XOpenIM(gX11Dpy, NULL, NULL, NULL);
	return 1;
}

// the window's title (Shift-JIS): _NET_WM_NAME in UTF-8, and the ASCII part as the legacy WM_NAME
void X11_SetTitle(Window win, const char* sjis)
{
	char utf8[0x400];
	XTextProperty prop;
	OsCommon_SjisToUtf8(sjis, utf8, sizeof utf8);
	XChangeProperty(gX11Dpy, win, gX11NetWmName, gX11Utf8String, 8, PropModeReplace, (unsigned char*)utf8,
		(int)strlen(utf8));
	// the legacy WM_NAME for window managers without EWMH: ASCII only
	{
		char* p = utf8;
		char ascii[0x400];
		size_t o = 0;
		for(; *p && o + 1 < sizeof ascii; p++)
			if((unsigned char)*p < 0x80)
				ascii[o++] = *p;
		ascii[o] = 0;
		p = ascii;
		if(XStringListToTextProperty(&p, 1, &prop))
		{
			XSetWMName(gX11Dpy, win, &prop);
			XFree(prop.value);
		}
	}
}

// ---- pixel conversion and blitting -------------------------------------------------------------

static uint32_t* gConv; // the XImage rows handed to XPutImage
static size_t gConvCap; // its capacity in pixels

// a scratch buffer of at least `pixels` 32-bit pixels, grown as needed and kept; NULL when out of memory
uint32_t* X11_ConvBuffer(size_t pixels)
{
	if(pixels > gConvCap)
	{
		free(gConv);
		gConv = (uint32_t*)malloc(pixels * 4);
		gConvCap = gConv ? pixels : 0;
	}
	return gConv;
}

// 0x00RRGGBB -> the visual's pixel (each channel cut to the visual's depth and moved to its position)
uint32_t X11_PackPixel(uint32_t rgb)
{
	uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
	if(gX11VisRBits == 8 && gX11VisGBits == 8 && gX11VisBBits == 8 && gX11VisRShift == 16 && gX11VisGShift == 8 && gX11VisBShift == 0)
		return rgb; // the usual 24-bit visual: nothing to do
	return ((r >> (8 - gX11VisRBits)) << gX11VisRShift) | ((g >> (8 - gX11VisGBits)) << gX11VisGShift) | ((b >> (8 - gX11VisBBits)) << gX11VisBShift);
}

// one source pixel of a surface as 0x00RRGGBB (16-bit surfaces are RGB555, expanded to 8 bits per channel)
uint32_t X11_SurfacePixel(const OsSurface_t* s, int x, int y)
{
	const uint8_t* row = (const uint8_t*)s->pixels + (ptrdiff_t)y * s->pitch;
	if(s->bpp == 16)
	{
		uint32_t v = ((const uint16_t*)row)[x];
		uint32_t r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, b = v & 0x1f;
		return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
	}
	return ((const uint32_t*)row)[x] & 0xffffff;
}

/* w x h pixels in the visual's layout (32 bits each, pitch w) to (x, y)
 * of the window; `data` is usually the X11_ConvBuffer and stays the
 * caller's */
void X11_PutImage(Window win, int x, int y, int w, int h, uint32_t* data)
{
	XImage* img;
	GC gc;
	if(w <= 0 || h <= 0)
		return;
	img = XCreateImage(gX11Dpy, gX11Visual, gX11Depth, ZPixmap, 0, (char*)data, w, h, 32, w * 4);
	if(!img)
		return;
	img->byte_order = gHostLsb ? LSBFirst : MSBFirst;
	// the rows hold 32 bits per pixel whatever the depth: Xlib picks that for
	// depth 24 from the server's formats, but Xlibe takes the depth itself
	// (24: packed 3-byte pixels) - and for a shallower visual Xlib converts
	img->bits_per_pixel = 32;
	img->bitmap_unit = 32;
	gc = XCreateGC(gX11Dpy, win, 0, NULL);
	XPutImage(gX11Dpy, win, gc, img, 0, 0, x, y, (unsigned)w, (unsigned)h);
	XFreeGC(gX11Dpy, gc);
	img->data = NULL; // the buffer is the caller's: XDestroyImage must not free it
	XDestroyImage(img);
	XFlush(gX11Dpy);
}

// a 0x00RRGGBB buffer (pitch in pixels) converted to the visual's layout and put at (x, y) of the window
void X11_PutArgb(Window win, int x, int y, int w, int h, const uint32_t* pix, int pitch)
{
	uint32_t* d;
	int yy, xx;
	if(pitch == w && gX11VisRBits == 8 && gX11VisGBits == 8 && gX11VisBBits == 8 && gX11VisRShift == 16 && gX11VisGShift == 8 &&
		gX11VisBShift == 0)
	{ // the visual's own layout: the rows go as they are (XPutImage only reads them)
		X11_PutImage(win, x, y, w, h, (uint32_t*)(uintptr_t)pix);
		return;
	}
	d = X11_ConvBuffer((size_t)w * h);
	if(!d)
		return;
	for(yy = 0; yy < h; yy++)
		for(xx = 0; xx < w; xx++)
			d[yy * w + xx] = X11_PackPixel(pix[(size_t)yy * pitch + xx]);
	X11_PutImage(win, x, y, w, h, d);
}

// ---- keys --------------------------------------------------------------------------------------

/* X keysym -> Windows virtual key (0 = none).  Letters and digits map to
 * their ASCII codes, the function keys to VK_F1 .., the keypad to
 * VK_NUMPAD0 ..; the punctuation keys get the VK_OEM_* codes of a US
 * layout, so a script that tests them sees what it would on Windows. */
static int KeySymToVk(KeySym ks)
{
	if(ks >= XK_a && ks <= XK_z)
		return 'A' + (int)(ks - XK_a);
	if(ks >= XK_A && ks <= XK_Z)
		return 'A' + (int)(ks - XK_A);
	if(ks >= XK_0 && ks <= XK_9)
		return '0' + (int)(ks - XK_0);
	if(ks >= XK_F1 && ks <= XK_F24)
		return 0x70 + (int)(ks - XK_F1);
	if(ks >= XK_KP_0 && ks <= XK_KP_9)
		return 0x60 + (int)(ks - XK_KP_0);
	switch(ks)
	{
		case XK_BackSpace: return 0x08;
		case XK_Tab:
		case XK_ISO_Left_Tab: return 0x09;
		case XK_Clear: return 0x0c;
		case XK_Return:
		case XK_KP_Enter: return 0x0d;
		case XK_Shift_L:
		case XK_Shift_R: return 0x10;
		case XK_Control_L:
		case XK_Control_R: return 0x11;
		case XK_Alt_L:
		case XK_Alt_R:
		case XK_Meta_L:
		case XK_Meta_R: return 0x12;
		case XK_Pause: return 0x13;
		case XK_Caps_Lock: return 0x14;
		case XK_Kanji: return 0x19;       // VK_KANJI
		case XK_Henkan_Mode: return 0x1c; // VK_CONVERT
		case XK_Muhenkan: return 0x1d;    // VK_NONCONVERT
		case XK_Escape: return 0x1b;
		case XK_space: return 0x20;
		case XK_Page_Up:
		case XK_KP_Page_Up: return 0x21;
		case XK_Page_Down:
		case XK_KP_Page_Down: return 0x22;
		case XK_End:
		case XK_KP_End: return 0x23;
		case XK_Home:
		case XK_KP_Home: return 0x24;
		case XK_Left:
		case XK_KP_Left: return 0x25;
		case XK_Up:
		case XK_KP_Up: return 0x26;
		case XK_Right:
		case XK_KP_Right: return 0x27;
		case XK_Down:
		case XK_KP_Down: return 0x28;
		case XK_Print: return 0x2c;
		case XK_Insert:
		case XK_KP_Insert: return 0x2d;
		case XK_Delete:
		case XK_KP_Delete: return 0x2e;
		case XK_Help: return 0x2f;
		case XK_Super_L: return 0x5b;
		case XK_Super_R: return 0x5c;
		case XK_Menu: return 0x5d;
		case XK_KP_Multiply: return 0x6a;
		case XK_KP_Add: return 0x6b;
		case XK_KP_Separator: return 0x6c;
		case XK_KP_Subtract: return 0x6d;
		case XK_KP_Decimal: return 0x6e;
		case XK_KP_Divide: return 0x6f;
		case XK_Num_Lock: return 0x90;
		case XK_Scroll_Lock: return 0x91;
		case XK_semicolon:
		case XK_colon: return 0xba;
		case XK_plus:
		case XK_equal: return 0xbb;
		case XK_comma:
		case XK_less: return 0xbc;
		case XK_minus:
		case XK_underscore: return 0xbd;
		case XK_period:
		case XK_greater: return 0xbe;
		case XK_slash:
		case XK_question: return 0xbf;
		case XK_grave:
		case XK_asciitilde: return 0xc0;
		case XK_bracketleft:
		case XK_braceleft: return 0xdb;
		case XK_backslash:
		case XK_bar:
		case XK_yen: return 0xdc;
		case XK_bracketright:
		case XK_braceright: return 0xdd;
		case XK_apostrophe:
		case XK_quotedbl: return 0xde;
		case XK_Hiragana_Katakana: return 0xf2; // VK_OEM_COPY on Japanese keyboards
		case XK_Zenkaku_Hankaku: return 0xf3;   // VK_OEM_AUTO
		default: return 0;
	}
}

/* The virtual key of a key event (0 when the key has none), from the
 * key's unshifted keysym so that Shift+1 is still VK '1' as on Windows;
 * *sys tells whether Windows would deliver it as a system key
 * (WM_SYSKEYDOWN): Alt held, F10, or Alt itself. */
int X11_EventVk(XKeyEvent* ev, int* sys)
{
	KeySym base = XkbKeycodeToKeysym(gX11Dpy, (KeyCode)ev->keycode, 0, 0); // unshifted
	KeySym shifted = XLookupKeysym(ev, 0);
	int vk;
	if(base >= XK_KP_Space && base <= XK_KP_9)
	{ // the keypad follows Num Lock: XLookupString's keysym honours it
		char dummy[8];
		KeySym ks = 0;
		XLookupString(ev, dummy, sizeof dummy, &ks, NULL);
		if(ks)
			base = ks;
	}
	vk = KeySymToVk(base);
	if(!vk)
		vk = KeySymToVk(shifted);
	// Alt+key and F10 are "system" keys (WM_SYSKEYDOWN)
	*sys = ((ev->state & Mod1Mask) != 0 && vk != 0x12) || vk == 0x79 || vk == 0x12;
	return vk;
}

/* The text a key press types, as UTF-8 into out (n bytes, NUL-
 * terminated); the byte count, 0 for a key that types nothing.  With an
 * input context the input method composes the text (Japanese through an
 * IME); without one only ASCII comes through.  Control characters are
 * left out: they are keys, not text. */
int X11_EventUtf8(XKeyEvent* ev, XIC ic, char* out, int n)
{
	char utf8[64];
	int len = 0, o = 0, i;
	KeySym ks;
	Status st;
	if(n <= 0)
		return 0;
	if(ic)
	{
		len = Xutf8LookupString(ic, ev, utf8, (int)sizeof utf8 - 1, &ks, &st);
		if(st != XLookupChars && st != XLookupBoth)
			len = 0;
	}
	else
	{
		len = XLookupString(ev, utf8, (int)sizeof utf8 - 1, &ks, NULL); // Latin-1
		if(len > 0 && (uint8_t)utf8[0] >= 0x80)
			len = 0; // a non-ASCII Latin-1 byte is not valid UTF-8: dropped
	}
	for(i = 0; i < len && o + 1 < n; i++)
		if((uint8_t)utf8[i] >= 0x20 && utf8[i] != 0x7f) // control characters are keys, not text
			out[o++] = utf8[i];
	out[o] = 0;
	return o;
}
