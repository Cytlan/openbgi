/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * base.c - the Background display object: the base class every
 *          background type derives from, and the factory
 *          (inc/bgi/gfx/background.h)
 *
 * A background is the display object at the bottom of the picture: it
 * covers the screen, and a change to it redraws everything.  The type
 * (Background_Create) decides what it paints; mode 0 paints black, and so
 * does a type whose render() fails (a bitmap gone missing).  The helpers
 * Bmp_MatchesScreen, Bg_BitmapUsable, Bmp_IsScreenVectorMap and
 * Bmp_IsScreenVecDist are the checks the types' setters share.
 */
#include "background_internal.h"

// DispObj(classOrder 0, slot 0), screen-sized, visible, mode 0
void Background_Ctor(Background_t* b, int typeId)
{
	DispObj_Ctor(&b->obj, 0, 0);
	b->obj.vt = &Background_Vtbl.base;
	b->typeId = typeId;
	b->mode = 0;
	Background_MatchScreenSize(b); // the base version, not virtual
	DispObj_SetVisible(&b->obj, 1);
}

void Background_Dtor(Background_t* b)
{
	b->obj.vt = &Background_Vtbl.base;
	DispObj_Dtor(&b->obj);
}

// the destroy slot: flags & 1 frees the object as well
void Background_Destroy(DispObj_t* o, int flags)
{
	Background_Dtor((Background_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

void Background_SetMode(Background_t* b, int mode)
{
	b->mode = mode;
}

// a background covers the screen, so any change redraws it all
void Background_Invalidate(DispObj_t* o)
{
	Background_t* b = (Background_t*)o;
	if(b->mode != 0)
		Gfx_InvalidateAll(gDispGfx);
}

/* the draw slot: render() paints `dst` (the dirty part of the back buffer,
 * `local` tells which part); mode 0 or a failed render clears it to black */
void Background_Draw(DispObj_t* o, Bmp_t* dst, const Rect_t* local, uint32_t minKey)
{
	Background_t* b = (Background_t*)o;
	(void)minKey;
	if(!(b->mode != 0 && Background_Render(b, dst, local)))
		Bmp_Clear(dst, NULL);
}

// the screen rectangle of a background is its local rectangle (it is at the origin)
void Background_ScreenRect(DispObj_t* o, Rect_t* out)
{
	o->vt->localRect(o, out);
}

// resize the object to the back buffer; 1 when something changed
int Background_MatchScreenSize(Background_t* b)
{
	Bmp_t back, own;
	Gfx_GetBackBmp(&back);
	DispObj_CopyBmp(&b->obj, &own);
	if(back.w == own.w && back.h == own.h && back.mode == own.mode)
		return 0;
	VT(b)->setSize(&b->obj, back.w, back.h);
	return 1;
}

// the render slot of the base class: paints nothing (the draw clears to black)
int Background_NoRender(Background_t* b, Bmp_t* dst, const Rect_t* local)
{
	(void)b;
	(void)dst;
	(void)local;
	return 0;
}

// width, height and pixel mode of the back buffer
int Bmp_MatchesScreen(const Bmp_t* b)
{
	Bmp_t back;
	Gfx_GetBackBmp(&back);
	return b->w == back.w && b->h == back.h && b->mode == back.mode;
}

// the slot holds a bitmap and it matches the screen
int Bg_BitmapUsable(int bmpNo)
{
	Bmp_t info;
	if(!BgGetInfo(&info, bmpNo))
		return 0;
	return Bmp_MatchesScreen(&info);
}

// a vector map (mode 4) of the screen's size
int Bmp_IsScreenVectorMap(const Bmp_t* b)
{
	Bmp_t back;
	if(b->mode != 4)
		return 0;
	Gfx_GetBackBmp(&back);
	return b->w == back.w && b->h == back.h;
}

// a vector + distance map (mode 6) of the screen's size
int Bmp_IsScreenVecDist(const Bmp_t* b)
{
	Bmp_t back;
	if(b->mode != 6)
		return 0;
	Gfx_GetBackBmp(&back);
	return b->w == back.w && b->h == back.h;
}

void Background_NopSetPos(DispObj_t* o, int x, int y)
{
	(void)o;
	(void)x;
	(void)y;
}

BG_VTABLE(Background_Vtbl, Background_Destroy, DispObj_SetVisible, Background_Invalidate,
	Background_NopSetPos, DispObj_GetPos, DispObj_SetOffset, DispObj_SetLevel,
	DispObj_GetLevel, DispObj_SetParam, DispObj_NopNotify, Background_SetMode,
	Background_MatchScreenSize, Background_NoRender);

/* allocate (zero filled) and construct the background of `type` 1..12;
 * NULL for an unknown type */
Background_t* Background_Create(int type)
{
	static const size_t sizes[13] = {
		0, sizeof(Bg1_t), sizeof(Bg2_t), sizeof(Bg3_t), sizeof(Bg4_t), sizeof(Bg5_t), sizeof(Bg6_t),
		sizeof(Bg7_t), sizeof(Bg8_t), sizeof(Bg9_t), sizeof(Bg10_t), sizeof(Bg11_t), sizeof(Bg12_t)};
	void* p;
	if(type < 1 || type > 12)
		return NULL;
	p = BGI_Calloc(sizes[type]);
	switch(type)
	{
		case 1: Bg1_Ctor((Bg1_t*)p); break;
		case 2: Bg2_Ctor((Bg2_t*)p); break;
		case 3: Bg3_Ctor((Bg3_t*)p); break;
		case 4: Bg4_Ctor((Bg4_t*)p); break;
		case 5: Bg5_Ctor((Bg5_t*)p); break;
		case 6: Bg6_Ctor((Bg6_t*)p); break;
		case 7: Bg7_Ctor((Bg7_t*)p); break;
		case 8: Bg8_Ctor((Bg8_t*)p); break;
		case 9: Bg9_Ctor((Bg9_t*)p); break;
		case 10: Bg10_Ctor((Bg10_t*)p); break;
		case 11: Bg11_Ctor((Bg11_t*)p); break;
		default: Bg12_Ctor((Bg12_t*)p); break;
	}
	return (Background_t*)p;
}
