/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * screen.c - the window's picture: what the main window shows, kept by
 *            the engine and handed to the host after every frame
 *            (openbgi.h, OpenBGI_Frame_t); interface in bgi/display.h
 *
 * Everything that reaches the main window lands here: the compositor's
 * presents of the back buffer (or of its stretched copy on a letter-boxed
 * full screen), the screen quake's shifted copies, the bitmaps of "B0 00",
 * the frames of the decoded movies and the debugger's overlay.  The
 * picture has the back buffer's pixel mode and rows padded to 4 bytes,
 * as the original's DIBs.  What changed is kept as a short list of
 * rectangles until the frame ends; a change that does not fit is merged
 * into the rectangle it enlarges least.  The text entry, the child
 * windows and the dialogs drawn in the frame lie over the picture without
 * changing it (Screen_Overlay): the frames hand over a copy with them on
 * top.  Nothing goes to the window itself: the host shows the frames and
 * paints its window again from them when it is uncovered.
 */
#include "bgi/display.h"
#include "bgi/gfx.h"
#include "bgi/core.h"

#include <string.h>

// ---- the picture ---------------------------------------------------------------------

/* A buffer of the picture goes: at once, unless it is what the last frame
 * handed to the host - the host may read that until the next frame
 * (openbgi.h), whatever the events handed over in between do to the
 * picture (another display mode, say), so it is kept until then. */
static void Drop(OpenBGI_t* o, uint8_t* p)
{
	if(p && p == o->screen.handed)
	{
		BGI_Free(o->screen.kept);
		o->screen.kept = p;
	}
	else
		BGI_Free(p);
}

void Screen_NextFrame(OpenBGI_t* o)
{
	BGI_Free(o->screen.kept);
	o->screen.kept = NULL;
	o->screen.handed = NULL;
}

void Screen_Free(OpenBGI_t* o)
{
	Drop(o, o->screen.mem);
	o->screen.mem = NULL;
	Drop(o, o->screen.shown);
	o->screen.shown = NULL;
	o->screen.shownSize = 0;
	memset(&o->screen.pic, 0, sizeof o->screen.pic);
	o->screen.changes = -1;
}

void Screen_Setup(OpenBGI_t* o, int w, int h, int mode)
{
	int32_t pitch = (int32_t)((((uint32_t)w * (uint32_t)ModeBits(mode) + 31u) >> 5) << 2);
	Screen_Free(o);
	if(w <= 0 || h <= 0)
		return;
	o->screen.mem = (uint8_t*)BGI_Calloc((size_t)pitch * (size_t)h); // black
	if(!o->screen.mem)
		return;
	o->screen.pic.pixels = o->screen.mem;
	o->screen.pic.pitch = pitch;
	o->screen.pic.w = w;
	o->screen.pic.h = h;
	o->screen.pic.mode = mode;
	o->screen.pic.bpp = ModeBytes(mode);
}

// ---- the changes ---------------------------------------------------------------------

static int64_t Area(const Rect_t* r)
{
	return (int64_t)(r->r - r->l + 1) * (int64_t)(r->b - r->t + 1);
}

static Rect_t Union(const Rect_t* a, const Rect_t* b)
{
	Rect_t u;
	u.l = a->l < b->l ? a->l : b->l;
	u.t = a->t < b->t ? a->t : b->t;
	u.r = a->r > b->r ? a->r : b->r;
	u.b = a->b > b->b ? a->b : b->b;
	return u;
}

/* (l, t) .. (r, b), inclusive, changed: clipped to the picture and added
 * to the list, merged into the rectangle it enlarges least when the list
 * is full; nothing when one already holds it */
