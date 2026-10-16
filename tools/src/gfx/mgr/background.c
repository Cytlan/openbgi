/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * background.c - the graphics manager's background operations: replacing the
 *                background object by type, its bitmaps, the layers
 *                (inc/bgi/gfx/gfxmgr.h)
 *
 * Each of the "90 40" .. "90 4A" and "91 40" instructions selects one of
 * the background types of inc/bgi/gfx/background.h: the Gfx_BgSet*
 * wrapper swaps the manager's background object for one of that type
 * (Gfx_SetBgType) and hands the arguments to the new object's setters,
 * mapping their 0x8000000n results onto the small codes the instruction
 * reports.  Where an unexpected result falls through, the original
 * returns one of the wrapper's own arguments; those paths are kept and
 * marked.  The Gfx_Bgl* wrappers ("91 41" .. "91 4A") address one layer
 * of a type 12 background.
 */
#include <string.h>

#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/objects.h"
#include "bgi/gfx/text.h"
#include "bgi/gfx.h"
#include "bgi/engine.h"
#include "bgi/file.h"
#include "bgi/sys.h"

/* replace the background object when the requested type differs (unknown
 * types give type 1); the new object takes the visibility and mode of
 * the last Gfx_BgShow.  Then tell the compositor the type and repaint
 * everything. */
void Gfx_SetBgType(Gfx_t* g, int type)
{
	Background_t* bg = g->background;

	if(type != bg->typeId)
	{
		Compositor_Remove(g->comp, &bg->obj);
		bg->obj.vt->destroy(&bg->obj, 1);
		g->background = NULL;
	}
	if(g->background == NULL)
	{
		bg = Background_Create((uint32_t)(type - 1) < 12u ? type : 1);
		g->background = bg;
		Compositor_Add(g->comp, &bg->obj);
		bg->obj.vt->setVisible(&bg->obj, g->bgVisible);
		((const BackgroundVtbl_t*)bg->obj.vt)->setMode(bg, g->bgMode);
	}
	Compositor_SetBgType(g->comp, g->background->typeId);
	Compositor_InvalidateAll(g->comp);
}

// "90 4D": the type (1..12) of the current background
int Gfx_BgTypeId(Gfx_t* g)
{
	return g->background->typeId;
}

/* "90 4C": show / hide the background and set its mode (0 paints black,
 * anything else renders the type); both are remembered for the next
 * background object.  The whole screen is repainted. */
void Gfx_BgShow(Gfx_t* g, int visible, int mode)
{
	Background_t* bg = g->background;
	g->bgVisible = visible;
	g->bgMode = mode;
	bg->obj.vt->setVisible(&bg->obj, visible);
	((const BackgroundVtbl_t*)bg->obj.vt)->setMode(bg, mode);
	Compositor_InvalidateAll(g->comp);
}

#define BG(g)    ((g)->background)
#define BGOBJ(g) (&(g)->background->obj)

// "90 40": type 1, one screen-sized bitmap; 1 ok, 0 the bitmap is unusable
int Gfx_BgSetBitmap(Gfx_t* g, int bmp)
{
	Gfx_SetBgType(g, 1);
	return Bg1_Set((Bg1_t*)BG(g), bmp);
}

/* "90 41": type 2, `bmp` cross-faded by `level` with `second` (a bitmap
 * or BG_BLACK / BG_WHITE); 1 ok, 0 a bitmap is unusable */
int Gfx_BgSetFade(Gfx_t* g, int bmp, int second, int level)
{
	Gfx_SetBgType(g, 2);
	BGOBJ(g)->vt->setLevel(BGOBJ(g), level);
	return Bg2_Set((Bg2_t*)BG(g), bmp, second);
}

/* "90 42": type 3, the 2 x 2 tiled view of four bitmaps with its origin
 * at (x, y); 0 ok, 1 the origin is out of range, 2 a bitmap is unusable */
int Gfx_BgSetScroll(Gfx_t* g, int b0, int b1, int b2, int b3, int x, int y)
{
	Gfx_SetBgType(g, 3);
	if(!Bg3_SetOrigin((Bg3_t*)BG(g), x, y))
		return 1;
	return Bg3_Set((Bg3_t*)BG(g), b0, b1, b2, b3) ? 0 : 2;
}

/* "90 43": type 4, bmp1 at (x1, y1) over bmp2 (a bitmap or a sentinel)
 * at (x2, y2), blended by `level` or wiped through the gray map (`gray`
 * -1 = none, `param` the threshold parameter).  0 ok, 1 / 2 bmp1 / bmp2
 * missing, 3 the gray map missing, 4 not grayscale, 5 not screen sized,
 * -1 any other result. */
