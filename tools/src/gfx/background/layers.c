/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * layers.c - background type 12: up to eight transformed layers
 *            (inc/bgi/gfx/background.h; selected by "91 40", the layers
 *            addressed by "91 41" .. "91 4A")
 *
 * Every layer is a bitmap of any size placed with its centre point
 * (cx, cy) at a screen position (x, y), rotated and scaled about that
 * point.  Layer 0 is the base picture and replaces the destination; the
 * others are blended over it with their own effect and level.  The deltas
 * (centre, angle, scale) are applied over the object's progress ("90 35";
 * the full delta at progress 0x100), the angle and scale through an Ease
 * curve each, which animates a layer without a script loop.  One layer is
 * "current": the generic setPos /
 * getPos / setLevel / getLevel / setParam of handle 0 address it, so the
 * tweens of the object instructions work on that layer.
 */
#include "background_internal.h"

static void Bg12_Destroy(DispObj_t* o, int flags)
{
	Background_Dtor((Background_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

// every layer empty and hidden, layer 0 current
void Bg12_Ctor(Bg12_t* b)
{
	Background_Ctor(&b->bg, 12);
	b->bg.obj.vt = &Bg12_Vtbl.base;
	b->current = 0;
	memset(b->layer, 0, sizeof b->layer);
}

// layer `i` of `b`, or NULL for an index out of range
static Bg12Layer_t* Bg12_LayerAt(Bg12_t* b, int i)
{
	return (uint32_t)i < 8 ? &b->layer[i] : NULL;
}

// the prologue of every layer operation: L = the layer, or return 0x80000001
#define BG12_LAYER(b, i)            \
	do                              \
	{                               \
		L = Bg12_LayerAt((b), (i)); \
		if(!L)                      \
			return (int)0x80000001; \
	} while(0)

int Bg12_LayerShow(Bg12_t* b, int i, int on)
{
	Bg12Layer_t* L;
	BG12_LAYER(b, i);
	L->visible = on;
	return 0;
}

int Bg12_LayerIsShown(Bg12_t* b, int* out, int i)
{
	Bg12Layer_t* L;
	BG12_LAYER(b, i);
	*out = L->visible;
	return 0;
}

// the screen position of the layer's centre point (16.16)
int Bg12_LayerSetPos(Bg12_t* b, int i, int32_t x, int32_t y)
{
	Bg12Layer_t* L;
	BG12_LAYER(b, i);
	L->x = x;
	L->y = y;
	return 0;
}

int Bg12_LayerGetPos(Bg12_t* b, int32_t out[2], int i)
{
	Bg12Layer_t* L;
	BG12_LAYER(b, i);
	out[0] = L->x;
	out[1] = L->y;
	return 0;
}

int Bg12_LayerSetEffect(Bg12_t* b, int i, int effect)
{
	Bg12Layer_t* L;
	BG12_LAYER(b, i);
	L->effect = effect;
	return 0;
}

int Bg12_LayerSetLevel(Bg12_t* b, int i, int level)
{
	Bg12Layer_t* L;
	BG12_LAYER(b, i);
	L->level = level;
	return 0;
}

int Bg12_LayerGetLevel(Bg12_t* b, int* out, int i)
{
	Bg12Layer_t* L;
	BG12_LAYER(b, i);
	*out = L->level;
	return 0;
}

/* the layer's bitmap (any size) and its centre point (cx, cy) in 16.16;
 * bmp -1 removes the bitmap.  The centre deltas are reset.  0 ok,
 * 0x80000002 the bitmap is missing. */
int Bg12_LayerSetBitmap(Bg12_t* b, int i, int bmp, int32_t cx, int32_t cy)
{
	Bg12Layer_t* L;
	Bmp_t info;
	BG12_LAYER(b, i);
	if(bmp == -1)
	{
		L->hasBitmap = 0;
		return 0;
	}
	if(!BgGetInfo(&info, bmp))
		return (int)0x80000002;
	L->hasBitmap = 1;
	L->bmp = bmp;
	L->gen = BgGen(bmp);
	L->cx = cx;
	L->cy = cy;
	L->dcx = 0;
	L->dcy = 0;
	return 0;
}

/* angle (16.16 degrees), scale (16.16, both non-zero) and bilinear
 * sampling; the deltas of angle and scale are reset.  0 ok, 0x80000003
 * a zero scale. */
int Bg12_LayerSetTransform(Bg12_t* b, int i, int32_t angle, int32_t sx, int32_t sy, int smooth)
{
	Bg12Layer_t* L;
	BG12_LAYER(b, i);
	if((uint32_t)sx == 0 || (uint32_t)sy == 0)
		return (int)0x80000003;
	L->sy = sy;
	L->angle = angle;
	L->sx = sx;
	L->dAngle = 0;
	L->dsx = 0;
	L->dsy = 0;
	L->smooth = smooth;
	return 0;
}

// the Ease curves applied to the progress for the angle and the scale deltas
int Bg12_LayerSetCurves(Bg12_t* b, int i, int angleCurve, int scaleCurve)
{
	Bg12Layer_t* L;
	BG12_LAYER(b, i);
	L->angleCurve = angleCurve;
	L->scaleCurve = scaleCurve;
	return 0;
}

// how far the centre point moves (16.16) over the full progress
int Bg12_LayerSetCentreDelta(Bg12_t* b, int i, int32_t dcx, int32_t dcy)
{
	Bg12Layer_t* L;
	BG12_LAYER(b, i);
	L->dcx = dcx;
	L->dcy = dcy;
	return 0;
}

// the angle change (16.16 degrees) over the full progress
int Bg12_LayerSetAngleDelta(Bg12_t* b, int i, int32_t d)
{
	Bg12Layer_t* L;
	BG12_LAYER(b, i);
	L->dAngle = d;
	return 0;
}

// the scale change (16.16) over the full progress
int Bg12_LayerSetScaleDelta(Bg12_t* b, int i, int32_t dsx, int32_t dsy)
{
	Bg12Layer_t* L;
	BG12_LAYER(b, i);
	L->dsx = dsx;
	L->dsy = dsy;
	return 0;
}

// make layer i the one the generic object operations address; 0 ok, 0x80000001 out of range
int Bg12_Select(Bg12_t* b, int i)
{
	if((uint32_t)i >= 8)
		return (int)0x80000001;
	b->current = i;
	return 0;
}

// the generic object operations address the current layer
static void Bg12_SetPos(DispObj_t* o, int x, int y)
{
	Bg12_t* b = (Bg12_t*)o;
	Bg12_LayerSetPos(b, b->current, x, y);
}

static void Bg12_GetPos(DispObj_t* o, int32_t out[2])
{
	Bg12_t* b = (Bg12_t*)o;
	Bg12_LayerGetPos(b, out, b->current);
}

static void Bg12_SetLevel(DispObj_t* o, int level)
{
	Bg12_t* b = (Bg12_t*)o;
	Bg12_LayerSetLevel(b, b->current, level);
}

static int Bg12_GetLevel(DispObj_t* o)
{
	Bg12_t* b = (Bg12_t*)o;
	int level = 0;
	Bg12_LayerGetLevel(b, &level, b->current);
	return level;
}

// the setParam slot: 0x80 centre delta, 0x81 angle delta, 0x82 scale delta, 0x8F curves (always 0)
static int Bg12_SetParam(DispObj_t* o, int no, int a, int c)
{
	Bg12_t* b = (Bg12_t*)o;
	switch(no)
	{
		case 0x80: Bg12_LayerSetCentreDelta(b, b->current, a, c); return 0;
		case 0x81: Bg12_LayerSetAngleDelta(b, b->current, a); return 0;
		case 0x82: Bg12_LayerSetScaleDelta(b, b->current, a, c); return 0;
		case 0x8f: Bg12_LayerSetCurves(b, b->current, a, c); return 0;
		default: return DispObj_SetParam(o, no, a, c);
	}
}

/* the scale after the eased delta e (16.16 of the full delta), interpolated
 * in the reciprocal: 65536 / (65536/s + (65536/(s+ds) - 65536/s) * e / 65536) */
static int32_t Bg12_ScaleAt(int32_t s, int32_t ds, int32_t e)
{
	double inv = 65536.0 / (double)s;
	double v = 65536.0 / (double)(int32_t)((uint32_t)s + (uint32_t)ds);
	v = v - inv;
	v = v * (double)e;
	v = v * 1.52587890625e-05; // 1 / 65536
	v = v + inv;
	return BGI_Ftol(65536.0 / v);
}

/* every visible layer with a current bitmap is drawn with its transform at
 * the object's progress.  Layer 0 is copied (it replaces the destination,
 * with a plain blit when nothing is transformed); the other layers are
 * blended, through a temporary bitmap when their effect is not a plain
 * alpha blend.  The destination is cleared when layer 0 did not draw.
 * Always returns 1. */
static int Bg12_Render(Background_t* bg, Bmp_t* dst, const Rect_t* local)
{
	Bg12_t* b = (Bg12_t*)bg;
	Bmp_t back, src;
	int i, drawn = 0;
	Gfx_GetBackBmp(&back);
	for(i = 0; i < 8; i++)
	{
		Bg12Layer_t L = b->layer[i];
		int32_t t, x, y, cx, cy, angle, sx, sy;
		if(!L.hasBitmap || !L.visible)
			goto next;
		if(!BgCurrent(&src, L.bmp, L.gen))
			goto next;
		t = DispObj_GetProgress(&b->bg.obj, 1);
		x = (int32_t)((uint32_t)L.x - ((uint32_t)local->l << 16)); // relative to the dirty part
		y = (int32_t)((uint32_t)L.y - ((uint32_t)local->t << 16));
		cx = L.cx + MulShr(L.dcx, (uint32_t)t, 24); // the progress is 8.16: the full delta at 0x100
		cy = L.cy + MulShr(L.dcy, (uint32_t)t, 24);
		angle = L.angle + (int32_t)(((int64_t)Ease(t, L.angleCurve) * (int64_t)L.dAngle) >> 16);
		{
			int32_t e = Ease(t, L.scaleCurve);
			sx = Bg12_ScaleAt(L.sx, L.dsx, e);
			sy = Bg12_ScaleAt(L.sy, L.dsy, e);
		}
		if(i == 0)
		{
			// a plain copy when the bitmap covers the screen untransformed
			int32_t dx = (int32_t)((uint32_t)L.x - (uint32_t)cx);
			int32_t dy = (int32_t)((uint32_t)L.y - (uint32_t)cy);
			if((uint32_t)src.w >= (uint32_t)back.w && (uint32_t)src.h >= (uint32_t)back.h &&
				(dx & 0xffff) == 0 && (dy & 0xffff) == 0 && dx <= 0 && dy <= 0 &&
				(uint32_t)(src.w + (dx >> 16)) >= (uint32_t)back.w &&
				(uint32_t)(src.h + (dy >> 16)) >= (uint32_t)back.h &&
				angle == 0 && sx == 0x10000 && sy == 0x10000)
			{
				Bmp_Blit(dst, (dx >> 16) - local->l, (dy >> 16) - local->t, &src, 5, L.level); // effect 5: dim toward black by the level
			}
			else
			{
				Blit_XformCopy(dst, x, y, &src, cx, cy, angle, sx, sy, L.level, L.smooth);
			}
		}
		else if(L.effect == 0 || L.effect == 1 || L.effect == 0x20)
		{
			Blit_Xform(dst, x, y, &src, cx, cy, angle, sx, sy, L.level, L.smooth);
		}
		else
		{
			Bmp_t tmp;
			Bmp_Alloc(&tmp, local->r - local->l + 1, local->b - local->t + 1, src.mode);
			Blit_XformCopy(&tmp, x, y, &src, cx, cy, angle, sx, sy, 0, L.smooth);
			Bmp_BlitEffect(dst, &tmp, L.effect, L.level);
			Bmp_Free(&tmp);
		}
		drawn = 1;
	next:
		if(i == 0 && !drawn)
		{
			Bmp_Clear(dst, NULL);
			drawn = 1;
		}
	}
	return drawn;
}

BG_VTABLE(Bg12_Vtbl, Bg12_Destroy, DispObj_SetVisible, Background_Invalidate, Bg12_SetPos,
	Bg12_GetPos, DispObj_SetOffset, Bg12_SetLevel, Bg12_GetLevel,
	Bg12_SetParam, DispObj_NopNotify, Background_SetMode, Background_MatchScreenSize,
	Bg12_Render);
