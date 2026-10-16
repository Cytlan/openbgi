/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sprites.c - the graphics manager's sprite operations: the "90 5x" and
 *             "91 55" instructions (inc/bgi/gfx/gfxmgr.h)
 *
 * The 256 sprite slots are addressed by H_SPRITE | index.  Every "set"
 * wrapper repaints the sprite's old footprint, hands the arguments to the
 * Sprite_Set* method of the mode it selects, then repaints the new
 * footprint and re-sorts the sprite.  The methods report 0x8000000n for
 * the n-th check that failed; the wrappers map those onto the small codes
 * the instructions report, and, as the original does, return the handle
 * for a code they do not expect.
 */
#include "mgr_internal.h"

void Gfx_DeleteAllSprites(Gfx_t* g)
{
	Mgr_DeleteAll(g->sprites, GFX_SPRITES, &g->spriteCount, &g->spriteNextId);
}

/* "90 50": a new sprite in the first free slot; its slot id (the low word
 * of the sort key) is the running counter, so a later sprite of equal
 * priority draws on top.  Returns the handle, 0 when all 256 are in use. */
uint32_t Gfx_SpriteCreate(Gfx_t* g)
{
	Sprite_t* s;
	uint32_t i;

	if(g->spriteCount >= GFX_SPRITES)
		return 0;
	i = Mgr_FreeSlot(g->sprites);
	s = (Sprite_t*)BGI_Calloc(sizeof(Sprite_t));
	Sprite_Ctor(s, g->spriteNextId++);
	s->useProjCentre = 1; // the window sub-sprites keep the back-buffer centre
	g->sprites[i] = &s->obj;
	Compositor_Add(g->comp, &s->obj);
	g->spriteCount++;
	return H_SPRITE | i;
}

// "90 51": 1 ok, 0 unknown handle
int Gfx_SpriteDelete(Gfx_t* g, uint32_t h)
{
	return Mgr_DeleteFromTable(g, g->sprites, Gfx_FindSprite(g, h), h, &g->spriteCount, 0);
}

/* the tail every sprite "set" wrapper shares on success: repaint the new
 * footprint and re-sort (position and priority may have changed); 0 */
static int SpriteSetDone(Gfx_t* g, DispObj_t* o)
{
	InvalidateIfVisible(o);
	Compositor_Resort(g->comp, o);
	return 0;
}

/* "90 56": the plain sprite - bitmap, position, effect, level and
 * priority in one call.  0 ok, 1 the bitmap is unusable (Sprite_Set
 * 0x80000001), 0xFF unknown handle. */