int Gfx_BgSetBlend(Gfx_t* g, int x1, int y1, int bmp1, int x2, int y2, int bmp2, int gray, int param, int level)
{
	uint32_t r;

	Gfx_SetBgType(g, 4);
	r = (uint32_t)Bg4_Set((Bg4_t*)BG(g), x1, y1, bmp1, x2, y2, bmp2);
	if(r == 0x80000001u)
		return 1;
	if(r == 0x80000002u)
		return 2;
	if(r != 0)
		return -1;
	r = (uint32_t)Bg4_SetWipe((Bg4_t*)BG(g), gray, param);
	switch(r)
	{
		case 0:
			BGOBJ(g)->vt->setLevel(BGOBJ(g), level);
			return 0;
		case 0x80000003u: return 3;
		case 0x80000004u: return 4;
		case 0x80000005u: return 5;
		default: return -1;
	}
}

/* "90 44": type 5, the flip book of the n bitmaps in `list` with frame
 * `level` shown; 0 ok, 1 n is not 2..32, 2 a bitmap is unusable */
int Gfx_BgSetFrames(Gfx_t* g, int n, const int* list, int level)
{
	uint32_t r;

	Gfx_SetBgType(g, 5);
	BGOBJ(g)->vt->setLevel(BGOBJ(g), level);
	r = (uint32_t)Bg5_Set((Bg5_t*)BG(g), n, list);
	switch(r)
	{
		case 0: return 0;
		case 0x80000001u: return 1;
		case 0x80000002u: return 2;
		default: return level; // (original: returns the third argument)
	}
}

/* "90 45": type 6, `bmp` displaced through the vector maps vec1 and vec2
 * (-1 = none) at `level`, `amount` selecting bilinear sampling.  0 ok,
 * 1 / 2 the bitmap missing / not screen sized, 3 / 4 the same for vec1,
 * 5 / 6 for vec2. */
int Gfx_BgSetDisplace(Gfx_t* g, int bmp, int vec1, int vec2, int level, int amount)
{
	uint32_t r;

	Gfx_SetBgType(g, 6);
	r = (uint32_t)Bg6_Set((Bg6_t*)BG(g), bmp, vec1, vec2);
	if(r == 0)
	{
		BGOBJ(g)->vt->setLevel(BGOBJ(g), level);
		Bg6_SetAmount((Bg6_t*)BG(g), amount);
		return 0;
	}
	if(r >= 0x80000001u && r <= 0x80000006u)
		return (int)(r - 0x80000000u);
	return vec2; // (original: returns the third argument)
}

/* "90 46": type 7, `bmp` through the box blur of edge type `type` at
 * `level`; 0 ok, 1 the bitmap is missing, 2 not screen sized, 3 the type
 * is not 0 or 1 */
int Gfx_BgSetGradient(Gfx_t* g, int bmp, int type, int level)
{
	uint32_t r;

	Gfx_SetBgType(g, 7);
	r = (uint32_t)Bg7_SetBitmap((Bg7_t*)BG(g), bmp);
	if(r == 0x80000001u)
		return 1;
	if(r == 0x80000002u)
		return 2;
	if(r != 0)
		return bmp; // (original: returns the first argument)
	r = (uint32_t)Bg7_SetType((Bg7_t*)BG(g), type);
	if(r == 0x80000003u)
		return 3;
	if(r != 0)
		return bmp;
	BGOBJ(g)->vt->setLevel(BGOBJ(g), level);
	return 0;
}

/* "90 47": type 8, `bmp` under the ripples of definition `rippleNo` with
 * `rings` rings, read through the vector + distance map `map`, at
 * amplitude `level`.  0 ok, 1 the bitmap is missing, 2 not screen sized,
 * 3 the map is missing, 4 not a screen-sized vecdist map, 5 zero rings,
 * 6 unknown ripple definition, 7 the definition has fewer rings than
 * asked for. */
int Gfx_BgSetRipple(Gfx_t* g, int bmp, int map, int rings, int rippleNo, int level)
{
	uint32_t r;

	Gfx_SetBgType(g, 8);
	r = (uint32_t)Bg8_SetBitmap((Bg8_t*)BG(g), bmp);
	if(r == 0x80000001u)
		return 1;
	if(r == 0x80000002u)
		return 2;
	if(r != 0)
		return bmp;
	r = (uint32_t)Bg8_SetRipple((Bg8_t*)BG(g), map, rings, rippleNo, level);
	if(r == 0)
		return 0;
	if(r >= 0x80000003u && r <= 0x80000007u)
		return (int)(r - 0x80000000u);
	return bmp;
}

/* "90 48": type 9, the w x h view of `bmp` (any size) at (x, y) stretched
 * over the screen; the position is handed to the object in 16.16.  0 ok,
 * 1 the bitmap is missing, 2 w or h below 2. */
