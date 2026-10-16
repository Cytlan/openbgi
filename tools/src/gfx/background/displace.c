/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * displace.c - background type 6: a bitmap displaced by vector maps
 *              (inc/bgi/gfx/background.h; selected by "90 45")
 *
 * Every pixel is read from the bitmap at the offset the vector map gives
 * (Blit_Displace); with two maps the object's level mixes between them.
 * The compositor does not band this type: the whole screen is one job.
 */
#include "background_internal.h"

static void Bg6_Destroy(DispObj_t* o, int flags)
{
	Background_Dtor((Background_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

void Bg6_Ctor(Bg6_t* b)
{
	Background_Ctor(&b->bg, 6);
	b->bg.obj.vt = &Bg6_Vtbl.base;
	b->bmp = -1;
	b->vec1 = -1;
	b->vec2 = -1;
}

/* the bitmap and one or two vector maps (mode 4), all screen sized; vec2
 * -1 means none.  0 ok, 0x80000001 bitmap missing, 0x80000002 bitmap not
 * screen sized, 0x80000003 / 0x80000004 the same for vec1, 0x80000005 /
 * 0x80000006 for vec2. */
int Bg6_Set(Bg6_t* b, int bmp, int vec1, int vec2)
{
	Bmp_t info;
	if(!BgGetInfo(&info, bmp))
		return (int)0x80000001;
	if(!Bmp_MatchesScreen(&info))
		return (int)0x80000002;
	if(!BgGetInfo(&info, vec1))
		return (int)0x80000003;
	if(!Bmp_IsScreenVectorMap(&info))
		return (int)0x80000004;
	if(vec2 != -1)
	{
		if(!BgGetInfo(&info, vec2))
			return (int)0x80000005;
		if(!Bmp_IsScreenVectorMap(&info))
			return (int)0x80000006;
	}
	b->bmp = bmp;
	b->vec1 = vec1;
	b->vec2 = vec2;
	b->gen = BgGen(bmp);
	b->gen1 = BgGen(vec1);
	b->gen2 = BgGen(vec2); // -1 for vec2 == -1
	return 0;
}

// the `amount` argument of Blit_Displace (non-zero samples bilinearly)
void Bg6_SetAmount(Bg6_t* b, int amount)
{
	b->amount = amount;
}

// the bitmap through the vector maps at the effective level; 0 once one of them has been replaced
static int Bg6_Render(Background_t* bg, Bmp_t* dst, const Rect_t* local)
{
	Bg6_t* b = (Bg6_t*)bg;
	Bmp_t src, v1, v2;
	if(!BgCurrent(&src, b->bmp, b->gen))
		return 0;
	if(!BgCurrent(&v1, b->vec1, b->gen1))
		return 0;
	if(b->vec2 != -1)
	{
		if(!BgGetInfo(&v2, b->vec2))
			return 0;
		if(BgGen(b->vec2) != b->gen2)
			return 0;
	}
	Bmp_Crop(&src, local);
	Bmp_Crop(&v1, local);
	if(b->vec2 != -1)
		Bmp_Crop(&v2, local);
	Blit_Displace(dst, &src, &v1, b->vec2 != -1 ? &v2 : NULL,
		DispObj_EffectiveLevel(&b->bg.obj), b->amount);
	return 1;
}

BG_VTABLE(Bg6_Vtbl, Bg6_Destroy, DispObj_SetVisible, Background_Invalidate, Background_NopSetPos,
	DispObj_GetPos, DispObj_SetOffset, DispObj_SetLevel, DispObj_GetLevel,
	DispObj_SetParam, DispObj_NopNotify, Background_SetMode, Background_MatchScreenSize,
	Bg6_Render);