static void Changed(OpenBGI_t* o, int l, int t, int r, int b)
{
	Rect_t n, *c = o->screen.changed;
	int i, best = 0, count = o->screen.changes;
	int64_t bestGrow = -1;
	if(count < 0)
		return; // all of it already
	if(l < 0)
		l = 0;
	if(t < 0)
		t = 0;
	if(r >= o->screen.pic.w)
		r = o->screen.pic.w - 1;
	if(b >= o->screen.pic.h)
		b = o->screen.pic.h - 1;
	if(l > r || t > b)
		return;
	n.l = l;
	n.t = t;
	n.r = r;
	n.b = b;
	for(i = 0; i < count; i++)
		if(c[i].l <= l && c[i].t <= t && c[i].r >= r && c[i].b >= b)
			return;
	if(count < OPENBGI_FRAME_CHANGES)
	{
		c[count] = n;
		o->screen.changes = count + 1;
		return;
	}
	for(i = 0; i < count; i++)
	{
		Rect_t u = Union(&c[i], &n);
		int64_t grow = Area(&u) - Area(&c[i]);
		if(bestGrow < 0 || grow < bestGrow)
		{
			bestGrow = grow;
			best = i;
		}
	}
	c[best] = Union(&c[best], &n);
}

static void WriteRgb(Bmp_t* p, int x, int y, uint32_t rgb);
static uint32_t ReadRgb(const Bmp_t* b, int x, int y);

// the part of overlay i inside the picture (inclusive); 0 when there is none
static int OverlayRect(OpenBGI_t* o, int i, Rect_t* r)
{
	const Bmp_t* p = &o->screen.pic;
	r->l = o->screen.overlay[i].x < 0 ? 0 : o->screen.overlay[i].x;
	r->t = o->screen.overlay[i].y < 0 ? 0 : o->screen.overlay[i].y;
	r->r = o->screen.overlay[i].x + o->screen.overlay[i].w < p->w ? o->screen.overlay[i].x + o->screen.overlay[i].w - 1 : p->w - 1;
	r->b = o->screen.overlay[i].y + o->screen.overlay[i].h < p->h ? o->screen.overlay[i].y + o->screen.overlay[i].h - 1 : p->h - 1;
	return r->l <= r->r && r->t <= r->b;
}

// an overlay is up
static int Overlaid(OpenBGI_t* o)
{
	int i;
	for(i = 0; i < SCREEN_OVERLAYS; i++)
		if(o->screen.overlay[i].pix)
			return 1;
	return 0;
}

/* the copy of the picture the frames hand over while an overlay is up:
 * what changed in the picture copied (all of it for a new copy), the
 * overlays drawn over it; NULL when there is no memory for it */
static uint8_t* Compose(OpenBGI_t* o)
{
	const Bmp_t* p = &o->screen.pic;
	size_t size = (size_t)p->pitch * (size_t)p->h;
	Bmp_t s;
	int i;
	if(!o->screen.shown || o->screen.shownSize != size)
	{
		BGI_Free(o->screen.shown);
		o->screen.shown = (uint8_t*)BGI_Alloc(size);
		o->screen.shownSize = size;
		memcpy(o->screen.shown, p->pixels, size);
	}
	else if(o->screen.changes < 0)
		memcpy(o->screen.shown, p->pixels, size);
	else
		for(i = 0; i < o->screen.changes; i++)
		{
			const Rect_t* r = &o->screen.changed[i];
			int y, bytes = (r->r - r->l + 1) * p->bpp;
			for(y = r->t; y <= r->b; y++)
			{
				size_t at = (size_t)y * (size_t)p->pitch + (size_t)r->l * (size_t)p->bpp;
				memcpy(o->screen.shown + at, p->pixels + at, (size_t)bytes);
			}
		}
	s = *p;
	s.pixels = o->screen.shown;
	for(i = 0; i < SCREEN_OVERLAYS; i++)
	{ // under an overlay composited over the picture the copy is the picture again first
		Rect_t r;
		int y;
		if(!o->screen.overlay[i].pix || !o->screen.overlay[i].alpha || !OverlayRect(o, i, &r))
			continue;
		for(y = r.t; y <= r.b; y++)
		{
			size_t at = (size_t)y * (size_t)p->pitch + (size_t)r.l * (size_t)p->bpp;
			memcpy(o->screen.shown + at, p->pixels + at, (size_t)(r.r - r.l + 1) * (size_t)p->bpp);
		}
	}
	for(i = 0; i < SCREEN_OVERLAYS; i++)
	{
		const uint32_t* pix = o->screen.overlay[i].pix;
		int ox = o->screen.overlay[i].x, oy = o->screen.overlay[i].y, x, y;
		Rect_t r;
		if(!pix || !OverlayRect(o, i, &r))
			continue;
		for(y = r.t; y <= r.b; y++)
			for(x = r.l; x <= r.r; x++)
			{
				uint32_t v = pix[(size_t)(y - oy) * (size_t)o->screen.overlay[i].pitch + (size_t)(x - ox)], a = v >> 24, d, rr, g, b;
				if(!o->screen.overlay[i].alpha || a == 0xff)
					WriteRgb(&s, x, y, v & 0xffffff);
				else if(a)
				{
					d = ReadRgb(&s, x, y);
					rr = (((v >> 16) & 0xff) * a + ((d >> 16) & 0xff) * (255 - a)) / 255;
					g = (((v >> 8) & 0xff) * a + ((d >> 8) & 0xff) * (255 - a)) / 255;
					b = ((v & 0xff) * a + (d & 0xff) * (255 - a)) / 255;
					WriteRgb(&s, x, y, (rr << 16) | (g << 8) | b);
				}
			}
	}
	return o->screen.shown;
}