int Gfx_SpriteSet(Gfx_t* g, uint32_t h, int x, int y, int bmp, int effect, int level, int prio)
{
	DispObj_t* o = Gfx_FindSprite(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	InvalidateIfVisible(o);
	r = (uint32_t)Sprite_Set((Sprite_t*)o, x, y, bmp, effect, level, prio);
	if(r == 0)
		return SpriteSetDone(g, o);
	if(r == 0x80000001u)
		return 1;
	return (int)h; // (original: returns the handle)
}

/* "90 58": the cross-fade sprite - two bitmaps mixed by `mixRatio`.  0 ok,
 * 1 a bitmap is unusable (0x80000001 / 0x80000002), 9 the bitmaps differ
 * in size (0x80000003), 0xFF unknown handle. */
int Gfx_SpriteSetBlend(Gfx_t* g, uint32_t h, int x, int y, int bmp1, int bmp2, int mixRatio, int level,
	int prio, int levelRoute)
{
	DispObj_t* o = Gfx_FindSprite(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	InvalidateIfVisible(o);
	r = (uint32_t)Sprite_SetBlendEx((Sprite_t*)o, x, y, bmp1, bmp2, mixRatio, level, prio, levelRoute);
	if(r == 0)
		return SpriteSetDone(g, o);
	if(r == 0x80000001u || r == 0x80000002u)
		return 1;
	if(r == 0x80000003u)
		return 9;
	return (int)h;
}

/* "90 59": the transformed sprite - rotated by `angle` and scaled by
 * (sx, sy) about the bitmap point (ox, oy).  0 ok, 1 the bitmap is
 * unusable (0x80000001), 8 the transformed picture is empty (0x80000004),
 * 0xFF unknown handle. */
int Gfx_SpriteSetTransform(Gfx_t* g, uint32_t h, int x, int y, int bmp, int ox, int oy, int32_t angle,
	int32_t sx, int32_t sy, int smooth, int effect, int level, int prio)
{
	DispObj_t* o = Gfx_FindSprite(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	InvalidateIfVisible(o);
	r = (uint32_t)Sprite_SetTransformEx((Sprite_t*)o, x, y, bmp, ox, oy, angle, sx, sy, smooth, effect, level, prio);
	if(r == 0)
		return SpriteSetDone(g, o);
	if(r == 0x80000001u)
		return 1;
	if(r == 0x80000004u)
		return 8;
	return (int)h;
}

/* "90 5B" of version 1.58: re-scale a sprite in place - the bitmap and
 * the factors change, position, effect, level and priority stay.  That
 * build's sprite only scaled (no offsets, no rotation), so the 1.69
 * transform core is used with those at zero; the same result codes as
 * Gfx_SpriteSetTransform. */
int Gfx_SpriteRescale(Gfx_t* g, uint32_t h, int bmp, int32_t sx, int32_t sy, int smooth)
{
	DispObj_t* o = Gfx_FindSprite(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	InvalidateIfVisible(o);
	r = (uint32_t)Sprite_SetTransform((Sprite_t*)o, bmp, 0, 0, 0, sx, sy, smooth);
	if(r == 0)
		return SpriteSetDone(g, o);
	if(r == 0x80000001u)
		return 1;
	if(r == 0x80000004u)
		return 8;
	return (int)h;
}

/* "90 5A": the wipe sprite - the bitmap revealed through a grayscale
 * pattern by `progress`.  0 ok, 1 the bitmap or the pattern is missing
 * (0x80000001 / 0x80000002), 2 the pattern is not grayscale (0x8000000A),
 * 0xFF unknown handle. */
int Gfx_SpriteSetWipe(Gfx_t* g, uint32_t h, int x, int y, int bmp, int gray, int wipeMode, int progress,
	int effect, int level, int prio)
{
	DispObj_t* o = Gfx_FindSprite(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	InvalidateIfVisible(o);
	r = (uint32_t)Sprite_SetWipeEx((Sprite_t*)o, x, y, bmp, gray, wipeMode, progress, effect, level, prio);
	if(r == 0)
		return SpriteSetDone(g, o);
	if(r == 0x80000001u || r == 0x80000002u)
		return 1;
	if(r == 0x8000000au)
		return 2;
	return (int)h;
}

/* "90 5B": the ripple sprite - the bitmap under the rings of ripple
 * definition `rippleNo` read through a vector + distance map.  0 ok, 1 no
 * such bitmap (0x80000001), 2 the bitmap is empty (0x8000000A), 3 no such
 * map (0x80000005), 4 the map is not mode 6 or differs in size
 * (0x80000006), 5 no rings (0x80000007), 6 no such ripple definition
 * (0x80000008), 7 the definition has fewer rings than asked for
 * (0x80000009), 0xFF unknown handle. */
int Gfx_SpriteSetRipple(Gfx_t* g, uint32_t h, int x, int y, int bmp, int map, int rings, int rippleNo,
	int level, int fade, int prio)
{
	DispObj_t* o = Gfx_FindSprite(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	InvalidateIfVisible(o);
	r = (uint32_t)Sprite_SetRippleEx((Sprite_t*)o, x, y, bmp, map, rings, rippleNo, level, fade, prio);
	switch(r)
	{
		case 0: return SpriteSetDone(g, o);
		case 0x80000001u: return 1;
		case 0x80000005u: return 3;
		case 0x80000006u: return 4;
		case 0x80000007u: return 5;
		case 0x80000008u: return 6;
		case 0x80000009u: return 7;
		case 0x8000000au: return 2;
		default: return (int)h;
	}
}

/* "90 5C": the projected sprite - placed by a 16.16 3D position through
 * the projection, optionally cross-faded and transformed.  0 ok, 1 a
 * bitmap is unusable (0x80000001 / 0x80000002), 9 the second bitmap
 * differs in size (0x80000003), 8 the picture is empty (0x80000004), 0xFF
 * unknown handle. */
int Gfx_SpriteSetProjected(Gfx_t* g, uint32_t h, int32_t fx, int32_t fy, int32_t fz, int bmp1, int bmp2,
	int mixRatio, int levelRoute, int ox, int oy, int32_t angle, int projDist,
	int projPos, int smooth, int effect, int level, int prio)
{
	DispObj_t* o = Gfx_FindSprite(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	InvalidateIfVisible(o);
	r = (uint32_t)Sprite_SetProjectedEx((Sprite_t*)o, fx, fy, fz, bmp1, bmp2, mixRatio, levelRoute, ox, oy,
		angle, projDist, projPos, smooth, effect, level, prio);
	if(r == 0)
		return SpriteSetDone(g, o);
	if(r == 0x80000001u || r == 0x80000002u)
		return 1;
	if(r == 0x80000003u)
		return 9;
	if(r == 0x80000004u)
		return 8;
	return (int)h;
}

/* "90 57": swap the bitmap of a plain / transformed / projected sprite
 * (modes 0, 2, 5), keeping the mode's parameters; a sprite of another mode
 * reports 0 without doing anything.  0 ok, 1 no such bitmap (0x80000001),
 * 0xFF unknown handle. */
int Gfx_SpriteChangeBitmap(Gfx_t* g, uint32_t h, int bmp)
{
	DispObj_t* o = Gfx_FindSprite(g, h);
	uint32_t r;
	int was;

	if(!o)
		return NO_OBJECT;
	was = IsVisible(o);
	if(was)
		Invalidate(o);
	r = (uint32_t)Sprite_ChangeBitmap((Sprite_t*)o, bmp);
	if(r == 0)
	{
		if(was)
			Invalidate(o);
		return 0;
	}
	if(r == 0x80000001u)
		return 1;
	return (int)h;
}

/* "90 55": a screen-sized grayscale mask the sprite shows through (-1
 * removes it).  0 ok, 1 no such bitmap (0x80000001), 2 not a grayscale map
 * of the screen's size (0x8000000A), 0xFF unknown handle.  The old state
 * is not repainted: the footprint does not change. */
int Gfx_SpriteSetMask(Gfx_t* g, uint32_t h, int bmp)
{
	DispObj_t* o = Gfx_FindSprite(g, h);
	uint32_t r;

	if(!o)
		return NO_OBJECT;
	r = (uint32_t)Sprite_SetMask((Sprite_t*)o, bmp); // no repaint of the old state
	if(r == 0)
	{
		InvalidateIfVisible(o);
		return 0;
	}
	if(r == 0x80000001u)
		return 1;
	if(r == 0x8000000au)
		return 2;
	return (int)h;
}

/* "91 55" of 1.529 on: link sprite h to the mask sprite h2 (its picture
 * becomes h's mask); h2 = 0 unlinks.  0 ok, 0xB the same sprite or an
 * unknown h2, 0xD already linked / nothing linked, 0xE the mask sprite is
 * in use by another sprite, 0xF it had no user, 0xFF no sprite h. */
int Gfx_SpriteLinkMask(Gfx_t* g, uint32_t h, uint32_t h2)
{
	DispObj_t* o = Gfx_FindSprite(g, h);
	DispObj_t* m = h2 ? Gfx_FindSprite(g, h2) : NULL;
	uint32_t r;
	if(!o)
		return NO_OBJECT;
	if(o == m || (h2 && !m))
		return 0xb;
	r = Sprite_LinkMask((Sprite_t*)o, (Sprite_t*)m);
	switch(r)
	{
		case 0: InvalidateIfVisible(o); return 0;
		case 0x8000000bu:
		case 0x8000000cu: return 0xd;
		case 0x8000000du: return 0xe;
		case 0x8000000eu: return 0xf;
		default: return (int)r;
	}
}

// "90 54": show / hide; 1 ok, 0 unknown handle
int Gfx_SpriteShow(Gfx_t* g, uint32_t h, int on)
{
	return Mgr_ShowBracket(Gfx_FindSprite(g, h), on);
}

/* "90 53": dirty the part of the sprite that shows the rectangle
 * (x, y, w, h) of its bitmap after the script drew into it.  0 ok, 0xA
 * nothing was dirtied (the mapped rectangle is empty), 0xFF unknown
 * handle. */
int Gfx_SpriteUpdateRect(Gfx_t* g, uint32_t h, int x, int y, int w, int hgt)
{
	DispObj_t* o = Gfx_FindSprite(g, h);
	Rect_t r;

	if(!o)
		return NO_OBJECT;
	r.l = x;
	r.t = y;
	r.r = x + w - 1;
	r.b = y + hgt - 1;
	return Sprite_UpdateRect((Sprite_t*)o, &r) ? 0 : 0xa;
}