int Gfx_BgSetView(Gfx_t* g, int bmp, int x, int y, int w, int h)
{
	uint32_t r;

	Gfx_SetBgType(g, 9);
	r = (uint32_t)Bg9_SetBitmap((Bg9_t*)BG(g), bmp);
	if(r == 0x80000001u)
		return 1;
	if(r != 0)
		return bmp;
	r = (uint32_t)Bg9_SetViewSize((Bg9_t*)BG(g), w, h);
	if(r == 0x80000002u)
		return 2;
	if(r != 0)
		return bmp;
	BGOBJ(g)->vt->setPos(BGOBJ(g), (int)((uint32_t)x << 16), (int)((uint32_t)y << 16));
	return 0;
}

/* "90 49": type 10, `bmp` (any size) zoomed by `scale` (16.16) and
 * rotated by `option` (16.16 degrees) about the centre; 0 ok, 1 the
 * bitmap is missing, 2 a zero scale */
int Gfx_BgSetZoom(Gfx_t* g, int bmp, int scale, int option)
{
	uint32_t r;

	Gfx_SetBgType(g, 10);
	r = (uint32_t)Bg9_SetBitmap((Bg9_t*)BG(g), bmp);
	if(r == 0x80000001u)
		return 1;
	if(r != 0)
		return bmp;
	r = (uint32_t)Bg10_SetScale((Bg10_t*)BG(g), scale, option);
	if(r == 0x80000002u)
		return 2;
	if(r != 0)
		return bmp;
	return 0;
}

/* "90 4A": type 11, the mosaic transition from `front` to `back` (a
 * bitmap or BG_BLACK / BG_WHITE) at `level`; 0 ok, 1 / 2 front / back
 * unusable, 3 the style is not 0, 4 the link flag is not 0 or 1 */
int Gfx_BgSetFlip(Gfx_t* g, int front, int back, int style, int link, int level)
{
	uint32_t r;

	Gfx_SetBgType(g, 11);
	r = (uint32_t)Bg11_Set((Bg11_t*)BG(g), front, back);
	if(r == 0x80000001u)
		return 1;
	if(r == 0x80000002u)
		return 2;
	if(r != 0)
		return back; // (original: returns the second argument)
	if(!Bg11_SetStyle((Bg11_t*)BG(g), style))
		return 3;
	if(!Bg11_SetLink((Bg11_t*)BG(g), link))
		return 4;
	BGOBJ(g)->vt->setLevel(BGOBJ(g), level);
	return 0;
}

/* "91 40": type 12 with layer 0 set up in one go: bitmap `bmp` with its
 * centre (cx, cy) at (x, y), rotated by `angle` and scaled by (sx, sy)
 * (all 16.16), `smooth` for bilinear sampling; the layer is shown.  0 ok,
 * 3 the bitmap is missing, 4 a zero scale. */
int Gfx_BgSetLayers(Gfx_t* g, int x, int y, int bmp, int32_t cx, int32_t cy, int32_t angle,
	int32_t sx, int32_t sy, int smooth)
{
	Bg12_t* b;
	uint32_t r;

	Gfx_SetBgType(g, 12);
	b = (Bg12_t*)BG(g);
	r = (uint32_t)Bg12_LayerSetBitmap(b, 0, bmp, cx, cy);
	if(r == 0x80000002u)
		return 3;
	if(r != 0)
		return (int)cy; // (original: returns the fifth argument)
	r = (uint32_t)Bg12_LayerSetTransform(b, 0, angle, sx, sy, smooth);
	if(r == 0x80000003u)
		return 4;
	if(r != 0)
		return (int)cy;
	Bg12_LayerShow(b, 0, 1);
	Bg12_LayerSetPos(b, 0, x, y);
	return 0;
}

/* The "91 4x" layer operations below share the result codes 0 ok, 1 the
 * background is not type 12, 2 the layer index is not 0..7 (and the
 * original's fall-through argument for anything else).  This is their
 * success tail: the screen is repainted when the layer is shown. */
static int BglRepaint(Gfx_t* g, int layer)
{
	int shown = 0;
	Bg12_LayerIsShown((Bg12_t*)BG(g), &shown, layer);
	if(shown)
		Compositor_InvalidateAll(g->comp);
	return 0;
}

// "91 41": make `layer` the one the generic object operations on handle 0 address
int Gfx_BglSelect(Gfx_t* g, int layer)
{
	uint32_t r;
	if(BG(g)->typeId != 12)
		return 1;
	r = (uint32_t)Bg12_Select((Bg12_t*)BG(g), layer);
	if(r == 0)
		return 0;
	if(r == 0x80000001u)
		return 2;
	return layer;
}