void Screen_TakeFrame(OpenBGI_t* o, OpenBGI_Frame_t* f)
{
	const Bmp_t* p = &o->screen.pic;
	int i;
	f->pixels = p->pixels;
	if(p->pixels && Overlaid(o))
		f->pixels = Compose(o);
	else if(o->screen.shown)
	{ // the overlays are gone: the picture itself again
		BGI_Free(o->screen.shown);
		o->screen.shown = NULL;
		o->screen.shownSize = 0;
	}
	o->screen.handed = f->pixels;
	f->width = p->w;
	f->height = p->h;
	f->pitch = p->pitch;
	f->bpp = p->pixels ? ModeBits(p->mode) : 0;
	f->changes = p->pixels ? o->screen.changes : 0;
	for(i = 0; i < f->changes; i++)
	{
		const Rect_t* r = &o->screen.changed[i];
		f->change[i].x = r->l;
		f->change[i].y = r->t;
		f->change[i].w = r->r - r->l + 1;
		f->change[i].h = r->b - r->t + 1;
	}
	o->screen.changes = 0;
}

// ---- pixels --------------------------------------------------------------------------

// pixel (x, y) of a 16- or 32-bit bitmap as 0x00RRGGBB; 5-bit channels are widened as the window back ends do
static uint32_t ReadRgb(const Bmp_t* b, int x, int y)
{
	const uint8_t* row = b->pixels + (ptrdiff_t)y * b->pitch;
	if(b->bpp == 2)
	{
		uint32_t v = ((const uint16_t*)row)[x];
		uint32_t r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, bl = v & 0x1f;
		return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (bl << 3 | bl >> 2);
	}
	return ((const uint32_t*)row)[x] & 0xffffff;
}

// 0x00RRGGBB into pixel (x, y) of the picture
static void WriteRgb(Bmp_t* p, int x, int y, uint32_t rgb)
{
	uint8_t* row = p->pixels + (ptrdiff_t)y * p->pitch;
	if(p->bpp == 2)
		((uint16_t*)row)[x] = (uint16_t)(((rgb >> 9) & 0x7c00) | ((rgb >> 6) & 0x03e0) | ((rgb >> 3) & 0x001f));
	else
		((uint32_t*)row)[x] = rgb;
}

// a source the picture can take: 16 or 32 bits per pixel, with pixels
static int Usable(const Bmp_t* b)
{
	return b && b->pixels && (b->bpp == 2 || b->bpp == 4) && b->w > 0 && b->h > 0;
}

/* BitBlt into p: w x h pixels of src from (sx, sy) to (dx, dy), clipped to
 * p, what lies outside src black; the rectangle written (inclusive) into
 * *out, 0 when nothing was */
