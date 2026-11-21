/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ripple.c - background type 8: a bitmap under ripples
 *            (src/include/bgi/gfx/background.h; selected by "90 47")
 *
 * The bitmap is read through a vector + distance map (mode 6) and a ring
 * table that the bitmap manager fills from a ripple definition ("92 00" /
 * "92 01").  The object's level is the amplitude: every setLevel refills
 * the table.  A script animates the ripple with "90 38" (handle 0,
 * parameter 0x100, phase selector, level) every frame.
 */
#include "background_internal.h"
#include "bgi/core.h"

static void Bg8_SetLevel(OpenBGI_t* o, DispObj_t* obj, int level);

void Bg8_Dtor(Bg8_t* b)
{
	b->bg.obj.vt = &Bg8_Vtbl.base;
	Bg8_FreeTable(b);
	Background_Dtor(&b->bg);
}

static void Bg8_Destroy(OpenBGI_t* o, DispObj_t* obj, int flags)
{
	Bg8_Dtor((Bg8_t*)obj);
	if(flags & 1)
		BGI_Free(obj);
}

void Bg8_Ctor(OpenBGI_t* o, Bg8_t* b)
{
	Background_Ctor(o, &b->bg, 8);
	b->bg.obj.vt = &Bg8_Vtbl.base;
	b->bmp = -1;
	b->vecdist = -1;
	b->table = NULL;
}

void Bg8_FreeTable(Bg8_t* b)
{
	BGI_Free(b->table);
	b->table = NULL;
}

// a screen-sized bitmap; 0 ok, 0x80000001 missing, 0x80000002 not screen sized
int Bg8_SetBitmap(OpenBGI_t* o, Bg8_t* b, int bmp)
{
	Bmp_t info;
	if(!BgGetInfo(o, &info, bmp))
		return (int)0x80000001;
	if(!Bmp_MatchesScreen(o, &info))
		return (int)0x80000002;
	b->bmp = bmp;
	b->gen = BgGen(o, bmp);
	return 0;
}

/* the vector + distance map (mode 6, screen sized), the number of rings
 * and the ripple definition ("92 00") to read; the ring table is (re)built
 * for `level` through the virtual setLevel and the phase selector starts
 * at 0.  0 ok, 0x80000003 map missing, 0x80000004 not a screen-sized
 * vecdist map, 0x80000005 zero rings, 0x80000006 unknown definition,
 * 0x80000007 the definition has fewer rings than asked for. */
int Bg8_SetRipple(OpenBGI_t* o, Bg8_t* b, int vecdist, int rings, int rippleNo, int level)
{
	Bmp_t info;
	uint32_t* table;
	int ok = 0, r;
	if(!BgGetInfo(o, &info, vecdist))
		return (int)0x80000003;
	if(!Bmp_IsScreenVecDist(o, &info))
		return (int)0x80000004;
	if((uint32_t)rings == 0)
		return (int)0x80000005;
	table = (uint32_t*)BGI_Alloc((size_t)rings * 16); // 4 dwords per ring
	r = BmpMgr_RippleCheck(o->gfx.dispBmpMgr, &ok, rippleNo, 0, rings);
	if(r != 0)
	{
		BGI_Free(table);
		return (int)0x80000006;
	}
	if(!ok)
	{
		BGI_Free(table); // the original leaks it
		return (int)0x80000007;
	}
	b->vecdist = vecdist;
	b->vecGen = BgGen(o, vecdist);
	b->rings = rings;
	BGI_Free(b->table);
	b->rippleNo = rippleNo;
	b->table = table;
	b->selector = 0;
	VT(&b->bg)->setLevel(o, &b->bg.obj, level);
	return 0;
}

// the setLevel slot: the level is the amplitude, so the ring table is refilled
static void Bg8_SetLevel(OpenBGI_t* o, DispObj_t* obj, int level)
{
	Bg8_t* b = (Bg8_t*)obj;
	DispObj_SetLevel(o, obj, level);
	BmpMgr_RippleFill(o->gfx.dispBmpMgr, b->table, b->rippleNo, b->selector, level, b->rings);
}

/* pick a phase selector of the ripple definition and set the level; a
 * level above 0x100 keeps the current level.  0 ok, 0x80000006 unknown
 * definition, 0x80000007 fewer than `rings` rings left from that phase. */
int Bg8_SelectRipple(OpenBGI_t* o, Bg8_t* b, int selector, int level)
{
	int ok = 0;
	if(BmpMgr_RippleCheck(o->gfx.dispBmpMgr, &ok, b->rippleNo, selector, b->rings) != 0)
		return (int)0x80000006;
	if(!ok)
		return (int)0x80000007;
	b->selector = selector;
	if((uint32_t)level > 0x100)
		level = GET_LEVEL(o, &b->bg);
	VT(&b->bg)->setLevel(o, &b->bg.obj, level);
	return 0;
}

// the setParam slot: parameter 0x100 is Bg8_SelectRipple(a, c), 0xFFFF0002 when it fails
static int Bg8_SetParam(OpenBGI_t* o, DispObj_t* obj, int no, int a, int c)
{
	if(no != 0x100)
		return DispObj_SetParam(o, obj, no, a, c);
	return Bg8_SelectRipple(o, (Bg8_t*)obj, a, c) != 0 ? (int)0xffff0002 : 0;
}

/* the bitmap through the map and the ring table; 0 once the bitmap or the
 * map has been replaced.  The blitter gets the full source bitmap as well:
 * displaced pixels may come from outside the dirty part. */
static int Bg8_Render(OpenBGI_t* o, Background_t* bg, Bmp_t* dst, const Rect_t* local)
{
	Bg8_t* b = (Bg8_t*)bg;
	Bmp_t srcFull, srcCrop, vec;
	if(!BgCurrent(o, &srcFull, b->bmp, b->gen))
		return 0;
	if(!BgCurrent(o, &vec, b->vecdist, b->vecGen))
		return 0;
	srcCrop = srcFull;
	Bmp_Crop(&srcCrop, local);
	Bmp_Crop(&vec, local);
	Blit_Ripple(dst, &srcCrop, &srcFull, &vec, b->table);
	return 1;
}

BG_VTABLE(Bg8_Vtbl, Bg8_Destroy, DispObj_SetVisible, Background_Invalidate, Background_NopSetPos,
	DispObj_GetPos, Background_NopSetPos /* setOffset is a no-op too */, Bg8_SetLevel,
	DispObj_GetLevel, Bg8_SetParam, DispObj_NopNotify, Background_SetMode,
	Background_MatchScreenSize, Bg8_Render);
