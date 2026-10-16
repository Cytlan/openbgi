/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * flipbook.c - background type 5: a flip book of up to 32 screen-sized
 *              bitmaps (inc/bgi/gfx/background.h; selected by "90 44")
 *
 * The object's level is the frame index.  When the frames are set, every
 * ordered pair of them is compared on a grid of 32 x 24 cells and the
 * rectangles of the cells that differ are kept; switching the level then
 * dirties only those cells, so an animated background (a blinking light,
 * lip-sync on a full-screen picture) costs only what actually changes.
 * The compositor's end-of-frame notification records which frame is on
 * screen; showing, hiding or changing the mode forgets it, which forces a
 * full repaint next time.
 */
#include "background_internal.h"

#define BG5_RECT_BYTES 0x1800 // room for 384 rectangles per frame pair (32 x 24 cells, worst case)

static void Bg5_Dtor(Bg5_t* b)
{
	b->bg.obj.vt = &Bg5_Vtbl.base;
	Bg5_Clear(b);
	Background_Dtor(&b->bg);
}

static void Bg5_Destroy(DispObj_t* o, int flags)
{
	Bg5_Dtor((Bg5_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

/* every frame empty.  `shown` and `count` are left as they are (the
 * original does not initialise them; zeroed memory gives count 0, which
 * draws nothing until Bg5_Set()). */
void Bg5_Ctor(Bg5_t* b)
{
	int i;
	Background_Ctor(&b->bg, 5);
	b->bg.obj.vt = &Bg5_Vtbl.base;
	for(i = 0; i < 32; i++)
	{
		b->frame[i].bmp = -1;
		b->frame[i].gen = 0;
		memset(b->frame[i].diff, 0, sizeof b->frame[i].diff);
	}
}

// drop every frame and its difference lists; no frame is on screen
void Bg5_Clear(Bg5_t* b)
{
	int i, j;
	for(i = 0; i < 32; i++)
	{
		b->frame[i].bmp = -1;
		b->frame[i].gen = 0;
		for(j = 0; j < 32; j++)
		{
			if(b->frame[i].diff[j].rects)
			{
				BGI_Free(b->frame[i].diff[j].rects);
				b->frame[i].diff[j].count = 0;
				b->frame[i].diff[j].rects = NULL;
			}
		}
	}
	b->count = 0;
	b->shown = -1;
}

// the screen is compared in 32 x 24 cells; their size in pixels
void Bg5_CellSize(int* cw, int* ch)
{
	Bmp_t back;
	Gfx_GetBackBmp(&back);
	*cw = (int)((uint32_t)back.w >> 5);
	*ch = (int)((uint32_t)back.h / 24u);
}

/* fill `out` with the rectangles (merged along each cell row) of the cells
 * where bitmaps A and B differ; `out->rects` must hold 384 rectangles.
 * Only 16-bit (mode 0) and 32-bit (mode 1, colour bits only) screens are
 * compared; in other modes no cell ever differs.  0 ok, 0x80000003 /
 * 0x80000005 a bitmap missing, 0x80000004 / 0x80000006 not screen sized. */
int Bg5_DiffFrames(Bg5_t* b, Bg5Diff_t* out, int bmpA, int bmpB)
{
	Bmp_t back, a, s;
	int cw, ch, row, col;
	(void)b;
	Gfx_GetBackBmp(&back);
	if(!BgGetInfo(&a, bmpA))
		return (int)0x80000003;
	if(!Bmp_MatchesScreen(&a))
		return (int)0x80000004;
	if(!BgGetInfo(&s, bmpB))
		return (int)0x80000005;
	if(!Bmp_MatchesScreen(&s))
		return (int)0x80000006;
	Bg5_CellSize(&cw, &ch);
	out->count = 0;
	for(row = 0; row < 24; row++)
	{
		int y = row * ch, x = 0, startNew = 1;
		for(col = 0; col < 32; col++, x += cw)
		{
			const uint8_t* pa = a.pixels + (ptrdiff_t)a.pitch * (row * ch) + (ptrdiff_t)a.bpp * (col * cw);
			const uint8_t* pb = s.pixels + (ptrdiff_t)s.pitch * (row * ch) + (ptrdiff_t)s.bpp * (col * cw);
			int differs = 0, cy, cx;
			for(cy = 0; cy < ch && !differs; cy++)
			{
				const uint8_t *ra = pa, *rb = pb;
				for(cx = 0; cx < cw && !differs; cx++)
				{
					if(back.mode == 0)
						differs = *(const uint16_t*)ra != *(const uint16_t*)rb;
					else if(back.mode == 1)
						differs = ((*(const uint32_t*)ra ^ *(const uint32_t*)rb) & 0xffffff) != 0;
					ra += a.bpp;
					rb += s.bpp;
				}
				pa += a.pitch;
				pb += s.pitch;
			}
			if(!differs)
			{
				startNew = 1;
			}
			else if(startNew)
			{
				Rect_t* r = &out->rects[out->count++];
				r->l = x;
				r->t = y;
				r->r = x + cw - 1;
				r->b = y + ch - 1;
				startNew = 0;
			}
			else
			{
				out->rects[out->count - 1].r += cw; // extend the run
			}
		}
	}
	return 0;
}

/* the frames: n (2..32) screen-sized bitmaps; every ordered pair gets its
 * difference list.  0 ok, 0x80000001 n out of range, 0x80000002 a bitmap
 * unusable; the previous frames are dropped in every case. */
int Bg5_Set(Bg5_t* b, int n, const int* bmpList)
{
	int i, j;
	Bg5_Clear(b);
	if(n < 2 || n > 32)
		return (int)0x80000001;
	for(i = 0; i < n; i++)
	{
		if(!Bg_BitmapUsable(bmpList[i]))
			break;
		b->frame[i].bmp = bmpList[i];
		b->frame[i].gen = BgGen(bmpList[i]);
	}
	if(i != n)
	{
		Bg5_Clear(b);
		return (int)0x80000002;
	}
	b->count = n;
	for(i = 0; i < n; i++)
	{
		for(j = 0; j < n; j++)
		{
			Bg5Diff_t* d = &b->frame[i].diff[j];
			d->rects = (Rect_t*)BGI_Alloc(BG5_RECT_BYTES);
			Bg5_DiffFrames(b, d, b->frame[j].bmp, b->frame[i].bmp);
		}
	}
	return 0;
}

/* the invalidate slot: only the cells that differ between the frame on
 * screen and the frame selected by the level are dirtied; without a frame
 * on screen the whole background is.  A level of 32 or more dirties
 * nothing (render() will not draw it either). */
static void Bg5_Invalidate(DispObj_t* o)
{
	Bg5_t* b = (Bg5_t*)o;
	int level = GET_LEVEL(&b->bg), i;
	const Bg5Diff_t* d;
	if((uint32_t)level >= 32)
		return;
	if((uint32_t)b->shown >= (uint32_t)b->count)
	{
		Background_Invalidate(o);
		return;
	}
	d = &b->frame[level].diff[b->shown];
	for(i = 0; i < d->count; i++)
		Gfx_AddDirty(gDispGfx, o->vt->sortKey(o), &d->rects[i]);
}

// showing or hiding forgets the frame on screen
static void Bg5_SetVisible(DispObj_t* o, int on)
{
	((Bg5_t*)o)->shown = -1;
	DispObj_SetVisible(o, on);
}

// so does a mode change
static void Bg5_SetMode(Background_t* bg, int mode)
{
	((Bg5_t*)bg)->shown = -1;
	Background_SetMode(bg, mode);
}

// a screen size change drops the frames (their cell grid no longer fits)
static int Bg5_MatchScreenSize(Background_t* bg)
{
	Bg5_Clear((Bg5_t*)bg);
	return Background_MatchScreenSize(bg);
}

// the compositor's end-of-frame notification (0xF0000000): the frame selected by the level is on screen now
static void Bg5_Notify(DispObj_t* o, uint32_t what, int a1, int a2)
{
	(void)a1;
	(void)a2;
	if(what == 0xf0000000u)
		((Bg5_t*)o)->shown = GET_LEVEL(&((Bg5_t*)o)->bg);
}

// copy the frame selected by the level; 0 for a level beyond the frames or a replaced bitmap
static int Bg5_Render(Background_t* bg, Bmp_t* dst, const Rect_t* local)
{
	Bg5_t* b = (Bg5_t*)bg;
	int level = GET_LEVEL(bg);
	Bmp_t src;
	if((uint32_t)level >= (uint32_t)b->count)
		return 0;
	if(!BgCurrent(&src, b->frame[level].bmp, b->frame[level].gen))
		return 0;
	Bmp_Crop(&src, local);
	Bmp_BlitEffect(dst, &src, 0x80, 0); // raw copy
	return 1;
}

BG_VTABLE(Bg5_Vtbl, Bg5_Destroy, Bg5_SetVisible, Bg5_Invalidate, Background_NopSetPos,
	DispObj_GetPos, DispObj_SetOffset, DispObj_SetLevel, DispObj_GetLevel,
	DispObj_SetParam, Bg5_Notify, Bg5_SetMode, Bg5_MatchScreenSize,
	Bg5_Render);