static int CopyInto(Bmp_t* p, int dx, int dy, const Bmp_t* src, int sx, int sy, int w, int h, Rect_t* out)
{
	int x, y;
	if(!p->pixels || !Usable(src))
		return 0;
	// clipped to the picture, then to the source (what lies outside the source is black)
	if(dx < 0)
	{
		sx -= dx;
		w += dx;
		dx = 0;
	}
	if(dy < 0)
	{
		sy -= dy;
		h += dy;
		dy = 0;
	}
	if(dx + w > p->w)
		w = p->w - dx;
	if(dy + h > p->h)
		h = p->h - dy;
	if(w <= 0 || h <= 0)
		return 0;
	for(y = 0; y < h; y++)
	{
		int ry = sy + y;
		uint8_t* d = p->pixels + (ptrdiff_t)(dy + y) * p->pitch + (ptrdiff_t)dx * p->bpp;
		if(ry < 0 || ry >= src->h)
		{
			memset(d, 0, (size_t)w * (size_t)p->bpp);
			continue;
		}
		if(src->bpp == p->bpp && sx >= 0 && sx + w <= src->w)
		{
			memcpy(d, src->pixels + (ptrdiff_t)ry * src->pitch + (ptrdiff_t)sx * src->bpp, (size_t)w * (size_t)p->bpp);
			continue;
		}
		for(x = 0; x < w; x++)
		{
			int rx = sx + x;
			WriteRgb(p, dx + x, dy + y, rx >= 0 && rx < src->w ? ReadRgb(src, rx, ry) : 0);
		}
	}
	out->l = dx;
	out->t = dy;
	out->r = dx + w - 1;
	out->b = dy + h - 1;
	return 1;
}

/* StretchDIBits into p: all of src to dw x dh at (dx, dy), nearest
 * neighbour, or bilinear and clipped to `clip` ({l, t, r, b}, exclusive);
 * the rectangle written into *out, 0 when nothing was */
static int StretchInto(Bmp_t* p, int dx, int dy, int dw, int dh, const Bmp_t* src, const int* clip, Rect_t* out)
{
	int cl = 0, ct = 0, cr, cb, x, y;
	double fx, fy; // source pixels per picture pixel
	if(!p->pixels || !Usable(src) || dw <= 0 || dh <= 0)
		return 0;
	if(dw == src->w && dh == src->h && !clip)
		return CopyInto(p, dx, dy, src, 0, 0, dw, dh, out);
	// the output: the picture, cut by `clip` and the destination ({l, t, r, b}, exclusive)
	cr = p->w;
	cb = p->h;
	if(clip)
	{
		if(clip[0] > cl)
			cl = clip[0];
		if(clip[1] > ct)
			ct = clip[1];
		if(clip[2] < cr)
			cr = clip[2];
		if(clip[3] < cb)
			cb = clip[3];
	}
	if(cl < dx)
		cl = dx;
	if(ct < dy)
		ct = dy;
	if(cr > dx + dw)
		cr = dx + dw;
	if(cb > dy + dh)
		cb = dy + dh;
	if(cr <= cl || cb <= ct)
		return 0;
	fx = (double)src->w / dw;
	fy = (double)src->h / dh;
	if(fx == 1.0 && fy == 1.0)
		return CopyInto(p, cl, ct, src, cl - dx, ct - dy, cr - cl, cb - ct, out);
	for(y = ct; y < cb; y++)
	{
		if(!clip)
		{ // nearest neighbour
			int sy = (int)((y - dy) * fy);
			if(sy >= src->h)
				sy = src->h - 1;
			for(x = cl; x < cr; x++)
			{
				int sx = (int)((x - dx) * fx);
				if(sx >= src->w)
					sx = src->w - 1;
				WriteRgb(p, x, y, ReadRgb(src, sx, sy));
			}
		}
		else
		{ // bilinear (HALFTONE): each pixel from the four source pixels around its centre, weights in 1/256
			double syf = ((y - dy) + 0.5) * fy - 0.5;
			int sy0 = (int)syf, sy1, wy = (int)((syf - sy0) * 256);
			if(syf < 0)
				sy0 = 0, wy = 0;
			if(sy0 >= src->h)
				sy0 = src->h - 1;
			sy1 = sy0 + 1 < src->h ? sy0 + 1 : sy0;
			for(x = cl; x < cr; x++)
			{
				double sxf = ((x - dx) + 0.5) * fx - 0.5;
				int sx0 = (int)sxf, sx1, wx = (int)((sxf - sx0) * 256);
				uint32_t p00, p01, p10, p11, r, g, b;
				if(sxf < 0)
					sx0 = 0, wx = 0;
				if(sx0 >= src->w)
					sx0 = src->w - 1;
				sx1 = sx0 + 1 < src->w ? sx0 + 1 : sx0;
				p00 = ReadRgb(src, sx0, sy0);
				p01 = ReadRgb(src, sx1, sy0);
				p10 = ReadRgb(src, sx0, sy1);
				p11 = ReadRgb(src, sx1, sy1);
#define CH(v, sh)     (((v) >> (sh)) & 0xff)
#define LERP(a, b, t) ((a) + ((((b) - (a)) * (t)) >> 8))
				r = LERP(LERP(CH(p00, 16), CH(p01, 16), wx), LERP(CH(p10, 16), CH(p11, 16), wx), wy);
				g = LERP(LERP(CH(p00, 8), CH(p01, 8), wx), LERP(CH(p10, 8), CH(p11, 8), wx), wy);
				b = LERP(LERP(CH(p00, 0), CH(p01, 0), wx), LERP(CH(p10, 0), CH(p11, 0), wx), wy);
#undef CH
#undef LERP
				WriteRgb(p, x, y, (r << 16) | (g << 8) | b);
			}
		}
	}
	out->l = cl;
	out->t = ct;
	out->r = cr - 1;
	out->b = cb - 1;
	return 1;
}