// "91 42": show / hide the layer; the screen is repainted either way
int Gfx_BglShow(Gfx_t* g, int layer, int on)
{
	uint32_t r;
	if(BG(g)->typeId != 12)
		return 1;
	r = (uint32_t)Bg12_LayerShow((Bg12_t*)BG(g), layer, on);
	if(r == 0)
	{
		Compositor_InvalidateAll(g->comp);
		return 0;
	}
	if(r == 0x80000001u)
		return 2;
	return on;
}

// "91 43": the screen position (16.16) of the layer's centre point
int Gfx_BglSetPos(Gfx_t* g, int layer, int32_t x, int32_t y)
{
	uint32_t r;
	if(BG(g)->typeId != 12)
		return 1;
	r = (uint32_t)Bg12_LayerSetPos((Bg12_t*)BG(g), layer, x, y);
	if(r == 0)
		return BglRepaint(g, layer);
	if(r == 0x80000001u)
		return 2;
	return y;
}

// "91 44": the blit effect of the layer (layers 1..7; layer 0 is always copied)
int Gfx_BglSetEffect(Gfx_t* g, int layer, int effect)
{
	uint32_t r;
	if(BG(g)->typeId != 12)
		return 1;
	r = (uint32_t)Bg12_LayerSetEffect((Bg12_t*)BG(g), layer, effect);
	if(r == 0)
		return BglRepaint(g, layer);
	if(r == 0x80000001u)
		return 2;
	return effect;
}

// "91 45": the level of the layer
int Gfx_BglSetLevel(Gfx_t* g, int layer, int level)
{
	uint32_t r;
	if(BG(g)->typeId != 12)
		return 1;
	r = (uint32_t)Bg12_LayerSetLevel((Bg12_t*)BG(g), layer, level);
	if(r == 0)
		return BglRepaint(g, layer);
	if(r == 0x80000001u)
		return 2;
	return level;
}

// "91 46": the layer's bitmap (-1 removes it) and its centre point (cx, cy) in 16.16; 3 the bitmap is missing
int Gfx_BglSetBitmap(Gfx_t* g, int layer, int bmp, int32_t cx, int32_t cy)
{
	uint32_t r;
	if(BG(g)->typeId != 12)
		return 1;
	r = (uint32_t)Bg12_LayerSetBitmap((Bg12_t*)BG(g), layer, bmp, cx, cy);
	if(r == 0)
		return BglRepaint(g, layer);
	if(r == 0x80000001u)
		return 2;
	if(r == 0x80000002u)
		return 3;
	return (int)cy;
}

// "91 47": angle (16.16 degrees), scale (16.16) and bilinear sampling of the layer; 4 a zero scale
int Gfx_BglSetTransform(Gfx_t* g, int layer, int32_t angle, int32_t sx, int32_t sy, int smooth)
{
	uint32_t r;
	if(BG(g)->typeId != 12)
		return 1;
	r = (uint32_t)Bg12_LayerSetTransform((Bg12_t*)BG(g), layer, angle, sx, sy, smooth);
	if(r == 0)
		return BglRepaint(g, layer);
	if(r == 0x80000001u)
		return 2;
	if(r == 0x80000003u)
		return 4;
	return smooth;
}

// "91 48": the Ease curves of the layer's angle and scale deltas (no repaint: they only matter as the progress moves)
int Gfx_BglSetCurves(Gfx_t* g, int layer, int angleCurve, int scaleCurve)
{
	uint32_t r;
	if(BG(g)->typeId != 12)
		return 1;
	r = (uint32_t)Bg12_LayerSetCurves((Bg12_t*)BG(g), layer, angleCurve, scaleCurve);
	if(r == 0)
		return 0;
	if(r == 0x80000001u)
		return 2;
	return scaleCurve;
}

// "91 49": how far the layer's centre point moves (16.16) over the full progress (no repaint)
int Gfx_BglSetCentreDelta(Gfx_t* g, int layer, int32_t dcx, int32_t dcy)
{
	uint32_t r;
	if(BG(g)->typeId != 12)
		return 1;
	r = (uint32_t)Bg12_LayerSetCentreDelta((Bg12_t*)BG(g), layer, dcx, dcy);
	if(r == 0)
		return 0;
	if(r == 0x80000001u)
		return 2;
	return (int)dcy;
}

// "91 4A": the angle and scale change of the layer over the full progress (no repaint)
int Gfx_BglSetXformDelta(Gfx_t* g, int layer, int32_t dAngle, int32_t dsx, int32_t dsy)
{
	uint32_t r;
	if(BG(g)->typeId != 12)
		return 1;
	r = (uint32_t)Bg12_LayerSetAngleDelta((Bg12_t*)BG(g), layer, dAngle);
	if(r == 0)
	{
		Bg12_LayerSetScaleDelta((Bg12_t*)BG(g), layer, dsx, dsy); // result ignored
		return 0;
	}
	if(r == 0x80000001u)
		return 2;
	return (int)dAngle;
}