void Screen_Copy(OpenBGI_t* o, int dx, int dy, const Bmp_t* src, int sx, int sy, int w, int h)
{
	Rect_t r;
	if(CopyInto(&o->screen.pic, dx, dy, src, sx, sy, w, h, &r))
		Changed(o, r.l, r.t, r.r, r.b);
}

void Screen_Stretch(OpenBGI_t* o, int dx, int dy, int dw, int dh, const Bmp_t* src, const int* clip)
{
	Rect_t r;
	if(StretchInto(&o->screen.pic, dx, dy, dw, dh, src, clip, &r))
		Changed(o, r.l, r.t, r.r, r.b);
}

void Screen_StretchTo(Bmp_t* dst, int dx, int dy, int dw, int dh, const Bmp_t* src, const int* clip)
{
	Rect_t r;
	StretchInto(dst, dx, dy, dw, dh, src, clip, &r);
}

void Screen_ToRgb32(uint32_t* dst, int pitch, const Bmp_t* src)
{
	int x, y;
	if(!Usable(src))
		return;
	for(y = 0; y < src->h; y++)
		for(x = 0; x < src->w; x++)
			dst[(size_t)y * (size_t)pitch + (size_t)x] = ReadRgb(src, x, y);
}

void Screen_Overlay(OpenBGI_t* o, int slot, const uint32_t* pix, int pitch, int x, int y, int w, int h, int alpha)
{
	if(slot < 0 || slot >= SCREEN_OVERLAYS)
		return;
	if(o->screen.overlay[slot].pix) // where it was is the picture's again (or the new overlay's)
		Changed(o, o->screen.overlay[slot].x, o->screen.overlay[slot].y, o->screen.overlay[slot].x + o->screen.overlay[slot].w - 1,
			o->screen.overlay[slot].y + o->screen.overlay[slot].h - 1);
	o->screen.overlay[slot].pix = w > 0 && h > 0 ? pix : NULL;
	o->screen.overlay[slot].pitch = pitch;
	o->screen.overlay[slot].x = x;
	o->screen.overlay[slot].y = y;
	o->screen.overlay[slot].w = w;
	o->screen.overlay[slot].h = h;
	o->screen.overlay[slot].alpha = alpha;
	if(o->screen.overlay[slot].pix)
		Changed(o, x, y, x + w - 1, y + h - 1);
}
